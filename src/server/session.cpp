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
    case proto::CommandTag::Begin: return "BEGIN";
    case proto::CommandTag::Commit: return "COMMIT";
    case proto::CommandTag::Rollback: return "ROLLBACK";
    }
    return "";
}

// db::ErrCode -> proto wire 码的穷尽转换, 新增 ErrCode 未映射时 -Wswitch 报警
proto::WireErrCode to_wire(db::ErrCode code)
{
    switch (code) {
    case db::ErrCode::IoError:        return proto::WireErrCode::IoError;
    case db::ErrCode::CorruptCatalog: return proto::WireErrCode::CorruptCatalog;
    case db::ErrCode::CatalogMissing: return proto::WireErrCode::CatalogMissing;
    case db::ErrCode::CatalogExists:  return proto::WireErrCode::CatalogExists;
    case db::ErrCode::CorruptData:    return proto::WireErrCode::CorruptData;
    case db::ErrCode::InvalidType:    return proto::WireErrCode::InvalidType;
    case db::ErrCode::TableNotFound:  return proto::WireErrCode::TableNotFound;
    case db::ErrCode::TableExists:    return proto::WireErrCode::TableExists;
    case db::ErrCode::SchemaNotFound: return proto::WireErrCode::SchemaNotFound;
    case db::ErrCode::SchemaExists:   return proto::WireErrCode::SchemaExists;
    case db::ErrCode::SchemaNotEmpty: return proto::WireErrCode::SchemaNotEmpty;
    case db::ErrCode::ProtectedTable: return proto::WireErrCode::ProtectedTable;
    case db::ErrCode::InvalidDdl:     return proto::WireErrCode::InvalidDdl;
    case db::ErrCode::ValueMismatch:  return proto::WireErrCode::ValueMismatch;
    case db::ErrCode::RecordTooLong:  return proto::WireErrCode::RecordTooLong;
    case db::ErrCode::ArithError:     return proto::WireErrCode::ArithError;
    case db::ErrCode::UnknownColumn:  return proto::WireErrCode::UnknownColumn;
    case db::ErrCode::UnknownVar:     return proto::WireErrCode::UnknownVar;
    case db::ErrCode::UnknownStmt:    return proto::WireErrCode::UnknownStmt;
    case db::ErrCode::NotImplemented: return proto::WireErrCode::NotImplemented;
    case db::ErrCode::Internal:       return proto::WireErrCode::Internal;
    case db::ErrCode::SyntaxError:    return proto::WireErrCode::SyntaxError;
    case db::ErrCode::TxnActive:      return proto::WireErrCode::TxnActive;
    case db::ErrCode::NoActiveTxn:    return proto::WireErrCode::NoActiveTxn;
    case db::ErrCode::DdlInTxn:       return proto::WireErrCode::DdlInTxn;
    case db::ErrCode::BootstrapMode:  return proto::WireErrCode::BootstrapMode;
    case db::ErrCode::TooManyClients: return proto::WireErrCode::TooManyClients;
    }
    return proto::WireErrCode::Internal;  // 不可达: 上方穷尽
}

