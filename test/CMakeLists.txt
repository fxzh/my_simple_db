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

# L0 模块冒烟: storage 页删除整理/增删扫/重开持久化/WAL 记录往返, TempDir 隔离数据目录
add_executable(test_storage storage/test_storage.cpp)
target_link_libraries(test_storage PRIVATE catalog storage test_common GTest::gtest_main)

add_test(NAME storage_smoke COMMAND test_storage)

# L0 崩溃恢复: fork 子进程 _exit 模拟 kill -9, 验证 WAL 重放幸存与未提交残留撤销
add_executable(test_storage_crash storage/test_storage_crash.cpp)
target_link_libraries(test_storage_crash PRIVATE catalog storage test_common GTest::gtest_main)

add_test(NAME storage_crash COMMAND test_storage_crash)

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

# L1 e2e: server 配置项校验, 非法值启动即退
add_executable(test_config e2e/config/test_config.cpp)
target_link_libraries(test_config PRIVATE test_common GTest::gtest_main)
target_compile_definitions(test_config PRIVATE
    MSDB_BIN_DIR="${PROJECT_BINARY_DIR}/bin"
)

add_test(NAME config_bad_frames COMMAND test_config --gtest_filter=Config.BadBufferPoolFrames)
add_test(NAME config_bad_log_level COMMAND test_config --gtest_filter=Config.BadServerLogLevel)

# L2 SQL : 以 client -a -c 的 stdout 全文 diff 为唯一通过判据;
# 实际输出与 diff 落盘至 e2e_out/<功能目录>_<stem>.{out,diff}
add_executable(test_sql e2e/sql/test_sql.cpp)
target_link_libraries(test_sql PRIVATE test_common)
target_compile_definitions(test_sql PRIVATE
    MSDB_BIN_DIR="${PROJECT_BINARY_DIR}/bin"
    MSDB_STATE_DIR="${CMAKE_CURRENT_BINARY_DIR}/e2e_state"
    MSDB_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/e2e_out"
)

# SQL 用例各自管库表生命周期(开头建表, 结尾删表), 仅依赖 SRV_UP, 相互独立;
# 全部挂 SRV_UP, 保证 server_stop 在所有 SQL 用例之后收尾
set(SQL_CASES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/e2e/sql)

# 注册 L2 SQL 用例: 用例文件 <feature>/cases/<stem>.sql, 预期文件 <feature>/expected/<stem>.out, 统一挂 SRV_UP
function(add_sql_case test_name feature stem)
    add_test(NAME ${test_name} COMMAND test_sql
        ${SQL_CASES_DIR}/${feature}/cases/${stem}.sql
        ${SQL_CASES_DIR}/${feature}/expected/${stem}.out)
    set_tests_properties(${test_name} PROPERTIES FIXTURES_REQUIRED SRV_UP)
endfunction()

add_sql_case(sql_ddl             ddl       ddl)
add_sql_case(sql_ddl_schema      ddl       schema)
add_sql_case(sql_dml_insert      dml       insert)
add_sql_case(sql_dml_delete      dml       delete)
add_sql_case(sql_select_basic    select    select_basic)
add_sql_case(sql_proto_long      proto     long_sql)
add_sql_case(sql_proto_stream    proto     stream_batch)
add_sql_case(sql_proto_notice    proto     notice)
add_sql_case(sql_types           types     types)
add_sql_case(sql_where_basic     where     where_basic)
add_sql_case(sql_where_null      where     where_null)
add_sql_case(sql_where_char      where     where_char)
add_sql_case(sql_reserved        reserved  reserved)
add_sql_case(sql_semantic_insert semantic  semantic_insert)
add_sql_case(sql_semantic_select semantic  semantic_select)
add_sql_case(sql_semantic_set    semantic  semantic_set)
add_sql_case(sql_lexer_comment   lexer     lexer_comment)
add_sql_case(sql_lexer_string    lexer     lexer_string)
add_sql_case(sql_txn_basic       txn       txn_basic)
add_sql_case(sql_txn_error       txn       txn_error_rollback)
add_sql_case(sql_txn_control_err txn       txn_control_error)

# L2 e2e: 断连隐式回滚, 两次 client 连接接力验证(-f 脚本止于未提交事务)
add_executable(test_txn_disconnect e2e/sql/txn/test_disconnect.cpp)
target_link_libraries(test_txn_disconnect PRIVATE test_common GTest::gtest_main)
target_compile_definitions(test_txn_disconnect PRIVATE MSDB_BIN_DIR="${PROJECT_BINARY_DIR}/bin")

add_test(NAME sql_txn_disconnect COMMAND test_txn_disconnect)
set_tests_properties(sql_txn_disconnect PROPERTIES FIXTURES_REQUIRED SRV_UP)

# L2 e2e: client -f 文件执行入口, Ok 复用 lexer 用例与预期, 其余三项连接前报错无需 server
add_executable(test_sqlfile e2e/sqlfile/test_sqlfile.cpp)
target_link_libraries(test_sqlfile PRIVATE test_common GTest::gtest)
target_compile_definitions(test_sqlfile PRIVATE MSDB_BIN_DIR="${PROJECT_BINARY_DIR}/bin")

add_test(NAME sqlfile_ok COMMAND test_sqlfile
    ${SQL_CASES_DIR}/lexer/cases/lexer_comment.sql
    ${SQL_CASES_DIR}/lexer/expected/lexer_comment.out
    --gtest_filter=SqlFile.Ok)
set_tests_properties(sqlfile_ok PROPERTIES FIXTURES_REQUIRED SRV_UP)

add_test(NAME sqlfile_bad_path COMMAND test_sqlfile
    ${SQL_CASES_DIR}/lexer/cases/lexer_comment.sql
    ${SQL_CASES_DIR}/lexer/expected/lexer_comment.out
    --gtest_filter=SqlFile.BadPath)

add_test(NAME sqlfile_empty COMMAND test_sqlfile
    ${SQL_CASES_DIR}/lexer/cases/lexer_comment.sql
    ${SQL_CASES_DIR}/lexer/expected/lexer_comment.out
    --gtest_filter=SqlFile.Empty)

add_test(NAME sqlfile_conflict COMMAND test_sqlfile
    ${SQL_CASES_DIR}/lexer/cases/lexer_comment.sql
    ${SQL_CASES_DIR}/lexer/expected/lexer_comment.out
    --gtest_filter=SqlFile.Conflict)
