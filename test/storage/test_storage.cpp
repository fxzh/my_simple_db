// test_storage.cpp: 存储引擎 M1 冒烟测试(页删除整理/增删扫/重开持久化)
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/err.h"
#include "common/temp_dir.hpp"
#include "log/log.h"
#include "page.h"
#include "storage.h"

using namespace st;

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
        Database db(dir.path);
        db.create();
        db.open();

        std::vector<ColumnSpec> cols = {
                {"id", ColType::Int, 0},
                {"name", ColType::VarChar, 16},
        };
        db.create_table("t", cols);

        db.insert("t", {Value{int64_t{1}}, Value{std::string{"alice"}}});
        db.insert("t", {Value{int64_t{2}}, Value{std::string{"bob"}}});
        // 跨页: 一条记录塞满首个数据页后触发扩展
        for (int i = 0; i < 400; ++i) {
            db.insert("t", {Value{int64_t{3}}, Value{std::string{"spam"}}});
        }
        EXPECT_EQ(db.row_count("t"), size_t{402});

        // float/char 列编解码往返
        db.create_table("t2", {
                {"score", ColType::Float, 0},
                {"grade", ColType::Char, 3},
        });
        db.insert("t2", {Value{0.5}, Value{std::string{"A"}}});
        db.insert("t2", {Value{-1.25}, Value{std::string{"XYZ"}}});
        db.close();
    }

    {
        Database db(dir.path);
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
            EXPECT_EQ(db.delete_by_ref(first.ref), 1);
            EXPECT_EQ(db.row_count("t"), size_t{401});
            EXPECT_EQ(db.delete_by_ref(first.ref), 0);
            EXPECT_EQ(db.delete_by_ref(RowRef{}), 0);
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
        db.delete_all("t2");
        EXPECT_EQ(db.row_count("t2"), size_t{0});
        {
            auto s = db.scan("t2");
            Row r;
            EXPECT_FALSE(s->next(&r));
        }

        // drop 后目录不可见且数据文件删除, 其他表文件不受影响
        db.drop_table("t");
        EXPECT_THROW(db.row_count("t"), std::runtime_error);
        EXPECT_FALSE(std::filesystem::exists(dir.path + "/t_1.dat"));
        EXPECT_TRUE(std::filesystem::exists(dir.path + "/t_2.dat"));
        db.close();
    }

    {
        Database db(dir.path);
        db.open();
        EXPECT_THROW(db.row_count("t"), std::runtime_error);
        EXPECT_EQ(db.row_count("t2"), size_t{0});
        db.close();
    }
}

// 目录未初始化时 open 报 CatalogMissing
TEST_F(StorageDb, OpenWithoutCreateFails)
{
    Database db(dir.path);
    try {
        db.open();
        FAIL() << "未初始化目录 open 应报错";
    } catch (const db::DbError& e) {
        EXPECT_EQ(e.code(), db::ErrCode::CatalogMissing);
    }
}

// 已初始化目录重复 create 报 CatalogExists
TEST_F(StorageDb, CreateTwiceFails)
{
    {
        Database db(dir.path);
        db.create();
    }
    Database db(dir.path);
    try {
        db.create();
        FAIL() << "重复 create 应报错";
    } catch (const db::DbError& e) {
        EXPECT_EQ(e.code(), db::ErrCode::CatalogExists);
    }
}
