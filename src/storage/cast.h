// cast.h: 值级隐式转换统一入口: 按 (源域, 目标域) 内置转换函数分派表执行
#ifndef STORAGE_CAST_H
#define STORAGE_CAST_H

#include "types.h"

namespace st {

// 统一 cast 执行入口: 按 (v.type, to) 查分派表执行内置转换并按目标域定型;
// NULL 与空槽(无该转换)当场报 Internal, NULL 传播由调用方先行判空;
// 用户可见报错由表项自带(如 Double→Float 收窄越界)
Value cast_value(const Value& v, ColType to);

}  // namespace st

#endif  // STORAGE_CAST_H
