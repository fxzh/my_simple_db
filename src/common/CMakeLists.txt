# 跨层错误静态库(common): DB_RAISE/DB_CRITICAL 的实现集中于 err.cpp
add_library(common STATIC
    err.cpp
)

# err.h 报错记日志依赖 log/log.h 的 Logger
target_link_libraries(common PUBLIC log)

# err.cpp 捕获堆栈需要 Boost.Stacktrace 头文件
if(BoostFound)
    target_include_directories(common PRIVATE ${Boost_INCLUDE_DIRS})
endif()
