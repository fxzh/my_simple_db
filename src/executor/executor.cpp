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

// SELECT 执行: 拉取投影算子逐行物化
ExecResult run_select(ct::Catalog& db, const pl::ProjectPlan& pp)
{
    ExecResult result;
    result.is_result_set = true;
    result.col_names.reserve(pp.projs.size());
    for (const ana::ProjCol& p : pp.projs) {
        result.col_names.push_back(p.name);
    }

    // 物化期: 逐行拉取算子树, 投影求值在投影算子内完成
    std::unique_ptr<Operator> op = make_operator(db, pp);
    op->open();
    st::Row row;
    while (op->next(&row)) {
        result.rows.push_back(std::move(row.values));
    }
    op->close();
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
        // 逐行求值并写盘: 求值期错误(溢出/除零)在执行中报错, 已写入行保留
        for (const auto& plan_row : p.rows) {
            std::vector<st::Value> values;
            values.reserve(plan_row.size());
            for (const auto& v : plan_row) {
                values.push_back(to_st_value(eval_const(*v)));
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
    case pl::PlanKind::Project:
        return run_select(db, static_cast<const pl::ProjectPlan&>(*plan));
    default:
        break;  // SeqScan/Filter 不作为根计划出现
    }
    // 不可达: 全部根计划种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::EXECUTOR, "executor: 未知计划种类");
}

}  // namespace exec