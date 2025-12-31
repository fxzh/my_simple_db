#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8123
#define BUFFER_SIZE 1024

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};
    
    // 创建socket文件描述符
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        std::cerr << "Socket创建失败" << std::endl;
        return -1;
    }
    
    // 设置socket选项，允许端口重用
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        std::cerr << "设置socket选项失败" << std::endl;
        return -1;
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    // 绑定socket到地址和端口
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        std::cerr << "绑定端口失败" << std::endl;
        return -1;
    }
    
    // 开始监听连接
    if (listen(server_fd, 3) < 0) {
        std::cerr << "监听失败" << std::endl;
        return -1;
    }
    
    std::cout << "服务器已启动，监听端口 " << PORT << "..." << std::endl;
    std::cout << "等待客户端连接..." << std::endl;
    
    // 接受客户端连接
    if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
        std::cerr << "接受连接失败" << std::endl;
        return -1;
    }
    
    std::cout << "客户端已连接！" << std::endl;
    std::cout << "客户端IP: " << inet_ntoa(address.sin_addr) << std::endl;
    std::cout << "客户端端口: " << ntohs(address.sin_port) << std::endl;
    
    // 持续接收和回显消息
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        
        // 接收客户端消息
        int valread = read(new_socket, buffer, BUFFER_SIZE);
        if (valread <= 0) {
            if (valread == 0) {
                std::cout << "客户端断开连接" << std::endl;
            } else {
                std::cerr << "读取数据失败" << std::endl;
            }
            break;
        }
        
        std::cout << "收到消息: " << buffer << std::endl;
        
        // 检查是否收到退出指令
        if (strcmp(buffer, "quit") == 0 || strcmp(buffer, "exit") == 0) {
            std::cout << "收到退出指令，关闭连接..." << std::endl;
            break;
        }
        
        // 将消息回显给客户端
        send(new_socket, buffer, strlen(buffer), 0);
        std::cout << "已回显消息给客户端" << std::endl;
    }
    
    // 关闭连接
    close(new_socket);
    close(server_fd);
    
    std::cout << "服务器已关闭" << std::endl;
    return 0;
}