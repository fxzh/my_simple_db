// test_storage.cpp: 存储引擎冒烟测试(页删除整理/增删扫/重开持久化/WAL 崩溃恢复)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "common/err.h"
#include "common/temp_dir.hpp"
#include "config/config.h"
#include "log/log.h"
#include "catalog.h"
#include "page.h"
#include "wal.h"

using namespace st;
using namespace ct;

TEST(HeapPage, DeleteCompact)
{
    char page[PAGE_SIZE];
    init_page(page, MAGIC_HEAP, PageType::Heap);
    const uint8_t r0[] = {0x01, 0x02, 0x03};
    const uint8_t r1[] = {0x04, 0x05};
    const uint8_t r2[] = {0x06, 0x07, 0x08, 0x09};
    uint16_t s0 = 0, s1 = 0, s2 = 0;
    EXPECT_TRUE(heap_append(page, r0, sizeof(r0), &s0));
    EXPECT_TRUE(heap_append(page, r1, sizeof(r1), &s1));
    EXPECT_TRUE(heap_append(page, r2, sizeof(r2), &s2));
    EXPECT_TRUE(s0 == 0 && s1 == 1 && s2 == 2);

    // 删除中间行制造洞
    heap_delete(page, 1);
    EXPECT_EQ(header(page)->slot_count, 3);
    EXPECT_TRUE(slot_tombstone(page, 1));
    EXPECT_TRUE(!slot_tombstone(page, 0) && !slot_tombstone(page, 2));

    // 删除尾行触发尾槽收缩
    heap_delete(page, 2);
    EXPECT_EQ(header(page)->slot_count, 1);
    EXPECT_FALSE(slot_tombstone(page, 0));
    EXPECT_TRUE(page_valid(page, MAGIC_HEAP));

    // 页内整理: 压实记录并回收洞
    heap_compact(page);
    EXPECT_EQ(header(page)->slot_count, 1);
    EXPECT_EQ(std::memcmp(record(page, 0), r0, sizeof(r0)), 0);
    EXPECT_EQ(free_space(page), static_cast<uint16_t>(PAGE_SIZE - PAGE_HEADER_SIZE
                                                      - sizeof(r0) - SLOT_SIZE));
    EXPECT_TRUE(page_valid(page, MAGIC_HEAP));

    // 中间墓碑: compact 把后续记录前移重建槽
    char pg2[PAGE_SIZE];
    init_page(pg2, MAGIC_HEAP, PageType::Heap);
    EXPECT_TRUE(heap_append(pg2, r0, sizeof(r0), nullptr));
    EXPECT_TRUE(heap_append(pg2, r1, sizeof(r1), nullptr));
    EXPECT_TRUE(heap_append(pg2, r2, sizeof(r2), nullptr));
    heap_delete(pg2, 1);
    heap_compact(pg2);
    EXPECT_EQ(header(pg2)->slot_count, 2);
    EXPECT_EQ(std::memcmp(record(pg2, 0), r0, sizeof(r0)), 0);
    EXPECT_EQ(std::memcmp(record(pg2, 1), r2, sizeof(r2)), 0);
    EXPECT_TRUE(page_valid(pg2, MAGIC_HEAP));
}

// 独立临时目录作数据目录
struct StorageDb : ::testing::Test {
    void SetUp() override
    {
        std::string error;
        ASSERT_TRUE(dir.create("msdb_storage_smoke", error)) << error;
        // 日志文件随数据目录, 先建目录再初始化日志路径
        Logger::initPath(dir.path + "/simple.log");
    }

    tcommon::TempDir dir;
};

