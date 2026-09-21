// storage.cpp: 存储引擎 M1 实现(堆页追加 + 全表扫描 + 删除)
#include "storage.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "codec.h"
#include "common/err.h"
#include "log/log.h"
#include "page.h"

namespace st {

namespace {

// 元数据表名/列名的 varchar 声明长度
constexpr uint16_t kMetaNameLen = 64;

// db_table 列定义
std::vector<ColumnSpec> table_meta_cols()
{
    return {{"table_id", ColType::BigInt, 0, true},
            {"table_name", ColType::VarChar, kMetaNameLen, true}};
}

// db_column 列定义
std::vector<ColumnSpec> column_meta_cols()
{
    return {{"table_id", ColType::BigInt, 0, true},
            {"col_name", ColType::VarChar, kMetaNameLen, true},
            {"ordinal", ColType::Int, 0, true},
            {"type", ColType::Int, 0, true},
            {"length", ColType::Int, 0, true},
            {"not_null", ColType::Int, 0, true}};
}

// 生成某表的 db_column 自描述行(ordinal 从 0 起, type/not_null 存枚举值与 0/1)
std::vector<std::vector<Value>> column_meta_rows(uint64_t tid, const std::vector<ColumnSpec>& cols)
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

// 行内取 int 字段, NULL 或类型不符报元数据行损坏
int64_t row_int(const std::vector<Value>& row, size_t idx)
{
    const int64_t* v = std::get_if<int64_t>(&row[idx]);
    if (v == nullptr) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "元数据行损坏");
    }
    return *v;
}

// 行内取字符串字段, NULL 或类型不符报元数据行损坏
const std::string& row_str(const std::vector<Value>& row, size_t idx)
{
    const std::string* v = std::get_if<std::string>(&row[idx]);
    if (v == nullptr) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "元数据行损坏");
    }
    return *v;
}

// db_column 行解出 (ordinal, 列定义), 值缺失或非法当场报错
std::pair<int64_t, ColumnSpec> parse_column_row(const std::vector<Value>& row)
{
    const int64_t type = row_int(row, 3);
    const int64_t length = row_int(row, 4);
    const int64_t not_null = row_int(row, 5);
    if (type < static_cast<int64_t>(ColType::Int) ||
        type > static_cast<int64_t>(ColType::Char)) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "列类型值非法: {}", type);
    }
    if (length < 0 || length > UINT16_MAX) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "列长度值非法: {}", length);
    }
    if (not_null != 0 && not_null != 1) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "非空标记值非法: {}", not_null);
    }
    return {row_int(row, 2), ColumnSpec{row_str(row, 1), static_cast<ColType>(type),
                                        static_cast<uint16_t>(length), not_null == 1}};
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
    if (files_.table_file_exists(kTableMetaId)) {
        DB_RAISE(db::ErrCode::CatalogExists, LogModule::STORAGE, "数据目录已初始化: {}", dir_);
    }
    bootstrap_meta_tables();
    // create 不进入打开状态, 落盘脏页与文件后再返回
    pool_.flush_all(files_);
    files_.flush_all();
}

void Database::open()
{
    if (!files_.table_file_exists(kTableMetaId) || !files_.table_file_exists(kColumnMetaId)) {
        DB_RAISE(db::ErrCode::CatalogMissing, LogModule::STORAGE, "数据目录未初始化: {}", dir_);
    }
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

TableMeta Database::table_meta(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return find_table_meta(name);
}

uint64_t Database::create_table(const std::string& name, const std::vector<ColumnSpec>& cols)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return create_table_impl(name, cols, alloc_table_id());
}

uint64_t Database::create_table_impl(const std::string& name, const std::vector<ColumnSpec>& cols,
                                     uint64_t tid)
{
    if (name.empty()) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::STORAGE, "表名为空");
    }
    if (name.size() > kMetaNameLen) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::STORAGE, "表名超过 {} 字节上限", kMetaNameLen);
    }
    if (has_table_name(name)) {
        DB_RAISE(db::ErrCode::TableExists, LogModule::STORAGE, "表已存在: {}", name);
    }
    if (cols.empty()) {
        DB_RAISE(db::ErrCode::InvalidDdl, LogModule::STORAGE, "表至少需要一列");
    }
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i].name.size() > kMetaNameLen) {
            DB_RAISE(db::ErrCode::InvalidDdl, LogModule::STORAGE, "列名超过 {} 字节上限: {}",
                     kMetaNameLen, cols[i].name);
        }
        for (size_t j = i + 1; j < cols.size(); ++j) {
            if (cols[i].name == cols[j].name) {
                DB_RAISE(db::ErrCode::InvalidDdl, LogModule::STORAGE, "存在重复列名: {}", cols[i].name);
            }
        }
    }

    // 先建数据文件, 再写元数据行, 保证元数据可见时数据文件必有效
    init_table_file(tid);
    write_meta_rows(tid, name, cols);
    return tid;
}

