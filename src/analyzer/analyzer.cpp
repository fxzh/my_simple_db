// analyzer.cpp: 语义分析层实现: AST + catalog 元数据 → BoundStmt
#include "analyzer.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hh"
#include "bound_expr.h"
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

// 行结构(名字解析结果): 列名定位表 + 列静态类型 + char 定长列标记, 绑定表达式树时使用
using ColMap = std::unordered_map<std::string, size_t>;
struct Schema {
    ColMap cols;                         // 列名 → 行内下标
    std::vector<st::ColType> col_types;  // 列静态类型, 与行内下标对应
    std::vector<bool> char_col;          // char 定长列标记, 与行内下标对应
};

// 表达式静态类型: 推导规则与求值器行为逐点对齐, 绑定层放过的表达式执行层不因类型报错
enum class ExprType : uint8_t { Null, Int, Double, String, Bool };

std::unique_ptr<BoundExpr> bind_expr(const Expr& expr, const Schema* schema, ExprType& type);

// 数值类(Int/Double)判定
bool is_num_type(ExprType t)
{
    return t == ExprType::Int || t == ExprType::Double;
}

// 列静态类型映射: 整型归 Int, 浮点归 Double, 文本归 String
ExprType col_expr_type(st::ColType type)
{
    switch (type) {
    case st::ColType::Int:
    case st::ColType::BigInt: return ExprType::Int;
    case st::ColType::Float:
    case st::ColType::Double: return ExprType::Double;
    case st::ColType::Char:
    case st::ColType::VarChar: return ExprType::String;
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未映射的列类型");
}

// 绑定表达式: 递归建树并推导类型, 名字解析当场完成; schema 为空表示常量上下文(禁止引用列)
std::unique_ptr<BoundExpr> bind_expr(const Expr& expr, const Schema* schema, ExprType& type)
{
    switch (expr.kind()) {
    case ExprKind::Int:
        type = ExprType::Int;
        return std::make_unique<BoundConst>(static_cast<const IntExpr&>(expr).value);
    case ExprKind::Float:
        type = ExprType::Double;
        return std::make_unique<BoundConst>(static_cast<const FloatExpr&>(expr).value);
    case ExprKind::String:
        type = ExprType::String;
        return std::make_unique<BoundConst>(static_cast<const StringExpr&>(expr).value);
    case ExprKind::Null:
        type = ExprType::Null;
        return std::make_unique<BoundConst>(std::monostate{});
    case ExprKind::Identifier: {
        const auto& id = static_cast<const IdentifierExpr&>(expr);
        if (schema == nullptr) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "常量上下文不允许引用列: {}", id.name);
        }
        const auto it = schema->cols.find(id.name);
        if (it == schema->cols.end()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER, "列不存在: {}", id.name);
        }
        type = col_expr_type(schema->col_types[it->second]);
        return std::make_unique<BoundColRef>(it->second, schema->char_col[it->second]);
    }
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        ExprType lt;
        ExprType rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, schema, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, schema, rt);
        if ((!is_num_type(lt) && lt != ExprType::Null)
            || (!is_num_type(rt) && rt != ExprType::Null)) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "算术运算操作数不是数值");
        }
        if (lt == ExprType::Double || rt == ExprType::Double) {
            type = ExprType::Double;
        } else {
            type = ExprType::Int;
        }
        return std::make_unique<BoundArith>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        ExprType t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, schema, t);
        if (!is_num_type(t) && t != ExprType::Null) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "一元运算操作数不是数值");
        }
        type = t;
        if (e.op == '+') {
            return operand;  // 一元正号原值返回, 绑定期折为操作数本身
        }
        return std::make_unique<BoundNeg>(std::move(operand));
    }
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        ExprType lt;
        ExprType rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, schema, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, schema, rt);
        const bool same_num = is_num_type(lt) && is_num_type(rt);
        const bool same_str = lt == ExprType::String && rt == ExprType::String;
        if (lt != ExprType::Null && rt != ExprType::Null && !same_num && !same_str) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "比较运算两侧须同为数值或字符串");
        }
        type = ExprType::Bool;
        return std::make_unique<BoundCmp>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        ExprType lt;
        ExprType rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, schema, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, schema, rt);
        if ((lt != ExprType::Bool && lt != ExprType::Null)
            || (rt != ExprType::Bool && rt != ExprType::Null)) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "逻辑运算操作数不是布尔值");
        }
        type = ExprType::Bool;
        return std::make_unique<BoundLogic>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::Not: {
        const auto& e = static_cast<const NotExpr&>(expr);
        ExprType t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, schema, t);
        if (t != ExprType::Bool && t != ExprType::Null) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "逻辑运算操作数不是布尔值");
        }
        type = ExprType::Bool;
        return std::make_unique<BoundNot>(std::move(operand));
    }
    case ExprKind::IsNull: {
        const auto& e = static_cast<const IsNullExpr&>(expr);
        ExprType t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, schema, t);
        type = ExprType::Bool;
        return std::make_unique<BoundIsNull>(std::move(operand), e.negate);
    }
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

