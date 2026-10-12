// const_fold.cpp: 计划期常量折叠实现
#include "planner.h"

#include <memory>

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

// 求值结果转常量节点(常量子树不含列引用)
std::unique_ptr<ana::BoundExpr> to_fold_const(const st::Value& v)
{
    return std::make_unique<ana::BoundConst>(v);
}

// 节点是否 NULL 常量
bool is_null_const(const ana::BoundExpr& e)
{
    if (e.kind() != ana::BoundExprKind::Const) {
        return false;
    }
    return st::value_is_null(static_cast<const ana::BoundConst&>(e).value);
}

// 表达式折叠: 自底向上, 子节点折叠后均为常量则整节点求值替换(该子树必无列引用),
// 算术/比较任一侧为 NULL 常量即折为 NULL 常量, 逻辑与非节点随后做布尔化简;
// weak 标记弱布尔语境(值仅按真/非真消费, NULL 与 false 等价), 该语境下比较节点
// 另做自比较消除, 且仅 AND/OR 操作数延续该语境
void fold_expr(std::unique_ptr<ana::BoundExpr>& e, bool weak)
{
    switch (e->kind()) {
    case ana::BoundExprKind::Const:
    case ana::BoundExprKind::ColRef:
        return;  // 叶子节点
    case ana::BoundExprKind::Arith: {
        auto& a = static_cast<ana::BoundArith&>(*e);
        fold_expr(a.left, false);
        fold_expr(a.right, false);
        if (is_const(*a.left) && is_const(*a.right)) {
            e = to_fold_const(expr::eval_const(*e));
        } else if (is_null_const(*a.left) || is_null_const(*a.right)) {
            e = to_fold_const(st::null_val());  // 任一侧 NULL 常量, 结果恒 NULL
        }
        return;
    }
    case ana::BoundExprKind::Neg: {
        auto& n = static_cast<ana::BoundNeg&>(*e);
        fold_expr(n.operand, false);
        if (is_const(*n.operand)) {
            e = to_fold_const(expr::eval_const(*e));
        }
        return;
    }
    case ana::BoundExprKind::Cmp: {
        auto& c = static_cast<ana::BoundCmp&>(*e);
        fold_expr(c.left, false);
        fold_expr(c.right, false);
        if (is_const(*c.left) && is_const(*c.right)) {
            e = to_fold_const(expr::eval_const(*e));
        } else if (is_null_const(*c.left) || is_null_const(*c.right)) {
            e = to_fold_const(st::null_val());  // 任一侧 NULL 常量, 结果恒 UNKNOWN
        } else if (weak) {
            simplify_cmp(e);  // 弱布尔语境自比较消除, 未命中保持原树
        }
        return;
    }
    case ana::BoundExprKind::Logic: {
        auto& l = static_cast<ana::BoundLogic&>(*e);
        fold_expr(l.left, weak);  // AND/OR 操作数延续弱布尔语境
        fold_expr(l.right, weak);
        if (is_const(*l.left) && is_const(*l.right)) {
            e = to_fold_const(expr::eval_const(*e));
        } else {
            simplify_logic(e);  // 单侧常量 bool 布尔化简, 未命中保持原树
        }
        return;
    }
    case ana::BoundExprKind::Not: {
        auto& n = static_cast<ana::BoundNot&>(*e);
        fold_expr(n.operand, false);
        if (is_const(*n.operand)) {
            e = to_fold_const(expr::eval_const(*e));
        } else {
            simplify_not(e);  // 非常量操作数 NOT 消除, 未命中保持原树
            if (weak && e->kind() == ana::BoundExprKind::Cmp) {
                simplify_cmp(e);  // NOT 吸收把子树提升到弱语境位置, 补一次自比较消除
            }
        }
        return;
    }
    case ana::BoundExprKind::IsNull: {
        auto& i = static_cast<ana::BoundIsNull&>(*e);
        fold_expr(i.operand, false);
        if (is_const(*i.operand)) {
            e = to_fold_const(expr::eval_const(*e));
        }
        return;
    }
    case ana::BoundExprKind::FuncCall: {
        auto& f = static_cast<ana::BoundFuncCall&>(*e);
        bool all_const = true;
        for (std::unique_ptr<ana::BoundExpr>& a : f.args) {
            fold_expr(a, false);
            all_const = all_const && is_const(*a);
        }
        if (all_const) {
            e = to_fold_const(expr::eval_const(*e));
        }
        return;
    }
    case ana::BoundExprKind::Cast: {
        auto& c = static_cast<ana::BoundCast&>(*e);
        fold_expr(c.operand, false);
        if (is_const(*c.operand)) {
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
    case pl::PlanKind::IndexScan:
    case pl::PlanKind::DummyScan:
    case pl::PlanKind::Empty:
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
        fold_expr(f.pred, true);  // WHERE 谓词根为弱布尔语境
        fold_const(*f.child);
        return;
    }
    case pl::PlanKind::Fetch:
        fold_const(*static_cast<pl::FetchPlan&>(plan).child);
        return;
    case pl::PlanKind::Project: {
        auto& p = static_cast<pl::ProjectPlan&>(plan);
        for (ana::ProjCol& c : p.projs) {
            if (c.expr != nullptr) {  // star 展开列无表达式
                fold_expr(c.expr, false);
            }
        }
        fold_const(*p.child);
        return;
    }
    case pl::PlanKind::Sort: {
        auto& s = static_cast<pl::SortPlan&>(plan);
        if (!s.sort_on_output) {  // 输出列下标键无表达式
            for (ana::BoundOrderItem& o : s.orders) {
                fold_expr(o.expr, false);
            }
        }
        fold_const(*s.child);
        return;
    }
    case pl::PlanKind::Insert: {
        auto& ip = static_cast<pl::InsertPlan&>(plan);
        for (std::vector<std::unique_ptr<ana::BoundExpr>>& row : ip.rows) {
            for (std::unique_ptr<ana::BoundExpr>& v : row) {
                fold_expr(v, false);
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
            fold_expr(a.value, false);
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

// 计划优化入口: 在 build 产物上就地串接各优化 pass, 当前为常量折叠(含布尔化简与
// 自比较消除)、常量过滤器剪除与索引选择
void optimize(ct::Catalog& db, std::unique_ptr<PlanNode>& plan)
{
    fold_const(*plan);
    prune_filter(plan);
    select_index(db, plan);
}

}  // namespace pl
