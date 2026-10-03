#include <iostream>
#include <string>
#include <memory>
#include <optional>
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
    case proto::CommandTag::CreateIndex: return "CREATE_INDEX";
    case proto::CommandTag::DropIndex: return "DROP_INDEX";
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
    case db::ErrCode::IndexExists:    return proto::WireErrCode::IndexExists;
    case db::ErrCode::IndexNotFound:  return proto::WireErrCode::IndexNotFound;
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
    case db::ErrCode::InvalidVarValue: return proto::WireErrCode::InvalidVarValue;
    }
    return proto::WireErrCode::Internal;  // 不可达: 上方穷尽
}

// 发错误帧: 错误码经映射入帧, 文案原样
void send_error(int sock, db::ErrCode code, const std::string& message)
{
    proto::send_frame(sock, proto::MsgType::Error, proto::encode_error(to_wire(code), message));
}

// 发消息帧: 消息级别不低于会话变量 client_msg_level 才发, 会话诊断统一 debug 级;
void send_notice(int sock, LogLevel session_level, LogLevel msg_level, const std::string& message)
{
    if (msg_level < session_level) {
        return;
    }
    proto::send_frame(sock, proto::MsgType::Notice,
                      proto::encode_notice(static_cast<uint8_t>(msg_level), message));
}

// 结果集流式发送: 头帧 + 逐行攒批帧 + 结束帧(总行数); 发送失败返回 false(对端已断)
bool send_result_stream(int sock, exec::ExecResult& result, int client_id, LogLevel session_level)
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
    // 执行结果消息先于结束帧发出, 归入本条语句的响应流
    std::string exec_log = "ID:" + std::to_string(client_id) + " SQL执行结果: 返回 "
                         + std::to_string(total) + " 行";
    LOG(DEBUG, EXECUTOR, "%s", exec_log.c_str());
    send_notice(sock, session_level, DEBUG, "执行结果: 返回 " + std::to_string(total) + " 行");
    if (!proto::send_frame(sock, proto::MsgType::ResultSetEnd, proto::encode_rs_end(total))) {
        return false;
    }
    return true;
}

// 会话上下文: 主循环与各拆分函数共用的会话状态, 成员直接读写
struct SessionCtx {
    int sock;
    int client_id;
    ct::Catalog* db;
    // 事务状态: in_txn 为客户端已 BEGIN, txn_started 为引擎事务上下文已建立
    // (BEGIN 不取锁, 锁在事务首条语句执行时获取)
    bool in_txn = false;
    bool txn_started = false;
    // 会话变量: 本连接消息级别, 缺省 info
    LogLevel client_msg_level = LogLevel::INFO;
};

// 报错路径的整事务回滚: 引擎事务上下文已建立才调引擎, 会话状态无条件复位
void rollback_session_txn(SessionCtx& s)
{
    if (s.txn_started) {
        s.db->rollback_txn();
        s.txn_started = false;
    }
    s.in_txn = false;
}

// 收整条请求帧并做会话级预检; false = 结束会话(断开/非法类型/主动退出)
bool recv_query(const SessionCtx& s, std::string& msg)
{
    proto::MsgType msg_type;
    if (!proto::recv_frame(s.sock, proto::MAX_REQUEST_PAYLOAD, msg_type, msg)) {
        LOG(INFO, NETWORK, "客户端 ID:%d 断开连接", s.client_id);
        return false;
    }
    if (msg_type != proto::MsgType::Query) {
        LOG(WARNING, NETWORK, "客户端 ID:%d 消息类型非法: %u, 断开连接", s.client_id,
            static_cast<unsigned int>(msg_type));
        return false;
    }
    LOG(DEBUG, NETWORK, "来自 ID:%d 的SQL: %s", s.client_id, msg.c_str());
    send_notice(s.sock, s.client_msg_level, DEBUG, "收到 SQL: " + msg);
    if (msg == "quit" || msg == "exit") {
        LOG(INFO, NETWORK, "客户端 ID:%d 主动退出", s.client_id);
        return false;
    }
    return true;
}

