#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "log/log.h"

#define PORT 8123
#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024

using enum LogModule;
using enum LogLevel;

// 客户端连接信息
struct ClientInfo {
    int socket;
    int client_id;
    std::string ip_address;
    std::thread thread;
    
    ClientInfo(int sock, int id, const std::string& ip) 
        : socket(sock), client_id(id), ip_address(ip) {}
    
    ~ClientInfo() {
        if (thread.joinable()) {
            thread.detach();
        }
    }
};

// 全局变量
std::vector<std::shared_ptr<ClientInfo>> clients;
std::mutex clients_mutex;
std::atomic<int> client_counter{0};
std::atomic<bool> server_running{true};
std::mutex cout_mutex;  // 保护标准输出

// 线程安全的输出
void safe_cout(const std::string& message) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << message << std::endl;
}

// 处理单个客户端的函数
void handle_client(int client_socket, int client_id, const std::string& client_ip) {
    char buffer[BUFFER_SIZE] = {0};
    char client_name[BUFFER_SIZE];
    
    // 首次读取客户端名称
    auto name_read = read(client_socket, client_name, BUFFER_SIZE - 1);
    if (name_read <= 0) {
        close(client_socket);
        return;
    }
    client_name[name_read] = '\0';
    
    std::string welcome_msg = "客户端 [" + std::string(client_name) + 
                              "] ID:" + std::to_string(client_id) + 
                              " 已连接 (" + client_ip + ")";
    LOG(INFO, NETWORK, "%s", welcome_msg.c_str());
    
    // 发送欢迎消息
    std::string welcome_client = "欢迎 " + std::string(client_name) + 
                                 "! 你是第 " + std::to_string(client_id) + 
                                 " 个连接。发送 'quit' 或 'exit' 退出。";
    send(client_socket, welcome_client.c_str(), welcome_client.length(), 0);
    
    // 处理客户端消息循环
    while (server_running) {
        try {
        memset(buffer, 0, BUFFER_SIZE);
        
        // 接收客户端消息
        auto valread = read(client_socket, buffer, BUFFER_SIZE - 1);
        if (valread <= 0) {
            if (valread == 0) {
                std::string disconnect_msg = "客户端 [" + std::string(client_name) + 
                                             "] ID:" + std::to_string(client_id) + " 断开连接";
                LOG(INFO, NETWORK, "%s", disconnect_msg.c_str());
            } else {
                std::string error_msg = "从客户端 [" + std::string(client_name) + 
                                       "] ID:" + std::to_string(client_id) + " 读取数据失败";
                LOG(WARNING, NETWORK, "%s", error_msg.c_str());
            }
            break;
        }
        
        std::string msg_str(buffer);
        std::string log_msg = "来自 [" + std::string(client_name) + 
                             "] ID:" + std::to_string(client_id) + " 的消息: " + msg_str;
        LOG(INFO, NETWORK, "%s", log_msg.c_str());
        
        // 检查是否收到退出指令
        if (msg_str == "quit" || msg_str == "exit") {
            std::string goodbye_msg = "再见，" + std::string(client_name) + "!";
            send(client_socket, goodbye_msg.c_str(), goodbye_msg.length(), 0);
            
            std::string leave_msg = "客户端 [" + std::string(client_name) + 
                                   "] ID:" + std::to_string(client_id) + " 主动退出";
            LOG(INFO, NETWORK, "%s", leave_msg.c_str());
            break;
        }
        
        // 处理特殊指令
        if (msg_str == "list") {
            std::lock_guard<std::mutex> lock(clients_mutex);
            std::string list_msg = "当前在线客户端 (" + std::to_string(clients.size()) + " 个):\n";
            for (const auto& client : clients) {
                if (client->socket != client_socket) {
                    list_msg += "  ID:" + std::to_string(client->client_id) + 
                               " [" + client->ip_address + "]\n";
                }
            }
            if (clients.size() <= 1) {
                list_msg += "  没有其他客户端在线\n";
            }
            send(client_socket, list_msg.c_str(), list_msg.length(), 0);
            continue;
        }

        // 模拟错误
        if (msg_str == "error;") {
            LOG(ERROR, NETWORK, "模拟错误触发于客户端 [%s] ID:%d", client_name, client_id);
        }
        
        if (msg_str == "help") {
            std::string help_msg = "可用命令:\n"
                                  "  help     - 显示帮助信息\n"
                                  "  list     - 显示在线客户端列表\n"
                                  "  quit/exit - 退出连接\n"
                                  "  其他消息 - 服务器会回显您的消息";
            send(client_socket, help_msg.c_str(), help_msg.length(), 0);
            continue;
        }
        
        // 普通消息：回显给客户端
        std::string echo_msg = "服务器回显: " + msg_str;
        send(client_socket, echo_msg.c_str(), echo_msg.length(), 0);
        } catch (const std::exception& e) {
            std::string error_log = "处理客户端 [" + std::string(client_name) + 
                                    "] ID:" + std::to_string(client_id) + 
                                    " 时发生异常: " + e.what();
            safe_cout(error_log);
            send(client_socket, e.what(), strlen(e.what()), 0);
        }
    }
    
    // 清理客户端连接
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if ((*it)->socket == client_socket) {
                clients.erase(it);
                break;
            }
        }
    }
    
    // 输出当前客户端数量
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        std::string count_msg = "当前在线客户端数量: " + std::to_string(clients.size());
        safe_cout(count_msg);
    }
    
    close(client_socket);
}

