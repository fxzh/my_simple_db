# log
日志：单例 Logger + 异步写线程，调用方把 LogMessage 入队，写线程统一格式化追加到日志文件

- 使用前必须 Logger::initPath(path)，且须在单例首次使用前完成，未设置即打日志抛错；server 传数据目录内 simple.log
- 级别阈值 server_log_level(缺省 info)：低于阈值整条丢弃，ERROR 不豁免；初值经 initLevel 于单例构造前设置，运行期 setLevelThreshold 调整
- 控制台输出关闭；CRITICAL 会补一路 stderr
- 不附堆栈：ERROR/CRITICAL 的正文(含堆栈)由 common 的 DB_RAISE/DB_CRITICAL 组装，调用方不得绕开它们直接向本层传这两个级别
