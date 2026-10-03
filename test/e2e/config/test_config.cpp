// test_config.cpp: server 配置项 e2e, 非法配置值启动即退
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/process.hpp"
#include "common/temp_dir.hpp"
#include "e2e/test_config.hpp"

// 编译期注入: 被测二进制目录
constexpr const char* kBinDir = MSDB_BIN_DIR;

using tcommon::ProcessResult;
using tcommon::TempDir;

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

// buffer_pool_frames 低于下限 16 时 server 拒绝启动
TEST(Config, BadBufferPoolFrames)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_conf", error)) << error;
    std::string data = (std::filesystem::path(dir.path) / "data").string();

    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 10000, r));
    ASSERT_EQ(r.exit_code, 0) << r.err;

    {
        std::ofstream out(std::filesystem::path(data) / "db.conf");
        out << "port = " << kTestPort << "\n"
            << "buffer_pool_frames = 15\n";
    }

    ASSERT_TRUE(run_ok({bin("server"), "-D", data}, 5000, r));
    EXPECT_NE(r.exit_code, 0);
    EXPECT_NE(r.err.find("buffer_pool_frames 超出范围 16~1048576"), std::string::npos) << r.err;
}

// server_log_level 值非法时 server 拒绝启动
TEST(Config, BadServerLogLevel)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_conf", error)) << error;
    std::string data = (std::filesystem::path(dir.path) / "data").string();

    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 10000, r));
    ASSERT_EQ(r.exit_code, 0) << r.err;

    {
        std::ofstream out(std::filesystem::path(data) / "db.conf");
        out << "port = " << kTestPort << "\n"
            << "server_log_level = banana\n";
    }

    ASSERT_TRUE(run_ok({bin("server"), "-D", data}, 5000, r));
    EXPECT_NE(r.exit_code, 0);
    EXPECT_NE(r.err.find("server_log_level 值非法"), std::string::npos) << r.err;
}
