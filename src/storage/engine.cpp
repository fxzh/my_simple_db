// engine.cpp: 文件引擎实现(堆页追加 + 全表扫描 + 删除 + 二级索引)
#include "engine.h"

#include <algorithm>
#include <utility>

#include "codec.h"
#include "common/err.h"
#include "config/config.h"
#include "log/log.h"
#include "page.h"
#include "recovery.h"

namespace st {

// ==================== Engine ====================

Engine::Engine(std::string dir)
        : dir_(std::move(dir)), files_(dir_), wal_(dir_ + "/wal.log"),
          pool_(config::cfg.buffer_pool_frames, wal_) {}

Engine::~Engine()
{
    if (open_) {
        close();
    }
}

void Engine::open()
{
    // 崩溃恢复: 进入运行状态前重放 wal.log 并顺势完成检查点(清空日志),
    // 此时缓冲池仍为空, 重放直接落盘不经过池(见 recovery.cpp 头注释)
    recover(files_, wal_);
    pool_.invalidate_all();
    tail_pages_.clear();
    trees_.clear();
    open_ = true;
}

// 检查点: 三段顺序是 WAL 持久性语义的关键——页字节写入前补丁已记入
// wal.log(mark_dirty 保证), 此处先 fsync 日志、再 fsync 数据文件与目录,
// 保证数据文件持久化的每个字节都有已持久的日志兜底, 掉电后 torn 页也能
// 靠重放修复; 全部落定后清空日志, 重放起点归零
void Engine::checkpoint()
{
    pool_.flush_all(files_);
    wal_.sync();
    files_.flush_all();
    files_.flush_dir();
    wal_.reset();
}

void Engine::close()
{
    checkpoint();
    files_.close_all();
    open_ = false;
}

void Engine::sync_wal()
{
    wal_.sync();
}

bool Engine::table_file_exists(uint64_t fid) const
{
    return files_.table_file_exists(fid);
}

// 物理建表(须持锁): 建数据文件并初始化落盘文件头页
void Engine::init_table_file(uint64_t fid)
{
    files_.create_table_file(fid);

    // 初始化并落盘文件头页(mark_dirty 把初始化记成补丁, 落盘前先入 WAL)
    const PageId pid0 = PageId{fid, 0};
    char* h = pool_.allocate(pid0, files_);
    init_page(h, MAGIC_FILE_HEADER, PageType::FileHeader);
    pool_.mark_dirty(h);
    pool_.unpin(h);
    pool_.flush(pid0, files_);
}

// 物理删表(须持锁): 删数据文件并清缓冲与尾页跟踪
void Engine::remove_table_file(uint64_t fid)
{
    // 删文件记录先入 WAL 并 fsync, 再 unlink: 崩溃时要么记录已持久(重放补删,
    // 幂等), 要么 unlink 未生效, 不出现"文件没了而日志也没有"的半损坏状态
    char payload[sizeof(uint64_t)];
    wal_put_u64(payload, fid);
    wal_.append(WalOp::DropFile, payload, sizeof(payload));
    wal_.sync();
    files_.remove_table_file(fid);
    pool_.drop_table(fid);
    tail_pages_.erase(fid);
}

uint32_t Engine::link_header_to_first_data_page(uint64_t file_id)
{
    // 页号 0 是文件头页, 首个数据页固定为页号 1
    const PageId pid0 = PageId{file_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    const uint32_t new_no = 1;
    char* np = pool_.allocate(PageId{file_id, new_no}, files_);
    init_page(np, MAGIC_HEAP, PageType::Heap);
    pool_.mark_dirty(np);
    pool_.unpin(np);

    PageHeader* hh = header(h);
    hh->next_page = new_no;
    hh->checksum = page_checksum(h);
    pool_.mark_dirty(h);
    pool_.unpin(h);
    return new_no;
}

// 插行(须持锁): 值合法性由调用方保证, 编码后追加并分配 rowid, ref 输出新行物理位置
RowId Engine::insert_row(uint64_t file_id, const std::vector<ColumnSpec>& cols,
                         const std::vector<Value>& values, RowRef* ref)
{
    std::vector<uint8_t> rec;
    if (!encode_row(cols, values, rec)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "值与列类型不匹配");
    }
    if (rec.size() > MAX_RECORD_LEN) {
        DB_RAISE(db::ErrCode::RecordTooLong, LogModule::STORAGE, "记录超长");
    }

    // 文件头页计数器自增分配 rowid
    const PageId pid0 = PageId{file_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    const RowId rid = file_next_rowid(h) + 1;
    set_file_next_rowid(h, rid);
    header(h)->checksum = page_checksum(h);
    pool_.mark_dirty(h);
    pool_.unpin(h);

    // 尾页可能只在内存中(尚未落盘), 用 tail_pages_ 记住最高页号
    uint32_t tail = 0;
    auto it = tail_pages_.find(file_id);
    if (it != tail_pages_.end()) {
        tail = it->second;
    } else if (files_.page_count(file_id) == 1) {
        tail = link_header_to_first_data_page(file_id);
    } else {
        tail = files_.page_count(file_id) - 1;
    }

    for (;;) {
        const PageId pid = PageId{file_id, tail};
        char* pg = pool_.read(pid, MAGIC_HEAP, files_);
        uint16_t slot = 0;
        if (heap_append(pg, rec.data(), static_cast<uint16_t>(rec.size()), &slot)) {
            pool_.mark_dirty(pg);
            pool_.unpin(pg);
            tail_pages_[file_id] = tail;
            *ref = RowRef{pid, slot};
            return rid;
        }
        // 页满: 扩展一个新页并把尾页链上去
        // 新页号取磁盘页数与当前尾页+1 的较大值, 避免与仅存内存中的页冲突
        PageHeader* ph = header(pg);
        const uint32_t new_no = std::max(files_.page_count(file_id), tail + 1);
        char* np = pool_.allocate(PageId{file_id, new_no}, files_);
        init_page(np, MAGIC_HEAP, PageType::Heap);
        pool_.mark_dirty(np);
        pool_.unpin(np);
        ph->next_page = new_no;
        ph->checksum = page_checksum(pg);
        pool_.mark_dirty(pg);
        pool_.unpin(pg);
        tail = new_no;
    }
}

// 读取指定表全部存活行(须持锁): 沿页链解码, 行损坏当场报错
std::vector<std::vector<Value>> Engine::read_rows(uint64_t file_id,
                                                  const std::vector<ColumnSpec>& cols)
{
    std::vector<std::vector<Value>> rows;
    const PageId pid0 = PageId{file_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    uint32_t pno = header(h)->next_page;
    pool_.unpin(h);
    while (pno != 0) {
        const PageId pid = PageId{file_id, pno};
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

// 校验行引用页位置(须持锁): 缺页位置/页 0/页号越界当场报错
void Engine::check_row_ref(const RowRef& ref) const
{
    if (ref.page == INVALID_PAGE) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "行引用缺少页位置");
    }
    if (ref.page.page_no == 0) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "行引用指向文件头页");
    }
    const uint32_t pages = files_.page_count(ref.page.file_id);
    uint32_t max_no = pages == 0 ? 0 : pages - 1;
    auto tail = tail_pages_.find(ref.page.file_id);
    if (tail != tail_pages_.end() && tail->second > max_no) {
        max_no = tail->second;
    }
    if (ref.page.page_no > max_no) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "行引用页号越界: fid={} page_no={}",
                 ref.page.file_id, ref.page.page_no);
    }
}

