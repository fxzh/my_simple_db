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
    if (txn_) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "检查点不能在活动事务内执行");
    }
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

uint64_t Engine::wal_bytes_since_reset() const
{
    return wal_.bytes_since_reset_.load();
}

// 落盘性原语调用前提校验(须持锁): 无活动事务即报错
void Engine::check_txn(uint64_t fid) const
{
    if (!txn_) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "落盘性原语须在活动事务内调用");
    }
    for (uint64_t pending : txn_->pending_drops) {
        if (pending == fid) {
            DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                     "同事务内已登记删除的文件禁止再修改: fid={}", fid);
        }
    }
}

void Engine::begin_txn()
{
    if (txn_) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "已有活动事务");
    }
    txn_ = std::make_unique<TxnContext>();
    txn_->txn_id = wal_.alloc_txn_id();
    pool_.set_txn(txn_.get());
}

void Engine::commit_txn()
{
    if (!txn_) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "无活动事务");
    }
    pool_.set_txn(nullptr);
    if (txn_->wrote) {
        wal_.append(WalOp::Commit, txn_->txn_id, nullptr, 0);
        // 持久化边界: Commit 记录 fsync 之后事务才算提交, 之后才允许 unlink
        wal_.sync();
    }
    for (uint64_t fid : txn_->pending_drops) {
        files_.remove_table_file(fid);
    }
    if (!txn_->pending_drops.empty()) {
        files_.flush_dir();
    }
    txn_.reset();
}

void Engine::rollback_txn()
{
    if (!txn_) {
        DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "无活动事务");
    }
    pool_.set_txn(nullptr);
    // 逆序恢复: 同页多次修改的 before 链式相依, 正向应用会以后写的 before 覆盖
    // 先写的 before。页在缓存则帧内覆写——上一事务已提交未落盘的修改只存在于
    // 帧内, 不可弃帧; before 是内容覆盖, 幂等
    for (auto it = txn_->undo.rbegin(); it != txn_->undo.rend(); ++it) {
        pool_.restore_region(PageId{it->fid, it->page_no}, it->off, it->len, it->before.data(),
                             files_);
    }
    // Abort 不 fsync: 崩溃丢失时按崩溃中止处理, 恢复期重放 undo 幂等无害
    if (txn_->wrote) {
        wal_.append(WalOp::Abort, txn_->txn_id, nullptr, 0);
    }
    txn_.reset();
}

bool Engine::table_file_exists(uint64_t fid) const
{
    return files_.table_file_exists(fid);
}

// 物理建表(须持锁且在事务内): 建数据文件并初始化落盘文件头页
void Engine::init_table_file(uint64_t fid)
{
    check_txn(fid);
    files_.create_table_file(fid);

    // 初始化并落盘文件头页(mark_dirty 把初始化记成补丁, 落盘前先入 WAL)
    const PageId pid0 = PageId{fid, 0};
    char* h = pool_.allocate(pid0, files_);
    init_page(h, MAGIC_FILE_HEADER, PageType::FileHeader);
    pool_.mark_dirty(h);
    pool_.unpin(h);
    pool_.flush(pid0, files_);
}

// 物理删表(须持锁且在事务内): 记 DropFile、清缓冲与尾页跟踪并登记 pending_drops;
// unlink 延迟到提交后——Commit fsync 之前崩溃则恢复视为未提交(DropFile 跳过,
// 文件从未被删), 之后崩溃则重放补删(幂等)
void Engine::remove_table_file(uint64_t fid)
{
    check_txn();
    char payload[sizeof(uint64_t)];
    wal_put_u64(payload, fid);
    wal_.append(WalOp::DropFile, txn_->txn_id, payload, sizeof(payload));
    txn_->wrote = true;
    pool_.drop_table(fid);
    tail_pages_.erase(fid);
    txn_->pending_drops.push_back(fid);
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

// 插行(须持锁且在事务内): 值合法性由调用方保证, 编码后追加并分配 rowid, ref 输出新行物理位置
RowId Engine::insert_row(uint64_t file_id, const std::vector<ColumnSpec>& cols,
                         const std::vector<Value>& values, RowRef* ref)
{
    check_txn(file_id);
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
                DB_CRITICAL(LogModule::STORAGE, "数据页记录损坏");
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
        DB_CRITICAL(LogModule::STORAGE, "数据页记录损坏");
    }
    out->ref = ref;
    pool_.unpin(pg);
    return true;
}

// 删除单行(须持锁且在事务内): 按物理位置打墓碑, 已删/槽位越界返回 0, 无效引用当场报错
size_t Engine::delete_row(const RowRef& ref)
{
    check_txn(ref.page.file_id);
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

// 清空指定表全部行(须持锁且在事务内): 沿页链收集全部存活槽后逐个墓碑删除。
// 不用"删文件重建"——ftruncate 截掉的旧页字节不在任何 undo 补丁覆盖范围内,
// 事务回滚会永久丢失已提交数据
size_t Engine::delete_all_rows(uint64_t file_id)
{
    check_txn(file_id);
    std::vector<RowRef> refs;
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
                refs.push_back(RowRef{pid, i});
            }
        }
        pno = ph->next_page;
        pool_.unpin(pg);
    }
    for (const RowRef& ref : refs) {
        delete_row(ref);
    }
    return refs.size();
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

// 建索引文件(须持锁且在事务内): 初始化空树, 空叶根落盘
void Engine::init_index_file(uint64_t fid)
{
    check_txn(fid);
    trees_.erase(fid);
    BTree& tree = trees_.try_emplace(fid, pool_, files_, fid).first->second;
    tree.create();
    pool_.flush(PageId{fid, 1}, files_);
}

// 删索引文件(须持锁且在事务内): 记 DropFile、清缓冲与树跟踪并登记 pending_drops,
// unlink 延迟到提交后(同 remove_table_file)
void Engine::remove_index_file(uint64_t fid)
{
    check_txn();
    char payload[sizeof(uint64_t)];
    wal_put_u64(payload, fid);
    wal_.append(WalOp::DropFile, txn_->txn_id, payload, sizeof(payload));
    txn_->wrote = true;
    pool_.drop_table(fid);
    trees_.erase(fid);
    txn_->pending_drops.push_back(fid);
}

// 索引条目插入(须持锁且在事务内): (键, 行定位) 唯一性由调用方保证
void Engine::index_insert(uint64_t fid, const IndexKey& key, const RowRef& ref)
{
    check_txn(fid);
    tree_for(fid).insert(BTreeEntry{key, ref.page.page_no, ref.slot});
}

// 索引范围扫描(须持锁): 迭代器不持锁, 仅持页 pin
std::unique_ptr<BTreeScanner> Engine::index_scan(uint64_t fid, std::optional<IndexKey> lo,
                                                 std::optional<IndexKey> hi)
{
    return std::make_unique<BTreeScanner>(tree_for(fid), std::move(lo), std::move(hi));
}

// 建索引回填(须持锁且在事务内): 全表扫描堆页, 逐行取指定列编码入树, 返回条目数
size_t Engine::build_index(uint64_t table_fid, const std::vector<ColumnSpec>& cols, uint16_t ordinal,
                           uint64_t index_fid)
{
    check_txn(index_fid);
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
                DB_CRITICAL(LogModule::STORAGE, "数据页记录损坏");
            }
            out->ref = RowRef{cur_page_, slot_};
            ++slot_;
            return true;
        }
        advance_page();  // 本页读完, 进入下一页
    }
}

}  // namespace st
