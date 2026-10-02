#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "config/config.h"
#include "log/log.h"
#include "net.h"

using enum LogModule;
using enum LogLevel;

namespace {

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

}  // namespace

// 创建 TCP 监听 socket: bootstrap 模式仅本机监听且端口 0 由内核临时分配, 实际端口写回 listen_port
int create_tcp_listener(bool bootstrap_mode, int& listen_port)
{
    // 创建socket文件描述符
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG(CRITICAL, NETWORK, "Socket创建失败");
        return -1;
    }

    // 非阻塞: poll 唤醒与 accept 之间队列被清空时返回 EAGAIN, 不挂起主循环
    int sock_flags = fcntl(server_fd, F_GETFL, 0);
    if (sock_flags < 0 || fcntl(server_fd, F_SETFL, sock_flags | O_NONBLOCK) < 0) {
        close(server_fd);
        LOG(CRITICAL, NETWORK, "设置监听 socket 非阻塞失败");
        return -1;
    }

    // 设置socket选项
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        close(server_fd);
        LOG(CRITICAL, NETWORK, "设置socket选项失败");
        return -1;
    }

    sockaddr_in address;
    address.sin_family = AF_INET;
    // bootstrap 模式仅本机监听且端口 0 由内核临时分配, 配置端口忽略
    address.sin_addr.s_addr = bootstrap_mode ? htonl(INADDR_LOOPBACK) : INADDR_ANY;
    address.sin_port = htons(bootstrap_mode
                                  ? 0
                                  : static_cast<in_port_t>(config::cfg.port));

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

    // 实际监听端口: bootstrap 模式由内核分配后回填, 正常模式为配置端口
    listen_port = config::cfg.port;
    if (bootstrap_mode) {
        sockaddr_in bound;
        socklen_t bound_len = sizeof(bound);
        if (getsockname(server_fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
            close(server_fd);
            LOG(CRITICAL, NETWORK, "获取实际监听端口失败");
            return -1;
        }
        listen_port = ntohs(bound.sin_port);
    }
    return server_fd;
}

// 创建控制通道监听 socket(unix domain): 清理残留路径后绑定, 仅属主可读写
int create_control_listener(const std::string& ctl_sock)
{
    int control_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (control_fd < 0) {
        LOG(CRITICAL, NETWORK, "控制 socket 创建失败");
        return -1;
    }

    // 非阻塞: poll 唤醒与 accept 之间队列被清空时返回 EAGAIN, 不挂起主循环
    int ctl_flags = fcntl(control_fd, F_GETFL, 0);
    if (ctl_flags < 0 || fcntl(control_fd, F_SETFL, ctl_flags | O_NONBLOCK) < 0) {
        close(control_fd);
        LOG(CRITICAL, NETWORK, "设置监听 socket 非阻塞失败");
        return -1;
    }
    sockaddr_un ctl_addr;
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
    return control_fd;
}

// 受理一条控制通道连接: 收包 2 秒超时, 返回是否收到 shutdown
bool accept_control_command(int control_fd)
{
    int control_conn = accept(control_fd, nullptr, nullptr);
    if (control_conn < 0) {
        return false;  // 本轮无连接可受理
    }
    timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(control_conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));  // 输入防护, 防挂死
    return handle_control_command(control_conn);
}
