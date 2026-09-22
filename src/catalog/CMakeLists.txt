# 目录层静态库(catalog): 元数据表逻辑与名字型门面, executor 链接
# 报错/告警依赖 log 库, server 与测试链接它

add_library(catalog STATIC
    catalog.cpp
)

target_include_directories(catalog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# catalog.h 引用 engine.h 与 st 类型, storage 经 PUBLIC 传播 include 与链接
target_link_libraries(catalog PUBLIC storage)

# 源文件经 log/log.h 报错/告警、经 common/err.h 落地报错, 链接两库
target_link_libraries(catalog PRIVATE log common)
