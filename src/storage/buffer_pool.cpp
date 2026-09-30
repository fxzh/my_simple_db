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
        raise_error(db::ErrCode::Internal, "脏页存在未记 WAL 的修改");
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
// (after-image)。补丁只写入 wal.log 的 OS 缓冲, 持久化由语句提交点的
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
    char payload[WAL_PATCH_HEADER_SIZE + PAGE_SIZE];
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
        wal_.append(WalOp::PagePatch, payload, WAL_PATCH_HEADER_SIZE + len);
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
        if (expect_magic == MAGIC_FILE_HEADER) {
            raise_error(db::ErrCode::CorruptData, "文件头页损坏");
        }
        // 数据页损坏: 按追加截断处理, 重建空页
        LOG_WARNING(LogModule::STORAGE, "检测到损坏数据页, 按空页重建: file=%llu page=%u",
                    static_cast<unsigned long long>(page.file_id), page.page_no);
        init_page(f.data, MAGIC_HEAP, PageType::Heap);
        f.dirty = true;
    }
    // 进池即建 diff 基线(含上方损坏重建后的内容): 重建视为修复动作本身
    // 定义了基线, 其效果不单独记补丁, 历史 WAL 里该页的补丁重放时照常覆盖
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
    // 先把本次修改记成补丁再置脏: 补丁先于任何可能的落盘(write-ahead)
    log_page_diff(frames_[idx]);
    frames_[idx].dirty = true;
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

void BufferPool::drop_table(uint64_t file_id)
{
    for (auto it = page_table_.begin(); it != page_table_.end();) {
        if (it->first.file_id == file_id) {
            frames_[it->second] = PageFrame{};
            it = page_table_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace st