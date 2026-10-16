// expr_value.h: 值取盒/转换/判空辅助(expr 目录内共享), 仅供本目录使用
#ifndef EXPR_EXPR_VALUE_H
#define EXPR_EXPR_VALUE_H

#include "storage/types.h"

namespace expr {

// 数值转 double: 经 cast 分派表提升到目标域(目标须为 double 形态), 非数值源域由表入口报错
double to_double(const st::Value& v, st::TypeAttr to);

// 数值转 float: 经 cast 分派表转到目标域(目标须为 float 形态, 整型单次舍入),
// 非数值源域由表入口报错
float to_float(const st::Value& v, st::TypeAttr to);

// 整型取值: 经 cast 分派表归一到目标整域(int/bigint 同盒)后取盒值
int64_t get_int(const st::Value& v, st::TypeAttr domain);

// 取 DecimalVal 盒值, 表示不符当场报错
const st::DecimalVal& get_dec(const st::Value& v);

// 取整型盒值(int/bigint 同盒), 表示不符当场报错
int64_t get_i64(const st::Value& v);

// 取 float 盒值, 表示不符当场报错
float get_f32(const st::Value& v);

// 取 double 盒值, 表示不符当场报错
double get_f64(const st::Value& v);

// 数值盒值提升 double: 经 cast 分派表转 double 域(double 属性取自内置类型种子数据),
// 供无目标域上下文的比较提升
double box_f64(const st::Value& v);

// 数值转 numeric 域: 经 cast 分派表转换后取盒值(目标须为定点十进制形态),
// 非数值源域由表入口报错
st::DecimalVal to_numeric(const st::Value& v, st::TypeAttr to);

// 值是否 NULL(仅看 box)
bool is_null(const st::Value& v);

}  // namespace expr

#endif  // EXPR_EXPR_VALUE_H
