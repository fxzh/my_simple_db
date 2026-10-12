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

namespace {

// 转换函数: 入参非 NULL, 源域即 v.type(由入口按表分派)
using CastFn = Value (*)(const Value& v);

// 取 int64 盒值, 形态不符当场报错
int64_t get_i64(const Value& v)
{
    const int64_t* i = std::get_if<int64_t>(&v.box);
    if (i == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值形态与源域不符");
    }
    return *i;
}

// 取 float 盒值, 形态不符当场报错
float get_f32(const Value& v)
{
    const float* f = std::get_if<float>(&v.box);
    if (f == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值形态与源域不符");
    }
    return *f;
}

// 取 double 盒值, 形态不符当场报错
double get_f64(const Value& v)
{
    const double* d = std::get_if<double>(&v.box);
    if (d == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值形态与源域不符");
    }
    return *d;
}

// 取 string 盒值, 形态不符当场报错
std::string get_str(const Value& v)
{
    const std::string* s = std::get_if<std::string>(&v.box);
    if (s == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值形态与源域不符");
    }
    return *s;
}

// 取 string 盒值只读视图, 形态不符当场报错
std::string_view get_str_view(const Value& v)
{
    const std::string* s = std::get_if<std::string>(&v.box);
    if (s == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值形态与源域不符");
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

// 取 bool 盒值, 形态不符当场报错
bool get_bool(const Value& v)
{
    const bool* b = std::get_if<bool>(&v.box);
    if (b == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 值形态与源域不符");
    }
    return *b;
}

// 恒等转换
Value cast_id(const Value& v)
{
    return v;
}

// int64 同盒换域: Int→BigInt
Value int_to_bigint(const Value& v)
{
    return bigint_val(get_i64(v));
}

// int64 同盒换域: BigInt→Int, int32 域界由存储编码检查
Value bigint_to_int(const Value& v)
{
    return int_val(get_i64(v));
}

// int64→float: 单次舍入, 不经 double 中转
Value int_to_float(const Value& v)
{
    return float_val(static_cast<float>(get_i64(v)));
}

// int64→double
Value int_to_double(const Value& v)
{
    return double_val(static_cast<double>(get_i64(v)));
}

// float→double: 宽化无损
Value float_to_double(const Value& v)
{
    return double_val(static_cast<double>(get_f32(v)));
}

// double→float: 收窄, 有限值不可表示当场报错
Value double_to_float(const Value& v)
{
    const double d = get_f64(v);
    const float f = static_cast<float>(d);
    if (std::isfinite(d) && !std::isfinite(f)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "浮点值超出 float 可表示范围");
    }
    return float_val(f);
}

// string 同盒换域: VarChar→Char, 长度由语义层校验
Value varchar_to_char(const Value& v)
{
    return char_val(get_str(v));
}

// string 同盒换域: Char→VarChar, 去尾随 pad
Value char_to_varchar(const Value& v)
{
    return str_val(std::string(get_char_view(v)));
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

// double 向零截断为 int64: 非有限或超 int64 域当场报错
int64_t trunc_to_i64(double d)
{
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "浮点值超出整型可表示范围");
    }
    return static_cast<int64_t>(d);
}

// int64(int/BigInt 源共用盒形态)转文本: 最短十进制
Value int_to_varchar(const Value& v)
{
    return str_val(std::format("{}", get_i64(v)));
}
Value int_to_char(const Value& v)
{
    return char_val(std::format("{}", get_i64(v)));
}

// float 转文本: 最短往返表示
Value float_to_varchar(const Value& v)
{
    return str_val(std::format("{}", get_f32(v)));
}
Value float_to_char(const Value& v)
{
    return char_val(std::format("{}", get_f32(v)));
}

// double 转文本: 最短往返表示
Value double_to_varchar(const Value& v)
{
    return str_val(std::format("{}", get_f64(v)));
}
Value double_to_char(const Value& v)
{
    return char_val(std::format("{}", get_f64(v)));
}

// bool 转文本
Value bool_to_varchar(const Value& v)
{
    return str_val(get_bool(v) ? "true" : "false");
}
Value bool_to_char(const Value& v)
{
    return char_val(get_bool(v) ? "true" : "false");
}

// bool 转数值
Value bool_to_int(const Value& v)
{
    return int_val(get_bool(v) ? 1 : 0);
}
Value bool_to_bigint(const Value& v)
{
    return bigint_val(get_bool(v) ? 1 : 0);
}
Value bool_to_float(const Value& v)
{
    return float_val(get_bool(v) ? 1.0f : 0.0f);
}
Value bool_to_double(const Value& v)
{
    return double_val(get_bool(v) ? 1.0 : 0.0);
}

// 数值转 Bool: 仅精确 0/1
Value int_to_bool(const Value& v)
{
    const int64_t i = get_i64(v);
    if (i != 0 && i != 1) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "数值转布尔仅接受 0 或 1");
    }
    return bool_val(i != 0);
}
Value float_to_bool(const Value& v)
{
    const float f = get_f32(v);
    if (f != 0.0f && f != 1.0f) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "数值转布尔仅接受 0 或 1");
    }
    return bool_val(f != 0.0f);
}
Value double_to_bool(const Value& v)
{
    const double d = get_f64(v);
    if (d != 0.0 && d != 1.0) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "数值转布尔仅接受 0 或 1");
    }
    return bool_val(d != 0.0);
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
Value text_to_bool(const Value& v)
{
    return bool_val(parse_bool(get_str_view(v)));
}

