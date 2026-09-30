# initdb: 复用 config(db.conf 路径推导)、catalog(生成元数据表)与 utils
add_executable(initdb
    initdb.cpp
)
target_link_libraries(initdb PRIVATE catalog config utils)
set_target_properties(initdb PROPERTIES
    OUTPUT_NAME "initdb"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# bootstrap.sql 与二进制同目录部署, initdb 运行期按同目录伴生文件定位
# ALL 部署目标每次构建执行, 内容不变时 copy_if_different 为空操作
add_custom_target(deploy_bootstrap_sql ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap.sql
            ${CMAKE_BINARY_DIR}/bin/bootstrap.sql
)

install(FILES bootstrap.sql
    DESTINATION bin
    CONFIGURATIONS Release
)
