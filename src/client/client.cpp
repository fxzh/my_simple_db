#include <format>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <FlexLexer.h>
#include "parse_args.h"
#include "client.h"
#include "proto/proto.h"

int sock = 0;
ScannerState scanner_state = STATE_INITIAL;
std::string sql_buffer;
bool sql_overflow = false;
// -c 模式退出码依据: 任一语句出错即置位
bool sql_failed = false;

// 跨行累积依赖扫描器的 start condition 记忆, 实例全程复用
std::unique_ptr<yyFlexLexer> lexer;

void reset_sql_buffer()
{
    sql_buffer.clear();
    sql_overflow = false;
}

void append_to_sql(const char* text, std::size_t len)
{
    if (sql_overflow) {
        // 已超限, 跳过后续累积
        return;
    }
    if (sql_buffer.size() + len > SQL_BUFFER_LIMIT) {
        std::cerr << "错误: SQL 超过上限 " << SQL_BUFFER_LIMIT << " 字节, 本轮输入已丢弃" << std::endl;
        sql_buffer.clear();
        sql_overflow = true;
        sql_failed = true;
        return;
    }
    sql_buffer.append(text, len);
}

// 单元格显示文本: NULL 显示 NULL, 整数十进制, 浮点最短表示, 字符串原样
std::string cell_text(const proto::CellVal& cell)
{
    if (const auto* i = std::get_if<int64_t>(&cell)) {
        return std::format("{}", *i);
    }
    if (const auto* d = std::get_if<double>(&cell)) {
        return std::format("{}", *d);
    }
    if (const auto* s = std::get_if<std::string>(&cell)) {
        return *s;
    }
    return "NULL";
}

// 结果集渲染为表格: 列宽取表头与各单元格的最大字节宽, 全左对齐,
// 每列前后各一空格且补齐列宽, 列间 '|' 分隔, 表头下每列 '-' × (列宽+2) 以 '+' 连接,
// 末列不补尾空格, 末行输出 (N 行)
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

    // 输出一行: 每列前后各一空格且补齐列宽, 列间 '|' 连接, 末列不补尾空格
    const auto print_row = [&](const std::vector<std::string>& texts) {
        for (std::size_t c = 0; c < ncol; ++c) {
            if (c > 0) {
                std::cout << '|';
            }
            std::cout << ' ' << texts[c];
            if (c + 1 < ncol) {
                if (texts[c].size() < widths[c]) {
                    std::cout << std::string(widths[c] - texts[c].size(), ' ');
                }
                std::cout << ' ';
            }
        }
        std::cout << "\n";
    };

    print_row(rs.cols);

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
        print_row(texts);
    }
    std::cout << "(" << rs.rows.size() << " 行)" << std::endl;
}

void send_to_server()
{
    if (sql_overflow) {
        // 超限错误已在上限触发时报出, 这里只跳过发送流程
        reset_sql_buffer();
        return;
    }
    if (sql_buffer.empty()) {
        return;
    }

    std::cout << "已发送消息: " << sql_buffer << std::endl;
    // 整条 SQL 组帧发送
    if (!proto::send_frame(sock, proto::MsgType::Query, sql_buffer)) {
        std::cerr << "服务器连接已断开" << std::endl;
        sql_failed = true;
        reset_sql_buffer();
        return;
    }
    add_history(sql_buffer.c_str());
    reset_sql_buffer();

    // 接收服务器回复整帧
    proto::MsgType type;
    std::string body;
    if (!proto::recv_frame(sock, 0, type, body)) {
        std::cerr << "服务器连接已断开" << std::endl;
        sql_failed = true;
        return;
    }

    // Ok 原样回显, Error 补前缀打印, ResultSet 渲染为表格
    if (type == proto::MsgType::Error) {
        sql_failed = true;
        std::cout << "服务器回显: ERROR: " << body << std::endl;
    } else if (type == proto::MsgType::Ok) {
        std::cout << "服务器回显: " << body << std::endl;
    } else if (type == proto::MsgType::ResultSet) {
        proto::ResultSet rs;
        if (!proto::decode_result_set(body, rs)) {
            std::cerr << "错误: 结果集解码失败" << std::endl;
            sql_failed = true;
        } else {
            render_result_set(rs);
        }
    } else {
        std::cerr << "错误: 收到未实现的消息类型: "
                  << static_cast<unsigned int>(type) << std::endl;
        sql_failed = true;
    }
}

void process_input(std::string& input)
{
    if (input.empty()) {
        return;
    }

    // 构造带终止换行的输入流（readline 返回的不包含 '\n'）
    std::istringstream stream(input + "\n");
    lexer->yyrestart(&stream);
    lexer->yylex();
}

int main(int argc, char* argv[])
{
    struct sockaddr_in serv_addr;

    Options opts;
    if (!parse_args(argc, argv, opts)) {
        return -1;
    }

    // 创建socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Socket创建失败" << std::endl;
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(static_cast<in_port_t>(opts.port));

    // 转换IP地址
    if (inet_pton(AF_INET, opts.host.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cerr << "无效地址/地址不支持" << std::endl;
        return -1;
    }

    // 连接服务器
    if (connect(sock, reinterpret_cast<sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0) {
        std::cerr << "连接服务器失败" << std::endl;
        std::cerr << "请确保服务器已启动" << std::endl;
        return -1;
    }

    // -c 模式面向脚本执行, 不打印交互横幅
    if (opts.sql.empty()) {
        std::cout << "已连接到服务器！" << std::endl;
        std::cout << "输入消息发送给服务器，输入 'quit' 或 'exit' 退出" << std::endl;
        std::cout << "==========================================" << std::endl;
    }

    // 连接成功后创建扫描器, 供每行输入复用
    lexer = std::make_unique<yyFlexLexer>(nullptr, nullptr);

    // -c 模式: 整段输入按 ';' 逐条发送, 末尾无 ';' 的残留缓冲补上终结符后发送, 按执行结果定退出码
    if (!opts.sql.empty()) {
        process_input(opts.sql);
        if (!sql_buffer.empty()) {
            append_to_sql(";", 1);
            send_to_server();
        }
        close(sock);
        return sql_failed ? 1 : 0;
    }

    // 持续发送和接收消息
    while (true) {
        const char* prompt = "SQL> ";
        if (scanner_state == STATE_SINGLE) {
            prompt = "SQL>' ";
        } else if (scanner_state == STATE_DOUBLE) {
            prompt = "SQL>\" ";
        }

        // 获取用户输入
        char* line = readline(prompt);
        if (line == nullptr) {
            // EOF(Ctrl-D) 直接断开
            std::cout << "正在断开连接..." << std::endl;
            break;
        }
        std::string message = std::string(line);
        free(line);

        // 检查退出指令
        if (message == "quit" || message == "exit") {
            proto::send_frame(sock, proto::MsgType::Query, message);
            std::cout << "正在断开连接..." << std::endl;
            break;
        }

        // 处理输入
        process_input(message);
    }

    // 关闭socket
    close(sock);
    std::cout << "连接已关闭" << std::endl;

    return 0;
}