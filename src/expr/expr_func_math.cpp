// expr_func_math.cpp: 数值类内建函数实现
#include "expr_func.h"
#include "expr_value.h"

#include <cmath>
#include <cstdint>

#include "common/err.h"
#include "log/log.h"

namespace expr {

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

}  // namespace expr
