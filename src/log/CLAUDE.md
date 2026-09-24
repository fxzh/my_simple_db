# log
日志：单例 Logger + 异步写线程，调用方把 LogMessage 入队，写线程统一格式化追加到日志文件

- 使用前必须 Logger::initPath(path)，且须在单例首次使用前完成，未设置即打日志抛错；server 传数据目录内 simple.log
- 控制台输出当前硬编码关闭；CRITICAL 会同步补一路 stderr
