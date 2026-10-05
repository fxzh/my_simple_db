// test_storage_crash.cpp: 崩溃恢复冒烟测试(fork 子进程 _exit 模拟 kill -9, 验证
// WAL 重放幸存与未提交残留撤销)
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "config/config.h"
#include "storage/storage_fixture.hpp"
#include "catalog.h"

using namespace st;
using namespace ct;

// fork 出子进程执行 fn 后 _exit(0): 析构全部跳过, 等价于进程在缓冲池还持有
// 脏页的任意时刻被 kill -9(进程内存全丢, 已 write/fsync 的文件内容幸存)。
// 子进程内只用成功路径(不触发 LOG, 避免与 fork 只保留调用线程的日志写线程交互)
static void run_crashed(const std::function<void(Catalog&)>& fn, const std::string& path)
{
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        try {
            Catalog db(path);
            db.open();
            fn(db);
            _exit(0);
        } catch (...) {
            _exit(1);
        }
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << "子进程应正常 _exit";
    ASSERT_EQ(WEXITSTATUS(status), 0) << "崩溃前工作应全部成功";
}

// fork 出子进程直接以 Engine 原语执行 fn 后 _exit(0): 跳过析构(不做检查点),
// 等价于进程在事务进行中被 kill -9(进程内存全丢, 已 write/fsync 的文件内容幸存)。
// 子进程内只用成功路径(不触发 LOG, 避免与 fork 只保留调用线程的日志写线程交互)
static void run_crashed_engine(const std::function<void(Engine&)>& fn, const std::string& path)
{
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        try {
            Engine engine(path);
            engine.open();
            fn(engine);
            _exit(0);
        } catch (...) {
            _exit(1);
        }
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << "子进程应正常 _exit";
    ASSERT_EQ(WEXITSTATUS(status), 0) << "崩溃前工作应全部成功";
}

// ==================== WAL 重放 ====================

// 崩溃后已提交数据幸存: 子进程插行并提交后 _exit 模拟崩溃, 父进程重开经
// WAL 重放恢复全部数据; 干净关闭后 WAL 被检查点清空
TEST_F(StorageDb, WalRecoverInsertAfterCrash)
{
    bootstrap_version_marker(dir.path);

    run_crashed([](Catalog& db) {
        db.begin_txn();
        db.create_table({"", "t"},{{"id", ColType::Int, 0, true}});
        for (int i = 1; i <= 50; ++i) {
            db.insert({"", "t"},{Value{int64_t{i}}});
        }
        db.commit_txn();
    }, dir.path);

    {
        Catalog db(dir.path);
        db.open();
        EXPECT_EQ(db.row_count({"", "t"}), size_t{50});
        db.close();
        // 干净关闭做了检查点: WAL 应已清空, 下次启动零重放
        std::error_code ec;
        EXPECT_EQ(std::filesystem::file_size(dir.path + "/wal.log", ec), uintmax_t{0});
    }
}

// 崩溃后 DDL 幸存: 子进程 drop 表并提交后崩溃, 父进程重开后表经重放消失
TEST_F(StorageDb, WalRecoverDropAfterCrash)
{
    bootstrap_version_marker(dir.path);

    run_crashed([](Catalog& db) {
        db.begin_txn();
        db.create_table({"", "t"},{{"id", ColType::Int, 0, true}});
        db.insert({"", "t"},{Value{int64_t{1}}});
        db.commit_txn();
    }, dir.path);

    run_crashed([](Catalog& db) {
        db.begin_txn();
        db.drop_table({"", "t"});
        db.commit_txn();
    }, dir.path);

    {
        Catalog db(dir.path);
        db.open();
        EXPECT_THROW(db.row_count({"", "t"}), std::runtime_error);
        db.close();
    }
}

// ==================== 事务撤销 ====================

// 崩溃后已提交事务幸存、崩溃中止事务的磁盘残留(steal)被撤销:
// 压小缓冲池帧数迫使未提交脏页淘汰落盘
TEST_F(StorageDb, TxnCrashCommittedSurvivesUncommittedUndone)
{
    const std::vector<ColumnSpec> cols = {{"id", ColType::Int, 0, true}};
    constexpr uint64_t fid = 200;

    const size_t frames_saved = config::cfg.buffer_pool_frames;
    config::cfg.buffer_pool_frames = 4;
    run_crashed_engine([&](Engine& engine) {
        // 事务一(提交): 建表并插入
        engine.begin_txn();
        engine.init_table_file(fid);
        RowRef ref;
        for (int i = 1; i <= 30; ++i) {
            engine.insert_row(fid, cols, {Value{int64_t{i}}}, &ref);
        }
        engine.commit_txn();
        // 事务二(崩溃中止): 同表继续插入, 脏页随缓冲池淘汰落盘形成 steal 残留
        engine.begin_txn();
        for (int i = 31; i <= 1500; ++i) {
            engine.insert_row(fid, cols, {Value{int64_t{i}}}, &ref);
        }
    }, dir.path);
    config::cfg.buffer_pool_frames = frames_saved;

    {
        Engine engine(dir.path);
        engine.open();
        EXPECT_EQ(engine.row_count(fid), size_t{30});
        engine.close();
    }
}

// 运行期已回滚(Abort)事务不得干扰后续已提交事务: 恢复撤销遍跳过其记录,
// 后续已提交插入的字节不被回滚事务的 before 覆盖
TEST_F(StorageDb, TxnAbortedDoesNotClobberLaterCommitted)
{
    const std::vector<ColumnSpec> cols = {{"id", ColType::Int, 0, true}};
    constexpr uint64_t fid = 210;
    run_crashed_engine([&](Engine& engine) {
        RowRef ref;
        engine.begin_txn();
        engine.init_table_file(fid);
        for (int i = 1; i <= 10; ++i) {
            engine.insert_row(fid, cols, {Value{int64_t{i}}}, &ref);
        }
        engine.commit_txn();
        // 事务二: 插入后回滚, Abort 记录入日志(磁盘已复原)
        engine.begin_txn();
        engine.insert_row(fid, cols, {Value{int64_t{999}}}, &ref);
        engine.rollback_txn();
        // 事务三: 同页插入并提交, Commit 的 fsync 一并固化前面的 Abort 记录
        engine.begin_txn();
        engine.insert_row(fid, cols, {Value{int64_t{777}}}, &ref);
        engine.commit_txn();
    }, dir.path);

    {
        Engine engine(dir.path);
        engine.open();
        const auto rows = engine.read_rows(fid, cols);
        EXPECT_EQ(rows.size(), size_t{11});
        size_t seen_777 = 0;
        size_t seen_999 = 0;
        for (const std::vector<Value>& row : rows) {
            const int64_t v = std::get<int64_t>(row[0]);
            seen_777 += (v == 777);
            seen_999 += (v == 999);
        }
        EXPECT_EQ(seen_777, size_t{1});
        EXPECT_EQ(seen_999, size_t{0});
        engine.close();
    }
}

// 未提交的删文件事务崩溃后文件与内容复原(延迟 unlink + 未提交 DropFile 跳过)
TEST_F(StorageDb, TxnUncommittedDropKeepsFile)
{
    const std::vector<ColumnSpec> cols = {{"id", ColType::Int, 0, true}};
    constexpr uint64_t fid = 220;
    run_crashed_engine([&](Engine& engine) {
        RowRef ref;
        engine.begin_txn();
        engine.init_table_file(fid);
        for (int i = 1; i <= 10; ++i) {
            engine.insert_row(fid, cols, {Value{int64_t{i}}}, &ref);
        }
        engine.commit_txn();
        engine.begin_txn();
        engine.remove_table_file(fid);
    }, dir.path);

    {
        Engine engine(dir.path);
        engine.open();
        EXPECT_TRUE(engine.table_file_exists(fid));
        EXPECT_EQ(engine.row_count(fid), size_t{10});
        engine.close();
    }
}
