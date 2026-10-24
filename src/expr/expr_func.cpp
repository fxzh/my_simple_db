// expr_func.cpp: 内建函数求值表与求值入口
#include "expr_func.h"

#include <iterator>

#include "common/err.h"
#include "log/log.h"

namespace expr {

namespace {

// 内建函数求值表: 下标即 db_func.func_id(从 1 起, 0 号槽占位), 每个重载各对应一个实现,
// 增改函数须同步 bootstrap.sql
constexpr FuncEval kFuncTable[] = {
    nullptr,
    eval_abs_int,            // 1: abs(int)
    eval_abs_bigint,         // 2: abs(bigint)
    eval_abs_double,         // 3: abs(double)
    eval_abs_float,          // 4: abs(float)
    eval_abs_numeric,        // 5: abs(numeric)
    eval_round_int,          // 6: round(int)
    eval_round_bigint,       // 7: round(bigint)
    eval_round_double,       // 8: round(double)
    eval_round_float,        // 9: round(float)
    eval_round_numeric,      // 10: round(numeric)
    eval_round_numeric_int,  // 11: round(numeric,int)
    eval_floor_int,          // 12: floor(int)
    eval_floor_bigint,       // 13: floor(bigint)
    eval_floor_double,       // 14: floor(double)
    eval_floor_float,        // 15: floor(float)
    eval_floor_numeric,      // 16: floor(numeric)
    eval_ceil_int,           // 17: ceil(int)
    eval_ceil_bigint,        // 18: ceil(bigint)
    eval_ceil_double,        // 19: ceil(double)
    eval_ceil_float,         // 20: ceil(float)
    eval_ceil_numeric,       // 21: ceil(numeric)
    eval_mod_int,            // 22: mod(int,int)
    eval_mod_bigint,         // 23: mod(bigint,bigint)
    eval_mod_double,         // 24: mod(double,double)
    eval_mod_float,          // 25: mod(float,float)
    eval_mod_numeric,        // 26: mod(numeric,numeric)
};

}  // namespace

st::Value eval_builtin(uint64_t func_id, const std::vector<st::Value>& args, st::TypeAttr ret)
{
    const size_t idx = static_cast<size_t>(func_id);
    if (idx >= std::size(kFuncTable) || kFuncTable[idx] == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXPR, "expr: 未知内建函数");
    }
    return kFuncTable[idx](args, ret);
}

}  // namespace expr