// 常量表达式求值结果是否可能为 NULL: 树中含 NULL 字面量即可能(IS NULL 结果必为布尔)
bool may_be_null(const Expr& expr)
{
    switch (expr.kind()) {
    case ExprKind::Null: return true;
    case ExprKind::Int:
    case ExprKind::Float:
    case ExprKind::String:
    case ExprKind::Identifier:
        return false;
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        return may_be_null(*e.left) || may_be_null(*e.right);
    }
    case ExprKind::UnaryOp:
        return may_be_null(*static_cast<const UnaryOpExpr&>(expr).operand);
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        return may_be_null(*e.left) || may_be_null(*e.right);
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        return may_be_null(*e.left) || may_be_null(*e.right);
    }
    case ExprKind::Not:
        return may_be_null(*static_cast<const NotExpr&>(expr).operand);
    case ExprKind::IsNull:
        return false;
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

// WHERE 条件绑定: 须为布尔(NULL 视为 UNKNOWN 放行), 返回绑定谓词树
std::unique_ptr<BoundExpr> bind_where(const Expr& where, const Schema& schema)
{
    ExprType t;
    std::unique_ptr<BoundExpr> pred = bind_expr(where, &schema, t);
    if (t != ExprType::Bool && t != ExprType::Null) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "WHERE 条件不是布尔表达式");
    }
    return pred;
}

// 值类型与列规格匹配: NULL 值放行(NOT NULL 已在前置检查拦截), 字面量范围/长度随类型一并校验
void check_value_type(const Expr& expr, ExprType t, const st::ColumnSpec& col)
{
    if (may_be_null(expr)) {
        return;
    }
    bool ok = false;
    switch (col.type) {
    case st::ColType::Int: {
        ok = t == ExprType::Int;
        // 字面量须在 int32 范围内, 折叠表达式的范围检查留待执行期
        if (ok && expr.kind() == ExprKind::Int) {
            const int64_t v = static_cast<const IntExpr&>(expr).value;
            ok = v >= INT32_MIN && v <= INT32_MAX;
        }
        break;
    }
    case st::ColType::BigInt:
        ok = t == ExprType::Int;
        break;
    case st::ColType::Double:
    case st::ColType::Float: {
        ok = t == ExprType::Double;
        // float 列字面量须可被 float 表示
        if (ok && col.type == st::ColType::Float && expr.kind() == ExprKind::Float) {
            const double d = static_cast<const FloatExpr&>(expr).value;
            ok = !std::isfinite(d) || std::isfinite(static_cast<float>(d));
        }
        break;
    }
    case st::ColType::Char:
        // 常量上下文的 String 必为字面量, 须不超声明长度
        ok = t == ExprType::String
             && static_cast<const StringExpr&>(expr).value.size() <= col.length;
        break;
    case st::ColType::VarChar:
        ok = t == ExprType::String
             && (col.length == 0
                 || static_cast<const StringExpr&>(expr).value.size() <= col.length);
        break;
    }
    if (!ok) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "值与列类型不匹配");
    }
}

