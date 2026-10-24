// storage_fixture.hpp: storage 系测试共享基建: 临时数据目录 fixture 与版本标记引导
#ifndef TEST_STORAGE_STORAGE_FIXTURE_HPP
#define TEST_STORAGE_STORAGE_FIXTURE_HPP

#include <string>

#include <gtest/gtest.h>

#include "common/temp_dir.hpp"
#include "log/log.h"
#include "catalog.h"

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

// 引导数据目录后干净关闭, 供测试起步;
// 同 bootstrap.sql 保持一致, 漂移由 Initdb.Ok 守卫拦截
inline void bootstrap_version_marker(const std::string& path)
{
    ct::Catalog db(path, true);
    db.create();
    db.open();
    db.begin_txn();
    db.create_schema(ct::kPublicSchemaName);
    db.set_bootstrap_table_id(5);
    db.create_table({"system", ct::kIndexMetaName}, {{"table_id", st::kBigint, 0, true},
                                         {"index_name", st::kVarChar, 64, true},
                                         {"col_ordinal", st::kInt, 0, true},
                                         {"file_id", st::kBigint, 0, true}});
    db.set_bootstrap_table_id(6);
    db.create_table({"system", ct::kFuncMetaName}, {{"func_id", st::kBigint, 0, true},
                                         {"func_name", st::kVarChar, 64, true},
                                         {"arity", st::kInt, 0, true},
                                         {"arg_0_type", st::kInt, 0, true},
                                         {"arg_1_type", st::kInt, 0, true},
                                         {"arg_2_type", st::kInt, 0, true},
                                         {"ret_type", st::kInt, 0, true},
                                         {"strict_null", st::kInt, 0, true}});
    // 函数注册行: 一元重载实参与结果同型, round 二元重载为 float 实参加整数保留位, mod 两实参与结果同型
    const struct
    {
        int64_t id;
        const char* name;
        int64_t arity;
        int64_t arg_0;
        int64_t arg_1;
        int64_t ret;
    } func_rows[] = {
            {1, "abs", 1, 1, 0, 1}, {2, "abs", 1, 2, 0, 2}, {3, "abs", 1, 3, 0, 3},
            {4, "abs", 1, 5, 0, 5}, {5, "abs", 1, 8, 0, 8}, {6, "round", 1, 1, 0, 1},
            {7, "round", 1, 2, 0, 2}, {8, "round", 1, 3, 0, 3}, {9, "round", 1, 5, 0, 5},
            {10, "round", 1, 8, 0, 8}, {11, "round", 2, 8, 1, 8}, {12, "floor", 1, 1, 0, 1},
            {13, "floor", 1, 2, 0, 2}, {14, "floor", 1, 3, 0, 3}, {15, "floor", 1, 5, 0, 5},
            {16, "floor", 1, 8, 0, 8}, {17, "ceil", 1, 1, 0, 1}, {18, "ceil", 1, 2, 0, 2},
            {19, "ceil", 1, 3, 0, 3}, {20, "ceil", 1, 5, 0, 5}, {21, "ceil", 1, 8, 0, 8},
            {22, "mod", 2, 1, 1, 1}, {23, "mod", 2, 2, 2, 2}, {24, "mod", 2, 3, 3, 3},
            {25, "mod", 2, 5, 5, 5}, {26, "mod", 2, 8, 8, 8}};
    const st::TableMeta func_meta = db.table_meta({"system", ct::kFuncMetaName});
    for (const auto& r : func_rows) {
        db.insert(func_meta, {st::int_val(st::kBigint, r.id), st::str_val(st::kVarChar, r.name),
                              st::int_val(st::kInt, r.arity), st::int_val(st::kInt, r.arg_0),
                              st::int_val(st::kInt, r.arg_1), st::int_val(st::kInt, 0),
                              st::int_val(st::kInt, r.ret), st::int_val(st::kInt, 1)});
    }
    db.set_bootstrap_table_id(7);
    db.create_table({"system", ct::kVersionMetaName}, {{"version", st::kBigint, 0, true}});
    db.commit_txn();
    db.close();
}

#endif
