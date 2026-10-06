# 计划层静态库(planner): 把绑定语句转成计划节点树并运行计划期优化 pass
add_library(planner STATIC
    bool_simplify.cpp
    const_fold.cpp
    planner.cpp
)

target_include_directories(planner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(planner PUBLIC analyzer expr)

target_link_libraries(planner PRIVATE log common)
