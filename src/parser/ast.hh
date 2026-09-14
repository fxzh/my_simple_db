// ast.hh: SQL 语句与表达式的 AST 节点定义(纯语法树, 不含存储/执行)
// 由 parser.y 的 %code requires 包含, 因此 parser.tab.hh(lexer.l/sql_parser.cpp
// 均包含它)也能看到完整类型 —— variant 语义值中 unique_ptr<Expr>/<SQLStatement>
// 的析构要求这些类型必须是完整类型
#ifndef PARSER_AST_HH
#define PARSER_AST_HH

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// 列定义: 列名 + 类型
struct ColumnDef {
    std::string name;
    std::string type;
};

// ==================== 表达式 AST ====================

// 表达式种类, 供执行层等上层按类型分派
enum class ExprKind {
    Int, Float, String, Identifier, BinaryOp, UnaryOp,
};

// 表达式基类
struct Expr {
    virtual ~Expr() = default;
    virtual void print(std::ostream& os, int indent = 0) const = 0;
    // 表达式种类, 供上层分派
    virtual ExprKind kind() const = 0;
};

// 整数字面量
struct IntExpr : Expr {
    long long value;
    explicit IntExpr(long long val) : value(val) {}
    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "Int: " << value << std::endl;
    }
    ExprKind kind() const override { return ExprKind::Int; }
};

// 浮点字面量
struct FloatExpr : Expr {
    double value;
    explicit FloatExpr(double val) : value(val) {}
    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "Float: " << value << std::endl;
    }
    ExprKind kind() const override { return ExprKind::Float; }
};

// 字符串字面量
struct StringExpr : Expr {
    std::string value;
    explicit StringExpr(std::string val) : value(std::move(val)) {}
    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "String: " << value << std::endl;
    }
    ExprKind kind() const override { return ExprKind::String; }
};

// 表达式中的标识符
struct IdentifierExpr : Expr {
    std::string name;
    explicit IdentifierExpr(std::string n) : name(std::move(n)) {}
    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "Identifier: " << name << std::endl;
    }
    ExprKind kind() const override { return ExprKind::Identifier; }
};

// 二元运算符节点
struct BinaryOpExpr : Expr {
    char op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    BinaryOpExpr(char op_, std::unique_ptr<Expr> left_, std::unique_ptr<Expr> right_)
        : op(op_), left(std::move(left_)), right(std::move(right_)) {}

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "BinaryOp: " << op << std::endl;
        left->print(os, indent + 4);
        right->print(os, indent + 4);
    }
    ExprKind kind() const override { return ExprKind::BinaryOp; }
};

// 一元运算符节点
struct UnaryOpExpr : Expr {
    char op;
    std::unique_ptr<Expr> operand;
    UnaryOpExpr(char op_, std::unique_ptr<Expr> operand_)
        : op(op_), operand(std::move(operand_)) {}

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "UnaryOp: " << op << std::endl;
        operand->print(os, indent + 4);
    }
    ExprKind kind() const override { return ExprKind::UnaryOp; }
};

// ==================== SQL 语句 AST ====================

// 语句种类, 供执行层等上层按类型分派(取代字符串识别)
enum class StmtKind {
    CreateTable,
    DropTable,
    Insert,
    Delete,
};

// SQL 语句基类
class SQLStatement {
public:
    virtual ~SQLStatement() = default;
    virtual void print(std::ostream& os, int indent = 0) const = 0;
    // 语句种类, 供上层分派
    virtual StmtKind kind() const = 0;
};

// CREATE TABLE 表名 (列定义列表)
class CreateTableStmt : public SQLStatement {
    std::string table;
    std::vector<ColumnDef> columns;
public:
    CreateTableStmt(std::string name, std::vector<ColumnDef> cols)
        : table(std::move(name)), columns(std::move(cols)) {}

    const std::string& table_name() const { return table; }
    const std::vector<ColumnDef>& column_defs() const { return columns; }

    void print(std::ostream& os, int indent) const override 
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "CreateTable: " << table << std::endl;
        for (const auto& col : columns) {
            os << std::string(static_cast<std::size_t>(indent + 4), ' ') << col.name << " " << col.type << std::endl;
        }
    }

    StmtKind kind() const override { return StmtKind::CreateTable; }
};

// DROP TABLE 表名
class DropTableStmt : public SQLStatement {
    std::string table;
public:
    explicit DropTableStmt(std::string name) : table(std::move(name)) {}

    const std::string& table_name() const { return table; }

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "DropTable: " << table << std::endl;
    }

    StmtKind kind() const override { return StmtKind::DropTable; }
};

// INSERT INTO 表名 VALUES (值列表)
class InsertStmt : public SQLStatement {
    std::string table;
    std::vector<std::unique_ptr<Expr>> values_;
public:
    InsertStmt(std::string name, std::vector<std::unique_ptr<Expr>> vals)
        : table(std::move(name)), values_(std::move(vals)) {}

    const std::string& table_name() const { return table; }
    const std::vector<std::unique_ptr<Expr>>& values() const { return values_; }

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "InsertInto: " << table << std::endl;
        for (const auto& e : values_) {
            e->print(os, indent + 4);
        }
    }

    StmtKind kind() const override { return StmtKind::Insert; }
};

// DELETE FROM 表名
class DeleteStmt : public SQLStatement {
    std::string table;
public:
    explicit DeleteStmt(std::string name) : table(std::move(name)) {}

    const std::string& table_name() const { return table; }

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "DeleteFrom: " << table << std::endl;
    }

    StmtKind kind() const override { return StmtKind::Delete; }
};

#endif  // PARSER_AST_HH
