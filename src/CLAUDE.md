# src 模块划分
client   客户端可执行程序：交互收集完整 SQL，-p 参数指定端口(缺省 8123)，TCP 发给 server
parser   服务端 SQL 词法/语法解析静态库，语法校验并返回首个语句 AST
analyzer 语义分析层静态库：把 parser 的 AST 经 catalog 元数据绑定为 BoundStmt(类型映射/名字解析/类型推导/约束检查/投影展开)
expr     表达式求值静态库：绑定表达式常量/行上下文求值，planner(常量折叠)与 executor(运行期)共用
planner  计划层静态库：把 BoundStmt 转成计划节点树并运行优化 pass，逻辑计划即物理计划
executor 执行层静态库：先经 ana::analyze 绑定、pl::build 生成计划、pl::optimize 优化，再把计划节点树转成 catalog 调用；EXPLAIN 语句为渲染计划树为文本行结果集
server   服务端可执行程序：多线程 TCP，-D <数据目录> 必选启动，读目录内 db.conf 配置端口与控制通道，sql_parser 分析后经 executor 执行，使用 log
log      日志静态库：单例 + 异步写线程，输出 simple.log
common    跨层错误库：DbError + DB_RAISE + DB_CRITICAL，源头报错记日志并抛结构化异常/致命退出
utils    通用函数静态库
config    配置静态库：db.conf 逐项解析写入全局变量 cfg，零依赖
proto    帧协议头文件：长度前缀+消息类型，client 与 server 共用，无链接依赖
storage  文件引擎静态库：堆页追加+全表扫描+二级索引原语+WAL 崩溃恢复
catalog  目录层静态库：元数据表逻辑与门面（按名/按句柄两类入口），持全局锁组合引擎原语
tool     独立工具
