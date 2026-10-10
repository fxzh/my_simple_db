// expr_eval.cpp: 表达式求值器实现
#include "expr_eval.h"

#include <cmath>
#include <cstdint>
#include <string_view>

#include "common/err.h"
#include "log/log.h"

namespace expr {

namespace {

st::Value eval_bound(const ana::BoundExpr& expr, const st::Row* row);

// 数值转 double(整型提升), 非数值形态当场报错
double to_double(const st::Value& v)
{
    if (const auto* d = std::get_if<double>(&v.box)) {
        return *d;
    }
    if (const auto* i = std::get_if<int64_t>(&v.box)) {
        return static_cast<double>(*i);
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 数值操作数形态非法");
}

// 整型取值, 形态不符当场报错
int64_t get_int(const st::Value& v)
{
    const int64_t* i = std::get_if<int64_t>(&v.box);
    if (i == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 整型操作数形态非法");
    }
    return *i;
}

// 值是否 NULL(仅看 box)
bool is_null(const st::Value& v)
{
    return std::holds_alternative<std::monostate>(v.box);
}

// 整型域结果溢出文案(随运算符)
const char* arith_overflow_msg(char op)
{
    switch (op) {
    case '+': return "整型加法溢出";
    case '-': return "整型减法溢出";
    case '*': return "整型乘法溢出";
    case '/': return "整型除法溢出";
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 未知算术运算符");
}

// 去尾随空格(比较 PAD SPACE 语义用)
std::string_view rtrim_space(std::string_view s)
{
    while (!s.empty() && s.back() == ' ') {
        s.remove_suffix(1);
    }
    return s;
}

// 值家族: 数值/文本/布尔, 供比较分派; 值无语义类型时归 None
enum class ValueFamily { Num, Text, Bool, None };

ValueFamily value_family(const st::Value& v)
{
    switch (v.type) {
    case st::ColType::Int:
    case st::ColType::BigInt:
    case st::ColType::Float:
    case st::ColType::Double: return ValueFamily::Num;
    case st::ColType::Char:
    case st::ColType::VarChar: return ValueFamily::Text;
    case st::ColType::Bool: return ValueFamily::Bool;
    case st::ColType::Null: return ValueFamily::None;
    }
    return ValueFamily::None;
}

// 一元负号: NULL 传播, 取反; 结果按绑定期推导域装配并按域做溢出检查
st::Value eval_neg(const ana::BoundNeg& e, const st::Row* row)
{
    const st::Value v = eval_bound(*e.operand, row);
    if (is_null(v)) {
        return v;  // NULL 传播
    }
    if (e.type == st::ColType::Float || e.type == st::ColType::Double) {
        const double out = -to_double(v);  // 有限值取反仍有限
        return e.type == st::ColType::Float ? st::float_val(out) : st::double_val(out);
    }
    const int64_t i = get_int(v);
    if (i == INT64_MIN || (e.type == st::ColType::Int && i == INT32_MIN)) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型取反溢出");
    }
    const int64_t out = -i;
    return e.type == st::ColType::Int ? st::int_val(out) : st::bigint_val(out);
}

// 双目算术: 任一操作数为 NULL 结果 NULL, 按绑定期推导域计算与装配, 类型驱动提升
// (整型向零截断), 溢出/除零当场报错; 操作数数值性由语义层保证
st::Value eval_binary(const ana::BoundArith& e, const st::Row* row)
{
    const st::Value lv = eval_bound(*e.left, row);
    const st::Value rv = eval_bound(*e.right, row);
    if (is_null(lv) || is_null(rv)) {
        return st::Value{};  // NULL 传播, 短路于除零/溢出检查
    }
    if (e.type == st::ColType::Float || e.type == st::ColType::Double) {
        double out = 0.0;
        switch (e.op) {
        case '+': out = to_double(lv) + to_double(rv); break;
        case '-': out = to_double(lv) - to_double(rv); break;
        case '*': out = to_double(lv) * to_double(rv); break;
        case '/': out = to_double(lv) / to_double(rv); break;
        }
        if (!std::isfinite(out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "浮点运算溢出或除零");
        }
        return e.type == st::ColType::Float ? st::float_val(out) : st::double_val(out);
    }
    const int64_t l = get_int(lv);
    const int64_t r = get_int(rv);
    int64_t out = 0;
    switch (e.op) {
    case '+':
        if (__builtin_add_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型加法溢出");
        }
        break;
    case '-':
        if (__builtin_sub_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型减法溢出");
        }
        break;
    case '*':
        if (__builtin_mul_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型乘法溢出");
        }
        break;
    case '/':
        if (r == 0) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型除零");
        }
        if (l == INT64_MIN && r == -1) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型除法溢出");
        }
        out = l / r;  // 向零截断
        break;
    }
    if (e.type == st::ColType::Int && (out < INT32_MIN || out > INT32_MAX)) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, arith_overflow_msg(e.op));
    }
    return e.type == st::ColType::Int ? st::int_val(out) : st::bigint_val(out);
}

