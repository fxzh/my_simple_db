# 执行层静态库(executor): 把计划节点树转成 catalog 调用与迭代算子
add_library(executor STATIC
    executor.cpp
    explain.cpp
    operator.cpp
)

target_include_directories(executor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(executor PUBLIC planner analyzer sql_parser storage catalog)
target_link_libraries(executor PRIVATE expr log common)