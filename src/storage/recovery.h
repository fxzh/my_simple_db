// recovery.h: 启动期崩溃恢复 —— 重放 WAL 重建磁盘状态
#ifndef STORAGE_RECOVERY_H
#define STORAGE_RECOVERY_H

#include <cstdint>

#include "file_manager.h"
#include "wal.h"

namespace st {

// 恢复结果统计(记入日志, 供运维观察恢复成本)
struct RecoveryStats {
    uint64_t replayed = 0;  // 读到的合法记录总数
    uint64_t patches = 0;   // 其中重放的页补丁数(已提交事务)
    uint64_t drops = 0;     // 其中重放的删文件数(已提交事务)
    uint64_t undone = 0;    // 撤销的页补丁数(崩溃中止事务)
};

// 崩溃恢复入口: 三遍处理 wal.log——收集(建 committed/aborted 集合)→按序重放已提交
// 事务的补丁 after 与 DropFile →逆序撤销崩溃中止事务的补丁 before, 完成后清空日志。
// 由 Engine::open 在进入运行状态前调用一次。wal.log 不存在或无合法记录时
// 是空操作(干净关闭后的正常启动路径)。记录语义非法(区间越界/长度不符等)
// 当场报错——CRC 通过但内容不合约定, 属日志文件被外部破坏, 不能静默跳过。
RecoveryStats recover(FileManager& files, Wal& wal);

}  // namespace st

#endif  // STORAGE_RECOVERY_H
