// storage.cpp: 存储引擎 M1 实现(堆页追加 + 全表扫描 + 删除)
#include "storage.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

#include "codec.h"
#include "common/err.h"
#include "log/log.h"
#include "page.h"

namespace st {

namespace {

std::string catalog_path_of(const std::string& dir) { return dir + "/catalog.dat"; }

// 元数据表名/列名的 varchar 声明长度
constexpr uint16_t kMetaNameLen = 64;

// db_table 列定义
std::vector<ColumnSpec> table_meta_cols()
{
    return {{"table_id", ColType::Int, 0, true},
            {"table_name", ColType::VarChar, kMetaNameLen, true}};
}

// db_column 列定义
std::vector<ColumnSpec> column_meta_cols()
{
    return {{"table_id", ColType::Int, 0, true},
            {"col_name", ColType::VarChar, kMetaNameLen, true},
            {"ordinal", ColType::Int, 0, true},
            {"type", ColType::Int, 0, true},
            {"length", ColType::Int, 0, true},
            {"not_null", ColType::Int, 0, true}};
}

// 生成某表的 db_column 自描述行(ordinal 从 0 起, type/not_null 存枚举值与 0/1)
std::vector<std::vector<Value>> column_meta_rows(uint32_t tid, const std::vector<ColumnSpec>& cols)
{
    std::vector<std::vector<Value>> rows;
    rows.reserve(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) {
        rows.push_back({Value{static_cast<int64_t>(tid)}, Value{cols[i].name},
                        Value{static_cast<int64_t>(i)},
                        Value{static_cast<int64_t>(cols[i].type)},
                        Value{static_cast<int64_t>(cols[i].length)},
                        Value{static_cast<int64_t>(cols[i].not_null ? 1 : 0)}});
    }
    return rows;
}

}  // namespace

// ==================== Database ====================

Database::Database(std::string dir)
        : dir_(std::move(dir)), files_(dir_), pool_(BufferPool::kDefaultCapacity) {}

Database::~Database()
{
    if (open_) {
        close();
    }
}

void Database::create()
{
    const std::string path = catalog_path_of(dir_);
    if (std::filesystem::exists(path)) {
        DB_RAISE(db::ErrCode::CatalogExists, LogModule::STORAGE, "目录文件已存在: {}", path);
    }
    bootstrap_meta_tables();
    catalog_.save(path);
    // create 不进入打开状态, 落盘脏页与文件后再返回
    pool_.flush_all(files_);
    files_.flush_all();
}

void Database::open()
{
    catalog_.load(catalog_path_of(dir_));
    pool_.invalidate_all();
    tail_pages_.clear();
    open_ = true;
}

void Database::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pool_.flush_all(files_);
    files_.flush_all();
    files_.close_all();
    open_ = false;
}

const TableMeta& Database::table_meta(const std::string& name) const
{
    const TableMeta* meta = catalog_.find(name);
    if (meta == nullptr) {
        DB_RAISE(db::ErrCode::TableNotFound, LogModule::STORAGE, "表不存在: {}", name);
    }
    return *meta;
}

uint32_t Database::create_table(const std::string& name, const std::vector<ColumnSpec>& cols)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return create_table_impl(name, cols, catalog_.alloc_table_id());
}

uint32_t Database::create_table_impl(const std::string& name, const std::vector<ColumnSpec>& cols,
                                     uint32_t tid)
{
    if (name.empty()) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::STORAGE, "表名为空");
    }
    if (catalog_.find(name) != nullptr) {
        DB_RAISE(db::ErrCode::TableExists, LogModule::STORAGE, "表已存在: {}", name);
    }
    if (cols.empty()) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::STORAGE, "表至少需要一列");
    }
    for (size_t i = 0; i < cols.size(); ++i) {
        for (size_t j = i + 1; j < cols.size(); ++j) {
            if (cols[i].name == cols[j].name) {
                DB_RAISE(db::ErrCode::InvalidDdl, LogModule::STORAGE, "存在重复列名: {}", cols[i].name);
            }
        }
    }

    // 先建数据文件, 再写目录, 保证目录有表时数据文件必有效
    init_table_file(tid);

    TableMeta meta{tid, name, cols};
    catalog_.add_or_update(meta);
    catalog_.save(catalog_path_of(dir_));
    return tid;
}

void Database::init_table_file(uint32_t tid)
{
    files_.create_table_file(tid);

    // 初始化并落盘文件头页
    const PageId pid0 = make_page_id(tid, 0);
    char* h = pool_.allocate(pid0, files_);
    init_page(h, MAGIC_FILE_HEADER, PageType::FileHeader);
    pool_.unpin(h);
    pool_.flush(pid0, files_);
}

