// buffer_pool.cpp: 页缓存实现
#include "buffer_pool.h"

#include <cstring>
#include <stdexcept>
#include <string>

#include "common/error.h"
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
    for (size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].valid && frames_[i].page == page) {
            return i;
        }
    }
    return frames_.size();
}

void BufferPool::write_back(PageFrame& f, FileManager& files)
{
    if (!f.valid || !f.dirty) {
        return;
    }
    files.write_page(page_table_id(f.page), page_no(f.page), f.data);
    f.dirty = false;
}

size_t BufferPool::evict(FileManager& files)
{
    for (size_t i = 0; i < frames_.size(); ++i) {
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
    files.read_page(page_table_id(page), page_no(page), f.data);

    if (!page_valid(f.data, expect_magic)) {
        if (expect_magic == MAGIC_FILE_HEADER) {
            raise_error(db::ErrCode::CorruptData, "文件头页损坏");
        }
        // 数据页损坏: 按追加截断处理, 重建空页
        LOG_WARNING(LogModule::STORAGE, "检测到损坏数据页, 按空页重建: table=%u page=%u",
                    page_table_id(page), page_no(page));
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
    std::memset(f.data, 0, PAGE_SIZE);
    return f.data;
}

void BufferPool::unpin(char* data)
{
    for (auto& f : frames_) {
        if (f.valid && f.data == data) {
            if (f.pin > 0) {
                --f.pin;
            }
            return;
        }
    }
    raise_error(db::ErrCode::Internal, "unpin 未命中的页");
}

void BufferPool::mark_dirty(char* data)
{
    for (auto& f : frames_) {
        if (f.valid && f.data == data) {
            f.dirty = true;
            return;
        }
    }
    raise_error(db::ErrCode::Internal, "mark_dirty 未命中的页");
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
    clock_hand_ = 0;
}

void BufferPool::drop_table(uint32_t table_id)
{
    for (auto& f : frames_) {
        if (f.valid && page_table_id(f.page) == table_id) {
            f = PageFrame{};
        }
    }
}

}  // namespace st