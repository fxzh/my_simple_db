# 日志库(log.h 纯头文件)
单例 Logger + 异步写线程：调用方把 LogMessage 入队，写线程统一格式化追加到日志文件。

# 使用
LOG(LEVEL, MODULE, printf格式, ...)     // 便捷宏 LOG_DEBUG/LOG_INFO/LOG_WARNING/... 同形
LOGCPP(LEVEL, MODULE, "{}", ...)        // std::format 风格；LOG_SOURCE 附带 source_location
DB_RAISE(ErrCode, MODULE, "{}", ...)    // common/error.h：源头报错，记 ERROR 日志并抛 DbError

# 行为要点
- LogLevel: DEBUG5~CRITICAL；LogModule: SYNTAX/PARSER/PLANNER/EXECUTOR/NETWORK/SYSTEM/STORAGE/GENERAL
- ERROR 及以上附带 Boost.Stacktrace 堆栈；log() 无 throw/exit 副作用，落地报错走 common/error.h 的 DB_RAISE
  (源头记 ERROR 并抛 DbError)；CRITICAL 同步补一路 stderr，进程命运由调用方决定
- 使用前必须 Logger::initPath(路径) 设置日志文件路径(单例首次使用前)，未设置即打日志抛错；
  server 传数据目录内 simple.log；进程退出自动析构
- 依赖 Boost.Stacktrace(顶层 CMake 硬性检查)、std::format、localtime_r；控制台输出当前硬编码关闭