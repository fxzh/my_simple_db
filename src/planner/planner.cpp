// planner.cpp: 计划层实现: BoundStmt → PlanNode 树
#include "planner.h"

#include <memory>
#include <utility>

#include "common/err.h"
#include "log/log.h"

namespace pl {

namespace {

// SELECT → Project(Filter(SeqScan)), 无 WHERE 省 Filter
std::unique_ptr<PlanNode> build_select(ana::BoundSelect& bs)
{
    auto scan = std::make_unique<SeqScanPlan>();
    scan->table = bs.table;

    std::unique_ptr<PlanNode> input = std::move(scan);
    if (bs.where != nullptr) {
        auto filter = std::make_unique<FilterPlan>();
        filter->pred = bs.where;
        filter->schema = bs.schema;
        filter->child = std::move(input);
        input = std::move(filter);
    }

    auto project = std::make_unique<ProjectPlan>();
    project->projs = std::move(bs.projs);
    project->schema = std::move(bs.schema);
    project->child = std::move(input);
    return project;
}

}  // namespace

std::unique_ptr<PlanNode> build(ana::BoundStmt& bound)
{
    switch (bound.kind()) {
    case ana::BoundKind::CreateTable: {
        auto& bs = static_cast<ana::BoundCreateTable&>(bound);
        auto p = std::make_unique<CreateTablePlan>();
        p->table = bs.table;
        p->cols = std::move(bs.cols);
        return p;
    }
    case ana::BoundKind::DropTable: {
        const auto& bs = static_cast<const ana::BoundDropTable&>(bound);
        auto p = std::make_unique<DropTablePlan>();
        p->table = bs.table;
        return p;
    }
    case ana::BoundKind::Insert: {
        auto& bs = static_cast<ana::BoundInsert&>(bound);
        auto p = std::make_unique<InsertPlan>();
        p->table = bs.table;
        p->values = std::move(bs.values);
        return p;
    }
    case ana::BoundKind::Delete: {
        auto& bs = static_cast<ana::BoundDelete&>(bound);
        auto p = std::make_unique<DeletePlan>();
        p->table = bs.table;
        if (bs.where != nullptr) {
            auto scan = std::make_unique<SeqScanPlan>();
            scan->table = bs.table;
            auto filter = std::make_unique<FilterPlan>();
            filter->pred = bs.where;
            filter->schema = std::move(bs.schema);
            filter->child = std::move(scan);
            p->child = std::move(filter);
        }
        return p;
    }
    case ana::BoundKind::Select:
        return build_select(static_cast<ana::BoundSelect&>(bound));
    }
    // 不可达: 全部绑定语句种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "planner: 未知绑定语句种类");
}

}  // namespace pl
