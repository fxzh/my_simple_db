# 绑定层静态库(analyzer): 把 parser 的 AST 绑定到 catalog 元数据, executor 链接它
add_library(analyzer STATIC
    analyzer.cpp
)

target_include_directories(analyzer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 依赖 parser(AST 类型)、storage(st 类型)与 catalog(元数据查找), PUBLIC 使头文件 include 与链接
# 随依赖传播给最终链接 analyzer 的 executor
target_link_libraries(analyzer PUBLIC sql_parser storage catalog)

# 绑定层报错/告警使用 log/log.h 与 common/err.h, 链接两库
target_link_libraries(analyzer PRIVATE log common)
