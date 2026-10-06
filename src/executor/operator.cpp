// operator.cpp: 迭代子算子实现: 顺序扫描/过滤/投影
#include "operator.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog.h"
#include "common/err.h"
#include "log/log.h"
#include "plan.h"
#include "expr_eval.h"

namespace exec {

namespace {

// 顺序扫描: open 时经 catalog 建扫描器, 扫描器析构自会释放页 pin
struct SeqScanOp : Operator {
    ct::Catalog& db;
    ct::TableRef table;
    std::unique_ptr<st::Scanner> scanner;

    SeqScanOp(ct::Catalog& db_, ct::TableRef table_) : db(db_), table(std::move(table_)) {}

    void open() override { scanner = db.scan(table); }
    bool next(st::Row* out) override { return scanner->next(out); }
    void close() override { scanner.reset(); }
};

// 空结果: 恒 0 行, 无外部资源
struct EmptyOp : Operator {
    void open() override {}
    bool next(st::Row*) override { return false; }
    void close() override {}
};

// 过滤: 谓词不满足的行不上抛
struct FilterOp : Operator {
    std::unique_ptr<Operator> child;
    const ana::BoundExpr& pred;

    FilterOp(std::unique_ptr<Operator> child_, const ana::BoundExpr& pred_)
        : child(std::move(child_)), pred(pred_) {}

    void open() override { child->open(); }
    bool next(st::Row* out) override
    {
        while (child->next(out)) {
            if (expr::where_match(pred, *out)) {
                return true;
            }
        }
        return false;
    }
    void close() override { child->close(); }
};

// 投影: 按展开的投影列对子行求值, 输出行保留源行 ref/rid
struct ProjectOp : Operator {
    std::unique_ptr<Operator> child;
    const std::vector<ana::ProjCol>& projs;

    ProjectOp(std::unique_ptr<Operator> child_, const std::vector<ana::ProjCol>& projs_)
        : child(std::move(child_)), projs(projs_) {}

    void open() override { child->open(); }
    bool next(st::Row* out) override
    {
        if (!child->next(out)) {
            return false;
        }
        std::vector<st::Value> vals;
        vals.reserve(projs.size());
        for (const ana::ProjCol& p : projs) {
            vals.push_back(p.expr != nullptr ? expr::to_st_value(expr::eval_row(*p.expr, *out))
                                             : out->values[p.col_idx]);
        }
        out->values = std::move(vals);
        return true;
    }
    void close() override { child->close(); }
};

}  // namespace

std::unique_ptr<Operator> make_operator(ct::Catalog& db, const pl::PlanNode& node)
{
    switch (node.kind()) {
    case pl::PlanKind::SeqScan: {
        const auto& p = static_cast<const pl::SeqScanPlan&>(node);
        return std::make_unique<SeqScanOp>(db, p.table);
    }
    case pl::PlanKind::Empty:
        return std::make_unique<EmptyOp>();
    case pl::PlanKind::Filter: {
        const auto& p = static_cast<const pl::FilterPlan&>(node);
        return std::make_unique<FilterOp>(make_operator(db, *p.child), *p.pred);
    }
    case pl::PlanKind::Project: {
        const auto& p = static_cast<const pl::ProjectPlan&>(node);
        return std::make_unique<ProjectOp>(make_operator(db, *p.child), p.projs);
    }
    default:
        break;
    }
    // 不可达: 有对应算子的计划种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 计划节点无对应算子");
}

}  // namespace exec
