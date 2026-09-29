// test_sqlfile.cpp, 用法: test_sqlfile <case.sql> <expected.out>
// client -f 文件执行入口的驱动测试:
//   Ok       复用 lexer 用例经 -a -f 执行, stdout 与 -c 预期全文一致
//            (用例末尾为预期报错语句, 退出码为 1), 挂 SRV_UP
//   BadPath/Empty/Conflict 在连接建立前即报错退出, 无需 server
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/process.hpp"
#include "common/temp_dir.hpp"
#include "e2e/test_config.hpp"

// 编译期注入: 被测二进制目录
constexpr const char* kBinDir = MSDB_BIN_DIR;

// 用例与预期文件路径, 由命令行传入
static std::string g_case_path;
static std::string g_expected_path;

// 读文件全文(二进制), 失败返回 false
static bool read_file(const std::string& path, std::string& content)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// 便捷封装: 基建故障直接判负, 不混入被测进程行为断言
static bool run_client(const std::vector<std::string>& args, tcommon::ProcessResult& r)
{
    std::vector<std::string> argv_list = {(std::filesystem::path(kBinDir) / "client").string()};
    argv_list.insert(argv_list.end(), args.begin(), args.end());
    std::string error;
    if (!tcommon::run_process(argv_list, 15000, r, error)) {
        ADD_FAILURE() << "run_process 基建错误: " << error;
        return false;
    }
    return true;
}

// -f 正常路径: 输出与 -c 预期全文一致, 退出码 1(用例末尾为预期报错语句)
TEST(SqlFile, Ok)
{
    std::string expected;
    ASSERT_TRUE(read_file(g_expected_path, expected)) << "读取预期失败: " << g_expected_path;

    tcommon::ProcessResult r;
    ASSERT_TRUE(run_client({"-a", "-h", "127.0.0.1", "-p", std::to_string(kTestPort),
                            "-f", g_case_path},
                           r));
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_EQ(r.out, expected);
}

// -f 文件不存在: 连接前报错退出
TEST(SqlFile, BadPath)
{
    tcommon::TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_sqlfile", error)) << error;
    const std::string path = (std::filesystem::path(dir.path) / "no_such.sql").string();

    tcommon::ProcessResult r;
    ASSERT_TRUE(run_client({"-f", path}, r));
    EXPECT_NE(r.exit_code, 0);
    EXPECT_NE(r.err.find("无法打开SQL文件"), std::string::npos) << r.out << r.err;
    EXPECT_TRUE(r.out.empty());
}

// -f 空文件: 连接前报错退出
TEST(SqlFile, Empty)
{
    tcommon::TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_sqlfile", error)) << error;
    const std::string path = (std::filesystem::path(dir.path) / "empty.sql").string();
    {
        std::ofstream out(path);
    }

    tcommon::ProcessResult r;
    ASSERT_TRUE(run_client({"-f", path}, r));
    EXPECT_NE(r.exit_code, 0);
    EXPECT_NE(r.err.find("SQL文件为空"), std::string::npos) << r.out << r.err;
    EXPECT_TRUE(r.out.empty());
}

// -c 与 -f 同给: 参数解析报错退出
TEST(SqlFile, Conflict)
{
    tcommon::ProcessResult r;
    ASSERT_TRUE(run_client({"-c", "SELECT 1", "-f", g_case_path}, r));
    EXPECT_NE(r.exit_code, 0);
    EXPECT_NE(r.err.find("不能同时指定"), std::string::npos) << r.out << r.err;
}

int main(int argc, char* argv[])
{
    // 先剥离 gtest 参数(--gtest_filter 等), 剩余 argv[1]/argv[2] 为用例与预期路径
    ::testing::InitGoogleTest(&argc, argv);
    if (argc != 3) {
        std::cerr << "用法: " << argv[0] << " <case.sql> <expected.out>" << std::endl;
        return 2;
    }
    g_case_path = argv[1];
    g_expected_path = argv[2];
    return RUN_ALL_TESTS();
}
