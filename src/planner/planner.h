// planner.h: 计划层入口: BoundStmt → 计划节点树
#ifndef PLANNER_PLANNER_H
#define PLANNER_PLANNER_H

#include <memory>

#include "bound.h"
#include "plan.h"

namespace pl {

// 生成一条语句的计划: select → Project(Filter(SeqScan))(无 WHERE 省 Filter),
// delete 带 WHERE → Delete(Filter(SeqScan))(无 WHERE 为无 child 叶子), 其余为叶子计划;
// 计划构建移动消费绑定语句的容器字段(投影列/绑定树/值列表/列规格), 绑定语句之后不可再用
std::unique_ptr<PlanNode> build(ana::BoundStmt& bound);

// 计划优化入口: 在 build 产物上就地运行优化 pass, 常量运算错误(溢出/除零)在此报错
void optimize(PlanNode& plan);

// 逻辑节点布尔化简: 单侧常量 bool 按支配/恒等规则整树替换, 未命中保持原树;
// 由常量折叠在同一遍历中调用, 调用方保证 e 为 Logic 且两子树已折叠
void simplify_logic(std::unique_ptr<ana::BoundExpr>& e);

}  // namespace pl

#endif  // PLANNER_PLANNER_H
