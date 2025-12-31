# 客户端可执行文件
add_executable(client
    client.cpp
)

# 平台特定的链接库
if(WIN32)
    target_link_libraries(client ws2_32)
endif()

# 设置可执行文件的属性
set_target_properties(client PROPERTIES
    OUTPUT_NAME "client"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 可选的：安装规则
install(TARGETS client
    RUNTIME DESTINATION bin
    CONFIGURATIONS Release
)