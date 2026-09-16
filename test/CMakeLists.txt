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
