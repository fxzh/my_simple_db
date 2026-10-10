// bool_simplify.cpp: 逻辑节点与 NOT 布尔化简实现
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

}  // namespace pl
