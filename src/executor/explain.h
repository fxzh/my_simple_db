// explain.h: EXPLAIN 执行入口: 计划树渲染为 PG 风格文本行结果集
#ifndef EXECUTOR_EXPLAIN_H
#define EXECUTOR_EXPLAIN_H

#include "executor.h"

namespace exec {

// 解释一条解释计划: 渲染被解释语句的计划树为单列(QUERY PLAN)文本行,
// 返回已 open 的物化结果集算子; 不执行被解释语句, 列名取计划携带的表句柄
ExecResult run_explain(const pl::ExplainPlan& plan);

}  // namespace exec

#endif  // EXECUTOR_EXPLAIN_H
