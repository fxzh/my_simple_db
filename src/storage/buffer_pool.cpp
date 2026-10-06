// buffer_pool.cpp: 页缓存实现
#include "buffer_pool.h"

#include <cstring>
#include <stdexcept>
#include <string>

#include "common/err.h"
#include "log/log.h"
#include "page.h"

namespace st {

namespace {

// 记 ERROR 日志并抛出 DbError, noreturn 供编译期确认调用点终止
[[noreturn]] void raise_error(db::ErrCode code, const std::string& msg)
{
    DB_RAISE(code, LogModule::STORAGE, "{}", msg);
}

}  // namespace

BufferPool::BufferPool(size_t capacity, Wal& wal) : frames_(capacity), wal_(wal) {}

size_t BufferPool::find(PageId page) const
{
    const auto it = page_table_.find(page);
    return it == page_table_.end() ? frames_.size() : it->second;
}

size_t BufferPool::frame_index(char* data) const
{
    // frames_ 定长不重分配, 数据指针差值除以帧大小即下标
    const size_t idx = static_cast<size_t>(data - frames_.front().data) / sizeof(PageFrame);
    if (idx >= frames_.size() || !frames_[idx].valid || frames_[idx].data != data) {
        return frames_.size();
    }
    return idx;
}

void BufferPool::write_back(PageFrame& f, FileManager& files)
{
    if (!f.valid || !f.dirty) {
        return;
    }
    // write-ahead 不变量的运行时兜底: 脏页相对基线还有差异, 说明存在改了页
    // 却没调 mark_dirty 的代码路径, 把这样的页落盘会让无日志的修改外泄,
    // 崩溃后既无法重放也解释不了来源, 只能当场报错
    if (std::memcmp(f.before, f.data, PAGE_SIZE) != 0) {
        DB_CRASH(LogModule::STORAGE, "脏页存在未记 WAL 的修改");
    }
    files.write_page(f.page.file_id, f.page.page_no, f.data);
    f.dirty = false;
}

// 把 f.data 相对 f.before 的差异区间记成 WAL 页补丁, 并把基线推进到当前内容。
// 这是 write-ahead 规则的落点: 所有页修改(堆页/文件头页/B+树页/元数据表页)
// 都以"就地改字节 + mark_dirty"收尾, 在此统一转为补丁记录, 保证任何页字节
// 到达磁盘之前其日志必然已写入。
// diff 以 8 字节字长为粒度: 差异字聚成区间, 相邻区间间隔不超过一个字长时
// 合并以免碎片化; 每个区间一条 OP_PAGE_PATCH, 载荷为区间内的当前字节
// (after)与基线字节(before)双向, before 侧同时追加进事务 undo 列表, 使同一
// 记录兼作 redo 与 undo。补丁只写入 wal.log 的 OS 缓冲, 持久化由事务提交点的
// Wal::sync() 完成, 与页的刷盘时机解耦
void BufferPool::log_page_diff(PageFrame& f)
{
    // memcpy 装载字长: 避免对 char 数组做违反严格别名安全的类型双关
    const auto load = [](const char* p) {
        uint64_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return v;
    };
    constexpr size_t kWord = sizeof(uint64_t);
    const size_t words = PAGE_SIZE / kWord;
    char payload[WAL_PATCH_HEADER_SIZE + 2 * PAGE_SIZE];
    for (size_t w = 0; w < words;) {
        if (load(f.before + w * kWord) == load(f.data + w * kWord)) {
            ++w;
            continue;
        }
        // 差异字区间 [lo, hi); 允许吸收一个字长的间隔再确认是否延续
        const size_t lo = w;
        size_t hi = w + 1;
        ++w;
        while (w < words && w <= hi) {
            if (load(f.before + w * kWord) != load(f.data + w * kWord)) {
                hi = w + 1;
            }
            ++w;
        }
        const uint32_t off = static_cast<uint32_t>(lo * kWord);
        const uint32_t len = static_cast<uint32_t>((hi - lo) * kWord);
        wal_put_u64(payload, f.page.file_id);
        wal_put_u32(payload + 8, f.page.page_no);
        wal_put_u32(payload + 12, off);
        wal_put_u32(payload + 16, len);
        std::memcpy(payload + WAL_PATCH_HEADER_SIZE, f.data + off, len);
        std::memcpy(payload + WAL_PATCH_HEADER_SIZE + len, f.before + off, len);
        wal_.append(WalOp::PagePatch, txn_->txn_id, payload, WAL_PATCH_HEADER_SIZE + 2 * len);
        txn_->undo.push_back(UndoEntry{f.page.file_id, f.page.page_no, off, len,
                                       std::vector<char>(f.before + off, f.before + off + len)});
        txn_->wrote = true;
    }
    // 基线推进: 此后的修改将以当前内容为起点做增量 diff
    std::memcpy(f.before, f.data, PAGE_SIZE);
}

size_t BufferPool::evict(FileManager& files)
{
    // 扫最多两圈: 第一圈清引用位, 第二圈选牺牲帧; 两圈仍无 pin==0 帧才报错
    for (size_t i = 0; i < 2 * frames_.size(); ++i) {
        const size_t idx = (clock_hand_ + i) % frames_.size();
        PageFrame& f = frames_[idx];
        if (f.pin > 0) {
            continue;
        }
        if (f.valid && f.ref) {
            f.ref = false;  // 第二机会: 下次再遇到才淘汰
            continue;
        }
        write_back(f, files);
        if (f.valid) {
            page_table_.erase(f.page);  // 有效帧才登记在页表
        }
        f.valid = false;
        clock_hand_ = (idx + 1) % frames_.size();
        return idx;
    }
    raise_error(db::ErrCode::Internal, "缓冲池无可用帧(全部帧被 pin)");
}

char* BufferPool::read(PageId page, uint32_t expect_magic, FileManager& files)
{
    size_t idx = find(page);
    if (idx != frames_.size()) {
        frames_[idx].ref = true;
        ++frames_[idx].pin;
        return frames_[idx].data;
    }

    idx = evict(files);
    PageFrame& f = frames_[idx];
    f = PageFrame{};
    f.page = page;
    f.valid = true;
    f.ref = true;
    f.pin = 1;
    page_table_.emplace(page, idx);
    files.read_page(page.file_id, page.page_no, f.data);

    if (!page_valid(f.data, expect_magic)) {
        // 损坏页不入缓存: 作废帧并从页表摘除, 后续重读仍会重新校验并报错
        page_table_.erase(page);
        f = PageFrame{};
        if (expect_magic == MAGIC_FILE_HEADER) {
            DB_CRITICAL(LogModule::STORAGE, "文件头页损坏");
        }
        DB_CRITICAL(LogModule::STORAGE, "数据页损坏: fid={} page_no={}", page.file_id, page.page_no);
    }
    // 进池即建 diff 基线, mark_dirty 据此算增量补丁
    std::memcpy(f.before, f.data, PAGE_SIZE);
    return f.data;
}

char* BufferPool::allocate(PageId page, FileManager& files)
{
    size_t idx = find(page);
    if (idx != frames_.size()) {
        ++frames_[idx].pin;
        return frames_[idx].data;
    }
    idx = evict(files);
    PageFrame& f = frames_[idx];
    f = PageFrame{};
    f.page = page;
    f.valid = true;
    f.dirty = true;
    f.ref = true;
    f.pin = 1;
    page_table_.emplace(page, idx);
    std::memset(f.data, 0, PAGE_SIZE);
    // 新页从全零起步, 基线同样置零: 后续 init_page/写入全部落在 diff 里
    std::memcpy(f.before, f.data, PAGE_SIZE);
    return f.data;
}

void BufferPool::unpin(char* data)
{
    const size_t idx = frame_index(data);
    if (idx == frames_.size()) {
        raise_error(db::ErrCode::Internal, "unpin 未命中的页");
    }
    if (frames_[idx].pin > 0) {
        --frames_[idx].pin;
    }
}

void BufferPool::mark_dirty(char* data)
{
    const size_t idx = frame_index(data);
    if (idx == frames_.size()) {
        raise_error(db::ErrCode::Internal, "mark_dirty 未命中的页");
    }
    if (txn_ == nullptr) {
        raise_error(db::ErrCode::Internal, "页修改须在活动事务内");
    }
    // 先把本次修改记成补丁再置脏: 补丁先于任何可能的落盘(write-ahead)
    log_page_diff(frames_[idx]);
    frames_[idx].dirty = true;
}

// 回滚恢复一个字节区间: 页在缓存则直接改帧内容并同步推进基线(不记新补丁,
// 否则回滚会向 WAL 追加本事务新记录形成自反馈); 页不在缓存(事务期间被淘汰
// 落盘)则改磁盘页。帧内/盘上是事务后内容, before 覆写后回到事务前状态
void BufferPool::restore_region(PageId page, uint32_t off, uint32_t len, const char* before,
                                FileManager& files)
{
    const size_t idx = find(page);
    if (idx == frames_.size()) {
        char buf[PAGE_SIZE];
        files.read_page(page.file_id, page.page_no, buf);
        std::memcpy(buf + off, before, len);
        files.write_page(page.file_id, page.page_no, buf);
        return;
    }
    PageFrame& f = frames_[idx];
    std::memcpy(f.data + off, before, len);
    std::memcpy(f.before + off, before, len);
    f.dirty = true;
}

void BufferPool::flush(PageId page, FileManager& files)
{
    const size_t idx = find(page);
    if (idx == frames_.size()) {
        return;
    }
    write_back(frames_[idx], files);
}

void BufferPool::flush_all(FileManager& files)
{
    for (auto& f : frames_) {
        write_back(f, files);
    }
}

void BufferPool::invalidate_all()
{
    for (auto& f : frames_) {
        f = PageFrame{};
    }
    page_table_.clear();
    clock_hand_ = 0;
}

void BufferPool::drop_table(uint64_t file_id, FileManager& files)
{
    for (auto it = page_table_.begin(); it != page_table_.end();) {
        if (it->first.file_id == file_id) {
            write_back(frames_[it->second], files);
            frames_[it->second] = PageFrame{};
            it = page_table_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace st