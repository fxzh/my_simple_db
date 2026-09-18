// codec.h: 记录(行)的序列化/反序列化与列类型解析
#ifndef STORAGE_CODEC_H
#define STORAGE_CODEC_H

#include <string_view>
#include <vector>
#include "types.h"

namespace st {

// 行值转记录字节: [记录长度 uint16][NULL 位图][固定类型 inline][varchar: 长度 u16 + 字节]
// char: 定长 n 字节无前缀; 值类型与列类型不匹配/越界/NOT NULL 列为 NULL 返回 false
bool encode_row(const std::vector<ColumnSpec>& cols,
                                const std::vector<Value>& values,
                                std::vector<uint8_t>& out);

// 记录字节解出行值, 长度非法返回 false
bool decode_row(const std::vector<ColumnSpec>& cols,
                                const uint8_t* data,
                                size_t len,
                                std::vector<Value>& out);

// 解析类型名: int/bigint/float/double/char(n)/varchar(n), 未知类型返回 false
// char 缺省长度 1; varchar 缺省 0(动态); len 仅在 char/varchar 时输出
bool parse_column_type(std::string_view type_str, ColType* type, uint16_t* len);

}  // namespace st
#endif