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
    Query = 1,          // body: SQL 原文
    Ok = 2,             // body: 命令标签(编码见下方命令标签部分)
    Error = 3,          // body: 错误文案
    ResultSetHead = 4,  // body: 结果集列名(编码见下方结果集部分)
    ResultSetBatch = 5, // body: 一批结果行
    ResultSetEnd = 6,   // body: 总行数 u64
};

// 命令完成标签: 非结果集语句的执行语义, 展示格式由 client 决定
enum class CommandTag : uint8_t {
    Empty = 0,       // 空语句(无实际语句)
    CreateTable = 1,
    DropTable = 2,
    Insert = 3,      // count 为插入行数
    Delete = 4,      // count 为删除行数
    CreateSchema = 5,
    DropSchema = 6,
    Set = 7,          // bootstrap 模式变量设置
    Begin = 8,        // 事务开始
    Commit = 9,       // 事务提交
    Rollback = 10,    // 事务回滚
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

// 发一帧: 帧头/类型/body 拼为整帧一次写出
inline bool send_frame(int fd, MsgType type, const std::string& body)
{
    const uint32_t len_net = htonl(1 + static_cast<uint32_t>(body.size()));
    std::string frame;
    frame.reserve(FRAME_HEADER_SIZE + 1 + body.size());
    frame.append(reinterpret_cast<const char*>(&len_net), sizeof(len_net));
    frame.push_back(static_cast<char>(type));
    frame.append(body);
    return send_exact(fd, frame.data(), frame.size());
}

// ==================== 结果集(Head/Batch/End 三帧编解码) ====================

// 结果集单元格值(与 st::Value 同构, proto 层独立定义避免依赖)
using CellVal = std::variant<std::monostate, int64_t, double, std::string>;

// 客户端物化结果集: 列名(来自 Head) + 行值(累积自 Batch)
struct ResultSet {
    std::vector<std::string> cols;
    std::vector<std::vector<CellVal>> rows;
};

// 单批最大行数: 发送侧攒批上限, 接收侧不感知
constexpr uint32_t RS_BATCH_MAX_ROWS = 256;

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

// 单元格编码追加: [tag u8][payload]
inline void append_cell(std::string& out, const CellVal& cell)
{
    if (const auto* i = std::get_if<int64_t>(&cell)) {
        out.push_back(static_cast<char>(CELL_INT));
        append_u64(out, static_cast<uint64_t>(*i));
    } else if (const auto* d = std::get_if<double>(&cell)) {
        uint64_t bits = 0;
        std::memcpy(&bits, d, sizeof(bits));
        out.push_back(static_cast<char>(CELL_DOUBLE));
        append_u64(out, bits);
    } else if (const auto* s = std::get_if<std::string>(&cell)) {
        out.push_back(static_cast<char>(CELL_STRING));
        append_u16(out, static_cast<uint16_t>(s->size()));
        out.append(*s);
    } else {
        out.push_back(static_cast<char>(CELL_NULL));
    }
}

// Head body 布局(大端网络序): [列数 u32] 每列[列名长度 u16][列名]
inline std::string encode_rs_head(const std::vector<std::string>& cols)
{
    std::string body;
    append_u32(body, static_cast<uint32_t>(cols.size()));
    for (const std::string& col : cols) {
        append_u16(body, static_cast<uint16_t>(col.size()));
        body.append(col);
    }
    return body;
}

// Batch body 布局: [行数 u32] 每行每列一个单元格 [tag u8][payload]:
// 0=NULL 无 payload, 1=int64 8B, 2=double 8B(位模式按 u64), 3=string[长度 u16][字节]
inline std::string encode_rs_batch(const std::vector<std::vector<CellVal>>& rows)
{
    std::string body;
    append_u32(body, static_cast<uint32_t>(rows.size()));
    for (const std::vector<CellVal>& row : rows) {
        for (const CellVal& cell : row) {
            append_cell(body, cell);
        }
    }
    return body;
}

// End body 布局: [总行数 u64]
inline std::string encode_rs_end(uint64_t row_count)
{
    std::string body;
    append_u64(body, row_count);
    return body;
}

// 读取游标族: 长度不足返回 false
inline bool take_u16(std::string_view body, std::size_t& off, uint16_t& v)
{
    if (off + 2 > body.size()) {
        return false;
    }
    v = static_cast<uint16_t>(static_cast<unsigned char>(body[off]) << 8)
      | static_cast<unsigned char>(body[off + 1]);
    off += 2;
    return true;
}

inline bool take_u32(std::string_view body, std::size_t& off, uint32_t& v)
{
    if (off + 4 > body.size()) {
        return false;
    }
    v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v = (v << 8) | static_cast<unsigned char>(body[off + i]);
    }
    off += 4;
    return true;
}

