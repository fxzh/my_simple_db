// utils.cpp: 可执行目录定位与控制通道客户端实现
#include "utils/utils.h"

#include <cstring>
#include <filesystem>
#include <system_error>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace utils {

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

bool companion_path(const char* name, std::string& path, std::string& error)
{
    std::string dir;
    if (!exe_dir(dir, error)) {
        return false;
    }
    path = (std::filesystem::path(dir) / name).string();
    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    if (ec) {
        error = "无法访问伴生文件: " + path;
        return false;
    }
    if (!present) {
        error = "缺少伴生文件: " + path;
        return false;
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        error = "无法访问伴生文件: " + path;
        return false;
    }
    return true;
}

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

bool control_shutdown(const std::string& sock_path, std::string& error)
{
    std::string reply;
    if (!control_send_recv(sock_path, "shutdown\n", reply, error)) {
        return false;
    }
    if (reply != "OK") {
        error = "shutdown 回复异常: " + reply;
        return false;
    }
    return true;
}

}  // namespace utils
