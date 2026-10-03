// err.cpp: DB_RAISE/DB_CRITICAL/DB_CRASH 的非模板实现, 格式化与堆栈捕获收敛在库内完成
#include "common/err.h"

#include <boost/stacktrace.hpp>
#include <cstdlib>

namespace db {

namespace detail {

// 消息后附当前调用栈文本, ERROR/CRITICAL 日志正文统一在此组装
static std::string withStacktrace(std::string_view message)
{
    return std::format("{}\nStack trace:\n{}", message,
                       boost::stacktrace::to_string(boost::stacktrace::stacktrace()));
}

[[noreturn]] void raise_error_impl(ErrCode code, LogModule module,
                                   const std::source_location& location,
                                   std::string_view fmt, std::format_args args)
{
    std::string message;
    try {
        message = std::vformat(fmt, args);
    } catch (const std::format_error& e) {
        message = std::string("[format error] ") + e.what();
    }
    const std::string logtext = withStacktrace(
        std::format("[{}] {}", errCodeName(code), message));
    Logger::getInstance().log(LogLevel::ERROR, module, "%s", logtext.c_str());
    throw DbError(code, std::move(message), location);
}

// 记 CRITICAL 日志(带堆栈), exit/abort 两种致命出口共用
static void log_critical(LogModule module, std::string_view fmt, std::format_args args)
{
    std::string message;
    try {
        message = std::vformat(fmt, args);
    } catch (const std::format_error& e) {
        message = std::string("[format error] ") + e.what();
    }
    Logger::getInstance().log(LogLevel::CRITICAL, module, "%s",
                              withStacktrace(message).c_str());
}

// std::exit 触发 Logger 静态析构, 以写线程 join 语义全量排空后退出
[[noreturn]] void raise_critical_impl(LogModule module,
                                      std::string_view fmt, std::format_args args)
{
    log_critical(module, fmt, args);
    std::exit(EXIT_FAILURE);
}

// 立即 abort: 不走静态析构与全量排空, 取证依赖 stderr 同步回显与 core dump
[[noreturn]] void raise_crash_impl(LogModule module,
                                   std::string_view fmt, std::format_args args)
{
    log_critical(module, fmt, args);
    std::abort();
}

}  // namespace detail

}  // namespace db
