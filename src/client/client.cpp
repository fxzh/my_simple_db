#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PORT 8123
#define BUFFER_SIZE 1024

int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE] = {0};
    
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
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "连接服务器失败" << std::endl;
        std::cerr << "请确保服务器已启动" << std::endl;
        return -1;
    }
    
    std::cout << "已连接到服务器！" << std::endl;
    std::cout << "输入消息发送给服务器，输入 'quit' 或 'exit' 退出" << std::endl;
    std::cout << "==========================================" << std::endl;
    
    // 持续发送和接收消息
    while (true) {
        std::string message;
        
        // 获取用户输入
        std::cout << "请输入消息: ";
        std::getline(std::cin, message);
        
        // 检查退出指令
        if (message == "quit" || message == "exit") {
            send(sock, message.c_str(), message.length(), 0);
            std::cout << "正在断开连接..." << std::endl;
            break;
        }
        
        // 发送消息到服务器
        send(sock, message.c_str(), message.length(), 0);
        std::cout << "已发送消息: " << message << std::endl;
        
        // 接收服务器回显
        memset(buffer, 0, BUFFER_SIZE);
        int valread = read(sock, buffer, BUFFER_SIZE);
        
        if (valread <= 0) {
            std::cerr << "服务器连接已断开" << std::endl;
            break;
        }
        
        std::cout << "服务器回显: " << buffer << std::endl;
    }
    
    // 关闭socket
    close(sock);
    std::cout << "连接已关闭" << std::endl;
    
    return 0;
}