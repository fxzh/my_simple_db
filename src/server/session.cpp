#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "common/err.h"
#include "log/log.h"
#include "proto/proto.h"
#include "sql_parser.h"
#include "ast.hh"
#include "executor.h"
#include "catalog.h"
#include "session.h"

using enum LogModule;
using enum LogLevel;

// 全局变量: server_running 定义在 server.cpp
std::vector<std::shared_ptr<ClientInfo>> clients;
std::mutex clients_mutex;
std::condition_variable clients_cv;
std::atomic<int> client_counter{0};
std::mutex cout_mutex;  // 保护标准输出

// 线程安全的输出
void safe_cout(const std::string& message)
{
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << message << std::endl;
}

namespace {

// 命令标签日志名: server 日志文案, 与 client 展示格式互不相关
const char* tag_log_name(proto::CommandTag tag)
{
    switch (tag) {
    case proto::CommandTag::Empty: return "EMPTY";
    case proto::CommandTag::CreateTable: return "CREATE";
    case proto::CommandTag::DropTable: return "DROP";
    case proto::CommandTag::CreateSchema: return "CREATE_SCHEMA";
    case proto::CommandTag::DropSchema: return "DROP_SCHEMA";
    case proto::CommandTag::Insert: return "INSERT";
    case proto::CommandTag::Delete: return "DELETE";
    case proto::CommandTag::Set: return "SET";
    }
    return "";
}

// 结果集流式发送: 头帧 + 逐行攒批帧 + 结束帧(总行数); 发送失败返回 false(对端已断)
bool send_result_stream(int sock, exec::ExecResult& result, int client_id)
{
    if (!proto::send_frame(sock, proto::MsgType::ResultSetHead,
                           proto::encode_rs_head(result.col_names))) {
        return false;
    }
    // st::Value 与 CellVal 同构, 逐行拉取逐格搬运, 攒满一批发一帧
    uint64_t total = 0;
    std::vector<std::vector<proto::CellVal>> batch;
    st::Row row;
    while (result.stream->next(&row)) {
        std::vector<proto::CellVal> cells;
        cells.reserve(row.values.size());
        for (const st::Value& v : row.values) {
            cells.push_back(std::visit([](const auto& x) { return proto::CellVal{x}; }, v));
        }
        batch.push_back(std::move(cells));
        ++total;
        if (batch.size() >= proto::RS_BATCH_MAX_ROWS) {
            if (!proto::send_frame(sock, proto::MsgType::ResultSetBatch,
                                   proto::encode_rs_batch(batch))) {
                return false;
            }
            batch.clear();
        }
    }
    result.stream->close();
    if (!batch.empty() && !proto::send_frame(sock, proto::MsgType::ResultSetBatch,
                                             proto::encode_rs_batch(batch))) {
        return false;
    }
    if (!proto::send_frame(sock, proto::MsgType::ResultSetEnd, proto::encode_rs_end(total))) {
        return false;
    }
    std::string exec_log = "ID:" + std::to_string(client_id) + " SQL执行结果: 返回 "
                         + std::to_string(total) + " 行";
    LOG(INFO, EXECUTOR, "%s", exec_log.c_str());
    return true;
}

}  // namespace

// 处理单个客户端的函数
void handle_client(int client_socket, int client_id, const std::string& client_ip,
                   ct::Catalog* db)
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
            if (stmt) {
                // 语句级自动提交事务: 失败回滚半途修改后异常上抛, 成功提交即持久化
                // 边界(SELECT 只读无记录, 在算子 open 后提交释放锁, 结果集锁外流式拉取)
                db->begin_txn();
                exec::ExecResult result;
                try {
                    result = exec::execute(*db, *stmt);
                } catch (...) {
                    db->rollback_txn();
                    throw;
                }
                db->commit_txn();
                if (result.is_result_set) {
                    if (!send_result_stream(client_socket, result, client_id)) {
                        // 发送失败即对端已断, 结束会话(算子经析构释放页 pin)
                        break;
                    }
                } else {
                    std::string exec_log = "ID:" + std::to_string(client_id)
                                         + " SQL执行结果: " + tag_log_name(result.tag);
                    if (result.count > 0) {
                        exec_log += " " + std::to_string(result.count);
                    }
                    LOG(INFO, EXECUTOR, "%s", exec_log.c_str());
                    proto::send_frame(client_socket, proto::MsgType::Ok,
                                      proto::encode_command(result.tag, result.count));
                }
            } else {
                // 空输入或仅 ";", 无实际语句
                proto::send_frame(client_socket, proto::MsgType::Ok,
                                  proto::encode_command(proto::CommandTag::Empty, 0));
            }
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

    // 通知收尾等待放行, notify 后不得再触碰全局生命周期对象
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients_cv.notify_all();
    }
}

// 受理一个新连接: 拒超限/建线程/入表
void spawn_client(int new_socket, const struct sockaddr_in& address, ct::Catalog* db)
{
    // 关闭 Nagle, 回复帧即时发出
    int nodelay = 1;
    if (setsockopt(new_socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
        LOG(WARNING, NETWORK, "设置 TCP_NODELAY 失败, 断开该连接");
        close(new_socket);
        return;
    }

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
