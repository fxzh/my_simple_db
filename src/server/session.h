#ifndef SESSION_H
#define SESSION_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

namespace st {
class Database;  // 存储引擎门面, 完整定义见 storage.h
}

// 客户端连接信息
struct ClientInfo {
    int socket;
    int client_id;
    std::string ip_address;
    std::thread thread;

    ClientInfo(int sock, int id, const std::string& ip)
        : socket(sock), client_id(id), ip_address(ip) {}

    ~ClientInfo()
    {
        if (thread.joinable()) {
            thread.detach();
        }
    }
};

#define MAX_CLIENTS 100

// 全局变量声明: clients 族定义在 session.cpp, server_running 定义在 server.cpp
extern std::vector<std::shared_ptr<ClientInfo>> clients;
extern std::mutex clients_mutex;
extern std::atomic<int> client_counter;
extern std::atomic<bool> server_running;
extern std::mutex cout_mutex;  // 保护标准输出

// 线程安全的输出
void safe_cout(const std::string& message);

// 处理单个客户端的函数
void handle_client(int client_socket, int client_id, const std::string& client_ip,
                   st::Database* db);

// 受理一个新连接: 拒超限/建线程/入表
void spawn_client(int new_socket, const struct sockaddr_in& address, st::Database* db);

#endif
