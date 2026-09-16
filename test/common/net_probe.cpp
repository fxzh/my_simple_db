// net_probe.cpp: TCP 端口探测与就绪轮询(IPv4)
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/net_probe.hpp"

namespace tcommon {

namespace {

// 单次非阻塞连接尝试, connect_timeout_ms 内未完成即失败
bool connect_once(const std::string& host, int port, int connect_timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return false;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        close(fd);
        return true;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    struct pollfd pfd{fd, POLLOUT, 0};
    if (poll(&pfd, 1, connect_timeout_ms) <= 0) {
        close(fd);
        return false;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

}  // namespace

bool port_is_open(const std::string& host, int port)
{
    return connect_once(host, port, 200);
}

bool wait_port_ready(const std::string& host, int port, int timeout_ms, std::string& error)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (connect_once(host, port, 200)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (connect_once(host, port, 200)) {
        return true;
    }
    error = std::format("端口在 {}ms 内未就绪: {}:{}", timeout_ms, host, port);
    return false;
}

}
