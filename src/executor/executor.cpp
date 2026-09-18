// executor.cpp: 执行层实现: AST 转 storage 调用
#include "executor.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "ast.hh"
#include "codec.h"
#include "common/error.h"
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

st::Value eval_const_expr(const Expr& expr);

// 数值转 double(整型提升)
double to_double(const st::Value& v)
{
    if (const auto* d = std::get_if<double>(&v)) {
        return *d;
    }
    return static_cast<double>(std::get<int64_t>(v));
}

// 一元数值运算: '+' 原值返回, '-' 取反
st::Value eval_unary(char op, const Expr& operand)
{
    const st::Value v = eval_const_expr(operand);
    if (std::holds_alternative<std::string>(v)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "一元运算操作数不是数值");
    }
    if (op == '+') {
        return v;
    }
    if (const auto* d = std::get_if<double>(&v)) {
        return st::Value{-*d};  // 有限值取反仍有限
    }
    const int64_t i = std::get<int64_t>(v);
    if (i == INT64_MIN) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型取反溢出");
    }
    return st::Value{-i};
}

// 双目算术: 类型驱动提升(int/int 向零截断), 溢出/除零当场报错
st::Value eval_binary(char op, const Expr& le, const Expr& re)
{
    const st::Value lv = eval_const_expr(le);
    const st::Value rv = eval_const_expr(re);
    if (std::holds_alternative<std::string>(lv) || std::holds_alternative<std::string>(rv)) {
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
        return st::Value{out};
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
    return st::Value{out};
}

// 常量折叠: VALUES 上下文求值表达式为单值, 非常量或越界当场报错
st::Value eval_const_expr(const Expr& expr)
{
    switch (expr.kind()) {
    case ExprKind::Int:
        return st::Value{static_cast<const IntExpr&>(expr).value};
    case ExprKind::Float: {
        const double d = static_cast<const FloatExpr&>(expr).value;
        if (!std::isfinite(d)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "浮点字面量超出可表示范围");
        }
        return st::Value{d};
    }
    case ExprKind::String:
        return st::Value{static_cast<const StringExpr&>(expr).value};
    case ExprKind::Identifier:
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "VALUES 中不允许引用列: {}",
                 static_cast<const IdentifierExpr&>(expr).name);
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        return eval_binary(e.op, *e.left, *e.right);
    }
    case ExprKind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        return eval_unary(e.op, *e.operand);
    }
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 未知表达式节点");
}

}  // namespace

std::string execute(st::Database& db, const SQLStatement& stmt)
{
    switch (stmt.kind()) {
    case StmtKind::CreateTable: {
        const auto& cs = static_cast<const CreateTableStmt&>(stmt);
        std::vector<st::ColumnSpec> cols;
        convert_columns(cs.column_defs(), cols);
        db.create_table(cs.table_name(), cols);
        return "OK";
    }
    case StmtKind::DropTable: {
        const auto& ds = static_cast<const DropTableStmt&>(stmt);
        db.drop_table(ds.table_name());
        return "OK";
    }
    case StmtKind::Insert: {
        const auto& is = static_cast<const InsertStmt&>(stmt);
        std::vector<st::Value> values;
        values.reserve(is.values().size());
        for (const auto& v : is.values()) {
            values.push_back(eval_const_expr(*v));
        }
        db.insert(is.table_name(), values);
        return "OK";
    }
    case StmtKind::Delete: {
        const auto& ds = static_cast<const DeleteStmt&>(stmt);
        const size_t n = db.delete_all(ds.table_name());
        return "OK (删除 " + std::to_string(n) + " 行)";
    }
    case StmtKind::Select:
        DB_RAISE(db::ErrCode::UnknownStmt, LogModule::EXECUTOR, "executor: select 语句暂不支持");
        return "";
    }
    // 不可达: 全部语句种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::EXECUTOR, "executor: 未知语句种类");
    return "";
}

}  // namespace exec