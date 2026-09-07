// codec.h: 记录(行)的序列化/反序列化与列类型解析
#ifndef STORAGE_CODEC_H
#define STORAGE_CODEC_H

#include <string_view>
#include <vector>
#include "types.h"

namespace st {

// 行值转记录字节: [记录长度 uint16][固定类型 inline][varchar: 长度 uint16 + 字节]
// 值类型与列类型不匹配或 Int 越界时返回 false
bool encode_row(const std::vector<ColumnSpec>& cols,
                                const std::vector<Value>& values,
                                std::vector<uint8_t>& out);

// 记录字节解出行值, 长度非法返回 false
bool decode_row(const std::vector<ColumnSpec>& cols,
                                const uint8_t* data,
                                size_t len,
                                std::vector<Value>& out);

// 解析类型名: int/bigint/double/varchar(n), 未知类型返回 false
// varchar_len 仅在 varchar 时输出
bool parse_column_type(std::string_view type_str, ColType* type, uint16_t* varchar_len);

}  // namespace st
#endif