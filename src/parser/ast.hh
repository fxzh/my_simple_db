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

// 表达式基类
class Expr {
public:
  virtual ~Expr() = default;
  virtual void print(std::ostream& os, int indent = 0) const = 0;
};

// 整数字面量
class IntExpr : public Expr {
  long long value;
public:
  explicit IntExpr(long long val) : value(val) {}
  void print(std::ostream& os, int indent) const override {
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Int: " << value << std::endl;
  }
};

// 浮点字面量
class FloatExpr : public Expr {
  double value;
public:
  explicit FloatExpr(double val) : value(val) {}
  void print(std::ostream& os, int indent) const override {
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Float: " << value << std::endl;
  }
};

// 字符串字面量
class StringExpr : public Expr {
  std::string value;
public:
  explicit StringExpr(std::string val) : value(std::move(val)) {}
  void print(std::ostream& os, int indent) const override {
    os << std::string(static_cast<std::size_t>(indent), ' ') << "String: " << value << std::endl;
  }
};

// 表达式中的标识符
class IdentifierExpr : public Expr {
  std::string name;
public:
  explicit IdentifierExpr(std::string n) : name(std::move(n)) {}
  void print(std::ostream& os, int indent) const override {
    os << std::string(static_cast<std::size_t>(indent), ' ') << "Identifier: " << name << std::endl;
  }
};

// 二元运算符节点
class BinaryOpExpr : public Expr {
  char op;
  std::unique_ptr<Expr> left;
  std::unique_ptr<Expr> right;
public:
  BinaryOpExpr(char op_, std::unique_ptr<Expr> left_,
               std::unique_ptr<Expr> right_)
    : op(op_), left(std::move(left_)), right(std::move(right_)) {}

  void print(std::ostream& os, int indent) const override {
    os << std::string(static_cast<std::size_t>(indent), ' ') << "BinaryOp: " << op << std::endl;
    left->print(os, indent + 2);
    right->print(os, indent + 2);
  }
};

// 一元运算符节点
class UnaryOpExpr : public Expr {
  char op;
  std::unique_ptr<Expr> operand;
public:
  UnaryOpExpr(char op_, std::unique_ptr<Expr> operand_)
    : op(op_), operand(std::move(operand_)) {}

  void print(std::ostream& os, int indent) const override {
    os << std::string(static_cast<std::size_t>(indent), ' ') << "UnaryOp: " << op << std::endl;
    operand->print(os, indent + 2);
  }
};

// ==================== SQL 语句 AST ====================

// SQL 语句基类
class SQLStatement {
public:
  virtual ~SQLStatement() = default;
  virtual void print(std::ostream& os, int indent = 0) const = 0;
};

// CREATE TABLE 表名 (列定义列表)
class CreateTableStmt : public SQLStatement {
  std::string table;
  std::vector<ColumnDef> columns;
public:
  CreateTableStmt(std::string name, std::vector<ColumnDef> cols)
    : table(std::move(name)), columns(std::move(cols)) {}

  void print(std::ostream& os, int indent) const override {
    os << std::string(static_cast<std::size_t>(indent), ' ') << "CreateTable: " << table << std::endl;
    for (const auto& col : columns) {
      os << std::string(static_cast<std::size_t>(indent + 2), ' ') << col.name << " " << col.type << std::endl;
    }
  }
};

// DROP TABLE 表名
class DropTableStmt : public SQLStatement {
  std::string table;
public:
  explicit DropTableStmt(std::string name) : table(std::move(name)) {}

  void print(std::ostream& os, int indent) const override {
    os << std::string(static_cast<std::size_t>(indent), ' ') << "DropTable: " << table << std::endl;
  }
};

// INSERT INTO 表名 VALUES (值列表)
class InsertStmt : public SQLStatement {
  std::string table;
  std::vector<std::unique_ptr<Expr>> values;
public:
  InsertStmt(std::string name, std::vector<std::unique_ptr<Expr>> vals)
    : table(std::move(name)), values(std::move(vals)) {}

  void print(std::ostream& os, int indent) const override {
    os << std::string(static_cast<std::size_t>(indent), ' ') << "InsertInto: " << table << std::endl;
    for (const auto& e : values) {
      e->print(os, indent + 2);
    }
  }
};

#endif  // PARSER_AST_HH