// 比较: 任一侧 NULL 即 NULL; 其余经 value_cmp 三向比较, 可比性由语义层保证
st::Value eval_compare(const ana::BoundCmp& e, const st::Row* row)
{
    const st::Value lv = eval_bound(*e.left, row);
    const st::Value rv = eval_bound(*e.right, row);
    if (is_null(lv) || is_null(rv)) {
        return st::Value{};  // NULL 传播, 求值结果即 UNKNOWN
    }
    const int c = value_cmp(lv, rv);
    switch (e.op) {
    case CmpOp::Eq: return st::bool_val(c == 0);
    case CmpOp::Ne: return st::bool_val(c != 0);
    case CmpOp::Lt: return st::bool_val(c < 0);
    case CmpOp::Le: return st::bool_val(c <= 0);
    case CmpOp::Gt: return st::bool_val(c > 0);
    case CmpOp::Ge: return st::bool_val(c >= 0);
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 未知比较种类");
}

// AND/OR: 三值逻辑, AND 有 false 即 false / OR 有 true 即 true, 其余 NULL 传播;
// 操作数布尔性由语义层保证
st::Value eval_logic(const ana::BoundLogic& e, const st::Row* row)
{
    const st::Value lv = eval_bound(*e.left, row);
    const st::Value rv = eval_bound(*e.right, row);
    const bool* lb = std::get_if<bool>(&lv.box);
    const bool* rb = std::get_if<bool>(&rv.box);
    if ((!is_null(lv) && lb == nullptr) || (!is_null(rv) && rb == nullptr)) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 逻辑操作数不是布尔");
    }
    if (e.op == LogicOp::And) {
        if ((lb != nullptr && !*lb) || (rb != nullptr && !*rb)) {
            return st::bool_val(false);  // 有 false 即 false
        }
        if (lb == nullptr || rb == nullptr) {
            return st::Value{};  // 有 UNKNOWN 即 UNKNOWN
        }
        return st::bool_val(true);
    }
    if ((lb != nullptr && *lb) || (rb != nullptr && *rb)) {
        return st::bool_val(true);  // 有 true 即 true
    }
    if (lb == nullptr || rb == nullptr) {
        return st::Value{};  // 有 UNKNOWN 即 UNKNOWN
    }
    return st::bool_val(false);
}

// NOT: 三值逻辑, NULL 传播; 操作数布尔性由语义层保证
st::Value eval_not(const ana::BoundNot& e, const st::Row* row)
{
    const st::Value v = eval_bound(*e.operand, row);
    if (is_null(v)) {
        return v;
    }
    const bool* b = std::get_if<bool>(&v.box);
    if (b == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 逻辑操作数不是布尔");
    }
    return st::bool_val(!*b);
}

// IS [NOT] NULL: 对任意类型操作数判空
st::Value eval_is_null(const ana::BoundIsNull& e, const st::Row* row)
{
    const st::Value v = eval_bound(*e.operand, row);
    return st::bool_val(e.negate ? !is_null(v) : is_null(v));
}

