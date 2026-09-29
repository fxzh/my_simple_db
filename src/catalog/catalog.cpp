// catalog.cpp: 目录层实现(元数据表 schema/引导/查找/写删 + 名字型门面)
#include "catalog.h"

#include <algorithm>
#include <utility>

#include "common/err.h"
#include "log/log.h"

namespace ct {

namespace {

// 元数据表名/列名的 varchar 声明长度
constexpr uint16_t kMetaNameLen = 64;

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
        rows.push_back({st::Value{static_cast<int64_t>(tid)}, st::Value{cols[i].name},
                        st::Value{static_cast<int64_t>(i)},
                        st::Value{static_cast<int64_t>(cols[i].type)},
                        st::Value{static_cast<int64_t>(cols[i].length)},
                        st::Value{static_cast<int64_t>(cols[i].not_null ? 1 : 0)}});
    }
    return rows;
}

// 行内取 int 字段, NULL 或类型不符报元数据行损坏
int64_t row_int(const std::vector<st::Value>& row, size_t idx)
{
    const int64_t* v = std::get_if<int64_t>(&row[idx]);
    if (v == nullptr) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::CATALOG, "元数据行损坏");
    }
    return *v;
}

// 行内取字符串字段, NULL 或类型不符报元数据行损坏
const std::string& row_str(const std::vector<st::Value>& row, size_t idx)
{
    const std::string* v = std::get_if<std::string>(&row[idx]);
    if (v == nullptr) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::CATALOG, "元数据行损坏");
    }
    return *v;
}

// db_column 行解出 (ordinal, 列定义), 值缺失或非法当场报错
std::pair<int64_t, st::ColumnSpec> parse_column_row(const std::vector<st::Value>& row)
{
    const int64_t type = row_int(row, 3);
    const int64_t length = row_int(row, 4);
    const int64_t not_null = row_int(row, 5);
    if (type < static_cast<int64_t>(st::ColType::Int) ||
        type > static_cast<int64_t>(st::ColType::Char)) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::CATALOG, "列类型值非法: {}", type);
    }
    if (length < 0 || length > UINT16_MAX) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::CATALOG, "列长度值非法: {}", length);
    }
    if (not_null != 0 && not_null != 1) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::CATALOG, "非空标记值非法: {}", not_null);
    }
    return {row_int(row, 2), st::ColumnSpec{row_str(row, 1), static_cast<st::ColType>(type),
                                            static_cast<uint16_t>(length), not_null == 1}};
}

}  // namespace

// ==================== Catalog ====================

Catalog::Catalog(std::string dir)
        : dir_(dir), engine_(std::move(dir)) {}

void Catalog::create()
{
    if (engine_.table_file_exists(kTableMetaId)) {
        DB_RAISE(db::ErrCode::CatalogExists, LogModule::CATALOG, "数据目录已初始化: {}", dir_);
    }
    bootstrap_meta_tables();
    // create 不进入打开状态, 落盘脏页与文件后再返回
    engine_.flush_all();
}

void Catalog::open()
{
    if (!engine_.table_file_exists(kTableMetaId) || !engine_.table_file_exists(kColumnMetaId)
        || !engine_.table_file_exists(kSchemaMetaId)) {
        DB_RAISE(db::ErrCode::CatalogMissing, LogModule::CATALOG, "数据目录未初始化: {}", dir_);
    }
    engine_.open();
    // 此处先于客户端线程, 不持锁; 扫 db_table 取现存最大 file_id/table_id 作分配起点
    int64_t max_fid = 0;
    int64_t max_tid = 0;
    for (const std::vector<st::Value>& row : engine_.read_rows(kTableMetaId, table_meta_cols())) {
        max_fid = std::max(max_fid, row_int(row, 2));
        max_tid = std::max(max_tid, row_int(row, 0));
    }
    next_file_id_.store(static_cast<uint64_t>(max_fid) + 1);
    next_table_id_.store(static_cast<uint64_t>(
            std::max(max_tid + 1, static_cast<int64_t>(kFirstUserTableId))));
}

void Catalog::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    engine_.close();
}

