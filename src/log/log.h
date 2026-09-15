#ifndef LOGGER_H
#define LOGGER_H

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
#include <utility>

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
constexpr std::string_view levelToString(LogLevel level)
{
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
    STORAGE,    // 存储模块
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

    // 日志文件路径, 单例构造前由 initPath 设置
    static std::string log_path_;

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
    Logger();

    // 格式化时间戳
    std::string formatTimestamp(const std::chrono::system_clock::time_point& tp);

    // 写入线程函数
    void writerThreadFunc();

    // 启动写入线程
    void startWriterThread();

    // 停止写入线程
    void stopWriterThread();

    // 格式化可变参数
    std::string formatMessage(const char* format, va_list args);

    // CRITICAL 级别的同步 stderr 出口: 队列写入之外的即时可见性, 供运维第一时间捕获
    // 进程是否退出由调用方决定(log() 只负责记录, 不承担控制流副作用)
    void echoCritical(const std::string& message);

    // 捕获当前调用栈文本, ERROR 及以上日志附在消息尾部
    std::string stacktraceText();

    // logCpp 的非模板实现, 格式化收敛在库内完成
    void logCppImpl(LogLevel level, LogModule module, std::string_view fmt,
                    std::format_args args);

public:
    // 删除拷贝构造和赋值
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    ~Logger();
    
    // 获取单例实例
    static Logger& getInstance()
    {
        static Logger instance;
        instance_ = &instance;
        return instance;
    }

    // 设置日志文件路径, 须在单例首次使用前调用
    static void initPath(const std::string& path)
    {
        log_path_ = path;
    }
    
    // 记录日志的主函数
    void log(LogLevel level, LogModule module, const char* format, ...);
    
    // 记录日志，带源码位置（可选功能）
    void logWithSource(LogLevel level, LogModule module, const std::source_location& location,
                       const char* format, ...);

    // 使用 std::format 的模板化记录方法，支持传入任意 C++ 类型参数
    template<typename... Args>
    void logCpp(LogLevel level, LogModule module, std::string_view fmt, Args&&... args)
    {
        logCppImpl(level, module, fmt, std::make_format_args(args...));
    }
    
    // 启用/禁用日志
    void setEnabled(bool enabled)
    {
        enabled_ = enabled;
    }
    
    // 启用/禁用特定模块的日志
    void setModuleEnabled(LogModule module, bool enabled)
    {
        modules_enabled_[static_cast<size_t>(module)] = enabled;
    }
    
    // 检查特定模块是否启用
    bool isModuleEnabled(LogModule module) const
    {
        return modules_enabled_[static_cast<size_t>(module)];
    }
    
    // 获取队列中待处理的日志数量
    size_t pendingLogs() const
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return queue_.size();
    }
    
    // 等待所有日志写入完成
    void flush()
    {
        while (pendingLogs() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // 清理资源
    static void cleanup()
    {
        if (instance_) {
            instance_->flush();
            // 单例会在程序退出时自动销毁
        }
    }
    
    // 设置是否输出到控制台
    void setConsoleOutput(bool enable)
    {
        // 这里可以扩展，目前是硬编码为总是输出到控制台
        // 如果需要动态控制，可以添加一个成员变量
        (void)enable;
    }
};

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