#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "common/error.h"
#include "log/log.h"
#include "proto/proto.h"
#include "sql_parser.h"
#include "ast.hh"
#include "executor.h"
#include "storage.h"
#include "session.h"

using enum LogModule;
using enum LogLevel;

// 全局变量: server_running 定义在 server.cpp
std::vector<std::shared_ptr<ClientInfo>> clients;
std::mutex clients_mutex;
std::atomic<int> client_counter{0};
std::mutex cout_mutex;  // 保护标准输出

// 线程安全的输出
void safe_cout(const std::string& message)
{
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << message << std::endl;
}

// 处理单个客户端的函数
void handle_client(int client_socket, int client_id, const std::string& client_ip,
                   st::Database* db)
{
    std::string connect_msg = "客户端 ID:" + std::to_string(client_id) + " 已连接 (" + client_ip + ")";
    LOG(INFO, NETWORK, "%s", connect_msg.c_str());

    // 处理客户端消息循环
    while (server_running) {
        try {
        // 按帧收整条请求: 断开/读取失败/长度超限即结束会话
        proto::MsgType msg_type;
        std::string msg_str;
        if (!proto::recv_frame(client_socket, proto::MAX_REQUEST_PAYLOAD, msg_type, msg_str)) {
            std::string disconnect_msg = "客户端 ID:" + std::to_string(client_id) + " 断开连接";
            LOG(INFO, NETWORK, "%s", disconnect_msg.c_str());
            break;
        }
        if (msg_type != proto::MsgType::Query) {
            LOG(WARNING, NETWORK, "客户端 ID:%d 消息类型非法: %u, 断开连接", client_id,
                static_cast<unsigned int>(msg_type));
            break;
        }
        std::string log_msg = "来自 ID:" + std::to_string(client_id) + " 的SQL: " + msg_str;
        LOG(INFO, NETWORK, "%s", log_msg.c_str());

        // 检查是否收到退出指令
        if (msg_str == "quit" || msg_str == "exit") {
            std::string goodbye_msg = "再见!";
            proto::send_frame(client_socket, proto::MsgType::Ok, goodbye_msg);

            std::string leave_msg = "客户端 ID:" + std::to_string(client_id) + " 主动退出";
            LOG(INFO, NETWORK, "%s", leave_msg.c_str());
            break;
        }

        // SQL 解析: 合法语句交给执行层执行, 空语句原样回显
        std::string parse_error;
        std::unique_ptr<SQLStatement> stmt;
        if (sql::parse(msg_str, parse_error, stmt)) {
            std::string ok_log = "SQL解析成功 ID:" + std::to_string(client_id) + ": " + msg_str;
            LOG(INFO, PARSER, "%s", ok_log.c_str());
            std::string reply;
            if (stmt) {
                reply = exec::execute(*db, *stmt);
                std::string exec_log = "ID:" + std::to_string(client_id) + " SQL执行结果: " + reply;
                LOG(INFO, EXECUTOR, "%s", exec_log.c_str());
            } else {
                // 空输入或仅 ";", 无实际语句
                reply = msg_str;
            }
            proto::send_frame(client_socket, proto::MsgType::Ok, reply);
        } else {
            std::string err_log = "SQL解析失败 ID:" + std::to_string(client_id) + ": " + parse_error;
            LOG(WARNING, PARSER, "%s", err_log.c_str());
            proto::send_frame(client_socket, proto::MsgType::Error, parse_error);
        }
        } catch (const db::DbError& e) {
            // 结构化错误: 源头已记 ERROR(带堆栈), 这里只路由给客户端, 不再重复记
            proto::send_frame(client_socket, proto::MsgType::Error, e.what());
        } catch (const std::exception& e) {
            // 非 DbError 的底层异常降级收录后回客户端
            LOG(WARNING, EXECUTOR, "ID:%d SQL执行异常: %s", client_id, e.what());
            proto::send_frame(client_socket, proto::MsgType::Error, e.what());
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

// 受理一个新连接: 拒超限/建线程/入表
void spawn_client(int new_socket, const struct sockaddr_in& address, st::Database* db)
{
    // 检查是否达到最大客户端数
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        if (clients.size() >= MAX_CLIENTS) {
            std::string reject_msg = "服务器已达到最大客户端数限制 (" + std::to_string(MAX_CLIENTS) + ")";
            proto::send_frame(new_socket, proto::MsgType::Error, reject_msg);
            close(new_socket);
            std::cout << "拒绝新连接：已达到最大客户端数限制" << std::endl;
            return;
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
        client_info->ip_address,
        db
    );
    client_info->thread.detach();  // 分离线程

    // 添加到客户端列表
    size_t online = 0;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.push_back(client_info);
        online = clients.size();
    }

    std::cout << "新客户端连接，ID:" << client_id
             << " [" << client_info->ip_address << "]"
             << " 当前客户端数: " << online << std::endl;
}
