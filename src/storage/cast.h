// cast.h: 值级转换统一入口: 按 (源域, 目标域, 模式) 内置转换函数分派表执行
#ifndef STORAGE_CAST_H
#define STORAGE_CAST_H

#include <string>
#include <string_view>

#include "types.h"

namespace st {

// 转换模式: 隐式仅同族提升/收窄, 显式(CAST)含跨族转换
enum class CastMode : uint8_t { Implicit, Explicit };

// 统一 cast 执行入口: 按 (v.type, to, mode) 查分派表执行内置转换并按目标域定型;
// NULL 与空槽(无该转换)当场报 Internal, NULL 传播由调用方先行判空;
// 隐式模式查到仅显式表项视同空槽; 用户可见报错由表项自带(如字符串解析失败)
Value cast_value(const Value& v, ColType to, CastMode mode = CastMode::Implicit);

// 文本解析为定点十进制: [+/-]数字[.数字][eE[+/-]数字], 全串参与, 尾零保留;
// unscaled 幅值达 10^38、scale 超 255 或指数超 1000 当场报错
DecimalVal parse_decimal_text(std::string_view s);

// 定点十进制转文本: 按 scale 定点展开, 尾零保留
std::string decimal_to_text(const DecimalVal& d);

// numeric 目标归一: 先经分派表转 Numeric 域, 再四舍五入(远离零)到目标标度;
// precision 非 0 时校验结果十进制位数, 放大超界/超精度当场报错
Value coerce_numeric(const Value& v, uint16_t precision, uint8_t scale);

}  // namespace st

#endif  // STORAGE_CAST_H
