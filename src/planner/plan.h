// plan.h: 计划层产物: 计划节点(PlanNode)树, 现阶段逻辑计划即物理计划
#ifndef PLANNER_PLAN_H
#define PLANNER_PLAN_H

#include <memory>
#include <string>
#include <vector>

#include "bound.h"
#include "storage/types.h"

namespace pl {

// 计划节点种类, 供执行层按类型分派
enum class PlanKind {
    SeqScan, Filter, Project, Empty, Insert, Delete, Update, CreateTable, DropTable, CreateSchema,
    DropSchema, CreateIndex, DropIndex, Set, Explain,
};

// 计划节点基类: 表达式为绑定树, 由计划节点持有
struct PlanNode {
    virtual ~PlanNode() = default;
    virtual PlanKind kind() const = 0;
};

// 全表扫描
struct SeqScanPlan : PlanNode {
    ct::TableRef table;
    PlanKind kind() const override { return PlanKind::SeqScan; }
};

// 过滤: 逐行求值谓词, 不满足的行不向父节点输出
struct FilterPlan : PlanNode {
    std::unique_ptr<PlanNode> child;
    std::unique_ptr<ana::BoundExpr> pred;  // 绑定谓词
    PlanKind kind() const override { return PlanKind::Filter; }
};

// 投影: 按已展开的投影列逐行求值输出
struct ProjectPlan : PlanNode {
    std::unique_ptr<PlanNode> child;
    std::vector<ana::ProjCol> projs;
    PlanKind kind() const override { return PlanKind::Project; }
};

// 空结果: 恒不满足的过滤子树剪枝产物, 恒 0 行
struct EmptyPlan : PlanNode {
    PlanKind kind() const override { return PlanKind::Empty; }
};

// 插入: 每行的值为常量上下文绑定树(已归一化为表全宽)
struct InsertPlan : PlanNode {
    ct::TableRef table;
    std::vector<std::vector<std::unique_ptr<ana::BoundExpr>>> rows;
    PlanKind kind() const override { return PlanKind::Insert; }
};

// 删除: child 为空表示全表删除, 非空为 Filter(SeqScan) 子树
struct DeletePlan : PlanNode {
    ct::TableRef table;
    std::unique_ptr<PlanNode> child;
    PlanKind kind() const override { return PlanKind::Delete; }
};

// 更新: 赋值右值基于旧行求值后替换目标列; child 为 Filter(SeqScan), 无 WHERE 时为 SeqScan
struct UpdatePlan : PlanNode {
    ct::TableRef table;
    std::vector<ana::BoundUpdateItem> assigns;
    std::unique_ptr<PlanNode> child;
    PlanKind kind() const override { return PlanKind::Update; }
};

// 建表: 列规格已完成类型映射与长度校验
struct CreateTablePlan : PlanNode {
    ct::TableRef table;
    std::vector<st::ColumnSpec> cols;
    PlanKind kind() const override { return PlanKind::CreateTable; }
};

// 删表
struct DropTablePlan : PlanNode {
    ct::TableRef table;
    PlanKind kind() const override { return PlanKind::DropTable; }
};

// 建 schema
struct CreateSchemaPlan : PlanNode {
    std::string schema;
    PlanKind kind() const override { return PlanKind::CreateSchema; }
};

// 删 schema
struct DropSchemaPlan : PlanNode {
    std::string schema;
    PlanKind kind() const override { return PlanKind::DropSchema; }
};

// 建索引
struct CreateIndexPlan : PlanNode {
    ct::TableRef table;
    std::string index;
    uint16_t col_ordinal = 0;  // 索引列的列序号
    PlanKind kind() const override { return PlanKind::CreateIndex; }
};

// 删索引
struct DropIndexPlan : PlanNode {
    ct::TableRef table;
    std::string index;
    PlanKind kind() const override { return PlanKind::DropIndex; }
};

// SET 变量(bootstrap 模式): 值透传给 catalog 变量成员
struct SetPlan : PlanNode {
    ana::SetVar var;
    uint64_t value;
    PlanKind kind() const override { return PlanKind::Set; }
};

// 解释: 持被解释语句的计划, 执行期渲染为文本行结果集
struct ExplainPlan : PlanNode {
    std::unique_ptr<PlanNode> child;
    PlanKind kind() const override { return PlanKind::Explain; }
};

}  // namespace pl

#endif  // PLANNER_PLAN_H
