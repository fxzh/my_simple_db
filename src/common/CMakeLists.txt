# 跨层错误静态库(common): DB_RAISE 的实现集中于 error.cpp, 头文件仅保留声明与宏
add_library(common STATIC
    error.cpp
)

# error.h 报错记日志依赖 log/log.h 的 Logger
target_link_libraries(common PUBLIC log)