// 建表/插入/删除/删表全链路, 各阶段经 close+open 验证持久化
TEST_F(StorageDb, ReopenLifecycle)
{
    {
        // bootstrap 模式建 db_version 完成标记(模拟 bootstrap.sql 结尾), 之后各阶段走正常模式
        Catalog db(dir.path, true);
        db.create();
        db.open();
        db.set_bootstrap_table_id(5);
        db.begin_txn();
        db.create_table(kVersionMetaName, {{"version", ColType::BigInt, 0, true}});
        db.commit_txn();
        db.close();
    }
    {
        Catalog db(dir.path);
        db.open();

        std::vector<ColumnSpec> cols = {
                {"id", ColType::Int, 0},
                {"name", ColType::VarChar, 16},
        };
        db.begin_txn();
        db.create_table("t", cols);

        db.insert("t", {Value{int64_t{1}}, Value{std::string{"alice"}}});
        db.insert("t", {Value{int64_t{2}}, Value{std::string{"bob"}}});
        // 跨页: 一条记录塞满首个数据页后触发扩展
        for (int i = 0; i < 400; ++i) {
            db.insert("t", {Value{int64_t{3}}, Value{std::string{"spam"}}});
        }
        db.commit_txn();
        EXPECT_EQ(db.row_count("t"), size_t{402});

        // float/char 列编解码往返
        db.begin_txn();
        db.create_table("t2", {
                {"score", ColType::Float, 0},
                {"grade", ColType::Char, 3},
        });
        db.insert("t2", {Value{0.5}, Value{std::string{"A"}}});
        db.insert("t2", {Value{-1.25}, Value{std::string{"XYZ"}}});
        db.commit_txn();
        db.close();
    }

    {
        Catalog db(dir.path);
        db.open();
        EXPECT_EQ(db.row_count("t"), size_t{402});

        size_t n = 0;
        {
            auto s = db.scan("t");
            Row r;
            while (s->next(&r)) {
                ++n;
                if (n == 1) {
                    EXPECT_EQ(std::get<int64_t>(r.values[0]), 1);
                    EXPECT_EQ(std::get<std::string>(r.values[1]), "alice");
                }
                if (n == 2) {
                    EXPECT_EQ(std::get<int64_t>(r.values[0]), 2);
                    EXPECT_EQ(std::get<std::string>(r.values[1]), "bob");
                }
            }
        }
        EXPECT_EQ(n, size_t{402});

        size_t n2 = 0;
        {
            auto s = db.scan("t2");
            Row r;
            while (s->next(&r)) {
                ++n2;
                if (n2 == 1) {
                    EXPECT_EQ(std::get<double>(r.values[0]), 0.5);
                    EXPECT_EQ(std::get<std::string>(r.values[1]), "A");
                }
                if (n2 == 2) {
                    EXPECT_EQ(std::get<double>(r.values[0]), -1.25);
                    EXPECT_EQ(std::get<std::string>(r.values[1]), "XYZ");
                }
            }
        }
        EXPECT_EQ(n2, size_t{2});

        // 删除单行: 首行删除后计数与扫描均不可见
        {
            auto s = db.scan("t");
            Row first;
            EXPECT_TRUE(s->next(&first));
            db.begin_txn();
            EXPECT_EQ(db.delete_by_ref(first.ref), 1);
            EXPECT_EQ(db.row_count("t"), size_t{401});
            EXPECT_EQ(db.delete_by_ref(first.ref), 0);
            EXPECT_THROW(db.delete_by_ref(RowRef{}), std::runtime_error);
            db.commit_txn();
            bool seen = false;
            {
                auto s2 = db.scan("t");
                Row r;
                while (s2->next(&r)) {
                    seen = seen || std::get<int64_t>(r.values[0]) == 1;
                }
            }
            EXPECT_FALSE(seen);
        }
        // 清空表: 全部行删除后计数与扫描为空
        db.begin_txn();
        db.delete_all("t2");
        db.commit_txn();
        EXPECT_EQ(db.row_count("t2"), size_t{0});
        {
            auto s = db.scan("t2");
            Row r;
            EXPECT_FALSE(s->next(&r));
        }

        // drop 后悬空引用删除报错, 目录不可见
        Row victim;
        {
            auto s = db.scan("t");
            EXPECT_TRUE(s->next(&victim));
        }
        db.begin_txn();
        db.drop_table("t");
        db.commit_txn();
        EXPECT_THROW(db.delete_by_ref(victim.ref), std::runtime_error);
        EXPECT_THROW(db.row_count("t"), std::runtime_error);
        db.close();
    }

    {
        Catalog db(dir.path);
        db.open();
        EXPECT_THROW(db.row_count("t"), std::runtime_error);
        EXPECT_EQ(db.row_count("t2"), size_t{0});
        db.close();
    }
}

