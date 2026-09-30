// recovery.cpp: 崩溃恢复实现 —— 顺序重放页补丁, 幂等重建磁盘状态
//
// ============ 恢复算法 ============
//
// 崩溃后磁盘上的状态是"任意的中间态": 缓冲池里没来得及刷盘的修改丢了,
// 刷了一半的页可能 torn(4KB 写入掉电时只落了一部分扇区), wal.log 尾部
// 可能带着半条记录。恢复的目标是把数据库推到"所有已确认语句都生效"的
// 最终状态, 算法因此出奇地简单:
//
//   从头到尾顺序重放每一条合法的 WAL 记录, 不做任何条件判断。
//
// 之所以能这么简单, 依赖补丁记录的两个性质:
//
//   1. 幂等: 补丁携带 after-image(修改后的字节), 重放就是"把这段字节覆盖
//      过去"。一条补丁无论应用 0 次、1 次还是多次, 页的最终内容都一样。
//      所以不需要在页上记 page_lsn 来判断"这条是否已应用过"——崩溃时
//      某页可能已经含某条补丁的效果, 直接再覆盖一遍即可。
//
//   2. 全序: 记录按修改发生的顺序写入, 重放按同序执行, 后写的补丁自然
//      覆盖先写的。同一页的多次修改(如先插行后删行)重放后停在最后一次
//      修改的状态。
//
// torn 页在这里被无害化: 恢复读页不做魔数/校验和检查(检查也无意义——torn
// 页本来就不合法), 直接把补丁字节覆盖上去, 整页重写。只要覆盖该页最后一
// 次修改的补丁存在(它 fsync 过, 因为它所属语句已确认), 页就被完整重建,
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
// 全部记录重放完后, 必须先让重放结果真正持久, 再丢弃日志, 顺序不能反:
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

#include "common/err.h"
#include "log/log.h"
#include "page.h"
#include "types.h"

namespace st {

namespace {

// 记 ERROR 日志并抛 DbError: 用于"CRC 合法但语义非法"的日志内容
[[noreturn]] void raise_corrupt(const std::string& what)
{
    DB_RAISE(db::ErrCode::CorruptData, LogModule::STORAGE, "WAL 记录非法: {}", what);
}

// 应用一条页补丁: 读原页(不校验, 短读补零) -> 覆盖补丁字节 -> 整页写回
void apply_patch(FileManager& files, const WalRecord& rec)
{
    const char* p = rec.payload.data();
    const uint64_t file_id = wal_get_u64(p);
    const uint32_t page_no = wal_get_u32(p + 8);
    const uint32_t offset = wal_get_u32(p + 12);
    const uint32_t len = wal_get_u32(p + 16);

    // 语义校验: CRC 只保证字节没坏, 这里保证字段值合乎约定
    if (file_id == 0) {
        raise_corrupt("补丁 file_id 为 0");
    }
    if (offset > PAGE_SIZE || len == 0 || len > PAGE_SIZE - offset) {
        raise_corrupt("补丁区间越界");
    }
    if (rec.payload.size() != WAL_PATCH_HEADER_SIZE + len) {
        raise_corrupt("补丁长度与 len 字段不符");
    }

    char page[PAGE_SIZE];
    // 目标文件可能尚不存在(建表后尚未刷盘即崩溃): fd_for 的 O_CREAT 会
    // 建出空文件, read_page 短读部分补零, 补丁覆盖后整页写回即完成重建
    files.read_page(file_id, page_no, page);
    std::memcpy(page + offset, p + WAL_PATCH_HEADER_SIZE, len);
    files.write_page(file_id, page_no, page);
}

}  // namespace

RecoveryStats recover(FileManager& files, Wal& wal)
{
    RecoveryStats stats;
    WalReader reader(wal.path_);
    WalRecord rec;
    while (reader.next(&rec)) {
        switch (rec.op) {
        case WalOp::PagePatch:
            apply_patch(files, rec);
            ++stats.patches;
            break;
        case WalOp::DropFile: {
            if (rec.payload.size() != sizeof(uint64_t)) {
                raise_corrupt("删文件记录长度不符");
            }
            const uint64_t file_id = wal_get_u64(rec.payload.data());
            if (file_id == 0) {
                raise_corrupt("删文件记录 file_id 为 0");
            }
            // 幂等: 文件可能已被删除(删除已持久化但日志还在), 不存在即跳过
            if (files.table_file_exists(file_id)) {
                files.remove_table_file(file_id);
                ++stats.drops;
            }
            break;
        }
        default:
            raise_corrupt("未知操作码");
        }
        ++stats.replayed;
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
             "崩溃恢复: 重放 %llu 条记录(页补丁 %llu, 删文件 %llu), 日志有效长度 %llu 字节",
             static_cast<unsigned long long>(stats.replayed),
             static_cast<unsigned long long>(stats.patches),
             static_cast<unsigned long long>(stats.drops),
             static_cast<unsigned long long>(reader.valid_bytes()));

    // 收尾: 重放结果持久化(文件 -> 目录)之后才清空日志, 顺序见文件头注释
    files.flush_all();
    files.flush_dir();
    wal.reset();
    return stats;
}

}  // namespace st
