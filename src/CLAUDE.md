# src 模块划分
client   客户端可执行程序：交互收集完整 SQL → TCP 发给 server
parser   服务端 SQL 词法/语法解析静态库(sql_parser)，供 server 链接
server   服务端可执行程序：多线程 TCP，调用 sql_parser 做语法校验，使用 log
log      日志头文件库(log.h)：单例 + 异步写线程，输出 simple.log
storage  存储引擎静态库：M1 堆页追加+全表扫描(页/缓冲池/目录/编解码)，后续 B+树/WAL

依赖关系：server → parser、log；storage 独立(将来被执行器调用)；client 与 server 仅通过 TCP 交互