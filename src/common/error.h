// error.h: 跨层结构化错误(db::) — 在报错源头记 ERROR 日志(带堆栈)并抛出 DbError,
// 由 server 统一路由给客户端; 日志与客户端出口共用同一消息来源, 各自取用不同字段。
// 使用: DB_RAISE(ErrCode, LogModule, "fmt{}", args...) 取代"LOG_ERROR + 裸 throw"的耦合写法
#ifndef DB_COMMON_ERROR_H
#define DB_COMMON_ERROR_H

#include <exception>
#include <format>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "log/log.h"

namespace db {

// 客户端/服务端共用的错误分类码; 后续 wire 协议定型时可直接入帧
enum class ErrCode {
    IoError,        // 文件/IO 类失败
    CorruptCatalog, // 目录文件损坏
    CorruptData,    // 数据页/记录损坏
    InvalidType,    // 列类型非法
    TableNotFound,  // 表不存在
    TableExists,    // 表已存在
    InvalidDdl,     // 建表参数非法(空表名/空列集/重复列名)
    ValueMismatch,  // 插入值与列类型不匹配
    RecordTooLong,  // 记录超长
    ArithError,     // 算术溢出/除零(表达式求值)
    UnknownStmt,    // 未知语句种类(执行层不变量违反)
    Internal,       // 内部不变量违反(缓冲池记账等)
};

// 错误码转字符串, 用于日志书写
constexpr std::string_view errCodeName(ErrCode code) noexcept
{
    switch (code) {
        case ErrCode::IoError:        return "IO_ERROR";
        case ErrCode::CorruptCatalog: return "CORRUPT_CATALOG";
        case ErrCode::CorruptData:    return "CORRUPT_DATA";
        case ErrCode::InvalidType:    return "INVALID_TYPE";
        case ErrCode::TableNotFound:  return "TABLE_NOT_FOUND";
        case ErrCode::TableExists:    return "TABLE_EXISTS";
        case ErrCode::InvalidDdl:     return "INVALID_DDL";
        case ErrCode::ValueMismatch:  return "VALUE_MISMATCH";
        case ErrCode::RecordTooLong:  return "RECORD_TOO_LONG";
        case ErrCode::ArithError:     return "ARITH_ERROR";
        case ErrCode::UnknownStmt:    return "UNKNOWN_STMT";
        case ErrCode::Internal:       return "INTERNAL";
        default:                      return "UNKNOWN";
    }
}

// 跨层异常: what() 是给客户端看的可读文案; code/location 供日志与后续 wire 协议使用
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

// DB_RAISE 的实现: 格式化消息 -> 记 ERROR 日志(带错误码与堆栈) -> 抛 DbError
template<typename... Args>
[[noreturn]] void raise_error(ErrCode code, LogModule module,
                              const std::source_location& location,
                              std::string_view fmt, Args&&... args)
{
    std::string message;
    try {
        message = std::vformat(fmt, std::make_format_args(args...));
    } catch (const std::format_error& e) {
        message = std::string("[format error] ") + e.what();
    }
    const std::string logtext = std::format("[{}] {}", errCodeName(code), message);
    Logger::getInstance().log(LogLevel::ERROR, module, "%s", logtext.c_str());
    throw DbError(code, std::move(message), location);
}

}  // namespace detail

}  // namespace db

// 报错宏: 与 LOG 同形(码、模块、fmt 及其参数), 源头记 ERROR 日志并抛出 db::DbError
#define DB_RAISE(code, module, fmt, ...) \
    ::db::detail::raise_error(code, module, std::source_location::current(), \
                              fmt, ##__VA_ARGS__)

#endif // DB_COMMON_ERROR_H