// ast_print.hxx: AST 渲染函数: 各节点 print 的类外定义与 SQL 文本反解析(*_to_string)
// AST 节点只在 parser.y 中构造, 其编译单元必须包含本文件,
// 否则虚表引用的 print 定义无法解析
#ifndef PARSER_AST_PRINT_HXX
#define PARSER_AST_PRINT_HXX

#include "ast.hh"

#include <cstddef>
#include <format>
#include <iostream>
#include <string>

// ==================== 名称/类型/运算符转文本 ====================

// 限定名转文本: 供打印与日志使用
inline std::string qualified_to_string(const QualifiedName& q)
{
    return q.schema.empty() ? q.name : q.schema + "." + q.name;
}

// 类型名转文本, 带 (长度) 或 (精度,标度) 后缀: 供打印与日志使用
inline std::string type_to_string(const std::string& type, std::optional<long long> length,
                                  std::optional<long long> scale)
{
    if (!length) {
        return type;
    }
    if (scale) {
        return std::format("{}({},{})", type, *length, *scale);
    }
    return std::format("{}({})", type, *length);
}

// 比较运算符转文本: 供打印与反解析使用
inline std::string cmp_to_string(CmpOp op)
{
    switch (op) {
    case CmpOp::Eq: return "=";
    case CmpOp::Ne: return "<>";
    case CmpOp::Lt: return "<";
    case CmpOp::Le: return "<=";
    case CmpOp::Gt: return ">";
    case CmpOp::Ge: return ">=";
    }
    return "";
}

// ==================== 表达式渲染 ====================

inline void IntExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Int: " << text << std::endl;
}

inline void FloatExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Float: " << text << std::endl;
}

inline void DecimalExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Decimal: " << text << std::endl;
}

inline void StringExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "String: " << value << std::endl;
}

inline void NullExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Null" << std::endl;
}

inline void BoolExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Bool: "
       << (value ? "true" : "false") << std::endl;
}

inline void IdentifierExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Identifier: " << name << std::endl;
}

inline void BinaryOpExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "BinaryOp: " << op << std::endl;
    left->print(os, indent + 4);
    right->print(os, indent + 4);
}

inline void UnaryOpExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "UnaryOp: " << op << std::endl;
    operand->print(os, indent + 4);
}

inline void CompareExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Compare: "
       << cmp_to_string(op) << std::endl;
    left->print(os, indent + 4);
    right->print(os, indent + 4);
}

inline void LogicExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Logic: "
       << (op == LogicOp::And ? "AND" : "OR") << std::endl;
    left->print(os, indent + 4);
    right->print(os, indent + 4);
}

inline void NotExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Not" << std::endl;
    operand->print(os, indent + 4);
}

inline void IsNullExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "IsNull" << (negate ? " not" : "")
       << std::endl;
    operand->print(os, indent + 4);
}

inline void BetweenExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Between" << std::endl;
    expr->print(os, indent + 4);
    lo->print(os, indent + 4);
    hi->print(os, indent + 4);
}

inline void FuncCallExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "FuncCall: " << name << std::endl;
    for (const auto& arg : args) {
        arg->print(os, indent + 4);
    }
}

inline void CastExpr::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Cast: "
       << type_to_string(target.type, target.length, target.scale) << std::endl;
    operand->print(os, indent + 4);
}

// 表达式反解析 SQL 文本: 供输出列命名与日志使用
inline std::string expr_to_string(const Expr& e)
{
    switch (e.kind()) {
    case ExprKind::Int:
        return static_cast<const IntExpr&>(e).text;
    case ExprKind::Float:
        return static_cast<const FloatExpr&>(e).text;
    case ExprKind::Decimal:
        return static_cast<const DecimalExpr&>(e).text;
    case ExprKind::String:
        return "'" + static_cast<const StringExpr&>(e).value + "'";
    case ExprKind::Null:
        return "NULL";
    case ExprKind::Bool:
        return static_cast<const BoolExpr&>(e).value ? "true" : "false";
    case ExprKind::Identifier:
        return static_cast<const IdentifierExpr&>(e).name;
    case ExprKind::BinaryOp: {
        const auto& b = static_cast<const BinaryOpExpr&>(e);
        return "(" + expr_to_string(*b.left) + " " + b.op + " " + expr_to_string(*b.right) + ")";
    }
    case ExprKind::UnaryOp: {
        const auto& u = static_cast<const UnaryOpExpr&>(e);
        return "(" + std::string(1, u.op) + expr_to_string(*u.operand) + ")";
    }
    case ExprKind::Compare: {
        const auto& c = static_cast<const CompareExpr&>(e);
        return "(" + expr_to_string(*c.left) + " " + cmp_to_string(c.op) + " "
               + expr_to_string(*c.right) + ")";
    }
    case ExprKind::Logic: {
        const auto& l = static_cast<const LogicExpr&>(e);
        return "(" + expr_to_string(*l.left) + (l.op == LogicOp::And ? " AND " : " OR ")
               + expr_to_string(*l.right) + ")";
    }
    case ExprKind::Not:
        return "(NOT " + expr_to_string(*static_cast<const NotExpr&>(e).operand) + ")";
    case ExprKind::IsNull: {
        const auto& i = static_cast<const IsNullExpr&>(e);
        return "(" + expr_to_string(*i.operand) + (i.negate ? " IS NOT NULL" : " IS NULL") + ")";
    }
    case ExprKind::Between: {
        const auto& b = static_cast<const BetweenExpr&>(e);
        return "(" + expr_to_string(*b.expr) + " BETWEEN " + expr_to_string(*b.lo) + " AND "
               + expr_to_string(*b.hi) + ")";
    }
    case ExprKind::FuncCall: {
        const auto& f = static_cast<const FuncCallExpr&>(e);
        std::string s = f.name + "(";
        for (size_t i = 0; i < f.args.size(); ++i) {
            if (i > 0) {
                s += ", ";
            }
            s += expr_to_string(*f.args[i]);
        }
        return s + ")";
    }
    case ExprKind::Cast: {
        const auto& c = static_cast<const CastExpr&>(e);
        return "CAST(" + expr_to_string(*c.operand) + " AS "
               + type_to_string(c.target.type, c.target.length, c.target.scale) + ")";
    }
    }
    return "";
}

