# 测试目录: 使用系统已装 googletest, 仅 ENABLE_TESTING=ON 时构建
find_package(GTest)
if(NOT GTest_FOUND)
    message(FATAL_ERROR "googletest not found, install libgtest-dev first")
endif()

# e2e 基建静态库: 仅 POSIX 接口, 目标平台为 Linux
add_library(test_common STATIC
    common/temp_dir.cpp
    common/process.cpp
    common/net_probe.cpp
)
target_include_directories(test_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 基建自测
add_executable(test_common_selftest common_selftest.cpp)
target_link_libraries(test_common_selftest PRIVATE test_common GTest::gtest_main)

add_test(NAME common_selftest COMMAND test_common_selftest)

# L0 模块冒烟: storage 页删除整理/增删扫/重开持久化, TempDir 隔离数据目录
add_executable(test_storage storage/test_storage.cpp)
target_link_libraries(test_storage PRIVATE catalog storage test_common GTest::gtest_main)

add_test(NAME storage_smoke COMMAND test_storage)

# L1 e2e: initdb 正常路径(挂 DB_READY)与异常路径(独立执行)
add_executable(test_initdb e2e/initdb/test_initdb.cpp)
target_link_libraries(test_initdb PRIVATE test_common GTest::gtest_main)
target_compile_definitions(test_initdb PRIVATE
    MSDB_BIN_DIR="${PROJECT_BINARY_DIR}/bin"
    MSDB_STATE_DIR="${CMAKE_CURRENT_BINARY_DIR}/e2e_state"
)

add_test(NAME initdb_ok COMMAND test_initdb --gtest_filter=Initdb.Ok)
set_tests_properties(initdb_ok PROPERTIES FIXTURES_SETUP DB_READY)

add_test(NAME initdb_empty_dir_ok COMMAND test_initdb --gtest_filter=Initdb.DirExistsEmpty)
add_test(NAME initdb_nonempty COMMAND test_initdb --gtest_filter=Initdb.NonEmptyDir)
add_test(NAME initdb_path_is_file COMMAND test_initdb --gtest_filter=Initdb.PathIsFile)
add_test(NAME initdb_parent_readonly COMMAND test_initdb --gtest_filter=Initdb.ParentNotWritable)
add_test(NAME initdb_usage COMMAND test_initdb --gtest_filter=Initdb.Usage)

# L1 e2e: serverctl 门控链与独立异常用例
add_executable(test_serverctl e2e/serverctl/test_serverctl.cpp)
target_link_libraries(test_serverctl PRIVATE test_common GTest::gtest_main)
target_compile_definitions(test_serverctl PRIVATE
    MSDB_BIN_DIR="${PROJECT_BINARY_DIR}/bin"
    MSDB_STATE_DIR="${CMAKE_CURRENT_BINARY_DIR}/e2e_state"
)

add_test(NAME server_start COMMAND test_serverctl --gtest_filter=Ctl.Start)
set_tests_properties(server_start PROPERTIES
    FIXTURES_REQUIRED DB_READY
    FIXTURES_SETUP SRV_UP
)

add_test(NAME server_status COMMAND test_serverctl --gtest_filter=Ctl.StatusRunning)
set_tests_properties(server_status PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME server_double_start COMMAND test_serverctl --gtest_filter=Ctl.DoubleStart)
set_tests_properties(server_double_start PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME server_stop COMMAND test_serverctl --gtest_filter=Ctl.Stop)
set_tests_properties(server_stop PROPERTIES FIXTURES_CLEANUP SRV_UP)

add_test(NAME server_stop_not_running COMMAND test_serverctl --gtest_filter=Ctl.StopNotRunning)
add_test(NAME server_status_not_running COMMAND test_serverctl --gtest_filter=Ctl.StatusNotRunning)

# L2 SQL e2e: golden file 驱动, 以 client -a -c 的 stdout 全文 diff 为唯一通过判据
add_executable(test_sql e2e/sql/test_sql.cpp)
target_link_libraries(test_sql PRIVATE test_common)
target_compile_definitions(test_sql PRIVATE
    MSDB_BIN_DIR="${PROJECT_BINARY_DIR}/bin"
    MSDB_STATE_DIR="${CMAKE_CURRENT_BINARY_DIR}/e2e_state"
)

# SQL 用例各自管库表生命周期(开头建表, 结尾删表), 仅依赖 SRV_UP, 相互独立;
# 全部挂 SRV_UP, 保证 server_stop 在所有 SQL 用例之后收尾
set(SQL_CASES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/e2e/sql)

add_test(NAME sql_ddl COMMAND test_sql
    ${SQL_CASES_DIR}/ddl/cases/ddl.sql
    ${SQL_CASES_DIR}/ddl/expected/ddl.out)
set_tests_properties(sql_ddl PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME sql_dml_insert COMMAND test_sql
    ${SQL_CASES_DIR}/dml/cases/insert.sql
    ${SQL_CASES_DIR}/dml/expected/insert.out)
set_tests_properties(sql_dml_insert PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME sql_dml_delete COMMAND test_sql
    ${SQL_CASES_DIR}/dml/cases/delete.sql
    ${SQL_CASES_DIR}/dml/expected/delete.out)
set_tests_properties(sql_dml_delete PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME sql_dml_insert_null COMMAND test_sql
    ${SQL_CASES_DIR}/dml/cases/insert_null.sql
    ${SQL_CASES_DIR}/dml/expected/insert_null.out)
set_tests_properties(sql_dml_insert_null PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME sql_select_basic COMMAND test_sql
    ${SQL_CASES_DIR}/select/cases/select_basic.sql
    ${SQL_CASES_DIR}/select/expected/select_basic.out)
set_tests_properties(sql_select_basic PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME sql_proto_long COMMAND test_sql
    ${SQL_CASES_DIR}/proto/cases/long_sql.sql
    ${SQL_CASES_DIR}/proto/expected/long_sql.out)
set_tests_properties(sql_proto_long PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME sql_types COMMAND test_sql
    ${SQL_CASES_DIR}/types/cases/types.sql
    ${SQL_CASES_DIR}/types/expected/types.out)
set_tests_properties(sql_types PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME sql_reserved COMMAND test_sql
    ${SQL_CASES_DIR}/reserved/cases/reserved.sql
    ${SQL_CASES_DIR}/reserved/expected/reserved.out)
set_tests_properties(sql_reserved PROPERTIES FIXTURES_REQUIRED SRV_UP)
