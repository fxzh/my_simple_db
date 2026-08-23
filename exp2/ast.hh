// ast.hh: AST 节点与内存数据库定义
// 由 parser.y 的 %code requires 包含, 因此 parser.tab.hh(main.cc/lexer.l 均包含它)
// 也能看到完整类型 —— variant 语义值中 unique_ptr<Expr>/<SQLStatement>
// 的析构要求这些类型必须是完整类型
#ifndef EXP2_AST_HH
#define EXP2_AST_HH

#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// 列定义: 列名 + 类型
struct ColumnDef {
  std::string name;
  std::string type;
};

// ==================== 内存数据库 ====================

// 数据库中的值: 整数 / 浮点数 / 字符串
using DBValue = std::variant<long long, double, std::string>;

// 把值转成字符串用于打印
inline std::string valueToString(const DBValue& v)
{
  return std::visit([](const auto& val) -> std::string {
    using T = std::decay_t<decltype(val)>;
    if constexpr (std::is_same_v<T, long long>) {
      return std::to_string(val);
    } else if constexpr (std::is_same_v<T, double>) {
      std::ostringstream oss;
      oss << val;
      return oss.str();
    } else {
      return val;
    }
  }, v);
}

struct Table {
  std::vector<ColumnDef> columns;           // 列定义
  std::vector<std::vector<DBValue>> rows;   // 数据行
};

// 内存数据库: 表名 -> 表
class Database {
public:
  bool exists(const std::string& name) const
  {
    return tables_.find(name) != tables_.end();
  }

  void createTable(const std::string& name, std::vector<ColumnDef> columns)
  {
    tables_[name] = Table{ std::move(columns), {} };
  }

  bool dropTable(const std::string& name)
  {
    return tables_.erase(name) > 0;
  }

  bool insertRow(const std::string& name, const std::vector<DBValue>& row)
  {
    auto it = tables_.find(name);
    if (it == tables_.end()) {
      std::cerr << "Error: table '" << name << "' does not exist" << std::endl;
      return false;
    }
    if (row.size() != it->second.columns.size()) {
      std::cerr << "Error: table '" << name << "' expects "
                << it->second.columns.size() << " value(s), got "
                << row.size() << std::endl;
      return false;
    }
    it->second.rows.push_back(row);
    return true;
  }

  // 打印所有表及其数据
  void print() const
  {
    if (tables_.empty()) {
      std::cout << "(no tables)" << std::endl;
      return;
    }
    for (const auto& entry : tables_) {
      const Table& table = entry.second;
      std::cout << "Table " << entry.first << " (";
      for (size_t i = 0; i < table.columns.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << table.columns[i].name << " " << table.columns[i].type;
      }
      std::cout << "), " << table.rows.size() << " row(s)" << std::endl;
      for (const auto& row : table.rows) {
        std::cout << "  (";
        for (size_t i = 0; i < row.size(); ++i) {
          if (i > 0) std::cout << ", ";
          std::cout << valueToString(row[i]);
        }
        std::cout << ")" << std::endl;
      }
    }
  }

private:
  std::map<std::string, Table> tables_;
};

// 全局数据库(定义在 parser.y 的 %code 中)
extern Database database;

// ==================== 表达式 AST ====================

// 把值转成数值(用于算术运算)
inline double asNumber(const DBValue& v)
{
  if (const auto* i = std::get_if<long long>(&v)) return static_cast<double>(*i);
  if (const auto* d = std::get_if<double>(&v)) return *d;
  std::cerr << "Error: string value used in arithmetic" << std::endl;
  return 0.0;
}

// 表达式基类
class Expr {
public:
  virtual ~Expr() = default;
  virtual DBValue evaluate() const = 0;
  virtual void print(std::ostream& os, int indent = 0) const = 0;
};

// 整数字面量
class IntExpr : public Expr {
  long long value;
public:
  explicit IntExpr(long long val) : value(val) {}
  DBValue evaluate() const override { return value; }
  void print(std::ostream& os, int indent) const override {
    os << std::string(indent, ' ') << "Int: " << value << std::endl;
  }
};

// 浮点字面量
class FloatExpr : public Expr {
  double value;
public:
  explicit FloatExpr(double val) : value(val) {}
  DBValue evaluate() const override { return value; }
  void print(std::ostream& os, int indent) const override {
    os << std::string(indent, ' ') << "Float: " << value << std::endl;
  }
};

// 字符串字面量
class StringExpr : public Expr {
  std::string value;
public:
  explicit StringExpr(std::string val) : value(std::move(val)) {}
  DBValue evaluate() const override { return value; }
  void print(std::ostream& os, int indent) const override {
    os << std::string(indent, ' ') << "String: " << value << std::endl;
  }
};

