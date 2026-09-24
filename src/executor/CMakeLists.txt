# 执行层静态库(executor): 把绑定语句转成 catalog 调用, server 链接它
add_library(executor STATIC
    executor.cpp
    expr_eval.cpp
)

target_include_directories(executor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 依赖 planner(计划节点树)、analyzer(绑定层)、parser(AST 类型)、storage(st 类型)与 catalog(目录调用),
# PUBLIC 使头文件 include 与链接随依赖传播给最终链接 executor 的 server
target_link_libraries(executor PUBLIC planner analyzer sql_parser storage catalog)

# 执行层报错/告警使用 log/log.h 与 common/err.h, 链接两库
target_link_libraries(executor PRIVATE log common)