// 目录未初始化时 open 报 CatalogMissing
TEST_F(StorageDb, OpenWithoutCreateFails)
{
    Catalog db(dir.path);
    try {
        db.open();
        FAIL() << "未初始化目录 open 应报错";
    } catch (const db::DbError& e) {
        EXPECT_EQ(e.code(), db::ErrCode::CatalogMissing);
    }
}

// 自举表齐备但缺 db_version 完成标记时正常模式 open 报 CatalogMissing
TEST_F(StorageDb, OpenWithoutVersionMarkerFails)
{
    {
        Catalog db(dir.path, true);
        db.create();
    }
    Catalog db(dir.path);
    try {
        db.open();
        FAIL() << "缺完成标记的目录 open 应报错";
    } catch (const db::DbError& e) {
        EXPECT_EQ(e.code(), db::ErrCode::CatalogMissing);
        EXPECT_NE(std::string(e.what()).find("初始化未完成"), std::string::npos);
    }
}

// 已初始化目录重复 create 报 CatalogExists
TEST_F(StorageDb, CreateTwiceFails)
{
    {
        Catalog db(dir.path);
        db.create();
    }
    Catalog db(dir.path);
    try {
        db.create();
        FAIL() << "重复 create 应报错";
    } catch (const db::DbError& e) {
        EXPECT_EQ(e.code(), db::ErrCode::CatalogExists);
    }
}

// ==================== WAL ====================

// 引导数据目录(元数据表 + db_version 完成标记)后干净关闭, 供 WAL 测试起步
static void bootstrap_version_marker(const std::string& path)
{
    Catalog db(path, true);
    db.create();
    db.open();
    db.set_bootstrap_table_id(5);
    db.begin_txn();
    db.create_table(kVersionMetaName, {{"version", ColType::BigInt, 0, true}});
    db.commit_txn();
    db.close();
}

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

