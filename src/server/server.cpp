#include <iostream>
#include <string>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <optional>
#include <csignal>
#include <sys/socket.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include "common/err.h"
#include "config/config.h"
#include "log/log.h"
#include "catalog.h"
#include "net.h"
#include "session.h"

using enum LogModule;
using enum LogLevel;

// 全局变量: 进程停止标志, 声明见 session.h
std::atomic<bool> server_running{true};

namespace {

// 信号处理: 仅置停止标志, 主循环经 poll EINTR 退出
void signal_handler(int)
{
    server_running.store(false, std::memory_order_relaxed);
}

// 用法提示
void usage()
{
    std::cerr << "用法: server -D <数据目录> [--daemon] [--bootstrap]" << std::endl;
}

// 截断重写 pidfile 内容, 失败返回 false, errno 留给调用方报 DB_CRITICAL
bool write_pid_file(int pid_fd, pid_t pid)
{
    std::string pid_str = std::to_string(pid);
    return ftruncate(pid_fd, 0) == 0 && pwrite(pid_fd, pid_str.data(), pid_str.size(), 0) >= 0;
}

// daemon 父进程: 写 pidfile(子进程 pid) 后读握手(≤5s), 各路径直接 _exit, 永不返回
void run_daemon_parent(int pid_fd, pid_t daemon_pid, int notify_rd, int notify_wr,
                       const std::string& pidfile, const std::string& log_path)
{
    if (!write_pid_file(pid_fd, daemon_pid)) {
        kill(daemon_pid, SIGTERM);  // 终止刚 fork 的 daemon, 避免留下孤儿进程
        DB_CRITICAL(SYSTEM, "写入 pidfile 失败: {}", std::strerror(errno));
    }
    close(notify_wr);
    // 读握手(≤5s): R=就绪, F+消息=失败, EOF=子进程未通知即退出, 超时交 serverctl 轮询
    pollfd npfd{notify_rd, POLLIN, 0};
    int pret = poll(&npfd, 1, 5000);
    char tag = 0;
    ssize_t n = -1;
    if (pret > 0) {
        do {
            n = read(notify_rd, &tag, 1);
        } while (n < 0 && errno == EINTR);
    }
    if (n == 1 && tag == 'R') {
        _exit(0);
    }
    if (n == 1 && tag == 'F') {
        std::string msg;
        char buf[512];
        while ((n = read(notify_rd, buf, sizeof(buf))) > 0) {
            msg.append(buf, static_cast<size_t>(n));
        }
        std::cerr << msg << std::endl;  // 原因行, 结论行由 serverctl 输出
        unlink(pidfile.c_str());
        _exit(1);
    }
    if (n == 0) {
        std::cerr << "daemon 进程异常退出, 原因见日志: " << log_path << std::endl;
        unlink(pidfile.c_str());
        _exit(1);
    }
    if (pret == 0 || (n < 0 && errno == EINTR) || (pret < 0 && errno == EINTR)) {
        _exit(0);  // 超时/被打断: daemon 仍在启动(如长恢复), 交 serverctl 轮询
    }
    kill(daemon_pid, SIGTERM);  // 握手通道异常, daemon 状态未知, 终止防孤儿
    std::cerr << "读握手失败: " << std::strerror(errno) << std::endl;
    unlink(pidfile.c_str());
    _exit(1);
}

// daemon 子进程: 关握手读端, 脱离会话与控制终端, 重定向标准输入/输出/错误, 返回握手写端
int detach_daemon_child(int notify_rd, int notify_wr)
{
    close(notify_rd);
    if (setsid() < 0) {
        DB_CRITICAL(SYSTEM, "setsid 失败");
    }
    // 标准输入/输出/错误重定向, 不再依赖启动终端
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
    return notify_wr;
}

// daemon 握手写入: 接收端(中间父进程)超时退出后 EPIPE 属预期, 忽略写入结果
void daemon_notify(int fd, char tag, const std::string& msg)
{
    if (fd < 0) {
        return;  // 前台模式无握手
    }
    ssize_t n = write(fd, &tag, 1);
    if (!msg.empty()) {
        n = write(fd, msg.data(), msg.size());
    }
    (void)n;
}

}  // namespace

