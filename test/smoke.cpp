// smoke.cpp: 冒烟测试, 验证 gtest 引入/编译/ctest 注册全链路
#include <gtest/gtest.h>

TEST(Smoke, FrameworkReady)
{
    EXPECT_EQ(1 + 1, 2);
    EXPECT_STREQ("my_simple_db", "my_simple_db");
}