inline bool take_u64(std::string_view body, std::size_t& off, uint64_t& v)
{
    if (off + 8 > body.size()) {
        return false;
    }
    v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<unsigned char>(body[off + i]);
    }
    off += 8;
    return true;
}

inline bool take_bytes(std::string_view body, std::size_t& off, std::size_t n, std::string_view& v)
{
    if (off + n > body.size()) {
        return false;
    }
    v = body.substr(off, n);
    off += n;
    return true;
}

// 单元格解码; 长度不足或未知 tag 返回 false
inline bool take_cell(std::string_view body, std::size_t& off, CellVal& out)
{
    std::string_view one;
    if (!take_bytes(body, off, 1, one)) {
        return false;
    }
    const auto tag = static_cast<unsigned char>(one.front());
    if (tag == CELL_INT) {
        uint64_t bits = 0;
        if (!take_u64(body, off, bits)) {
            return false;
        }
        out = static_cast<int64_t>(bits);
    } else if (tag == CELL_DOUBLE) {
        uint64_t bits = 0;
        if (!take_u64(body, off, bits)) {
            return false;
        }
        double d = 0.0;
        std::memcpy(&d, &bits, sizeof(d));
        out = d;
    } else if (tag == CELL_STRING) {
        uint16_t len = 0;
        std::string_view text;
        if (!take_u16(body, off, len) || !take_bytes(body, off, len, text)) {
            return false;
        }
        out = std::string(text);
    } else if (tag == CELL_NULL) {
        out = std::monostate{};
    } else {
        return false;  // 未知 tag
    }
    return true;
}

// Head 解码; 长度或结构非法(截断/尾部冗余)返回 false
inline bool decode_rs_head(std::string_view body, std::vector<std::string>& cols)
{
    cols.clear();
    std::size_t off = 0;
    uint32_t col_count = 0;
    if (!take_u32(body, off, col_count)) {
        return false;
    }
    for (uint32_t c = 0; c < col_count; ++c) {
        uint16_t len = 0;
        std::string_view name;
        if (!take_u16(body, off, len) || !take_bytes(body, off, len, name)) {
            return false;
        }
        cols.push_back(std::string(name));
    }
    return off == body.size();
}

// Batch 解码: 行按 col_count 分格, 行值追加进 out.rows; 非法返回 false
inline bool decode_rs_batch(std::string_view body, uint32_t col_count, ResultSet& out)
{
    std::size_t off = 0;
    uint32_t row_count = 0;
    if (!take_u32(body, off, row_count)) {
        return false;
    }
    for (uint32_t r = 0; r < row_count; ++r) {
        std::vector<CellVal> row;
        row.reserve(col_count);
        for (uint32_t c = 0; c < col_count; ++c) {
            CellVal cell;
            if (!take_cell(body, off, cell)) {
                return false;
            }
            row.push_back(std::move(cell));
        }
        out.rows.push_back(std::move(row));
    }
    return off == body.size();
}

// End 解码; 长度不为 8 返回 false
inline bool decode_rs_end(std::string_view body, uint64_t& row_count)
{
    if (body.size() != 8) {
        return false;
    }
    row_count = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        row_count = (row_count << 8) | static_cast<unsigned char>(body[i]);
    }
    return true;
}

// ==================== 命令标签(Ok body 编解码) ====================

// body 布局: [tag u8][count u64 大端], 共 9 字节
inline std::string encode_command(CommandTag tag, uint64_t count)
{
    std::string body(1, static_cast<char>(tag));
    append_u64(body, count);
    return body;
}

// 命令标签解码; 长度不为 9 或 tag 超出已知值返回 false
inline bool decode_command(std::string_view body, CommandTag& tag, uint64_t& count)
{
    if (body.size() != 9) {
        return false;
    }
    const auto t = static_cast<unsigned char>(body[0]);
    if (t > static_cast<unsigned char>(CommandTag::Rollback)) {
        return false;  // 未知 tag
    }
    tag = static_cast<CommandTag>(t);
    uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<unsigned char>(body[1 + i]);
    }
    count = v;
    return true;
}

}  // namespace proto

#endif  // PROTO_PROTO_H
