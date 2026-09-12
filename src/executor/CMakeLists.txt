# 执行层静态库(executor): 把 parser 的 AST 转成 storage 调用, server 链接它
add_library(executor STATIC
    executor.cpp
)

target_include_directories(executor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 依赖 parser(AST 类型)与 storage(存储调用), PUBLIC 使头文件 include 与链接
# 随依赖传播给最终链接 executor 的 server
target_link_libraries(executor PUBLIC sql_parser storage)

# 执行层报错/告警使用 log.h, 编译需要 Boost.Stacktrace 头文件
if(BoostFound)
    target_include_directories(executor PRIVATE ${Boost_INCLUDE_DIRS})
endif()