// cast.cpp: 值级隐式转换分派表实现
#include "cast.h"

#include <cmath>
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

// string 同盒换域: Char→VarChar
Value char_to_varchar(const Value& v)
{
    return str_val(get_str(v));
}

// 内置转换分派表: 行源域列目标域, 下标即 ColType 枚举值
// (Null, Int, BigInt, Double, VarChar, Float, Char, Bool); 空槽无转换, 查到即 Internal
constexpr CastFn kCastTable[8][8] = {
    /* Null */    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    /* Int */     {nullptr, cast_id, int_to_bigint, int_to_double, nullptr, int_to_float,
                   nullptr, nullptr},
    /* BigInt */  {nullptr, bigint_to_int, cast_id, int_to_double, nullptr, int_to_float,
                   nullptr, nullptr},
    /* Double */  {nullptr, nullptr, nullptr, cast_id, nullptr, double_to_float, nullptr,
                   nullptr},
    /* VarChar */ {nullptr, nullptr, nullptr, nullptr, cast_id, nullptr, varchar_to_char,
                   nullptr},
    /* Float */   {nullptr, nullptr, nullptr, float_to_double, nullptr, cast_id, nullptr,
                   nullptr},
    /* Char */    {nullptr, nullptr, nullptr, nullptr, char_to_varchar, nullptr, cast_id,
                   nullptr},
    /* Bool */    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, cast_id},
};

}  // namespace

Value cast_value(const Value& v, ColType to)
{
    if (value_is_null(v)) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 不接受 NULL");
    }
    const CastFn fn = kCastTable[static_cast<size_t>(v.type)][static_cast<size_t>(to)];
    if (fn == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "cast: 无 {}→{} 转换",
                 static_cast<int>(v.type), static_cast<int>(to));
    }
    return fn(v);
}

}  // namespace st