// 统一求值入口: row 为空表示常量上下文(绑定树不含列引用)
st::Value eval_bound(const ana::BoundExpr& expr, const st::Row* row)
{
    switch (expr.kind()) {
    case ana::BoundExprKind::Const: {
        const st::Value v = static_cast<const ana::BoundConst&>(expr).value;
        if (const auto* d = std::get_if<double>(&v.box); d != nullptr && !std::isfinite(*d)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "浮点字面量超出可表示范围");
        }
        return v;
    }
    case ana::BoundExprKind::ColRef: {
        const auto& ref = static_cast<const ana::BoundColRef&>(expr);
        if (row == nullptr) {
            DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "常量上下文不允许引用列");
        }
        return row->values[ref.col_idx];
    }
    case ana::BoundExprKind::Arith:
        return eval_binary(static_cast<const ana::BoundArith&>(expr), row);
    case ana::BoundExprKind::Neg:
        return eval_neg(static_cast<const ana::BoundNeg&>(expr), row);
    case ana::BoundExprKind::Cmp:
        return eval_compare(static_cast<const ana::BoundCmp&>(expr), row);
    case ana::BoundExprKind::Logic:
        return eval_logic(static_cast<const ana::BoundLogic&>(expr), row);
    case ana::BoundExprKind::Not:
        return eval_not(static_cast<const ana::BoundNot&>(expr), row);
    case ana::BoundExprKind::IsNull:
        return eval_is_null(static_cast<const ana::BoundIsNull&>(expr), row);
    }
    // 不可达: 全部绑定表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 未知绑定表达式节点");
}

}  // namespace

// 常量上下文求值(INSERT VALUES 等无行上下文的场景, 树中不含列引用)
st::Value eval_const(const ana::BoundExpr& e)
{
    return eval_bound(e, nullptr);
}

// 行上下文求值(SELECT 投影与 WHERE 过滤)
st::Value eval_row(const ana::BoundExpr& e, const st::Row& row)
{
    return eval_bound(e, &row);
}

// 值三向比较(排序/去重共享): NULL 最大, 双 NULL 相等; 布尔 false<true; 整型按 int64;
// 数值家族混合提升 double; 字符串字典序且仅 Char 侧去尾随空格; 家族不可比报 Internal
int value_cmp(const st::Value& a, const st::Value& b)
{
    const bool an = is_null(a);
    const bool bn = is_null(b);
    if (an || bn) {
        return an && bn ? 0 : (an ? 1 : -1);  // NULL 最大, 双 NULL 相等
    }
    switch (value_family(a)) {
    case ValueFamily::Num: {
        if (value_family(b) != ValueFamily::Num) {
            break;
        }
        const int64_t* li = std::get_if<int64_t>(&a.box);
        const int64_t* ri = std::get_if<int64_t>(&b.box);
        if (li != nullptr && ri != nullptr) {
            return *li < *ri ? -1 : (*li > *ri ? 1 : 0);  // 纯整型精确比较
        }
        const double l = to_double(a);  // 数值混合提升 double
        const double r = to_double(b);
        return l < r ? -1 : (l > r ? 1 : 0);
    }
    case ValueFamily::Text: {
        if (value_family(b) != ValueFamily::Text) {
            break;
        }
        const std::string* ls = std::get_if<std::string>(&a.box);
        const std::string* rs = std::get_if<std::string>(&b.box);
        if (ls == nullptr || rs == nullptr) {
            break;
        }
        // 仅 Char 侧去尾随空格(PAD SPACE 语义)
        const std::string_view lv = a.type == st::ColType::Char ? rtrim_space(*ls) : *ls;
        const std::string_view rv = b.type == st::ColType::Char ? rtrim_space(*rs) : *rs;
        return lv.compare(rv);
    }
    case ValueFamily::Bool: {
        if (value_family(b) != ValueFamily::Bool) {
            break;
        }
        const bool* lb = std::get_if<bool>(&a.box);
        const bool* rb = std::get_if<bool>(&b.box);
        if (lb == nullptr || rb == nullptr) {
            break;
        }
        return *lb == *rb ? 0 : (*lb ? 1 : -1);
    }
    case ValueFamily::None:
        break;
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 值家族不可比");
}

// WHERE 条件判定: NULL(UNKNOWN) 视为不满足, bool 由语义层保证
bool where_match(const ana::BoundExpr& where, const st::Row& row)
{
    const st::Value v = eval_bound(where, &row);
    if (is_null(v)) {
        return false;
    }
    const bool* b = std::get_if<bool>(&v.box);
    if (b == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: WHERE 条件不是布尔");
    }
    return *b;
}

}  // namespace expr
