// expr_func_math.cpp: 数值类内建函数实现
#include "expr_func.h"
#include "expr_value.h"

#include <cmath>
#include <cstdint>

#include "common/err.h"
#include "log/log.h"

namespace expr {

namespace mp = boost::multiprecision;

namespace {

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

// 10 的 e 次幂(e 须在 [0,38] 内), 超界视为约定违约
mp::int256_t dec_pow10(long long e)
{
    if (e < 0 || e > 38) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 10 的幂指数超界");
    }
    mp::int256_t p = 1;
    for (long long i = 0; i < e; ++i) {
        p *= 10;
    }
    return p;
}

// numeric 四舍五入(半值远离零)到 n 位小数, n 可为负: n 不小于原标度原值返回;
// shift = 原标度 - n, shift 达 39 时幅值不足除数之半, 商 0 不进位, 结果精确为 0;
// n 为负时商乘回 10^(-n) 且结果标度 0, 否则结果标度 n; 结果幅值达 38 位十进制报错
st::Value round_dec(const st::DecimalVal& d, int64_t n, st::TypeAttr ret)
{
    if (n >= d.scale) {
        return st::numeric_val(ret, d.unscaled, d.scale);
    }
    const bool neg = d.unscaled < 0;
    const mp::int256_t ua = mp::abs(d.unscaled);
    const long long shift = static_cast<long long>(d.scale) - n;
    mp::int256_t q = 0;
    if (shift < 39) {
        const mp::int256_t p = dec_pow10(shift);
        q = ua / p;
        const mp::int256_t rem = ua % p;
        if (rem * 2 >= p) {
            ++q;  // 余数达半即远离零进位
        }
        if (n < 0) {
            q *= dec_pow10(-n);
        }
    }
    if (q >= st::kNumericUnscaledMax) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "numeric 算术结果超界");
    }
    return st::numeric_val(ret, neg ? -q : q, n < 0 ? 0 : static_cast<uint8_t>(n));
}

// numeric 向 -inf(up 为假)/+inf(up 为真)取整, 结果标度 0: 标度 0 原值返回;
// 标度达 39 时幅值必小于 1, 结果为 -1/0(floor) 或 0/1(ceil);
// 否则按 10^标度 移位后整除, 有余且取整方向远离零时幅值进 1
st::Value trunc_dec(const st::DecimalVal& d, bool up, st::TypeAttr ret)
{
    if (d.scale == 0) {
        return st::numeric_val(ret, d.unscaled, 0);
    }
    const bool neg = d.unscaled < 0;
    if (d.scale >= 39) {
        const mp::int256_t out = d.unscaled == 0 ? 0 : (up ? (neg ? 0 : 1) : (neg ? -1 : 0));
        return st::numeric_val(ret, out, 0);
    }
    const mp::int256_t p = dec_pow10(d.scale);
    const mp::int256_t ua = mp::abs(d.unscaled);
    mp::int256_t q = ua / p;
    if (ua % p != 0 && up != neg) {
        ++q;
    }
    return st::numeric_val(ret, neg ? -q : q, 0);
}

}  // namespace

st::Value eval_abs_int(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    const int64_t i = get_i64(args[0]);
    if (i == INT32_MIN) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型绝对值溢出");
    }
    return st::int_val(ret, i < 0 ? -i : i);
}

st::Value eval_abs_bigint(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    const int64_t i = get_i64(args[0]);
    if (i == INT64_MIN) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型绝对值溢出");
    }
    return st::int_val(ret, i < 0 ? -i : i);
}

st::Value eval_abs_double(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::double_val(ret, std::fabs(get_f64(args[0])));
}

st::Value eval_abs_float(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::float_val(ret, std::fabs(get_f32(args[0])));
}

st::Value eval_abs_numeric(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    const st::DecimalVal& d = get_dec(args[0]);
    return st::numeric_val(ret, d.unscaled < 0 ? -d.unscaled : d.unscaled, d.scale);
}

st::Value eval_round_int(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::int_val(ret, get_i64(args[0]));
}

st::Value eval_round_bigint(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::int_val(ret, get_i64(args[0]));
}

st::Value eval_round_double(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::double_val(ret, std::round(get_f64(args[0])));
}

st::Value eval_round_float(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::float_val(ret, std::round(get_f32(args[0])));
}

st::Value eval_round_numeric(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return round_dec(get_dec(args[0]), 0, ret);
}

st::Value eval_round_numeric_int(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return round_dec(get_dec(args[0]), get_i64(args[1]), ret);
}

st::Value eval_floor_int(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::int_val(ret, get_i64(args[0]));
}

st::Value eval_floor_bigint(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::int_val(ret, get_i64(args[0]));
}

st::Value eval_floor_double(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::double_val(ret, std::floor(get_f64(args[0])));
}

st::Value eval_floor_float(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::float_val(ret, std::floor(get_f32(args[0])));
}

st::Value eval_floor_numeric(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return trunc_dec(get_dec(args[0]), false, ret);
}

st::Value eval_ceil_int(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::int_val(ret, get_i64(args[0]));
}

st::Value eval_ceil_bigint(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::int_val(ret, get_i64(args[0]));
}

st::Value eval_ceil_double(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::double_val(ret, std::ceil(get_f64(args[0])));
}

st::Value eval_ceil_float(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return st::float_val(ret, std::ceil(get_f32(args[0])));
}

st::Value eval_ceil_numeric(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    return trunc_dec(get_dec(args[0]), true, ret);
}

st::Value eval_mod_int(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    const int64_t l = get_i64(args[0]);
    const int64_t r = get_i64(args[1]);
    if (r == 0) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型取余除零");
    }
    if (l == INT64_MIN && r == -1) {
        return st::int_val(ret, 0);
    }
    return st::int_val(ret, l % r);
}

st::Value eval_mod_bigint(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    const int64_t l = get_i64(args[0]);
    const int64_t r = get_i64(args[1]);
    if (r == 0) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "整型取余除零");
    }
    if (l == INT64_MIN && r == -1) {
        return st::int_val(ret, 0);
    }
    return st::int_val(ret, l % r);
}

st::Value eval_mod_double(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    const double out = std::fmod(get_f64(args[0]), get_f64(args[1]));
    if (!std::isfinite(out)) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "浮点取余除零");
    }
    return st::double_val(ret, out);
}

st::Value eval_mod_float(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    const float out = std::fmod(get_f32(args[0]), get_f32(args[1]));
    if (!std::isfinite(out)) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "浮点取余除零");
    }
    return st::float_val(ret, out);
}

st::Value eval_mod_numeric(const std::vector<st::Value>& args, st::TypeAttr ret)
{
    const st::DecimalVal& l = get_dec(args[0]);
    const st::DecimalVal& r = get_dec(args[1]);
    if (r.unscaled == 0) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXPR, "numeric 取余除零");
    }
    const uint8_t s = l.scale > r.scale ? l.scale : r.scale;
    const mp::int256_t lu = dec_align(l.unscaled, l.scale, s);
    const mp::int256_t ru = dec_align(r.unscaled, r.scale, s);
    return st::numeric_val(ret, lu % ru, s);
}

}  // namespace expr
