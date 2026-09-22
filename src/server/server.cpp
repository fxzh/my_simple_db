#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "common/err.h"
#include "config.h"
#include "log/log.h"
#include "catalog.h"
#include "session.h"

using enum LogModule;
using enum LogLevel;

// 全局变量: 进程停止标志, 声明见 session.h
std::atomic<bool> server_running{true};

// 信号处理: 仅置停止标志, 主循环经 poll EINTR 退出(handler 内不得 LOG, 非 async-signal-safe)
void signal_handler(int)
{
    server_running.store(false, std::memory_order_relaxed);
}

// 控制通道命令: 一次连接一条命令, 返回是否收到 shutdown
bool handle_control_command(int cfd)
{
    char buf[128] = {0};
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return false;  // 超时或连接半开, 直接关闭
    }
    buf[static_cast<size_t>(n)] = '\0';
    std::string cmd(buf);
    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) {
        cmd.pop_back();  // 容忍/忽略结尾换行
    }
    std::string reply;
    if (cmd == "ping") {
        reply = "PONG";
    } else if (cmd == "status") {
        reply = "ERROR: status 暂不支持";  // 占位: 富状态字段后续扩展, 协议沿用一行回复
    } else if (cmd == "shutdown") {
        reply = "OK";
    } else {
        reply = "ERROR: unknown command";
    }
    send(cfd, reply.c_str(), reply.size(), 0);
    close(cfd);
    return cmd == "shutdown";
}

