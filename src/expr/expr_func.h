// expr_func.h: 内建函数实现声明与求值入口(expr 目录内共享), 仅供本目录使用
#ifndef EXPR_EXPR_FUNC_H
#define EXPR_EXPR_FUNC_H

#include <vector>

#include "storage/types.h"

namespace expr {

// 内建函数求值签名: args 已求值, 严格重载无 NULL 实参, 非严格重载实参可含 NULL 由实现
// 判空; ret 为绑定期固定的结果域属性
using FuncEval = st::Value (*)(const std::vector<st::Value>& args, st::TypeAttr ret);

// abs(int) 求值: 取绝对值, 下界溢出报错
st::Value eval_abs_int(const std::vector<st::Value>& args, st::TypeAttr ret);

// abs(bigint) 求值: 取绝对值, 下界溢出报错
st::Value eval_abs_bigint(const std::vector<st::Value>& args, st::TypeAttr ret);

// abs(double) 求值: 取绝对值
st::Value eval_abs_double(const std::vector<st::Value>& args, st::TypeAttr ret);

// abs(float) 求值: 取绝对值
st::Value eval_abs_float(const std::vector<st::Value>& args, st::TypeAttr ret);

// abs(numeric) 求值: unscaled 取绝对值, 标度不变
st::Value eval_abs_numeric(const std::vector<st::Value>& args, st::TypeAttr ret);

// 内建函数求值入口: 按绑定期选定的重载 id 查求值表执行, id 越界或槽位空报 Internal
st::Value eval_builtin(uint64_t func_id, const std::vector<st::Value>& args, st::TypeAttr ret);

}  // namespace expr

#endif  // EXPR_EXPR_FUNC_H
