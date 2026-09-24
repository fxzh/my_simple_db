# common(err.h/err.cpp)
跨层错误库：DbError + DB_RAISE

- 报错约定：源头用 DB_RAISE——先记一条 ERROR 日志(错误码+Boost.Stacktrace)再抛 DbError；catch 处只取 what() 回客户端，不重复记日志；禁止在深处 LOG_ERROR 后依赖隐式 throw
- ErrCode 枚举是将来 wire 协议错误码的候选，现阶段仅进日志、未入帧
