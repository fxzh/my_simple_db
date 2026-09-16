// process.cpp: 子进程运行(双管道捕获 + 超时 SIGKILL + 环境净化)
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/process.hpp"

namespace tcommon {

bool run_process(const std::vector<std::string>& argv, int timeout_ms, ProcessResult& result,
                 std::string& error)
{
    if (argv.empty()) {
        error = "argv 为空";
        return false;
    }
    if (timeout_ms <= 0) {
        error = "超时须为正数";
        return false;
    }

    int out_pipe[2];
    int err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        error = std::string("创建 stdout 管道失败: ") + std::strerror(errno);
        return false;
    }
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        error = std::string("创建 stderr 管道失败: ") + std::strerror(errno);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }

    // 环境净化: 子进程只带 locale 定值
    std::vector<std::string> env_storage = {"LC_ALL=C", "LANG=C"};
    std::vector<char*> envp;
    envp.reserve(env_storage.size() + 1);
    for (std::string& s : env_storage) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    std::vector<std::string> argv_storage(argv);
    std::vector<char*> child_argv;
    child_argv.reserve(argv_storage.size() + 1);
    for (std::string& s : argv_storage) {
        child_argv.push_back(s.data());
    }
    child_argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        error = std::string("fork 失败: ") + std::strerror(errno);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return false;
    }
    if (pid == 0) {
        // 子进程: 重定向后 exec, dup2 副本不带 CLOEXEC, exec 失败以 127 退出
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        execve(child_argv[0], child_argv.data(), envp.data());
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);

    // 轮询读取两端管道直到 EOF, 超时先 SIGKILL 再读完剩余数据
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool killed = false;
    bool out_open = true;
    bool err_open = true;
    char buf[4096];
    while (out_open || err_open) {
        if (!killed && std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            killed = true;
        }
        struct pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
        int nready = poll(fds, 2, 50);
        if (nready < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("poll 失败: ") + std::strerror(errno);
            close(out_pipe[0]);
            close(err_pipe[0]);
            kill(pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
            }
            return false;
        }
        if (out_open && (fds[0].revents & (POLLIN | POLLHUP)) != 0) {
            ssize_t n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                result.out.append(buf, static_cast<size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                out_open = false;
            }
        }
        if (err_open && (fds[1].revents & (POLLIN | POLLHUP)) != 0) {
            ssize_t n = read(err_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                result.err.append(buf, static_cast<size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                err_open = false;
            }
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);

    // 收尸: SIGKILL 已发出时进程必然终止
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            break;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("waitpid 失败: ") + std::strerror(errno);
            return false;
        }
        if (!killed && std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            killed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
    result.timed_out = killed;
    return true;
}

}
