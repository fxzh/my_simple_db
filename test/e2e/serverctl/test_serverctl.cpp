// test_serverctl.cpp: serverctl L1 e2e, Start/Status/DoubleStart/Stop 构成 SRV_UP 门控链,
// 未运行类用例独立执行
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/net_probe.hpp"
#include "common/process.hpp"
#include "common/temp_dir.hpp"
#include "e2e/test_config.hpp"

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

std::string state_data_dir()
{
    return (std::filesystem::path(kStateDir) / "data").string();
}

bool ctl(const std::string& data_dir, const char* sub, int timeout_ms, ProcessResult& r)
{
    return run_ok({bin("serverctl"), "-D", data_dir, sub}, timeout_ms, r);
}

}  // namespace

TEST(Ctl, Start)
{
    std::string data = state_data_dir();
    ASSERT_TRUE(std::filesystem::is_regular_file(std::filesystem::path(data) / "db.conf"))
        << "DB_READY 前置缺失";
    // 配置改写为测试专用端口, 由本用例接管
    {
        std::ofstream out(std::filesystem::path(data) / "db.conf");
        out << "port = " << kTestPort << "\n";
    }
    if (tcommon::port_is_open("127.0.0.1", kTestPort)) {
        std::filesystem::path pidfile = std::filesystem::path(data) / "server.pid";
        std::string holder = "(无 pidfile)";
        if (std::filesystem::exists(pidfile)) {
            std::ifstream in(pidfile);
            std::getline(in, holder);
        }
        FAIL() << "测试端口 " << kTestPort << " 被占用, 数据目录 pidfile 记录: " << holder;
    }

    ProcessResult r;
    ASSERT_TRUE(ctl(data, "start", 15000, r));
    EXPECT_EQ(r.exit_code, 0) << r.err;
    EXPECT_NE(r.out.find("已启动"), std::string::npos) << r.err;
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(data) / "server.sock"));
    std::string error;
    EXPECT_TRUE(tcommon::wait_port_ready("127.0.0.1", kTestPort, 5000, error)) << error;
}

TEST(Ctl, StatusRunning)
{
    // status 为占位实现: 固定回复且退出码 2, 富状态字段实现后同步更新
    ProcessResult r;
    ASSERT_TRUE(ctl(state_data_dir(), "status", 5000, r));
    EXPECT_EQ(r.exit_code, 2);
    EXPECT_EQ(r.out, "ERROR: status 暂不支持\n");
}

TEST(Ctl, DoubleStart)
{
    ProcessResult r;
    ASSERT_TRUE(ctl(state_data_dir(), "start", 15000, r));
    EXPECT_EQ(r.exit_code, 2);
    EXPECT_NE(r.err.find("服务器已在运行"), std::string::npos);
}

TEST(Ctl, Stop)
{
    // cleanup 用例: 幂等容忍服务未启动与数据目录未初始化
    ProcessResult r;
    ASSERT_TRUE(ctl(state_data_dir(), "stop", 15000, r));
    if (r.exit_code == 0) {
        EXPECT_NE(r.out.find("已停止"), std::string::npos) << r.err;
        std::filesystem::path sock = std::filesystem::path(state_data_dir()) / "server.sock";
        EXPECT_FALSE(std::filesystem::exists(sock));
        EXPECT_FALSE(tcommon::port_is_open("127.0.0.1", kTestPort));
        return;
    }
    if (r.exit_code == 1) {
        EXPECT_NE(r.err.find("服务器未运行"), std::string::npos);
    } else {
        EXPECT_EQ(r.exit_code, 2);
        EXPECT_NE(r.err.find("读取配置失败"), std::string::npos);
    }
}

TEST(Ctl, StopNotRunning)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_ctl", error)) << error;
    std::string data = (std::filesystem::path(dir.path) / "data").string();
    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 5000, r));
    EXPECT_EQ(r.exit_code, 0);

    ASSERT_TRUE(ctl(data, "stop", 5000, r));
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(r.err.find("服务器未运行"), std::string::npos);
}

TEST(Ctl, StatusNotRunning)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_ctl", error)) << error;
    std::string data = (std::filesystem::path(dir.path) / "data").string();
    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 5000, r));
    EXPECT_EQ(r.exit_code, 0);

    ASSERT_TRUE(ctl(data, "status", 5000, r));
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(r.err.find("服务器未运行"), std::string::npos);
}
