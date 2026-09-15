// log.cpp: Logger 实现(构造/写线程/格式化/日志入口), 头文件仅保留声明与宏
#include "log/log.h"

#include <boost/stacktrace.hpp>
#include <cstdio>
#include <iostream>

// 初始化静态成员
Logger* Logger::instance_ = nullptr;
std::string Logger::log_path_;

// 私有构造函数
Logger::Logger()
{
    // 默认所有模块都启用
    for (size_t i = 0; i <= static_cast<size_t>(LogModule::GENERAL); ++i) {
        modules_enabled_[i] = true;
    }

    // 打开日志文件
    if (log_path_.empty()) {
        throw std::runtime_error("日志路径未初始化");
    }
    log_file_.open(log_path_, std::ios::out | std::ios::app);
    if (!log_file_.is_open()) {
        throw std::runtime_error("无法打开日志文件: " + log_path_);
    }

    // 启动写入线程
    startWriterThread();
}

Logger::~Logger()
{
    stopWriterThread();
}

// 格式化时间戳
std::string Logger::formatTimestamp(const std::chrono::system_clock::time_point& tp)
{
    auto time = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()
    ) % 1000;

    std::tm tm_info;
    localtime_r(&time, &tm_info);

    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_info);

    return std::format("{}.{:03d}", buffer, static_cast<int>(ms.count()));
}

// 写入线程函数
void Logger::writerThreadFunc()
{
    while (!writer_stop_ || !queue_.empty()) {
        std::shared_ptr<LogMessage> msg;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return !queue_.empty() || writer_stop_; });

            if (queue_.empty() && writer_stop_) {
                break;
            }

            if (!queue_.empty()) {
                msg = queue_.front();
                queue_.pop();
            }
        }

        if (msg) {
            // 格式化日志行
            std::string timestamp = formatTimestamp(msg->timestamp);
            std::string level_str(levelToString(msg->level));
            // std::string module_str(moduleToString(msg->module));

            std::string log_line = std::format("[{}] [{}] {}\n",
                timestamp, level_str, msg->content);

            // 写入文件
            if (log_file_.is_open()) {
                log_file_ << log_line;
                log_file_.flush();  // 立即刷新，确保日志及时写入
            }

            // 同时输出到控制台（可选）
            // std::cout << log_line;
        }
    }
}

// 启动写入线程
void Logger::startWriterThread()
{
    writer_running_ = true;
    writer_thread_ = std::thread(&Logger::writerThreadFunc, this);
}

// 停止写入线程
void Logger::stopWriterThread()
{
    writer_stop_ = true;
    queue_cv_.notify_all();

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    writer_running_ = false;

    if (log_file_.is_open()) {
        log_file_.close();
    }
}

// 格式化可变参数
std::string Logger::formatMessage(const char* format, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    // 获取需要的缓冲区大小
    int size = vsnprintf(nullptr, 0, format, args);
    if (size < 0) {
        va_end(args_copy);
        return "";
    }

    // 分配缓冲区并格式化
    std::size_t u_size = static_cast<std::size_t>(size);
    std::string result(u_size, '\0');
    vsnprintf(result.data(), u_size + 1, format, args_copy);
    va_end(args_copy);

    return result;
}

// CRITICAL 级别的同步 stderr 出口: 队列写入之外的即时可见性, 供运维第一时间捕获
// 进程是否退出由调用方决定(log() 只负责记录, 不承担控制流副作用)
void Logger::echoCritical(const std::string& message)
{
    std::cerr << "[CRITICAL] " << message << std::endl;
}

// 捕获当前调用栈文本, ERROR 及以上日志附在消息尾部
std::string Logger::stacktraceText()
{
    return boost::stacktrace::to_string(boost::stacktrace::stacktrace());
}

// 记录日志的主函数
void Logger::log(LogLevel level, LogModule module, const char* format, ...)
{
    if (level < LogLevel::ERROR &&
        (!enabled_ || !modules_enabled_[static_cast<size_t>(module)])) {
        return;
    }

    // 格式化消息
    va_list args;
    va_start(args, format);
    std::string message = formatMessage(format, args);
    va_end(args);

    std::string errmsg{};

    if (level >= LogLevel::ERROR) {
        errmsg = message;
        message += "\nStack trace:\n";
        message += stacktraceText();
    }

    // 创建日志消息并加入队列
    auto log_msg = std::make_shared<LogMessage>(level, module, std::move(message));

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(std::move(log_msg));
    }
    queue_cv_.notify_one();

    if (level == LogLevel::CRITICAL) {
        echoCritical(errmsg);
    }
}

// 记录日志，带源码位置（可选功能）
void Logger::logWithSource(LogLevel level, LogModule module,
                           const std::source_location& location, const char* format, ...)
{
    if (level < LogLevel::ERROR &&
        (!enabled_ || !modules_enabled_[static_cast<size_t>(module)])) {
        return;
    }

    // 格式化消息
    va_list args;
    va_start(args, format);
    std::string content = formatMessage(format, args);
    va_end(args);

    // 添加源码位置信息
    std::string message = std::format("{}:{}:{} {}",
        location.file_name(), location.line(), location.function_name(), content);

    std::string errmsg{};
    if (level >= LogLevel::ERROR) {
        errmsg = message;
        message += "\nStack trace:\n";
        message += stacktraceText();
    }

    // 创建日志消息并加入队列
    auto log_msg = std::make_shared<LogMessage>(level, module, std::move(message));

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(std::move(log_msg));
    }
    queue_cv_.notify_one();

    if (level == LogLevel::CRITICAL) {
        echoCritical(errmsg);
    }
}

// logCpp 的非模板实现, 格式化收敛在库内完成
void Logger::logCppImpl(LogLevel level, LogModule module, std::string_view fmt,
                        std::format_args args)
{
    if (level < LogLevel::ERROR &&
        (!enabled_ || !modules_enabled_[static_cast<size_t>(module)])) {
        return;
    }

    // 使用 std::vformat 格式化运行期格式串
    std::string message;
    try {
        message = std::vformat(fmt, args);
    } catch (const std::format_error& e) {
        message = std::string("[format error] ") + e.what();
    }

    std::string errmsg{};
    if (level >= LogLevel::ERROR) {
        errmsg = message;
        message += "\nStack trace:\n";
        message += stacktraceText();
    }

    // 创建日志消息并加入队列
    auto log_msg = std::make_shared<LogMessage>(level, module, std::move(message));

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(std::move(log_msg));
    }
    queue_cv_.notify_one();

    if (level == LogLevel::CRITICAL) {
        echoCritical(errmsg);
    }
}