// INSERT 值绑定: 推导/布尔/个数/NOT NULL/类型匹配, 检查顺序与原执行链一致, 文案不变
std::vector<std::unique_ptr<BoundExpr>> bind_insert_values(
    const std::vector<std::unique_ptr<Expr>>& values, const st::TableMeta& meta)
{
    std::vector<std::unique_ptr<BoundExpr>> bound;
    std::vector<ExprType> types;
    types.reserve(values.size());
    for (const auto& v : values) {
        ExprType t;
        bound.push_back(bind_expr(*v, nullptr, t));  // 常量上下文: 禁止引用列
        types.push_back(t);
    }
    for (size_t i = 0; i < values.size(); ++i) {
        if (types[i] == ExprType::Bool && !may_be_null(*values[i])) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "布尔值不可作为存储或输出值");
        }
    }
    if (values.size() != meta.cols.size()) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "值的数量与列数不符");
    }
    for (size_t i = 0; i < values.size(); ++i) {
        if (meta.cols[i].not_null && may_be_null(*values[i])) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "NOT NULL 列不允许 NULL: {}", meta.cols[i].name);
        }
    }
    for (size_t i = 0; i < values.size(); ++i) {
        check_value_type(*values[i], types[i], meta.cols[i]);
    }
    return bound;
}

// 保留表名拦截: 元数据表禁止 drop/insert/delete
// (select 可查元数据, create 由存储层按表已存在拒绝)
void check_reserved_table(const std::string& name)
{
    if (name == ct::kTableMetaName || name == ct::kColumnMetaName) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::ANALYZER, "保留表名禁止使用: {}", name);
    }
}

// 绑定行结构: 列名定位表 + 列静态类型 + char 定长列标记
Schema build_schema(const st::TableMeta& meta)
{
    Schema schema;
    schema.col_types.reserve(meta.cols.size());
    schema.char_col.reserve(meta.cols.size());
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        schema.cols.emplace(meta.cols[i].name, i);
        schema.col_types.push_back(meta.cols[i].type);
        schema.char_col.push_back(meta.cols[i].type == st::ColType::Char);
    }
    return schema;
}

// 绑定 SELECT 投影: star 按表列展开, 输出列名取别名>列名>表达式文本; 常量布尔不可投影
std::vector<ProjCol> build_projs(const SelectStmt& ss, const st::TableMeta& meta,
                                 const Schema& schema)
{
    std::vector<ProjCol> projs;
    for (const SelectItem& item : ss.items()) {
        if (item.star) {
            for (size_t i = 0; i < meta.cols.size(); ++i) {
                projs.push_back(ProjCol{nullptr, meta.cols[i].name, i});
            }
            continue;
        }
        ExprType t;
        std::unique_ptr<BoundExpr> e = bind_expr(*item.expr, &schema, t);
        if (t == ExprType::Bool && !may_be_null(*item.expr)) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "布尔值不可作为存储或输出值");
        }
        std::string name;
        if (!item.alias.empty()) {
            name = item.alias;
        } else if (item.expr->kind() == ExprKind::Identifier) {
            name = static_cast<const IdentifierExpr&>(*item.expr).name;
        } else {
            name = expr_to_string(*item.expr);
        }
        projs.push_back(ProjCol{std::move(e), std::move(name), 0});
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
        const st::TableMeta meta = db.table_meta(is.table_name());
        auto b = std::make_unique<BoundInsert>();
        b->table = is.table_name();
        b->values = bind_insert_values(is.values(), meta);
        return b;
    }
    case StmtKind::Delete: {
        const auto& ds = static_cast<const DeleteStmt&>(stmt);
        check_reserved_table(ds.table_name());
        auto b = std::make_unique<BoundDelete>();
        b->table = ds.table_name();
        if (ds.where_expr() != nullptr) {
            const Schema schema = build_schema(db.table_meta(ds.table_name()));
            b->where = bind_where(*ds.where_expr(), schema);
        }
        return b;
    }
    case StmtKind::Select: {
        const auto& ss = static_cast<const SelectStmt&>(stmt);
        const st::TableMeta meta = db.table_meta(ss.table_name());
        auto b = std::make_unique<BoundSelect>();
        b->table = ss.table_name();
        const Schema schema = build_schema(meta);
        if (ss.where_expr() != nullptr) {
            b->where = bind_where(*ss.where_expr(), schema);
        }
        b->projs = build_projs(ss, meta, schema);
        return b;
    }
    }
    // 不可达: 全部语句种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::ANALYZER, "analyzer: 未知语句种类");
}

}  // namespace ana
