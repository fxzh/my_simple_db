// test_storage.cpp: 存储引擎冒烟测试(页删除整理/增删扫/重开持久化/WAL 记录往返)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/err.h"
#include "common/temp_dir.hpp"
#include "storage/storage_fixture.hpp"
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
