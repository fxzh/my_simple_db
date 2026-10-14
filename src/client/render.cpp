// render.cpp: 渲染族实现: 回复帧文本展示, 结果集表格渲染与流式接收
#include "render.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#include "client.h"
#include "log/log.h"

// NOTICE_LEVEL_MAX 与 LogLevel 末位一致, 漂移时编译报错
static_assert(static_cast<int>(LogLevel::CRITICAL) == static_cast<int>(proto::NOTICE_LEVEL_MAX));

void print_notice(uint8_t level, const std::string& text)
{
    std::cout << levelToString(static_cast<LogLevel>(level)) << ": " << text << std::endl;
}

namespace {

// 定点十进制渲染: 16B 大端补码转十进制, 按 scale 插入小数点, 尾零保留
std::string decimal_text(const proto::DecimalCell& c)
{
    uint8_t b[16];
    std::memcpy(b, c.bytes, sizeof(b));
    bool neg = (b[0] & 0x80) != 0;
    if (neg) {
        // 模 2^128 取负: 逐字节取反加一
        bool carry = true;
        for (int i = 15; i >= 0; --i) {
            b[i] = static_cast<uint8_t>(~b[i]);
            if (carry) {
                ++b[i];
                carry = b[i] == 0;
            }
        }
    }
    std::string digits;  // 逐位除 10 自低位收集
    bool any = false;
    for (int i = 0; i < 16; ++i) {
        any = any || b[i] != 0;
    }
    while (any) {
        uint32_t carry = 0;
        any = false;
        for (int i = 0; i < 16; ++i) {
            const uint32_t cur = carry * 256 + b[i];
            b[i] = static_cast<uint8_t>(cur / 10);
            carry = cur % 10;
            any = any || b[i] != 0;
        }
        digits += static_cast<char>('0' + carry);
    }
    if (digits.empty()) {
        digits = "0";
    }
    std::reverse(digits.begin(), digits.end());
    if (c.scale == 0) {
        return neg ? "-" + digits : digits;
    }
    if (digits.size() <= c.scale) {
        digits.insert(0, static_cast<size_t>(c.scale) - digits.size() + 1, '0');
    }
    std::string out;
    if (neg) {
        out += '-';
    }
    out.append(digits, 0, digits.size() - static_cast<size_t>(c.scale));
    out += '.';
    out.append(digits, digits.size() - static_cast<size_t>(c.scale));
    return out;
}

// 单元格显示文本: NULL 显示 NULL, 整数十进制, 浮点最短表示, 定点十进制按标度,
// 字符串原样, 布尔 true/false
std::string cell_text(const proto::CellVal& cell)
{
    if (const auto* i = std::get_if<int64_t>(&cell)) {
        return std::format("{}", *i);
    }
    if (const auto* d = std::get_if<double>(&cell)) {
        return std::format("{}", *d);
    }
    if (const auto* f = std::get_if<float>(&cell)) {
        return std::format("{}", *f);
    }
    if (const auto* s = std::get_if<std::string>(&cell)) {
        return *s;
    }
    if (const auto* dc = std::get_if<proto::DecimalCell>(&cell)) {
        return decimal_text(*dc);
    }
    if (const auto* b = std::get_if<bool>(&cell)) {
        return *b ? "true" : "false";
    }
    return "NULL";
}

}  // namespace

std::string command_display(proto::CommandTag tag, uint64_t count)
{
    switch (tag) {
    case proto::CommandTag::CreateTable:
        return "CREATE";
    case proto::CommandTag::DropTable:
        return "DROP";
    case proto::CommandTag::CreateSchema:
        return "CREATE";
    case proto::CommandTag::DropSchema:
        return "DROP";
    case proto::CommandTag::CreateIndex:
        return "CREATE";
    case proto::CommandTag::DropIndex:
        return "DROP";
    case proto::CommandTag::Insert:
        return "INSERT " + std::to_string(count);
    case proto::CommandTag::Delete:
        return "DELETE " + std::to_string(count);
    case proto::CommandTag::Update:
        return "UPDATE " + std::to_string(count);
    case proto::CommandTag::Set:
        return "SET";
    case proto::CommandTag::Begin:
        return "BEGIN";
    case proto::CommandTag::Commit:
        return "COMMIT";
    case proto::CommandTag::Rollback:
        return "ROLLBACK";
    case proto::CommandTag::Empty:
        return "";
    }
    return "";  // 不可达: 全部标签已在上方穷尽
}

