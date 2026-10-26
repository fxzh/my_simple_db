// expr_eval.cpp: 表达式求值器实现
#include "expr_eval.h"
#include "expr_func.h"
#include "expr_value.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "common/err.h"
#include "log/log.h"
#include "storage/cast.h"

namespace expr {

namespace mp = boost::multiprecision;

namespace {

st::Value eval_bound(const ana::BoundExpr& expr, const st::Row* row);

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

// unscaled 对齐放大到目标标度(须不低于当前标度): 逐位乘 10, 幅值达 38 位十进制报错
mp::int256_t dec_align(mp::int256_t u, uint8_t from, uint8_t to)
{
    for (int i = from; i < to; ++i) {
        u *= 10;
        if (u != 0 && mp::abs(u) >= st::kNumericUnscaledMax) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "numeric 算术结果超界");
        }
    }
    return u;
}

// numeric 除法: 逐位长除到目标标度并四舍五入(远离零), 每步幅值 < 10^39;
// 除零与结果达 38 位十进制当场报错, 返回商的 unscaled
mp::int256_t dec_div(const st::DecimalVal& l, const st::DecimalVal& r, uint8_t rscale)
{
    if (r.unscaled == 0) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "numeric 除零");
    }
    const bool neg = (l.unscaled < 0) != (r.unscaled < 0);
    const mp::int256_t ua = mp::abs(l.unscaled);
    const mp::int256_t ub = mp::abs(r.unscaled);
    // 商 = ua/ub × 10^t, t = 目标标度 + 右标度 - 左标度(推导保证非负)
    const long long t = static_cast<long long>(rscale) + r.scale - l.scale;
    if (t < 0) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: numeric 除法标度推导违约");
    }
    const mp::int256_t gate = st::kNumericUnscaledMax / 10;  // 10^37, 再放大必超 38 位
    mp::int256_t q = ua / ub;
    mp::int256_t rem = ua % ub;
    for (long long i = 0; i < t; ++i) {
        if (q >= gate) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "numeric 算术结果超界");
        }
        q = q * 10 + rem * 10 / ub;
        rem = rem * 10 % ub;
    }
    if (rem * 2 >= ub) {
        ++q;  // 余数达半即远离零进位
    }
    if (q >= st::kNumericUnscaledMax) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "numeric 算术结果超界");
    }
    return neg ? -q : q;
}

// numeric 结果标度按操作数值标度推导: 加减取大, 乘取和(超 255 报错),
// 除取大加 4(封顶 255); 裸 numeric 列值标度不可静态知, 故在求值期推导
uint8_t dec_result_scale(char op, const st::DecimalVal& l, const st::DecimalVal& r)
{
    const int hi = l.scale > r.scale ? l.scale : r.scale;
    int s = 0;
    if (op == '+' || op == '-') {
        s = hi;
    } else if (op == '*') {
        s = static_cast<int>(l.scale) + r.scale;
    } else {
        s = std::min(hi + 4, 255);
    }
    if (s > 255) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "numeric 乘法标度超界");
    }
    return static_cast<uint8_t>(s);
}

// numeric 四则: 加减对齐到目标标度后加减(幅值 < 2×10^38), 乘直乘(幅值 < 10^76),
// 除走长除; 结果达 38 位十进制当场报错, 返回结果 unscaled(标度即目标标度)
mp::int256_t dec_arith(char op, const st::DecimalVal& l, const st::DecimalVal& r,
                       uint8_t rscale)
{
    mp::int256_t out = 0;
    switch (op) {
    case '+':
    case '-': {
        const mp::int256_t lu = dec_align(l.unscaled, l.scale, rscale);
        const mp::int256_t ru = dec_align(r.unscaled, r.scale, rscale);
        out = op == '+' ? lu + ru : lu - ru;
        break;
    }
    case '*':
        out = l.unscaled * r.unscaled;
        break;
    case '/':
        return dec_div(l, r, rscale);
    }
    if (mp::abs(out) >= st::kNumericUnscaledMax) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "numeric 算术结果超界");
    }
    return out;
}

// 幅值十进制位数(u 须非负)
int dec_digits(const mp::int256_t& u)
{
    int n = 0;
    mp::int256_t t = u;
    while (t > 0) {
        t /= 10;
        ++n;
    }
    return n;
}

// numeric 精确三向比较: 零值先行, 符号次之, 幅值按(位数-标度)定位阶,
// 同阶低位侧放大对齐后比较(位数差即标度差, 对齐侧幅值 < 10^38)
int dec_cmp(const st::DecimalVal& a, const st::DecimalVal& b)
{
    const bool an = a.unscaled < 0;
    const bool bn = b.unscaled < 0;
    const bool az = a.unscaled == 0;
    const bool bz = b.unscaled == 0;
    if (az || bz) {
        if (az && bz) {
            return 0;
        }
        if (az) {
            return bn ? 1 : -1;
        }
        return an ? -1 : 1;
    }
    if (an != bn) {
        return an ? -1 : 1;
    }
    const mp::int256_t ua = mp::abs(a.unscaled);
    const mp::int256_t ub = mp::abs(b.unscaled);
    const long long ea = dec_digits(ua) - a.scale;
    const long long eb = dec_digits(ub) - b.scale;
    if (ea != eb) {
        const bool a_smaller = ea < eb;
        return (an ? !a_smaller : a_smaller) ? -1 : 1;
    }
    mp::int256_t ma = ua;
    mp::int256_t mb = ub;
    if (a.scale > b.scale) {
        for (int i = b.scale; i < a.scale; ++i) {
            mb *= 10;
        }
    } else {
        for (int i = a.scale; i < b.scale; ++i) {
            ma *= 10;
        }
    }
    if (ma == mb) {
        return 0;
    }
    const bool m_smaller = ma < mb;
    return (an ? !m_smaller : m_smaller) ? -1 : 1;
}

