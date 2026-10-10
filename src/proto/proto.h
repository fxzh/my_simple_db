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
constexpr uint32_t MAX_REQUEST_PAYLOAD = 268435456;

// 消息类型: Query 为请求方向, 其余为响应方向
enum class MsgType : uint8_t {
    Query = 1,          // body: SQL 原文
    Ok = 2,             // body: 命令标签(编码见下方命令标签部分)
    Error = 3,          // body: [错误码 u16][文案](编码见下方错误帧部分)
    ResultSetHead = 4,  // body: 结果集列名(编码见下方结果集部分)
    ResultSetBatch = 5, // body: 一批结果行
    ResultSetEnd = 6,   // body: 总行数 u64
    Notice = 7,         // body: [消息级别 u8][文案](编码见下方消息帧部分)
};

// 命令完成标签: 非结果集语句的执行语义, 展示格式由 client 决定
enum class CommandTag : uint8_t {
    Empty = 0,       // 空语句
    CreateTable = 1,
    DropTable = 2,
    Insert = 3,      // count 为插入行数
    Delete = 4,      // count 为删除行数
    CreateSchema = 5,
    DropSchema = 6,
    Set = 7,          // 变量设置
    Begin = 8,        // 事务开始
    Commit = 9,       // 事务提交
    Rollback = 10,    // 事务回滚
    CreateIndex = 11, // 建索引
    DropIndex = 12,   // 删索引
    Update = 13,      // count 为匹配行数
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

// 结果集单元格值(proto 层独立定义避免依赖, 与 st::Value 不同构: float 独立备选,
// 无语义类型标签)
using CellVal = std::variant<std::monostate, bool, int64_t, double, float, std::string>;

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
constexpr uint8_t CELL_BOOL = 4;
constexpr uint8_t CELL_FLOAT = 5;

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
    } else if (const auto* b = std::get_if<bool>(&cell)) {
        out.push_back(static_cast<char>(CELL_BOOL));
        out.push_back(static_cast<char>(*b ? 1 : 0));
    } else if (const auto* f = std::get_if<float>(&cell)) {
        uint32_t bits = 0;
        std::memcpy(&bits, f, sizeof(bits));
        out.push_back(static_cast<char>(CELL_FLOAT));
        append_u32(out, bits);
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
// 0=NULL 无 payload, 1=int64 8B, 2=double 8B(位模式按 u64), 3=string[长度 u16][字节],
// 4=bool 1B(0/1), 5=float 4B(位模式按 u32)
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
    } else if (tag == CELL_BOOL) {
        if (!take_bytes(body, off, 1, one)) {
            return false;
        }
        const auto b = static_cast<unsigned char>(one.front());
        if (b > 1) {
            return false;  // 非 0/1 视为非法
        }
        out = (b != 0);
    } else if (tag == CELL_FLOAT) {
        uint32_t bits = 0;
        if (!take_u32(body, off, bits)) {
            return false;
        }
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        out = f;
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

// 命令标签解码; 长度不为 9 或 tag 未定义返回 false; 穷尽 switch, 新增 tag 未同步时 -Wswitch 报警
inline bool decode_command(std::string_view body, CommandTag& tag, uint64_t& count)
{
    if (body.size() != 9) {
        return false;
    }
    const CommandTag t = static_cast<CommandTag>(static_cast<unsigned char>(body[0]));
    switch (t) {
    case CommandTag::Empty:
    case CommandTag::CreateTable:
    case CommandTag::DropTable:
    case CommandTag::Insert:
    case CommandTag::Delete:
    case CommandTag::CreateSchema:
    case CommandTag::DropSchema:
    case CommandTag::Set:
    case CommandTag::Begin:
    case CommandTag::Commit:
    case CommandTag::Rollback:
    case CommandTag::CreateIndex:
    case CommandTag::DropIndex:
    case CommandTag::Update:
        tag = t;
        count = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            count = (count << 8) | static_cast<unsigned char>(body[1 + i]);
        }
        return true;
    }
    return false;  // 未定义 tag
}

// ==================== 错误帧(Error body 编解码) ====================