// WAL 记录往返: append 的记录经 WalReader 读回字段一致; 尾部半条记录按截断处理
TEST(WalLog, RecordRoundTripAndTailTruncation)
{
    tcommon::TempDir dir;
    std::string error;
    ASSERT_TRUE(dir.create("msdb_storage_wal", error)) << error;
    const std::string path = dir.path + "/wal.log";

    {
        Wal wal(path);
        char drop_payload[8];
        wal_put_u64(drop_payload, 42);
        EXPECT_EQ(wal.append(WalOp::DropFile, 1, drop_payload, sizeof(drop_payload)), uint64_t{1});

        // 补丁载荷: 定位头 + after 半段 + before 半段
        char patch[WAL_PATCH_HEADER_SIZE + 16] = {0};
        wal_put_u64(patch, 7);
        wal_put_u32(patch + 8, 3);
        wal_put_u32(patch + 12, 24);
        wal_put_u32(patch + 16, 8);
        std::memcpy(patch + WAL_PATCH_HEADER_SIZE, "abcdefgh", 8);
        std::memcpy(patch + WAL_PATCH_HEADER_SIZE + 8, "ABCDEFGH", 8);
        EXPECT_EQ(wal.append(WalOp::PagePatch, 2, patch, sizeof(patch)), uint64_t{2});
        EXPECT_EQ(wal.append(WalOp::Commit, 2, nullptr, 0), uint64_t{3});
    }

    {
        WalReader reader(path);
        WalRecord rec;
        ASSERT_TRUE(reader.next(&rec));
        EXPECT_EQ(rec.lsn, uint64_t{1});
        EXPECT_EQ(rec.txn, uint64_t{1});
        EXPECT_EQ(rec.op, WalOp::DropFile);
        ASSERT_EQ(rec.payload.size(), size_t{8});
        EXPECT_EQ(wal_get_u64(rec.payload.data()), uint64_t{42});
        ASSERT_TRUE(reader.next(&rec));
        EXPECT_EQ(rec.lsn, uint64_t{2});
        EXPECT_EQ(rec.txn, uint64_t{2});
        EXPECT_EQ(rec.op, WalOp::PagePatch);
        ASSERT_EQ(rec.payload.size(), size_t{WAL_PATCH_HEADER_SIZE + 16});
        EXPECT_EQ(wal_get_u32(rec.payload.data() + 12), uint32_t{24});
        EXPECT_EQ(std::memcmp(rec.payload.data() + WAL_PATCH_HEADER_SIZE, "abcdefgh", 8), 0);
        EXPECT_EQ(std::memcmp(rec.payload.data() + WAL_PATCH_HEADER_SIZE + 8, "ABCDEFGH", 8), 0);
        ASSERT_TRUE(reader.next(&rec));
        EXPECT_EQ(rec.lsn, uint64_t{3});
        EXPECT_EQ(rec.txn, uint64_t{2});
        EXPECT_EQ(rec.op, WalOp::Commit);
        EXPECT_TRUE(rec.payload.empty());
        EXPECT_FALSE(reader.next(&rec));
        EXPECT_EQ(reader.valid_bytes(),
                  uint64_t{(WAL_HEADER_SIZE + 8) + (WAL_HEADER_SIZE + WAL_PATCH_HEADER_SIZE + 16)
                           + WAL_HEADER_SIZE});
    }

    // 尾部追加半条垃圾(模拟崩溃时记录只写了一部分): 合法记录照常读出, 垃圾后停
    {
        std::string junk(10, 'x');
        FILE* f = std::fopen(path.c_str(), "ab");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fwrite(junk.data(), 1, junk.size(), f), junk.size());
        std::fclose(f);

        WalReader reader(path);
        WalRecord rec;
        EXPECT_TRUE(reader.next(&rec));
        EXPECT_TRUE(reader.next(&rec));
        EXPECT_TRUE(reader.next(&rec));
        EXPECT_FALSE(reader.next(&rec));
        EXPECT_EQ(reader.valid_bytes(),
                  uint64_t{(WAL_HEADER_SIZE + 8) + (WAL_HEADER_SIZE + WAL_PATCH_HEADER_SIZE + 16)
                           + WAL_HEADER_SIZE});
    }
}

// 崩溃后已提交数据幸存: 子进程插行并提交后 _exit 模拟崩溃, 父进程重开经
// WAL 重放恢复全部数据; 干净关闭后 WAL 被检查点清空
TEST_F(StorageDb, WalRecoverInsertAfterCrash)
{
    bootstrap_version_marker(dir.path);

    run_crashed([](Catalog& db) {
        db.begin_txn();
        db.create_table("t", {{"id", ColType::Int, 0, true}});
        for (int i = 1; i <= 50; ++i) {
            db.insert("t", {Value{int64_t{i}}});
        }
        db.commit_txn();
    }, dir.path);

    {
        Catalog db(dir.path);
        db.open();
        EXPECT_EQ(db.row_count("t"), size_t{50});
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
        db.create_table("t", {{"id", ColType::Int, 0, true}});
        db.insert("t", {Value{int64_t{1}}});
        db.commit_txn();
    }, dir.path);

    run_crashed([](Catalog& db) {
        db.begin_txn();
        db.drop_table("t");
        db.commit_txn();
    }, dir.path);

    {
        Catalog db(dir.path);
        db.open();
        EXPECT_THROW(db.row_count("t"), std::runtime_error);
        db.close();
    }
}

// ==================== 事务 ====================

