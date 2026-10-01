// wal.h: 预写日志(Write-Ahead Log, WAL) —— 崩溃恢复的核心机制
//
// ============ 为什么需要 WAL ============
//
// 缓冲池为了性能会把脏页(被修改过、还没写回磁盘的页)尽量久地留在内存, 等淘汰或
// 显式 flush 才落盘。这带来一个致命问题: 如果进程在脏页落盘之前崩溃, 这些修改
// 就永远丢了——而客户端可能已经收到"插入成功"的回复。
//
// WAL 的思路是: 在修改数据页之前, 先把"修改了什么"以追加方式写进一个独立的
// 日志文件(wal.log), 并在向客户端确认成功之前把日志 fsync 到磁盘。日志记录是
// 顺序小块写入, 远比随机刷 4KB 数据页便宜, 于是下面两件事可以同时成立:
//
//   1. 数据页可以任意延迟刷盘(性能);
//   2. 已确认成功的修改绝不丢失(正确性)——崩溃后重放日志即可重建。
//
// 重放的前提是"写前"(write-ahead)不变量: 任何数据页字节被写到磁盘之前, 描述
// 该修改的日志记录必须已经写入 wal.log。本实现把 diff+记日志挂在
// BufferPool::mark_dirty 里(修改完成即记), 把"记日志先于页落盘"变成代码顺序
// 上必然成立的事实, 详见 buffer_pool.cpp。
//
// ============ 日志记录格式 ============
//
// wal.log 是记录的顺序字节流, 每条记录自带长度与 CRC, 从文件头到尾依次为:
//
//   +---------+---------+-------+---------+----------------------+
//   | len u32 | lsn u64 | op u8 | crc u32 | payload (len 字节)   |
//   +---------+---------+-------+---------+----------------------+
//   |<-- 记录头 17B ------>|<-- CRC 覆盖 lsn+op+payload -------->|
//
//   len : payload 长度(不含记录头), 恢复时据此切分记录边界
//   lsn : 本条记录的日志序列号(Log Sequence Number), 全局单调递增, 从 1 起
//   op  : 操作类型, 见 WalOp
//   crc : CRC32(lsn + op + payload), 检测记录字节损坏
//
// 崩溃可能发生在任意字节处, 日志尾部因此可能是半条记录(长度头都在但 payload
// 不全, 或长度头本身被截断)。恢复时读到"长度不完整"或"CRC 校验失败"即认为
// 日志到此为止, 之后的字节全部丢弃——被丢弃的必然是还没 fsync(即还没向客户端
// 确认)的修改, 丢弃它们是崩溃恢复的正常语义。
//
// ============ 记录种类与 payload 布局 ============
//
//   PagePatch(页补丁, 物理重做):
//     [file_id u64][page_no u32][offset u32][len u32][len 字节的页内容]
//     语义: 把 len 字节"修改后"的内容覆盖到 t_<file_id>.dat 第 page_no 页的
//     offset 处。补丁携带的是 after-image(修改后的字节), 因此重放是幂等的:
//     同一补丁应用多少次结果都一样, 恢复时无须判断"这条是否已经应用过"。
//
//   DropFile(删数据文件):
//     [file_id u64]
//     语义: 删除 t_<file_id>.dat。文件删除无法用"页字节补丁"表达, 所以单独
//     记录; 重放时文件不存在则跳过(幂等)。
//
// 建表不设对应记录: 新文件头页的初始化本身就是 PagePatch, 重放补丁时读写页
// 会经由 FileManager 的 O_CREAT 语义自动把文件建出来。元数据表(db_table 等)
// 的行插删同样是页修改, 也由补丁覆盖, 因此不需要逻辑层的 CREATE/DROP 记录。
//
// ============ 提交与检查点 ============
//
//   提交: 每条 SQL 是一个自动提交事务。session 层在语句执行成功后调用
//   Catalog::sync() -> Wal::sync() fsync 日志, 之后才向客户端回 Ok。fsync
//   返回后, 此前 append 的全部记录掉电不丢, 语句的持久性由此保证。
//
//   检查点(checkpoint): 把全部脏页刷盘并 fsync 数据文件后, 磁盘数据已经完整,
//   wal.log 的历史使命结束, 用 Wal::reset() 清空。这样下次启动恢复的工作量
//   与"距上次干净关闭多久"解耦: 干净关闭后重启零重放。检查点在引擎 close
//   (正常停服)、恢复重放完成后、initdb 目录初始化完成时执行; 运行期由提交点
//   按体量触发——append 累计自上次清空以来的写入字节, Catalog::sync 发现达到
//   配置阈值(wal_checkpoint_bytes, 64KB~1GB)时取锁执行一次检查点, 日志体量
//   因此有界。
//
// ============ 并发约定 ============
//
// append 只在 catalog 全局锁内被调用(所有页修改都经持锁的 engine 原语发生),
// 天然串行; sync 只做 fsync, 允许与 append 并发(fsync 不要求独占), 提前或
// 滞后覆盖某条并发记录都是安全的——多 fsync 无害, 少 fsync 的那条记录所属
// 语句尚未确认。bytes_since_reset_ 做成 atomic: 提交点的阈值判断在锁外读,
// 与持锁的 append/reset 并发。
#ifndef STORAGE_WAL_H
#define STORAGE_WAL_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace st {

