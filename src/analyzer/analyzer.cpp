// analyzer.cpp: 绑定层实现: AST + catalog 元数据 → BoundStmt
#include "analyzer.h"

#include <cstdint>
#include <string>
#include <vector>

#include "ast.hh"
#include "catalog.h"
#include "common/err.h"
#include "log/log.h"

namespace ana {

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
    DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "未映射的列类型: {}", static_cast<int>(type));
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
                DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "长度非法: {}", *def.length);
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
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER, "列不存在: {}", id.name);
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
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

// 保留表名拦截: 元数据表禁止 drop/insert/delete
// (select 可查元数据, create 由存储层按表已存在拒绝)
void check_reserved_table(const std::string& name)
{
    if (name == ct::kTableMetaName || name == ct::kColumnMetaName) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::ANALYZER, "保留表名禁止使用: {}", name);
    }
}

// 绑定行结构: 列名定位表 + char 定长列标记
Schema build_schema(const st::TableMeta& meta)
{
    Schema schema;
    schema.char_col.reserve(meta.cols.size());
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        schema.cols.emplace(meta.cols[i].name, i);
        schema.char_col.push_back(meta.cols[i].type == st::ColType::Char);
    }
    return schema;
}

// 绑定 SELECT 投影: star 按表列展开, 输出列名取别名>列名>表达式文本
std::vector<ProjCol> build_projs(const SelectStmt& ss, const st::TableMeta& meta)
{
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
    return projs;
}

}  // namespace

std::unique_ptr<BoundStmt> analyze(ct::Catalog& db, const SQLStatement& stmt)
{
    switch (stmt.kind()) {
    case StmtKind::CreateTable: {
        const auto& cs = static_cast<const CreateTableStmt&>(stmt);
        auto b = std::make_unique<BoundCreateTable>();
        b->table = cs.table_name();
        convert_columns(cs.column_defs(), b->cols);
        return b;
    }
    case StmtKind::DropTable: {
        const auto& ds = static_cast<const DropTableStmt&>(stmt);
        check_reserved_table(ds.table_name());
        auto b = std::make_unique<BoundDropTable>();
        b->table = ds.table_name();
        return b;
    }
    case StmtKind::Insert: {
        const auto& is = static_cast<const InsertStmt&>(stmt);
        check_reserved_table(is.table_name());
        auto b = std::make_unique<BoundInsert>();
        b->table = is.table_name();
        b->values.reserve(is.values().size());
        for (const auto& v : is.values()) {
            b->values.push_back(v.get());
        }
        return b;
    }
    case StmtKind::Delete: {
        const auto& ds = static_cast<const DeleteStmt&>(stmt);
        check_reserved_table(ds.table_name());
        auto b = std::make_unique<BoundDelete>();
        b->table = ds.table_name();
        b->where = ds.where_expr();
        if (b->where != nullptr) {
            b->schema = build_schema(db.table_meta(ds.table_name()));
            validate_columns(*b->where, b->schema.cols);
        }
        return b;
    }
    case StmtKind::Select: {
        const auto& ss = static_cast<const SelectStmt&>(stmt);
        const st::TableMeta meta = db.table_meta(ss.table_name());
        auto b = std::make_unique<BoundSelect>();
        b->table = ss.table_name();
        b->schema = build_schema(meta);
        b->where = ss.where_expr();
        if (b->where != nullptr) {
            validate_columns(*b->where, b->schema.cols);
        }
        b->projs = build_projs(ss, meta);
        return b;
    }
    }
    // 不可达: 全部语句种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::ANALYZER, "analyzer: 未知语句种类");
}

}  // namespace ana