// 表达式中的标识符(exp1 的 expression: IDENTIFIER, 求值时报未定义)
class IdentifierExpr : public Expr {
  std::string name;
public:
  explicit IdentifierExpr(std::string n) : name(std::move(n)) {}
  DBValue evaluate() const override {
    std::cerr << "Error: Unknown identifier '" << name << "'" << std::endl;
    return 0ll;
  }
  void print(std::ostream& os, int indent) const override {
    os << std::string(indent, ' ') << "Identifier: " << name << std::endl;
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

  DBValue evaluate() const override {
    DBValue l = left->evaluate();
    DBValue r = right->evaluate();
    // 整数 op 整数 -> 整数
    const auto* li = std::get_if<long long>(&l);
    const auto* ri = std::get_if<long long>(&r);
    if (li != nullptr && ri != nullptr) {
      switch (op) {
        case '+': return *li + *ri;
        case '-': return *li - *ri;
        case '*': return *li * *ri;
        case '/':
          if (*ri == 0) {
            std::cerr << "Error: Division by zero!" << std::endl;
            return 0ll;
          }
          return *li / *ri;
      }
    }
    // 否则按浮点数计算
    double ld = asNumber(l);
    double rd = asNumber(r);
    switch (op) {
      case '+': return ld + rd;
      case '-': return ld - rd;
      case '*': return ld * rd;
      case '/':
        if (rd == 0.0) {
          std::cerr << "Error: Division by zero!" << std::endl;
          return 0.0;
        }
        return ld / rd;
    }
    return 0ll;
  }

  void print(std::ostream& os, int indent) const override {
    os << std::string(indent, ' ') << "BinaryOp: " << op << std::endl;
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

  DBValue evaluate() const override {
    DBValue v = operand->evaluate();
    if (const auto* i = std::get_if<long long>(&v)) {
      return op == '-' ? DBValue(-*i) : DBValue(*i);
    }
    double d = asNumber(v);
    return op == '-' ? -d : d;
  }

  void print(std::ostream& os, int indent) const override {
    os << std::string(indent, ' ') << "UnaryOp: " << op << std::endl;
    operand->print(os, indent + 2);
  }
};

// ==================== SQL语句 AST ====================

// SQL语句基类
class SQLStatement {
public:
  virtual ~SQLStatement() = default;
  virtual void execute() const = 0;
  virtual void print(std::ostream& os, int indent = 0) const = 0;
};

// CREATE TABLE 表名 (列定义列表)
class CreateTableStmt : public SQLStatement {
  std::string table;
  std::vector<ColumnDef> columns;
public:
  CreateTableStmt(std::string name, std::vector<ColumnDef> cols)
    : table(std::move(name)), columns(std::move(cols)) {}

  void execute() const override {
    if (database.exists(table)) {
      std::cerr << "Error: table '" << table << "' already exists" << std::endl;
      return;
    }
    database.createTable(table, columns);
    std::cout << "Table '" << table << "' created with "
              << columns.size() << " column(s)" << std::endl;
  }

  void print(std::ostream& os, int indent) const override {
    os << std::string(indent, ' ') << "CreateTable: " << table << std::endl;
    for (const auto& col : columns) {
      os << std::string(indent + 2, ' ') << col.name << " " << col.type << std::endl;
    }
  }
};

// DROP TABLE 表名
class DropTableStmt : public SQLStatement {
  std::string table;
public:
  explicit DropTableStmt(std::string name) : table(std::move(name)) {}

  void execute() const override {
    if (database.dropTable(table)) {
      std::cout << "Table '" << table << "' dropped" << std::endl;
    } else {
      std::cerr << "Error: table '" << table << "' does not exist" << std::endl;
    }
  }

  void print(std::ostream& os, int indent) const override {
    os << std::string(indent, ' ') << "DropTable: " << table << std::endl;
  }
};

// INSERT INTO 表名 VALUES (值列表)
class InsertStmt : public SQLStatement {
  std::string table;
  std::vector<std::unique_ptr<Expr>> values;
public:
  InsertStmt(std::string name, std::vector<std::unique_ptr<Expr>> vals)
    : table(std::move(name)), values(std::move(vals)) {}

  void execute() const override {
    std::vector<DBValue> row;
    row.reserve(values.size());
    for (const auto& e : values) {
      row.push_back(e->evaluate());
    }
    if (database.insertRow(table, row)) {
      std::cout << "1 row inserted into '" << table << "'" << std::endl;
    }
  }

  void print(std::ostream& os, int indent) const override {
    os << std::string(indent, ' ') << "InsertInto: " << table << std::endl;
    for (const auto& e : values) {
      e->print(os, indent + 2);
    }
  }
};

#endif  // EXP2_AST_HH
