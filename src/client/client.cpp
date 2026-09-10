#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <charconv>
#include <system_error>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <FlexLexer.h>
#include "client.h"

#define DEFAULT_PORT 8123
#define BUFFER_SIZE 1024

int sock = 0;
char buffer[BUFFER_SIZE] = {0};
ScannerState scanner_state = STATE_INITIAL;
std::string sql_buffer;
bool sql_overflow = false;

// 跨行累积依赖扫描器的 start condition 记忆, 实例全程复用
std::unique_ptr<yyFlexLexer> lexer;

void reset_sql_buffer() {
    sql_buffer.clear();
    sql_overflow = false;
}

void append_to_sql(const char* text, std::size_t len) {
    if (sql_overflow) {
        // 已超限, 跳过后续累积
        return;
    }
    if (sql_buffer.size() + len > SQL_BUFFER_LIMIT) {
        std::cerr << "错误: SQL 超过上限 " << SQL_BUFFER_LIMIT << " 字节, 本轮输入已丢弃" << std::endl;
        sql_buffer.clear();
        sql_overflow = true;
        return;
    }
    sql_buffer.append(text, len);
}

void send_to_server() {
    if (sql_overflow) {
        // 超限错误已在上限触发时报出, 这里只跳过发送流程
        reset_sql_buffer();
        return;
    }
    if (sql_buffer.empty()) {
        return;
    }

    std::cout << "已发送消息: " << sql_buffer << std::endl;
    send(sock, sql_buffer.c_str(), sql_buffer.size(), 0);
    add_history(sql_buffer.c_str());
    reset_sql_buffer();

    // 接收服务器回显
    memset(buffer, 0, BUFFER_SIZE);
    auto valread = read(sock, buffer, BUFFER_SIZE);

    if (valread <= 0) {
        std::cerr << "服务器连接已断开" << std::endl;
        return;
    }

    std::cout << "服务器回显: " << buffer << std::endl;
}

void process_input(std::string& input) {
    if (input.empty()) {
        return;
    }

    // 构造带终止换行的输入流（readline 返回的不包含 '\n'）
    std::istringstream stream(input + "\n");
    lexer->yyrestart(&stream);
    lexer->yylex();
}

// 打印命令行用法
void print_usage(const char* prog) {
    std::cerr << "用法: " << prog << " [-p 端口号]" << std::endl;
}

int main(int argc, char* argv[]) {
    struct sockaddr_in serv_addr;

    // 解析 -p 端口参数, 支持 -p8123 与 -p 8123 两种形式, 缺省 8123
    int port = DEFAULT_PORT;
    bool port_set = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        std::string_view value;
        if (arg == "-p") {
            if (i + 1 >= argc) {
                std::cerr << "错误: -p 后缺少端口号" << std::endl;
                print_usage(argv[0]);
                return -1;
            }
            value = argv[++i];
        } else if (arg.size() > 2 && arg.starts_with("-p")) {
            value = arg.substr(2);
        } else {
            std::cerr << "错误: 未知参数: " << arg << std::endl;
            print_usage(argv[0]);
            return -1;
        }
        if (port_set) {
            std::cerr << "错误: 重复指定端口" << std::endl;
            print_usage(argv[0]);
            return -1;
        }
        int parsed = 0;
        const char* begin = value.data();
        auto result = std::from_chars(begin, begin + value.size(), parsed);
        if (result.ec != std::errc() || result.ptr != begin + value.size()) {
            std::cerr << "错误: 端口值非法: " << value << std::endl;
            print_usage(argv[0]);
            return -1;
        }
        if (parsed < 1 || parsed > 65535) {
            std::cerr << "错误: 端口超出范围 1~65535: " << parsed << std::endl;
            print_usage(argv[0]);
            return -1;
        }
        port = parsed;
        port_set = true;
    }

    // 创建socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Socket创建失败" << std::endl;
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(static_cast<in_port_t>(port));

    // 转换IP地址
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        std::cerr << "无效地址/地址不支持" << std::endl;
        return -1;
    }

    // 连接服务器
    if (connect(sock, reinterpret_cast<sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0) {
        std::cerr << "连接服务器失败" << std::endl;
        std::cerr << "请确保服务器已启动" << std::endl;
        return -1;
    }

    std::cout << "已连接到服务器！" << std::endl;
    std::cout << "输入消息发送给服务器，输入 'quit' 或 'exit' 退出" << std::endl;
    std::cout << "==========================================" << std::endl;

    // 连接成功后创建扫描器, 供每行输入复用
    lexer = std::make_unique<yyFlexLexer>(nullptr, nullptr);

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
            send(sock, message.c_str(), message.length(), 0);
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