// 值族: 数值/文本/布尔, 供比较分派; 值无类型时归 None(属性内联于值)
st::Family value_family(const st::Value& v)
{
    return v.type.family;
}

// 一元负号: NULL 传播, 取反; 结果按绑定期推导域构造并按域做溢出检查
st::Value eval_neg(const ana::BoundNeg& e, const st::Row* row)
{
    const st::Value v = eval_bound(*e.operand, row);
    if (is_null(v)) {
        return v;  // NULL 传播
    }
    if (e.type.rep == st::Rep::Float) {
        return st::float_val(e.type, -to_float(v, e.type));  // 有限值取反仍有限
    }
    if (e.type.rep == st::Rep::Double) {
        return st::double_val(e.type, -to_double(v, e.type));  // 有限值取反仍有限
    }
    if (e.type.rep == st::Rep::Dec) {
        const st::DecimalVal& d = get_dec(v);
        return st::numeric_val(e.type, -d.unscaled, d.scale);  // 幅值不变
    }
    const int64_t i = get_int(v, e.type);
    if (i == INT64_MIN || (e.type.width == 4 && i == INT32_MIN)) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型取反溢出");
    }
    return st::int_val(e.type, -i);
}

// 双目算术: 任一操作数为 NULL 结果 NULL, 按绑定期推导域计算与构造, 类型驱动提升
// (整型向零截断), 溢出/除零当场报错; 操作数数值性由语义层保证
st::Value eval_binary(const ana::BoundArith& e, const st::Row* row)
{
    const st::Value lv = eval_bound(*e.left, row);
    const st::Value rv = eval_bound(*e.right, row);
    if (is_null(lv) || is_null(rv)) {
        return st::Value{};  // NULL 传播, 短路于除零/溢出检查
    }
    if (e.type.rep == st::Rep::Dec) {
        // 操作数转 numeric 域, 结果标度按操作数值标度推导
        const st::DecimalVal l = to_numeric(lv, e.type);
        const st::DecimalVal r = to_numeric(rv, e.type);
        const uint8_t rs = dec_result_scale(e.op, l, r);
        return st::numeric_val(e.type, dec_arith(e.op, l, r, rs), rs);
    }
    if (e.type.rep == st::Rep::Float) {
        // 单精度域逐运算舍入
        const float l = to_float(lv, e.type);
        const float r = to_float(rv, e.type);
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
        return st::float_val(e.type, out);
    }
    if (e.type.rep == st::Rep::Double) {
        double out = 0.0;
        switch (e.op) {
        case '+': out = to_double(lv, e.type) + to_double(rv, e.type); break;
        case '-': out = to_double(lv, e.type) - to_double(rv, e.type); break;
        case '*': out = to_double(lv, e.type) * to_double(rv, e.type); break;
        case '/': out = to_double(lv, e.type) / to_double(rv, e.type); break;
        }
        if (!std::isfinite(out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "浮点运算溢出或除零");
        }
        return st::double_val(e.type, out);
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
    if (e.type.width == 4 && (out < INT32_MIN || out > INT32_MAX)) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, arith_overflow_msg(e.op));
    }
    return st::int_val(e.type, out);
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
    case CmpOp::Eq: return st::bool_val(st::kBool, c == 0);
    case CmpOp::Ne: return st::bool_val(st::kBool, c != 0);
    case CmpOp::Lt: return st::bool_val(st::kBool, c < 0);
    case CmpOp::Le: return st::bool_val(st::kBool, c <= 0);
    case CmpOp::Gt: return st::bool_val(st::kBool, c > 0);
    case CmpOp::Ge: return st::bool_val(st::kBool, c >= 0);
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
            return st::bool_val(st::kBool, false);  // 有 false 即 false
        }
        if (lb == nullptr || rb == nullptr) {
            return st::Value{};  // 有 UNKNOWN 即 UNKNOWN
        }
        return st::bool_val(st::kBool, true);
    }
    if ((lb != nullptr && *lb) || (rb != nullptr && *rb)) {
        return st::bool_val(st::kBool, true);  // 有 true 即 true
    }
    if (lb == nullptr || rb == nullptr) {
        return st::Value{};  // 有 UNKNOWN 即 UNKNOWN
    }
    return st::bool_val(st::kBool, false);
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
    return st::bool_val(st::kBool, !*b);
}

