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
    SeqScan, Filter, Project, Insert, Delete, CreateTable, DropTable, CreateSchema, DropSchema,
    Set,
};

// 计划节点基类: 表达式为绑定树, 由计划节点持有
struct PlanNode {
    virtual ~PlanNode() = default;
    virtual PlanKind kind() const = 0;
};

// 全表扫描
struct SeqScanPlan : PlanNode {
    std::string table;
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

// 插入: 每行的值为常量上下文绑定树(已归一化为表全宽), 留待执行期求值
struct InsertPlan : PlanNode {
    std::string table;
    std::vector<std::vector<std::unique_ptr<ana::BoundExpr>>> rows;
    PlanKind kind() const override { return PlanKind::Insert; }
};

// 删除: child 为空表示全表删除, 非空为 Filter(SeqScan) 子树
struct DeletePlan : PlanNode {
    std::string table;
    std::unique_ptr<PlanNode> child;
    PlanKind kind() const override { return PlanKind::Delete; }
};

// 建表: 列规格已完成类型映射与长度校验
struct CreateTablePlan : PlanNode {
    std::string table;
    std::vector<st::ColumnSpec> cols;
    PlanKind kind() const override { return PlanKind::CreateTable; }
};

// 删表
struct DropTablePlan : PlanNode {
    std::string table;
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

// SET 变量(bootstrap 模式): 值透传给 catalog 变量成员
struct SetPlan : PlanNode {
    ana::SetVar var;
    uint64_t value;
    PlanKind kind() const override { return PlanKind::Set; }
};

}  // namespace pl

#endif  // PLANNER_PLAN_H
