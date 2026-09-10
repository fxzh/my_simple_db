# 日志库(log.h 纯头文件)
单例 Logger + 异步写线程：调用方把 LogMessage 入队，写线程统一格式化追加到 simple.log(工作目录)。

# 使用
LOG(LEVEL, MODULE, printf格式, ...)     // 便捷宏 LOG_DEBUG/LOG_INFO/LOG_WARNING/... 同形
LOGCPP(LEVEL, MODULE, "{}", ...)        // std::format 风格；LOG_SOURCE 附带 source_location

# 行为要点
- LogLevel: DEBUG5~CRITICAL；LogModule: SYNTAX/PARSER/PLANNER/EXECUTOR/NETWORK/SYSTEM/GENERAL
- ERROR 及以上附带 Boost.Stacktrace 堆栈；ERROR 抛出 std::runtime_error(见 server 兜底)；CRITICAL 以 EXIT_FAILURE 退出进程
- 无需初始化：getInstance() 首次调用创建；进程退出自动析构
- 依赖 Boost.Stacktrace(顶层 CMake 硬性检查)、std::format、localtime_r；控制台输出当前硬编码关闭