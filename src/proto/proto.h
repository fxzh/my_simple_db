// proto.h: client 与 server 间的 TCP 帧协议(长度前缀+消息类型), 双端共用, header-only
#ifndef PROTO_PROTO_H
#define PROTO_PROTO_H

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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
    ResultSet = 4,  // body: 结果集(编码见下方 ResultSet 部分)
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

// ==================== 结果集(ResultSet body 编解码) ====================

// 结果集单元格值(与 st::Value 同构, proto 层独立定义避免依赖)
using CellVal = std::variant<std::monostate, int64_t, double, std::string>;

// 结果集: 列名 + 行值
struct ResultSet {
    std::vector<std::string> cols;
    std::vector<std::vector<CellVal>> rows;
};

// 单元格编码 tag
constexpr uint8_t CELL_NULL = 0;
constexpr uint8_t CELL_INT = 1;
constexpr uint8_t CELL_DOUBLE = 2;
constexpr uint8_t CELL_STRING = 3;

// 大端序追加无符号整数
inline void append_u16(std::string& out, uint16_t v)
{
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

inline void append_u32(std::string& out, uint32_t v)
{
    for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
    }
}

inline void append_u64(std::string& out, uint64_t v)
{
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
    }
}

// body 布局(大端网络序): [列数 u32] 每列[列名长度 u16][列名] [行数 u32]
// 每行每列一个单元格 [tag u8][payload]: 0=NULL 无 payload, 1=int64 8B,
// 2=double 8B(位模式按 u64), 3=string[长度 u16][字节]
inline std::string encode_result_set(const ResultSet& rs)
{
    std::string body;
    append_u32(body, static_cast<uint32_t>(rs.cols.size()));
    for (const std::string& col : rs.cols) {
        append_u16(body, static_cast<uint16_t>(col.size()));
        body.append(col);
    }
    append_u32(body, static_cast<uint32_t>(rs.rows.size()));
    for (const std::vector<CellVal>& row : rs.rows) {
        for (const CellVal& cell : row) {
            if (const auto* i = std::get_if<int64_t>(&cell)) {
                body.push_back(static_cast<char>(CELL_INT));
                append_u64(body, static_cast<uint64_t>(*i));
            } else if (const auto* d = std::get_if<double>(&cell)) {
                uint64_t bits = 0;
                std::memcpy(&bits, d, sizeof(bits));
                body.push_back(static_cast<char>(CELL_DOUBLE));
                append_u64(body, bits);
            } else if (const auto* s = std::get_if<std::string>(&cell)) {
                body.push_back(static_cast<char>(CELL_STRING));
                append_u16(body, static_cast<uint16_t>(s->size()));
                body.append(*s);
            } else {
                body.push_back(static_cast<char>(CELL_NULL));
            }
        }
    }
    return body;
}

// ResultSet body 解回; 长度或结构非法(截断/未知 tag/尾部冗余)返回 false
inline bool decode_result_set(std::string_view body, ResultSet& out)
{
    out.cols.clear();
    out.rows.clear();
    std::size_t off = 0;

    // 读取游标: 长度不足返回 false
    const auto take_u16 = [&](uint16_t& v) -> bool {
        if (off + 2 > body.size()) {
            return false;
        }
        v = static_cast<uint16_t>(static_cast<unsigned char>(body[off]) << 8)
          | static_cast<unsigned char>(body[off + 1]);
        off += 2;
        return true;
    };
    const auto take_u32 = [&](uint32_t& v) -> bool {
        if (off + 4 > body.size()) {
            return false;
        }
        v = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            v = (v << 8) | static_cast<unsigned char>(body[off + i]);
        }
        off += 4;
        return true;
    };
    const auto take_u64 = [&](uint64_t& v) -> bool {
        if (off + 8 > body.size()) {
            return false;
        }
        v = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            v = (v << 8) | static_cast<unsigned char>(body[off + i]);
        }
        off += 8;
        return true;
    };
    const auto take_bytes = [&](std::size_t n, std::string_view& v) -> bool {
        if (off + n > body.size()) {
            return false;
        }
        v = body.substr(off, n);
        off += n;
        return true;
    };

    uint32_t col_count = 0;
    if (!take_u32(col_count)) {
        return false;
    }
    for (uint32_t c = 0; c < col_count; ++c) {
        uint16_t len = 0;
        std::string_view name;
        if (!take_u16(len) || !take_bytes(len, name)) {
            return false;
        }
        out.cols.push_back(std::string(name));
    }
    uint32_t row_count = 0;
    if (!take_u32(row_count)) {
        return false;
    }
    for (uint32_t r = 0; r < row_count; ++r) {
        std::vector<CellVal> row;
        row.reserve(out.cols.size());
        for (uint32_t c = 0; c < col_count; ++c) {
            std::string_view one;
            if (!take_bytes(1, one)) {
                return false;
            }
            const auto tag = static_cast<unsigned char>(one.front());
            if (tag == CELL_INT) {
                uint64_t bits = 0;
                if (!take_u64(bits)) {
                    return false;
                }
                row.push_back(static_cast<int64_t>(bits));
            } else if (tag == CELL_DOUBLE) {
                uint64_t bits = 0;
                if (!take_u64(bits)) {
                    return false;
                }
                double d = 0.0;
                std::memcpy(&d, &bits, sizeof(d));
                row.push_back(d);
            } else if (tag == CELL_STRING) {
                uint16_t len = 0;
                std::string_view text;
                if (!take_u16(len) || !take_bytes(len, text)) {
                    return false;
                }
                row.push_back(std::string(text));
            } else if (tag == CELL_NULL) {
                row.emplace_back();
            } else {
                return false;  // 未知 tag
            }
        }
        out.rows.push_back(std::move(row));
    }
    return off == body.size();
}

}  // namespace proto

#endif  // PROTO_PROTO_H
