// const_fold.cpp: 计划期常量折叠实现
#include "planner.h"

#include <memory>
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

// 节点静态类型: 常量取值语义类型, 列/算术取绑定期固化域, 逻辑类节点为布尔
st::ColType static_type(const ana::BoundExpr& e)
{
    switch (e.kind()) {
    case ana::BoundExprKind::Const:
        return static_cast<const ana::BoundConst&>(e).value.type;
    case ana::BoundExprKind::ColRef:
        return static_cast<const ana::BoundColRef&>(e).type;
    case ana::BoundExprKind::Arith:
        return static_cast<const ana::BoundArith&>(e).type;
    case ana::BoundExprKind::Neg:
        return static_cast<const ana::BoundNeg&>(e).type;
    case ana::BoundExprKind::FuncCall:
        return static_cast<const ana::BoundFuncCall&>(e).type;
    case ana::BoundExprKind::Cmp:
    case ana::BoundExprKind::Logic:
    case ana::BoundExprKind::Not:
    case ana::BoundExprKind::IsNull:
        return st::ColType::Bool;
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "const_fold: 未知绑定表达式节点");
}

// Float 语境常量收窄: ctx 为 Float 域且 cst 为 Double 常量时, cst 就地重建为单精度装箱;
// 不可表示的有限值收窄为 ±inf, 关系语义不变
void narrow_float_const(const ana::BoundExpr& ctx, std::unique_ptr<ana::BoundExpr>& cst)
{
    if (static_type(ctx) != st::ColType::Float || cst->kind() != ana::BoundExprKind::Const) {
        return;
    }
    const double* d = std::get_if<double>(&static_cast<ana::BoundConst&>(*cst).value.box);
    if (d == nullptr) {
        return;
    }
    cst = std::make_unique<ana::BoundConst>(st::float_val(static_cast<float>(*d)));
}

// 表达式折叠: 自底向上, 子节点折叠后均为常量则整节点求值替换(该子树必无列引用),
// 算术/比较任一侧为 NULL 常量即折为 NULL 常量, 逻辑与非节点随后做布尔化简
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
        } else if (is_null_const(*a.left) || is_null_const(*a.right)) {
            e = to_fold_const(st::null_val());  // 任一侧 NULL 常量, 结果恒 NULL
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
        narrow_float_const(*c.left, c.right);
        narrow_float_const(*c.right, c.left);
        if (is_const(*c.left) && is_const(*c.right)) {
            e = to_fold_const(expr::eval_const(*e));
        } else if (is_null_const(*c.left) || is_null_const(*c.right)) {
            e = to_fold_const(st::null_val());  // 任一侧 NULL 常量, 结果恒 UNKNOWN
        }
        return;
    }
    case ana::BoundExprKind::Logic: {
        auto& l = static_cast<ana::BoundLogic&>(*e);
        fold_expr(l.left);
        fold_expr(l.right);
        if (is_const(*l.left) && is_const(*l.right)) {
            e = to_fold_const(expr::eval_const(*e));
        } else {
            simplify_logic(e);  // 单侧常量 bool 布尔化简, 未命中保持原树
        }
        return;
    }
    case ana::BoundExprKind::Not: {
        auto& n = static_cast<ana::BoundNot&>(*e);
        fold_expr(n.operand);
        if (is_const(*n.operand)) {
            e = to_fold_const(expr::eval_const(*e));
        } else {
            simplify_not(e);  // 非常量操作数 NOT 消除, 未命中保持原树
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
    case ana::BoundExprKind::FuncCall: {
        auto& f = static_cast<ana::BoundFuncCall&>(*e);
        for (std::unique_ptr<ana::BoundExpr>& a : f.args) {
            fold_expr(a);  // 仅折叠实参, 调用节点不折叠
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
        fold_expr(f.pred);
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
                fold_expr(c.expr);
            }
        }
        fold_const(*p.child);
        return;
    }
    case pl::PlanKind::Sort: {
        auto& s = static_cast<pl::SortPlan&>(plan);
        if (!s.sort_on_output) {  // 输出列下标键无表达式
            for (ana::BoundOrderItem& o : s.orders) {
                fold_expr(o.expr);
            }
        }
        fold_const(*s.child);
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

// 计划优化入口: 在 build 产物上就地串接各优化 pass, 当前为常量折叠(含布尔化简)、
// 常量过滤器剪除与索引选择
void optimize(ct::Catalog& db, std::unique_ptr<PlanNode>& plan)
{
    fold_const(*plan);
    prune_filter(plan);
    select_index(db, plan);
}

}  // namespace pl
