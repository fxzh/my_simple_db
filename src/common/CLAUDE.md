# common(err.h/err.cpp)
跨层错误库：DbError + DB_RAISE

- 报错约定：源头用 DB_RAISE——先记一条 ERROR 日志(错误码+Boost.Stacktrace)再抛 DbError；catch 处取 code()/what() 组错误帧回客户端，不重复记日志；禁止在深处 LOG_ERROR 后依赖隐式 throw
- ErrCode 全量经 server 穷尽 switch 映射为 proto wire 码入错误帧；会话层判定类码(SyntaxError/事务控制/TooManyClients)不抛 DbError，仅作帧分类
