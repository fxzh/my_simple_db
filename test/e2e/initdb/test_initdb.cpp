// test_initdb.cpp: initdb L1 e2e, Ok 提供 DB_READY fixture, 其余用例独立执行
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/process.hpp"
#include "common/temp_dir.hpp"

// 编译期注入: 被测二进制目录与跨用例状态目录
constexpr const char* kBinDir = MSDB_BIN_DIR;
constexpr const char* kStateDir = MSDB_STATE_DIR;

using tcommon::ProcessResult;
using tcommon::TempDir;

// 便捷封装: 基建故障直接判负, 不混入被测进程行为断言
namespace {

bool run_ok(const std::vector<std::string>& argv, int timeout_ms, ProcessResult& r)
{
    std::string error;
    if (!tcommon::run_process(argv, timeout_ms, r, error)) {
        ADD_FAILURE() << "run_process 基建错误: " << error;
        return false;
    }
    return true;
}

std::string bin(const char* name)
{
    return (std::filesystem::path(kBinDir) / name).string();
}

}  // namespace

TEST(Initdb, Ok)
{
    // 每轮重建状态目录, 供 serverctl 门控链复用, 无隐式历史
    std::error_code ec;
    std::filesystem::remove_all(kStateDir, ec);
    std::filesystem::create_directories(kStateDir, ec);
    ASSERT_FALSE(ec) << "重建状态目录失败: " << ec.message();
    std::string data = (std::filesystem::path(kStateDir) / "data").string();

    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 5000, r));
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(r.out.find("已初始化数据目录"), std::string::npos) << r.err;
    std::filesystem::path conf = std::filesystem::path(data) / "db.conf";
    EXPECT_TRUE(std::filesystem::is_regular_file(conf));
    std::ifstream in(conf);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("port = 8123"), std::string::npos);
}

TEST(Initdb, DirExistsEmpty)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_initdb", error)) << error;
    std::string data = (std::filesystem::path(dir.path) / "data").string();
    std::filesystem::create_directories(data);

    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 5000, r));
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::path(data) / "db.conf"));
}

TEST(Initdb, NonEmptyDir)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_initdb", error)) << error;
    std::string data = (std::filesystem::path(dir.path) / "data").string();
    std::filesystem::create_directories(data);
    {
        std::ofstream out(std::filesystem::path(data) / "occupy.txt");
        out << "x";
    }

    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 5000, r));
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(r.err.find("目录非空"), std::string::npos);
}

TEST(Initdb, PathIsFile)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_initdb", error)) << error;
    std::string data = (std::filesystem::path(dir.path) / "notadir").string();
    {
        std::ofstream out(data);
        out << "x";
    }

    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 5000, r));
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(r.err.find("不是目录"), std::string::npos);
}

TEST(Initdb, ParentNotWritable)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root 下权限语义不成立";
    }
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_initdb", error)) << error;
    std::filesystem::path parent = std::filesystem::path(dir.path) / "parent";
    std::filesystem::create_directories(parent);
    ASSERT_EQ(::chmod(parent.string().c_str(), 0500), 0);
    std::string data = (parent / "data").string();

    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 5000, r));
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(r.err.find("无法创建数据目录"), std::string::npos);
    ASSERT_EQ(::chmod(parent.string().c_str(), 0700), 0);
}

TEST(Initdb, Usage)
{
    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb")}, 5000, r));
    EXPECT_EQ(r.exit_code, 2);
    EXPECT_NE(r.err.find("用法"), std::string::npos);
}
