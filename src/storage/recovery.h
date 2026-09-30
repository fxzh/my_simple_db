// recovery.h: 启动期崩溃恢复 —— 重放 WAL 重建磁盘状态
#ifndef STORAGE_RECOVERY_H
#define STORAGE_RECOVERY_H

#include <cstdint>

#include "file_manager.h"
#include "wal.h"

namespace st {

// 恢复结果统计(记入日志, 供运维观察恢复成本)
struct RecoveryStats {
    uint64_t replayed = 0;  // 重放的记录总数
    uint64_t patches = 0;   // 其中页补丁数
    uint64_t drops = 0;     // 其中删文件数
};

// 崩溃恢复入口: 顺序重放 wal.log 全部合法记录到数据文件, 完成后清空日志。
// 由 Engine::open 在进入运行状态前调用一次。wal.log 不存在或无合法记录时
// 是空操作(干净关闭后的正常启动路径)。记录语义非法(区间越界/长度不符等)
// 当场报错——CRC 通过但内容不合约定, 属日志文件被外部破坏, 不能静默跳过。
RecoveryStats recover(FileManager& files, Wal& wal);

}  // namespace st

#endif  // STORAGE_RECOVERY_H