// 服务器主函数
int main(int argc, char* argv[])
{
    // -D <数据目录> 必选, --daemon 可选后台运行
    std::string data_dir_arg;
    bool daemon_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-D") {
            if (i + 1 >= argc || !data_dir_arg.empty()) {
                std::cerr << "用法: server -D <数据目录> [--daemon]" << std::endl;
                return -1;
            }
            data_dir_arg = argv[++i];
        } else if (arg == "--daemon") {
            daemon_mode = true;
        } else {
            std::cerr << "用法: server -D <数据目录> [--daemon]" << std::endl;
            return -1;
        }
    }
    if (data_dir_arg.empty()) {
        std::cerr << "用法: server -D <数据目录> [--daemon]" << std::endl;
        return -1;
    }

    // 数据目录统一转绝对路径, daemon 化后不依赖 cwd
    std::filesystem::path data_dir_abs = std::filesystem::absolute(data_dir_arg);
    std::string data_dir = data_dir_abs.string();

    // 加载配置文件 db.conf(位于数据目录内), 日志未初始化, 报错只走控制台
    config::Config cfg;
    std::string config_path = config::conf_path(data_dir);
    std::string config_error;
    if (!config::load(config_path, cfg, config_error)) {
        std::cerr << config_error << std::endl;
        if (!std::filesystem::exists(config_path)) {
            std::cout << "配置文件不存在, 请先运行 initdb -D " << data_dir << std::endl;
        }
        return -1;
    }
    std::cout << "已加载配置文件: " << config_path << std::endl;

    std::string ctl_sock = cfg.control_socket.empty()
        ? (data_dir_abs / "server.sock").string()
        : std::filesystem::absolute(cfg.control_socket).string();

    // 日志文件与 pidfile 都在数据目录内
    std::string log_path = (data_dir_abs / "simple.log").string();
    {
        // fork 前仅探测日志文件可打开, 单例留待 fork 后首次 LOG 构造(daemon 子进程内建写线程)
        std::ofstream probe(log_path, std::ios::out | std::ios::app);
        if (!probe.is_open()) {
            std::cerr << "初始化日志失败: 无法打开日志文件: " << log_path << std::endl;
            return -1;
        }
    }
    Logger::initPath(log_path);

    // 单实例锁: flock(pidfile), 锁随 fd 在进程生命周期内持有
    std::string pidfile = (data_dir_abs / "server.pid").string();
    int pid_fd = open(pidfile.c_str(), O_CREAT | O_RDWR, 0644);
    if (pid_fd < 0) {
        LOG(CRITICAL, SYSTEM, "无法打开 pidfile: %s", pidfile.c_str());
        return -1;
    }
    if (flock(pid_fd, LOCK_EX | LOCK_NB) != 0) {
        LOG(CRITICAL, SYSTEM, "另一实例已在运行");
        return -1;
    }

    // 打开数据目录(存储引擎), 所有客户端线程共享这一个实例
    ct::Catalog db(data_dir);
    try {
        db.open();
    } catch (const db::DbError& e) {
        LOG(CRITICAL, SYSTEM, "打开数据目录失败: %s", e.what());
        if (e.code() == db::ErrCode::CatalogMissing) {
            std::cout << "数据目录未初始化, 请先执行 initdb" << std::endl;
        }
        return -1;
    } catch (const std::exception& e) {
        LOG(CRITICAL, SYSTEM, "打开数据目录失败: %s", e.what());
        return -1;
    }
    std::cout << "已打开数据目录: " << data_dir << std::endl;

    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // 创建socket文件描述符
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        LOG(CRITICAL, NETWORK, "Socket创建失败");
        return -1;
    }

    // 设置socket选项
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        LOG(CRITICAL, NETWORK, "设置socket选项失败");
        return -1;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<in_port_t>(cfg.port));

    // 绑定socket到地址和端口
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(server_fd);
        LOG(CRITICAL, NETWORK, "绑定端口失败");
        return -1;
    }

    // 开始监听连接
    if (listen(server_fd, 10) < 0) {  // 增加等待队列长度
        close(server_fd);
        LOG(CRITICAL, NETWORK, "监听失败");
        return -1;
    }

    // 控制通道: unix domain socket
    int control_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (control_fd < 0) {
        LOG(CRITICAL, NETWORK, "控制 socket 创建失败");
        return -1;
    }
    struct sockaddr_un ctl_addr;
    memset(&ctl_addr, 0, sizeof(ctl_addr));
    ctl_addr.sun_family = AF_UNIX;
    if (ctl_sock.size() >= sizeof(ctl_addr.sun_path)) {
        LOG(CRITICAL, NETWORK, "控制 socket 路径过长: %s", ctl_sock.c_str());
        return -1;
    }
    strncpy(ctl_addr.sun_path, ctl_sock.c_str(), sizeof(ctl_addr.sun_path) - 1);
    unlink(ctl_sock.c_str());  // 清理上次异常退出残留的 socket 文件
    if (bind(control_fd, reinterpret_cast<sockaddr*>(&ctl_addr), sizeof(ctl_addr)) < 0) {
        LOG(CRITICAL, NETWORK, "控制 socket 绑定失败: %s", ctl_sock.c_str());
        return -1;
    }
    // 仅属主可读写, 避免同机其他用户任意停服(fchmod 对 socket fd 无效, 必须对路径 chmod)
    if (chmod(ctl_sock.c_str(), 0600) != 0) {
        LOG(CRITICAL, NETWORK, "设置控制 socket 权限失败");
        return -1;
    }
    if (listen(control_fd, 5) < 0) {
        LOG(CRITICAL, NETWORK, "控制 socket 监听失败");
        return -1;
    }

    // 信号: SIGPIPE 忽略(向已关闭连接写回复的防护), 停止信号走统一收尾
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

    // --daemon: fork 后父进程写 pidfile 退出, 子进程脱离会话与控制终端
    if (daemon_mode) {
        pid_t daemon_pid = fork();
        if (daemon_pid < 0) {
            LOG(CRITICAL, SYSTEM, "daemon fork 失败");
            return -1;
        }
        if (daemon_pid > 0) {
            // 父进程: 写 pidfile(子进程 pid) 后立即退出
            std::string pid_str = std::to_string(daemon_pid);
            if (ftruncate(pid_fd, 0) != 0 ||
                pwrite(pid_fd, pid_str.data(), pid_str.size(), 0) < 0) {
                std::cerr << "写入 pidfile 失败" << std::endl;
            }
            _exit(0);
        }
        // 子进程(daemon): 脱离会话与控制终端
        if (setsid() < 0) {
            LOG(CRITICAL, SYSTEM, "setsid 失败");
            return -1;
        }
        // 标准输入/输出/错误重定向, 不再依赖启动终端
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    } else {
        // 前台: 写 pidfile(自身 pid)
        std::string pid_self = std::to_string(getpid());
        if (ftruncate(pid_fd, 0) != 0 ||
            pwrite(pid_fd, pid_self.data(), pid_self.size(), 0) < 0) {
            std::cerr << "写入 pidfile 失败" << std::endl;
        }
    }

    // 启动信息 LOG 必须在 fork 之后, 保证子进程内首次构造 Logger
    LOG(INFO, SYSTEM, "服务器已启动, 监听端口 %d", cfg.port);

    std::cout << "支持最多 " << MAX_CLIENTS << " 个客户端同时连接" << std::endl;

    // 主循环: poll 双 socket(数据连接 + 控制连接)
    struct pollfd fds[2];
    fds[0].fd = server_fd;
    fds[0].events = POLLIN;
    fds[1].fd = control_fd;
    fds[1].events = POLLIN;

    while (server_running) {
        int poll_ret = poll(fds, 2, -1);
        if (poll_ret < 0) {
            if (errno == EINTR) {
                if (!server_running) {
                    break;  // 信号触发停止
                }
                continue;
            }
            LOG(WARNING, NETWORK, "poll 失败");
            continue;
        }

        // 控制通道: 内联处理一条控制命令, 不建线程
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            int control_conn = accept(control_fd, nullptr, nullptr);
            if (control_conn >= 0) {
                struct timeval tv;
                tv.tv_sec = 2;
                tv.tv_usec = 0;
                setsockopt(control_conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));  // 输入防护, 防挂死
                if (handle_control_command(control_conn)) {
                    server_running = false;
                    break;  // 收到 shutdown
                }
            }
        }
        if (!server_running) {
            break;
        }

        // TCP 数据连接
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            continue;
        }

        // 接受客户端连接
        new_socket = accept(server_fd, reinterpret_cast<sockaddr*>(&address), reinterpret_cast<socklen_t*>(&addrlen));
        if (new_socket < 0) {
            if (!server_running) {
                break;  // 服务器正在关闭
            }
            LOG(WARNING, NETWORK, "接受连接失败");
            continue;
        }

        // 受理新连接: 拒超限/建线程/入表由会话层完成
        spawn_client(new_socket, address, &db);
    }

    // 等待所有客户端线程结束
    std::cout << "等待所有客户端断开连接..." << std::endl;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (auto& client : clients) {
            shutdown(client->socket, SHUT_RDWR);
        }
    }

    // 等待一段时间让客户端断开
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 清理资源: 数据与控制两个监听 socket
    close(server_fd);
    close(control_fd);

    // 删除控制 socket 与 pidfile, 避免残留文件挡路
    unlink(ctl_sock.c_str());
    unlink(pidfile.c_str());

    // 刷盘并关闭存储引擎
    try {
        db.close();
    } catch (const std::exception& e) {
        LOG(WARNING, STORAGE, "关闭存储引擎失败: %s", e.what());
    }

    close(pid_fd);  // 锁保持到收尾完成, 防止新实例提前抢锁

    LOG(INFO, SYSTEM, "服务器已安全关闭");
    return 0;
}
