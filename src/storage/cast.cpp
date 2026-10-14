// cast.cpp: 值级转换分派表实现: 隐式仅同族提升/收窄, 显式含跨族转换
#include "cast.h"

#include <charconv>
#include <cmath>
#include <format>
#include <string>
#include <variant>

#include "common/err.h"
#include "log/log.h"

namespace st {

namespace mp = boost::multiprecision;

namespace {

// 取 int64 盒值, 表示不符当场报错
int64_t get_i64(const Value& v)
{
    const int64_t* i = std::get_if<int64_t>(&v.box);
    if (i == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值表示与源域不符");
    }
    return *i;
}

// 取 float 盒值, 表示不符当场报错
float get_f32(const Value& v)
{
    const float* f = std::get_if<float>(&v.box);
    if (f == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值表示与源域不符");
    }
    return *f;
}

// 取 double 盒值, 表示不符当场报错
double get_f64(const Value& v)
{
    const double* d = std::get_if<double>(&v.box);
    if (d == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值表示与源域不符");
    }
    return *d;
}

// 取 string 盒值只读视图, 表示不符当场报错
std::string_view get_str_view(const Value& v)
{
    const std::string* s = std::get_if<std::string>(&v.box);
    if (s == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值表示与源域不符");
    }
    return *s;
}

// 取 Char 域文本视图并去尾随空格(pad 为存储补齐产物)
std::string_view get_char_view(const Value& v)
{
    std::string_view s = get_str_view(v);
    while (!s.empty() && s.back() == ' ') {
        s.remove_suffix(1);
    }
    return s;
}

// 取 bool 盒值, 表示不符当场报错
bool get_bool(const Value& v)
{
    const bool* b = std::get_if<bool>(&v.box);
    if (b == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值表示与源域不符");
    }
    return *b;
}

// 取 DecimalVal 盒值, 表示不符当场报错
const DecimalVal& get_dec(const Value& v)
{
    const DecimalVal* d = std::get_if<DecimalVal>(&v.box);
    if (d == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值表示与源域不符");
    }
    return *d;
}

// 10^n(n<=38): 循环累乘
mp::int256_t pow10(int n)
{
    mp::int256_t r(1);
    for (int i = 0; i < n; ++i) {
        r *= 10;
    }
    return r;
}

// 转换函数: 入参非 NULL, 源值表示即 v.type.rep(由入口按表分派), 产物按 to 定型
using CastFn = Value (*)(const Value& v, TypeAttr to);

// 同形式换类型属性(值不变, 域界与长度由存储编码或语义层检查)
Value restamp(const Value& v, TypeAttr to)
{
    return Value{to, v.box};
}

// int64→float: 单次舍入, 不经 double 中转
Value i64_to_f32(const Value& v, TypeAttr to)
{
    return float_val(to, static_cast<float>(get_i64(v)));
}

// int64→double
Value i64_to_f64(const Value& v, TypeAttr to)
{
    return double_val(to, static_cast<double>(get_i64(v)));
}

// double 收窄为 float: 有限值不可表示当场报错
float narrow_f32(double d)
{
    const float f = static_cast<float>(d);
    if (std::isfinite(d) && !std::isfinite(f)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "浮点值超出 float 可表示范围");
    }
    return f;
}

// double→float: 收窄, 有限值不可表示当场报错
Value f64_to_f32(const Value& v, TypeAttr to)
{
    return float_val(to, narrow_f32(get_f64(v)));
}

// float→double: 宽化无损
Value f32_to_f64(const Value& v, TypeAttr to)
{
    return double_val(to, static_cast<double>(get_f32(v)));
}

// 文本源视图: 定长源去尾随空格(pad 为存储补齐产物)
std::string_view text_view(const Value& v)
{
    return v.type.store == Store::CharPad ? get_char_view(v) : get_str_view(v);
}

// 文本域互换: 定长源去尾随 pad 后转变长, 其余同形式换类型属性(长度由语义层校验)
Value text_to_text(const Value& v, TypeAttr to)
{
    if (v.type.store == Store::CharPad && to.store == Store::VarLen) {
        return str_val(to, std::string(get_char_view(v)));
    }
    return Value{to, v.box};
}

// 文本解析为 int64: 可选 +/- 前缀(from_chars 不认 '+'), 全串参与解析,
// 不容忍空白与非数字字符
int64_t parse_i64(std::string_view s)
{
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
    }
    int64_t out = 0;
    const char* b = s.data();
    const char* e = b + s.size();
    const auto conv = std::from_chars(b, e, out);
    if (s.empty() || conv.ec != std::errc{} || conv.ptr != e) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "字符串转数值失败");
    }
    return out;
}

