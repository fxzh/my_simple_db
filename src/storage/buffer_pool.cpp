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

BufferPool::BufferPool(size_t capacity) : frames_(capacity) {}

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
    files.write_page(f.page.file_id, f.page.page_no, f.data);
    f.dirty = false;
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