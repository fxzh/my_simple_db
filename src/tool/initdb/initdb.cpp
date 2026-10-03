// initdb: 初始化工具, 在 -D 指定的数据目录内生成默认配置文件 db.conf 与元数据表 db_table/db_column/db_schema,
// 再拉起同目录 server --bootstrap, 经 client 执行伴生 bootstrap.sql 建系统表后经控制通道关闭
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log/log.h"
#include "config/config.h"
#include "catalog.h"
#include "utils/utils.h"

namespace {

// 默认配置文件内容
constexpr char kDefaultConf[] = R"(# my_simple_db 服务端配置
# 语法: 一行一项 key = value, '#' 之后为注释
# port           监听端口, 1~65535
# control_socket 控制通道 socket 路径, 不配置时为数据目录/server.sock
# buffer_pool_frames 缓冲池帧数, 16~1048576, 不配置时为 8192
# wal_checkpoint_bytes 运行期检查点阈值(字节), 65536~1073741824, 不配置时为 16777216(16MB)
# server_log_level 服务端日志级别, debug5~critical 之一(大小写不敏感), 不配置时为 info

port = 8123
)";

// initdb 失败时日志的保留路径
constexpr char kFailedLogPath[] = "/tmp/simple.log";

// bootstrap 阶段各等待上限(秒): 端口行读取 / server 退出
constexpr int kPortLineTimeoutSec = 10;
constexpr int kServerStopTimeoutSec = 10;

// 在指定路径写入默认配置文件, 已存在或写入失败返回 false 并填充错误描述
bool write_default_conf(const std::string& path, std::string& error)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        error = "配置文件已存在: " + path;
        return false;
    }
    if (ec) {
        error = "无法访问路径: " + path;
        return false;
    }
    std::ofstream out(path);
    if (!out.is_open()) {
        error = "无法创建配置文件: " + path;
        return false;
    }
    out << kDefaultConf;
    out.close();
    if (out.fail()) {
        std::filesystem::remove(path, ec);  // 清理写坏的残留文件
        error = "写入配置文件失败: " + path;
        return false;
    }
    return true;
}

// 数据目录就绪检查: 不存在则创建(created 置真), 路径不可用或目录非空返回 false 并填充错误描述
bool prepare_data_dir(const std::string& dir, bool& created, std::string& error)
{
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        if (!std::filesystem::is_directory(dir, ec)) {
            error = "路径已存在且不是目录: " + dir;
            return false;
        }
        if (!std::filesystem::is_empty(dir, ec)) {
            error = "目录非空: " + dir;
            return false;
        }
        return true;
    }
    if (ec) {
        error = "无法访问路径: " + dir;
        return false;
    }
    std::filesystem::create_directories(dir, ec);
    if (ec || !std::filesystem::is_directory(dir, ec)) {
        error = "无法创建数据目录: " + dir;
        return false;
    }
    created = true;
    return true;
}

// 从管道读 server 输出, 匹配端口行 bootstrap_port=<端口>; EOF/超时/格式非法返回 false 并填充错误描述
bool read_port_line(int fd, int& port, std::string& error)
{
    constexpr std::string_view kPrefix = "bootstrap_port=";
    std::string buf;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kPortLineTimeoutSec);
    for (;;) {
        size_t nl = buf.find('\n');
        while (nl != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.starts_with(kPrefix)) {
                std::string_view num(line.data() + kPrefix.size(), line.size() - kPrefix.size());
                int value = 0;
                auto res = std::from_chars(num.data(), num.data() + num.size(), value);
                if (res.ec != std::errc() || res.ptr != num.data() + num.size() || value <= 0) {
                    error = "端口行格式非法: " + line;
                    return false;
                }
                port = value;
                return true;
            }
            nl = buf.find('\n');
        }
        auto remain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
        if (remain_ms <= 0) {
            error = "等待 bootstrap 端口行超时";
            return false;
        }
        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pret = poll(&pfd, 1, static_cast<int>(remain_ms));
        if (pret < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "poll 失败";
            return false;
        }
        if (pret == 0) {
            error = "等待 bootstrap 端口行超时";
            return false;
        }
        char chunk[256];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            error = "读取 server 输出失败";
            return false;
        }
        if (n == 0) {
            error = "server 启动失败(进程提前退出, 详细错误见控制台)";
            return false;
        }
        buf.append(chunk, static_cast<size_t>(n));
    }
}

// 等待子进程退出(带超时), 已退出返回 true 并写入 status
bool wait_child(pid_t pid, int& status, std::string& error)
{
    auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(kServerStopTimeoutSec);
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            return true;
        }
        if (r < 0) {
            error = "waitpid 失败";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    error = "等待 server 退出超时";
    return false;
}

// 拉起 client 执行 bootstrap.sql: 输出直透当前终端, 退出码非零即有失败语句
bool run_client_file(const std::string& client_bin, int port, const std::string& sql_path,
                     std::string& error)
{
    pid_t pid = fork();
    if (pid < 0) {
        error = "fork 失败";
        return false;
    }
    if (pid == 0) {
        const std::string port_str = std::to_string(port);
        execl(client_bin.c_str(), "client", "-p", port_str.c_str(), "-f", sql_path.c_str(),
              static_cast<char*>(nullptr));
        std::cerr << "启动 client 失败: " << strerror(errno) << std::endl;
        _exit(1);
    }
    int status = 0;
    if (!wait_child(pid, status, error)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "client 执行 bootstrap.sql 失败";
        return false;
    }
    return true;
}

