// expr_eval.cpp: 表达式求值器实现
#include "expr_eval.h"

#include <cmath>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "common/err.h"
#include "log/log.h"

namespace exec {

namespace {

EvalValue eval_expr(const Expr& expr, const ana::Schema* ctx, const st::Row* row);

// 数值转 double(整型提升)
double to_double(const EvalValue& v)
{
    if (const auto* d = std::get_if<double>(&v)) {
        return *d;
    }
    return static_cast<double>(std::get<int64_t>(v));
}

// 值是否数值(int64/double)
bool is_number(const EvalValue& v)
{
    return std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v);
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

// 一元数值运算: NULL 传播, '+' 原值返回, '-' 取反
EvalValue eval_unary(char op, const Expr& operand, const ana::Schema* ctx, const st::Row* row)
{
    const EvalValue v = eval_expr(operand, ctx, row);
    if (is_null(v)) {
        return v;  // NULL 传播
    }
    if (!is_number(v)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "一元运算操作数不是数值");
    }
    if (op == '+') {
        return v;
    }
    if (const auto* d = std::get_if<double>(&v)) {
        return EvalValue{-*d};  // 有限值取反仍有限
    }
    const int64_t i = std::get<int64_t>(v);
    if (i == INT64_MIN) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型取反溢出");
    }
    return EvalValue{-i};
}

// 双目算术: 任一操作数为 NULL 结果 NULL, 非 NULL 操作数须数值; 类型驱动提升(int/int 向零截断), 溢出/除零当场报错
EvalValue eval_binary(char op, const Expr& le, const Expr& re, const ana::Schema* ctx, const st::Row* row)
{
    const EvalValue lv = eval_expr(le, ctx, row);
    const EvalValue rv = eval_expr(re, ctx, row);
    if ((!is_null(lv) && !is_number(lv)) || (!is_null(rv) && !is_number(rv))) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "算术运算操作数不是数值");
    }
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
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "浮点运算溢出或除零");
        }
        return EvalValue{out};
    }
    const int64_t l = std::get<int64_t>(lv);
    const int64_t r = std::get<int64_t>(rv);
    int64_t out = 0;
    switch (op) {
    case '+':
        if (__builtin_add_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型加法溢出");
        }
        break;
    case '-':
        if (__builtin_sub_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型减法溢出");
        }
        break;
    case '*':
        if (__builtin_mul_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型乘法溢出");
        }
        break;
    case '/':
        if (r == 0) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型除零");
        }
        if (l == INT64_MIN && r == -1) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型除法溢出");
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

// 比较: 任一侧 NULL 即 NULL; 数值提升为 double 比较, 字符串按 PAD SPACE 语义, 跨类当场报错
EvalValue eval_compare(CmpOp op, const Expr& le, const Expr& re, const ana::Schema* ctx,
                       const st::Row* row)
{
    const EvalValue lv = eval_expr(le, ctx, row);
    const EvalValue rv = eval_expr(re, ctx, row);
    if (is_null(lv) || is_null(rv)) {
        return EvalValue{};  // NULL 传播, 求值结果即 UNKNOWN
    }
    const StrVal* ls = std::get_if<StrVal>(&lv);
    const StrVal* rs = std::get_if<StrVal>(&rv);
    int c = 0;
    if (ls != nullptr || rs != nullptr) {
        if (ls == nullptr || rs == nullptr) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "比较运算两侧须同为数值或字符串");
        }
        c = cmp_str(*ls, *rs);
    } else if (is_number(lv) && is_number(rv)) {
        const double l = to_double(lv);
        const double r = to_double(rv);
        c = l < r ? -1 : (l > r ? 1 : 0);
    } else {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "比较运算两侧须同为数值或字符串");
    }
    switch (op) {
    case CmpOp::Eq: return EvalValue{c == 0};
    case CmpOp::Ne: return EvalValue{c != 0};
    case CmpOp::Lt: return EvalValue{c < 0};
    case CmpOp::Le: return EvalValue{c <= 0};
    case CmpOp::Gt: return EvalValue{c > 0};
    case CmpOp::Ge: return EvalValue{c >= 0};
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 未知比较种类");
}

