// operator.cpp: 迭代子算子实现: 顺序扫描/过滤/投影/排序
#include "operator.h"

#include <algorithm>
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

// 顺序扫描: open 时按语义分析层携带的表元数据建扫描器, 扫描器析构自会释放页 pin
struct SeqScanOp : Operator {
    ct::Catalog& db;
    st::TableMeta table;
    std::unique_ptr<st::Scanner> scanner;

    SeqScanOp(ct::Catalog& db_, st::TableMeta table_) : db(db_), table(std::move(table_)) {}

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

// 单行扫描: 无 FROM 的 SELECT 行源, open 后恰吐一行空行
struct DummyScanOp : Operator {
    bool done = false;

    void open() override { done = false; }
    bool next(st::Row* out) override
    {
        if (done) {
            return false;
        }
        done = true;
        *out = st::Row{};
        return true;
    }
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
            vals.push_back(p.expr != nullptr ? expr::eval_row(*p.expr, *out)
                                             : out->values[p.col_idx]);
        }
        out->values = std::move(vals);
        return true;
    }
    void close() override { child->close(); }
};

// 排序: open 时物化子行并预计算键值, 排序后顺序吐出; 同键行相对次序不保证
struct SortOp : Operator {
    using RowKeys = std::pair<st::Row, std::vector<st::Value>>;

    std::unique_ptr<Operator> child;
    const pl::SortPlan& plan;
    std::vector<RowKeys> rows;  // 物化行 + 预计算键值
    size_t pos = 0;

    SortOp(std::unique_ptr<Operator> child_, const pl::SortPlan& plan_)
        : child(std::move(child_)), plan(plan_) {}

    // 键序列比较: 逐键三向比较, desc 键取反, 全相等视为相等
    static bool key_less(const RowKeys& l, const RowKeys& r,
                         const std::vector<ana::BoundOrderItem>& orders)
    {
        for (size_t i = 0; i < orders.size(); ++i) {
            int c = expr::value_cmp(l.second[i], r.second[i]);
            if (orders[i].desc) {
                c = -c;
            }
            if (c != 0) {
                return c < 0;
            }
        }
        return false;
    }

    void open() override
    {
        child->open();
        rows.clear();
        st::Row row;
        while (child->next(&row)) {
            std::vector<st::Value> keys;
            keys.reserve(plan.orders.size());
            for (const ana::BoundOrderItem& o : plan.orders) {
                keys.push_back(plan.sort_on_output ? row.values[o.out_idx]
                                                   : expr::eval_row(*o.expr, row));
            }
            rows.emplace_back(std::move(row), std::move(keys));
            row = st::Row{};
        }
        std::sort(rows.begin(), rows.end(),
                  [this](const RowKeys& l, const RowKeys& r) { return key_less(l, r, plan.orders); });
        pos = 0;
    }
    bool next(st::Row* out) override
    {
        if (pos >= rows.size()) {
            return false;
        }
        *out = rows[pos++].first;
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
        return std::make_unique<SeqScanOp>(db, p.table.meta);
    }
    case pl::PlanKind::DummyScan:
        return std::make_unique<DummyScanOp>();
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
    case pl::PlanKind::Sort: {
        const auto& p = static_cast<const pl::SortPlan&>(node);
        return std::make_unique<SortOp>(make_operator(db, *p.child), p);
    }
    default:
        break;
    }
    // 不可达: 有对应算子的计划种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 计划节点无对应算子");
}

}  // namespace exec
