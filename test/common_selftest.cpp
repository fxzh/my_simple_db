// common_selftest.cpp: test/common 基建自测(单元级)
#include <sys/socket.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "common/net_probe.hpp"
#include "common/process.hpp"
#include "common/temp_dir.hpp"

using tcommon::ProcessResult;
using tcommon::TempDir;

// 便捷封装: 基建故障直接判负, 不混入被测进程行为断言
namespace {

bool run_ok(const std::vector<std::string>& argv, ProcessResult& r)
{
    std::string error;
    if (!tcommon::run_process(argv, 5000, r, error)) {
        ADD_FAILURE() << "run_process 基建错误: " << error;
        return false;
    }
    return true;
}

}  // namespace

TEST(TempDir, CreateAndRemove)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_selftest", error)) << error;
    EXPECT_FALSE(dir.path.empty());
    EXPECT_TRUE(std::filesystem::is_directory(dir.path));
    {
        std::ofstream out(std::filesystem::path(dir.path) / "f.txt");
        out << "x";
    }
    EXPECT_TRUE(dir.remove(error)) << error;
    EXPECT_FALSE(std::filesystem::exists(dir.path));
}

TEST(TempDir, KeepPreservesDir)
{
    std::string kept;
    {
        TempDir dir;
        std::string error;
        ASSERT_TRUE(dir.create("msdb_selftest", error)) << error;
        dir.keep = true;
        kept = dir.path;
    }
    EXPECT_TRUE(std::filesystem::exists(kept));
    std::filesystem::remove_all(kept);
}

TEST(TempDir, RemoveFailureIsReported)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root 下权限语义不成立";
    }
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_selftest", error)) << error;
    std::filesystem::path sub = std::filesystem::path(dir.path) / "sub";
    std::filesystem::create_directories(sub);
    {
        std::ofstream out(sub / "inner.txt");
        out << "x";
    }
    ASSERT_EQ(::chmod(sub.string().c_str(), 0500), 0);
    std::string rm_error;
    EXPECT_FALSE(dir.remove(rm_error));
    EXPECT_FALSE(rm_error.empty());
    // 恢复权限后正常清理
    ASSERT_EQ(::chmod(sub.string().c_str(), 0700), 0);
    ASSERT_TRUE(dir.remove(error)) << error;
}

TEST(TempDir, ReadFileTailReturnsLastBytes)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_selftest", error)) << error;
    std::filesystem::path f = std::filesystem::path(dir.path) / "big.log";
    {
        std::ofstream out(f);
        out << std::string(10000, 'a') << "TAIL_MARKER";
    }
    std::string content;
    ASSERT_TRUE(tcommon::read_file_tail(f.string(), 32, content, error)) << error;
    EXPECT_EQ(content, std::string(21, 'a') + "TAIL_MARKER");
    std::string missing = (std::filesystem::path(dir.path) / "nope").string();
    EXPECT_FALSE(tcommon::read_file_tail(missing, 32, content, error));
    EXPECT_FALSE(error.empty());
}

TEST(Process, CapturesOutputAndExitCode)
{
    ProcessResult r;
    ASSERT_TRUE(run_ok({"/bin/echo", "hello"}, r));
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.out, "hello\n");
    EXPECT_TRUE(r.err.empty());
}

TEST(Process, CapturesBothStreams)
{
    ProcessResult r;
    ASSERT_TRUE(run_ok({"/bin/sh", "-c", "echo out; echo err >&2"}, r));
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.out, "out\n");
    EXPECT_EQ(r.err, "err\n");
}

TEST(Process, NonZeroExitCode)
{
    ProcessResult r;
    ASSERT_TRUE(run_ok({"/bin/false"}, r));
    EXPECT_EQ(r.exit_code, 1);
}

TEST(Process, ExecFailureExits127)
{
    ProcessResult r;
    ASSERT_TRUE(run_ok({"/nonexistent/msdb_test_bin"}, r));
    EXPECT_EQ(r.exit_code, 127);
    EXPECT_FALSE(r.timed_out);
}

TEST(Process, TimeoutKills)
{
    auto start = std::chrono::steady_clock::now();
    ProcessResult r;
    std::string error;
    ASSERT_TRUE(tcommon::run_process({"/bin/sleep", "30"}, 300, r, error)) << error;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    EXPECT_TRUE(r.timed_out);
    EXPECT_LT(elapsed_ms.count(), 10000);
}

TEST(Process, BigOutputNoDeadlock)
{
    // 20000 行 × 11B ≈ 220KB, 远超管道缓冲 64KB
    std::string script = "i=0; while [ $i -lt 20000 ]; do echo 0123456789; i=$((i+1)); done";
    ProcessResult r;
    std::string error;
    ASSERT_TRUE(tcommon::run_process({"/bin/sh", "-c", script}, 30000, r, error)) << error;
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_FALSE(r.timed_out);
    EXPECT_GT(r.out.size(), static_cast<size_t>(20000 * 11 - 100));
}

TEST(Process, EnvSanitized)
{
    ProcessResult r;
    ASSERT_TRUE(run_ok({"/usr/bin/env"}, r));
    EXPECT_NE(r.out.find("LC_ALL=C"), std::string::npos);
    EXPECT_EQ(r.out.find("HOME="), std::string::npos);
}

TEST(NetProbe, EphemeralListener)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(lfd, 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(bind(lfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(lfd, 1), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(getsockname(lfd, reinterpret_cast<struct sockaddr*>(&addr), &len), 0);
    int port = ntohs(addr.sin_port);

    EXPECT_TRUE(tcommon::port_is_open("127.0.0.1", port));
    std::string error;
    EXPECT_TRUE(tcommon::wait_port_ready("127.0.0.1", port, 1000, error));

    close(lfd);
    EXPECT_FALSE(tcommon::port_is_open("127.0.0.1", port));
    error.clear();
    EXPECT_FALSE(tcommon::wait_port_ready("127.0.0.1", port, 300, error));
    EXPECT_NE(error.find(std::to_string(port)), std::string::npos);
}
