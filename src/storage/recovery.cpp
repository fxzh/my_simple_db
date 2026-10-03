// recovery.cpp: 崩溃恢复实现 —— 三遍: 收集 / 重放已提交 / 撤销崩溃中止
//
// ============ 恢复算法 ============
//
// 崩溃后磁盘上的状态是"任意的中间态": 缓冲池里没来得及刷盘的修改丢了,
// 刷了一半的页可能 torn(4KB 写入掉电时只落了一部分扇区), wal.log 尾部
// 可能带着半条记录。缓冲池允许把未提交事务的脏页淘汰落盘(steal), 因此
// 磁盘上还可能残留未提交修改的字节。恢复的目标是把数据库推到"所有已提交
// 事务都生效、未提交事务无残留"的状态, 分三遍:
//
//   1. 收集: 全部合法记录读入内存, 按记录头事务号建立 committed(出现过
//      Commit)与 aborted(出现过 Abort)两个集合;
//   2. 重放: 按原顺序应用 committed 事务的补丁 after 半段与 DropFile;
//   3. 撤销: 按 LSN 逆序应用"无 Commit 且无 Abort"事务的补丁 before 半段,
//      清除崩溃中止事务残留磁盘的未提交字节。有 Abort 的事务跳过——其磁盘
//      已在运行期回滚时复原, 重放 before 反而会覆盖其后已提交事务的字节
//      (如同表插入改同一 slot_count), 破坏持久性。
//
// 幂等性依旧成立: 补丁是内容覆盖, 应用 0/1/N 次结果一致, 无须 page_lsn。
// 事务粒度串行保证 WAL 中不同事务的记录不交错, 崩溃中止事务必是日志中
// 最后一个, 其 before 即此前全部已提交状态。Abort 未 fsync 而崩溃丢失时,
// 该事务按崩溃中止处理, undo 幂等重放无副作用。
//
// torn 页在这里被无害化: 恢复读页不做魔数/校验和检查(检查也无意义——torn
// 页本来就不合法), 直接把补丁字节覆盖上去, 整页重写。只要覆盖该页最后一
// 次修改的补丁存在(它 fsync 过, 因为它所属事务已提交), 页就被完整重建,
// 校验和字段也随补丁字节一起恢复正确。
//
// ============ 恢复路径为什么绕开缓冲池 ============
//
// 重放直接走 FileManager 的 pread/pwrite, 不进缓冲池: 缓冲池的写入路径
// (mark_dirty)会往 WAL 里记新补丁, 恢复期间重放"产生"补丁会形成自反馈;
// 绕开后恢复是一个纯函数式的"日志 -> 磁盘"过程, 结束时缓冲池仍为空,
// 与新启动的进程无异。
//
// ============ 收尾顺序 ============
//
// 全部记录处理完后, 必须先让恢复结果真正持久, 再丢弃日志, 顺序不能反:
//   1. files.flush_all()   —— fsync 全部数据文件, 补丁效果落盘;
//   2. fsync 数据目录       —— 补丁可能新建了数据文件, 目录项(文件名)的
//                              持久化需要单独 fsync 目录;
//   3. wal.reset()          —— 清空日志(相当于完成一次检查点)。
// 若在 1 之前清空日志而随后掉电, 重放结果尚未持久, 日志又没了, 修改就真的
// 丢了。这个顺序与 Engine::close 的检查点收尾完全一致。
#include "recovery.h"

#include <sys/stat.h>

#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/err.h"
#include "log/log.h"
#include "page.h"
#include "types.h"

