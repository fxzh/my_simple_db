# initdb: 复用 config(db.conf 路径推导)与 catalog(生成元数据表), 不链接 sql_parser/executor/readline
add_executable(initdb
    initdb.cpp
)
target_link_libraries(initdb PRIVATE catalog config)
set_target_properties(initdb PROPERTIES
    OUTPUT_NAME "initdb"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# bootstrap.sql 与二进制同目录部署, initdb 运行期按同目录伴生文件定位
add_custom_command(TARGET initdb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap.sql
            ${CMAKE_BINARY_DIR}/bin/bootstrap.sql
)

install(FILES bootstrap.sql
    DESTINATION bin
    CONFIGURATIONS Release
)
