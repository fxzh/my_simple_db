// planner.h: 计划层入口: BoundStmt → 计划节点树
#ifndef PLANNER_PLANNER_H
#define PLANNER_PLANNER_H

#include <memory>

#include "bound.h"
#include "plan.h"

namespace pl {

// 生成一条语句的计划: select → [Sort](Project([Sort]([Filter](SeqScan))))(无 WHERE/排序省对应节点),
// delete 带 WHERE → Delete(Filter(SeqScan))(无 WHERE 为无 child 叶子), 其余为叶子计划;
// 计划构建移动消费绑定语句的容器字段(投影列/绑定树/值列表/列规格), 绑定语句之后不可再用
std::unique_ptr<PlanNode> build(ana::BoundStmt& bound);

// 计划优化入口: 在 build 产物上就地运行优化 pass, 常量运算错误(溢出/除零)在此报错
void optimize(std::unique_ptr<PlanNode>& plan);

// 逻辑节点布尔化简: 单侧常量 bool 按支配/恒等规则整树替换, 未命中保持原树;
// 由常量折叠在同一遍历中调用, 调用方保证 e 为 Logic 且两子树已折叠
void simplify_logic(std::unique_ptr<ana::BoundExpr>& e);

// NOT 消除: 双重否定/比较符取反/判空取反整树替换, 未命中保持原树;
// 由常量折叠在同一遍历中调用, 调用方保证 e 为 Not 且操作数已折叠且非常量
void simplify_not(std::unique_ptr<ana::BoundExpr>& e);

// 常量过滤器剪除: 恒真 Filter 以子节点替换, 恒不满足(含 NULL 常量谓词)的 Filter
// 整棵子树剪成空结果节点; 在常量折叠之后运行
void prune_filter(std::unique_ptr<PlanNode>& plan);

}  // namespace pl

#endif  // PLANNER_PLANNER_H