// 事务控制语句: 会话层短路处理, 自行回帧
void handle_txn_control(SessionCtx& s, StmtKind kind)
{
    if (kind == StmtKind::Begin) {
        if (s.db->bootstrap_mode()) {
            LOG(WARNING, EXECUTOR, "ID:%d bootstrap 模式拒绝 BEGIN", s.client_id);
            send_error(s.sock, db::ErrCode::BootstrapMode, "bootstrap 模式不允许 BEGIN");
            return;
        }
        if (s.in_txn) {
            // 嵌套 BEGIN: 报错并回滚外层事务
            LOG(WARNING, EXECUTOR, "ID:%d 嵌套 BEGIN, 外层事务已回滚", s.client_id);
            rollback_session_txn(s);
            send_error(s.sock, db::ErrCode::TxnActive,
                       "事务已在进行中, 嵌套 BEGIN 已拒绝, 外层事务已回滚");
            return;
        }
        s.in_txn = true;
        proto::send_frame(s.sock, proto::MsgType::Ok,
                          proto::encode_command(proto::CommandTag::Begin, 0));
        return;
    }
    if (!s.in_txn) {
        const char* name = kind == StmtKind::Commit ? "COMMIT" : "ROLLBACK";
        LOG(WARNING, EXECUTOR, "ID:%d 事务外 %s", s.client_id, name);
        send_error(s.sock, db::ErrCode::NoActiveTxn, std::string(name) + ": 无活动事务");
        return;
    }
    // 结束事务: 已建立引擎事务上下文才调引擎(空事务无操作), 会话状态先复位,
    // 提交/回滚失败经外层 catch 回错误帧
    s.in_txn = false;
    if (s.txn_started) {
        s.txn_started = false;
        if (kind == StmtKind::Commit) {
            s.db->commit_txn();
        } else {
            s.db->rollback_txn();
        }
    }
    const proto::CommandTag tag = kind == StmtKind::Commit ? proto::CommandTag::Commit
                                                            : proto::CommandTag::Rollback;
    proto::send_frame(s.sock, proto::MsgType::Ok, proto::encode_command(tag, 0));
}

// SET 会话/全局变量: 会话层短路处理; 返回是否已处理, 未识别变量落回执行路径
bool handle_session_set(SessionCtx& s, const SetStmt& ss)
{
    if (ss.var_name() == "client_msg_level") {
        const std::optional<LogLevel> lvl = levelFromString(ss.value());
        if (!lvl.has_value()) {
            std::string err_log = "ID:" + std::to_string(s.client_id)
                                 + " client_msg_level 值非法: " + ss.value();
            LOG(WARNING, NETWORK, "%s", err_log.c_str());
            send_error(s.sock, db::ErrCode::InvalidVarValue,
                       "client_msg_level 值非法: " + ss.value());
            return true;
        }
        s.client_msg_level = *lvl;
        std::string set_log = "ID:" + std::to_string(s.client_id)
                            + " 会话变量 client_msg_level = "
                            + std::string(levelToString(s.client_msg_level));
        LOG(INFO, NETWORK, "%s", set_log.c_str());
        send_notice(s.sock, s.client_msg_level, DEBUG,
                    "client_msg_level = " + std::string(levelToString(s.client_msg_level)));
        proto::send_frame(s.sock, proto::MsgType::Ok,
                          proto::encode_command(proto::CommandTag::Set, 0));
        return true;
    }
    if (ss.var_name() == "server_log_level") {
        // 全局变量: 调整服务端日志级别阈值, 对所有会话生效
        const std::optional<LogLevel> lvl = levelFromString(ss.value());
        if (!lvl.has_value()) {
            std::string err_log = "ID:" + std::to_string(s.client_id)
                                 + " server_log_level 值非法: " + ss.value();
            LOG(WARNING, NETWORK, "%s", err_log.c_str());
            send_error(s.sock, db::ErrCode::InvalidVarValue,
                       "server_log_level 值非法: " + ss.value());
            return true;
        }
        Logger::getInstance().setLevelThreshold(*lvl);
        std::string set_log = "ID:" + std::to_string(s.client_id)
                            + " 全局变量 server_log_level = "
                            + std::string(levelToString(*lvl));
        LOG(INFO, NETWORK, "%s", set_log.c_str());
        send_notice(s.sock, s.client_msg_level, DEBUG,
                    "server_log_level = " + std::string(levelToString(*lvl)));
        proto::send_frame(s.sock, proto::MsgType::Ok,
                          proto::encode_command(proto::CommandTag::Set, 0));
        return true;
    }
    return false;
}

// 执行一条语句: 开事务/执行/自动提交/结果分流; false = 结果集发送失败(对端已断)
bool exec_statement(SessionCtx& s, const SQLStatement& stmt)
{
    // 事务开启: 自动提交语句为单语句事务, 显式事务首条语句取锁长持至 COMMIT/ROLLBACK
    if (s.in_txn) {
        if (!s.txn_started) {
            s.db->begin_txn();
            s.txn_started = true;
        }
    } else {
        s.db->begin_txn();
    }

    exec::ExecResult result;
    try {
        result = exec::execute(*s.db, stmt);
    } catch (const db::DbError& e) {
        // 结构化错误: 源头已记 ERROR, 当前事务回滚, 显式事务一并结束
        s.db->rollback_txn();
        std::string err = e.what();
        if (s.in_txn) {
            s.in_txn = false;
            s.txn_started = false;
            err += ", 事务已回滚";
        }
        send_error(s.sock, e.code(), err);
        return true;
    } catch (const std::exception& e) {
        // 非 DbError 的底层异常: 升格为结构化错误, 由外层 catch 路由回客户端
        s.db->rollback_txn();
        std::string err = e.what();
        if (s.in_txn) {
            s.in_txn = false;
            s.txn_started = false;
            err += ", 事务已回滚";
        }
        DB_RAISE(db::ErrCode::Internal, EXECUTOR, "ID:{} SQL执行异常: {}", s.client_id, err);
    }
    if (!s.in_txn) {
        // 自动提交: 成功即提交(持久化边界); SELECT 在算子 open 后提交释放锁,
        // 结果集锁外流式拉取——显式事务内的 SELECT 在锁内流式发送(见下)
        s.db->commit_txn();
    }
    if (result.is_result_set) {
        // 发送失败即对端已断, 结束会话(算子经析构释放页 pin)
        return send_result_stream(s.sock, result, s.client_id, s.client_msg_level);
    }
    std::string exec_log = "ID:" + std::to_string(s.client_id)
                         + " SQL执行结果: " + tag_log_name(result.tag);
    std::string exec_msg = std::string("执行结果: ") + tag_log_name(result.tag);
    if (result.count > 0) {
        exec_log += " " + std::to_string(result.count);
        exec_msg += " " + std::to_string(result.count);
    }
    LOG(DEBUG, EXECUTOR, "%s", exec_log.c_str());
    send_notice(s.sock, s.client_msg_level, DEBUG, exec_msg);
    proto::send_frame(s.sock, proto::MsgType::Ok, proto::encode_command(result.tag, result.count));
    return true;
}