// ==================== 语句渲染 ====================

inline void CreateTableStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "CreateTable: "
       << qualified_to_string(table) << std::endl;
    for (const auto& col : columns) {
        os << std::string(static_cast<std::size_t>(indent + 4), ' ') << col.name << " "
           << type_to_string(col.type, col.length, col.scale);
        if (col.not_null) {
            os << " NOT NULL";
        }
        os << std::endl;
    }
}

inline void DropTableStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "DropTable: "
       << qualified_to_string(table) << std::endl;
}

inline void CreateSchemaStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "CreateSchema: " << schema
       << std::endl;
}

inline void DropSchemaStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "DropSchema: " << schema
       << std::endl;
}

inline void CreateIndexStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "CreateIndex: " << index
       << " ON " << qualified_to_string(table) << " (" << column << ")" << std::endl;
}

inline void DropIndexStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "DropIndex: " << index
       << " ON " << qualified_to_string(table) << std::endl;
}

inline void InsertStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "InsertInto: "
       << qualified_to_string(table) << std::endl;
    for (const std::string& col : columns_) {
        os << std::string(static_cast<std::size_t>(indent + 4), ' ') << "Column: " << col << std::endl;
    }
    for (const auto& row : rows_) {
        for (const auto& e : row) {
            e->print(os, indent + 4);
        }
    }
}

inline void DeleteStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "DeleteFrom: "
       << qualified_to_string(table) << std::endl;
    if (where_) {
        where_->print(os, indent + 4);
    }
}

inline void UpdateStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Update: "
       << qualified_to_string(table) << std::endl;
    for (const UpdateItem& item : assignments_) {
        os << std::string(static_cast<std::size_t>(indent + 4), ' ') << "Assign: "
           << item.column << std::endl;
        item.value->print(os, indent + 8);
    }
    if (where_) {
        where_->print(os, indent + 4);
    }
}

inline void SelectStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Select: "
       << qualified_to_string(table_) << std::endl;
    for (const SelectItem& item : items_) {
        if (item.star) {
            os << std::string(static_cast<std::size_t>(indent + 4), ' ') << "*" << std::endl;
        } else {
            item.expr->print(os, indent + 4);
        }
    }
    if (where_) {
        where_->print(os, indent + 4);
    }
    for (const OrderItem& item : orders_) {
        os << std::string(static_cast<std::size_t>(indent + 4), ' ') << "Order: "
           << expr_to_string(*item.expr) << (item.desc ? " DESC" : " ASC") << std::endl;
    }
}

inline void SelectNoFromStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "SelectNoFrom" << std::endl;
    for (const SelectItem& item : items_) {
        if (item.star) {
            os << std::string(static_cast<std::size_t>(indent + 4), ' ') << "*" << std::endl;
        } else {
            item.expr->print(os, indent + 4);
        }
    }
    if (where_) {
        where_->print(os, indent + 4);
    }
    for (const OrderItem& item : orders_) {
        os << std::string(static_cast<std::size_t>(indent + 4), ' ') << "Order: "
           << expr_to_string(*item.expr) << (item.desc ? " DESC" : " ASC") << std::endl;
    }
}

inline void ExplainStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Explain" << std::endl;
    inner_->print(os, indent + 4);
}

inline void SetStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Set: " << var_ << " = "
       << value_ << std::endl;
}

inline void BeginStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Begin" << std::endl;
}

inline void CommitStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Commit" << std::endl;
}

inline void RollbackStmt::print(std::ostream& os, int indent) const
{
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Rollback" << std::endl;
}

#endif  // PARSER_AST_PRINT_HXX
