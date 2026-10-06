// bool_simplify.cpp: 逻辑节点布尔化简实现
#include "planner.h"

#include <memory>
#include <variant>

#include "bound_expr.h"

namespace pl {

namespace {

// 节点为常量 bool 时返回其值指针, 其余(含 NULL 常量与非常量节点)返回 nullptr
const bool* const_bool(const ana::BoundExpr& e)
{
    if (e.kind() != ana::BoundExprKind::Const) {
        return nullptr;
    }
    return std::get_if<bool>(&static_cast<const ana::BoundConst&>(e).value);
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
            e = std::make_unique<ana::BoundConst>(false);
        } else if (lb != nullptr) {  // 此处 lb 必为 true
            e = std::move(l.right);
        } else if (rb != nullptr) {  // 此处 rb 必为 true
            e = std::move(l.left);
        }
        return;
    }
    // LogicOp 仅 And/Or 两值, 余下为 Or
    if ((lb != nullptr && *lb) || (rb != nullptr && *rb)) {
        e = std::make_unique<ana::BoundConst>(true);
    } else if (lb != nullptr) {  // 此处 lb 必为 false
        e = std::move(l.right);
    } else if (rb != nullptr) {  // 此处 rb 必为 false
        e = std::move(l.left);
    }
}

}  // namespace pl
