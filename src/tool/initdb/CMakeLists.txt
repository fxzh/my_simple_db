# initdb: 只复用 config.cpp(db.conf 路径推导), 不链接 sql_parser/executor/readline
add_executable(initdb
    initdb.cpp
    ${CMAKE_SOURCE_DIR}/src/server/config.cpp
)
set_target_properties(initdb PROPERTIES
    OUTPUT_NAME "initdb"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
