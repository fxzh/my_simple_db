// filter_prune.cpp: 常量过滤器剪除实现
#include "planner.h"

#include <memory>
#include <variant>

#include "bound_expr.h"
#include "common/err.h"
#include "log/log.h"

namespace pl {

namespace {

// 常量谓词的过滤语义: bool 常量取其值, NULL 常量按 WHERE 语义恒不满足
enum class ConstPred {
    None, AlwaysTrue, AlwaysFalse,
};

ConstPred classify_pred(const ana::BoundExpr& pred)
{
    if (pred.kind() != ana::BoundExprKind::Const) {
        return ConstPred::None;
    }
    const auto& v = static_cast<const ana::BoundConst&>(pred).value;
    if (const bool* b = std::get_if<bool>(&v.box)) {
        return *b ? ConstPred::AlwaysTrue : ConstPred::AlwaysFalse;
    }
    if (std::holds_alternative<std::monostate>(v.box)) {
        return ConstPred::AlwaysFalse;
    }
    // 语义层保证 WHERE 常量谓词仅 bool/NULL
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "filter_prune: 常量谓词不是布尔");
}

// 剪除子树行源的表名: SeqScan 取限定表名, DummyScan 无 FROM 留空
std::string scan_table(const PlanNode& node)
{
    switch (node.kind()) {
    case PlanKind::SeqScan:
        return static_cast<const SeqScanPlan&>(node).table.display;
    case PlanKind::DummyScan:
        return {};
    default:
        break;
    }
    // 不可达: Filter 的 child 按构造恒为扫描节点
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "filter_prune: 剪除子树行源非扫描节点");
}

}  // namespace

// 常量过滤器剪除: 先剪子树, 恒真 Filter 以子节点替换, 恒不满足 Filter 整棵子树剪成空结果节点
void prune_filter(std::unique_ptr<PlanNode>& plan)
{
    switch (plan->kind()) {
    case PlanKind::Empty:
    case PlanKind::SeqScan:
    case PlanKind::DummyScan:
    case PlanKind::Insert:
    case PlanKind::CreateTable:
    case PlanKind::DropTable:
    case PlanKind::CreateSchema:
    case PlanKind::DropSchema:
    case PlanKind::CreateIndex:
    case PlanKind::DropIndex:
    case PlanKind::Set:
        return;  // 不含过滤子树
    case PlanKind::Filter: {
        auto& f = static_cast<FilterPlan&>(*plan);
        prune_filter(f.child);
        switch (classify_pred(*f.pred)) {
        case ConstPred::None:
            return;
        case ConstPred::AlwaysTrue:
            plan = std::move(f.child);
            return;
        case ConstPred::AlwaysFalse: {
            auto empty = std::make_unique<EmptyPlan>();
            empty->table = scan_table(*f.child);
            plan = std::move(empty);
            return;
        }
        }
        return;
    }
    case PlanKind::Project:
        prune_filter(static_cast<ProjectPlan&>(*plan).child);
        return;
    case PlanKind::Sort:
        prune_filter(static_cast<SortPlan&>(*plan).child);
        return;
    case PlanKind::Delete: {
        auto& d = static_cast<DeletePlan&>(*plan);
        if (d.child != nullptr) {  // child 为空表示全表删除
            prune_filter(d.child);
        }
        return;
    }
    case PlanKind::Update:
        prune_filter(static_cast<UpdatePlan&>(*plan).child);
        return;
    case PlanKind::Explain:
        prune_filter(static_cast<ExplainPlan&>(*plan).child);
        return;
    }
    // 不可达: 全部计划种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "filter_prune: 未知计划种类");
}

}  // namespace pl
