// executor.cpp: 执行层实现: AST 转 catalog 调用
#include "executor.h"

#include <cstdint>
#include <string>
#include <vector>

#include "ast.hh"
#include "codec.h"
#include "common/err.h"
#include "log/log.h"
#include "catalog.h"
#include "expr_eval.h"

namespace exec {

namespace {

// 语法层类型枚举映射到存储层列类型
st::ColType map_col_type(DataType type)
{
    switch (type) {
    case DataType::Int: return st::ColType::Int;
    case DataType::BigInt: return st::ColType::BigInt;
    case DataType::Float: return st::ColType::Float;
    case DataType::Double: return st::ColType::Double;
    case DataType::Char: return st::ColType::Char;
    case DataType::VarChar: return st::ColType::VarChar;
    }
    DB_RAISE(db::ErrCode::InvalidType, LogModule::EXECUTOR, "未映射的列类型: {}", static_cast<int>(type));
}

// AST 列定义转存储层列规格; 显式长度非法时当场记录并抛出
void convert_columns(const std::vector<ColumnDef>& defs, std::vector<st::ColumnSpec>& cols)
{
    cols.reserve(defs.size());
    for (const ColumnDef& def : defs) {
        // 显式长度取值 1..65535; 未声明时 char 缺省 1, varchar 缺省 0(动态), 其余为 0
        uint16_t len = 0;
        if (def.length) {
            if (*def.length <= 0 || *def.length > UINT16_MAX) {
                DB_RAISE(db::ErrCode::InvalidType, LogModule::EXECUTOR, "长度非法: {}", *def.length);
            }
            len = static_cast<uint16_t>(*def.length);
        } else if (def.type == DataType::Char) {
            len = 1;
        }
        cols.emplace_back(st::ColumnSpec{def.name, map_col_type(def.type), len});
    }
}

// 编译期校验表达式引用的列均存在: 空表时也能报出未知列
void validate_columns(const Expr& expr, const ColMap& cols)
{
    switch (expr.kind()) {
    case ExprKind::Int:
    case ExprKind::Float:
    case ExprKind::String:
    case ExprKind::Null:
        return;
    case ExprKind::Identifier: {
        const auto& id = static_cast<const IdentifierExpr&>(expr);
        if (cols.find(id.name) == cols.end()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::EXECUTOR, "列不存在: {}", id.name);
        }
        return;
    }
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        validate_columns(*e.left, cols);
        validate_columns(*e.right, cols);
        return;
    }
    case ExprKind::UnaryOp:
        validate_columns(*static_cast<const UnaryOpExpr&>(expr).operand, cols);
        return;
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        validate_columns(*e.left, cols);
        validate_columns(*e.right, cols);
        return;
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        validate_columns(*e.left, cols);
        validate_columns(*e.right, cols);
        return;
    }
    case ExprKind::Not:
        validate_columns(*static_cast<const NotExpr&>(expr).operand, cols);
        return;
    case ExprKind::IsNull:
        validate_columns(*static_cast<const IsNullExpr&>(expr).operand, cols);
        return;
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 未知表达式节点");
}

// 命令标签结果(非结果集语句)
ExecResult tag_result(proto::CommandTag tag, uint64_t count)
{
    ExecResult r;
    r.tag = tag;
    r.count = count;
    return r;
}

// 保留表名拦截: 元数据表禁止 drop/insert/delete
// (select 可查元数据, create 由存储层按表已存在拒绝)
void check_reserved_table(const std::string& name)
{
    if (name == ct::kTableMetaName || name == ct::kColumnMetaName) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::EXECUTOR, "保留表名禁止使用: {}", name);
    }
}

// ==================== SELECT 执行 ====================

// 投影输出列: star 展开的原始列直接取行值, 其余按表达式逐行求值
struct ProjCol {
    const Expr* expr = nullptr;  // 为空表示 star 展开的原始列
    std::string name;            // 输出列名
    size_t col_idx = 0;          // star 列的行内下标
};

// 编译行上下文: 列名定位表 + char 定长列标记
RowCtx build_row_ctx(const st::TableMeta& meta)
{
    RowCtx ctx;
    ctx.char_col.reserve(meta.cols.size());
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        ctx.cols.emplace(meta.cols[i].name, i);
        ctx.char_col.push_back(meta.cols[i].type == st::ColType::Char);
    }
    return ctx;
}

