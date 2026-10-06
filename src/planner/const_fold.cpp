// const_fold.cpp: 计划期常量折叠实现
#include "planner.h"

#include <memory>
#include <type_traits>
#include <variant>

#include "bound_expr.h"
#include "common/err.h"
#include "expr_eval.h"
#include "log/log.h"

namespace pl {

namespace {

// 节点是否常量
bool is_const(const ana::BoundExpr& e)
{
    return e.kind() == ana::BoundExprKind::Const;
}

// 求值结果转常量节点: StrVal 剥离 char 定长标记(常量子树不含列引用)
std::unique_ptr<ana::BoundExpr> to_fold_const(const expr::EvalValue& v)
{
    return std::visit(
        [](const auto& val) -> std::unique_ptr<ana::BoundExpr> {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, expr::StrVal>) {
                return std::make_unique<ana::BoundConst>(val.text);
            } else {
                return std::make_unique<ana::BoundConst>(val);
            }
        },
        v);
}

// 表达式折叠: 自底向上, 子节点折叠后均为常量则整节点求值替换(该子树必无列引用)
void fold_expr(std::unique_ptr<ana::BoundExpr>& e)
{
    switch (e->kind()) {
    case ana::BoundExprKind::Const:
    case ana::BoundExprKind::ColRef:
        return;  // 叶子节点
    case ana::BoundExprKind::Arith: {
        auto& a = static_cast<ana::BoundArith&>(*e);
        fold_expr(a.left);
        fold_expr(a.right);
        if (is_const(*a.left) && is_const(*a.right)) {
            e = to_fold_const(expr::eval_const(*e));
        }
        return;
    }
    case ana::BoundExprKind::Neg: {
        auto& n = static_cast<ana::BoundNeg&>(*e);
        fold_expr(n.operand);
        if (is_const(*n.operand)) {
            e = to_fold_const(expr::eval_const(*e));
        }
        return;
    }
    case ana::BoundExprKind::Cmp: {
        auto& c = static_cast<ana::BoundCmp&>(*e);
        fold_expr(c.left);
        fold_expr(c.right);
        if (is_const(*c.left) && is_const(*c.right)) {
            e = to_fold_const(expr::eval_const(*e));
        }
        return;
    }
    case ana::BoundExprKind::Logic: {
        auto& l = static_cast<ana::BoundLogic&>(*e);
        fold_expr(l.left);
        fold_expr(l.right);
        if (is_const(*l.left) && is_const(*l.right)) {
            e = to_fold_const(expr::eval_const(*e));
        }
        return;
    }
    case ana::BoundExprKind::Not: {
        auto& n = static_cast<ana::BoundNot&>(*e);
        fold_expr(n.operand);
        if (is_const(*n.operand)) {
            e = to_fold_const(expr::eval_const(*e));
        }
        return;
    }
    case ana::BoundExprKind::IsNull: {
        auto& i = static_cast<ana::BoundIsNull&>(*e);
        fold_expr(i.operand);
        if (is_const(*i.operand)) {
            e = to_fold_const(expr::eval_const(*e));
        }
        return;
    }
    }
    // 不可达: 全部绑定表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "const_fold: 未知绑定表达式节点");
}

void fold_const(pl::PlanNode& plan)
{
    switch (plan.kind()) {
    case pl::PlanKind::SeqScan:
    case pl::PlanKind::CreateTable:
    case pl::PlanKind::DropTable:
    case pl::PlanKind::CreateSchema:
    case pl::PlanKind::DropSchema:
    case pl::PlanKind::CreateIndex:
    case pl::PlanKind::DropIndex:
    case pl::PlanKind::Set:
        return;  // 不持有绑定表达式
    case pl::PlanKind::Filter: {
        auto& f = static_cast<pl::FilterPlan&>(plan);
        fold_expr(f.pred);
        fold_const(*f.child);
        return;
    }
    case pl::PlanKind::Project: {
        auto& p = static_cast<pl::ProjectPlan&>(plan);
        for (ana::ProjCol& c : p.projs) {
            if (c.expr != nullptr) {  // star 展开列无表达式
                fold_expr(c.expr);
            }
        }
        fold_const(*p.child);
        return;
    }
    case pl::PlanKind::Insert: {
        auto& ip = static_cast<pl::InsertPlan&>(plan);
        for (std::vector<std::unique_ptr<ana::BoundExpr>>& row : ip.rows) {
            for (std::unique_ptr<ana::BoundExpr>& v : row) {
                fold_expr(v);
            }
        }
        return;
    }
    case pl::PlanKind::Delete: {
        auto& d = static_cast<pl::DeletePlan&>(plan);
        if (d.child != nullptr) {  // child 为空表示全表删除
            fold_const(*d.child);
        }
        return;
    }
    case pl::PlanKind::Update: {
        auto& u = static_cast<pl::UpdatePlan&>(plan);
        for (ana::BoundUpdateItem& a : u.assigns) {
            fold_expr(a.value);
        }
        fold_const(*u.child);
        return;
    }
    case pl::PlanKind::Explain:
        fold_const(*static_cast<pl::ExplainPlan&>(plan).child);
        return;
    }
    // 不可达: 全部计划种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "const_fold: 未知计划种类");
}

}  // namespace

// 计划优化入口: 在 build 产物上就地串接各优化 pass, 当前仅常量折叠
void optimize(PlanNode& plan)
{
    fold_const(plan);
}

}  // namespace pl
