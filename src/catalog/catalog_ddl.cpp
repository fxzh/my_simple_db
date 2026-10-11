// catalog_ddl.cpp: 建删表/schema/索引(含 bootstrap 建表路径)与 id 分配
#include "catalog_internal.h"

#include "common/err.h"
#include "log/log.h"

namespace ct {

// ==================== 建表与建 schema ====================

uint64_t Catalog::create_table(const TableRef& table, const std::vector<st::ColumnSpec>& cols)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const uint64_t sid = resolve_schema_id(table);
    // bootstrap 模式走 SET 的显式 id 路径, 正常模式自动分配
    if (bootstrap_mode_) {
        return create_table_bootstrap(sid, table, cols);
    }
    return create_table_impl(sid, table, cols, alloc_table_id());
}

void Catalog::create_schema(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (name.empty()) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "schema 名为空");
    }
    if (name.size() > kMetaNameLen) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "schema 名超过 {} 字节上限", kMetaNameLen);
    }
    if (has_schema_name(name)) {
        DB_RAISE(db::ErrCode::SchemaExists, LogModule::CATALOG, "schema 已存在: {}", name);
    }
    st::RowRef ref;
    engine_.insert_row(kSchemaMetaId, schema_meta_cols(),
                       {st::bigint_val(static_cast<int64_t>(alloc_schema_id())),
                        st::str_val(name)}, &ref);
}

uint64_t Catalog::create_table_impl(uint64_t sid, const TableRef& table,
                                    const std::vector<st::ColumnSpec>& cols, uint64_t tid)
{
    if (table.name.empty()) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "表名为空");
    }
    if (table.name.size() > kMetaNameLen) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "表名超过 {} 字节上限", kMetaNameLen);
    }
    if (has_table_name(sid, table.name)) {
        DB_RAISE(db::ErrCode::TableExists, LogModule::CATALOG, "表已存在: {}",
                 table_ref_to_string(table));
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
    write_meta_rows(sid, tid, fid, table.name, cols);
    return tid;
}

// bootstrap 模式建表(须持锁): 用 SET 的显式 table_id, 未 set 或 id 被占用报错
uint64_t Catalog::create_table_bootstrap(uint64_t sid, const TableRef& table,
                                         const std::vector<st::ColumnSpec>& cols)
{
    if (bootstrap_table_id_ == 0) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG,
                 "bootstrap 模式 create table 前须 SET table_id");
    }
    if (has_table_id(bootstrap_table_id_)) {
        DB_RAISE(db::ErrCode::TableExists, LogModule::CATALOG, "table_id 已被占用: {}",
                 bootstrap_table_id_);
    }
    const uint64_t tid = create_table_impl(sid, table, cols, bootstrap_table_id_);
    if (tid == kIndexMetaId) {
        // bootstrap 建 db_index: 填充内存列定义缓存(open 时该表尚不存在)
        index_cols_ = cols;
    }
    if (tid == kFuncMetaId) {
        // bootstrap 建 db_func: 填充内存列定义缓存(open 时该表尚不存在)
        func_cols_ = cols;
    }
    return tid;
}

// 写入指定表的元数据行(须持锁): db_table 一行 + db_column 每列一行
void Catalog::write_meta_rows(uint64_t sid, uint64_t tid, uint64_t fid, const std::string& name,
                              const std::vector<st::ColumnSpec>& cols)
{
    st::RowRef ref;
    engine_.insert_row(kTableMetaId, table_meta_cols(),
                       {st::bigint_val(static_cast<int64_t>(tid)), st::str_val(name),
                        st::bigint_val(static_cast<int64_t>(fid)),
                        st::bigint_val(static_cast<int64_t>(sid))}, &ref);
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
                       {st::bigint_val(static_cast<int64_t>(kSystemSchemaId)),
                        st::str_val(std::string{kSystemSchemaName})}, &ref);
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
            const int64_t* row_tid = std::get_if<int64_t>(&row.values[0].box);
            if (row_tid == nullptr) {
                DB_CRITICAL(LogModule::CATALOG, "元数据行损坏");
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

// ==================== id 分配 ====================

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

// schema_id 分配: 原子自增返回, 依赖 open() 扫描初始化
uint64_t Catalog::alloc_schema_id()
{
    return next_schema_id_.fetch_add(1);
}

// ==================== 删表/schema 与索引 DDL ====================

void Catalog::drop_table(const TableHandle& table)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (table.meta.table_id <= kReservedMaxTableId) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::CATALOG, "保留表禁止删除: {}",
                 table.display);
    }
    // 连带删除该表全部索引: 先删 db_index 行再删索引文件(与建索引顺序相反)
    for (const IndexEntry& ent : table_indexes(engine_, index_cols_, table.meta.table_id)) {
        engine_.delete_row(ent.row_ref);
        engine_.remove_index_file(ent.file_id);
    }
    delete_meta_rows(table.meta.table_id);
    engine_.remove_table_file(table.meta.file_id);
}

