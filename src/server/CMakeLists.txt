# 服务器可执行文件
add_executable(server
    config.cpp
    server.cpp
)

# 链接 SQL 词法/语法解析库与执行层
target_link_libraries(server PRIVATE sql_parser executor)

# 平台特定的链接库
if(WIN32)
    target_link_libraries(server ws2_32)
endif()

# 链接 Boost（如果顶层找到）
if(Boost_FOUND)
    target_include_directories(server PRIVATE ${Boost_INCLUDE_DIRS})
endif()

# 设置可执行文件的属性
set_target_properties(server PROPERTIES
    OUTPUT_NAME "server"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 可选的：安装规则
install(TARGETS server
    RUNTIME DESTINATION bin
    CONFIGURATIONS Release
)