st::TableMeta Catalog::table_meta(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return find_table_meta(name);
}

uint64_t Catalog::create_table(const std::string& name, const std::vector<st::ColumnSpec>& cols)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return create_table_impl(name, cols, alloc_table_id());
}

uint64_t Catalog::create_table_impl(const std::string& name,
                                    const std::vector<st::ColumnSpec>& cols, uint64_t tid)
{
    if (name.empty()) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "表名为空");
    }
    if (name.size() > kMetaNameLen) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "表名超过 {} 字节上限", kMetaNameLen);
    }
    if (has_table_name(name)) {
        DB_RAISE(db::ErrCode::TableExists, LogModule::CATALOG, "表已存在: {}", name);
    }
    if (cols.empty()) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "表至少需要一列");
    }
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i].name.size() > kMetaNameLen) {
            DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "列名超过 {} 字节上限: {}",
                     kMetaNameLen, cols[i].name);
        }
        for (size_t j = i + 1; j < cols.size(); ++j) {
            if (cols[i].name == cols[j].name) {
                DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "存在重复列名: {}",
                         cols[i].name);
            }
        }
    }

    const uint64_t fid = alloc_file_id();
    // 先建数据文件, 再写元数据行, 保证元数据可见时数据文件必有效
    engine_.init_table_file(fid);
    write_meta_rows(kSystemSchemaId, tid, fid, name, cols);
    return tid;
}

// 写入指定表的元数据行(须持锁): db_table 一行 + db_column 每列一行
void Catalog::write_meta_rows(uint64_t sid, uint64_t tid, uint64_t fid, const std::string& name,
                              const std::vector<st::ColumnSpec>& cols)
{
    st::RowRef ref;
    engine_.insert_row(kTableMetaId, table_meta_cols(),
                       {st::Value{static_cast<int64_t>(tid)}, st::Value{name},
                        st::Value{static_cast<int64_t>(fid)},
                        st::Value{static_cast<int64_t>(sid)}}, &ref);
    for (const std::vector<st::Value>& row : column_meta_rows(tid, cols)) {
        engine_.insert_row(kColumnMetaId, column_meta_cols(), row, &ref);
    }
}

void Catalog::bootstrap_meta_tables()
{
    engine_.init_table_file(kTableMetaId);
    engine_.init_table_file(kColumnMetaId);
    engine_.init_table_file(kSchemaMetaId);
    write_meta_rows(kSystemSchemaId, kTableMetaId, kTableMetaId, kTableMetaName,
                    table_meta_cols());
    write_meta_rows(kSystemSchemaId, kColumnMetaId, kColumnMetaId, kColumnMetaName,
                    column_meta_cols());
    write_meta_rows(kSystemSchemaId, kSchemaMetaId, kSchemaMetaId, kSchemaMetaName,
                    schema_meta_cols());
    st::RowRef ref;
    engine_.insert_row(kSchemaMetaId, schema_meta_cols(),
                       {st::Value{static_cast<int64_t>(kSystemSchemaId)},
                        st::Value{std::string{kSystemSchemaName}}}, &ref);
}

// 删除指定表的元数据行(须持锁): 扫两张元数据表, 收集第 0 列等于 tid 的行引用后逐个物理删除
void Catalog::delete_meta_rows(uint64_t tid)
{
    const std::vector<std::pair<uint64_t, std::vector<st::ColumnSpec>>> metas = {
            {kTableMetaId, table_meta_cols()}, {kColumnMetaId, column_meta_cols()}};
    for (const auto& [meta_tid, cols] : metas) {
        st::TableMeta meta;
        meta.table_id = meta_tid;
        meta.file_id = meta_tid;
        meta.cols = cols;
        std::vector<st::RowRef> refs;
        st::Scanner scanner(&engine_, meta);
        st::Row row;
        while (scanner.next(&row)) {
            const int64_t* row_tid = std::get_if<int64_t>(&row.values[0]);
            if (row_tid == nullptr) {
                DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::CATALOG, "元数据行损坏");
            }
            if (*row_tid == static_cast<int64_t>(tid)) {
                refs.push_back(row.ref);
            }
        }
        for (const st::RowRef& ref : refs) {
            engine_.delete_row(ref);
        }
    }
}

