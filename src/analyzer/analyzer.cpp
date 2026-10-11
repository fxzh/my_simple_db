// analyzer.cpp: 语义分析: 语句主分发与 DDL/SET 类语句绑定
#include "analyzer.h"

#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "analyzer_expr.h"
#include "analyzer_insert.h"
#include "analyzer_select.h"
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
    case DataType::Bool: return st::ColType::Bool;
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
        cols.emplace_back(st::ColumnSpec{def.name, map_col_type(def.type), len, def.not_null});
    }
}

// system schema 拦截: 一切建改删语句拒绝(select 可查, 其他 schema 同名表不受限)
void check_reserved_table(const ct::TableRef& table)
{
    if (table.schema == ct::kSystemSchemaName) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::ANALYZER, "system schema 禁止修改: {}",
                 table_ref_to_string(table));
    }
}

// AST 限定名转 catalog 表引用: 未限定名填入会话 current_schema
ct::TableRef to_table_ref(const QualifiedName& name, const std::string& current_schema)
{
    if (name.schema.empty()) {
        return {current_schema, name.name};
    }
    return {name.schema, name.name};
}

}  // namespace

std::unique_ptr<BoundStmt> analyze(ct::Catalog& db, const SQLStatement& stmt,
                                   const std::string& current_schema)
{
    switch (stmt.kind()) {
    case StmtKind::CreateTable: {
        const auto& cs = static_cast<const CreateTableStmt&>(stmt);
        const ct::TableRef table = to_table_ref(cs.table_name(), current_schema);
        // bootstrap 会话自举建 system 元数据表, 放行
        if (!db.bootstrap_mode()) {
            check_reserved_table(table);
        }
        auto b = std::make_unique<BoundCreateTable>();
        b->table = table;
        convert_columns(cs.column_defs(), b->cols);
        return b;
    }
    case StmtKind::DropTable: {
        const auto& ds = static_cast<const DropTableStmt&>(stmt);
        const ct::TableRef table = to_table_ref(ds.table_name(), current_schema);
        check_reserved_table(table);
        auto b = std::make_unique<BoundDropTable>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        return b;
    }
    case StmtKind::CreateSchema: {
        const auto& cs = static_cast<const CreateSchemaStmt&>(stmt);
        auto b = std::make_unique<BoundCreateSchema>();
        b->schema = cs.schema_name();
        return b;
    }
    case StmtKind::DropSchema: {
        const auto& ds = static_cast<const DropSchemaStmt&>(stmt);
        auto b = std::make_unique<BoundDropSchema>();
        b->schema = ds.schema_name();
        return b;
    }
    case StmtKind::CreateIndex: {
        const auto& cs = static_cast<const CreateIndexStmt&>(stmt);
        const ct::TableRef table = to_table_ref(cs.table_name(), current_schema);
        check_reserved_table(table);
        st::TableMeta meta = db.table_meta(table);
        size_t ordinal = meta.cols.size();
        for (size_t i = 0; i < meta.cols.size(); ++i) {
            if (meta.cols[i].name == cs.column_name()) {
                ordinal = i;
                break;
            }
        }
        if (ordinal == meta.cols.size()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER, "列不存在: {}",
                     cs.column_name());
        }
        const st::ColType type = meta.cols[ordinal].type;
        if (type != st::ColType::Int && type != st::ColType::BigInt && type != st::ColType::Float
            && type != st::ColType::Double && type != st::ColType::Bool) {
            DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "索引列类型不支持: {}",
                     cs.column_name());
        }
        auto b = std::make_unique<BoundCreateIndex>();
        b->table = ct::TableHandle{std::move(meta), table_ref_to_string(table)};
        b->index = cs.index_name();
        b->col_ordinal = static_cast<uint16_t>(ordinal);
        return b;
    }
    case StmtKind::DropIndex: {
        const auto& ds = static_cast<const DropIndexStmt&>(stmt);
        const ct::TableRef table = to_table_ref(ds.table_name(), current_schema);
        check_reserved_table(table);
        auto b = std::make_unique<BoundDropIndex>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        b->index = ds.index_name();
        return b;
    }
    case StmtKind::Insert: {
        const auto& is = static_cast<const InsertStmt&>(stmt);
        const ct::TableRef table = to_table_ref(is.table_name(), current_schema);
        // bootstrap 会话向 system.db_func 灌内建函数行, 放行
        if (!db.bootstrap_mode()) {
            check_reserved_table(table);
        }
        st::TableMeta meta = db.table_meta(table);
        const InsertTargets targets = resolve_insert_targets(is.columns(), meta);
        auto b = std::make_unique<BoundInsert>();
        b->rows.reserve(is.rows().size());
        for (const auto& row : is.rows()) {
            b->rows.push_back(bind_insert_row(row, targets, meta, db));
        }
        b->table = ct::TableHandle{std::move(meta), table_ref_to_string(table)};
        return b;
    }
    case StmtKind::Delete: {
        const auto& ds = static_cast<const DeleteStmt&>(stmt);
        const ct::TableRef table = to_table_ref(ds.table_name(), current_schema);
        check_reserved_table(table);
        auto b = std::make_unique<BoundDelete>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        if (ds.where_expr() != nullptr) {
            b->where = bind_where(*ds.where_expr(), build_schema(b->table.meta), db);
        }
        return b;
    }
    case StmtKind::Update: {
        const auto& us = static_cast<const UpdateStmt&>(stmt);
        const ct::TableRef table = to_table_ref(us.table_name(), current_schema);
        check_reserved_table(table);
        auto b = std::make_unique<BoundUpdate>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        const Schema schema = build_schema(b->table.meta);
        b->assigns.reserve(us.assignments().size());
        for (const UpdateItem& item : us.assignments()) {
            const auto it = schema.cols.find(item.column);
            if (it == schema.cols.end()) {
                DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER, "列不存在: {}",
                         item.column);
            }
            for (const BoundUpdateItem& a : b->assigns) {
                if (a.col_idx == it->second) {
                    DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                             "更新列清单中列重复: {}", item.column);
                }
            }
            BoundUpdateItem a;
            a.col_idx = it->second;
            st::ColType t;
            // 行上下文: 右值基于旧行求值, 赋值间互不可见
            a.value = bind_expr(*item.value, &schema, db, t);
            const st::ColumnSpec& col = b->table.meta.cols[it->second];
            if (has_col_ref(*item.value)) {
                check_update_type(t, col);
            } else {
                // 纯常量右值: 与插入同构的 NOT NULL/长度/范围检查
                if (col.not_null && may_be_null(*item.value)) {
                    DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                             "NOT NULL 列不允许 NULL: {}", col.name);
                }
                check_value_type(*item.value, t, col);
            }
            b->assigns.push_back(std::move(a));
        }
        if (us.where_expr() != nullptr) {
            b->where = bind_where(*us.where_expr(), schema, db);
        }
        return b;
    }
    case StmtKind::Select: {
        const auto& ss = static_cast<const SelectStmt&>(stmt);
        const ct::TableRef table = to_table_ref(ss.table_name(), current_schema);
        auto b = std::make_unique<BoundSelect>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        const Schema schema = build_schema(b->table.meta);
        if (ss.where_expr() != nullptr) {
            b->where = bind_where(*ss.where_expr(), schema, db);
        }
        ProjBind pb = build_projs(ss, b->table.meta, schema, db);
        if (!ss.orders().empty()) {
            b->orders = bind_orders(ss.orders(), pb, schema, b->sort_on_output, db);
        }
        b->projs = std::move(pb.projs);
        return b;
    }
    case StmtKind::SelectNoFrom: {
        const auto& ss = static_cast<const SelectNoFromStmt&>(stmt);
        auto b = std::make_unique<BoundSelectNoFrom>();
        const Schema schema;  // 空行结构: 无列可解析, 列引用按列不存在报
        if (ss.where_expr() != nullptr) {
            b->where = bind_where(*ss.where_expr(), schema, db);
        }
        ProjBind pb = build_projs_no_from(ss, schema, db);
        if (!ss.orders().empty()) {
            b->orders = bind_orders(ss.orders(), pb, schema, b->sort_on_output, db);
        }
        b->projs = std::move(pb.projs);
        return b;
    }
    case StmtKind::Explain: {
        const auto& es = static_cast<const ExplainStmt&>(stmt);
        auto b = std::make_unique<BoundExplain>();
        b->inner = analyze(db, es.inner(), current_schema);
        return b;
    }
    case StmtKind::Set: {
        const auto& ss = static_cast<const SetStmt&>(stmt);
        if (!db.bootstrap_mode()) {
            DB_RAISE(db::ErrCode::NotImplemented, LogModule::ANALYZER, "SET 仅 bootstrap 模式可用");
        }
        auto b = std::make_unique<BoundSet>();
        if (ss.var_name() == "table_id") {
            b->var = SetVar::TableId;
        } else {
            DB_RAISE(db::ErrCode::UnknownVar, LogModule::ANALYZER, "未知 SET 变量: {}",
                     ss.var_name());
        }
        // 值文本转 uint64, 非十进制非负整数即报错(bootstrap.sql 为受控文件)
        uint64_t value = 0;
        const char* begin = ss.value().data();
        const char* end = begin + ss.value().size();
        const auto conv = std::from_chars(begin, end, value);
        if (conv.ec != std::errc{} || conv.ptr != end) {
            DB_RAISE(db::ErrCode::InvalidVarValue, LogModule::ANALYZER, "table_id 值非法: {}",
                     ss.value());
        }
        b->value = value;
        return b;
    }
    case StmtKind::Begin:
    case StmtKind::Commit:
    case StmtKind::Rollback:
        // 事务控制语句在会话层短路处理, 不进语义分析
        break;
    }
    // 不可达: 全部语句种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::ANALYZER, "analyzer: 未知语句种类");
}

}  // namespace ana
