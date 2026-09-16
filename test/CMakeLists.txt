# 测试目录: 使用系统已装 googletest, 仅 ENABLE_TESTING=ON 时构建
find_package(GTest)
if(NOT GTest_FOUND)
    message(FATAL_ERROR "googletest not found, install libgtest-dev first")
endif()

# 冒烟测试: 验证 gtest 链接/ctest 注册全链路
add_executable(test_smoke smoke.cpp)
target_link_libraries(test_smoke PRIVATE GTest::gtest_main)

add_test(NAME smoke COMMAND test_smoke)

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
