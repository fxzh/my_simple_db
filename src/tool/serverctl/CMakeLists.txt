# serverctl: 只复用 config.cpp(db.conf 解析), 不链接 sql_parser/executor/readline
add_executable(serverctl
    serverctl.cpp
    ${CMAKE_SOURCE_DIR}/src/server/config.cpp
)
set_target_properties(serverctl PROPERTIES
    OUTPUT_NAME "serverctl"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)