namespace st {

namespace {

// 记 CRITICAL 日志并退出进程: 用于"CRC 合法但语义非法"的日志内容
[[noreturn]] void raise_corrupt(const std::string& what)
{
    DB_CRITICAL(LogModule::STORAGE, "WAL 记录非法: {}", what);
}

// 补丁记录语义校验: 定位字段与双向载荷长度合乎约定
void validate_patch(const WalRecord& rec)
{
    const char* p = rec.payload.data();
    const uint64_t file_id = wal_get_u64(p);
    const uint32_t offset = wal_get_u32(p + 12);
    const uint32_t len = wal_get_u32(p + 16);

    if (file_id == 0) {
        raise_corrupt("补丁 file_id 为 0");
    }
    if (offset > PAGE_SIZE || len == 0 || len > PAGE_SIZE - offset) {
        raise_corrupt("补丁区间越界");
    }
    if (rec.payload.size() != WAL_PATCH_HEADER_SIZE + 2 * static_cast<size_t>(len)) {
        raise_corrupt("补丁长度与 len 字段不符");
    }
}

// 删文件记录语义校验
void validate_drop(const WalRecord& rec)
{
    if (rec.payload.size() != sizeof(uint64_t)) {
        raise_corrupt("删文件记录长度不符");
    }
    if (wal_get_u64(rec.payload.data()) == 0) {
        raise_corrupt("删文件记录 file_id 为 0");
    }
}

// 应用一条页补丁: 读原页(不校验, 短读补零) -> 覆盖指定半段字节 -> 整页写回。
// undo 为真覆盖 before 半段(撤销), 否则覆盖 after 半段(重做)
void apply_patch(FileManager& files, const WalRecord& rec, bool undo)
{
    const char* p = rec.payload.data();
    const uint64_t file_id = wal_get_u64(p);
    const uint32_t page_no = wal_get_u32(p + 8);
    const uint32_t offset = wal_get_u32(p + 12);
    const uint32_t len = wal_get_u32(p + 16);

    char page[PAGE_SIZE];
    // 目标文件可能尚不存在(建表后尚未刷盘即崩溃): fd_for 的 O_CREAT 会
    // 建出空文件, read_page 短读部分补零, 补丁覆盖后整页写回即完成重建
    files.read_page(file_id, page_no, page);
    const char* bytes = p + WAL_PATCH_HEADER_SIZE + (undo ? len : 0);
    std::memcpy(page + offset, bytes, len);
    files.write_page(file_id, page_no, page);
}

}  // namespace

RecoveryStats recover(FileManager& files, Wal& wal)
{
    RecoveryStats stats;
    WalReader reader(wal.path_);

    // 第一遍(收集): 全部合法记录进内存, 建立 committed/aborted 集合
    std::vector<WalRecord> records;
    std::unordered_set<uint64_t> committed;
    std::unordered_set<uint64_t> aborted;
    uint64_t commit_abort_count = 0;
    WalRecord rec;
    while (reader.next(&rec)) {
        switch (rec.op) {
        case WalOp::PagePatch:
            validate_patch(rec);
            records.push_back(std::move(rec));
            break;
        case WalOp::DropFile:
            validate_drop(rec);
            records.push_back(std::move(rec));
            break;
        case WalOp::Commit:
        case WalOp::Abort:
            if (!rec.payload.empty()) {
                raise_corrupt("提交/中止记录带载荷");
            }
            (rec.op == WalOp::Commit ? committed : aborted).insert(rec.txn);
            ++commit_abort_count;
            break;
        default:
            raise_corrupt("未知操作码");
        }
    }
    stats.replayed = records.size() + commit_abort_count;

    // 第二遍(重放): 按原顺序应用已提交事务的补丁 after 与删文件
    for (const WalRecord& r : records) {
        if (committed.count(r.txn) == 0) {
            continue;
        }
        if (r.op == WalOp::PagePatch) {
            apply_patch(files, r, false);
            ++stats.patches;
        } else {
            // 幂等: 文件可能已被删除(删除已持久化但日志还在), 不存在即跳过
            const uint64_t file_id = wal_get_u64(r.payload.data());
            if (files.table_file_exists(file_id)) {
                files.remove_table_file(file_id);
                ++stats.drops;
            }
        }
    }

    // 第三遍(撤销): 逆序应用崩溃中止事务的补丁 before; 有 Abort 的事务磁盘
    // 已在运行期回滚时复原, 跳过; 未提交的 DropFile 跳过(文件从未被删)
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (committed.count(it->txn) != 0 || aborted.count(it->txn) != 0 || it->op != WalOp::PagePatch) {
            continue;
        }
        apply_patch(files, *it, true);
        ++stats.undone;
    }

    if (stats.replayed == 0) {
        // 文件非空却一条都解析不出(纯垃圾头): 只可能来自日志被外部破坏,
        // 可解析记录为 0 即无已确认数据可保, 告警后清掉, 避免坏头永远挡住
        // 后续正常追加的记录
        struct stat st {};
        if (::stat(wal.path_.c_str(), &st) == 0 && st.st_size > 0) {
            LOG_WARNING(LogModule::STORAGE, "wal.log 无任何合法记录(长度 %lld 字节), 予以清空",
                        static_cast<long long>(st.st_size));
            wal.reset();
        }
        return stats;  // 无日志需恢复
    }
    LOG_INFO(LogModule::STORAGE,
             "崩溃恢复: 读到 %llu 条记录, 重放已提交(页补丁 %llu, 删文件 %llu), 撤销未提交补丁 %llu, 日志有效长度 %llu 字节",
             static_cast<unsigned long long>(stats.replayed),
             static_cast<unsigned long long>(stats.patches),
             static_cast<unsigned long long>(stats.drops),
             static_cast<unsigned long long>(stats.undone),
             static_cast<unsigned long long>(reader.valid_bytes()));

    // 收尾: 恢复结果持久化(文件 -> 目录)之后才清空日志, 顺序见文件头注释
    files.flush_all();
    files.flush_dir();
    wal.reset();
    return stats;
}

}  // namespace st
