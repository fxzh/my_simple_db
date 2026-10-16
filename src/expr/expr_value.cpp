// expr_value.cpp: 值取盒/转换/判空辅助实现
#include "expr_value.h"

#include <variant>

#include "common/err.h"
#include "log/log.h"
#include "storage/cast.h"

namespace expr {

double to_double(const st::Value& v, st::TypeAttr to)
{
    return std::get<double>(st::cast_value(v, to).box);
}

float to_float(const st::Value& v, st::TypeAttr to)
{
    return std::get<float>(st::cast_value(v, to).box);
}

int64_t get_int(const st::Value& v, st::TypeAttr domain)
{
    return std::get<int64_t>(st::cast_value(v, domain).box);
}

const st::DecimalVal& get_dec(const st::Value& v)
{
    const st::DecimalVal* d = std::get_if<st::DecimalVal>(&v.box);
    if (d == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 值表示与数值域不符");
    }
    return *d;
}

int64_t get_i64(const st::Value& v)
{
    const int64_t* i = std::get_if<int64_t>(&v.box);
    if (i == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 值表示与数值域不符");
    }
    return *i;
}

float get_f32(const st::Value& v)
{
    const float* f = std::get_if<float>(&v.box);
    if (f == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 值表示与数值域不符");
    }
    return *f;
}

double get_f64(const st::Value& v)
{
    const double* d = std::get_if<double>(&v.box);
    if (d == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 值表示与数值域不符");
    }
    return *d;
}

double box_f64(const st::Value& v)
{
    return std::get<double>(st::cast_value(v, st::kDouble).box);
}

st::DecimalVal to_numeric(const st::Value& v, st::TypeAttr to)
{
    return std::get<st::DecimalVal>(st::cast_value(v, to).box);
}

bool is_null(const st::Value& v)
{
    return std::holds_alternative<std::monostate>(v.box);
}

}  // namespace expr
