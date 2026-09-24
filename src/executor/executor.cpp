// executor.cpp: 执行层实现: 绑定语句转 catalog 调用
#include "executor.h"

#include <cstdint>
#include <string>
#include <vector>

#include "ast.hh"
#include "codec.h"
#include "common/err.h"
#include "log/log.h"
#include "catalog.h"
#include "analyzer.h"
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

// SELECT 执行: 按绑定结果(投影已展开/WHERE 已校验)全表扫描逐行物化
ExecResult run_select(ct::Catalog& db, const ana::BoundSelect& bs)
{
    ExecResult result;
    result.is_result_set = true;
    result.col_names.reserve(bs.projs.size());
    for (const ana::ProjCol& p : bs.projs) {
        result.col_names.push_back(p.name);
    }

    // 物化期: 扫描一行求值一行, WHERE 不满足即跳过
    std::unique_ptr<st::Scanner> scanner = db.scan(bs.table);
    st::Row row;
    while (scanner->next(&row)) {
        if (bs.where != nullptr && !where_match(*bs.where, bs.schema, row)) {
            continue;
        }
        std::vector<st::Value> out;
        out.reserve(bs.projs.size());
        for (const ana::ProjCol& p : bs.projs) {
            out.push_back(p.expr != nullptr ? to_st_value(eval_row(*p.expr, bs.schema, row))
                                            : row.values[p.col_idx]);
        }
        result.rows.push_back(std::move(out));
    }
    return result;
}

// DELETE ... WHERE: 扫描收集满足条件的行引用, 再逐个物理删除, 返回实际删除行数
uint64_t run_delete_where(ct::Catalog& db, const ana::BoundDelete& bs)
{
    std::vector<st::RowRef> refs;
    std::unique_ptr<st::Scanner> scanner = db.scan(bs.table);
    st::Row row;
    while (scanner->next(&row)) {
        if (where_match(*bs.where, bs.schema, row)) {
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
    switch (bound->kind()) {
    case ana::BoundKind::CreateTable: {
        const auto& bs = static_cast<const ana::BoundCreateTable&>(*bound);
        db.create_table(bs.table, bs.cols);
        return tag_result(proto::CommandTag::CreateTable, 0);
    }
    case ana::BoundKind::DropTable: {
        const auto& bs = static_cast<const ana::BoundDropTable&>(*bound);
        db.drop_table(bs.table);
        return tag_result(proto::CommandTag::DropTable, 0);
    }
    case ana::BoundKind::Insert: {
        const auto& bs = static_cast<const ana::BoundInsert&>(*bound);
        std::vector<st::Value> values;
        values.reserve(bs.values.size());
        for (const Expr* v : bs.values) {
            values.push_back(to_st_value(eval_const(*v)));
        }
        db.insert(bs.table, values);
        return tag_result(proto::CommandTag::Insert, 1);
    }
    case ana::BoundKind::Delete: {
        const auto& bs = static_cast<const ana::BoundDelete&>(*bound);
        const uint64_t n = bs.where != nullptr
                ? run_delete_where(db, bs)
                : db.delete_all(bs.table);
        return tag_result(proto::CommandTag::Delete, n);
    }
    case ana::BoundKind::Select:
        return run_select(db, static_cast<const ana::BoundSelect&>(*bound));
    }
    // 不可达: 全部语句种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::EXECUTOR, "executor: 未知绑定语句种类");
}

}  // namespace exec