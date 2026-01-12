#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <format>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include <cstdarg>
#include <source_location>
#include <cstdio>
#include <utility>
#include <boost/stacktrace.hpp>

// 日志级别枚举
enum class LogLevel {
    DEBUG5,
    DEBUG4,
    DEBUG3,
    DEBUG2,
    DEBUG,
    INFO,
    NOTICE,
    WARNING,
    ERROR,
    CRITICAL
};

// 日志级别转换为字符串
constexpr std::string_view levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG5:   return "DEBUG5";
        case LogLevel::DEBUG4:   return "DEBUG4";
        case LogLevel::DEBUG3:   return "DEBUG3";
        case LogLevel::DEBUG2:   return "DEBUG2";
        case LogLevel::DEBUG:    return "DEBUG";
        case LogLevel::INFO:     return "INFO";
        case LogLevel::NOTICE:   return "NOTICE";
        case LogLevel::WARNING:  return "WARNING";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
        default:                 return "UNKNOWN";
    }
}

// 日志模块枚举
enum class LogModule {
    SYNTAX,     // 语法模块
    PARSER,     // 解析模块
    PLANNER,    // 计划模块
    EXECUTOR,   // 执行模块
    NETWORK,    // 网络模块
    SYSTEM,     // 系统模块
    GENERAL     // 通用模块
};

// 日志消息结构
struct LogMessage {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    LogModule module;
    std::string content;
    
    LogMessage(LogLevel lvl, LogModule mod, std::string msg)
        : timestamp(std::chrono::system_clock::now())
        , level(lvl)
        , module(mod)
        , content(std::move(msg)) {}
};

// 日志记录器类
class Logger {
private:
    // 单例实例
    static Logger* instance_;
    
    // 线程安全的日志队列
    std::queue<std::shared_ptr<LogMessage>> queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    // 日志写入线程
    std::thread writer_thread_;
    std::atomic<bool> writer_running_{false};
    std::atomic<bool> writer_stop_{false};
    
    // 日志文件
    std::ofstream log_file_;
    
    // 日志控制
    std::atomic<bool> enabled_{true};
    std::atomic<bool> modules_enabled_[static_cast<size_t>(LogModule::GENERAL) + 1];
    
    // 私有构造函数
    Logger() {
        // 默认所有模块都启用
        for (size_t i = 0; i <= static_cast<size_t>(LogModule::GENERAL); ++i) {
            modules_enabled_[i] = true;
        }
        
        // 打开日志文件
        log_file_.open("simple.log", std::ios::out | std::ios::app);
        if (!log_file_.is_open()) {
            throw std::runtime_error("无法打开日志文件: simple.log");
        }
        
        // 启动写入线程
        startWriterThread();
    }
    
