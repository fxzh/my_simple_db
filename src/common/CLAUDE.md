# common(err.h/err.cpp)
跨层错误库

- 报错约定：源头用 DB_RAISE——先记一条 ERROR 日志(错误码+堆栈)再抛 DbError；catch 处取 code()/what() 组错误帧回客户端，不重复记日志；禁止在深处记 error 后依赖隐式 throw
- 致命错误用 DB_CRITICAL——记 CRITICAL 日志；无 ErrCode、不出客户端错误帧；不承担调用点业务收尾
- 崩溃式致命错误用 DB_CRASH——记 CRITICAL 日志后立即 abort，不排空不析构；仅用于退出流程会挂死或扩大损坏的场景
- ERROR/CRITICAL 日志的堆栈在本库(err.cpp)组装，log 层不附栈；这两个级别只能经 DB_RAISE/DB_CRITICAL 出口
- ErrCode 全量经 server 穷尽 switch 映射为 proto wire 码入错误帧；会话层判定类码(SyntaxError/事务控制/TooManyClients)不抛 DbError，仅作帧分类
