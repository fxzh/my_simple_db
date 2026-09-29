# serverctl: 只链接 config(db.conf 解析), 不链接 sql_parser/executor/readline
add_executable(serverctl
    serverctl.cpp
)

target_link_libraries(serverctl PRIVATE config)
set_target_properties(serverctl PROPERTIES
    OUTPUT_NAME "serverctl"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)