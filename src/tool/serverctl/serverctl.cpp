// serverctl: server 控制工具, 子命令 start/stop/status
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include "server/config.h"

namespace {

// TODO: 与 config.cpp::exe_dir 重复, 后续统一迁移到 src/utils
bool exe_dir(std::string& dir, std::string& error)
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        error = "无法获取可执行文件路径(/proc/self/exe)";
        return false;
    }
    buf[static_cast<size_t>(n)] = '\0';
    dir = std::filesystem::path(buf).parent_path().string();
    return true;
}

// 控制通道路径解析: 与 server 启动解析规则保持一致
std::string control_socket_path(const config::Config& cfg, const std::string& data_dir)
{
    if (!cfg.control_socket.empty()) {
        return std::filesystem::absolute(cfg.control_socket).string();
    }
    return (std::filesystem::path(data_dir) / "server.sock").string();
}

// 一次控制连接: 发一条命令, 收全部回复直到 EOF
bool control_send_recv(const std::string& socket_path, const std::string& cmd,
                       std::string& reply, std::string& error)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "创建控制 socket 失败";
        return false;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        error = "控制 socket 路径过长: " + socket_path;
        close(fd);
        return false;
    }
    strncpy(addr.sun_path, socket_path.c_str(), socket_path.size());

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error = "连接控制 socket 失败: " + socket_path;
        close(fd);
        return false;
    }
    // 读侧超时: server 无响应时不永久挂死
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (send(fd, cmd.c_str(), cmd.size(), MSG_NOSIGNAL) < 0) {
        error = "发送控制命令失败";
        close(fd);
        return false;
    }
    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            reply.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;  // EOF: 回复结束
        }
        error = "读取控制回复失败";
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

int cmd_start(const std::string& data_dir, const std::string& sock_path, const std::string& pidfile_path)
{
    // 已运行检测: 控制 socket 可连即视为在跑
    {
        std::string reply, error;
        if (control_send_recv(sock_path, "ping\n", reply, error)) {
            std::cerr << "服务器已在运行" << std::endl;
            return 2;
        }
    }

    std::string dir, error;
    if (!exe_dir(dir, error)) {
        std::cerr << error << std::endl;
        return 2;
    }
    std::string server_bin = (std::filesystem::path(dir) / "server").string();

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "启动失败: fork 错误" << std::endl;
        return 2;
    }
    if (pid == 0) {
        // 子进程保留 stderr 继承, server fork 前错误直接透传
        execl(server_bin.c_str(), "server", "-D", data_dir.c_str(), "--daemon",
              static_cast<char*>(nullptr));
        std::cerr << "启动失败: exec 失败" << std::endl;
        _exit(1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "启动失败" << std::endl;
        return 2;
    }

    // 轮询就绪(≤5s): ping 回 PONG 才是真实存活, connect 只证明有监听
    bool ready = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string reply, err;
        if (control_send_recv(sock_path, "ping\n", reply, err) && reply == "PONG") {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!ready) {
        std::cerr << "启动超时" << std::endl;
        return 2;
    }

    // 父进程 _exit 前已写 pidfile, 直接读取
    pid_t daemon_pid = 0;
    {
        std::ifstream in(pidfile_path);
        if (in) {
            std::string pid_text;
            std::getline(in, pid_text);
            daemon_pid = static_cast<pid_t>(std::atol(pid_text.c_str()));
        }
    }
    std::cout << "已启动, pid=" << daemon_pid << std::endl;
    return 0;
}

int cmd_stop(const std::string& sock_path)
{
    std::string reply, error;
    if (!control_send_recv(sock_path, "shutdown\n", reply, error)) {
        std::cerr << "服务器未运行" << std::endl;
        return 1;
    }
    if (reply != "OK") {
        std::cerr << "收到异常回复: " << reply << std::endl;
        return 2;
    }
    // 等控制 socket 路径消失(≤10s), 表示已走完整收尾
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!std::filesystem::exists(sock_path)) {
            std::cout << "已停止" << std::endl;
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cerr << "服务器未退出" << std::endl;
    return 2;
}

int cmd_status(const std::string& sock_path)
{
    std::string reply, error;
    if (!control_send_recv(sock_path, "status\n", reply, error)) {
        std::cerr << "服务器未运行" << std::endl;
        return 1;
    }
    std::cout << reply << std::endl;
    return 2;  // 占位: 富状态字段后续扩展
}

}  // namespace

int main(int argc, char* argv[])
{
    // -D <数据目录> 必选, 可与子命令任意先后
    std::string data_dir_arg;
    std::string cmd;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-D") {
            if (i + 1 >= argc || !data_dir_arg.empty()) {
                std::cerr << "用法: serverctl -D <数据目录> start|stop|status" << std::endl;
                return 2;
            }
            data_dir_arg = argv[++i];
        } else if (cmd.empty() && (arg == "start" || arg == "stop" || arg == "status")) {
            cmd = arg;
        } else {
            std::cerr << "用法: serverctl -D <数据目录> start|stop|status" << std::endl;
            return 2;
        }
    }
    if (data_dir_arg.empty() || cmd.empty()) {
        std::cerr << "用法: serverctl -D <数据目录> start|stop|status" << std::endl;
        return 2;
    }

    // 数据目录统一转绝对路径, 与 server 解析规则保持一致
    std::string data_dir = std::filesystem::absolute(data_dir_arg).string();

    config::Config cfg;
    std::string config_path = config::conf_path(data_dir);
    std::string config_error;
    if (!config::load(config_path, cfg, config_error)) {
        std::cerr << "读取配置失败: " << config_error << std::endl;
        if (!std::filesystem::exists(config_path)) {
            std::cerr << "请先运行 initdb -D " << data_dir << std::endl;
        }
        return 2;
    }

    std::string sock_path = control_socket_path(cfg, data_dir);
    std::string pidfile_path = (std::filesystem::path(data_dir) / "server.pid").string();

    if (cmd == "start") {
        return cmd_start(data_dir, sock_path, pidfile_path);
    }
    if (cmd == "stop") {
        return cmd_stop(sock_path);
    }
    return cmd_status(sock_path);
}