// 文本解析为 double: 前缀与全串规则同整型, inf/nan 文本与超界值拒绝
double parse_f64(std::string_view s)
{
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
    }
    double out = 0;
    const char* b = s.data();
    const char* e = b + s.size();
    const auto conv = std::from_chars(b, e, out);
    if (s.empty() || conv.ec != std::errc{} || conv.ptr != e || !std::isfinite(out)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "字符串转数值失败");
    }
    return out;
}

// 定点十进制四舍五入(远离零)为 int64, 超 int64 域当场报错
int64_t dec_round_i64(const DecimalVal& d)
{
    mp::int256_t mag = mp::abs(d.unscaled);
    if (d.scale >= 39) {
        return 0;  // 幅值 < 10^38×10^-39 < 0.5, 舍入为 0
    }
    const mp::int256_t p = pow10(d.scale);
    const mp::int256_t r = mag % p;
    mag = mag / p;
    if (r * 2 >= p) {
        ++mag;
    }
    if (mag > mp::int256_t(INT64_MAX)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "numeric 值超出整型可表示范围");
    }
    return static_cast<int64_t>(d.unscaled < 0 ? -mag : mag);
}

// double 向零截断为 int64: 非有限或超 int64 域当场报错
int64_t trunc_to_i64(double d)
{
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "浮点值超出整型可表示范围");
    }
    return static_cast<int64_t>(d);
}

// 定点十进制转 double 的共用路径: 定点文本经 from_chars 舍入
double dec_to_f64_raw(const DecimalVal& d)
{
    return parse_f64(decimal_to_text(d));
}

// ==================== 值表示间转换函数 ====================

// int64(int/bigint 源共用盒形式)转文本: 最短十进制
Value i64_to_text(const Value& v, TypeAttr to)
{
    return str_val(to, std::format("{}", get_i64(v)));
}

// float 转文本: 最短往返表示
Value f32_to_text(const Value& v, TypeAttr to)
{
    return str_val(to, std::format("{}", get_f32(v)));
}

// double 转文本: 最短往返表示
Value f64_to_text(const Value& v, TypeAttr to)
{
    return str_val(to, std::format("{}", get_f64(v)));
}

// bool 转文本
Value bool_to_text(const Value& v, TypeAttr to)
{
    return str_val(to, get_bool(v) ? "true" : "false");
}

// bool 转数值
Value bool_to_i64(const Value& v, TypeAttr to)
{
    return int_val(to, get_bool(v) ? 1 : 0);
}
Value bool_to_f32(const Value& v, TypeAttr to)
{
    return float_val(to, get_bool(v) ? 1.0f : 0.0f);
}
Value bool_to_f64(const Value& v, TypeAttr to)
{
    return double_val(to, get_bool(v) ? 1.0 : 0.0);
}
Value bool_to_dec(const Value& v, TypeAttr to)
{
    return numeric_val(to, mp::int256_t(get_bool(v) ? 1 : 0), 0);
}

// 数值转 Bool: 仅精确 0/1
Value i64_to_bool(const Value& v, TypeAttr to)
{
    const int64_t i = get_i64(v);
    if (i != 0 && i != 1) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "数值转布尔仅接受 0 或 1");
    }
    return bool_val(to, i != 0);
}
Value f32_to_bool(const Value& v, TypeAttr to)
{
    const float f = get_f32(v);
    if (f != 0.0f && f != 1.0f) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "数值转布尔仅接受 0 或 1");
    }
    return bool_val(to, f != 0.0f);
}
Value f64_to_bool(const Value& v, TypeAttr to)
{
    const double d = get_f64(v);
    if (d != 0.0 && d != 1.0) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "数值转布尔仅接受 0 或 1");
    }
    return bool_val(to, d != 0.0);
}
Value dec_to_bool(const Value& v, TypeAttr to)
{
    const DecimalVal& d = get_dec(v);
    if (d.unscaled == 0) {
        return bool_val(to, false);
    }
    // scale<39 时幅值 < 10^38, 与 10^scale 比较判定值是否恰为 1
    if (d.scale < 39 && mp::abs(d.unscaled) == pow10(d.scale)) {
        return bool_val(to, true);
    }
    DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "数值转布尔仅接受 0 或 1");
}

// 文本转 Bool 解析: 仅 true/false, 大小写不敏感
bool parse_bool(std::string_view s)
{
    auto ieq = [&](std::string_view t) {
        if (s.size() != t.size()) {
            return false;
        }
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            if (c != t[i]) {
                return false;
            }
        }
        return true;
    };
    if (ieq("true")) {
        return true;
    }
    if (ieq("false")) {
        return false;
    }
    DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "文本转布尔仅接受 true 或 false");
}

