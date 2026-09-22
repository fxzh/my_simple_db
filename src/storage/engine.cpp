// engine.cpp: 文件引擎 M1 实现(堆页追加 + 全表扫描 + 删除)
#include "engine.h"

#include <algorithm>
#include <utility>

#include "codec.h"
#include "common/err.h"
#include "log/log.h"
#include "page.h"

namespace st {

// ==================== Engine ====================

Engine::Engine(std::string dir)
        : dir_(std::move(dir)), files_(dir_), pool_(BufferPool::kDefaultCapacity) {}

Engine::~Engine()
{
    if (open_) {
        close();
    }
}

void Engine::open()
{
    pool_.invalidate_all();
    tail_pages_.clear();
    open_ = true;
}

void Engine::flush_all()
{
    pool_.flush_all(files_);
    files_.flush_all();
}

void Engine::close()
{
    pool_.flush_all(files_);
    files_.flush_all();
    files_.close_all();
    open_ = false;
}

bool Engine::table_file_exists(uint64_t tid) const
{
    return files_.table_file_exists(tid);
}

// 物理建表(须持锁): 建数据文件并初始化落盘文件头页
void Engine::init_table_file(uint64_t tid)
{
    files_.create_table_file(tid);

    // 初始化并落盘文件头页
    const PageId pid0 = PageId{tid, 0};
    char* h = pool_.allocate(pid0, files_);
    init_page(h, MAGIC_FILE_HEADER, PageType::FileHeader);
    pool_.unpin(h);
    pool_.flush(pid0, files_);
}

// 物理删表(须持锁): 删数据文件并清缓冲与尾页跟踪
void Engine::remove_table_file(uint64_t tid)
{
    files_.remove_table_file(tid);
    pool_.drop_table(tid);
    tail_pages_.erase(tid);
}

uint32_t Engine::link_header_to_first_data_page(uint64_t table_id)
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

// 插行(须持锁): 校验编码后追加
RowRef Engine::insert_row(uint64_t table_id, const std::vector<ColumnSpec>& cols,
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

// 读取指定表全部存活行(须持锁): 沿页链解码, 行损坏当场报错
std::vector<std::vector<Value>> Engine::read_rows(uint64_t table_id,
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
                DB_RAISE(db::ErrCode::CorruptData, LogModule::STORAGE, "数据页记录损坏");
            }
            rows.push_back(std::move(vals));
        }
        pno = ph->next_page;
        pool_.unpin(pg);
    }
    return rows;
}

// 删除单行(须持锁): 按物理位置打墓碑, 无效/已删引用返回 0
size_t Engine::delete_row(const RowRef& ref)
{
    if (ref.page == INVALID_PAGE) {
        return 0;
    }
    const uint64_t tid = ref.page.table_id;
    const uint32_t no = ref.page.page_no;
    if (no == 0) {
        return 0;  // 指向文件头页
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

// 清空指定表全部行(须持锁): 沿页链逐页重置为空页
size_t Engine::delete_all_rows(uint64_t table_id)
{
    size_t n = 0;
    const PageId pid0 = PageId{table_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    uint32_t pno = header(h)->next_page;
    pool_.unpin(h);
    while (pno != 0) {
        const PageId pid = PageId{table_id, pno};
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

// 存活行数统计(须持锁): 沿页链数非墓碑槽
size_t Engine::row_count(uint64_t table_id)
{
    size_t n = 0;
    const PageId pid0 = PageId{table_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    uint32_t pno = header(h)->next_page;
    pool_.unpin(h);
    while (pno != 0) {
        const PageId pid = PageId{table_id, pno};
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

Scanner::Scanner(Engine* engine, const TableMeta& meta)
        : engine_(engine), meta_(meta)
{
    const PageId pid0 = PageId{meta_.table_id, 0};
    char* h = engine_->pool_.read(pid0, MAGIC_FILE_HEADER, engine_->files_);
    next_page_no_ = header(h)->next_page;
    engine_->pool_.unpin(h);
}

Scanner::~Scanner() { close(); }

void Scanner::close()
{
    if (cur_data_ != nullptr) {
        engine_->pool_.unpin(cur_data_);
        cur_data_ = nullptr;
    }
    done_ = true;
}

void Scanner::advance_page()
{
    if (cur_data_ != nullptr) {
        engine_->pool_.unpin(cur_data_);
        cur_data_ = nullptr;
    }
    if (next_page_no_ == 0) {
        done_ = true;
        return;
    }
    cur_page_ = PageId{meta_.table_id, next_page_no_};
    cur_data_ = engine_->pool_.read(cur_page_, MAGIC_HEAP, engine_->files_);
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
