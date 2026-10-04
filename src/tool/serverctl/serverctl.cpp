// serverctl: server 控制工具, 子命令 start/stop/status
#include <iostream>
#include <string>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include "config/config.h"
#include "utils/utils.h"

namespace {

// 控制通道路径解析: 与 server 启动解析规则保持一致
std::string control_socket_path(const std::string& data_dir)
{
    if (!config::cfg.control_socket.empty()) {
        return std::filesystem::absolute(config::cfg.control_socket).string();
    }
    return (std::filesystem::path(data_dir) / "server.sock").string();
}

int cmd_start(const std::string& data_dir, const std::string& sock_path, const std::string& pidfile_path)
{
    // 已运行检测: 控制 socket 可连即视为在跑
    {
        std::string reply, error;
        if (utils::control_send_recv(sock_path, "ping\n", reply, error)) {
            std::cerr << "服务器已在运行" << std::endl;
            return 2;
        }
    }

    std::string dir, error;
    if (!utils::exe_dir(dir, error)) {
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
        if (utils::control_send_recv(sock_path, "ping\n", reply, err) && reply == "PONG") {
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
    if (!utils::control_send_recv(sock_path, "shutdown\n", reply, error)) {
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
    if (!utils::control_send_recv(sock_path, "status\n", reply, error)) {
        std::cerr << "服务器未运行" << std::endl;
        return 1;
    }
    std::cout << reply << std::endl;
    return 0;
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

    std::string config_path = config::conf_path(data_dir);
    std::string config_error;
    if (!config::load(config_path, config_error)) {
        std::cerr << "读取配置失败: " << config_error << std::endl;
        if (!std::filesystem::exists(config_path)) {
            std::cerr << "请先运行 initdb -D " << data_dir << std::endl;
        }
        return 2;
    }

    std::string sock_path = control_socket_path(data_dir);
    std::string pidfile_path = (std::filesystem::path(data_dir) / "server.pid").string();

    if (cmd == "start") {
        return cmd_start(data_dir, sock_path, pidfile_path);
    }
    if (cmd == "stop") {
        return cmd_stop(sock_path);
    }
    return cmd_status(sock_path);
}