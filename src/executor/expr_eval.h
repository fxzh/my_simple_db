// expr_eval.h: 表达式求值器: 常量/行上下文求值, WHERE 判定与结果值转换
#ifndef EXECUTOR_EXPR_EVAL_H
#define EXECUTOR_EXPR_EVAL_H

#include <cstdint>
#include <string>
#include <variant>

#include "ast.hh"
#include "bound.h"
#include "storage/types.h"

namespace exec {

// 求值字符串值: 记录是否来自 char 定长列, 该侧比较按 PAD SPACE 语义处理
struct StrVal {
    std::string text;
    bool from_char = false;
};

// 求值值域: 在存储值上扩展 bool; monostate 表示 NULL(条件上下文即 UNKNOWN)
using EvalValue = std::variant<std::monostate, bool, int64_t, double, StrVal>;

// 行上下文为绑定层产物 ana::Schema(见 analyzer/bound.h)

// 常量上下文求值(INSERT VALUES 等无行上下文的场景)
EvalValue eval_const(const Expr& e);

// 行上下文求值(SELECT 投影与 WHERE 过滤)
EvalValue eval_row(const Expr& e, const ana::Schema& ctx, const st::Row& row);

// 求值结果转存储/输出值: bool 不允许作为结果值, 其余原样(monostate 即 NULL)
st::Value to_st_value(const EvalValue& v);

// WHERE 条件判定: 结果须为 bool, NULL(UNKNOWN) 视为不满足
bool where_match(const Expr& where, const ana::Schema& ctx, const st::Row& row);

}  // namespace exec

#endif  // EXECUTOR_EXPR_EVAL_H
