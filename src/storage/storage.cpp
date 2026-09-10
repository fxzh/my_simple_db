// storage.cpp: 存储引擎 M1 实现(堆页追加 + 全表扫描 + 删除)
#include "storage.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

#include "codec.h"
#include "page.h"

namespace st {

namespace {

std::string catalog_path_of(const std::string& dir) { return dir + "/catalog.dat"; }

}  // namespace

// ==================== Database ====================

Database::Database(std::string dir)
        : dir_(std::move(dir)), files_(dir_), pool_(BufferPool::kDefaultCapacity) {}

Database::~Database() {
    if (open_) {
        close();
    }
}

void Database::open() {
    std::filesystem::create_directories(dir_);
    catalog_.load(catalog_path_of(dir_));
    if (!std::filesystem::exists(catalog_path_of(dir_))) {
        catalog_.save(catalog_path_of(dir_));  // 首次打开: 生成空目录文件
    }
    pool_.invalidate_all();
    tail_pages_.clear();
    open_ = true;
}

void Database::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    pool_.flush_all(files_);
    files_.flush_all();
    files_.close_all();
    open_ = false;
}

const TableMeta& Database::get_table(const std::string& name) const {
    const TableMeta* meta = catalog_.find(name);
    if (meta == nullptr) {
        throw std::runtime_error("表不存在: " + name);
    }
    return *meta;
}

uint32_t Database::create_table(const std::string& name,
                                                                const std::vector<ColumnSpec>& cols) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (name.empty()) {
        throw std::runtime_error("表名为空");
    }
    if (catalog_.find(name) != nullptr) {
        throw std::runtime_error("表已存在: " + name);
    }
    if (cols.empty()) {
        throw std::runtime_error("表至少需要一列");
    }
    for (size_t i = 0; i < cols.size(); ++i) {
        for (size_t j = i + 1; j < cols.size(); ++j) {
            if (cols[i].name == cols[j].name) {
                throw std::runtime_error("存在重复列名: " + cols[i].name);
            }
        }
    }

    const uint32_t tid = catalog_.alloc_table_id();
    files_.create_table_file(tid);

    // 先初始化并落盘文件头页, 再写目录, 保证目录有表时数据文件必有效
    const PageId pid0 = make_page_id(tid, 0);
    char* h = pool_.allocate(pid0, files_);
    init_page(h, MAGIC_FILE_HEADER, PageType::FileHeader);
    pool_.unpin(h);
    pool_.flush(pid0, files_);

    TableMeta meta{tid, name, cols};
    catalog_.add_or_update(meta);
    catalog_.save(catalog_path_of(dir_));
    return tid;
}

void Database::drop_table(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t tid = get_table(name).table_id;
    catalog_.erase(name);
    catalog_.save(catalog_path_of(dir_));
    files_.remove_table_file(tid);
    pool_.drop_table(tid);
    tail_pages_.erase(tid);
}

uint32_t Database::link_header_to_first_data_page(uint32_t table_id) {
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

RowRef Database::insert(const std::string& table, const std::vector<Value>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    const TableMeta& meta = get_table(table);

    std::vector<uint8_t> rec;
    if (!encode_row(meta.cols, values, rec)) {
        throw std::runtime_error("值与列类型不匹配");
    }
    if (rec.size() > MAX_RECORD_LEN) {
        throw std::runtime_error("记录超长");
    }

    // 尾页可能只在内存中(尚未落盘), 用 tail_pages_ 记住最高页号
    uint32_t tail = 0;
    auto it = tail_pages_.find(meta.table_id);
    if (it != tail_pages_.end()) {
        tail = it->second;
    } else if (files_.page_count(meta.table_id) == 1) {
        tail = link_header_to_first_data_page(meta.table_id);
    } else {
        tail = files_.page_count(meta.table_id) - 1;
    }

    for (;;) {
        const PageId pid = make_page_id(meta.table_id, tail);
        char* pg = pool_.read(pid, MAGIC_HEAP, files_);
        uint16_t slot = 0;
        if (heap_append(pg, rec.data(), static_cast<uint16_t>(rec.size()), &slot)) {
            pool_.mark_dirty(pg);
            pool_.unpin(pg);
            tail_pages_[meta.table_id] = tail;
            return RowRef{pid, slot};
        }
        // 页满: 扩展一个新页并把尾页链上去
        // 新页号取磁盘页数与当前尾页+1 的较大值, 避免与仅存内存中的页冲突
        PageHeader* ph = header(pg);
        const uint32_t new_no =
                std::max(files_.page_count(meta.table_id), tail + 1);
        char* np = pool_.allocate(make_page_id(meta.table_id, new_no), files_);
        init_page(np, MAGIC_HEAP, PageType::Heap);
        pool_.unpin(np);
        ph->next_page = new_no;
        ph->checksum = page_checksum(pg);
        pool_.mark_dirty(pg);
        pool_.unpin(pg);
        tail = new_no;
    }
}

size_t Database::delete_by_ref(const RowRef& ref) {
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

size_t Database::delete_all(const std::string& table) {
    std::lock_guard<std::mutex> lock(mutex_);
    const TableMeta& meta = get_table(table);
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

std::unique_ptr<Scanner> Database::scan(const std::string& table) {
    const TableMeta& meta = get_table(table);
    return std::make_unique<Scanner>(this, meta);
}

size_t Database::row_count(const std::string& table) {
    std::lock_guard<std::mutex> lock(mutex_);
    const TableMeta& meta = get_table(table);
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
        : db_(db), meta_(meta) {
    const PageId pid0 = make_page_id(meta_.table_id, 0);
    char* h = db_->pool_.read(pid0, MAGIC_FILE_HEADER, db_->files_);
    next_page_no_ = header(h)->next_page;
    db_->pool_.unpin(h);
}

Scanner::~Scanner() { close(); }

void Scanner::close() {
    if (cur_data_ != nullptr) {
        db_->pool_.unpin(cur_data_);
        cur_data_ = nullptr;
    }
    done_ = true;
}

void Scanner::advance_page() {
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

bool Scanner::next(Row* out) {
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
                throw std::runtime_error("数据页记录损坏");
            }
            out->ref = RowRef{cur_page_, slot_};
            ++slot_;
            return true;
        }
        advance_page();  // 本页读完, 进入下一页
    }
}

}  // namespace st