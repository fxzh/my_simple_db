// test_disconnect.cpp: 断连隐式回滚 e2e
// 第一个 client 以 -f 执行止于 BEGIN+INSERT 不提交的脚本, 脚本结束关闭连接触发
// 会话清理回滚; 第二个 client 验证未提交数据不残留并清理表(未提交事务持全局锁,
// 第二次连接的语句阻塞至回滚释放, 时序上无竞争)
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

// -f 脚本止于未提交事务: 断连回滚后新会话查为空
TEST(TxnDisconnect, RollbackOnDisconnect)
{
    tcommon::TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_txn_disc", error)) << error;
    const std::string path = (std::filesystem::path(dir.path) / "txn_disc.sql").string();
    {
        std::ofstream out(path);
        out << "CREATE TABLE t_disc (id int);\n"
               "BEGIN;\n"
               "INSERT INTO t_disc VALUES (1);\n";
    }

    // 第一段: 建表 + 未提交插入, 脚本结束断连
    tcommon::ProcessResult r1;
    ASSERT_TRUE(run_client({"-a", "-h", "127.0.0.1", "-p", std::to_string(kTestPort),
                            "-f", path},
                           r1));
    EXPECT_EQ(r1.exit_code, 0);
    EXPECT_EQ(r1.out,
              "CREATE TABLE t_disc (id int);\n"
              "CREATE\n"
              "BEGIN;\n"
              "BEGIN\n"
              "INSERT INTO t_disc VALUES (1);\n"
              "INSERT 1\n");

    // 第二段: 新会话验证插入已回滚并清理表(-c 整行回显一次, 空结果集无分隔行)
    tcommon::ProcessResult r2;
    ASSERT_TRUE(run_client({"-a", "-h", "127.0.0.1", "-p", std::to_string(kTestPort),
                            "-c", "SELECT * FROM t_disc; DROP TABLE t_disc;"},
                           r2));
    EXPECT_EQ(r2.exit_code, 0);
    EXPECT_EQ(r2.out,
              "SELECT * FROM t_disc; DROP TABLE t_disc;\n"
              " id\n"
              "(0 行)\n"
              "DROP\n");
}