// 服务器主函数
int main(int argc, char* argv[])
{
    // -D <数据目录> 必选, --daemon 可选后台运行, --bootstrap 可选自举模式(前台)
    std::string data_dir_arg;
    bool daemon_mode = false;
    bool bootstrap_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-D") {
            if (i + 1 >= argc || !data_dir_arg.empty()) {
                usage();
                return -1;
            }
            data_dir_arg = argv[++i];
        } else if (arg == "--daemon") {
            daemon_mode = true;
        } else if (arg == "--bootstrap") {
            bootstrap_mode = true;
        } else {
            usage();
            return -1;
        }
    }
    if (data_dir_arg.empty()) {
        usage();
        return -1;
    }
    if (daemon_mode && bootstrap_mode) {
        std::cerr << "--daemon 与 --bootstrap 互斥" << std::endl;
        return -1;
    }

    // 数据目录统一转绝对路径, daemon 化后不依赖 cwd
    std::filesystem::path data_dir_abs = std::filesystem::absolute(data_dir_arg);
    std::string data_dir = data_dir_abs.string();

    // 加载配置文件 db.conf(位于数据目录内), 日志未初始化, 报错只走控制台
    std::string config_path = config::conf_path(data_dir);
    std::string config_error;
    if (!config::load(config_path, config_error)) {
        std::cerr << config_error << std::endl;
        if (!std::filesystem::exists(config_path)) {
            std::cout << "配置文件不存在, 请先运行 initdb -D " << data_dir << std::endl;
        }
        return -1;
    }
    std::cout << "已加载配置文件: " << config_path << std::endl;

    std::string ctl_sock = config::cfg.control_socket.empty()
        ? (data_dir_abs / "server.sock").string()
        : std::filesystem::absolute(config::cfg.control_socket).string();

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

    // 日志级别阈值初值来自 db.conf(缺省 info); config 已校验, 解析失败仍立即报错,
    // 此时日志未构造, 报错只走控制台
    const std::optional<LogLevel> log_level = levelFromString(config::cfg.server_log_level);
    if (!log_level.has_value()) {
        std::cerr << "server_log_level 值非法: " << config::cfg.server_log_level << std::endl;
        return -1;
    }
    Logger::initLevel(*log_level);

    // 单实例锁: flock(pidfile), 锁随 fd 在进程生命周期内持有
    std::string pidfile = (data_dir_abs / "server.pid").string();
    int pid_fd = open(pidfile.c_str(), O_CREAT | O_RDWR, 0644);
    if (pid_fd < 0) {
        DB_CRITICAL(SYSTEM, "无法打开 pidfile: {}", pidfile);
    }
    if (flock(pid_fd, LOCK_EX | LOCK_NB) != 0) {
        DB_CRITICAL(SYSTEM, "另一实例已在运行");
    }

    // 目录层实例, 所有客户端线程共享这一个实例; bootstrap 标志随构造传入, open 在 daemon fork 之后
    ct::Catalog db(data_dir, bootstrap_mode);

    int server_fd, new_socket;
    sockaddr_in address;
    int addrlen = sizeof(address);

    // 实际监听端口: bootstrap 模式由内核分配后回填, 正常模式为配置端口
    int listen_port = 0;
    server_fd = create_tcp_listener(bootstrap_mode, listen_port);

    // 控制通道: unix domain socket
    int control_fd = create_control_listener(ctl_sock);

    // 信号: SIGPIPE 忽略(向已关闭连接写回复的防护), 停止信号走统一收尾
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

    // daemon 就绪/失败握手管道写端, daemon 子进程内有效, 前台恒为 -1
    int daemon_notify_fd = -1;
    // --daemon: fork 后父进程写 pidfile 并读握手结果, 子进程脱离会话与控制终端
    if (daemon_mode) {
        int notify_pipe[2];
        if (pipe(notify_pipe) != 0) {
            DB_CRITICAL(SYSTEM, "创建握手管道失败: {}", std::strerror(errno));
        }
        pid_t daemon_pid = fork();
        if (daemon_pid < 0) {
            DB_CRITICAL(SYSTEM, "daemon fork 失败");
        }
        if (daemon_pid > 0) {
            run_daemon_parent(pid_fd, daemon_pid, notify_pipe[0], notify_pipe[1], pidfile,
                              log_path);
        }
        daemon_notify_fd = detach_daemon_child(notify_pipe[0], notify_pipe[1]);
    } else {
        // 前台: 写 pidfile(自身 pid)
        if (!write_pid_file(pid_fd, getpid())) {
            DB_CRITICAL(SYSTEM, "写入 pidfile 失败: {}", std::strerror(errno));
        }
    }

    // 打开数据目录(含崩溃恢复重放): 须在 daemon fork 之后, 恢复日志在子进程内构造 Logger
    try {
        db.open();
    } catch (const db::DbError& e) {
        if (e.code() == db::ErrCode::CatalogMissing) {
            if (daemon_notify_fd >= 0) {
                daemon_notify(daemon_notify_fd, 'F', "数据目录未初始化, 请先执行 initdb");
            } else {
                std::cout << "数据目录未初始化, 请先执行 initdb" << std::endl;
            }
        } else {
            daemon_notify(daemon_notify_fd, 'F', e.what());
        }
        DB_CRITICAL(SYSTEM, "打开数据目录失败: {}", e.what());
    } catch (const std::exception& e) {
        daemon_notify(daemon_notify_fd, 'F', e.what());
        DB_CRITICAL(SYSTEM, "打开数据目录失败: {}", e.what());
    }
    std::cout << "已打开数据目录: " << data_dir << std::endl;

    // 启动信息 LOG 必须在 fork 之后, 保证子进程内首次构造 Logger
    if (bootstrap_mode) {
        LOG(INFO, SYSTEM, "服务器已启动, bootstrap 模式监听 127.0.0.1:%d", listen_port);
    } else {
        LOG(INFO, SYSTEM, "服务器已启动, 监听端口 %d", listen_port);
    }

    std::cout << "支持最多 " << MAX_CLIENTS << " 个客户端同时连接" << std::endl;

    // bootstrap 端口行: initdb 经管道逐行匹配前缀读取, endl 立即刷新全缓冲的 stdout
    if (bootstrap_mode) {
        std::cout << "bootstrap_port=" << listen_port << std::endl;
    }

    // status 回复快照: 登记实际监听端口与就绪时刻
    set_control_status_info(listen_port);

    // daemon 就绪握手: 通知中间父进程, 前台为空操作
    daemon_notify(daemon_notify_fd, 'R', "");

    // 主循环: poll 双 socket(数据连接 + 控制连接)
    pollfd fds[2];
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
            LOG(WARNING, NETWORK, "poll 失败: %s", std::strerror(errno));
            continue;
        }

        // 控制通道: 内联处理一条控制命令, 不建线程
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (accept_control_command(control_fd)) {
                server_running = false;
                break;  // 收到 shutdown
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
            LOG(WARNING, NETWORK, "接受连接失败: %s", std::strerror(errno));
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

    // 等待客户端清空: 在途语句无取消点, 等待上界为最慢语句剩余时长, 超时 10 秒走崩溃式退出
    {
        std::unique_lock<std::mutex> lock(clients_mutex);
        if (!clients_cv.wait_for(lock, std::chrono::seconds(10),
                                 [] { return clients.empty(); })) {
            DB_CRASH(SYSTEM, "等待客户端退出超时, 剩余 {} 个会话, 崩溃式退出",
                     clients.size());
        }
    }

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
        DB_CRITICAL(STORAGE, "关闭存储引擎失败: {}", e.what());
    }

    close(pid_fd);  // 锁保持到收尾完成, 防止新实例提前抢锁

    LOG(INFO, SYSTEM, "服务器已安全关闭");
    return 0;
}