void Database::init_table_file(uint64_t tid)
{
    files_.create_table_file(tid);

    // 初始化并落盘文件头页
    const PageId pid0 = PageId{tid, 0};
    char* h = pool_.allocate(pid0, files_);
    init_page(h, MAGIC_FILE_HEADER, PageType::FileHeader);
    pool_.unpin(h);
    pool_.flush(pid0, files_);
}

// 写入指定表的元数据行: db_table 一行 + db_column 每列一行
void Database::write_meta_rows(uint64_t tid, const std::string& name,
                               const std::vector<ColumnSpec>& cols)
{
    insert_impl(kTableMetaId, table_meta_cols(),
                {Value{static_cast<int64_t>(tid)}, Value{name}});
    for (const std::vector<Value>& row : column_meta_rows(tid, cols)) {
        insert_impl(kColumnMetaId, column_meta_cols(), row);
    }
}

void Database::bootstrap_meta_tables()
{
    init_table_file(kTableMetaId);
    init_table_file(kColumnMetaId);
    write_meta_rows(kTableMetaId, kTableMetaName, table_meta_cols());
    write_meta_rows(kColumnMetaId, kColumnMetaName, column_meta_cols());
}

// 删除指定表的元数据行: 两表第 0 列均为 table_id, 匹配即删
void Database::delete_meta_rows(uint64_t tid)
{
    const std::vector<std::pair<uint64_t, std::vector<ColumnSpec>>> metas = {
            {kTableMetaId, table_meta_cols()}, {kColumnMetaId, column_meta_cols()}};
    for (const auto& [meta_tid, cols] : metas) {
        const PageId pid0 = PageId{meta_tid, 0};
        char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
        uint32_t pno = header(h)->next_page;
        pool_.unpin(h);
        while (pno != 0) {
            const PageId pid = PageId{meta_tid, pno};
            char* pg = pool_.read(pid, MAGIC_HEAP, files_);
            PageHeader* ph = header(pg);
            for (uint16_t i = 0; i < ph->slot_count; ++i) {
                if (slot_tombstone(pg, i)) {
                    continue;
                }
                std::vector<Value> vals;
                const Slot* s = slot_at(pg, i);
                if (!decode_row(cols, record(pg, i), s->len, vals)) {
                    DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "元数据行损坏");
                }
                const int64_t* row_tid = std::get_if<int64_t>(&vals[0]);
                if (row_tid == nullptr) {
                    DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "元数据行损坏");
                }
                if (*row_tid == static_cast<int64_t>(tid)) {
                    heap_delete(pg, i);
                    pool_.mark_dirty(pg);
                }
            }
            pno = ph->next_page;
            pool_.unpin(pg);
        }
    }
}

// 读取指定表全部存活行(须持锁): 沿页链解码, 行损坏当场报错
std::vector<std::vector<Value>> Database::read_rows(uint64_t table_id,
                                                    const std::vector<ColumnSpec>& cols)
{
    std::vector<std::vector<Value>> rows;
    const PageId pid0 = PageId{table_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    uint32_t pno = header(h)->next_page;
    pool_.unpin(h);
    while (pno != 0) {
        const PageId pid = PageId{table_id, pno};
        char* pg = pool_.read(pid, MAGIC_HEAP, files_);
        const PageHeader* ph = header(pg);
        for (uint16_t i = 0; i < ph->slot_count; ++i) {
            if (slot_tombstone(pg, i)) {
                continue;
            }
            std::vector<Value> vals;
            const Slot* s = slot_at(pg, i);
            if (!decode_row(cols, record(pg, i), s->len, vals)) {
                DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "元数据行损坏");
            }
            rows.push_back(std::move(vals));
        }
        pno = ph->next_page;
        pool_.unpin(pg);
    }
    return rows;
}

