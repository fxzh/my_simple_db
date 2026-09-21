// executor.cpp: 执行层实现: AST 转 storage 调用
#include "executor.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ast.hh"
#include "codec.h"
#include "common/err.h"
#include "log/log.h"
#include "storage.h"

namespace exec {

namespace {

// AST 列定义转存储层列规格; 类型名非法时当场记录并抛出
void convert_columns(const std::vector<ColumnDef>& defs, std::vector<st::ColumnSpec>& cols)
{
    cols.reserve(defs.size());
    for (const ColumnDef& def : defs) {
        st::ColType type;
        uint16_t len = 0;
        if (!st::parse_column_type(def.type, &type, &len)) {
            DB_RAISE(db::ErrCode::InvalidType, LogModule::EXECUTOR, "不支持的类型: {}", def.type);
        }
        cols.emplace_back(st::ColumnSpec{def.name, type, len});
    }
}

// ==================== 表达式求值 ====================

// 求值值域: 在存储值上扩展 bool; monostate 表示 NULL(条件上下文即 UNKNOWN)
using EvalValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

// 行上下文: 列名 → 行内下标, 每条语句编译一次
using ColMap = std::unordered_map<std::string, size_t>;

EvalValue eval_expr(const Expr& expr, const ColMap* cols, const st::Row* row);

// 数值转 double(整型提升)
double to_double(const EvalValue& v)
{
    if (const auto* d = std::get_if<double>(&v)) {
        return *d;
    }
    return static_cast<double>(std::get<int64_t>(v));
}

// 值是否数值(int64/double)
bool is_number(const EvalValue& v)
{
    return std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v);
}

// 一元数值运算: '+' 原值返回, '-' 取反
EvalValue eval_unary(char op, const Expr& operand, const ColMap* cols, const st::Row* row)
{
    const EvalValue v = eval_expr(operand, cols, row);
    if (!is_number(v)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "一元运算操作数不是数值");
    }
    if (op == '+') {
        return v;
    }
    if (const auto* d = std::get_if<double>(&v)) {
        return EvalValue{-*d};  // 有限值取反仍有限
    }
    const int64_t i = std::get<int64_t>(v);
    if (i == INT64_MIN) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型取反溢出");
    }
    return EvalValue{-i};
}

// 双目算术: 类型驱动提升(int/int 向零截断), 溢出/除零当场报错
EvalValue eval_binary(char op, const Expr& le, const Expr& re, const ColMap* cols, const st::Row* row)
{
    const EvalValue lv = eval_expr(le, cols, row);
    const EvalValue rv = eval_expr(re, cols, row);
    if (!is_number(lv) || !is_number(rv)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "算术运算操作数不是数值");
    }
    if (std::holds_alternative<double>(lv) || std::holds_alternative<double>(rv)) {
        double out = 0.0;
        switch (op) {
        case '+': out = to_double(lv) + to_double(rv); break;
        case '-': out = to_double(lv) - to_double(rv); break;
        case '*': out = to_double(lv) * to_double(rv); break;
        case '/': out = to_double(lv) / to_double(rv); break;
        }
        if (!std::isfinite(out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "浮点运算溢出或除零");
        }
        return EvalValue{out};
    }
    const int64_t l = std::get<int64_t>(lv);
    const int64_t r = std::get<int64_t>(rv);
    int64_t out = 0;
    switch (op) {
    case '+':
        if (__builtin_add_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型加法溢出");
        }
        break;
    case '-':
        if (__builtin_sub_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型减法溢出");
        }
        break;
    case '*':
        if (__builtin_mul_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型乘法溢出");
        }
        break;
    case '/':
        if (r == 0) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型除零");
        }
        if (l == INT64_MIN && r == -1) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型除法溢出");
        }
        out = l / r;  // 向零截断
        break;
    }
    return EvalValue{out};
}

// 统一求值入口: cols/row 同时为空表示常量上下文(标识符不可用)
EvalValue eval_expr(const Expr& expr, const ColMap* cols, const st::Row* row)
{
    switch (expr.kind()) {
    case ExprKind::Int:
        return EvalValue{static_cast<const IntExpr&>(expr).value};
    case ExprKind::Float: {
        const double d = static_cast<const FloatExpr&>(expr).value;
        if (!std::isfinite(d)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "浮点字面量超出可表示范围");
        }
        return EvalValue{d};
    }
    case ExprKind::String:
        return EvalValue{static_cast<const StringExpr&>(expr).value};
    case ExprKind::Identifier: {
        const auto& id = static_cast<const IdentifierExpr&>(expr);
        if (cols == nullptr) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "常量上下文不允许引用列: {}",
                     id.name);
        }
        const auto it = cols->find(id.name);
        if (it == cols->end()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::EXECUTOR, "列不存在: {}", id.name);
        }
        const st::Value& raw = row->values[it->second];
        return std::visit([](const auto& val) -> EvalValue { return val; }, raw);
    }
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        return eval_binary(e.op, *e.left, *e.right, cols, row);
    }
    case ExprKind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        return eval_unary(e.op, *e.operand, cols, row);
    }
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 未知表达式节点");
}

// 常量上下文求值(INSERT VALUES 等无行上下文的场景)
EvalValue eval_const(const Expr& e)
{
    return eval_expr(e, nullptr, nullptr);
}

// 行上下文求值(SELECT 投影与过滤)
EvalValue eval_row(const Expr& e, const ColMap& cols, const st::Row& row)
{
    return eval_expr(e, &cols, &row);
}

// 求值结果转存储/输出值: bool 不允许作为结果值, 其余原样(monostate 即 NULL)
st::Value to_st_value(const EvalValue& v)
{
    if (std::holds_alternative<bool>(v)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "布尔值不可作为存储或输出值");
    }
    return std::visit([](const auto& val) -> st::Value { return val; }, v);
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
    if (name == st::kTableMetaName || name == st::kColumnMetaName) {
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

// SELECT 执行: 编译投影(列定位/stars 展开/输出列名)后全表扫描逐行物化
ExecResult exec_select(st::Database& db, const SelectStmt& ss)
{
    const st::TableMeta meta = db.table_meta(ss.table_name());

    // 编译期: 列定位表与投影展开
    ColMap cols;
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        cols.emplace(meta.cols[i].name, i);
    }
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

    // 物化期: 扫描一行求值一行
    std::unique_ptr<st::Scanner> scanner = db.scan(ss.table_name());
    st::Row row;
    while (scanner->next(&row)) {
        std::vector<st::Value> out;
        out.reserve(projs.size());
        for (const ProjCol& p : projs) {
            out.push_back(p.expr != nullptr ? to_st_value(eval_row(*p.expr, cols, row))
                                            : row.values[p.col_idx]);
        }
        result.rows.push_back(std::move(out));
    }
    return result;
}

}  // namespace

ExecResult execute(st::Database& db, const SQLStatement& stmt)
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
        const uint64_t n = db.delete_all(ds.table_name());
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