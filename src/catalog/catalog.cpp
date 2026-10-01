// catalog.cpp: 目录层实现(元数据表 schema/引导/查找/写删 + 名字型门面)
#include "catalog.h"

#include <algorithm>
#include <utility>

#include "common/err.h"
#include "config/config.h"
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

Catalog::Catalog(std::string dir, bool bootstrap_mode)
        : dir_(dir), engine_(std::move(dir)), bootstrap_mode_(bootstrap_mode) {}

void Catalog::create()
{
    if (engine_.table_file_exists(kTableMetaId)) {
        DB_RAISE(db::ErrCode::CatalogExists, LogModule::CATALOG, "数据目录已初始化: {}", dir_);
    }
    // 自举全程一个事务: 引擎落盘性原语须在活动事务内调用
    begin_txn();
    bootstrap_meta_tables();
    commit_txn();
    // create 不进入打开状态, 检查点收尾(含清空 WAL)后返回, bootstrap 阶段从零日志起步
    engine_.checkpoint();
}

void Catalog::open()
{
    if (bootstrap_mode_) {
        // bootstrap 模式: 目录刚由 initdb 引导, 检查三张自举表文件存在
        if (!engine_.table_file_exists(kTableMetaId) || !engine_.table_file_exists(kColumnMetaId)
            || !engine_.table_file_exists(kSchemaMetaId)) {
            DB_RAISE(db::ErrCode::CatalogMissing, LogModule::CATALOG, "数据目录未初始化: {}", dir_);
        }
    } else if (!engine_.table_file_exists(kTableMetaId)) {
        // 正常模式: db_table 文件缺失无法扫描, 完成标记检查在下方扫 db_table 时按名进行
        DB_RAISE(db::ErrCode::CatalogMissing, LogModule::CATALOG,
                 "数据目录未初始化或初始化未完成: {}", dir_);
    }
    engine_.open();
    // 此处先于客户端线程, 不持锁; 扫 db_table 取现存最大 file_id/table_id, 扫 db_schema 取现存最大 schema_id 作分配起点
    int64_t max_fid = 0;
    int64_t max_tid = 0;
    bool has_version = false;
    for (const std::vector<st::Value>& row : engine_.read_rows(kTableMetaId, table_meta_cols())) {
        max_fid = std::max(max_fid, row_int(row, 2));
        max_tid = std::max(max_tid, row_int(row, 0));
        if (row_str(row, 1) == kVersionMetaName) {
            has_version = true;
        }
    }
    // 完成标记: db_version 行存在代表 bootstrap.sql 全部执行成功, 仅正常模式要求
    if (!bootstrap_mode_ && !has_version) {
        DB_RAISE(db::ErrCode::CatalogMissing, LogModule::CATALOG, "数据目录未初始化或初始化未完成: {}",
                 dir_);
    }
    next_file_id_.store(static_cast<uint64_t>(max_fid) + 1);
    next_table_id_.store(static_cast<uint64_t>(
            std::max(max_tid + 1, static_cast<int64_t>(kFirstUserTableId))));
    int64_t max_sid = 0;
    for (const std::vector<st::Value>& row : engine_.read_rows(kSchemaMetaId, schema_meta_cols())) {
        max_sid = std::max(max_sid, row_int(row, 0));
    }
    next_schema_id_.store(static_cast<uint64_t>(max_sid) + 1);
}

void Catalog::close()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    engine_.close();
}

// 开启事务: 全局锁长持至事务结束, 事务内门面方法同线程递归重入
void Catalog::begin_txn()
{
    mutex_.lock();
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::adopt_lock);
    engine_.begin_txn();
    lock.release();
}

// 提交事务: Engine 侧追加 Commit 并 fsync(持久化边界), 之后 WAL 自上次清空累计
// 字节达到阈值则做运行期检查点——此刻事务记录已结束、锁仍持有, 检查点不可能
// 打断进行中的事务; 守卫保证任意退出路径都解除全局锁
void Catalog::commit_txn()
{
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::adopt_lock);
    try {
        engine_.commit_txn();
    } catch (...) {
        // 提交失败: 残留引擎事务按回滚清理, 异常原样上抛
        if (engine_.in_txn()) {
            engine_.rollback_txn();
        }
        throw;
    }
    const uint64_t bytes = engine_.wal_bytes_since_reset();
    if (bytes >= config::cfg.wal_checkpoint_bytes) {
        engine_.checkpoint();
        LOG_INFO(LogModule::CATALOG,
                 "运行期检查点: WAL 自上次清空累计 %llu 字节达到阈值, 已刷脏页并清空日志",
                 static_cast<unsigned long long>(bytes));
    }
}

// 回滚事务: Engine 侧按 undo 逆序复原已发生的修改, 守卫保证异常路径也解除全局锁
void Catalog::rollback_txn()
{
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::adopt_lock);
    engine_.rollback_txn();
}

st::TableMeta Catalog::table_meta(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return find_table_meta(name);
}

uint64_t Catalog::create_table(const std::string& name, const std::vector<st::ColumnSpec>& cols)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // bootstrap 模式: 用 SET 的显式 table_id 建表, 未 set 与重复 id 报错; 正常模式自动分配
    if (bootstrap_mode_) {
        if (bootstrap_table_id_ == 0) {
            DB_RAISE(db::ErrCode::InvalidDdl, LogModule::CATALOG,
                     "bootstrap 模式 create table 前须 SET table_id");
        }
        if (has_table_id(bootstrap_table_id_)) {
            DB_RAISE(db::ErrCode::TableExists, LogModule::CATALOG, "table_id 已被占用: {}",
                     bootstrap_table_id_);
        }
        return create_table_impl(name, cols, bootstrap_table_id_);
    }
    return create_table_impl(name, cols, alloc_table_id());
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
                       {st::Value{static_cast<int64_t>(alloc_schema_id())}, st::Value{name}}, &ref);
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

void Catalog::drop_table(const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const st::TableMeta meta = find_table_meta(name);
    if (meta.table_id <= kReservedMaxTableId) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::CATALOG, "保留表禁止删除: {}", name);
    }
    delete_meta_rows(meta.table_id);
    engine_.remove_table_file(meta.file_id);
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

st::RowId Catalog::insert(const std::string& table, const std::vector<st::Value>& values)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const st::TableMeta meta = find_table_meta(table);
    st::RowRef ref;
    return engine_.insert_row(meta.file_id, meta.cols, values, &ref);
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

size_t Catalog::delete_all(const std::string& table)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const st::TableMeta meta = find_table_meta(table);
    return engine_.delete_all_rows(meta.file_id);
}

std::unique_ptr<st::Scanner> Catalog::scan(const std::string& table)
{
    return std::make_unique<st::Scanner>(&engine_, table_meta(table));
}

size_t Catalog::row_count(const std::string& table)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const st::TableMeta meta = find_table_meta(table);
    return engine_.row_count(meta.file_id);
}

}  // namespace ct
