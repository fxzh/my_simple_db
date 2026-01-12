#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#define PORT 8123
#define BUFFER_SIZE 1024

extern "C" {
    #include "client.h"
    extern int yylex();
    extern void* yy_scan_string(const char* s);
    extern void yy_delete_buffer(void* b);
}

int sock = 0;
char buffer[BUFFER_SIZE] = {0};
enum ScannerState scanner_state = STATE_INITIAL;

void send_to_server() {
    if (sql_pos > 0) {
        std::cout << "已发送消息: " << sql_buffer << std::endl;
        send(sock, sql_buffer, sql_pos, 0);
        add_history(sql_buffer);
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
}

void process_input(std::string& input) {
    if (input.empty()) {
        return;
    }
    
    // 构造带终止换行的临时字符串（readline 返回的不包含 '\n'）
    std::string tmp = input + "\n";

    void* buf = yy_scan_string(tmp.c_str());
    yylex();
    yy_delete_buffer(buf);
}

int main() {
    struct sockaddr_in serv_addr;
    
    // 创建socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Socket创建失败" << std::endl;
        return -1;
    }
    
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    
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

// flex需要的其他函数
extern "C" int yywrap() {
    return 1; // 表示输入结束
}

extern "C" void yyerror(const char* s) {
    fprintf(stderr, "解析错误: %s\n", s);
}