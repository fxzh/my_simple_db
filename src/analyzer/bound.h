// bound.h: 绑定层产物: 名字解析后的语句结构(BoundStmt)与行结构/投影列
#ifndef ANALYZER_BOUND_H
#define ANALYZER_BOUND_H

#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hh"
#include "storage/types.h"

namespace ana {

// 行结构(名字解析结果): 列名定位表 + char 定长列标记, 每条语句绑定一次
using ColMap = std::unordered_map<std::string, size_t>;
struct Schema {
    ColMap cols;                 // 列名 → 行内下标
    std::vector<bool> char_col;  // char 定长列标记, 与行内下标对应
};

// 投影输出列: star 展开的原始列直接取行值, 其余按表达式逐行求值
struct ProjCol {
    const Expr* expr = nullptr;  // 为空表示 star 展开的原始列
    std::string name;            // 输出列名
    size_t col_idx = 0;          // star 列的行内下标
};

// 绑定语句种类, 供执行层按类型分派
enum class BoundKind {
    CreateTable, DropTable, Insert, Delete, Select,
};

// 绑定语句基类: 表达式指针指向 AST 原节点, 生命周期由 execute() 调用期持有的语句保证
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

// INSERT: 值表达式留待执行期常量上下文求值
struct BoundInsert : BoundStmt {
    std::string table;
    std::vector<const Expr*> values;
    BoundKind kind() const override { return BoundKind::Insert; }
};

// DELETE FROM: where 为空表示全表删除
struct BoundDelete : BoundStmt {
    std::string table;
    const Expr* where = nullptr;
    Schema schema;               // where 求值用行结构
    BoundKind kind() const override { return BoundKind::Delete; }
};

// SELECT: 投影已展开(star 列定位/输出列名), where 为空表示无过滤
struct BoundSelect : BoundStmt {
    std::string table;
    Schema schema;               // 投影与 where 求值用行结构
    std::vector<ProjCol> projs;
    const Expr* where = nullptr;
    BoundKind kind() const override { return BoundKind::Select; }
};

}  // namespace ana

#endif  // ANALYZER_BOUND_H