// 文本转 Bool: 仅 true/false, 大小写不敏感
Value text_to_bool(const Value& v, TypeAttr to)
{
    return bool_val(to, parse_bool(text_view(v)));
}

// 文本转数值(int32 域界由存储编码检查)
Value text_to_i64(const Value& v, TypeAttr to)
{
    return int_val(to, parse_i64(text_view(v)));
}
Value text_to_f64(const Value& v, TypeAttr to)
{
    return double_val(to, parse_f64(text_view(v)));
}
Value text_to_f32(const Value& v, TypeAttr to)
{
    return float_val(to, narrow_f32(parse_f64(text_view(v))));
}

// 浮点向零截断为整型: Float 经 double 宽化无损中转
Value f64_to_i64(const Value& v, TypeAttr to)
{
    return int_val(to, trunc_to_i64(get_f64(v)));
}
Value f32_to_i64(const Value& v, TypeAttr to)
{
    return int_val(to, trunc_to_i64(static_cast<double>(get_f32(v))));
}

// int64(int/bigint 源共用盒形式)转定点十进制: scale 0
Value i64_to_dec(const Value& v, TypeAttr to)
{
    return numeric_val(to, mp::int256_t(get_i64(v)), 0);
}

// double 转定点十进制: 最短往返十进制文本再精确解析
Value f64_to_dec(const Value& v, TypeAttr to)
{
    const DecimalVal d = parse_decimal_text(std::format("{}", get_f64(v)));
    return numeric_val(to, d.unscaled, d.scale);
}

// float 转定点十进制: 最短往返十进制文本再精确解析
Value f32_to_dec(const Value& v, TypeAttr to)
{
    const DecimalVal d = parse_decimal_text(std::format("{}", get_f32(v)));
    return numeric_val(to, d.unscaled, d.scale);
}

// 文本转定点十进制
Value text_to_dec(const Value& v, TypeAttr to)
{
    const DecimalVal d = parse_decimal_text(text_view(v));
    return numeric_val(to, d.unscaled, d.scale);
}

// 定点十进制转整型: 四舍五入(远离零), int32 域界由存储编码检查
Value dec_to_i64(const Value& v, TypeAttr to)
{
    return int_val(to, dec_round_i64(get_dec(v)));
}

// 定点十进制转 double: 定点文本经 from_chars 舍入
Value dec_to_f64(const Value& v, TypeAttr to)
{
    return double_val(to, dec_to_f64_raw(get_dec(v)));
}

// 定点十进制转 float: 经 double 中转收窄, 不可表示由收窄报错
Value dec_to_f32(const Value& v, TypeAttr to)
{
    return float_val(to, narrow_f32(dec_to_f64_raw(get_dec(v))));
}

// 定点十进制转文本
Value dec_to_text(const Value& v, TypeAttr to)
{
    return str_val(to, decimal_to_text(get_dec(v)));
}

// 转换分派表项: fn 为空表示无转换, implicit 为真时隐式模式可用
struct CastEntry {
    CastFn fn = nullptr;
    bool implicit = false;
};

// 内置转换分派表: 行源值表示列目标值表示, 下标即 Rep 枚举值
// (None, Int64, Float, Double, Str, Bool, Dec); 非 None 槽位全填,
// 空槽与隐式查到仅显式表项均为调用方违约
constexpr CastEntry kCastTable[7][7] = {
    /* None */   {{}, {}, {}, {}, {}, {}, {}},
    /* Int64 */  {{}, {restamp, true}, {i64_to_f32, true}, {i64_to_f64, true},
                   {i64_to_text}, {i64_to_bool}, {i64_to_dec, true}},
    /* Float */  {{}, {f32_to_i64}, {restamp, true}, {f32_to_f64, true},
                   {f32_to_text}, {f32_to_bool}, {f32_to_dec}},
    /* Double */ {{}, {f64_to_i64}, {f64_to_f32, true}, {restamp, true},
                   {f64_to_text}, {f64_to_bool}, {f64_to_dec}},
    /* Str */    {{}, {text_to_i64}, {text_to_f32}, {text_to_f64},
                   {text_to_text, true}, {text_to_bool}, {text_to_dec}},
    /* Bool */   {{}, {bool_to_i64}, {bool_to_f32}, {bool_to_f64},
                   {bool_to_text}, {restamp, true}, {bool_to_dec}},
    /* Dec */    {{}, {dec_to_i64}, {dec_to_f32, true}, {dec_to_f64, true},
                   {dec_to_text}, {dec_to_bool}, {restamp, true}},
};

}  // namespace