void Database::bootstrap_meta_tables()
{
    const std::vector<ColumnSpec> tcols = table_meta_cols();
    const std::vector<ColumnSpec> ccols = column_meta_cols();
    init_table_file(kTableMetaId);
    init_table_file(kColumnMetaId);
    insert_impl(kTableMetaId, tcols, {Value{static_cast<int64_t>(kTableMetaId)},
                                      Value{std::string{kTableMetaName}}});
    insert_impl(kTableMetaId, tcols, {Value{static_cast<int64_t>(kColumnMetaId)},
                                      Value{std::string{kColumnMetaName}}});
    for (const std::vector<Value>& row : column_meta_rows(kTableMetaId, tcols)) {
        insert_impl(kColumnMetaId, ccols, row);
    }
    for (const std::vector<Value>& row : column_meta_rows(kColumnMetaId, ccols)) {
        insert_impl(kColumnMetaId, ccols, row);
    }
}

void Database::drop_table(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t tid = table_meta(name).table_id;
    if (tid <= kReservedMaxTableId) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::STORAGE, "保留表禁止删除: {}", name);
    }
    catalog_.erase(name);
    catalog_.save(catalog_path_of(dir_));
    files_.remove_table_file(tid);
    pool_.drop_table(tid);
    tail_pages_.erase(tid);
}

uint32_t Database::link_header_to_first_data_page(uint32_t table_id)
{
    // 页号 0 是文件头页, 首个数据页固定为页号 1
    const PageId pid0 = make_page_id(table_id, 0);
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    const uint32_t new_no = 1;
    char* np = pool_.allocate(make_page_id(table_id, new_no), files_);
    init_page(np, MAGIC_HEAP, PageType::Heap);
    pool_.unpin(np);

    PageHeader* hh = header(h);
    hh->next_page = new_no;
    hh->checksum = page_checksum(h);
    pool_.mark_dirty(h);
    pool_.unpin(h);
    return new_no;
}

RowRef Database::insert(const std::string& table, const std::vector<Value>& values)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const TableMeta& meta = table_meta(table);
    return insert_impl(meta.table_id, meta.cols, values);
}

RowRef Database::insert_impl(uint32_t table_id, const std::vector<ColumnSpec>& cols,
                             const std::vector<Value>& values)
{
    if (cols.size() != values.size()) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "值的数量与列数不符");
    }
    for (size_t i = 0; i < values.size(); ++i) {
        if (cols[i].not_null && std::holds_alternative<std::monostate>(values[i])) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "NOT NULL 列不允许 NULL: {}",
                     cols[i].name);
        }
    }
    std::vector<uint8_t> rec;
    if (!encode_row(cols, values, rec)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "值与列类型不匹配");
    }
    if (rec.size() > MAX_RECORD_LEN) {
        DB_RAISE(db::ErrCode::RecordTooLong, LogModule::STORAGE, "记录超长");
    }

    // 尾页可能只在内存中(尚未落盘), 用 tail_pages_ 记住最高页号
    uint32_t tail = 0;
    auto it = tail_pages_.find(table_id);
    if (it != tail_pages_.end()) {
        tail = it->second;
    } else if (files_.page_count(table_id) == 1) {
        tail = link_header_to_first_data_page(table_id);
    } else {
        tail = files_.page_count(table_id) - 1;
    }

    for (;;) {
        const PageId pid = make_page_id(table_id, tail);
        char* pg = pool_.read(pid, MAGIC_HEAP, files_);
        uint16_t slot = 0;
        if (heap_append(pg, rec.data(), static_cast<uint16_t>(rec.size()), &slot)) {
            pool_.mark_dirty(pg);
            pool_.unpin(pg);
            tail_pages_[table_id] = tail;
            return RowRef{pid, slot};
        }
        // 页满: 扩展一个新页并把尾页链上去
        // 新页号取磁盘页数与当前尾页+1 的较大值, 避免与仅存内存中的页冲突
        PageHeader* ph = header(pg);
        const uint32_t new_no = std::max(files_.page_count(table_id), tail + 1);
        char* np = pool_.allocate(make_page_id(table_id, new_no), files_);
        init_page(np, MAGIC_HEAP, PageType::Heap);
        pool_.unpin(np);
        ph->next_page = new_no;
        ph->checksum = page_checksum(pg);
        pool_.mark_dirty(pg);
        pool_.unpin(pg);
        tail = new_no;
    }
}

