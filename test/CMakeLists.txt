# 测试目录: 使用系统已装 googletest, 仅 ENABLE_TESTING=ON 时构建
find_package(GTest)
if(NOT GTest_FOUND)
    message(FATAL_ERROR "googletest not found, install it first")
endif()

# 冒烟测试: 验证 gtest 链接/ctest 注册全链路
add_executable(test_smoke smoke.cpp)
target_link_libraries(test_smoke PRIVATE GTest::gtest_main)

add_test(NAME smoke COMMAND test_smoke)
