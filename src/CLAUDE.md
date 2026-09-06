# src 模块划分
client   客户端可执行程序：交互收集完整 SQL → TCP 发给 server
parser   服务端 SQL 词法/语法解析静态库(sql_parser)，供 server 链接
server   服务端可执行程序：多线程 TCP，调用 sql_parser 做语法校验，使用 log
log      日志头文件库(log.h)：单例 + 异步写线程，输出 simple.log

依赖关系：server → parser、log；client 与 server 仅通过 TCP 交互，代码独立