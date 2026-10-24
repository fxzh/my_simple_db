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

// round(int) 求值: 整型无小数位, 原值返回
st::Value eval_round_int(const std::vector<st::Value>& args, st::TypeAttr ret);

// round(bigint) 求值: 整型无小数位, 原值返回
st::Value eval_round_bigint(const std::vector<st::Value>& args, st::TypeAttr ret);

// round(double) 求值: 四舍五入到整数(半值远离零)
st::Value eval_round_double(const std::vector<st::Value>& args, st::TypeAttr ret);

// round(float) 求值: 四舍五入到整数(半值远离零)
st::Value eval_round_float(const std::vector<st::Value>& args, st::TypeAttr ret);

// round(numeric) 求值: 四舍五入到整数(半值远离零), 结果标度 0
st::Value eval_round_numeric(const std::vector<st::Value>& args, st::TypeAttr ret);

// round(numeric,int) 求值: 四舍五入到 n 位小数(半值远离零), n 可为负,
// 结果标度 n(负 n 时 0), n 不小于原标度时原值返回
st::Value eval_round_numeric_int(const std::vector<st::Value>& args, st::TypeAttr ret);

// floor(int) 求值: 整型无小数位, 原值返回
st::Value eval_floor_int(const std::vector<st::Value>& args, st::TypeAttr ret);

// floor(bigint) 求值: 整型无小数位, 原值返回
st::Value eval_floor_bigint(const std::vector<st::Value>& args, st::TypeAttr ret);

// floor(double) 求值: 向下取整
st::Value eval_floor_double(const std::vector<st::Value>& args, st::TypeAttr ret);

// floor(float) 求值: 向下取整
st::Value eval_floor_float(const std::vector<st::Value>& args, st::TypeAttr ret);

// floor(numeric) 求值: 向下取整, 结果标度 0
st::Value eval_floor_numeric(const std::vector<st::Value>& args, st::TypeAttr ret);

// ceil(int) 求值: 整型无小数位, 原值返回
st::Value eval_ceil_int(const std::vector<st::Value>& args, st::TypeAttr ret);

// ceil(bigint) 求值: 整型无小数位, 原值返回
st::Value eval_ceil_bigint(const std::vector<st::Value>& args, st::TypeAttr ret);

// ceil(double) 求值: 向上取整
st::Value eval_ceil_double(const std::vector<st::Value>& args, st::TypeAttr ret);

// ceil(float) 求值: 向上取整
st::Value eval_ceil_float(const std::vector<st::Value>& args, st::TypeAttr ret);

// ceil(numeric) 求值: 向上取整, 结果标度 0
st::Value eval_ceil_numeric(const std::vector<st::Value>& args, st::TypeAttr ret);

// mod(int,int) 求值: 取余, 符号随被除数, 除零报错
st::Value eval_mod_int(const std::vector<st::Value>& args, st::TypeAttr ret);

// mod(bigint,bigint) 求值: 取余, 符号随被除数, 除零报错
st::Value eval_mod_bigint(const std::vector<st::Value>& args, st::TypeAttr ret);

// mod(double,double) 求值: 取余, 符号随被除数, 除零报错
st::Value eval_mod_double(const std::vector<st::Value>& args, st::TypeAttr ret);

// mod(float,float) 求值: 取余, 符号随被除数, 除零报错
st::Value eval_mod_float(const std::vector<st::Value>& args, st::TypeAttr ret);

// mod(numeric,numeric) 求值: 取余, 符号随被除数, 结果标度取两操作数标度较大者, 除零报错
st::Value eval_mod_numeric(const std::vector<st::Value>& args, st::TypeAttr ret);

// 内建函数求值入口: 按绑定期选定的重载 id 查求值表执行, id 越界或槽位空报 Internal
st::Value eval_builtin(uint64_t func_id, const std::vector<st::Value>& args, st::TypeAttr ret);

}  // namespace expr

#endif  // EXPR_EXPR_FUNC_H