size_t Database::delete_by_ref(const RowRef& ref)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ref.page == INVALID_PAGE) {
        return 0;
    }
    const uint32_t tid = page_table_id(ref.page);
    const uint32_t no = page_no(ref.page);
    if (catalog_.find_by_id(tid) == nullptr || no == 0) {
        return 0;  // 表不存在或指向文件头页
    }
    uint32_t max_no = files_.page_count(tid) - 1;
    auto tail = tail_pages_.find(tid);
    if (tail != tail_pages_.end() && tail->second > max_no) {
        max_no = tail->second;
    }
    if (no > max_no) {
        return 0;  // 页号越界, 避免缓冲池补造空页
    }
    char* pg = pool_.read(ref.page, MAGIC_HEAP, files_);
    const PageHeader* ph = header(pg);
    if (ref.slot >= ph->slot_count || slot_tombstone(pg, ref.slot)) {
        pool_.unpin(pg);
        return 0;  // 已删或越界
    }
    heap_delete(pg, ref.slot);
    pool_.mark_dirty(pg);
    pool_.unpin(pg);
    return 1;
}

size_t Database::delete_all(const std::string& table)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const TableMeta& meta = table_meta(table);
    size_t n = 0;
    const PageId pid0 = make_page_id(meta.table_id, 0);
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    uint32_t pno = header(h)->next_page;
    pool_.unpin(h);
    while (pno != 0) {
        const PageId pid = make_page_id(meta.table_id, pno);
        char* pg = pool_.read(pid, MAGIC_HEAP, files_);
        PageHeader* ph = header(pg);
        for (uint16_t i = 0; i < ph->slot_count; ++i) {
            if (!slot_tombstone(pg, i)) {
                ++n;
            }
        }
        ph->slot_count = 0;
        ph->free_begin = PAGE_HEADER_SIZE;
        ph->free_end = PAGE_SIZE;
        ph->checksum = page_checksum(pg);
        pool_.mark_dirty(pg);
        pno = ph->next_page;
        pool_.unpin(pg);
    }
    return n;
}

std::unique_ptr<Scanner> Database::scan(const std::string& table)
{
    const TableMeta& meta = table_meta(table);
    return std::make_unique<Scanner>(this, meta);
}

size_t Database::row_count(const std::string& table)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const TableMeta& meta = table_meta(table);
    size_t n = 0;
    const PageId pid0 = make_page_id(meta.table_id, 0);
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    uint32_t pno = header(h)->next_page;
    pool_.unpin(h);
    while (pno != 0) {
        const PageId pid = make_page_id(meta.table_id, pno);
        char* pg = pool_.read(pid, MAGIC_HEAP, files_);
        const PageHeader* ph = header(pg);
        for (uint16_t i = 0; i < ph->slot_count; ++i) {
            if (!slot_tombstone(pg, i)) {
                ++n;
            }
        }
        pno = ph->next_page;
        pool_.unpin(pg);
    }
    return n;
}

// ==================== Scanner ====================

Scanner::Scanner(Database* db, const TableMeta& meta)
        : db_(db), meta_(meta)
{
    const PageId pid0 = make_page_id(meta_.table_id, 0);
    char* h = db_->pool_.read(pid0, MAGIC_FILE_HEADER, db_->files_);
    next_page_no_ = header(h)->next_page;
    db_->pool_.unpin(h);
}

Scanner::~Scanner() { close(); }

void Scanner::close()
{
    if (cur_data_ != nullptr) {
        db_->pool_.unpin(cur_data_);
        cur_data_ = nullptr;
    }
    done_ = true;
}

void Scanner::advance_page()
{
    if (cur_data_ != nullptr) {
        db_->pool_.unpin(cur_data_);
        cur_data_ = nullptr;
    }
    if (next_page_no_ == 0) {
        done_ = true;
        return;
    }
    cur_page_ = make_page_id(meta_.table_id, next_page_no_);
    cur_data_ = db_->pool_.read(cur_page_, MAGIC_HEAP, db_->files_);
    slot_ = 0;
    next_page_no_ = header(cur_data_)->next_page;
}

bool Scanner::next(Row* out)
{
    if (done_) {
        return false;
    }
    for (;;) {
        if (cur_data_ == nullptr) {
            advance_page();
            if (done_) {
                return false;
            }
        }
        const PageHeader* ph = header(cur_data_);
        while (slot_ < ph->slot_count && slot_tombstone(cur_data_, slot_)) {
            ++slot_;  // 跳过已删墓碑槽
        }
        if (slot_ < ph->slot_count) {
            const Slot* s = slot_at(cur_data_, slot_);
            if (!decode_row(meta_.cols, record(cur_data_, slot_), s->len, out->values)) {
                DB_RAISE(db::ErrCode::CorruptData, LogModule::STORAGE, "数据页记录损坏");
            }
            out->ref = RowRef{cur_page_, slot_};
            ++slot_;
            return true;
        }
        advance_page();  // 本页读完, 进入下一页
    }
}

}  // namespace st