// 按表名查元数据(须持锁): db_table 定位 id, db_column 收集列并按 ordinal 排序
TableMeta Database::find_table_meta(const std::string& name)
{
    uint64_t tid = 0;
    bool found = false;
    for (const std::vector<Value>& row : read_rows(kTableMetaId, table_meta_cols())) {
        if (row_str(row, 1) == name) {
            tid = static_cast<uint64_t>(row_int(row, 0));
            found = true;
            break;
        }
    }
    if (!found) {
        DB_RAISE(db::ErrCode::TableNotFound, LogModule::STORAGE, "表不存在: {}", name);
    }

    std::vector<std::pair<int64_t, ColumnSpec>> pairs;
    for (const std::vector<Value>& row : read_rows(kColumnMetaId, column_meta_cols())) {
        if (row_int(row, 0) == static_cast<int64_t>(tid)) {
            pairs.push_back(parse_column_row(row));
        }
    }
    if (pairs.empty()) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "表无列定义: {}", name);
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 1; i < pairs.size(); ++i) {
        if (pairs[i].first == pairs[i - 1].first) {
            DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "列序号重复: {}", name);
        }
    }

    TableMeta meta;
    meta.table_id = tid;
    meta.name = name;
    meta.cols.reserve(pairs.size());
    for (std::pair<int64_t, ColumnSpec>& p : pairs) {
        meta.cols.push_back(std::move(p.second));
    }
    return meta;
}

// 表名是否已存在(须持锁): 全扫 db_table 匹配
bool Database::has_table_name(const std::string& name)
{
    for (const std::vector<Value>& row : read_rows(kTableMetaId, table_meta_cols())) {
        if (row_str(row, 1) == name) {
            return true;
        }
    }
    return false;
}

// table_id 是否已存在(须持锁): 全扫 db_table 匹配
bool Database::table_id_exists(uint64_t table_id)
{
    for (const std::vector<Value>& row : read_rows(kTableMetaId, table_meta_cols())) {
        if (static_cast<uint64_t>(row_int(row, 0)) == table_id) {
            return true;
        }
    }
    return false;
}

// 用户段分配(须持锁): max(当前最大表 id + 1, kFirstUserTableId)
uint64_t Database::alloc_table_id()
{
    int64_t max_id = 0;
    for (const std::vector<Value>& row : read_rows(kTableMetaId, table_meta_cols())) {
        max_id = std::max(max_id, row_int(row, 0));
    }
    return static_cast<uint64_t>(
            std::max(max_id + 1, static_cast<int64_t>(kFirstUserTableId)));
}

void Database::drop_table(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t tid = find_table_meta(name).table_id;
    if (tid <= kReservedMaxTableId) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::STORAGE, "保留表禁止删除: {}", name);
    }
    delete_meta_rows(tid);
    files_.remove_table_file(tid);
    pool_.drop_table(tid);
    tail_pages_.erase(tid);
}

uint32_t Database::link_header_to_first_data_page(uint64_t table_id)
{
    // 页号 0 是文件头页, 首个数据页固定为页号 1
    const PageId pid0 = PageId{table_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    const uint32_t new_no = 1;
    char* np = pool_.allocate(PageId{table_id, new_no}, files_);
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
    const TableMeta meta = find_table_meta(table);
    return insert_impl(meta.table_id, meta.cols, values);
}

RowRef Database::insert_impl(uint64_t table_id, const std::vector<ColumnSpec>& cols,
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
        const PageId pid = PageId{table_id, tail};
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
        char* np = pool_.allocate(PageId{table_id, new_no}, files_);
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
    const uint64_t tid = ref.page.table_id;
    const uint32_t no = ref.page.page_no;
    if (!table_id_exists(tid) || no == 0) {
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
    const TableMeta meta = find_table_meta(table);
    size_t n = 0;
    const PageId pid0 = PageId{meta.table_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    uint32_t pno = header(h)->next_page;
    pool_.unpin(h);
    while (pno != 0) {
        const PageId pid = PageId{meta.table_id, pno};
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
    return std::make_unique<Scanner>(this, table_meta(table));
}

size_t Database::row_count(const std::string& table)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const TableMeta meta = find_table_meta(table);
    size_t n = 0;
    const PageId pid0 = PageId{meta.table_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    uint32_t pno = header(h)->next_page;
    pool_.unpin(h);
    while (pno != 0) {
        const PageId pid = PageId{meta.table_id, pno};
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
    const PageId pid0 = PageId{meta_.table_id, 0};
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
    cur_page_ = PageId{meta_.table_id, next_page_no_};
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