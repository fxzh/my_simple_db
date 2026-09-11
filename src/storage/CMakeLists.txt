# 存储引擎静态库(storage): M1 堆页追加 + 全表扫描, 规划 B+ 树聚簇索引
# 报错/告警依赖 log 头文件库与 Boost.Stacktrace, server 与测试链接它

add_library(storage STATIC
    storage.cpp
    file_manager.cpp
    buffer_pool.cpp
    catalog.cpp
    codec.cpp
    page.cpp
)

target_include_directories(storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 存储源文件编译需要 Boost.Stacktrace(log.h 依赖)
if(BoostFound)
    target_include_directories(storage PRIVATE ${Boost_INCLUDE_DIRS})
endif()

if(ENABLE_TESTING)
    add_executable(storage_smoke test/smoke.cpp)
    target_link_libraries(storage_smoke PRIVATE storage)
endif()