// 文本转数值: VarChar 源, int32 域界由存储编码检查
Value text_to_int(const Value& v)
{
    return int_val(parse_i64(get_str_view(v)));
}
Value text_to_bigint(const Value& v)
{
    return bigint_val(parse_i64(get_str_view(v)));
}
Value text_to_double(const Value& v)
{
    return double_val(parse_f64(get_str_view(v)));
}
Value text_to_float(const Value& v)
{
    // 先解析为 double 再收窄, 不可表示由收窄报错
    return double_to_float(double_val(parse_f64(get_str_view(v))));
}

// Char 源文本转换: 去 pad 后按 VarChar 源同样规则解析
Value char_to_int(const Value& v)
{
    return int_val(parse_i64(get_char_view(v)));
}
Value char_to_bigint(const Value& v)
{
    return bigint_val(parse_i64(get_char_view(v)));
}
Value char_to_double(const Value& v)
{
    return double_val(parse_f64(get_char_view(v)));
}
Value char_to_float(const Value& v)
{
    // 先解析为 double 再收窄, 不可表示由收窄报错
    return double_to_float(double_val(parse_f64(get_char_view(v))));
}
Value char_to_bool(const Value& v)
{
    return bool_val(parse_bool(get_char_view(v)));
}

// 浮点向零截断为整型: Float 经 double 宽化无损中转
Value double_to_int(const Value& v)
{
    return int_val(trunc_to_i64(get_f64(v)));
}
Value double_to_bigint(const Value& v)
{
    return bigint_val(trunc_to_i64(get_f64(v)));
}
Value float_to_int(const Value& v)
{
    return int_val(trunc_to_i64(static_cast<double>(get_f32(v))));
}
Value float_to_bigint(const Value& v)
{
    return bigint_val(trunc_to_i64(static_cast<double>(get_f32(v))));
}

// 转换分派表项: fn 为空表示无转换, implicit 为真时隐式模式可用
struct CastEntry {
    CastFn fn = nullptr;
    bool implicit = false;
};

// 内置转换分派表: 行源域列目标域, 下标即 ColType 枚举值
// (Null, Int, BigInt, Double, VarChar, Float, Char, Bool); 非 Null 槽位全填,
// 空槽与隐式查到仅显式表项均为调用方违约
constexpr CastEntry kCastTable[8][8] = {
    /* Null */    {{}, {}, {}, {}, {}, {}, {}, {}},
    /* Int */     {{}, {cast_id, true}, {int_to_bigint, true}, {int_to_double, true},
                   {int_to_varchar}, {int_to_float, true}, {int_to_char}, {int_to_bool}},
    /* BigInt */  {{}, {bigint_to_int, true}, {cast_id, true}, {int_to_double, true},
                   {int_to_varchar}, {int_to_float, true}, {int_to_char}, {int_to_bool}},
    /* Double */  {{}, {double_to_int}, {double_to_bigint}, {cast_id, true},
                   {double_to_varchar}, {double_to_float, true}, {double_to_char},
                   {double_to_bool}},
    /* VarChar */ {{}, {text_to_int}, {text_to_bigint}, {text_to_double},
                   {cast_id, true}, {text_to_float}, {varchar_to_char, true}, {text_to_bool}},
    /* Float */   {{}, {float_to_int}, {float_to_bigint}, {float_to_double, true},
                   {float_to_varchar}, {cast_id, true}, {float_to_char}, {float_to_bool}},
    /* Char */    {{}, {char_to_int}, {char_to_bigint}, {char_to_double},
                   {char_to_varchar, true}, {char_to_float}, {cast_id, true}, {char_to_bool}},
    /* Bool */    {{}, {bool_to_int}, {bool_to_bigint}, {bool_to_double},
                   {bool_to_varchar}, {bool_to_float}, {bool_to_char}, {cast_id, true}},
};

}  // namespace

Value cast_value(const Value& v, ColType to, CastMode mode)
{
    if (value_is_null(v)) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 不接受 NULL");
    }
    const CastEntry& e = kCastTable[static_cast<size_t>(v.type)][static_cast<size_t>(to)];
    if (e.fn == nullptr || (mode == CastMode::Implicit && !e.implicit)) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 无 {}→{} 转换",
                 static_cast<int>(v.type), static_cast<int>(to));
    }
    return e.fn(v);
}

}  // namespace st
