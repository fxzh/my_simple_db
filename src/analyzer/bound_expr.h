// bound_expr.h: 表达式树: 名字已在语义分析期解析为行内下标
#ifndef ANALYZER_BOUND_EXPR_H
#define ANALYZER_BOUND_EXPR_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "ast.hh"

namespace ana {

// 语义分析表达式种类, 供执行层按类型分派
enum class BoundExprKind {
    Const, ColRef, Arith, Neg, Cmp, Logic, Not, IsNull,
};

// 语义分析表达式基类: 常量上下文绑定的树不含 ColRef
struct BoundExpr {
    virtual ~BoundExpr() = default;
    virtual BoundExprKind kind() const = 0;
};

// 字面量常量: 绑定期折入字面量值, 不做算术折叠
struct BoundConst : BoundExpr {
    std::variant<std::monostate, bool, int64_t, double, std::string> value;  // monostate 即 NULL
    template <typename T>
    explicit BoundConst(T&& v) : value(std::forward<T>(v)) {}
    BoundExprKind kind() const override { return BoundExprKind::Const; }
};

// 列引用: 行内下标与 char 定长标记在绑定期固化
struct BoundColRef : BoundExpr {
    size_t col_idx = 0;
    bool from_char = false;
    explicit BoundColRef(size_t idx, bool fc) : col_idx(idx), from_char(fc) {}
    BoundExprKind kind() const override { return BoundExprKind::ColRef; }
};

// 算术运算: + - * /
struct BoundArith : BoundExpr {
    char op;
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;
    BoundArith(char op_, std::unique_ptr<BoundExpr> left_, std::unique_ptr<BoundExpr> right_)
        : op(op_), left(std::move(left_)), right(std::move(right_)) {}
    BoundExprKind kind() const override { return BoundExprKind::Arith; }
};

// 一元负号(一元正号在绑定期折为操作数本身)
struct BoundNeg : BoundExpr {
    std::unique_ptr<BoundExpr> operand;
    explicit BoundNeg(std::unique_ptr<BoundExpr> operand_) : operand(std::move(operand_)) {}
    BoundExprKind kind() const override { return BoundExprKind::Neg; }
};

// 比较运算: = <> < <= > >=
struct BoundCmp : BoundExpr {
    CmpOp op;
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;
    BoundCmp(CmpOp op_, std::unique_ptr<BoundExpr> left_, std::unique_ptr<BoundExpr> right_)
        : op(op_), left(std::move(left_)), right(std::move(right_)) {}
    BoundExprKind kind() const override { return BoundExprKind::Cmp; }
};

// 逻辑运算: AND / OR
struct BoundLogic : BoundExpr {
    LogicOp op;
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;
    BoundLogic(LogicOp op_, std::unique_ptr<BoundExpr> left_, std::unique_ptr<BoundExpr> right_)
        : op(op_), left(std::move(left_)), right(std::move(right_)) {}
    BoundExprKind kind() const override { return BoundExprKind::Logic; }
};

// 逻辑非
struct BoundNot : BoundExpr {
    std::unique_ptr<BoundExpr> operand;
    explicit BoundNot(std::unique_ptr<BoundExpr> operand_) : operand(std::move(operand_)) {}
    BoundExprKind kind() const override { return BoundExprKind::Not; }
};

// IS [NOT] NULL 判空
struct BoundIsNull : BoundExpr {
    std::unique_ptr<BoundExpr> operand;
    bool negate = false;  // true 表示 IS NOT NULL
    BoundIsNull(std::unique_ptr<BoundExpr> operand_, bool negate_)
        : operand(std::move(operand_)), negate(negate_) {}
    BoundExprKind kind() const override { return BoundExprKind::IsNull; }
};

}  // namespace ana

#endif  // ANALYZER_BOUND_EXPR_H
