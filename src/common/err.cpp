// err.cpp: DB_RAISE 的非模板实现, 格式化收敛在库内完成
#include "common/err.h"

namespace db {

namespace detail {

[[noreturn]] void raise_error_impl(ErrCode code, LogModule module,
                                   const std::source_location& location,
                                   std::string_view fmt, std::format_args args)
{
    std::string message;
    try {
        message = std::vformat(fmt, args);
    } catch (const std::format_error& e) {
        message = std::string("[format error] ") + e.what();
    }
    const std::string logtext = std::format("[{}] {}", errCodeName(code), message);
    Logger::getInstance().log(LogLevel::ERROR, module, "%s", logtext.c_str());
    throw DbError(code, std::move(message), location);
}

}  // namespace detail

}  // namespace db