// AND/OR: 三值逻辑, AND 有 false 即 false / OR 有 true 即 true, 其余 NULL 传播; 非 NULL 操作数须为 bool
EvalValue eval_logic(LogicOp op, const Expr& le, const Expr& re, const ana::Schema* ctx,
                     const st::Row* row)
{
    const EvalValue lv = eval_expr(le, ctx, row);
    const EvalValue rv = eval_expr(re, ctx, row);
    const bool* lb = std::get_if<bool>(&lv);
    const bool* rb = std::get_if<bool>(&rv);
    if ((lb == nullptr && !is_null(lv)) || (rb == nullptr && !is_null(rv))) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "逻辑运算操作数不是布尔值");
    }
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

// NOT: 三值逻辑, NULL 传播, 非 NULL 操作数须为 bool
EvalValue eval_not(const Expr& operand, const ana::Schema* ctx, const st::Row* row)
{
    const EvalValue v = eval_expr(operand, ctx, row);
    if (is_null(v)) {
        return v;
    }
    const bool* b = std::get_if<bool>(&v);
    if (b == nullptr) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "逻辑运算操作数不是布尔值");
    }
    return EvalValue{!*b};
}

// IS [NOT] NULL: 对任意类型操作数判空
EvalValue eval_is_null(const IsNullExpr& e, const ana::Schema* ctx, const st::Row* row)
{
    const EvalValue v = eval_expr(*e.operand, ctx, row);
    return EvalValue{e.negate ? !is_null(v) : is_null(v)};
}

// 统一求值入口: ctx/row 同时为空表示常量上下文(标识符不可用)
EvalValue eval_expr(const Expr& expr, const ana::Schema* ctx, const st::Row* row)
{
    switch (expr.kind()) {
    case ExprKind::Int:
        return EvalValue{static_cast<const IntExpr&>(expr).value};
    case ExprKind::Float: {
        const double d = static_cast<const FloatExpr&>(expr).value;
        if (!std::isfinite(d)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "浮点字面量超出可表示范围");
        }
        return EvalValue{d};
    }
    case ExprKind::String:
        return EvalValue{StrVal{static_cast<const StringExpr&>(expr).value, false}};
    case ExprKind::Null:
        return EvalValue{};
    case ExprKind::Identifier: {
        const auto& id = static_cast<const IdentifierExpr&>(expr);
        if (ctx == nullptr) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "常量上下文不允许引用列: {}",
                     id.name);
        }
        const auto it = ctx->cols.find(id.name);
        if (it == ctx->cols.end()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::EXECUTOR, "列不存在: {}", id.name);
        }
        const st::Value& raw = row->values[it->second];
        const bool from_char = ctx->char_col[it->second];
        return std::visit(
            [from_char](const auto& val) -> EvalValue {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return EvalValue{StrVal{val, from_char}};
                } else {
                    return val;
                }
            },
            raw);
    }
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        return eval_binary(e.op, *e.left, *e.right, ctx, row);
    }
    case ExprKind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        return eval_unary(e.op, *e.operand, ctx, row);
    }
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        return eval_compare(e.op, *e.left, *e.right, ctx, row);
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        return eval_logic(e.op, *e.left, *e.right, ctx, row);
    }
    case ExprKind::Not: {
        const auto& e = static_cast<const NotExpr&>(expr);
        return eval_not(*e.operand, ctx, row);
    }
    case ExprKind::IsNull:
        return eval_is_null(static_cast<const IsNullExpr&>(expr), ctx, row);
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 未知表达式节点");
}

}  // namespace

// 常量上下文求值(INSERT VALUES 等无行上下文的场景)
EvalValue eval_const(const Expr& e)
{
    return eval_expr(e, nullptr, nullptr);
}

// 行上下文求值(SELECT 投影与 WHERE 过滤)
EvalValue eval_row(const Expr& e, const ana::Schema& ctx, const st::Row& row)
{
    return eval_expr(e, &ctx, &row);
}

// 求值结果转存储/输出值: bool 不允许作为结果值, 其余原样(monostate 即 NULL)
st::Value to_st_value(const EvalValue& v)
{
    if (std::holds_alternative<bool>(v)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "布尔值不可作为存储或输出值");
    }
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

// WHERE 条件判定: 结果须为 bool, NULL(UNKNOWN) 视为不满足
bool where_match(const Expr& where, const ana::Schema& ctx, const st::Row& row)
{
    const EvalValue v = eval_expr(where, &ctx, &row);
    if (is_null(v)) {
        return false;
    }
    const bool* b = std::get_if<bool>(&v);
    if (b == nullptr) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "WHERE 条件不是布尔表达式");
    }
    return *b;
}

}  // namespace exec