Value cast_value(const Value& v, TypeAttr to, CastMode mode)
{
    if (value_is_null(v)) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 不接受 NULL");
    }
    const CastEntry& e = kCastTable[static_cast<size_t>(v.type.rep)][static_cast<size_t>(to.rep)];
    if (e.fn == nullptr || (mode == CastMode::Implicit && !e.implicit)) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 无 {}→{} 转换",
                 static_cast<int>(v.type.rep), static_cast<int>(to.rep));
    }
    return e.fn(v, to);
}

// 文本解析为定点十进制: [+/-]数字[.数字][eE[+/-]数字], 全串参与, 尾零保留;
// unscaled 幅值达 10^38、scale 超 255 或指数超 1000 当场报错
DecimalVal parse_decimal_text(std::string_view s)
{
    bool neg = false;
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        ++i;
    }
    mp::int256_t u = 0;
    long long scale = 0;
    bool any = false;
    auto take_digits = [&](bool frac) {
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            u = u * 10 + (s[i] - '0');
            any = true;
            if (frac) {
                ++scale;
            }
            if (u >= kNumericUnscaledMax) {
                DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE,
                         "数值超出 numeric 可表示范围");
            }
        }
    };
    take_digits(false);
    if (i < s.size() && s[i] == '.') {
        ++i;
        take_digits(true);
    }
    if (!any) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "字符串转数值失败");
    }
    long long exp = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            eneg = s[i] == '-';
            ++i;
        }
        bool eany = false;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            exp = exp * 10 + (s[i] - '0');
            eany = true;
            if (exp > 1000) {
                DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE,
                         "数值超出 numeric 可表示范围");
            }
        }
        if (!eany) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "字符串转数值失败");
        }
        exp = eneg ? -exp : exp;
    }
    if (i != s.size()) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "字符串转数值失败");
    }
    scale -= exp;
    if (scale < 0) {
        for (long long k = 0; k < -scale; ++k) {
            u *= 10;
            if (u >= kNumericUnscaledMax) {
                DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE,
                         "数值超出 numeric 可表示范围");
            }
        }
        scale = 0;
    }
    if (scale > 255) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "数值超出 numeric 可表示范围");
    }
    return DecimalVal{neg ? -u : u, static_cast<uint8_t>(scale)};
}

// 定点十进制转文本: 按 scale 定点展开, 尾零保留
std::string decimal_to_text(const DecimalVal& d)
{
    const bool neg = d.unscaled < 0;
    std::string digits = mp::abs(d.unscaled).convert_to<std::string>();
    if (d.scale == 0) {
        return neg ? "-" + digits : digits;
    }
    if (digits.size() <= d.scale) {
        digits.insert(0, static_cast<size_t>(d.scale) - digits.size() + 1, '0');
    }
    std::string out;
    if (neg) {
        out += '-';
    }
    out.append(digits, 0, digits.size() - d.scale);
    out += '.';
    out.append(digits, digits.size() - static_cast<size_t>(d.scale),
               static_cast<size_t>(d.scale));
    return out;
}

// numeric 目标归一: 先经分派表转到定点十进制形态, 再四舍五入(远离零)到目标标度;
// 精度非 0 时校验结果十进制位数, 放大超界/超精度当场报错
Value coerce_numeric(const Value& v, TypeAttr to, uint16_t precision, uint8_t scale)
{
    const Value n = cast_value(v, to, CastMode::Explicit);
    const DecimalVal d = get_dec(n);
    mp::int256_t u = d.unscaled;
    if (d.scale > scale) {
        const mp::int256_t p = pow10(d.scale - scale);
        const mp::int256_t r = u % p;  // 与 u 同号
        u /= p;
        if (mp::abs(r) * 2 >= p) {
            u += u < 0 ? -1 : 1;  // 余数达半即远离零进位
        }
    } else {
        for (int i = d.scale; i < scale; ++i) {
            u *= 10;
            if (u != 0 && mp::abs(u) >= kNumericUnscaledMax) {
                DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE,
                         "数值超出 numeric 可表示范围");
            }
        }
    }
    if (precision > 0) {
        mp::int256_t mag = mp::abs(u);
        int digits = 0;
        while (mag > 0) {
            mag /= 10;
            ++digits;
        }
        if (digits > precision) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "numeric 值超出声明精度");
        }
    }
    return numeric_val(to, u, scale);
}

}  // namespace st