// IS [NOT] NULL: 对任意类型操作数判空
st::Value eval_is_null(const ana::BoundIsNull& e, const st::Row* row)
{
    const st::Value v = eval_bound(*e.operand, row);
    return st::bool_val(st::kBool, e.negate ? !is_null(v) : is_null(v));
}

// 函数调用: 严格重载任一实参为 NULL 即原样传布, 非严格重载 NULL 实参照常传入实现;
// 其余经求值表按绑定期固定域计算并构造, 整型溢出当场报错; 实参个数与类型由语义层保证
st::Value eval_func_call(const ana::BoundFuncCall& e, const st::Row* row)
{
    std::vector<st::Value> argv;
    argv.reserve(e.args.size());
    for (const std::unique_ptr<ana::BoundExpr>& a : e.args) {
        argv.push_back(eval_bound(*a, row));
    }
    if (e.strict_null) {
        for (const st::Value& v : argv) {
            if (is_null(v)) {
                return v;  // NULL 严格传布
            }
        }
    }
    return eval_builtin(e.func, argv, e.type);
}

// CAST: NULL 传播; 转换经 cast 分派表显式模式执行, numeric 声明目标走标度归一;
// int 目标超 int32 域与文本目标超声明长度(仅文本目标 length 非 0)当场报错
st::Value eval_cast(const ana::BoundCast& e, const st::Row* row)
{
    const st::Value v = eval_bound(*e.operand, row);
    if (is_null(v)) {
        return v;  // NULL 传播
    }
    if (e.type.rep == st::Rep::Dec && e.length > 0) {
        return st::coerce_numeric(v, e.type, e.length, e.scale, st::CastMode::Explicit);
    }
    const st::Value out = st::cast_value(v, e.type, st::CastMode::Explicit);
    if (e.type.rep == st::Rep::Int64 && e.type.width == 4) {
        const int64_t* i = std::get_if<int64_t>(&out.box);
        if (i == nullptr) {
            DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: cast 产物表示与目标域不符");
        }
        if (*i < INT32_MIN || *i > INT32_MAX) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXPR, "整型值超出 int 可表示范围");
        }
    }
    if (e.type.family == st::Family::Text && e.length > 0) {
        const std::string* s = std::get_if<std::string>(&out.box);
        if (s == nullptr) {
            DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: cast 产物表示与目标域不符");
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
// 数值家族中整型与 numeric 间走精确比较, 浮点参与提升 double; 字符串字典序且仅 Char 侧
// 去尾随空格; 家族不可比报 Internal
int value_cmp(const st::Value& a, const st::Value& b)
{
    const bool an = is_null(a);
    const bool bn = is_null(b);
    if (an || bn) {
        return an && bn ? 0 : (an ? 1 : -1);  // NULL 最大, 双 NULL 相等
    }
    switch (value_family(a)) {
    case st::Family::Num: {
        if (value_family(b) != st::Family::Num) {
            break;
        }
        const int64_t* li = std::get_if<int64_t>(&a.box);
        const int64_t* ri = std::get_if<int64_t>(&b.box);
        if (li != nullptr && ri != nullptr) {
            return *li < *ri ? -1 : (*li > *ri ? 1 : 0);  // 纯整型精确比较
        }
        const st::DecimalVal* ld = std::get_if<st::DecimalVal>(&a.box);
        const st::DecimalVal* rd = std::get_if<st::DecimalVal>(&b.box);
        if (ld != nullptr && rd != nullptr) {
            return dec_cmp(*ld, *rd);  // 纯 numeric 精确比较
        }
        // numeric 与整型互比: 整型侧按标度 0 转 numeric 后精确比较
        if (ld != nullptr && ri != nullptr) {
            return dec_cmp(*ld, st::DecimalVal{mp::int256_t(*ri), 0});
        }
        if (li != nullptr && rd != nullptr) {
            return dec_cmp(st::DecimalVal{mp::int256_t(*li), 0}, *rd);
        }
        const double l = box_f64(a);  // 浮点参与的混合比较提升 double
        const double r = box_f64(b);
        return l < r ? -1 : (l > r ? 1 : 0);
    }
    case st::Family::Text: {
        if (value_family(b) != st::Family::Text) {
            break;
        }
        const std::string* ls = std::get_if<std::string>(&a.box);
        const std::string* rs = std::get_if<std::string>(&b.box);
        if (ls == nullptr || rs == nullptr) {
            break;
        }
        // 仅定长侧去尾随空格(PAD SPACE 语义)
        const std::string_view lv = a.type.store == st::Store::CharPad ? rtrim_space(*ls) : *ls;
        const std::string_view rv = b.type.store == st::Store::CharPad ? rtrim_space(*rs) : *rs;
        return lv.compare(rv);
    }
    case st::Family::Bool: {
        if (value_family(b) != st::Family::Bool) {
            break;
        }
        const bool* lb = std::get_if<bool>(&a.box);
        const bool* rb = std::get_if<bool>(&b.box);
        if (lb == nullptr || rb == nullptr) {
            break;
        }
        return *lb == *rb ? 0 : (*lb ? 1 : -1);
    }
    case st::Family::None:
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
