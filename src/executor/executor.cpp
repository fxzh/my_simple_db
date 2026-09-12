// executor.cpp: 执行层实现: AST 转 storage 调用
#include "executor.h"

#include <cstdint>
#include <string>
#include <vector>

#include "ast.hh"
#include "codec.h"
#include "log/log.h"
#include "storage.h"

namespace exec {

namespace {

// AST 列定义转存储层列规格; 类型名非法时当场记录并抛出
void convert_columns(const std::vector<ColumnDef>& defs, std::vector<st::ColumnSpec>& cols)
{
    cols.reserve(defs.size());
    for (const ColumnDef& def : defs) {
        st::ColType type;
        uint16_t len = 0;
        if (!st::parse_column_type(def.type, &type, &len)) {
            LOG(LogLevel::ERROR, LogModule::EXECUTOR, "不支持的类型: %s", def.type.c_str());
        }
        cols.emplace_back(st::ColumnSpec{def.name, type, len});
    }
}

}  // namespace

std::string execute(st::Database& db, const SQLStatement& stmt)
{
    const std::string& kind = stmt.statement_kind();
    if (kind == "create table") {
        const auto& cs = static_cast<const CreateTableStmt&>(stmt);
        std::vector<st::ColumnSpec> cols;
        convert_columns(cs.column_defs(), cols);
        db.create_table(cs.table_name(), cols);
        return "OK";
    }
    if (kind == "drop table") {
        const auto& ds = static_cast<const DropTableStmt&>(stmt);
        db.drop_table(ds.table_name());
        return "OK";
    }
    return "ERROR: " + kind + " 暂不支持";
}

}  // namespace exec