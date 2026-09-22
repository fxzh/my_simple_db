# initdb: 复用 config.cpp(db.conf 路径推导)与 catalog(生成元数据表), 不链接 sql_parser/executor/readline
add_executable(initdb
    initdb.cpp
    ${CMAKE_SOURCE_DIR}/src/server/config.cpp
)
target_link_libraries(initdb PRIVATE catalog)
set_target_properties(initdb PROPERTIES
    OUTPUT_NAME "initdb"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
