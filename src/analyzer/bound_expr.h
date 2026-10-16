// bound_expr.h: 表达式树: 名字已在语义分析期解析为行内下标
#ifndef ANALYZER_BOUND_EXPR_H
#define ANALYZER_BOUND_EXPR_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast.hh"
#include "storage/types.h"

namespace ana {

// 语义分析表达式种类, 供执行层按类型分派
enum class BoundExprKind {
    Const, ColRef, Arith, Neg, Cmp, Logic, Not, IsNull, FuncCall, Cast,
};

// 语义分析表达式基类: 常量上下文绑定的树不含 ColRef
struct BoundExpr {
    virtual ~BoundExpr() = default;
    virtual BoundExprKind kind() const = 0;
};

// 字面量常量: 绑定期折入字面量值, 不做算术折叠
struct BoundConst : BoundExpr {
    st::Value value;
    explicit BoundConst(st::Value v) : value(std::move(v)) {}
    BoundExprKind kind() const override { return BoundExprKind::Const; }
};

// 列引用: 行内下标在绑定期固定, type 为列静态类型属性(供计划期语境判定)
struct BoundColRef : BoundExpr {
    size_t col_idx = 0;
    st::TypeAttr type;
    BoundColRef(size_t idx, st::TypeAttr t) : col_idx(idx), type(t) {}
    BoundExprKind kind() const override { return BoundExprKind::ColRef; }
};

// 算术运算: + - * /; type 为绑定期推导的结果域属性, 求值按域构造与检查;
// numeric 结果标度在求值期按操作数值标度推导(裸 numeric 列值标度不可静态知)
struct BoundArith : BoundExpr {
    char op;
    st::TypeAttr type;
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;
    BoundArith(char op_, std::unique_ptr<BoundExpr> left_, std::unique_ptr<BoundExpr> right_,
               st::TypeAttr type_)
        : op(op_), type(type_), left(std::move(left_)), right(std::move(right_)) {}
    BoundExprKind kind() const override { return BoundExprKind::Arith; }
};

// 一元负号(一元正号在绑定期折为操作数本身); type 为绑定期推导的结果域属性
struct BoundNeg : BoundExpr {
    st::TypeAttr type;
    std::unique_ptr<BoundExpr> operand;
    explicit BoundNeg(std::unique_ptr<BoundExpr> operand_, st::TypeAttr type_)
        : type(type_), operand(std::move(operand_)) {}
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

// 函数调用: func 为绑定期选定的重载版本 id(db_func.func_id), name 为函数名,
// strict_null 为该重载的 NULL 严格性(严格即任一实参 NULL 则结果 NULL),
// type 为绑定期推导的结果域属性, 实参已绑定
struct BoundFuncCall : BoundExpr {
    uint64_t func = 0;
    std::string name;
    bool strict_null = true;
    st::TypeAttr type;
    std::vector<std::unique_ptr<BoundExpr>> args;
    BoundFuncCall(uint64_t f, std::string n, bool strict, std::vector<std::unique_ptr<BoundExpr>> a,
                  st::TypeAttr t)
        : func(f), name(std::move(n)), strict_null(strict), type(t), args(std::move(a)) {}
    BoundExprKind kind() const override { return BoundExprKind::FuncCall; }
};

// CAST 显式转换: type/length/scale 为目标域属性与目标长度与目标标度(0 为不限), 实参已绑定;
// 长度检查在求值期
struct BoundCast : BoundExpr {
    st::TypeAttr type;
    uint16_t length = 0;
    uint8_t scale = 0;
    std::unique_ptr<BoundExpr> operand;
    BoundCast(std::unique_ptr<BoundExpr> operand_, st::TypeAttr type_, uint16_t length_,
              uint8_t scale_)
        : type(type_), length(length_), scale(scale_), operand(std::move(operand_)) {}
    BoundExprKind kind() const override { return BoundExprKind::Cast; }
};

}  // namespace ana

#endif  // ANALYZER_BOUND_EXPR_H
