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
#include "explain.h"
#include "operator.h"

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

// SELECT 执行: 建算子并 open, 计划树随行携带(算子引用其中数据), 行由调用方经 next 逐行拉取
ExecResult run_select(ct::Catalog& db, std::unique_ptr<pl::PlanNode> plan)
{
    const auto& pp = static_cast<const pl::ProjectPlan&>(*plan);
    ExecResult result;
    result.is_result_set = true;
    result.col_names.reserve(pp.projs.size());
    for (const ana::ProjCol& p : pp.projs) {
        result.col_names.push_back(p.name);
    }
    result.plan = std::move(plan);
    result.stream = make_operator(db, pp);
    result.stream->open();
    return result;
}

// DELETE ... WHERE: 抽干删除子树算子收集行引用, 再逐个物理删除, 返回实际删除行数
uint64_t run_delete_where(ct::Catalog& db, const pl::DeletePlan& dp)
{
    std::vector<st::RowRef> refs;
    std::unique_ptr<Operator> op = make_operator(db, *dp.child);
    op->open();
    st::Row row;
    while (op->next(&row)) {
        refs.push_back(row.ref);
    }
    op->close();
    uint64_t deleted = 0;
    for (const st::RowRef& ref : refs) {
        deleted += db.delete_by_ref(ref);
    }
    return deleted;
}

// UPDATE: 抽干子树算子收集旧行(引用+值), 逐行求值赋值生成新行后批量墓碑+追加,
// 返回更新行数; 堆追加式, 扫描与修改必须两阶段否则会扫到本语句新追加的行
uint64_t run_update(ct::Catalog& db, const pl::UpdatePlan& up)
{
    std::vector<st::Row> olds;
    std::unique_ptr<Operator> op = make_operator(db, *up.child);
    op->open();
    st::Row row;
    while (op->next(&row)) {
        olds.push_back(std::move(row));
    }
    op->close();
    std::vector<ct::RowUpdate> rows;
    rows.reserve(olds.size());
    for (const st::Row& old : olds) {
        std::vector<st::Value> vals = old.values;
        for (const ana::BoundUpdateItem& a : up.assigns) {
            vals[a.col_idx] = expr::to_st_value(expr::eval_row(*a.value, old));
        }
        rows.push_back(ct::RowUpdate{old.ref, std::move(vals)});
    }
    return db.update_rows(up.table, rows);
}

}  // namespace

ExecResult execute(ct::Catalog& db, const SQLStatement& stmt, const std::string& current_schema)
{
    // 绑定期: 名字解析/类型映射/投影展开
    std::unique_ptr<ana::BoundStmt> bound = ana::analyze(db, stmt, current_schema);
    // 计划期: 绑定语句转计划节点树
    std::unique_ptr<pl::PlanNode> plan = pl::build(*bound);
    // 计划期优化: 就地运行优化 pass(当前为常量折叠), 常量运算错误(溢出/除零)在此报错
    pl::optimize(*plan);
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
    case pl::PlanKind::CreateSchema: {
        const auto& p = static_cast<const pl::CreateSchemaPlan&>(*plan);
        db.create_schema(p.schema);
        return tag_result(proto::CommandTag::CreateSchema, 0);
    }
    case pl::PlanKind::DropSchema: {
        const auto& p = static_cast<const pl::DropSchemaPlan&>(*plan);
        db.drop_schema(p.schema);
        return tag_result(proto::CommandTag::DropSchema, 0);
    }
    case pl::PlanKind::CreateIndex: {
        const auto& p = static_cast<const pl::CreateIndexPlan&>(*plan);
        db.create_index(p.table, p.index, p.col_ordinal);
        return tag_result(proto::CommandTag::CreateIndex, 0);
    }
    case pl::PlanKind::DropIndex: {
        const auto& p = static_cast<const pl::DropIndexPlan&>(*plan);
        db.drop_index(p.table, p.index);
        return tag_result(proto::CommandTag::DropIndex, 0);
    }
    case pl::PlanKind::Insert: {
        const auto& p = static_cast<const pl::InsertPlan&>(*plan);
        for (const auto& plan_row : p.rows) {
            std::vector<st::Value> values;
            values.reserve(plan_row.size());
            for (const auto& v : plan_row) {
                values.push_back(expr::to_st_value(expr::eval_const(*v)));
            }
            db.insert(p.table, values);
        }
        return tag_result(proto::CommandTag::Insert, p.rows.size());
    }
    case pl::PlanKind::Delete: {
        const auto& p = static_cast<const pl::DeletePlan&>(*plan);
        const uint64_t n = p.child != nullptr ? run_delete_where(db, p) : db.delete_all(p.table);
        return tag_result(proto::CommandTag::Delete, n);
    }
    case pl::PlanKind::Update: {
        const auto& p = static_cast<const pl::UpdatePlan&>(*plan);
        return tag_result(proto::CommandTag::Update, run_update(db, p));
    }
    case pl::PlanKind::Project:
        return run_select(db, std::move(plan));
    case pl::PlanKind::Explain:
        return run_explain(db, static_cast<const pl::ExplainPlan&>(*plan));
    case pl::PlanKind::Set: {
        const auto& p = static_cast<const pl::SetPlan&>(*plan);
        switch (p.var) {
        case ana::SetVar::TableId:
            db.set_bootstrap_table_id(p.value);
            break;
        }
        return tag_result(proto::CommandTag::Set, 0);
    }
    default:
        break;  // SeqScan/Filter 不作为根计划出现
    }
    // 不可达: 全部根计划种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::EXECUTOR, "executor: 未知计划种类");
}

}  // namespace exec