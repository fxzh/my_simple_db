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
// db_type 列定义
std::vector<st::ColumnSpec> type_meta_cols();
// db_type 种子行: 由内置类型种子数据生成, 无别名的行 alias 为 NULL
std::vector<std::vector<st::Value>> type_meta_rows();
// db_type 行解出 TypeRow, 值缺失/非法或属性值不在枚举取值集合内当场报错
TypeRow parse_type_row(const std::vector<st::Value>& row);
// db_func 行解出 FuncRow, 实参与结果类型经 db_type 行解析, 值缺失或非法当场报错
FuncRow parse_func_row(const std::vector<st::Value>& row, const std::vector<TypeRow>& types);
// 校验 db_func 全量行: func_id 唯一, 同名同实参类型序列唯一, 违约报元数据行损坏
void validate_func_rows(st::Engine& engine, const std::vector<st::ColumnSpec>& cols);
// 生成某表的 db_column 自描述行(ordinal 从 0 起, type 存 db_type.type_id, not_null 存 0/1,
// length/scale 存声明长度/标度)
std::vector<std::vector<st::Value>> column_meta_rows(uint64_t tid,
                                                     const std::vector<st::ColumnSpec>& cols);

// ==================== 元数据行解码 ====================

// 行内取 int 字段, NULL 或类型不符报元数据行损坏
int64_t row_int(const std::vector<st::Value>& row, size_t idx);
// 行内取字符串字段, NULL 或类型不符报元数据行损坏
const std::string& row_str(const std::vector<st::Value>& row, size_t idx);
// 收集指定表的列定义(须持锁): 扫 db_column 匹配 tid 并按 ordinal 排序,
// 列类型经 db_type 行取属性, 无列定义或序号重复报错
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
