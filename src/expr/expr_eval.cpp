// expr_eval.cpp: 表达式求值器实现
#include "expr_eval.h"

#include <cmath>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "common/err.h"
#include "log/log.h"

namespace expr {

namespace {

EvalValue eval_bound(const ana::BoundExpr& expr, const st::Row* row);

// 数值转 double(整型提升)
double to_double(const EvalValue& v)
{
    if (const auto* d = std::get_if<double>(&v)) {
        return *d;
    }
    return static_cast<double>(std::get<int64_t>(v));
}

// 值是否 NULL(monostate)
bool is_null(const EvalValue& v)
{
    return std::holds_alternative<std::monostate>(v);
}

// 去尾随空格(比较 PAD SPACE 语义用)
std::string_view rtrim_space(std::string_view s)
{
    while (!s.empty() && s.back() == ' ') {
        s.remove_suffix(1);
    }
    return s;
}

// 一元负号: NULL 传播, 取反
EvalValue eval_neg(const ana::BoundExpr& operand, const st::Row* row)
{
    const EvalValue v = eval_bound(operand, row);
    if (is_null(v)) {
        return v;  // NULL 传播
    }
    if (const auto* d = std::get_if<double>(&v)) {
        return EvalValue{-*d};  // 有限值取反仍有限
    }
    const int64_t i = std::get<int64_t>(v);
    if (i == INT64_MIN) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型取反溢出");
    }
    return EvalValue{-i};
}

// 双目算术: 任一操作数为 NULL 结果 NULL, 类型驱动提升(int/int 向零截断), 溢出/除零当场报错;
// 操作数数值性由语义层保证
EvalValue eval_binary(char op, const ana::BoundExpr& le, const ana::BoundExpr& re,
                      const st::Row* row)
{
    const EvalValue lv = eval_bound(le, row);
    const EvalValue rv = eval_bound(re, row);
    if (is_null(lv) || is_null(rv)) {
        return EvalValue{};  // NULL 传播, 短路于除零/溢出检查
    }
    if (std::holds_alternative<double>(lv) || std::holds_alternative<double>(rv)) {
        double out = 0.0;
        switch (op) {
        case '+': out = to_double(lv) + to_double(rv); break;
        case '-': out = to_double(lv) - to_double(rv); break;
        case '*': out = to_double(lv) * to_double(rv); break;
        case '/': out = to_double(lv) / to_double(rv); break;
        }
        if (!std::isfinite(out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "浮点运算溢出或除零");
        }
        return EvalValue{out};
    }
    const int64_t l = std::get<int64_t>(lv);
    const int64_t r = std::get<int64_t>(rv);
    int64_t out = 0;
    switch (op) {
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
    return EvalValue{out};
}

// 字符串比较: 来自 char 定长列的一侧去尾随空格(PAD SPACE 语义)
int cmp_str(const StrVal& l, const StrVal& r)
{
    const std::string_view lv = l.from_char ? rtrim_space(l.text) : l.text;
    const std::string_view rv = r.from_char ? rtrim_space(r.text) : r.text;
    return lv.compare(rv);
}

// 比较: 任一侧 NULL 即 NULL; 纯整型整数比较, 数值混合提升为 double, 字符串按 PAD SPACE 语义,
// 布尔按 false<true; 两侧同类由语义层保证
EvalValue eval_compare(CmpOp op, const ana::BoundExpr& le, const ana::BoundExpr& re,
                       const st::Row* row)
{
    const EvalValue lv = eval_bound(le, row);
    const EvalValue rv = eval_bound(re, row);
    if (is_null(lv) || is_null(rv)) {
        return EvalValue{};  // NULL 传播, 求值结果即 UNKNOWN
    }
    int c = 0;
    if (const StrVal* ls = std::get_if<StrVal>(&lv)) {
        c = cmp_str(*ls, std::get<StrVal>(rv));
    } else if (const bool* lb = std::get_if<bool>(&lv)) {
        const bool rb = std::get<bool>(rv);
        c = *lb == rb ? 0 : (*lb ? 1 : -1);
    } else if (std::holds_alternative<int64_t>(lv) && std::holds_alternative<int64_t>(rv)) {
        const int64_t l = std::get<int64_t>(lv);
        const int64_t r = std::get<int64_t>(rv);
        c = l < r ? -1 : (l > r ? 1 : 0);
    } else {
        const double l = to_double(lv);
        const double r = to_double(rv);
        c = l < r ? -1 : (l > r ? 1 : 0);
    }
    switch (op) {
    case CmpOp::Eq: return EvalValue{c == 0};
    case CmpOp::Ne: return EvalValue{c != 0};
    case CmpOp::Lt: return EvalValue{c < 0};
    case CmpOp::Le: return EvalValue{c <= 0};
    case CmpOp::Gt: return EvalValue{c > 0};
    case CmpOp::Ge: return EvalValue{c >= 0};
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 未知比较种类");
}

// AND/OR: 三值逻辑, AND 有 false 即 false / OR 有 true 即 true, 其余 NULL 传播;
// 操作数布尔性由语义层保证
EvalValue eval_logic(LogicOp op, const ana::BoundExpr& le, const ana::BoundExpr& re,
                     const st::Row* row)
{
    const EvalValue lv = eval_bound(le, row);
    const EvalValue rv = eval_bound(re, row);
    const bool* lb = std::get_if<bool>(&lv);
    const bool* rb = std::get_if<bool>(&rv);
    if (op == LogicOp::And) {
        if ((lb != nullptr && !*lb) || (rb != nullptr && !*rb)) {
            return EvalValue{false};  // 有 false 即 false
        }
        if (lb == nullptr || rb == nullptr) {
            return EvalValue{};  // 有 UNKNOWN 即 UNKNOWN
        }
        return EvalValue{true};
    }
    if ((lb != nullptr && *lb) || (rb != nullptr && *rb)) {
        return EvalValue{true};  // 有 true 即 true
    }
    if (lb == nullptr || rb == nullptr) {
        return EvalValue{};  // 有 UNKNOWN 即 UNKNOWN
    }
    return EvalValue{false};
}

// NOT: 三值逻辑, NULL 传播; 操作数布尔性由语义层保证
EvalValue eval_not(const ana::BoundExpr& operand, const st::Row* row)
{
    const EvalValue v = eval_bound(operand, row);
    if (is_null(v)) {
        return v;
    }
    return EvalValue{!std::get<bool>(v)};
}

// IS [NOT] NULL: 对任意类型操作数判空
EvalValue eval_is_null(const ana::BoundIsNull& e, const st::Row* row)
{
    const EvalValue v = eval_bound(*e.operand, row);
    return EvalValue{e.negate ? !is_null(v) : is_null(v)};
}

// 统一求值入口: row 为空表示常量上下文(绑定树不含列引用)
EvalValue eval_bound(const ana::BoundExpr& expr, const st::Row* row)
{
    switch (expr.kind()) {
    case ana::BoundExprKind::Const: {
        const auto& v = static_cast<const ana::BoundConst&>(expr).value;
        if (const auto* d = std::get_if<double>(&v); d != nullptr && !std::isfinite(*d)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "浮点字面量超出可表示范围");
        }
        return std::visit(
            [](const auto& val) -> EvalValue {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return EvalValue{StrVal{val, false}};
                } else {
                    return val;
                }
            },
            v);
    }
    case ana::BoundExprKind::ColRef: {
        const auto& ref = static_cast<const ana::BoundColRef&>(expr);
        if (row == nullptr) {
            DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "常量上下文不允许引用列");
        }
        const st::Value& raw = row->values[ref.col_idx];
        return std::visit(
            [from_char = ref.from_char](const auto& val) -> EvalValue {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return EvalValue{StrVal{val, from_char}};
                } else {
                    return val;
                }
            },
            raw);
    }
    case ana::BoundExprKind::Arith: {
        const auto& e = static_cast<const ana::BoundArith&>(expr);
        return eval_binary(e.op, *e.left, *e.right, row);
    }
    case ana::BoundExprKind::Neg:
        return eval_neg(*static_cast<const ana::BoundNeg&>(expr).operand, row);
    case ana::BoundExprKind::Cmp: {
        const auto& e = static_cast<const ana::BoundCmp&>(expr);
        return eval_compare(e.op, *e.left, *e.right, row);
    }
    case ana::BoundExprKind::Logic: {
        const auto& e = static_cast<const ana::BoundLogic&>(expr);
        return eval_logic(e.op, *e.left, *e.right, row);
    }
    case ana::BoundExprKind::Not:
        return eval_not(*static_cast<const ana::BoundNot&>(expr).operand, row);
    case ana::BoundExprKind::IsNull:
        return eval_is_null(static_cast<const ana::BoundIsNull&>(expr), row);
    }
    // 不可达: 全部绑定表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 未知绑定表达式节点");
}

}  // namespace

// 常量上下文求值(INSERT VALUES 等无行上下文的场景, 树中不含列引用)
EvalValue eval_const(const ana::BoundExpr& e)
{
    return eval_bound(e, nullptr);
}

// 行上下文求值(SELECT 投影与 WHERE 过滤)
EvalValue eval_row(const ana::BoundExpr& e, const st::Row& row)
{
    return eval_bound(e, &row);
}

// 求值结果转存储/输出值: 原样转换, StrVal 剥离 char 定长标记(monostate 即 NULL)
st::Value to_st_value(const EvalValue& v)
{
    return std::visit(
        [](const auto& val) -> st::Value {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, StrVal>) {
                return val.text;
            } else {
                return val;
            }
        },
        v);
}

// WHERE 条件判定: NULL(UNKNOWN) 视为不满足, bool 由语义层保证
bool where_match(const ana::BoundExpr& where, const st::Row& row)
{
    const EvalValue v = eval_bound(where, &row);
    if (is_null(v)) {
        return false;
    }
    return std::get<bool>(v);
}

}  // namespace expr
