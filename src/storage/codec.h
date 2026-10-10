// codec.h: 记录(行)的序列化/反序列化
#ifndef STORAGE_CODEC_H
#define STORAGE_CODEC_H

#include <vector>
#include "types.h"

namespace st {

// 行值转记录字节: [记录长度 uint16][NULL 位图][固定类型 inline][varchar: 长度 u16 + 字节]
// char: 定长 n 字节无前缀; 按列规格驱动 + box 存储形态校验, 值的语义类型可与列类型不同
// (家族宽化合法: Int 值入 BigInt 列/Double 值入 Float 列), 形态不匹配/越界/NOT NULL 列为
// NULL 返回 false
bool encode_row(const std::vector<ColumnSpec>& cols,
                                const std::vector<Value>& values,
                                std::vector<uint8_t>& out);

// 记录字节解出行值, 长度非法返回 false; 值携带列类型(NULL 为携带列类型的无值)
bool decode_row(const std::vector<ColumnSpec>& cols,
                                const uint8_t* data,
                                size_t len,
                                std::vector<Value>& out);

}  // namespace st
#endif