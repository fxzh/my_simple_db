// plan.h: 计划层产物: 计划节点(PlanNode)树, 现阶段逻辑计划即物理计划
#ifndef PLANNER_PLAN_H
#define PLANNER_PLAN_H

#include <memory>
#include <string>
#include <vector>

#include "bound.h"
#include "storage/btree.h"
#include "storage/types.h"

namespace pl {

// 计划节点种类, 供执行层按类型分派
enum class PlanKind {
    SeqScan, IndexScan, Fetch, DummyScan, Filter, Project, Sort, Empty, Insert, Delete, Update,
    CreateTable, DropTable, CreateSchema, DropSchema, CreateIndex, DropIndex, Set, Explain,
};

// 计划节点基类: 表达式为绑定树, 由计划节点持有
struct PlanNode {
    virtual ~PlanNode() = default;
    virtual PlanKind kind() const = 0;
};

// 全表扫描
struct SeqScanPlan : PlanNode {
    ct::TableHandle table;
    PlanKind kind() const override { return PlanKind::SeqScan; }
};

// 索引扫描区间: lo/hi 缺省为无界, 开闭由 inclusive 决定(键序 NULL 最大)
struct IndexRange {
    std::optional<st::ScanBound> lo;
    std::optional<st::ScanBound> hi;
};

// 索引命中条件(EXPLAIN 渲染用): 比较条件用 col/op/value, 判空条件 is_null 置位
struct IndexCond {
    std::string col;
    CmpOp op = CmpOp::Eq;
    st::Value value;
    bool is_null = false;
    bool negate = false;  // is_null 时 true 表示 IS NOT NULL
};

// 索引扫描: 逐段扫索引只吐行物理位置(值列空), 区间精确等于被摘除的过滤合取项
struct IndexScanPlan : PlanNode {
    ct::TableHandle table;
    std::string index;                // 索引名(EXPLAIN)
    uint64_t index_fid = 0;           // 索引文件 id
    std::vector<IndexRange> ranges;   // 不相交区间, 键序升序
    std::vector<IndexCond> conds;     // 命中条件(渲染 Index Cond)
    PlanKind kind() const override { return PlanKind::IndexScan; }
};

// 回表: 按子行物理位置直读堆页补全行值, 死引用(残留索引条目)跳过
struct FetchPlan : PlanNode {
    ct::TableHandle table;
    std::unique_ptr<PlanNode> child;
    PlanKind kind() const override { return PlanKind::Fetch; }
};

// 单行扫描: 无 FROM 的 SELECT 行源, 恒一行零列
struct DummyScanPlan : PlanNode {
    PlanKind kind() const override { return PlanKind::DummyScan; }
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

// 排序: 物化子树全部行, 按键排序后输出; sort_on_output 为 true 时键取子行输出列下标
// (挂 Project 之上), 为 false 时键为行上下文表达式(挂 Project 之下, 对原始行求值)
struct SortPlan : PlanNode {
    std::unique_ptr<PlanNode> child;
    std::vector<ana::BoundOrderItem> orders;
    bool sort_on_output = false;
    PlanKind kind() const override { return PlanKind::Sort; }
};

// 空结果: 恒不满足的过滤子树剪枝产物, 恒 0 行
struct EmptyPlan : PlanNode {
    std::string table;  // 剪除前子树行源的限定表名, 无 FROM 时为空
    PlanKind kind() const override { return PlanKind::Empty; }
};

// 插入: 每行的值为常量上下文绑定树(已归一化为表全宽)
struct InsertPlan : PlanNode {
    ct::TableHandle table;
    std::vector<std::vector<std::unique_ptr<ana::BoundExpr>>> rows;
    PlanKind kind() const override { return PlanKind::Insert; }
};

// 删除: child 为空表示全表删除, 非空为 Filter(SeqScan) 子树
struct DeletePlan : PlanNode {
    ct::TableHandle table;
    std::unique_ptr<PlanNode> child;
    PlanKind kind() const override { return PlanKind::Delete; }
};

// 更新: 赋值右值基于旧行求值后替换目标列; child 为 Filter(SeqScan), 无 WHERE 时为 SeqScan
struct UpdatePlan : PlanNode {
    ct::TableHandle table;
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
    ct::TableHandle table;
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
    ct::TableHandle table;
    std::string index;
    uint16_t col_ordinal = 0;  // 索引列的列序号
    PlanKind kind() const override { return PlanKind::CreateIndex; }
};

// 删索引
struct DropIndexPlan : PlanNode {
    ct::TableHandle table;
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
