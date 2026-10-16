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
// 建表语句与 bootstrap.sql 保持一致, 漂移由 Initdb.Ok 守卫拦截
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
    // abs 重载行: 五个数值类型各一行, 实参与结果类型同为该类型
    const struct
    {
        int64_t id;
        int64_t type;
    } abs_rows[] = {{1, 1}, {2, 2}, {3, 3}, {4, 5}, {5, 8}};
    const st::TableMeta func_meta = db.table_meta({"system", ct::kFuncMetaName});
    for (const auto& r : abs_rows) {
        db.insert(func_meta, {st::int_val(st::kBigint, r.id), st::str_val(st::kVarChar, "abs"),
                              st::int_val(st::kInt, 1), st::int_val(st::kInt, r.type),
                              st::int_val(st::kInt, 0), st::int_val(st::kInt, 0),
                              st::int_val(st::kInt, r.type), st::int_val(st::kInt, 1)});
    }
    db.set_bootstrap_table_id(7);
    db.create_table({"system", ct::kVersionMetaName}, {{"version", st::kBigint, 0, true}});
    db.commit_txn();
    db.close();
}

#endif