// catalog 事务门面: 回滚后修改消失, 提交后修改可见且重开仍在, 事务内自见,
// 事务外调落盘性门面报错
TEST_F(StorageDb, CatalogTxnCommitRollback)
{
    bootstrap_version_marker(dir.path);
    {
        Catalog db(dir.path);
        db.open();

        // 回滚: 建表与插行随事务消失
        db.begin_txn();
        db.create_table("t", {{"id", ColType::Int, 0, true}});
        db.insert("t", {Value{int64_t{1}}});
        // 事务内自见: 未提交修改对本事务可见
        EXPECT_EQ(db.row_count("t"), size_t{1});
        db.rollback_txn();
        EXPECT_THROW(db.row_count("t"), std::runtime_error);

        // 提交: 修改可见
        db.begin_txn();
        db.create_table("t", {{"id", ColType::Int, 0, true}});
        db.insert("t", {Value{int64_t{1}}});
        db.commit_txn();
        EXPECT_EQ(db.row_count("t"), size_t{1});

        // 事务外调落盘性门面报错, 表内容不变
        EXPECT_THROW(db.insert("t", {Value{int64_t{2}}}), std::runtime_error);
        EXPECT_EQ(db.row_count("t"), size_t{1});
        db.close();
    }
    {
        Catalog db(dir.path);
        db.open();
        EXPECT_EQ(db.row_count("t"), size_t{1});
        db.close();
    }
}

// 事务回滚后内容复原: 含同页多次修改的 before 链, 复原后引擎可继续工作, 重开仍在
TEST_F(StorageDb, TxnRollbackRestoresContent)
{
    const std::vector<ColumnSpec> cols = {{"id", ColType::Int, 0, true}};
    constexpr uint64_t fid = 100;
    {
        Engine engine(dir.path);
        engine.open();
        engine.begin_txn();
        engine.init_table_file(fid);
        RowRef ref;
        for (int i = 1; i <= 20; ++i) {
            engine.insert_row(fid, cols, {Value{int64_t{i}}}, &ref);
        }
        engine.commit_txn();

        // 未提交事务: 插入/删除/再插入构成同页 before 链, 回滚后全部消失
        engine.begin_txn();
        engine.insert_row(fid, cols, {Value{int64_t{999}}}, &ref);
        EXPECT_EQ(engine.delete_row(ref), size_t{1});
        engine.insert_row(fid, cols, {Value{int64_t{888}}}, &ref);
        engine.rollback_txn();
        EXPECT_EQ(engine.row_count(fid), size_t{20});

        // 回滚后引擎可继续提交事务
        engine.begin_txn();
        engine.insert_row(fid, cols, {Value{int64_t{777}}}, &ref);
        engine.commit_txn();
        EXPECT_EQ(engine.row_count(fid), size_t{21});
        engine.close();
    }
    {
        Engine engine(dir.path);
        engine.open();
        EXPECT_EQ(engine.row_count(fid), size_t{21});
        engine.close();
    }
}

// 落盘性原语在事务外调用报错
TEST_F(StorageDb, EnginePrimitiveWithoutTxnFails)
{
    Engine engine(dir.path);
    engine.open();
    RowRef ref;
    EXPECT_THROW(engine.init_table_file(1), std::runtime_error);
    EXPECT_THROW(engine.insert_row(2, {{"id", ColType::Int, 0, true}},
                                   {Value{int64_t{1}}}, &ref),
                 std::runtime_error);
    engine.close();
}

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

// delete_all 逐行墓碑实现的可回滚性: 回滚后行全部复原; 提交后为空且文件仍在
TEST_F(StorageDb, TxnDeleteAllRollbackRestores)
{
    const std::vector<ColumnSpec> cols = {{"id", ColType::Int, 0, true}};
    constexpr uint64_t fid = 230;
    {
        Engine engine(dir.path);
        engine.open();
        RowRef ref;
        engine.begin_txn();
        engine.init_table_file(fid);
        for (int i = 1; i <= 15; ++i) {
            engine.insert_row(fid, cols, {Value{int64_t{i}}}, &ref);
        }
        engine.commit_txn();

        engine.begin_txn();
        EXPECT_EQ(engine.delete_all_rows(fid), size_t{15});
        EXPECT_EQ(engine.row_count(fid), size_t{0});
        engine.rollback_txn();
        EXPECT_EQ(engine.row_count(fid), size_t{15});
        engine.close();
    }
    {
        Engine engine(dir.path);
        engine.open();
        engine.begin_txn();
        EXPECT_EQ(engine.delete_all_rows(fid), size_t{15});
        engine.commit_txn();
        EXPECT_EQ(engine.row_count(fid), size_t{0});
        EXPECT_TRUE(engine.table_file_exists(fid));
        engine.close();
    }
}
