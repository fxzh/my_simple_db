# 计划层静态库(planner): 把绑定语句转成计划节点树, executor 链接它
add_library(planner STATIC
    planner.cpp
)

target_include_directories(planner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 依赖 analyzer(BoundStmt/Schema 类型), PUBLIC 使头文件 include 与链接随依赖传播给最终链接 planner 的 executor
target_link_libraries(planner PUBLIC analyzer)

# 计划层报错使用 log/log.h 与 common/err.h, 链接两库
target_link_libraries(planner PRIVATE log common)
