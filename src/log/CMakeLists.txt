# 日志静态库(log): Logger 实现集中于 log.cpp, 头文件仅保留声明与宏
add_library(log STATIC
    log.cpp
)

# log.cpp 捕获堆栈需要 Boost.Stacktrace 头文件
if(BoostFound)
    target_include_directories(log PRIVATE ${Boost_INCLUDE_DIRS})
endif()
