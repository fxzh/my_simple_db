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

// 引导数据目录(元数据表 + db_index + db_version 完成标记)后干净关闭, 供测试起步;
// 建表语句与 bootstrap.sql 保持一致, 漂移由 Initdb.Ok 守卫拦截
inline void bootstrap_version_marker(const std::string& path)
{
    ct::Catalog db(path, true);
    db.create();
    db.open();
    db.begin_txn();
    db.create_schema(ct::kPublicSchemaName);
    db.set_bootstrap_table_id(4);
    db.create_table({"system", ct::kIndexMetaName}, {{"table_id", st::ColType::BigInt, 0, true},
                                         {"index_name", st::ColType::VarChar, 64, true},
                                         {"col_ordinal", st::ColType::Int, 0, true},
                                         {"file_id", st::ColType::BigInt, 0, true}});
    db.set_bootstrap_table_id(5);
    db.create_table({"system", ct::kVersionMetaName}, {{"version", st::ColType::BigInt, 0, true}});
    db.commit_txn();
    db.close();
}

#endif
