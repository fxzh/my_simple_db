// catalog_meta.cpp: 元数据行编解码与查找: 元数据表列定义/行解码, 按名查表与 schema, has_* 查重
#include "catalog_meta.h"

#include <algorithm>
#include <utility>

#include "common/err.h"
#include "log/log.h"

namespace ct {

// ==================== 元数据表列定义与行编解码 ====================

// db_table 列定义
std::vector<st::ColumnSpec> table_meta_cols()
{
    return {{"table_id", st::ColType::BigInt, 0, true},
            {"table_name", st::ColType::VarChar, kMetaNameLen, true},
            {"file_id", st::ColType::BigInt, 0, true},
            {"schema_id", st::ColType::BigInt, 0, true}};
}

// db_column 列定义
std::vector<st::ColumnSpec> column_meta_cols()
{
    return {{"table_id", st::ColType::BigInt, 0, true},
            {"col_name", st::ColType::VarChar, kMetaNameLen, true},
            {"ordinal", st::ColType::Int, 0, true},
            {"type", st::ColType::Int, 0, true},
            {"length", st::ColType::Int, 0, true},
            {"not_null", st::ColType::Int, 0, true}};
}

// db_schema 列定义
std::vector<st::ColumnSpec> schema_meta_cols()
{
    return {{"schema_id", st::ColType::BigInt, 0, true},
            {"schema_name", st::ColType::VarChar, kMetaNameLen, true}};
}

// 生成某表的 db_column 自描述行(ordinal 从 0 起, type/not_null 存枚举值与 0/1)
std::vector<std::vector<st::Value>> column_meta_rows(uint64_t tid,
                                                     const std::vector<st::ColumnSpec>& cols)
{
    std::vector<std::vector<st::Value>> rows;
    rows.reserve(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) {
        rows.push_back({st::bigint_val(static_cast<int64_t>(tid)), st::str_val(cols[i].name),
                        st::int_val(static_cast<int64_t>(i)),
                        st::int_val(static_cast<int64_t>(cols[i].type)),
                        st::int_val(static_cast<int64_t>(cols[i].length)),
                        st::int_val(static_cast<int64_t>(cols[i].not_null ? 1 : 0))});
    }
    return rows;
}

// ==================== 元数据行解码 ====================

// 行内取 int 字段, NULL 或类型不符报元数据行损坏
int64_t row_int(const std::vector<st::Value>& row, size_t idx)
{
    const int64_t* v = std::get_if<int64_t>(&row[idx].box);
    if (v == nullptr) {
        DB_CRITICAL(LogModule::CATALOG, "元数据行损坏");
    }
    return *v;
}

// 行内取字符串字段, NULL 或类型不符报元数据行损坏
const std::string& row_str(const std::vector<st::Value>& row, size_t idx)
{
    const std::string* v = std::get_if<std::string>(&row[idx].box);
    if (v == nullptr) {
        DB_CRITICAL(LogModule::CATALOG, "元数据行损坏");
    }
    return *v;
}

namespace {

// 列类型值是否在已知集合内; 穷尽 switch, 新增类型未同步时 -Wswitch 报警
bool col_type_known(st::ColType type)
{
    switch (type) {
    case st::ColType::Int:
    case st::ColType::BigInt:
    case st::ColType::Double:
    case st::ColType::VarChar:
    case st::ColType::Float:
    case st::ColType::Char:
    case st::ColType::Bool:
        return true;
    case st::ColType::Null:
        return false;
    }
    return false;
}

// db_column 行解出 (ordinal, 列定义), 值缺失或非法当场报错
std::pair<int64_t, st::ColumnSpec> parse_column_row(const std::vector<st::Value>& row)
{
    const int64_t type = row_int(row, 3);
    const int64_t length = row_int(row, 4);
    const int64_t not_null = row_int(row, 5);
    if (!col_type_known(static_cast<st::ColType>(type))) {
        DB_CRITICAL(LogModule::CATALOG, "列类型值非法: {}", type);
    }
    if (length < 0 || length > UINT16_MAX) {
        DB_CRITICAL(LogModule::CATALOG, "列长度值非法: {}", length);
    }
    if (not_null != 0 && not_null != 1) {
        DB_CRITICAL(LogModule::CATALOG, "非空标记值非法: {}", not_null);
    }
    return {row_int(row, 2), st::ColumnSpec{row_str(row, 1), static_cast<st::ColType>(type),
                                            static_cast<uint16_t>(length), not_null == 1}};
}

}  // namespace

// 收集指定表的列定义(须持锁): 扫 db_column 匹配 tid 并按 ordinal 排序, 无列定义或序号重复报错
std::vector<st::ColumnSpec> collect_columns(st::Engine& engine, uint64_t tid,
                                            const std::string& name)
{
    std::vector<std::pair<int64_t, st::ColumnSpec>> pairs;
    for (const std::vector<st::Value>& row : engine.read_rows(kColumnMetaId, column_meta_cols())) {
        if (row_int(row, 0) == static_cast<int64_t>(tid)) {
            pairs.push_back(parse_column_row(row));
        }
    }
    if (pairs.empty()) {
        DB_CRITICAL(LogModule::CATALOG, "表无列定义: {}", name);
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 1; i < pairs.size(); ++i) {
        if (pairs[i].first == pairs[i - 1].first) {
            DB_CRITICAL(LogModule::CATALOG, "列序号重复: {}", name);
        }
    }
    std::vector<st::ColumnSpec> cols;
    cols.reserve(pairs.size());
    for (std::pair<int64_t, st::ColumnSpec>& p : pairs) {
        cols.push_back(std::move(p.second));
    }
    return cols;
}

// ==================== 索引条目查找 ====================

// 该表全部索引(须持锁): 扫 db_index 匹配 table_id, 行值非法当场报错; cols 为 db_index 列定义
std::vector<IndexEntry> table_indexes(st::Engine& engine, const std::vector<st::ColumnSpec>& cols,
                                      uint64_t tid)
{
    st::TableMeta meta;
    meta.table_id = kIndexMetaId;
    meta.file_id = kIndexMetaId;
    meta.cols = cols;
    std::vector<IndexEntry> out;
    st::Scanner scanner(&engine, meta);
    st::Row row;
    while (scanner.next(&row)) {
        if (row_int(row.values, 0) != static_cast<int64_t>(tid)) {
            continue;
        }
        const int64_t ordinal = row_int(row.values, 2);
        const int64_t fid = row_int(row.values, 3);
        if (ordinal < 0 || ordinal > UINT16_MAX) {
            DB_CRITICAL(LogModule::CATALOG, "索引列序号非法: {}", ordinal);
        }
        if (fid < 0) {
            DB_CRITICAL(LogModule::CATALOG, "索引文件 id 非法: {}", fid);
        }
        IndexEntry e;
        e.name = row_str(row.values, 1);
        e.col_ordinal = static_cast<uint16_t>(ordinal);
        e.file_id = static_cast<uint64_t>(fid);
        e.row_ref = row.ref;
        out.push_back(std::move(e));
    }
    return out;
}

// 表内索引名是否已存在(须持锁): 全扫 db_index 匹配 table_id 与索引名
bool has_index_name(st::Engine& engine, const std::vector<st::ColumnSpec>& cols, uint64_t tid,
                    const std::string& name)
{
    for (const std::vector<st::Value>& row : engine.read_rows(kIndexMetaId, cols)) {
        if (row_int(row, 0) == static_cast<int64_t>(tid) && row_str(row, 1) == name) {
            return true;
        }
    }
    return false;
}

// 表内按名查索引(须持锁): 命中返回 true 并带出条目
bool find_index(st::Engine& engine, const std::vector<st::ColumnSpec>& cols, uint64_t tid,
                const std::string& name, IndexEntry& out)
{
    for (const IndexEntry& e : table_indexes(engine, cols, tid)) {
        if (e.name == name) {
            out = e;
            return true;
        }
    }
    return false;
}

// ==================== 按名查找与查重 ====================

st::TableMeta Catalog::table_meta(const TableRef& table)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return find_table_meta(table);
}