    // 格式化时间戳
    std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) {
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
    void writerThreadFunc() {
        while (!writer_stop_ || !queue_.empty()) {
            std::shared_ptr<LogMessage> msg;
            
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this]() {
                    return !queue_.empty() || writer_stop_;
                });
                
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
    void startWriterThread() {
        writer_running_ = true;
        writer_thread_ = std::thread(&Logger::writerThreadFunc, this);
    }
    
    // 停止写入线程
    void stopWriterThread() {
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
    std::string formatMessage(const char* format, va_list args) {
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
    
public:
    // 删除拷贝构造和赋值
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    ~Logger() {
        stopWriterThread();
    }
    
    // 获取单例实例
    static Logger& getInstance() {
        static Logger instance;
        instance_ = &instance;
        return instance;
    }
    
    // 记录日志的主函数
    void log(LogLevel level, LogModule module, const char* format, ...) {
        if (!enabled_ || !modules_enabled_[static_cast<size_t>(module)]) {
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
            message += boost::stacktrace::to_string(boost::stacktrace::stacktrace());
        }
        
        // 创建日志消息并加入队列
        auto log_msg = std::make_shared<LogMessage>(level, module, std::move(message));
        
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push(std::move(log_msg));
        }
        queue_cv_.notify_one();

        if (level == LogLevel::ERROR) {
            throw std::runtime_error(errmsg);
        }
    }
    
    // 记录日志，带源码位置（可选功能）
    void logWithSource(LogLevel level, LogModule module, 
                      const std::source_location& location,
                      const char* format, ...) {
        if (!enabled_ || !modules_enabled_[static_cast<size_t>(module)]) {
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
        
        // 创建日志消息并加入队列
        auto log_msg = std::make_shared<LogMessage>(level, module, std::move(message));
        
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push(std::move(log_msg));
        }
        queue_cv_.notify_one();
    }

    // 使用 std::format 的模板化记录方法，支持传入任意 C++ 类型参数
    template<typename... Args>
    void logCpp(LogLevel level, LogModule module, std::string_view fmt, Args&&... args) {
        if (!enabled_ || !modules_enabled_[static_cast<size_t>(module)]) {
            return;
        }

        // 使用 std::format 格式化消息
        std::string message;
        try {
            message = std::format(fmt, std::forward<Args>(args)...);
        } catch (const std::format_error& e) {
            message = std::string("[format error] ") + e.what();
        }

        std::string errmsg{};
        if (level >= LogLevel::ERROR) {
            errmsg = message;
            message += "\nStack trace:\n";
            message += boost::stacktrace::to_string(boost::stacktrace::stacktrace());
        }

        auto log_msg = std::make_shared<LogMessage>(level, module, std::move(message));

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push(std::move(log_msg));
        }
        queue_cv_.notify_one();

        if (level == LogLevel::ERROR) {
            throw std::runtime_error(errmsg);
        }
    }
    
    // 启用/禁用日志
    void setEnabled(bool enabled) {
        enabled_ = enabled;
    }
    
    // 启用/禁用特定模块的日志
    void setModuleEnabled(LogModule module, bool enabled) {
        modules_enabled_[static_cast<size_t>(module)] = enabled;
    }
    
    // 检查特定模块是否启用
    bool isModuleEnabled(LogModule module) const {
        return modules_enabled_[static_cast<size_t>(module)];
    }
    
    // 获取队列中待处理的日志数量
    size_t pendingLogs() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return queue_.size();
    }
    
    // 等待所有日志写入完成
    void flush() {
        while (pendingLogs() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // 清理资源
    static void cleanup() {
        if (instance_) {
            instance_->flush();
            // 单例会在程序退出时自动销毁
        }
    }
    
    // 设置日志文件名
    void setLogFile(const std::string& filename) {
        // 停止当前写入线程
        stopWriterThread();
        
        // 重新打开文件
        log_file_.open(filename, std::ios::out | std::ios::app);
        if (!log_file_.is_open()) {
            throw std::runtime_error("无法打开日志文件: " + filename);
        }
        
        // 重置停止标志并重新启动线程
        writer_stop_ = false;
        startWriterThread();
    }
    
    // 设置是否输出到控制台
    void setConsoleOutput(bool enable) {
        // 这里可以扩展，目前是硬编码为总是输出到控制台
        // 如果需要动态控制，可以添加一个成员变量
    }
};

// 初始化静态成员
Logger* Logger::instance_ = nullptr;

// 方便使用的宏
#define LOG(level, module, format, ...) \
    Logger::getInstance().log(level, module, format, ##__VA_ARGS__)

#define LOG_DEBUG(module, format, ...) \
    LOG(LogLevel::DEBUG, module, format, ##__VA_ARGS__)

#define LOG_INFO(module, format, ...) \
    LOG(LogLevel::INFO, module, format, ##__VA_ARGS__)

#define LOG_NOTICE(module, format, ...) \
    LOG(LogLevel::NOTICE, module, format, ##__VA_ARGS__)

#define LOG_WARNING(module, format, ...) \
    LOG(LogLevel::WARNING, module, format, ##__VA_ARGS__)

#define LOG_ERROR(module, format, ...) \
    LOG(LogLevel::ERROR, module, format, ##__VA_ARGS__)

#define LOG_CRITICAL(module, format, ...) \
    LOG(LogLevel::CRITICAL, module, format, ##__VA_ARGS__)

// 带源码位置的日志宏
#define LOG_SOURCE(level, module, format, ...) \
    Logger::getInstance().logWithSource(level, module, \
        std::source_location::current(), format, ##__VA_ARGS__)

// 使用 std::format 的 C++ 风格日志宏
#define LOGCPP(level, module, format, ...) \
    Logger::getInstance().logCpp(level, module, format, ##__VA_ARGS__)

#endif // LOGGER_H