// buffer_pool.h: 页缓存, Clock 淘汰 + 引用计数保护
#ifndef STORAGE_BUFFER_POOL_H
#define STORAGE_BUFFER_POOL_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>

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

// PageId 哈希: file_id 与 page_no 折叠混合
struct PageIdHash {
    size_t operator()(const PageId& p) const
    {
        uint64_t h = p.file_id;
        h ^= static_cast<uint64_t>(p.page_no) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

// 定长帧缓冲池, 按页号缓存整页, 经哈希页表查找
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
    // 丢弃某文件全部帧(表已删除)
    void drop_table(uint64_t file_id);

    size_t capacity() const { return frames_.size(); }

private:
    size_t find(PageId page) const;
    // 帧数据指针 -> 帧下标, 非本池帧返回 frames_.size()
    size_t frame_index(char* data) const;
    size_t evict(FileManager& files);
    void write_back(PageFrame& f, FileManager& files);

    std::vector<PageFrame> frames_;
    std::unordered_map<PageId, size_t, PageIdHash> page_table_;  // 有效帧的页表
    size_t clock_hand_ = 0;
};

}  // namespace st
#endif