// wire 错误码: 数值独立编址, 与 db::ErrCode 一一对应, 转换收敛在 server 的穷尽 switch
enum class WireErrCode : uint16_t {
    // 以下注释禁止添加括号与括号内的额外说明
    IoError = 1,        // 文件/IO 类失败
    CatalogMissing = 3, // 元数据表缺失
    CatalogExists = 4,  // 元数据表已存在
    InvalidType = 6,    // 列类型非法
    TableNotFound = 7,  // 表不存在
    TableExists = 8,    // 表已存在
    SchemaNotFound = 9, // schema 不存在
    SchemaExists = 10,  // schema 已存在
    SchemaNotEmpty = 11, // 非空 schema 禁止删除
    ProtectedTable = 12, // 保留段表禁止删除
    InvalidDdl = 13,     // 建表参数非法
    ValueMismatch = 14,  // 插入值与列类型不匹配
    RecordTooLong = 15,  // 记录超长
    ArithError = 16,     // 算术溢出/除零
    UnknownColumn = 17,  // 引用了不存在的列
    UnknownVar = 18,     // SET 引用了不存在的变量
    UnknownStmt = 19,    // 未知语句种类
    NotImplemented = 20, // 功能未实现
    Internal = 21,       // 内部不变量违反
    SyntaxError = 22,    // SQL 解析失败
    TxnActive = 23,      // 事务已在进行中
    NoActiveTxn = 24,    // 无活动事务
    DdlInTxn = 25,       // 事务内 DDL/SET 被拒
    BootstrapMode = 26,  // bootstrap 模式限制
    TooManyClients = 27, // 连接数超限
    InvalidVarValue = 28, // SET 变量值非法
    IndexExists = 29,   // 索引已存在
    IndexNotFound = 30, // 索引不存在
    StarNoFrom = 31,    // 无 FROM 的 SELECT 不允许星号
};

// Error body 布局: [code u16 大端][错误文案(余量全体)]
inline std::string encode_error(WireErrCode code, const std::string& message)
{
    std::string body;
    append_u16(body, static_cast<uint16_t>(code));
    body.append(message);
    return body;
}

// 错误帧解码; 长度不足或码值未定义返回 false; 穷尽 switch, 新增码未同步时 -Wswitch 报警
inline bool decode_error(std::string_view body, WireErrCode& code, std::string& message)
{
    std::size_t off = 0;
    uint16_t v = 0;
    if (!take_u16(body, off, v)) {
        return false;
    }
    switch (static_cast<WireErrCode>(v)) {
    case WireErrCode::IoError:
    case WireErrCode::CatalogMissing:
    case WireErrCode::CatalogExists:
    case WireErrCode::InvalidType:
    case WireErrCode::TableNotFound:
    case WireErrCode::TableExists:
    case WireErrCode::SchemaNotFound:
    case WireErrCode::SchemaExists:
    case WireErrCode::SchemaNotEmpty:
    case WireErrCode::ProtectedTable:
    case WireErrCode::InvalidDdl:
    case WireErrCode::ValueMismatch:
    case WireErrCode::RecordTooLong:
    case WireErrCode::ArithError:
    case WireErrCode::UnknownColumn:
    case WireErrCode::UnknownVar:
    case WireErrCode::UnknownStmt:
    case WireErrCode::NotImplemented:
    case WireErrCode::Internal:
    case WireErrCode::SyntaxError:
    case WireErrCode::TxnActive:
    case WireErrCode::NoActiveTxn:
    case WireErrCode::DdlInTxn:
    case WireErrCode::BootstrapMode:
    case WireErrCode::TooManyClients:
    case WireErrCode::InvalidVarValue:
    case WireErrCode::IndexExists:
    case WireErrCode::IndexNotFound:
    case WireErrCode::StarNoFrom:
        code = static_cast<WireErrCode>(v);
        message.assign(body.substr(off));
        return true;
    }
    return false;  // 未定义码(含 0 与历史空洞)
}

// ==================== 消息帧(Notice 编解码) ====================

// 消息级别值与 log.h 的 LogLevel 枚举值一致(DEBUG5=0 .. CRITICAL=9),
// 发送与否由 server 按会话变量过滤, client 收到即渲染
constexpr uint8_t NOTICE_LEVEL_MAX = 9;

// Notice body 布局: [级别 u8][文案(余量全体)]
inline std::string encode_notice(uint8_t level, const std::string& message)
{
    std::string body(1, static_cast<char>(level));
    body.append(message);
    return body;
}

// Notice 解码; 级别越界返回 false
inline bool decode_notice(std::string_view body, uint8_t& level, std::string& message)
{
    if (body.empty() || static_cast<unsigned char>(body[0]) > NOTICE_LEVEL_MAX) {
        return false;
    }
    level = static_cast<uint8_t>(body[0]);
    message.assign(body.substr(1));
    return true;
}

}  // namespace proto

#endif  // PROTO_PROTO_H