void render_result_set(const proto::ResultSet& rs)
{
    const std::size_t ncol = rs.cols.size();
    std::vector<std::size_t> widths(ncol, 0);
    for (std::size_t c = 0; c < ncol; ++c) {
        widths[c] = rs.cols[c].size();
    }
    std::vector<std::vector<std::string>> cells;
    cells.reserve(rs.rows.size());
    for (const std::vector<proto::CellVal>& row : rs.rows) {
        std::vector<std::string> texts;
        texts.reserve(ncol);
        for (std::size_t c = 0; c < ncol; ++c) {
            std::string text = cell_text(row[c]);
            if (text.size() > widths[c]) {
                widths[c] = text.size();
            }
            texts.push_back(std::move(text));
        }
        cells.push_back(std::move(texts));
    }

    // 输出一行: 每列前后各一空格且补齐列宽, 列间 '|' 连接, 末列不补尾空格; 居中时前导补 (列宽-文本宽)/2 空格
    const auto print_row = [&](const std::vector<std::string>& texts, bool centered) {
        for (std::size_t c = 0; c < ncol; ++c) {
            if (c > 0) {
                std::cout << '|';
            }
            const std::size_t pad = widths[c] > texts[c].size() ? widths[c] - texts[c].size() : 0;
            const std::size_t lead = centered ? pad / 2 : 0;
            std::cout << ' ' << std::string(lead, ' ') << texts[c];
            if (c + 1 < ncol) {
                std::cout << std::string(pad - lead, ' ') << ' ';
            }
        }
        std::cout << "\n";
    };

    print_row(rs.cols, true);

    // 空结果: 仅表头 + 行数
    if (rs.rows.empty()) {
        std::cout << "(0 行)" << std::endl;
        return;
    }

    // 表头下分隔行: 每列 '-' × (列宽+2), 以 '+' 连接
    for (std::size_t c = 0; c < ncol; ++c) {
        if (c > 0) {
            std::cout << '+';
        }
        std::cout << std::string(widths[c] + 2, '-');
    }
    std::cout << "\n";

    for (const std::vector<std::string>& texts : cells) {
        print_row(texts, false);
    }
    std::cout << "(" << rs.rows.size() << " 行)" << std::endl;
}

bool recv_result_stream(proto::ResultSet& rs)
{
    for (;;) {
        proto::MsgType t;
        std::string b;
        if (!proto::recv_frame(sock, 0, t, b)) {
            std::cerr << "服务器连接已断开" << std::endl;
            return false;
        }
        if (t == proto::MsgType::ResultSetBatch) {
            if (!proto::decode_rs_batch(b, static_cast<uint32_t>(rs.cols.size()), rs)) {
                std::cerr << "错误: 结果集解码失败" << std::endl;
                return false;
            }
        } else if (t == proto::MsgType::ResultSetEnd) {
            uint64_t total = 0;
            if (!proto::decode_rs_end(b, total) || total != rs.rows.size()) {
                std::cerr << "错误: 结果集解码失败" << std::endl;
                return false;
            }
            return true;
        } else if (t == proto::MsgType::Error) {
            // 语句中途失败, 已收部分行不展示; 码只做解码校验不展示
            proto::WireErrCode code;
            std::string text;
            if (!proto::decode_error(b, code, text)) {
                std::cerr << "错误: 错误帧解码失败" << std::endl;
                return false;
            }
            std::cout << "ERROR: " << text << std::endl;
            return false;
        } else if (t == proto::MsgType::Notice) {
            // 流中消息帧: 打印后继续接收
            uint8_t level = 0;
            std::string text;
            if (!proto::decode_notice(b, level, text)) {
                std::cerr << "错误: 消息帧解码失败" << std::endl;
                return false;
            }
            print_notice(level, text);
        } else {
            std::cerr << "错误: 收到未实现的消息类型: "
                      << static_cast<unsigned int>(t) << std::endl;
            return false;
        }
    }
}