// 清理已完成的线程
void cleanup_threads() {
    std::lock_guard<std::mutex> lock(clients_mutex);
    auto it = clients.begin();
    while (it != clients.end()) {
        if (!(*it)->thread.joinable()) {
            it = clients.erase(it);
        } else {
            ++it;
        }
    }
}

// 服务器主函数
int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    
    // 创建socket文件描述符
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        std::cerr << "Socket创建失败" << std::endl;
        return -1;
    }
    
    // 设置socket选项
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        std::cerr << "设置socket选项失败" << std::endl;
        return -1;
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    // 绑定socket到地址和端口
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "绑定端口失败" << std::endl;
        close(server_fd);
        return -1;
    }
    
    // 开始监听连接
    if (listen(server_fd, 10) < 0) {  // 增加等待队列长度
        std::cerr << "监听失败" << std::endl;
        close(server_fd);
        return -1;
    }
    
    std::cout << "服务器已启动，监听端口 " << PORT << "..." << std::endl;
    std::cout << "支持最多 " << MAX_CLIENTS << " 个客户端同时连接" << std::endl;
    std::cout << "等待客户端连接..." << std::endl;
    
    // 主循环：接受客户端连接
    while (server_running) {
        // 接受客户端连接
        new_socket = accept(server_fd, reinterpret_cast<sockaddr*>(&address), reinterpret_cast<socklen_t*>(&addrlen));
        if (new_socket < 0) {
            if (!server_running) {
                break;  // 服务器正在关闭
            }
            std::cerr << "接受连接失败" << std::endl;
            continue;
        }
        
        // 检查是否达到最大客户端数
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (clients.size() >= MAX_CLIENTS) {
                std::string reject_msg = "服务器已达到最大客户端数限制 (" + 
                                        std::to_string(MAX_CLIENTS) + ")";
                send(new_socket, reject_msg.c_str(), reject_msg.length(), 0);
                close(new_socket);
                std::cout << "拒绝新连接：已达到最大客户端数限制" << std::endl;
                continue;
            }
        }
        
        // 获取客户端IP地址
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(address.sin_addr), client_ip, INET_ADDRSTRLEN);
        int client_port = ntohs(address.sin_port);
        
        // 创建客户端ID
        int client_id = ++client_counter;
        
        // 创建客户端信息
        auto client_info = std::make_shared<ClientInfo>(
            new_socket, client_id, std::string(client_ip) + ":" + std::to_string(client_port)
        );
        
        // 创建线程处理客户端
        client_info->thread = std::thread(
            handle_client, 
            new_socket, 
            client_id, 
            client_info->ip_address
        );
        client_info->thread.detach();  // 分离线程
        
        // 添加到客户端列表
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.push_back(client_info);
        }
        
        std::cout << "新客户端连接，ID:" << client_id 
                 << " [" << client_info->ip_address << "]" 
                 << " 当前客户端数: " << clients.size() << std::endl;
    }
    
    // 等待所有客户端线程结束
    std::cout << "等待所有客户端断开连接..." << std::endl;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (auto& client : clients) {
            shutdown(client->socket, SHUT_RDWR);
        }
    }
    
    // 等待一段时间让客户端断开
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // 清理资源
    close(server_fd);
    
    std::cout << "服务器已安全关闭" << std::endl;
    return 0;
}