// 回表(须持锁): 按行物理位置直读堆页取行, 已删/槽位越界返回 false, 无效引用与行损坏当场报错
bool Engine::read_row(const RowRef& ref, const std::vector<ColumnSpec>& cols, Row* out)
{
    check_row_ref(ref);
    char* pg = pool_.read(ref.page, MAGIC_HEAP, files_);
    const PageHeader* ph = header(pg);
    if (ref.slot >= ph->slot_count || slot_tombstone(pg, ref.slot)) {
        pool_.unpin(pg);
        return false;  // 已删或越界
    }
    const Slot* s = slot_at(pg, ref.slot);
    if (!decode_row(cols, record(pg, ref.slot), s->len, out->values)) {
        pool_.unpin(pg);
        DB_RAISE(db::ErrCode::CorruptData, LogModule::STORAGE, "数据页记录损坏");
    }
    out->ref = ref;
    pool_.unpin(pg);
    return true;
}

// 删除单行(须持锁): 按物理位置打墓碑, 已删/槽位越界返回 0, 无效引用当场报错
size_t Engine::delete_row(const RowRef& ref)
{
    check_row_ref(ref);
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

// 清空指定表全部行(须持锁): 统计存活行数后删除并重建表文件
size_t Engine::delete_all_rows(uint64_t file_id)
{
    const size_t n = row_count(file_id);
    remove_table_file(file_id);
    init_table_file(file_id);
    return n;
}

// 存活行数统计(须持锁): 沿页链数非墓碑槽
size_t Engine::row_count(uint64_t file_id)
{
    size_t n = 0;
    const PageId pid0 = PageId{file_id, 0};
    char* h = pool_.read(pid0, MAGIC_FILE_HEADER, files_);
    uint32_t pno = header(h)->next_page;
    pool_.unpin(h);
    while (pno != 0) {
        const PageId pid = PageId{file_id, pno};
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

// ==================== 索引 ====================

// 取指定索引的树(须持锁): 未跟踪时打开已有索引文件
BTree& Engine::tree_for(uint64_t fid)
{
    auto it = trees_.find(fid);
    if (it == trees_.end()) {
        it = trees_.try_emplace(fid, pool_, files_, fid).first;
        it->second.open();
    }
    return it->second;
}

// 建索引文件(须持锁): 初始化空树, 空叶根落盘
void Engine::init_index_file(uint64_t fid)
{
    trees_.erase(fid);
    BTree& tree = trees_.try_emplace(fid, pool_, files_, fid).first->second;
    tree.create();
    pool_.flush(PageId{fid, 1}, files_);
}

// 删索引文件(须持锁): 删文件记录先入 WAL 并 fsync 再删文件(同 remove_table_file)
void Engine::remove_index_file(uint64_t fid)
{
    char payload[sizeof(uint64_t)];
    wal_put_u64(payload, fid);
    wal_.append(WalOp::DropFile, payload, sizeof(payload));
    wal_.sync();
    files_.remove_table_file(fid);
    pool_.drop_table(fid);
    trees_.erase(fid);
}

// 索引条目插入(须持锁): (键, 行定位) 唯一性由调用方保证
void Engine::index_insert(uint64_t fid, const IndexKey& key, const RowRef& ref)
{
    tree_for(fid).insert(BTreeEntry{key, ref.page.page_no, ref.slot});
}

// 索引范围扫描(须持锁): 迭代器不持锁, 仅持页 pin
std::unique_ptr<BTreeScanner> Engine::index_scan(uint64_t fid, std::optional<IndexKey> lo,
                                                 std::optional<IndexKey> hi)
{
    return std::make_unique<BTreeScanner>(tree_for(fid), std::move(lo), std::move(hi));
}

// 建索引回填(须持锁): 全表扫描堆页, 逐行取指定列编码入树, 返回条目数
size_t Engine::build_index(uint64_t table_fid, const std::vector<ColumnSpec>& cols, uint16_t ordinal,
                           uint64_t index_fid)
{
    if (ordinal >= cols.size()) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "索引列序号越界: {}", ordinal);
    }
    BTree& tree = tree_for(index_fid);
    TableMeta meta;
    meta.file_id = table_fid;
    meta.cols = cols;
    size_t n = 0;
    Scanner scanner(this, meta);
    Row row;
    while (scanner.next(&row)) {
        const IndexKey key = encode_key(cols[ordinal].type, row.values[ordinal]);
        tree.insert(BTreeEntry{key, row.ref.page.page_no, row.ref.slot});
        ++n;
    }
    return n;
}

// ==================== Scanner ====================

Scanner::Scanner(Engine* engine, const TableMeta& meta)
        : engine_(engine), meta_(meta)
{
    const PageId pid0 = PageId{meta_.file_id, 0};
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
    cur_page_ = PageId{meta_.file_id, next_page_no_};
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
