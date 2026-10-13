// catalog_meta.h: 目录层内部共享声明(元数据表列定义/行解码/索引条目查找), 仅供本目录使用
#ifndef CATALOG_CATALOG_META_H
#define CATALOG_CATALOG_META_H

#include <cstdint>
#include <string>
#include <vector>

#include "catalog.h"

namespace ct {

// ==================== 元数据表列定义与行编解码 ====================

// 元数据表名/列名的 varchar 声明长度
constexpr uint16_t kMetaNameLen = 64;

// db_table 列定义
std::vector<st::ColumnSpec> table_meta_cols();
// db_column 列定义
std::vector<st::ColumnSpec> column_meta_cols();
// db_schema 列定义
std::vector<st::ColumnSpec> schema_meta_cols();
// 生成某表的 db_column 自描述行(ordinal 从 0 起, type/not_null 存枚举值与 0/1,
// length/scale 存声明长度/标度)
std::vector<std::vector<st::Value>> column_meta_rows(uint64_t tid,
                                                     const std::vector<st::ColumnSpec>& cols);

// ==================== 元数据行解码 ====================

// 行内取 int 字段, NULL 或类型不符报元数据行损坏
int64_t row_int(const std::vector<st::Value>& row, size_t idx);
// 行内取字符串字段, NULL 或类型不符报元数据行损坏
const std::string& row_str(const std::vector<st::Value>& row, size_t idx);
// 收集指定表的列定义(须持锁): 扫 db_column 匹配 tid 并按 ordinal 排序, 无列定义或序号重复报错
std::vector<st::ColumnSpec> collect_columns(st::Engine& engine, uint64_t tid,
                                            const std::string& name);

// ==================== 索引条目查找 ====================

// 该表全部索引(须持锁): 扫 db_index 匹配 table_id, 行值非法当场报错; cols 为 db_index 列定义
std::vector<IndexEntry> table_indexes(st::Engine& engine, const std::vector<st::ColumnSpec>& cols,
                                      uint64_t tid);
// 表内索引名是否已存在(须持锁): 全扫 db_index 匹配 table_id 与索引名
bool has_index_name(st::Engine& engine, const std::vector<st::ColumnSpec>& cols, uint64_t tid,
                    const std::string& name);
// 表内按名查索引(须持锁): 命中返回 true 并带出条目
bool find_index(st::Engine& engine, const std::vector<st::ColumnSpec>& cols, uint64_t tid,
                const std::string& name, IndexEntry& out);

}  // namespace ct

#endif  // CATALOG_CATALOG_META_H