// 按限定名解析 schema_id(须持锁): schema 不存在当场报错, 空 schema 为调用方构造错误
uint64_t Catalog::resolve_schema_id(const TableRef& table)
{
    if (table.schema.empty()) {
        DB_RAISE(db::ErrCode::Internal, LogModule::CATALOG, "TableRef schema 为空: {}",
                 table.name);
    }
    for (const std::vector<st::Value>& row : engine_.read_rows(kSchemaMetaId, schema_meta_cols())) {
        if (row_str(row, 1) == table.schema) {
            return static_cast<uint64_t>(row_int(row, 0));
        }
    }
    DB_RAISE(db::ErrCode::SchemaNotFound, LogModule::CATALOG, "schema 不存在: {}", table.schema);
}

// 按限定名查元数据(须持锁): 解析 schema_id 后 db_table 按 (schema_id, 表名) 定位行,
// db_column 收集列并按 ordinal 排序
st::TableMeta Catalog::find_table_meta(const TableRef& table)
{
    const uint64_t sid = resolve_schema_id(table);
    uint64_t tid = 0;
    uint64_t fid = 0;
    bool found = false;
    for (const std::vector<st::Value>& row : engine_.read_rows(kTableMetaId, table_meta_cols())) {
        if (row_int(row, 3) == static_cast<int64_t>(sid) && row_str(row, 1) == table.name) {
            tid = static_cast<uint64_t>(row_int(row, 0));
            fid = static_cast<uint64_t>(row_int(row, 2));
            found = true;
            break;
        }
    }
    if (!found) {
        DB_RAISE(db::ErrCode::TableNotFound, LogModule::CATALOG, "表不存在: {}",
                 table_ref_to_string(table));
    }

    st::TableMeta meta;
    meta.table_id = tid;
    meta.file_id = fid;
    meta.name = table.name;
    meta.cols = collect_columns(engine_, tid, table.name);
    return meta;
}

// schema 内表名是否已存在(须持锁): 扫 db_table 匹配 schema_id 与表名
bool Catalog::has_table_name(uint64_t sid, const std::string& name)
{
    for (const std::vector<st::Value>& row : engine_.read_rows(kTableMetaId, table_meta_cols())) {
        if (row_int(row, 3) == static_cast<int64_t>(sid) && row_str(row, 1) == name) {
            return true;
        }
    }
    return false;
}

// table_id 是否已被占用(须持锁): 全扫 db_table 匹配, bootstrap 显式 id 建表查重
bool Catalog::has_table_id(uint64_t tid)
{
    for (const std::vector<st::Value>& row : engine_.read_rows(kTableMetaId, table_meta_cols())) {
        if (row_int(row, 0) == static_cast<int64_t>(tid)) {
            return true;
        }
    }
    return false;
}

// schema 名是否已存在(须持锁): 全扫 db_schema 匹配
bool Catalog::has_schema_name(const std::string& name)
{
    for (const std::vector<st::Value>& row : engine_.read_rows(kSchemaMetaId, schema_meta_cols())) {
        if (row_str(row, 1) == name) {
            return true;
        }
    }
    return false;
}

}  // namespace ct
