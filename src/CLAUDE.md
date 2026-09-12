# src 模块划分
client   客户端可执行程序：交互收集完整 SQL，-p 参数指定端口(缺省 8123)，TCP 发给 server
parser   服务端 SQL 词法/语法解析静态库(sql_parser)，语法校验并返回首个语句 AST
executor 执行层静态库(executor)：把 parser 的 AST 转成 storage 调用
server   服务端可执行程序：多线程 TCP，启动读 db.conf(同目录)配置端口与数据目录，sql_parser 分析后经 executor 执行，使用 log
log      日志头文件库(log.h)：单例 + 异步写线程，输出 simple.log
storage  存储引擎静态库：M1 堆页追加+全表扫描(页/缓冲池/目录/编解码)，后续 B+树/WAL
tool     独立工具

依赖关系：server → parser、log、executor；executor → parser、storage、log(报错/告警)；storage → log(报错/告警)；client 与 server 仅通过 TCP 交互