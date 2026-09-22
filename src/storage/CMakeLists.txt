# 存储引擎静态库(storage): M1 堆页追加 + 全表扫描, 规划 B+ 树聚簇索引
# 目录逻辑在 catalog 库, 本库只含页/文件/缓冲池/堆; 报错/告警依赖 log 库, catalog 与测试链接它

add_library(storage STATIC
    engine.cpp
    file_manager.cpp
    buffer_pool.cpp
    codec.cpp
    page.cpp
)

target_include_directories(storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 源文件经 log/log.h 报错/告警、经 common/err.h 落地报错, 链接两库
target_link_libraries(storage PRIVATE log common)