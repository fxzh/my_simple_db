# serverctl: 链接 config(db.conf 解析)与 utils
add_executable(serverctl
    serverctl.cpp
)

target_link_libraries(serverctl PRIVATE config utils)
set_target_properties(serverctl PROPERTIES
    OUTPUT_NAME "serverctl"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)