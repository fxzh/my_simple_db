// bound.h: 绑定层产物: 名字解析后的语句结构(BoundStmt)与投影列
#ifndef ANALYZER_BOUND_H
#define ANALYZER_BOUND_H

#include <memory>
#include <string>
#include <vector>

#include "bound_expr.h"
#include "storage/types.h"

namespace ana {

// 投影输出列: star 展开的原始列直接取行值, 其余按绑定表达式逐行求值
struct ProjCol {
    std::unique_ptr<BoundExpr> expr;  // 为空表示 star 展开的原始列
    std::string name;                 // 输出列名
    size_t col_idx = 0;               // star 列的行内下标
};

// 绑定语句种类, 供执行层按类型分派
enum class BoundKind {
    CreateTable, DropTable, CreateSchema, DropSchema, Insert, Delete, Select,
};

// 绑定语句基类: 表达式为绑定树, 由语句对象持有
struct BoundStmt {
    virtual ~BoundStmt() = default;
    virtual BoundKind kind() const = 0;
};

// CREATE TABLE: 列规格已完成类型映射与长度校验
struct BoundCreateTable : BoundStmt {
    std::string table;
    std::vector<st::ColumnSpec> cols;
    BoundKind kind() const override { return BoundKind::CreateTable; }
};

// DROP TABLE
struct BoundDropTable : BoundStmt {
    std::string table;
    BoundKind kind() const override { return BoundKind::DropTable; }
};

// CREATE SCHEMA: 名字透传, 名字校验在 catalog 层
struct BoundCreateSchema : BoundStmt {
    std::string schema;
    BoundKind kind() const override { return BoundKind::CreateSchema; }
};

// DROP SCHEMA: 名字透传
struct BoundDropSchema : BoundStmt {
    std::string schema;
    BoundKind kind() const override { return BoundKind::DropSchema; }
};

// INSERT: 每行的值为常量上下文绑定树(已归一化为表全宽), 留待执行期求值
struct BoundInsert : BoundStmt {
    std::string table;
    std::vector<std::vector<std::unique_ptr<BoundExpr>>> rows;
    BoundKind kind() const override { return BoundKind::Insert; }
};

// DELETE FROM: where 为空表示全表删除
struct BoundDelete : BoundStmt {
    std::string table;
    std::unique_ptr<BoundExpr> where;
    BoundKind kind() const override { return BoundKind::Delete; }
};

// SELECT: 投影已展开(star 列定位/输出列名), where 为空表示无过滤
struct BoundSelect : BoundStmt {
    std::string table;
    std::vector<ProjCol> projs;
    std::unique_ptr<BoundExpr> where;
    BoundKind kind() const override { return BoundKind::Select; }
};

}  // namespace ana

#endif  // ANALYZER_BOUND_H
