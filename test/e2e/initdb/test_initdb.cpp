// test_initdb.cpp: initdb L1 e2e, Ok 提供 DB_READY fixture, 其余用例独立执行
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "common/process.hpp"
#include "common/temp_dir.hpp"
#include "storage/storage_fixture.hpp"

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

// 元数据行值序列化: 三张元数据表仅含 int/string 值, 其余类型当场判负
std::string format_meta_value(const st::Value& v)
{
    if (const std::string* s = std::get_if<std::string>(&v)) {
        return *s;
    }
    if (const int64_t* i = std::get_if<int64_t>(&v)) {
        return std::to_string(*i);
    }
    ADD_FAILURE() << "元数据行出现非 int/string 值";
    return "";
}

// 采集目录三张元数据表全部行作可比对文本, 行序即堆扫描序
std::string dump_meta_rows(const std::string& dir)
{
    ct::Catalog db(dir);
    db.open();
    std::string dump;
    for (const char* table : {ct::kTableMetaName, ct::kColumnMetaName, ct::kSchemaMetaName}) {
        dump += table;
        dump += '\n';
        std::unique_ptr<st::Scanner> cursor = db.scan({"", table});
        st::Row row;
        while (cursor->next(&row)) {
            for (const st::Value& v : row.values) {
                dump += format_meta_value(v);
                dump += '|';
            }
            dump += '\n';
        }
    }
    db.close();
    return dump;
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
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 10000, r));
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(r.out.find("已初始化数据目录"), std::string::npos) << r.err;
    std::filesystem::path conf = std::filesystem::path(data) / "db.conf";
    EXPECT_TRUE(std::filesystem::is_regular_file(conf));
    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::path(data) / "t_1.dat"));
    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::path(data) / "t_2.dat"));
    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::path(data) / "t_3.dat"));
    // bootstrap.sql 经 client 执行后新增 db_index(t_4) 与 db_version(t_5)
    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::path(data) / "t_4.dat"));
    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::path(data) / "t_5.dat"));
    std::ifstream in(conf);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("port = 8123"), std::string::npos);

    // 漂移守卫: bootstrap_version_marker 引导目录与 initdb 产物的元数据行集一致
    TempDir fixture_dir;
    std::string error;
    ASSERT_TRUE(fixture_dir.create("msdb_initdb_drift", error)) << error;
    Logger::initPath(fixture_dir.path + "/simple.log");
    bootstrap_version_marker(fixture_dir.path);
    EXPECT_EQ(dump_meta_rows(fixture_dir.path), dump_meta_rows(data));
}

TEST(Initdb, DirExistsEmpty)
{
    TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_initdb", error)) << error;
    std::string data = (std::filesystem::path(dir.path) / "data").string();
    std::filesystem::create_directories(data);

    ProcessResult r;
    ASSERT_TRUE(run_ok({bin("initdb"), "-D", data}, 10000, r));
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
