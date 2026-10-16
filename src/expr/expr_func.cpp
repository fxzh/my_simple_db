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
    eval_abs_int,      // 1: abs(int)
    eval_abs_bigint,   // 2: abs(bigint)
    eval_abs_double,   // 3: abs(double)
    eval_abs_float,    // 4: abs(float)
    eval_abs_numeric,  // 5: abs(numeric)
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
