# 表达式求值静态库(expr): 绑定表达式常量/行上下文求值
add_library(expr STATIC
    expr_eval.cpp
)

target_include_directories(expr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(expr PUBLIC analyzer storage)
target_link_libraries(expr PRIVATE log common)
