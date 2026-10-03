// err.h: 跨层结构化错误(db::) — DB_RAISE 在报错源头记 ERROR 日志(带堆栈)并抛出 DbError,
// 由 server 统一路由给客户端(what()/code() 入错误帧); 日志与客户端出口共用同一消息来源, 各自取用不同字段。
// DB_CRITICAL 记 CRITICAL 日志(带堆栈)后全量排空并退出进程, 用于不可恢复的致命错误
// DB_CRASH 记 CRITICAL 日志(带堆栈)后立即 abort(不走静态析构), 用于退出流程会挂死或扩大损坏的场景
// 使用: DB_RAISE(ErrCode, LogModule, "fmt{}", args...) / DB_CRITICAL(LogModule, "fmt{}", args...)
//       / DB_CRASH(LogModule, "fmt{}", args...)
#ifndef DB_COMMON_ERR_H
#define DB_COMMON_ERR_H

#include <exception>
#include <format>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "log/log.h"

namespace db {

// 错误分类码; 经 server 穷尽 switch 映射为 proto wire 码入错误帧
enum class ErrCode {
    // 以下注释禁止添加括号与括号内的额外说明
    IoError,        // 文件/IO 类失败
    CatalogMissing, // 元数据表缺失
    CatalogExists,  // 元数据表已存在
    InvalidType,    // 列类型非法
    TableNotFound,  // 表不存在
    TableExists,    // 表已存在
    SchemaNotFound, // schema 不存在
    SchemaExists,   // schema 已存在
    SchemaNotEmpty, // 非空 schema 禁止删除
    IndexExists,    // 索引已存在
    IndexNotFound,  // 索引不存在
    ProtectedTable, // 保留段表禁止删除
    InvalidDdl,     // 建表参数非法
    ValueMismatch,  // 插入值与列类型不匹配
    RecordTooLong,  // 记录超长
    ArithError,     // 算术溢出/除零
    UnknownColumn,  // 引用了不存在的列
    UnknownVar,     // SET 引用了不存在的变量
    InvalidVarValue,// SET 变量值非法
    UnknownStmt,    // 未知语句种类
    NotImplemented, // 功能未实现
    Internal,       // 内部不变量违反
    SyntaxError,    // SQL 解析失败
    TxnActive,      // 事务已在进行中, 嵌套 BEGIN 被拒
    NoActiveTxn,    // 无活动事务的 COMMIT/ROLLBACK
    DdlInTxn,       // 事务内 DDL/SET 被拒
    BootstrapMode,  // bootstrap 模式限制
    TooManyClients, // 连接数超限
};

// 错误码转字符串, 用于日志书写
constexpr std::string_view errCodeName(ErrCode code) noexcept
{
    switch (code) {
        case ErrCode::IoError:        return "IO_ERROR";
        case ErrCode::CatalogMissing: return "CATALOG_MISSING";
        case ErrCode::CatalogExists:  return "CATALOG_EXISTS";
        case ErrCode::InvalidType:    return "INVALID_TYPE";
        case ErrCode::TableNotFound:  return "TABLE_NOT_FOUND";
        case ErrCode::TableExists:    return "TABLE_EXISTS";
        case ErrCode::SchemaNotFound: return "SCHEMA_NOT_FOUND";
        case ErrCode::SchemaExists:   return "SCHEMA_EXISTS";
        case ErrCode::SchemaNotEmpty: return "SCHEMA_NOT_EMPTY";
        case ErrCode::IndexExists:    return "INDEX_EXISTS";
        case ErrCode::IndexNotFound:  return "INDEX_NOT_FOUND";
        case ErrCode::ProtectedTable: return "PROTECTED_TABLE";
        case ErrCode::InvalidDdl:     return "INVALID_DDL";
        case ErrCode::ValueMismatch:  return "VALUE_MISMATCH";
        case ErrCode::RecordTooLong:  return "RECORD_TOO_LONG";
        case ErrCode::ArithError:     return "ARITH_ERROR";
        case ErrCode::UnknownColumn:  return "UNKNOWN_COLUMN";
        case ErrCode::UnknownVar:     return "UNKNOWN_VAR";
        case ErrCode::InvalidVarValue:return "INVALID_VAR_VALUE";
        case ErrCode::UnknownStmt:    return "UNKNOWN_STMT";
        case ErrCode::NotImplemented: return "NOT_IMPLEMENTED";
        case ErrCode::Internal:       return "INTERNAL";
        case ErrCode::SyntaxError:    return "SYNTAX_ERROR";
        case ErrCode::TxnActive:      return "TXN_ACTIVE";
        case ErrCode::NoActiveTxn:    return "NO_ACTIVE_TXN";
        case ErrCode::DdlInTxn:       return "DDL_IN_TXN";
        case ErrCode::BootstrapMode:  return "BOOTSTRAP_MODE";
        case ErrCode::TooManyClients: return "TOO_MANY_CLIENTS";
        default:                      return "UNKNOWN";
    }
}

// 跨层异常: what() 是给客户端看的可读文案; code 供错误帧与日志, location 供日志
// 沿用 std::runtime_error 基类, 兼容既有 catch(std::runtime_error) 调用方
class DbError : public std::runtime_error {
private:
    ErrCode code_;
    std::source_location location_;

public:
    DbError(ErrCode code, std::string message, const std::source_location& location)
        : std::runtime_error(std::move(message)), code_(code), location_(location) {}

