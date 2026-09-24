// planner.h: 计划层入口: BoundStmt → 计划节点树
#ifndef PLANNER_PLANNER_H
#define PLANNER_PLANNER_H

#include <memory>

#include "bound.h"
#include "plan.h"

namespace pl {

// 生成一条语句的计划: select → Project(Filter(SeqScan))(无 WHERE 省 Filter),
// delete 带 WHERE → Delete(Filter(SeqScan))(无 WHERE 为无 child 叶子), 其余为叶子计划;
// 计划构建移动消费绑定语句的容器字段(投影列/行结构/值列表/列规格), 绑定语句之后不可再用;
// 计划中的表达式指针指向绑定语句所引用的 AST 原节点, 生命周期由调用方保证
std::unique_ptr<PlanNode> build(ana::BoundStmt& bound);

}  // namespace pl

#endif  // PLANNER_PLANNER_H
