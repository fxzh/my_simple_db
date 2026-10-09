// bound.h: 名字解析后的语句结构(BoundStmt)与投影列
#ifndef ANALYZER_BOUND_H
#define ANALYZER_BOUND_H

#include <memory>
#include <string>
#include <vector>

#include "bound_expr.h"
#include "catalog.h"
#include "storage/types.h"

namespace ana {

// 投影输出列: star 展开的原始列直接取行值, 其余按表达式逐行求值
struct ProjCol {
    std::unique_ptr<BoundExpr> expr;  // 为空表示 star 展开的原始列
    std::string name;                 // 输出列名
    size_t col_idx = 0;               // star 列的行内下标
};

// SELECT 排序键: 方向 + 绑定结果(输出列下标或行上下文表达式, 由语句的 sort_on_output 决定取哪个)
struct BoundOrderItem {
    bool desc = false;
    size_t out_idx = 0;               // sort_on_output=true 时的输出列下标
    std::unique_ptr<BoundExpr> expr;  // sort_on_output=false 时的行上下文绑定键
};

// 语句种类, 供执行层按类型分派
enum class BoundKind {
    CreateTable, DropTable, CreateSchema, DropSchema, CreateIndex, DropIndex, Insert, Delete,
    Update, Select, SelectNoFrom, Set, Explain,
};

// 绑定语句基类: 表达式为绑定树, 由语句对象持有
struct BoundStmt {
    virtual ~BoundStmt() = default;
    virtual BoundKind kind() const = 0;
};

// CREATE TABLE: 限定表名(未限定已填 current_schema) + 列规格已完成类型映射与长度校验
struct BoundCreateTable : BoundStmt {
    ct::TableRef table;
    std::vector<st::ColumnSpec> cols;
    BoundKind kind() const override { return BoundKind::CreateTable; }
};

// DROP TABLE: 表已解析为句柄
struct BoundDropTable : BoundStmt {
    ct::TableHandle table;
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

// CREATE INDEX: 表已解析为句柄, 列已定位, 列类型限数值定长
struct BoundCreateIndex : BoundStmt {
    ct::TableHandle table;
    std::string index;
    uint16_t col_ordinal = 0;  // 索引列的列序号
    BoundKind kind() const override { return BoundKind::CreateIndex; }
};

// DROP INDEX: 名字透传, 索引存在性校验在 catalog 层
struct BoundDropIndex : BoundStmt {
    ct::TableHandle table;
    std::string index;
    BoundKind kind() const override { return BoundKind::DropIndex; }
};

// INSERT: 每行的值为常量上下文绑定树(已归一化为表全宽), 留待执行期求值
struct BoundInsert : BoundStmt {
    ct::TableHandle table;
    std::vector<std::vector<std::unique_ptr<BoundExpr>>> rows;
    BoundKind kind() const override { return BoundKind::Insert; }
};

// DELETE FROM: where 为空表示全表删除
struct BoundDelete : BoundStmt {
    ct::TableHandle table;
    std::unique_ptr<BoundExpr> where;
    BoundKind kind() const override { return BoundKind::Delete; }
};

// UPDATE 赋值项: 目标列行内下标 + 新值绑定表达式
struct BoundUpdateItem {
    size_t col_idx = 0;
    std::unique_ptr<BoundExpr> value;
};

// UPDATE: 赋值右值为行上下文绑定树, 全部基于同一旧行求值后替换目标列; where 为空表示全表更新
struct BoundUpdate : BoundStmt {
    ct::TableHandle table;
    std::vector<BoundUpdateItem> assigns;
    std::unique_ptr<BoundExpr> where;
    BoundKind kind() const override { return BoundKind::Update; }
};

// SELECT: 投影已展开(star 列定位/输出列名), where 为空表示无过滤, orders 为空表示无排序
struct BoundSelect : BoundStmt {
    ct::TableHandle table;
    std::vector<ProjCol> projs;
    std::unique_ptr<BoundExpr> where;
    std::vector<BoundOrderItem> orders;
    bool sort_on_output = false;  // true: 键为输出列下标(排序在投影后), false: 键为行上下文表达式(投影前)
    BoundKind kind() const override { return BoundKind::Select; }
};

// 无 FROM 的 SELECT: 投影不含 star 与列引用(常量上下文绑定), where 为空表示无过滤,
// orders 为空表示无排序(行上下文为恒一行零列, 键只能绑成常量表达式)
struct BoundSelectNoFrom : BoundStmt {
    std::vector<ProjCol> projs;
    std::unique_ptr<BoundExpr> where;
    std::vector<BoundOrderItem> orders;
    bool sort_on_output = false;  // 取义同 BoundSelect
    BoundKind kind() const override { return BoundKind::SelectNoFrom; }
};

// EXPLAIN: 持绑定后的被解释语句(可解释范围由语法层限定)
struct BoundExplain : BoundStmt {
    std::unique_ptr<BoundStmt> inner;
    BoundKind kind() const override { return BoundKind::Explain; }
};

// bootstrap SET 变量种类(按对象类型独立命名)
enum class SetVar : uint8_t { TableId };

// SET: 变量已知名绑定, 值文本经接收侧转换校验(此处绑定的均为 bootstrap 变量)
struct BoundSet : BoundStmt {
    SetVar var;
    uint64_t value;
    BoundKind kind() const override { return BoundKind::Set; }
};

}  // namespace ana

#endif  // ANALYZER_BOUND_H