    ErrCode code() const noexcept { return code_; }
    const std::source_location& location() const noexcept { return location_; }
};

namespace detail {

// 非模板实现, 格式化收敛在库内完成(err.cpp)
[[noreturn]] void raise_error_impl(ErrCode code, LogModule module,
                                   const std::source_location& location,
                                   std::string_view fmt, std::format_args args);

// CRITICAL 的非模板实现: 记 CRITICAL 日志后全量排空并退出进程(err.cpp)
[[noreturn]] void raise_critical_impl(LogModule module,
                                      std::string_view fmt, std::format_args args);

// CRASH 的非模板实现: 记 CRITICAL 日志后立即 abort(err.cpp)
[[noreturn]] void raise_crash_impl(LogModule module,
                                   std::string_view fmt, std::format_args args);

// DB_RAISE 的实现: 格式化消息 -> 记 ERROR 日志(带错误码与堆栈) -> 抛 DbError
template<typename... Args>
[[noreturn]] void raise_error(ErrCode code, LogModule module,
                              const std::source_location& location,
                              std::string_view fmt, Args&&... args)
{
    raise_error_impl(code, module, location, fmt, std::make_format_args(args...));
}

// DB_CRITICAL 的实现: 格式化消息 -> 记 CRITICAL 日志(带堆栈) -> 退出进程
template<typename... Args>
[[noreturn]] void raise_critical(LogModule module, std::string_view fmt, Args&&... args)
{
    raise_critical_impl(module, fmt, std::make_format_args(args...));
}

// DB_CRASH 的实现: 格式化消息 -> 记 CRITICAL 日志(带堆栈) -> abort
template<typename... Args>
[[noreturn]] void raise_crash(LogModule module, std::string_view fmt, Args&&... args)
{
    raise_crash_impl(module, fmt, std::make_format_args(args...));
}

}  // namespace detail

}  // namespace db

// 报错宏: 与 LOG 同形(码、模块、fmt 及其参数), 源头记 ERROR 日志并抛出 db::DbError
#define DB_RAISE(code, module, fmt, ...) \
    ::db::detail::raise_error(code, module, std::source_location::current(), \
                              fmt, ##__VA_ARGS__)

// 致命错误宏: 记 CRITICAL 日志(带堆栈)后全量排空并退出进程, 不返回调用方
#define DB_CRITICAL(module, fmt, ...) \
    ::db::detail::raise_critical(module, fmt, ##__VA_ARGS__)

// 崩溃式致命错误宏: 记 CRITICAL 日志(带堆栈)后立即 abort, 不返回调用方
#define DB_CRASH(module, fmt, ...) \
    ::db::detail::raise_crash(module, fmt, ##__VA_ARGS__)

#endif // DB_COMMON_ERR_H