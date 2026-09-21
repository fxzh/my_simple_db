// buffer_pool.h: 页缓存, Clock 淘汰 + 引用计数保护
#ifndef STORAGE_BUFFER_POOL_H
#define STORAGE_BUFFER_POOL_H

#include <cstdint>
#include <cstddef>
#include <vector>

#include "file_manager.h"
#include "types.h"

namespace st {

// 页在内存中的缓存帧: 数据 + 状态位
struct PageFrame {
    PageId page = INVALID_PAGE;
    bool valid = false;
    bool dirty = false;
    bool ref = false;    // Clock 引用位
    uint16_t pin = 0;    // 被使用者持有的帧数, 0 才可被淘汰
    char data[PAGE_SIZE];
};

// 定长帧缓冲池, 按页号缓存整页
// 并发由上层(Storage)的全局互斥锁保证, 内部不加锁
class BufferPool {
public:
    static constexpr size_t kDefaultCapacity = 128;

    explicit BufferPool(size_t capacity = kDefaultCapacity);
    ~BufferPool() = default;

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // 读页并 pin; 页校验失败时:
    //   - 文件头页: 抛异常(不可重建)
    //   - 数据页: 视为尾部截断, 重建为空页并标记脏
    char* read(PageId page, uint32_t expect_magic, FileManager& files);

    // 分配一个新页(清零, 标记脏, pin), 不落盘
    char* allocate(PageId page, FileManager& files);

    // 释放一次引用, 计数归 0 后可被淘汰
    void unpin(char* data);

    // 标记整页为脏(调用方就地改过页内容后调用)
    void mark_dirty(char* data);

    // 立即把脏页写回文件
    void flush(PageId page, FileManager& files);
    // 全部脏页写回
    void flush_all(FileManager& files);

    // 丢弃全部缓存帧(不写回, 用于重开/切换数据目录)
    void invalidate_all();
    // 丢弃某表全部帧(表已删除)
    void drop_table(uint64_t table_id);

    size_t capacity() const { return frames_.size(); }

private:
    size_t find(PageId page) const;
    size_t evict(FileManager& files);
    void write_back(PageFrame& f, FileManager& files);

    std::vector<PageFrame> frames_;
    size_t clock_hand_ = 0;
};

}  // namespace st
#endif