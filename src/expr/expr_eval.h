// expr_eval.h: 绑定表达式求值器: 常量/行上下文求值, WHERE 判定与值三向比较
#ifndef EXPR_EXPR_EVAL_H
#define EXPR_EXPR_EVAL_H

#include "bound_expr.h"
#include "storage/types.h"

namespace expr {

// 求值对象为绑定层产物 ana::BoundExpr(见 analyzer/bound_expr.h)

// 常量上下文求值(INSERT VALUES 等无行上下文的场景, 树中不含列引用)
st::Value eval_const(const ana::BoundExpr& e);

// 行上下文求值(SELECT 投影与 WHERE 过滤)
st::Value eval_row(const ana::BoundExpr& e, const st::Row& row);

// 值三向比较(排序/去重共享): NULL 最大, 双 NULL 相等; 布尔 false<true; 整型按 int64;
// 数值家族混合提升 double; 字符串字典序且仅 Char 侧去尾随空格; 家族不可比报 Internal
int value_cmp(const st::Value& a, const st::Value& b);

// WHERE 条件判定: NULL(UNKNOWN) 视为不满足, bool 由语义层保证
bool where_match(const ana::BoundExpr& where, const st::Row& row);

}  // namespace expr

#endif  // EXPR_EXPR_EVAL_H
