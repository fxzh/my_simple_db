# src 模块划分
client   客户端可执行程序：交互收集完整 SQL，-p 参数指定端口(缺省 8123)，TCP 发给 server
parser   服务端 SQL 词法/语法解析静态库(sql_parser)，语法校验并返回首个语句 AST
analyzer 语义分析层静态库：把 parser 的 AST 经 catalog 元数据绑定为 BoundStmt(类型映射/名字解析/类型推导/约束检查/投影展开)
planner  计划层静态库(planner)：把 BoundStmt 转成计划节点树(Project[Filter[SeqScan]]/叶子计划)，逻辑计划即物理计划
executor 执行层静态库(executor)：先经 ana::analyze 绑定、pl::build 生成计划，再把计划节点树转成 catalog 调用
server   服务端可执行程序：多线程 TCP，-D <数据目录> 必选启动，读目录内 db.conf 配置端口与控制通道，sql_parser 分析后经 executor 执行，使用 log
log      日志静态库：单例 + 异步写线程，输出 simple.log
common    跨层错误库：DbError + DB_RAISE，源头报错记日志并抛结构化异常
proto    帧协议头文件(header-only)：长度前缀+消息类型，client 与 server 共用，无链接依赖
storage  文件引擎静态库：M1 堆页追加+全表扫描(页/文件/缓冲池/编解码)，后续 B+树/WAL
catalog  目录层静态库(catalog)：元数据表(db_table/db_column)逻辑与名字型门面，持全局锁组合引擎原语
tool     独立工具

依赖关系：server → parser、log、common、executor；executor → planner、analyzer、parser、catalog、storage、log、common(报错/告警)、proto(header-only)；planner → analyzer、log、common(报错/告警)；analyzer → parser、catalog、storage、log、common(报错/告警)；catalog → storage、log、common(报错/告警)；storage → log、common(报错/告警)；client 与 server 仅通过 TCP 帧协议(proto)交互