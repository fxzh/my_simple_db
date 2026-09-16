#ifndef TEST_COMMON_PROCESS_H
#define TEST_COMMON_PROCESS_H

#include <string>
#include <vector>

namespace tcommon {

// 子进程运行结果
struct ProcessResult {
    int exit_code = -1;      // 正常退出的退出码; 信号终止时为 -1; exec 失败为 127
    bool timed_out = false;  // true 表示超时被 SIGKILL
    std::string out;         // stdout 全文
    std::string err;         // stderr 全文
};

// 运行子进程直至退出:
// - 双管道捕获 stdout/stderr 全文, 输出超过管道缓冲也不会死锁
// - 超过 timeout_ms 未退出则 SIGKILL, result.timed_out 置位
// - 子进程环境仅含 LC_ALL=C/LANG=C, 不继承 locale/HOME 等
// - argv[0] 须为可执行文件绝对路径
// 返回 false 仅表示基建自身故障(fork/poll/waitpid 失败);
// 被测进程的退出码与输出一律由调用方对 result 断言
bool run_process(const std::vector<std::string>& argv, int timeout_ms, ProcessResult& result,
                 std::string& error);

}

#endif
