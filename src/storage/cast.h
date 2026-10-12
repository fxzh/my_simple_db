// cast.h: 值级转换统一入口: 按 (源域, 目标域, 模式) 内置转换函数分派表执行
#ifndef STORAGE_CAST_H
#define STORAGE_CAST_H

#include "types.h"

namespace st {

// 转换模式: 隐式仅同族提升/收窄, 显式(CAST)含跨族转换
enum class CastMode : uint8_t { Implicit, Explicit };

// 统一 cast 执行入口: 按 (v.type, to, mode) 查分派表执行内置转换并按目标域定型;
// NULL 与空槽(无该转换)当场报 Internal, NULL 传播由调用方先行判空;
// 隐式模式查到仅显式表项视同空槽; 用户可见报错由表项自带(如字符串解析失败)
Value cast_value(const Value& v, ColType to, CastMode mode = CastMode::Implicit);

}  // namespace st

#endif  // STORAGE_CAST_H
