// bool_simplify.cpp: 逻辑节点与 NOT 布尔化简及自比较消除实现
#include "planner.h"

#include <memory>
#include <variant>

#include "bound_expr.h"
#include "common/err.h"
#include "log/log.h"

namespace pl {

namespace {

// 节点为常量 bool 时返回其值指针, 其余(含 NULL 常量与非常量节点)返回 nullptr
const bool* const_bool(const ana::BoundExpr& e)
{
    if (e.kind() != ana::BoundExprKind::Const) {
        return nullptr;
    }
    return std::get_if<bool>(&static_cast<const ana::BoundConst&>(e).value.box);
}

// 比较符取反: = <-> <>, < <-> >=, <= <-> >
CmpOp negate_cmp(CmpOp op)
{
    switch (op) {
    case CmpOp::Eq: return CmpOp::Ne;
    case CmpOp::Ne: return CmpOp::Eq;
    case CmpOp::Lt: return CmpOp::Ge;
    case CmpOp::Le: return CmpOp::Gt;
    case CmpOp::Gt: return CmpOp::Le;
    case CmpOp::Ge: return CmpOp::Lt;
    }
    // 不可达: 全部比较运算符已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "bool_simplify: 未知比较运算符");
}

// 常量值严格相等: 类型与形态一致且值相等
bool value_equal(const st::Value& a, const st::Value& b)
{
    return a.type == b.type && a.box == b.box;
}

// 表达式结构相等: 种类与影响求值的字段逐项一致, 子节点递归相等
bool expr_equal(const ana::BoundExpr& a, const ana::BoundExpr& b)
{
    if (a.kind() != b.kind()) {
        return false;
    }
    switch (a.kind()) {
    case ana::BoundExprKind::Const:
        return value_equal(static_cast<const ana::BoundConst&>(a).value,
                           static_cast<const ana::BoundConst&>(b).value);
    case ana::BoundExprKind::ColRef: {
        const auto& x = static_cast<const ana::BoundColRef&>(a);
        const auto& y = static_cast<const ana::BoundColRef&>(b);
        return x.col_idx == y.col_idx && x.type == y.type;
    }
    case ana::BoundExprKind::Arith: {
        const auto& x = static_cast<const ana::BoundArith&>(a);
        const auto& y = static_cast<const ana::BoundArith&>(b);
        return x.op == y.op && x.type == y.type && expr_equal(*x.left, *y.left)
               && expr_equal(*x.right, *y.right);
    }
    case ana::BoundExprKind::Neg: {
        const auto& x = static_cast<const ana::BoundNeg&>(a);
        const auto& y = static_cast<const ana::BoundNeg&>(b);
        return x.type == y.type && expr_equal(*x.operand, *y.operand);
    }
    case ana::BoundExprKind::Cmp: {
        const auto& x = static_cast<const ana::BoundCmp&>(a);
        const auto& y = static_cast<const ana::BoundCmp&>(b);
        return x.op == y.op && expr_equal(*x.left, *y.left) && expr_equal(*x.right, *y.right);
    }
    case ana::BoundExprKind::Logic: {
        const auto& x = static_cast<const ana::BoundLogic&>(a);
        const auto& y = static_cast<const ana::BoundLogic&>(b);
        return x.op == y.op && expr_equal(*x.left, *y.left) && expr_equal(*x.right, *y.right);
    }
    case ana::BoundExprKind::Not:
        return expr_equal(*static_cast<const ana::BoundNot&>(a).operand,
                          *static_cast<const ana::BoundNot&>(b).operand);
    case ana::BoundExprKind::IsNull: {
        const auto& x = static_cast<const ana::BoundIsNull&>(a);
        const auto& y = static_cast<const ana::BoundIsNull&>(b);
        return x.negate == y.negate && expr_equal(*x.operand, *y.operand);
    }
    case ana::BoundExprKind::FuncCall: {
        const auto& x = static_cast<const ana::BoundFuncCall&>(a);
        const auto& y = static_cast<const ana::BoundFuncCall&>(b);
        if (x.func != y.func || x.type != y.type || x.args.size() != y.args.size()) {
            return false;
        }
        for (size_t i = 0; i < x.args.size(); ++i) {
            if (!expr_equal(*x.args[i], *y.args[i])) {
                return false;
            }
        }
        return true;
    }
    }
    // 不可达: 全部绑定表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "bool_simplify: 未知绑定表达式节点");
}

}  // namespace

// 逻辑节点布尔化简: false 支配 AND / true 支配 OR, 常量恒等侧(AND 的 true / OR 的 false)
// 取另一侧; 无规则命中保持原树, 调用方保证 e 为 Logic 且两子树已折叠
void simplify_logic(std::unique_ptr<ana::BoundExpr>& e)
{
    auto& l = static_cast<ana::BoundLogic&>(*e);
    const bool* lb = const_bool(*l.left);
    const bool* rb = const_bool(*l.right);
    if (l.op == LogicOp::And) {
        if ((lb != nullptr && !*lb) || (rb != nullptr && !*rb)) {
            e = std::make_unique<ana::BoundConst>(st::bool_val(false));
        } else if (lb != nullptr) {  // 此处 lb 必为 true
            e = std::move(l.right);
        } else if (rb != nullptr) {  // 此处 rb 必为 true
            e = std::move(l.left);
        }
        return;
    }
    // LogicOp 仅 And/Or 两值, 余下为 Or
    if ((lb != nullptr && *lb) || (rb != nullptr && *rb)) {
        e = std::make_unique<ana::BoundConst>(st::bool_val(true));
    } else if (lb != nullptr) {  // 此处 lb 必为 false
        e = std::move(l.right);
    } else if (rb != nullptr) {  // 此处 rb 必为 false
        e = std::move(l.left);
    }
}

// NOT 消除: 双重否定取内层操作数, 比较/判空取反下放(三值逻辑下等价);
// 无规则命中(布尔列引用/逻辑节点等)保持原树, 调用方保证 e 为 Not 且操作数已折叠且非常量
void simplify_not(std::unique_ptr<ana::BoundExpr>& e)
{
    auto& n = static_cast<ana::BoundNot&>(*e);
    switch (n.operand->kind()) {
    case ana::BoundExprKind::Not:
        e = std::move(static_cast<ana::BoundNot&>(*n.operand).operand);
        return;
    case ana::BoundExprKind::Cmp: {
        auto& c = static_cast<ana::BoundCmp&>(*n.operand);
        c.op = negate_cmp(c.op);
        e = std::move(n.operand);
        return;
    }
    case ana::BoundExprKind::IsNull: {
        auto& i = static_cast<ana::BoundIsNull&>(*n.operand);
        i.negate = !i.negate;
        e = std::move(n.operand);
        return;
    }
    default:
        return;  // 其余节点不可吸收 NOT
    }
}

// 自比较消除: 弱布尔语境下两侧结构相等时, =/<=/>= 改写为 IS NOT NULL, <>/</> 折为
// false 常量; 未命中保持原树, 调用方保证 e 为 Cmp 且两子树已折叠且处于弱布尔语境
void simplify_cmp(std::unique_ptr<ana::BoundExpr>& e)
{
    auto& c = static_cast<ana::BoundCmp&>(*e);
    switch (c.op) {
    case CmpOp::Eq:
    case CmpOp::Le:
    case CmpOp::Ge:
        if (expr_equal(*c.left, *c.right)) {
            e = std::make_unique<ana::BoundIsNull>(std::move(c.left), true);
        }
        return;
    case CmpOp::Ne:
    case CmpOp::Lt:
    case CmpOp::Gt:
        if (expr_equal(*c.left, *c.right)) {
            e = std::make_unique<ana::BoundConst>(st::bool_val(false));
        }
        return;
    }
    // 不可达: 全部比较运算符已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "bool_simplify: 未知比较运算符");
}

}  // namespace pl
