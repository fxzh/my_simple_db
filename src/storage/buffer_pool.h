// buffer_pool.h: 页缓存, Clock 淘汰 + 引用计数保护
#ifndef STORAGE_BUFFER_POOL_H
#define STORAGE_BUFFER_POOL_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>

#include "file_manager.h"
#include "types.h"
#include "wal.h"

namespace st {

// 页在内存中的缓存帧: 数据 + 状态位
struct PageFrame {
    PageId page = INVALID_PAGE;
    bool valid = false;
    bool dirty = false;
    bool ref = false;    // Clock 引用位
    uint16_t pin = 0;    // 被使用者持有的帧数, 0 才可被淘汰
    char data[PAGE_SIZE];
    // WAL diff 基线: 页内容最近一次与"已记日志状态"一致时的快照, mark_dirty
    // 据此算出补丁。只在 data 与磁盘/WAL 一致的时刻更新——页新进池、
    // write_back 落盘后、mark_dirty 记完补丁后; read 命中已缓存帧时不动,
    // 否则两次 pin 之间的修改会从 diff 里漏掉(详见 wal.h 头注释)
    char before[PAGE_SIZE];
};

// undo 条目: 一段补丁的 before 侧, 回滚时逆序覆写回磁盘
struct UndoEntry {
    uint64_t fid;
    uint32_t page_no;
    uint32_t off;
    uint32_t len;
    std::vector<char> before;
};

// 尾页跟踪回滚条目: tail_pages_ 写入前的旧值, had 为 false 表示原先无记录
struct TailUndo {
    uint64_t fid;
    bool had;
    uint32_t old_tail;
};

// 活动事务上下文: Engine 持有并注入缓冲池; log_page_diff 产出的补丁把 before 侧
// 追加进 undo, 延迟 unlink 的 fid 登记进 pending_drops, 尾页跟踪的旧值登记进 tail_undo
struct TxnContext {
    uint64_t txn_id = 0;                  // Wal 分配的事务号
    bool wrote = false;                   // 是否已产生 WAL 记录
    std::vector<UndoEntry> undo;          // 回滚时逆序回放
    std::vector<TailUndo> tail_undo;      // 尾页跟踪旧值, 回滚时逆序恢复
    std::vector<uint64_t> pending_drops;  // 已记 DropFile、待提交后 unlink 的 fid
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
    explicit BufferPool(size_t capacity, Wal& wal);
    ~BufferPool() = default;

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // 读页并 pin; 页校验失败(魔数/校验和错)时抛异常, 损坏页不入缓存
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

    // 设置活动事务上下文(Engine 在事务开始/结束时切换), null 表示无活动事务
    void set_txn(TxnContext* txn) { txn_ = txn; }
    // 回滚恢复一个字节区间: 页在缓存则直接覆写帧内容并同步推进帧内基线、置脏
    // (不记新补丁); 页不在缓存(事务期间被淘汰落盘)则改磁盘页
    void restore_region(PageId page, uint32_t off, uint32_t len, const char* before,
                        FileManager& files);

    // 丢弃全部缓存帧(不写回, 用于重开/切换数据目录)
    void invalidate_all();
    // 写回脏帧后丢弃某文件全部帧(表已删除)
    void drop_table(uint64_t file_id, FileManager& files);

    size_t capacity() const { return frames_.size(); }

private:
    size_t find(PageId page) const;
    // 帧数据指针 -> 帧下标, 非本池帧返回 frames_.size()
    size_t frame_index(char* data) const;
    size_t evict(FileManager& files);
    void write_back(PageFrame& f, FileManager& files);
    // 把 f.data 相对 f.before 的差异区间记成 WAL 页补丁, 并把基线推进到当前内容
    void log_page_diff(PageFrame& f);

    std::vector<PageFrame> frames_;
    std::unordered_map<PageId, size_t, PageIdHash> page_table_;  // 有效帧的页表
    size_t clock_hand_ = 0;
    Wal& wal_;               // 预写日志, 脏页修改先记日志后落盘, Engine 构造时注入
    TxnContext* txn_ = nullptr;  // 活动事务, 补丁记入 WAL 同时收集 undo
};

}  // namespace st
#endif