# 服务器可执行文件
add_executable(server
    server.cpp
)

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