void Catalog::drop_schema(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 定位 schema 行: 扫 db_schema 按名匹配取 id 与行引用
    st::TableMeta meta;
    meta.table_id = kSchemaMetaId;
    meta.file_id = kSchemaMetaId;
    meta.cols = schema_meta_cols();
    int64_t sid = 0;
    st::RowRef ref;
    bool found = false;
    st::Scanner scanner(&engine_, meta);
    st::Row row;
    while (scanner.next(&row)) {
        if (row_str(row.values, 1) == name) {
            sid = row_int(row.values, 0);
            ref = row.ref;
            found = true;
            break;
        }
    }
    if (!found) {
        DB_RAISE(db::ErrCode::SchemaNotFound, LogModule::CATALOG, "schema 不存在: {}", name);
    }
    // 非空拒绝: db_table 有挂该 schema 的表则拒绝, system 名下有元数据表因此受保护
    for (const std::vector<st::Value>& trow : engine_.read_rows(kTableMetaId, table_meta_cols())) {
        if (row_int(trow, 3) == sid) {
            DB_RAISE(db::ErrCode::SchemaNotEmpty, LogModule::CATALOG, "schema 非空禁止删除: {}", name);
        }
    }
    engine_.delete_row(ref);
}

void Catalog::create_index(const TableHandle& table, const std::string& index, uint16_t col_ordinal)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const st::TableMeta& meta = table.meta;
    if (meta.table_id <= kReservedMaxTableId) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::CATALOG, "保留表禁止建索引: {}",
                 table.display);
    }
    if (index.empty()) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "索引名为空");
    }
    if (index.size() > kMetaNameLen) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG, "索引名超过 {} 字节上限", kMetaNameLen);
    }
    if (has_index_name(engine_, index_cols_, meta.table_id, index)) {
        DB_RAISE(db::ErrCode::IndexExists, LogModule::CATALOG, "索引已存在: {}", index);
    }
    if (col_ordinal >= meta.cols.size()) {
        DB_RAISE(db::ErrCode::Internal, LogModule::CATALOG, "索引列序号越界: {}", col_ordinal);
    }
    const uint64_t fid = alloc_file_id();
    // 先建索引文件并全表回填, 再写元数据行, 保证元数据可见时索引文件必有效
    engine_.init_index_file(fid);
    engine_.build_index(meta.file_id, meta.cols, col_ordinal, fid);
    st::RowRef ref;
    engine_.insert_row(kIndexMetaId, index_cols_,
                       {st::bigint_val(static_cast<int64_t>(meta.table_id)),
                        st::str_val(index),
                        st::int_val(static_cast<int64_t>(col_ordinal)),
                        st::bigint_val(static_cast<int64_t>(fid))}, &ref);
}

void Catalog::drop_index(const TableHandle& table, const std::string& index)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const st::TableMeta& meta = table.meta;
    if (meta.table_id <= kReservedMaxTableId) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::CATALOG, "保留表禁止删索引: {}",
                 table.display);
    }
    IndexEntry ent;
    if (!find_index(engine_, index_cols_, meta.table_id, index, ent)) {
        DB_RAISE(db::ErrCode::IndexNotFound, LogModule::CATALOG, "索引不存在: {}", index);
    }
    // 先删元数据行再删索引文件, 与建索引顺序相反
    engine_.delete_row(ent.row_ref);
    engine_.remove_index_file(ent.file_id);
}

}  // namespace ct
