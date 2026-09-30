# 存储引擎静态库(storage): 堆页追加 + 全表扫描 + B+ 树二级索引 + WAL/崩溃恢复
# 目录逻辑在 catalog 库, 本库只含页/文件/缓冲池/堆/B+树/WAL; 报错/告警依赖 log 库, catalog 与测试链接它

add_library(storage STATIC
    engine.cpp
    file_manager.cpp
    buffer_pool.cpp
    codec.cpp
    page.cpp
    btree.cpp
    wal.cpp
    recovery.cpp
)

target_include_directories(storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 源文件经 log/log.h 报错/告警、经 common/err.h 落地报错、engine.cpp 读 config 全局配置, 链接三库
target_link_libraries(storage PRIVATE log common config)