// WAL 记录操作类型
enum class WalOp : uint8_t {
    PagePatch = 1,  // 页补丁: after-image 字节覆盖到指定页的指定区间
    DropFile = 2,   // 删数据文件: 重放时删除 t_<file_id>.dat
};

// 记录头长度: [len u32][lsn u64][op u8][crc u32]
constexpr uint32_t WAL_HEADER_SIZE = 17;

// 一条补丁记录 payload 里定位信息的定长前缀: [file_id][page_no][offset][len]
constexpr uint32_t WAL_PATCH_HEADER_SIZE = 20;

// 记录 payload 字段的定长读写: 原生小端序 memcpy, 与页格式(page.cpp)同一约定,
// 补丁组装(buffer_pool.cpp)、记录写入(wal.cpp)与解析(recovery.cpp)共用
inline uint64_t wal_get_u64(const char* p)
{
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline uint32_t wal_get_u32(const char* p)
{
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline void wal_put_u64(char* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); }
inline void wal_put_u32(char* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }

// 预写日志文件: 追加记录 / fsync 提交 / 检查点清空
struct Wal {
    explicit Wal(std::string path);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // 追加一条记录并返回其 LSN。
    // 只 ::write 进 OS 缓冲不落盘, 掉电可能丢——需要持久化时调 sync()。
    uint64_t append(WalOp op, const char* payload, uint32_t len);
    // 提交: fsync 日志文件, 此前 append 的全部记录掉电不丢
    void sync();
    // 检查点收尾: 清空日志文件并回到文件头。
    // 调用前提: 覆盖现存全部记录的数据页已刷盘并 fsync(否则会丢修改)。
    void reset();

    const std::string path_;
    int fd_ = -1;
    uint64_t next_lsn_ = 1;  // 下一条记录的 LSN, reset 不回退(见 .cpp 注释)
    // 自上次清空以来的写入字节数(含记录头), 提交点的运行期检查点阈值判断用;
    // append 持锁累加, reset 清零, sync 锁外读, 故 atomic
    std::atomic<uint64_t> bytes_since_reset_ = 0;
};

// 一条从日志文件解析出的记录(payload 为 op 对应的原始字节)
struct WalRecord {
    uint64_t lsn = 0;
    WalOp op = WalOp::PagePatch;
    std::vector<char> payload;
};

// 从文件顺序读取记录的游标: 供崩溃恢复使用
struct WalReader {
    explicit WalReader(std::string path);
    ~WalReader();

    WalReader(const WalReader&) = delete;
    WalReader& operator=(const WalReader&) = delete;

    // 取下一条合法记录: 读到 EOF/半条记录/CRC 失败返回 false, 之后恒 false。
    // 半条记录与坏记录不报错——它们就是崩溃现场的一部分, 由调用方按截断处理。
    bool next(WalRecord* out);
    // 已成功解析的字节数(全部合法记录的头+payload 之和), 即安全截断点
    uint64_t valid_bytes() const { return valid_bytes_; }

private:
    // 从 fd 继续填充缓冲, 失败/EOF 返回 false
    bool fill();

    int fd_ = -1;
    bool eof_ = false;          // 已读到文件尾(fill 再无新数据)
    std::vector<char> buf_;     // 已读入未消费的字节
    uint64_t valid_bytes_ = 0;  // 已消费的合法字节数
};

}  // namespace st

#endif  // STORAGE_WAL_H
