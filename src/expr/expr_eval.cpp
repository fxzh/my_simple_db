// expr_eval.cpp: 表达式求值器实现
#include "expr_eval.h"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <string_view>

#include "common/err.h"
#include "log/log.h"
#include "storage/cast.h"

namespace expr {

namespace {

st::Value eval_bound(const ana::BoundExpr& expr, const st::Row* row);

// 数值转 double: 经 cast 分派表提升到 Double 域, 非数值源域由表入口报错
double to_double(const st::Value& v)
{
    return std::get<double>(st::cast_value(v, st::ColType::Double).box);
}

// 数值转 float: 经 cast 分派表转到 Float 域(整型单次舍入), 非数值源域由表入口报错
float to_float(const st::Value& v)
{
    return std::get<float>(st::cast_value(v, st::ColType::Float).box);
}

// 整型取值: 经 cast 分派表归一到目标整域(Int/BigInt 同盒)后取盒值
int64_t get_int(const st::Value& v, st::ColType domain)
{
    return std::get<int64_t>(st::cast_value(v, domain).box);
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
    case st::ColType::Double:
    case st::ColType::Numeric: return ValueFamily::Num;
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
    if (e.type == st::ColType::Float) {
        return st::float_val(-to_float(v));  // 有限值取反仍有限
    }
    if (e.type == st::ColType::Double) {
        return st::double_val(-to_double(v));  // 有限值取反仍有限
    }
    const int64_t i = get_int(v, e.type);
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
    if (e.type == st::ColType::Float) {
        // 单精度域逐运算舍入
        const float l = to_float(lv);
        const float r = to_float(rv);
        float out = 0.0f;
        switch (e.op) {
        case '+': out = l + r; break;
        case '-': out = l - r; break;
        case '*': out = l * r; break;
        case '/': out = l / r; break;
        }
        if (!std::isfinite(out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "浮点运算溢出或除零");
        }
        return st::float_val(out);
    }
    if (e.type == st::ColType::Double) {
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
        return st::double_val(out);
    }
    const int64_t l = get_int(lv, e.type);
    const int64_t r = get_int(rv, e.type);
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

// 内建函数求值签名: args 已求值且无 NULL, ret 为绑定期固化的结果域
using FuncEval = st::Value (*)(const std::vector<st::Value>& args, st::ColType ret);

// abs 求值: 按绑定期固化域装配, 整型域下界取绝对值溢出报错
st::Value eval_abs(const std::vector<st::Value>& args, st::ColType ret)
{
    const st::Value& v = args[0];
    if (ret == st::ColType::Float) {
        return st::float_val(std::fabs(to_float(v)));
    }
    if (ret == st::ColType::Double) {
        return st::double_val(std::fabs(to_double(v)));
    }
    const int64_t i = get_int(v, ret);
    if (i == INT64_MIN || (ret == st::ColType::Int && i == INT32_MIN)) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型绝对值溢出");
    }
    const int64_t out = i < 0 ? -i : i;
    return ret == st::ColType::Int ? st::int_val(out) : st::bigint_val(out);
}

// 内建函数求值表: 下标即 FuncId 取值(从 1 起, 0 号槽占位), 增改函数须同步 analyzer 注册表
constexpr FuncEval kFuncTable[] = {
    nullptr,
    eval_abs,
};

// 函数调用: 任一实参为 NULL 即原样传布, 其余经求值表按绑定期固化域计算装配,
// 整型溢出当场报错; 实参个数与类型由语义层保证
st::Value eval_func_call(const ana::BoundFuncCall& e, const st::Row* row)
{
    std::vector<st::Value> argv;
    argv.reserve(e.args.size());
    for (const std::unique_ptr<ana::BoundExpr>& a : e.args) {
        const st::Value v = eval_bound(*a, row);
        if (is_null(v)) {
            return v;  // NULL 严格传布
        }
        argv.push_back(v);
    }
    const size_t idx = static_cast<size_t>(e.func);
    if (idx >= std::size(kFuncTable) || kFuncTable[idx] == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 未知内建函数");
    }
    return kFuncTable[idx](argv, e.type);
}

// CAST: NULL 传播; 转换经 cast 分派表显式模式执行, Int 目标超 int32 域与
// 文本目标超声明长度(仅文本目标 length 非 0)当场报错
st::Value eval_cast(const ana::BoundCast& e, const st::Row* row)
{
    const st::Value v = eval_bound(*e.operand, row);
    if (is_null(v)) {
        return v;  // NULL 传播
    }
    const st::Value out = st::cast_value(v, e.type, st::CastMode::Explicit);
    if (e.type == st::ColType::Int) {
        const int64_t* i = std::get_if<int64_t>(&out.box);
        if (i == nullptr) {
            DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: cast 产物形态与目标域不符");
        }
        if (*i < INT32_MIN || *i > INT32_MAX) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXPR, "整型值超出 int 可表示范围");
        }
    }
    if (e.length > 0) {
        const std::string* s = std::get_if<std::string>(&out.box);
        if (s == nullptr) {
            DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: cast 产物形态与目标域不符");
        }
        if (s->size() > e.length) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXPR, "字符串超出目标长度");
        }
    }
    return out;
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
    case ana::BoundExprKind::FuncCall:
        return eval_func_call(static_cast<const ana::BoundFuncCall&>(expr), row);
    case ana::BoundExprKind::Cast:
        return eval_cast(static_cast<const ana::BoundCast&>(expr), row);
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
