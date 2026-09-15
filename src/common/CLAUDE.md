# 跨层错误(common 库: error.h + error.cpp, 依赖 log)
db::DbError(code 分类码 / what() 客户端文案 / location 触发位置) + 宏 DB_RAISE(码, MODULE, fmt, ...)。
实现集中于 error.cpp(raise_error_impl)

# 使用
DB_RAISE(db::ErrCode::X, LogModule::Y, "fmt{}", arg)
在报错源头调用: 先在 simple.log 记一条 ERROR(带错误码与 Boost.Stacktrace 堆栈), 再抛 DbError。
server 端 catch(const db::DbError&) 只取 what() 回客户端 "ERROR: <文案>", 由源头统一记账。

# 约定
- ErrCode 枚举即后续 wire 协议的错误码候选, 现阶段仅进日志, 未入帧
- fmt 为 std::format "{}" 风格(运行时格式串经 std::vformat 展开), 文案与客户端展示一致
- 报错即抛即有日志; 禁止在深处 LOG_ERROR 后依赖隐式 throw 继续(已由 DB_RAISE 取代)