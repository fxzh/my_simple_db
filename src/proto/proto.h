// proto.h: client 与 server 间的 TCP 帧协议(长度前缀+消息类型), 双端共用, header-only
#ifndef PROTO_PROTO_H
#define PROTO_PROTO_H

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>

#include <arpa/inet.h>
#include <sys/socket.h>

namespace proto {

// 帧格式: [4B 网络序 payload 长度][payload]; payload = [1B 消息类型][body], 长度含类型字节
constexpr uint32_t FRAME_HEADER_SIZE = 4;

// 服务端受理的请求 payload 上限, 与 client 的 SQL_BUFFER_LIMIT 一致, 超限断连
constexpr uint32_t MAX_REQUEST_PAYLOAD = 10240;

// 消息类型: Query 为请求方向, 其余为响应方向
enum class MsgType : uint8_t {
    Query = 1,      // body: SQL 原文
    Ok = 2,         // body: 状态文本
    Error = 3,      // body: 错误文案
    ResultSet = 4,  // body: 结果集, SELECT 接入时启用
};

// 读满 len 字节: 对端关闭或系统错误返回 false, EINTR 自动重试
inline bool recv_exact(int fd, void* buf, std::size_t len)
{
    auto* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0) {
            return false;  // 对端关闭
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

// 写完整个缓冲: 部分写与 EINTR 自动处理, 失败返回 false
inline bool send_exact(int fd, const void* buf, std::size_t len)
{
    const auto* p = static_cast<const char*>(buf);
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// 收一帧: max_payload 为 0 表示不限; 长度为 0 或超限时不再收 payload 直接失败
inline bool recv_frame(int fd, uint32_t max_payload, MsgType& type, std::string& body)
{
    uint32_t len_net = 0;
    if (!recv_exact(fd, &len_net, FRAME_HEADER_SIZE)) {
        return false;
    }
    const uint32_t len = ntohl(len_net);
    if (len < 1 || (max_payload != 0 && len > max_payload)) {
        return false;
    }
    std::string payload(len, '\0');
    if (!recv_exact(fd, payload.data(), len)) {
        return false;
    }
    type = static_cast<MsgType>(payload[0]);
    body = payload.substr(1);
    return true;
}

// 发一帧
inline bool send_frame(int fd, MsgType type, const std::string& body)
{
    const uint32_t len_net = htonl(1 + static_cast<uint32_t>(body.size()));
    const unsigned char type_byte = static_cast<unsigned char>(type);
    return send_exact(fd, &len_net, FRAME_HEADER_SIZE)
        && send_exact(fd, &type_byte, 1)
        && send_exact(fd, body.data(), body.size());
}

}  // namespace proto

#endif  // PROTO_PROTO_H