// 会话收尾: 摘表/报在线数/关连接; notify 后不得再触碰全局生命周期对象
void remove_client(int client_socket)
{
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

    // 通知收尾等待放行
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        clients_cv.notify_all();
    }
}

}  // namespace

// 处理单个客户端的函数
void handle_client(int client_socket, int client_id, const std::string& client_ip,
                   ct::Catalog* db)
{
    std::string connect_msg = "客户端 ID:" + std::to_string(client_id) + " 已连接 (" + client_ip + ")";
    LOG(INFO, NETWORK, "%s", connect_msg.c_str());

    SessionCtx ctx{client_socket, client_id, db};

    // 处理客户端消息循环
    while (server_running) {
        try {
        // 按帧收整条请求: 断开/读取失败/长度超限/非法类型/主动退出即结束会话
        std::string msg_str;
        if (!recv_query(ctx, msg_str)) {
            break;
        }

        // SQL 解析: 合法语句按事务状态路由, 空语句原样回显
        std::string parse_error;
        std::unique_ptr<SQLStatement> stmt;
        if (!sql::parse(msg_str, parse_error, stmt)) {
            LOG(WARNING, PARSER, "SQL解析失败 ID:%d: %s", client_id, parse_error.c_str());
            // 显式事务内解析失败: 整事务立即回滚并结束
            if (ctx.in_txn) {
                rollback_session_txn(ctx);
                parse_error += ", 事务已回滚";
            }
            send_error(client_socket, db::ErrCode::SyntaxError, parse_error);
            continue;
        }
        LOG(DEBUG2, PARSER, "SQL解析成功 ID:%d: %s", client_id, msg_str.c_str());
        send_notice(client_socket, ctx.client_msg_level, DEBUG, "解析成功: " + msg_str);
        if (!stmt) {
            // 空输入或仅 ";", 无实际语句
            proto::send_frame(client_socket, proto::MsgType::Ok,
                              proto::encode_command(proto::CommandTag::Empty, 0));
            continue;
        }

        const StmtKind kind = stmt->kind();
        // 事务控制语句: 会话层短路处理, 不进 analyzer/planner/executor
        if (kind == StmtKind::Begin || kind == StmtKind::Commit || kind == StmtKind::Rollback) {
            handle_txn_control(ctx, kind);
            continue;
        }

        if (ctx.in_txn && (kind == StmtKind::CreateTable || kind == StmtKind::DropTable
                           || kind == StmtKind::CreateSchema || kind == StmtKind::DropSchema
                           || kind == StmtKind::CreateIndex || kind == StmtKind::DropIndex
                           || kind == StmtKind::Set)) {
            // 显式事务内拒绝 DDL 与 SET: 报错即整事务回滚
            LOG(WARNING, EXECUTOR, "ID:%d 事务内 DDL/SET 被拒, 事务已回滚", client_id);
            rollback_session_txn(ctx);
            send_error(client_socket, db::ErrCode::DdlInTxn,
                       "事务内不允许 DDL 或 SET 语句, 事务已回滚");
            continue;
        }

        // SET 会话/全局变量: 会话层短路处理, 不进 analyzer/executor
        if (kind == StmtKind::Set
            && handle_session_set(ctx, static_cast<const SetStmt&>(*stmt))) {
            continue;
        }

        if (!exec_statement(ctx, *stmt)) {
            break;
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
    if (ctx.txn_started) {
        try {
            db->rollback_txn();
        } catch (const db::DbError&) {
            // 结构化错误源头已记 ERROR, 此处不重记
        } catch (const std::exception& e) {
            LOG(WARNING, EXECUTOR, "ID:%d 断连回滚事务失败: %s", client_id, e.what());
        }
    }

    remove_client(client_socket);
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
