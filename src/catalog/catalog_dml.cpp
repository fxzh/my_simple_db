// catalog_dml.cpp: DML 门面与扫描: 插入/更新/删除, 表扫描/索引扫描/回表/函数查询
#include "catalog_meta.h"

#include <utility>

#include "common/err.h"
#include "log/log.h"

namespace ct {

// ==================== 行写删 ====================

st::RowId Catalog::insert(const st::TableMeta& meta, const std::vector<st::Value>& values)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    st::RowRef ref;
    const st::RowId rid = engine_.insert_row(meta.file_id, meta.cols, values, &ref);
    // 双写该表全部索引: 堆槽不复用使 (键, 行定位) 全局唯一
    for (const IndexEntry& ent : table_indexes(engine_, index_cols_, meta.table_id)) {
        if (ent.col_ordinal >= values.size()) {
            DB_CRITICAL(LogModule::CATALOG, "索引列序号越界: {}", ent.name);
        }
        engine_.index_insert(ent.file_id,
                             encode_key(meta.cols[ent.col_ordinal].type, values[ent.col_ordinal]),
                             ref);
    }
    return rid;
}

size_t Catalog::delete_by_ref(const st::RowRef& ref)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (ref.page == st::INVALID_PAGE) {
        DB_RAISE(db::ErrCode::Internal, LogModule::CATALOG, "删除引用缺少页位置");
    }
    if (!engine_.table_file_exists(ref.page.file_id)) {
        DB_RAISE(db::ErrCode::TableNotFound, LogModule::CATALOG, "删除引用指向不存在的表文件: fid={}",
                 ref.page.file_id);
    }
    return engine_.delete_row(ref);
}

size_t Catalog::update_rows(const st::TableMeta& meta, const std::vector<RowUpdate>& rows)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::vector<IndexEntry> indexes = table_indexes(engine_, index_cols_, meta.table_id);
    for (const RowUpdate& r : rows) {
        if (r.ref.page.file_id != meta.file_id) {
            DB_RAISE(db::ErrCode::Internal, LogModule::CATALOG, "更新引用与目标表文件不符: fid={}",
                     r.ref.page.file_id);
        }
        if (engine_.delete_row(r.ref) == 0) {
            DB_RAISE(db::ErrCode::Internal, LogModule::CATALOG, "更新引用指向已删行");
        }
        st::RowRef new_ref;
        engine_.insert_row(meta.file_id, meta.cols, r.values, &new_ref);
        // 新行位置已变, 该表全部索引(不只被更新列)都要补新条目
        for (const IndexEntry& ent : indexes) {
            if (ent.col_ordinal >= r.values.size()) {
                DB_CRITICAL(LogModule::CATALOG, "索引列序号越界: {}", ent.name);
            }
            engine_.index_insert(
                    ent.file_id,
                    encode_key(meta.cols[ent.col_ordinal].type, r.values[ent.col_ordinal]), new_ref);
        }
    }
    return rows.size();
}

size_t Catalog::delete_all(const st::TableMeta& meta)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return engine_.delete_all_rows(meta.file_id);
}

// ==================== 扫描与查询 ====================

std::unique_ptr<st::Scanner> Catalog::scan(const st::TableMeta& meta)
{
    return std::make_unique<st::Scanner>(&engine_, meta);
}

std::vector<IndexEntry> Catalog::indexes(const st::TableMeta& meta)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return table_indexes(engine_, index_cols_, meta.table_id);
}

std::optional<uint64_t> Catalog::func_id(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const std::vector<st::Value>& row : engine_.read_rows(kFuncMetaId, func_cols_)) {
        if (row_str(row, 1) == name) {
            const int64_t id = row_int(row, 0);
            if (id <= 0) {
                DB_CRITICAL(LogModule::CATALOG, "db_func 函数 id 非法: {}", id);
            }
            return static_cast<uint64_t>(id);
        }
    }
    return std::nullopt;
}

// 按名查内建函数全部重载: 扫 db_func 匹配名并逐行解析
std::vector<FuncRow> Catalog::funcs(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<TypeRow> types;
    for (const std::vector<st::Value>& row : engine_.read_rows(kTypeMetaId, type_meta_cols())) {
        types.push_back(parse_type_row(row));
    }
    std::vector<FuncRow> out;
    for (const std::vector<st::Value>& row : engine_.read_rows(kFuncMetaId, func_cols_)) {
        if (row_str(row, 1) == name) {
            out.push_back(parse_func_row(row, types));
        }
    }
    return out;
}

std::unique_ptr<st::BTreeScanner> Catalog::index_scan(uint64_t index_fid,
                                                      std::optional<st::ScanBound> lo,
                                                      std::optional<st::ScanBound> hi)
{
    return engine_.index_scan(index_fid, std::move(lo), std::move(hi));
}

bool Catalog::read_row(const st::RowRef& ref, const std::vector<st::ColumnSpec>& cols, st::Row* out)
{
    return engine_.read_row(ref, cols, out);
}

size_t Catalog::row_count(const st::TableMeta& meta)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return engine_.row_count(meta.file_id);
}

}  // namespace ct
