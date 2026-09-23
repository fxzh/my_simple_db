// ast.hh: SQL 语句与表达式的 AST 节点定义(纯语法树, 不含存储/执行)
// 由 parser.y 的 %code requires 包含, 因此 parser.tab.hh(lexer.l/sql_parser.cpp
// 均包含它)也能看到完整类型 —— variant 语义值中 unique_ptr<Expr>/<SQLStatement>
// 的析构要求这些类型必须是完整类型
#ifndef PARSER_AST_HH
#define PARSER_AST_HH

#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// 列类型名(语法层枚举, 由执行层映射到存储层 st::ColType)
enum class DataType : uint8_t {
    Int, BigInt, Float, Double, Char, VarChar,
};

// 类型说明: 类型名 + 声明长度(未声明为 nullopt)
struct TypeInfo {
    DataType type;
    std::optional<long long> length;
};

// 列定义: 列名 + 类型说明
struct ColumnDef {
    std::string name;
    DataType type;
    std::optional<long long> length;  // char/varchar 的声明长度, 未声明为 nullopt
};

// 类型名转文本, 带 (长度) 后缀: 供打印与日志使用
inline std::string type_to_string(DataType type, std::optional<long long> length)
{
    const char* name = nullptr;
    switch (type) {
    case DataType::Int: name = "int"; break;
    case DataType::BigInt: name = "bigint"; break;
    case DataType::Float: name = "float"; break;
    case DataType::Double: name = "double"; break;
    case DataType::Char: name = "char"; break;
    case DataType::VarChar: name = "varchar"; break;
    }
    if (!length) {
        return name;
    }
    return std::format("{}({})", name, *length);
}

// ==================== 表达式 AST ====================

// 表达式种类, 供执行层等上层按类型分派
enum class ExprKind {
    Int, Float, String, Identifier, BinaryOp, UnaryOp, Null,
    Compare, Logic, Not, IsNull,
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

// NULL 字面量
struct NullExpr : Expr {
    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "Null" << std::endl;
    }
    ExprKind kind() const override { return ExprKind::Null; }
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

// 比较运算种类
enum class CmpOp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

// 逻辑运算种类
enum class LogicOp : uint8_t { And, Or };

// 比较运算符转文本: 供打印与反生成使用
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

// 比较运算节点: = <> < <= > >=
struct CompareExpr : Expr {
    CmpOp op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    CompareExpr(CmpOp op_, std::unique_ptr<Expr> left_, std::unique_ptr<Expr> right_)
        : op(op_), left(std::move(left_)), right(std::move(right_)) {}

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "Compare: "
           << cmp_to_string(op) << std::endl;
        left->print(os, indent + 4);
        right->print(os, indent + 4);
    }
    ExprKind kind() const override { return ExprKind::Compare; }
};

// 逻辑运算节点: AND / OR
struct LogicExpr : Expr {
    LogicOp op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    LogicExpr(LogicOp op_, std::unique_ptr<Expr> left_, std::unique_ptr<Expr> right_)
        : op(op_), left(std::move(left_)), right(std::move(right_)) {}

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "Logic: "
           << (op == LogicOp::And ? "AND" : "OR") << std::endl;
        left->print(os, indent + 4);
        right->print(os, indent + 4);
    }
    ExprKind kind() const override { return ExprKind::Logic; }
};

// 逻辑非节点
struct NotExpr : Expr {
    std::unique_ptr<Expr> operand;
    explicit NotExpr(std::unique_ptr<Expr> operand_) : operand(std::move(operand_)) {}

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "Not" << std::endl;
        operand->print(os, indent + 4);
    }
    ExprKind kind() const override { return ExprKind::Not; }
};

// IS [NOT] NULL 判空节点
struct IsNullExpr : Expr {
    std::unique_ptr<Expr> operand;
    bool negate;   // true 表示 IS NOT NULL
    IsNullExpr(std::unique_ptr<Expr> operand_, bool negate_)
        : operand(std::move(operand_)), negate(negate_) {}

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "IsNull" << (negate ? " not" : "")
           << std::endl;
        operand->print(os, indent + 4);
    }
    ExprKind kind() const override { return ExprKind::IsNull; }
};

// 表达式反生成 SQL 文本: 供输出列命名与日志使用
inline std::string expr_to_string(const Expr& e)
{
    switch (e.kind()) {
    case ExprKind::Int:
        return std::format("{}", static_cast<const IntExpr&>(e).value);
    case ExprKind::Float:
        return std::format("{}", static_cast<const FloatExpr&>(e).value);
    case ExprKind::String:
        return "'" + static_cast<const StringExpr&>(e).value + "'";
    case ExprKind::Null:
        return "NULL";
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
    }
    return "";
}

// ==================== SQL 语句 AST ====================

// 语句种类, 供执行层等上层按类型分派(取代字符串识别)
enum class StmtKind {
    CreateTable,
    DropTable,
    Insert,
    Delete,
    Select,
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
            os << std::string(static_cast<std::size_t>(indent + 4), ' ') << col.name << " "
               << type_to_string(col.type, col.length) << std::endl;
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

// DELETE FROM 表名 [WHERE 条件]
class DeleteStmt : public SQLStatement {
    std::string table;
    std::unique_ptr<Expr> where_;   // 无 WHERE 时为空
public:
    DeleteStmt(std::string name, std::unique_ptr<Expr> where)
        : table(std::move(name)), where_(std::move(where)) {}

    const std::string& table_name() const { return table; }
    const Expr* where_expr() const { return where_.get(); }

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "DeleteFrom: " << table << std::endl;
        if (where_) {
            where_->print(os, indent + 4);
        }
    }

    StmtKind kind() const override { return StmtKind::Delete; }
};

// SELECT 投影项: star 为 true 时 expr 为空, 且 items 中仅允许一个这样的元素
struct SelectItem {
    std::unique_ptr<Expr> expr;
    std::string alias;    // 仅显式 AS 时非空
    bool star = false;
};

// 排序键: 列名或别名 + 方向
struct OrderItem {
    std::string name;
    bool desc = false;
};

// SELECT [DISTINCT] 投影 FROM 表 [WHERE] [ORDER BY] [LIMIT n [OFFSET m]]
class SelectStmt : public SQLStatement {
    std::string table_;
    bool distinct_ = false;
    std::vector<SelectItem> items_;
    std::unique_ptr<Expr> where_;   // 无 WHERE 时为空
    std::vector<OrderItem> orders_;
    std::optional<long long> limit_;   // 未指定为 nullopt
    std::optional<long long> offset_;  // 未指定为 nullopt
public:
    SelectStmt(std::string name, bool distinct, std::vector<SelectItem> items,
               std::unique_ptr<Expr> where, std::vector<OrderItem> orders,
               std::optional<long long> limit, std::optional<long long> offset)
        : table_(std::move(name)), distinct_(distinct), items_(std::move(items)),
          where_(std::move(where)), orders_(std::move(orders)), limit_(limit), offset_(offset) {}

    const std::string& table_name() const { return table_; }
    bool distinct() const { return distinct_; }
    const std::vector<SelectItem>& items() const { return items_; }
    const Expr* where_expr() const { return where_.get(); }
    const std::vector<OrderItem>& orders() const { return orders_; }
    const std::optional<long long>& limit() const { return limit_; }
    const std::optional<long long>& offset() const { return offset_; }

    void print(std::ostream& os, int indent) const override
    {
        os << std::string(static_cast<std::size_t>(indent), ' ') << "Select: " << table_ << std::endl;
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
    }

    StmtKind kind() const override { return StmtKind::Select; }
};

#endif  // PARSER_AST_HH