// 发错误帧: 错误码经映射入帧, 文案原样
void send_error(int sock, db::ErrCode code, const std::string& message)
{
    proto::send_frame(sock, proto::MsgType::Error, proto::encode_error(to_wire(code), message));
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

    // 会话事务状态: in_txn 为客户端已 BEGIN, txn_started 为引擎事务上下文已建立
    // (BEGIN 不取锁, 锁在事务首条语句执行时获取, 见事务设计文档 §2.3)
    bool in_txn = false;
    bool txn_started = false;

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

        // SQL 解析: 合法语句按事务状态路由, 空语句原样回显
        std::string parse_error;
        std::unique_ptr<SQLStatement> stmt;
        if (!sql::parse(msg_str, parse_error, stmt)) {
            std::string err_log = "SQL解析失败 ID:" + std::to_string(client_id) + ": " + parse_error;
            LOG(WARNING, PARSER, "%s", err_log.c_str());
            // 显式事务内解析失败: 整事务立即回滚并结束
            if (in_txn) {
                if (txn_started) {
                    db->rollback_txn();
                    txn_started = false;
                }
                in_txn = false;
                parse_error += ", 事务已回滚";
            }
            send_error(client_socket, db::ErrCode::SyntaxError, parse_error);
            continue;
        }
        std::string ok_log = "SQL解析成功 ID:" + std::to_string(client_id) + ": " + msg_str;
        LOG(INFO, PARSER, "%s", ok_log.c_str());
        if (!stmt) {
            // 空输入或仅 ";", 无实际语句
            proto::send_frame(client_socket, proto::MsgType::Ok,
                              proto::encode_command(proto::CommandTag::Empty, 0));
            continue;
        }

        const StmtKind kind = stmt->kind();
        if (kind == StmtKind::Begin || kind == StmtKind::Commit || kind == StmtKind::Rollback) {
            // 事务控制语句: 会话层短路处理, 不进 analyzer/planner/executor
            if (kind == StmtKind::Begin) {
                if (db->bootstrap_mode()) {
                    std::string err_log = "ID:" + std::to_string(client_id)
                                         + " bootstrap 模式拒绝 BEGIN";
                    LOG(WARNING, EXECUTOR, "%s", err_log.c_str());
                    send_error(client_socket, db::ErrCode::BootstrapMode,
                               "bootstrap 模式不允许 BEGIN");
                    continue;
                }
                if (in_txn) {
                    // 嵌套 BEGIN: 报错并回滚外层事务
                    std::string err_log = "ID:" + std::to_string(client_id)
                                         + " 嵌套 BEGIN, 外层事务已回滚";
                    LOG(WARNING, EXECUTOR, "%s", err_log.c_str());
                    if (txn_started) {
                        db->rollback_txn();
                        txn_started = false;
                    }
                    in_txn = false;
                    send_error(client_socket, db::ErrCode::TxnActive,
                               "事务已在进行中, 嵌套 BEGIN 已拒绝, 外层事务已回滚");
                    continue;
                }
                in_txn = true;
                proto::send_frame(client_socket, proto::MsgType::Ok,
                                  proto::encode_command(proto::CommandTag::Begin, 0));
                continue;
            }
            if (!in_txn) {
                const char* name = kind == StmtKind::Commit ? "COMMIT" : "ROLLBACK";
                std::string err_log = "ID:" + std::to_string(client_id) + " 事务外 ";
                err_log += name;
                LOG(WARNING, EXECUTOR, "%s", err_log.c_str());
                send_error(client_socket, db::ErrCode::NoActiveTxn,
                           std::string(name) + ": 无活动事务");
                continue;
            }
            // 结束事务: 已建立引擎事务上下文才调引擎(空事务无操作), 会话状态先复位,
            // 提交/回滚失败经外层 catch 回错误帧
            in_txn = false;
            if (txn_started) {
                txn_started = false;
                if (kind == StmtKind::Commit) {
                    db->commit_txn();
                } else {
                    db->rollback_txn();
                }
            }
            const proto::CommandTag tag = kind == StmtKind::Commit ? proto::CommandTag::Commit
                                                                   : proto::CommandTag::Rollback;
            proto::send_frame(client_socket, proto::MsgType::Ok, proto::encode_command(tag, 0));
            continue;
        }

        if (in_txn && (kind == StmtKind::CreateTable || kind == StmtKind::DropTable
                       || kind == StmtKind::CreateSchema || kind == StmtKind::DropSchema
                       || kind == StmtKind::Set)) {
            // 显式事务内拒绝 DDL 与 SET: 报错即整事务回滚
            std::string err_log = "ID:" + std::to_string(client_id)
                                 + " 事务内 DDL/SET 被拒, 事务已回滚";
            LOG(WARNING, EXECUTOR, "%s", err_log.c_str());
            if (txn_started) {
                db->rollback_txn();
                txn_started = false;
            }
            in_txn = false;
            send_error(client_socket, db::ErrCode::DdlInTxn,
                       "事务内不允许 DDL 或 SET 语句, 事务已回滚");
            continue;
        }

        // 事务开启: 自动提交语句为单语句事务, 显式事务首条语句取锁长持至 COMMIT/ROLLBACK
        if (in_txn) {
            if (!txn_started) {
                db->begin_txn();
                txn_started = true;
            }
        } else {
            db->begin_txn();
        }

        exec::ExecResult result;
        try {
            result = exec::execute(*db, *stmt);
        } catch (const db::DbError& e) {
            // 结构化错误: 源头已记 ERROR, 当前事务回滚, 显式事务一并结束
            db->rollback_txn();
            std::string err = e.what();
            if (in_txn) {
                in_txn = false;
                txn_started = false;
                err += ", 事务已回滚";
            }
            send_error(client_socket, e.code(), err);
            continue;
        } catch (const std::exception& e) {
            // 非 DbError 的底层异常: 升格为结构化错误, 由外层 catch 路由回客户端
            db->rollback_txn();
            std::string err = e.what();
            if (in_txn) {
                in_txn = false;
                txn_started = false;
                err += ", 事务已回滚";
            }
            DB_RAISE(db::ErrCode::Internal, EXECUTOR, "ID:{} SQL执行异常: {}", client_id, err);
        }
        if (!in_txn) {
            // 自动提交: 成功即提交(持久化边界); SELECT 在算子 open 后提交释放锁,
            // 结果集锁外流式拉取——显式事务内的 SELECT 在锁内流式发送(见下)
            db->commit_txn();
        }
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
        } catch (const db::DbError& e) {
            // 结构化错误: 源头已记 ERROR(带堆栈), 这里只路由给客户端, 不再重复记
            send_error(client_socket, e.code(), e.what());
        } catch (const std::exception& e) {
            // 非 DbError 的底层异常降级收录后回客户端
            LOG(WARNING, EXECUTOR, "ID:%d SQL执行异常: %s", client_id, e.what());
            send_error(client_socket, db::ErrCode::Internal, e.what());
        }
    }

    // 断连/quit/停服: 未结束的显式事务同 ROLLBACK 清理
    if (txn_started) {
        try {
            db->rollback_txn();
        } catch (const db::DbError&) {
            // 结构化错误源头已记 ERROR, 此处不重记
        } catch (const std::exception& e) {
            LOG(WARNING, EXECUTOR, "ID:%d 断连回滚事务失败: %s", client_id, e.what());
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
void spawn_client(int new_socket, const sockaddr_in& address, ct::Catalog* db)
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
            send_error(new_socket, db::ErrCode::TooManyClients, reject_msg);
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

    // 添加到客户端列表
    size_t online = 0;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.push_back(client_info);
        online = clients.size();
    }

    // 创建线程处理客户端
    client_info->thread = std::thread(
        handle_client,
        new_socket,
        client_id,
        client_info->ip_address,
        db
    );
    client_info->thread.detach();  // 分离线程

    std::cout << "新客户端连接，ID:" << client_id
             << " [" << client_info->ip_address << "]"
             << " 当前客户端数: " << online << std::endl;
}
