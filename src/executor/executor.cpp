// executor.cpp: 执行层实现: 计划节点树转 catalog 调用
#include "executor.h"

#include <cstdint>
#include <string>
#include <vector>

#include "ast.hh"
#include "common/err.h"
#include "log/log.h"
#include "catalog.h"
#include "analyzer.h"
#include "planner.h"
#include "expr_eval.h"

namespace exec {

namespace {

// 命令标签结果(非结果集语句)
ExecResult tag_result(proto::CommandTag tag, uint64_t count)
{
    ExecResult r;
    r.tag = tag;
    r.count = count;
    return r;
}

// ==================== SELECT 执行 ====================

// SELECT 执行: 按投影计划(Project[Filter[SeqScan]] 或 Project[SeqScan])全表扫描逐行物化
ExecResult run_select(ct::Catalog& db, const pl::ProjectPlan& pp)
{
    ExecResult result;
    result.is_result_set = true;
    result.col_names.reserve(pp.projs.size());
    for (const ana::ProjCol& p : pp.projs) {
        result.col_names.push_back(p.name);
    }

    // 解构输入计划: 过滤节点可选, 其下必为扫描节点
    const pl::PlanNode* input = pp.child.get();
    const pl::FilterPlan* filter = nullptr;
    if (input->kind() == pl::PlanKind::Filter) {
        filter = &static_cast<const pl::FilterPlan&>(*input);
        input = filter->child.get();
    }
    if (input->kind() != pl::PlanKind::SeqScan) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 投影计划的输入非扫描");
    }
    const auto& scan = static_cast<const pl::SeqScanPlan&>(*input);

    // 物化期: 扫描一行求值一行, WHERE 不满足即跳过
    std::unique_ptr<st::Scanner> scanner = db.scan(scan.table);
    st::Row row;
    while (scanner->next(&row)) {
        if (filter != nullptr && !where_match(*filter->pred, filter->schema, row)) {
            continue;
        }
        std::vector<st::Value> out;
        out.reserve(pp.projs.size());
        for (const ana::ProjCol& p : pp.projs) {
            out.push_back(p.expr != nullptr ? to_st_value(eval_row(*p.expr, pp.schema, row))
                                            : row.values[p.col_idx]);
        }
        result.rows.push_back(std::move(out));
    }
    return result;
}

// DELETE ... WHERE: 抽干过滤计划收集行引用, 再逐个物理删除, 返回实际删除行数
uint64_t run_delete_where(ct::Catalog& db, const pl::DeletePlan& dp)
{
    // 删除计划的子树形态: Filter(SeqScan)
    if (dp.child->kind() != pl::PlanKind::Filter) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 删除计划的子节点非过滤");
    }
    const auto& filter = static_cast<const pl::FilterPlan&>(*dp.child);
    if (filter.child->kind() != pl::PlanKind::SeqScan) {
        DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 过滤计划的子节点非扫描");
    }
    const auto& scan = static_cast<const pl::SeqScanPlan&>(*filter.child);
    std::vector<st::RowRef> refs;
    std::unique_ptr<st::Scanner> scanner = db.scan(scan.table);
    st::Row row;
    while (scanner->next(&row)) {
        if (where_match(*filter.pred, filter.schema, row)) {
            refs.push_back(row.ref);
        }
    }
    uint64_t deleted = 0;
    for (const st::RowRef& ref : refs) {
        deleted += db.delete_by_ref(ref);
    }
    return deleted;
}

}  // namespace

ExecResult execute(ct::Catalog& db, const SQLStatement& stmt)
{
    // 绑定期: 名字解析/类型映射/投影展开
    std::unique_ptr<ana::BoundStmt> bound = ana::analyze(db, stmt);
    // 计划期: 绑定语句转计划节点树
    std::unique_ptr<pl::PlanNode> plan = pl::build(*bound);
    switch (plan->kind()) {
    case pl::PlanKind::CreateTable: {
        const auto& p = static_cast<const pl::CreateTablePlan&>(*plan);
        db.create_table(p.table, p.cols);
        return tag_result(proto::CommandTag::CreateTable, 0);
    }
    case pl::PlanKind::DropTable: {
        const auto& p = static_cast<const pl::DropTablePlan&>(*plan);
        db.drop_table(p.table);
        return tag_result(proto::CommandTag::DropTable, 0);
    }
    case pl::PlanKind::Insert: {
        const auto& p = static_cast<const pl::InsertPlan&>(*plan);
        std::vector<st::Value> values;
        values.reserve(p.values.size());
        for (const Expr* v : p.values) {
            values.push_back(to_st_value(eval_const(*v)));
        }
        db.insert(p.table, values);
        return tag_result(proto::CommandTag::Insert, 1);
    }
    case pl::PlanKind::Delete: {
        const auto& p = static_cast<const pl::DeletePlan&>(*plan);
        const uint64_t n = p.child != nullptr ? run_delete_where(db, p) : db.delete_all(p.table);
        return tag_result(proto::CommandTag::Delete, n);
    }
    case pl::PlanKind::Project:
        return run_select(db, static_cast<const pl::ProjectPlan&>(*plan));
    default:
        break;  // SeqScan/Filter 不作为根计划出现
    }
    // 不可达: 全部根计划种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::EXECUTOR, "executor: 未知计划种类");
}

}  // namespace exec