// 失败路径收尾: 尽力经控制通道关闭, 仍不退出则 SIGKILL, 保证收尸
void best_effort_stop(pid_t pid, const std::string& sock_path)
{
    std::string ignore;
    utils::control_shutdown(sock_path, ignore);  // server 可能已退出或未就绪, 失败不报
    int status = 0;
    if (!wait_child(pid, status, ignore)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

// bootstrap 阶段: 拉起同目录 server --bootstrap(前台, stdout 接管道), 读到端口行后
// 经 client 执行伴生 bootstrap.sql, 再经控制通道关闭并等 server 退出; 任一步失败时已尽力收尾子进程
bool run_bootstrap_stage(const std::string& dir, std::string& error)
{
    std::string server_bin;
    std::string client_bin;
    std::string bootstrap_sql;
    if (!utils::companion_path("server", server_bin, error)
        || !utils::companion_path("client", client_bin, error)
        || !utils::companion_path("bootstrap.sql", bootstrap_sql, error)) {
        return false;
    }
    std::string ctl_sock = (std::filesystem::path(dir) / "server.sock").string();

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        error = "创建管道失败";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        error = "fork 失败";
        return false;
    }
    if (pid == 0) {
        // 子进程: stdout 接管道, stderr 保留透传启动期报错
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[1]);
        execl(server_bin.c_str(), "server", "-D", dir.c_str(), "--bootstrap",
              static_cast<char*>(nullptr));
        std::cerr << "启动 server 失败: " << strerror(errno) << std::endl;
        _exit(1);
    }
    close(pipe_fds[1]);

    int port = 0;
    if (!read_port_line(pipe_fds[0], port, error)) {
        best_effort_stop(pid, ctl_sock);
        close(pipe_fds[0]);
        return false;
    }
    if (!run_client_file(client_bin, port, bootstrap_sql, error)) {
        best_effort_stop(pid, ctl_sock);
        close(pipe_fds[0]);
        return false;
    }
    if (!utils::control_shutdown(ctl_sock, error)) {
        best_effort_stop(pid, ctl_sock);
        close(pipe_fds[0]);
        return false;
    }
    int status = 0;
    if (!wait_child(pid, status, error)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(pipe_fds[0]);
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "server 异常退出";
        close(pipe_fds[0]);
        return false;
    }
    close(pipe_fds[0]);
    return true;
}

}  // namespace

int main(int argc, char* argv[])
{
    std::string data_dir_arg;
    if (argc == 3 && std::string(argv[1]) == "-D") {
        data_dir_arg = argv[2];
    }
    if (data_dir_arg.empty()) {
        std::cerr << "用法: initdb -D <数据目录>" << std::endl;
        return 2;
    }

    // 相对路径基于当前工作目录
    std::string dir = std::filesystem::absolute(data_dir_arg).string();

    std::string error;
    bool dir_created = false;
    if (!prepare_data_dir(dir, dir_created, error)) {
        std::cerr << error << std::endl;
        return 1;
    }
    // storage 报错走日志宏, 先设置日志路径
    Logger::initPath((std::filesystem::path(dir) / "simple.log").string());

    // 失败回滚: 日志移至 /tmp 保留, 清空目录内其余内容, 目录为本次创建则连目录一起删, 返回是否保留
    const auto rollback = [&dir, dir_created]() -> bool {
        std::error_code ec;
        std::filesystem::rename(std::filesystem::path(dir) / "simple.log", kFailedLogPath, ec);
        const bool log_kept = !ec;
        if (dir_created) {
            std::filesystem::remove_all(dir, ec);
            return log_kept;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::filesystem::remove_all(entry.path(), ec);
        }
        return log_kept;
    };

    std::string path = config::conf_path(dir);
    if (!write_default_conf(path, error)) {
        rollback();
        std::cerr << error << std::endl;
        return 1;
    }
    // 生成两张元数据表(自描述行), 兜 std::exception(目录不可写时 Logger 构造亦抛异常)
    try {
        ct::Catalog db(dir);
        db.create();
    } catch (const std::exception& e) {
        const bool log_kept = rollback();
        std::cerr << e.what() << std::endl;
        if (log_kept) {
            std::cerr << "详细信息可查看 " << kFailedLogPath << std::endl;
        }
        return 1;
    }
    // bootstrap 阶段: 拉起 server --bootstrap 执行 bootstrap.sql 后关闭
    if (!run_bootstrap_stage(dir, error)) {
        const bool log_kept = rollback();
        std::cerr << error << std::endl;
        if (log_kept) {
            std::cerr << "详细信息可查看 " << kFailedLogPath << std::endl;
        }
        return 1;
    }
    std::cout << "已初始化数据目录: " << dir << std::endl;
    std::cout << "可编辑配置后执行 serverctl -D " << dir << " start 启动服务" << std::endl;
    return 0;
}