// 按表名查元数据(须持锁): db_table 定位 id, db_column 收集列并按 ordinal 排序
st::TableMeta Catalog::find_table_meta(const std::string& name)
{
    uint64_t tid = 0;
    uint64_t fid = 0;
    bool found = false;
    for (const std::vector<st::Value>& row : engine_.read_rows(kTableMetaId, table_meta_cols())) {
        if (row_str(row, 1) == name) {
            tid = static_cast<uint64_t>(row_int(row, 0));
            fid = static_cast<uint64_t>(row_int(row, 2));
            found = true;
            break;
        }
    }
    if (!found) {
        DB_RAISE(db::ErrCode::TableNotFound, LogModule::CATALOG, "表不存在: {}", name);
    }

    std::vector<std::pair<int64_t, st::ColumnSpec>> pairs;
    for (const std::vector<st::Value>& row : engine_.read_rows(kColumnMetaId, column_meta_cols())) {
        if (row_int(row, 0) == static_cast<int64_t>(tid)) {
            pairs.push_back(parse_column_row(row));
        }
    }
    if (pairs.empty()) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::CATALOG, "表无列定义: {}", name);
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 1; i < pairs.size(); ++i) {
        if (pairs[i].first == pairs[i - 1].first) {
            DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::CATALOG, "列序号重复: {}", name);
        }
    }

    st::TableMeta meta;
    meta.table_id = tid;
    meta.file_id = fid;
    meta.name = name;
    meta.cols.reserve(pairs.size());
    for (std::pair<int64_t, st::ColumnSpec>& p : pairs) {
        meta.cols.push_back(std::move(p.second));
    }
    return meta;
}

// 表名是否已存在(须持锁): 全扫 db_table 匹配
bool Catalog::has_table_name(const std::string& name)
{
    for (const std::vector<st::Value>& row : engine_.read_rows(kTableMetaId, table_meta_cols())) {
        if (row_str(row, 1) == name) {
            return true;
        }
    }
    return false;
}

// 用户段 table_id 分配: 原子自增返回, 依赖 open() 扫描初始化(不低于 kFirstUserTableId)
uint64_t Catalog::alloc_table_id()
{
    return next_table_id_.fetch_add(1);
}

// file_id 分配: 原子自增返回, 依赖 open() 扫描初始化
uint64_t Catalog::alloc_file_id()
{
    return next_file_id_.fetch_add(1);
}

void Catalog::drop_table(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const st::TableMeta meta = find_table_meta(name);
    if (meta.table_id <= kReservedMaxTableId) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::CATALOG, "保留表禁止删除: {}", name);
    }
    delete_meta_rows(meta.table_id);
    engine_.remove_table_file(meta.file_id);
}

st::RowId Catalog::insert(const std::string& table, const std::vector<st::Value>& values)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const st::TableMeta meta = find_table_meta(table);
    st::RowRef ref;
    return engine_.insert_row(meta.file_id, meta.cols, values, &ref);
}

size_t Catalog::delete_by_ref(const st::RowRef& ref)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ref.page == st::INVALID_PAGE) {
        DB_RAISE(db::ErrCode::Internal, LogModule::CATALOG, "删除引用缺少页位置");
    }
    if (!engine_.table_file_exists(ref.page.file_id)) {
        DB_RAISE(db::ErrCode::TableNotFound, LogModule::CATALOG, "删除引用指向不存在的表文件: fid={}",
                 ref.page.file_id);
    }
    return engine_.delete_row(ref);
}

size_t Catalog::delete_all(const std::string& table)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const st::TableMeta meta = find_table_meta(table);
    return engine_.delete_all_rows(meta.file_id);
}

std::unique_ptr<st::Scanner> Catalog::scan(const std::string& table)
{
    return std::make_unique<st::Scanner>(&engine_, table_meta(table));
}

size_t Catalog::row_count(const std::string& table)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const st::TableMeta meta = find_table_meta(table);
    return engine_.row_count(meta.file_id);
}

}  // namespace ct