// SELECT 执行: 编译投影(列定位/stars 展开/输出列名)与 WHERE 列校验后全表扫描逐行物化
ExecResult exec_select(ct::Catalog& db, const SelectStmt& ss)
{
    const st::TableMeta meta = db.table_meta(ss.table_name());
    const RowCtx ctx = build_row_ctx(meta);
    if (ss.where_expr() != nullptr) {
        validate_columns(*ss.where_expr(), ctx.cols);
    }

    // 编译期: 投影展开
    std::vector<ProjCol> projs;
    for (const SelectItem& item : ss.items()) {
        if (item.star) {
            for (size_t i = 0; i < meta.cols.size(); ++i) {
                projs.push_back(ProjCol{nullptr, meta.cols[i].name, i});
            }
            continue;
        }
        std::string name;
        if (!item.alias.empty()) {
            name = item.alias;
        } else if (item.expr->kind() == ExprKind::Identifier) {
            name = static_cast<const IdentifierExpr&>(*item.expr).name;
        } else {
            name = expr_to_string(*item.expr);
        }
        projs.push_back(ProjCol{item.expr.get(), std::move(name), 0});
    }

    ExecResult result;
    result.is_result_set = true;
    result.col_names.reserve(projs.size());
    for (const ProjCol& p : projs) {
        result.col_names.push_back(p.name);
    }

    // 物化期: 扫描一行求值一行, WHERE 不满足即跳过
    const Expr* where = ss.where_expr();
    std::unique_ptr<st::Scanner> scanner = db.scan(ss.table_name());
    st::Row row;
    while (scanner->next(&row)) {
        if (where != nullptr && !where_match(*where, ctx, row)) {
            continue;
        }
        std::vector<st::Value> out;
        out.reserve(projs.size());
        for (const ProjCol& p : projs) {
            out.push_back(p.expr != nullptr ? to_st_value(eval_row(*p.expr, ctx, row))
                                            : row.values[p.col_idx]);
        }
        result.rows.push_back(std::move(out));
    }
    return result;
}

// DELETE ... WHERE: 列校验后扫描收集满足条件的行引用, 再逐个物理删除, 返回实际删除行数
uint64_t exec_delete_where(ct::Catalog& db, const Expr& where, const std::string& table)
{
    const st::TableMeta meta = db.table_meta(table);
    const RowCtx ctx = build_row_ctx(meta);
    validate_columns(where, ctx.cols);
    std::vector<st::RowRef> refs;
    std::unique_ptr<st::Scanner> scanner = db.scan(table);
    st::Row row;
    while (scanner->next(&row)) {
        if (where_match(where, ctx, row)) {
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
    switch (stmt.kind()) {
    case StmtKind::CreateTable: {
        const auto& cs = static_cast<const CreateTableStmt&>(stmt);
        std::vector<st::ColumnSpec> cols;
        convert_columns(cs.column_defs(), cols);
        db.create_table(cs.table_name(), cols);
        return tag_result(proto::CommandTag::CreateTable, 0);
    }
    case StmtKind::DropTable: {
        const auto& ds = static_cast<const DropTableStmt&>(stmt);
        check_reserved_table(ds.table_name());
        db.drop_table(ds.table_name());
        return tag_result(proto::CommandTag::DropTable, 0);
    }
    case StmtKind::Insert: {
        const auto& is = static_cast<const InsertStmt&>(stmt);
        check_reserved_table(is.table_name());
        std::vector<st::Value> values;
        values.reserve(is.values().size());
        for (const auto& v : is.values()) {
            values.push_back(to_st_value(eval_const(*v)));
        }
        db.insert(is.table_name(), values);
        return tag_result(proto::CommandTag::Insert, 1);
    }
    case StmtKind::Delete: {
        const auto& ds = static_cast<const DeleteStmt&>(stmt);
        check_reserved_table(ds.table_name());
        const uint64_t n = ds.where_expr() != nullptr
                ? exec_delete_where(db, *ds.where_expr(), ds.table_name())
                : db.delete_all(ds.table_name());
        return tag_result(proto::CommandTag::Delete, n);
    }
    case StmtKind::Select: {
        const auto& ss = static_cast<const SelectStmt&>(stmt);
        return exec_select(db, ss);
    }
    }
    // 不可达: 全部语句种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::EXECUTOR, "executor: 未知语句种类");
}

}  // namespace exec