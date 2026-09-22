# SELECT 支持设计文档

## 1. 目标与范围

为 my_simple_db 接入单表 SELECT 的核心子集，目标语法：

```sql
SELECT [DISTINCT] 投影列表 FROM 表名
    [WHERE 条件]
    [ORDER BY 列名/别名 [ASC|DESC], ...]
    [LIMIT 整数 [OFFSET 整数]]
```

投影列表为 `*`、列引用或常量/行表达式（含算术与比较），别名仅支持显式 `AS`。

范围（三期交付，每期独立可验证）：

- **M-S1 端到端最小闭环**：ResultSet 回传链路（proto/executor/server/client）+ 词法文法 + `SELECT *` / 列投影 / 常量表达式
- **M-S2 行上下文求值**：比较运算 + AND/OR/NOT 三值逻辑 + WHERE + IS NULL + NULL 字面量（顺带让 INSERT 支持显式 NULL）
- **M-S3 结果整形**：ORDER BY + LIMIT/OFFSET + DISTINCT

不在本设计范围内（各自另立设计）：

- 多表 JOIN、聚合与 GROUP BY/HAVING、子查询
- 内置函数、隐式类型转换、CAST、日期时间类型
- 索引加速（M3 B+ 树前 WHERE 只能全表扫，功能正确）
- 流式结果集（演进见 proto 模块的约定）

无新增第三方依赖，全部使用标准库（std::variant/std::optional/std::format）。

## 2. 总体链路与分期

```
client(渲染结果集表格)
   ▲ proto::ResultSet 帧
server session.cpp: ExecResult → encode_result_set → send_frame(ResultSet)
executor: SELECT 流水线: 扫描 → WHERE 过滤 → 投影物化 → 去重 → 排序 → 截断
storage: Scanner 全表扫描 + TableMeta 元数据
```

| 期 | 内容 | 涉及文件 |
|----|------|----------|
| M-S1 | SELECT/DISTINCT 等关键字与 select 文法、SelectStmt AST、ExecResult 接口改造、proto ResultSet 编解码、session 分流、client 渲染、storage 公开元数据入口 | lexer.l、parser.y、ast.hh、executor.h/.cpp、proto.h、session.cpp、client.cpp、storage.h/.cpp |
| M-S2 | 比较运算符 token、Compare/Logic/Not/IsNull/Null 表达式节点、EvalValue 三值逻辑求值、WHERE 过滤 | lexer.l、parser.y、ast.hh、executor.cpp |
| M-S3 | ORDER BY/LIMIT/OFFSET/DISTINCT 文法与执行、值比较器、排序键预计算 | lexer.l、parser.y、ast.hh、executor.cpp |

验收标准：每期完成后 `test/e2e/sql/select/` 对应用例全绿（见 §11），既有 ddl/dml/proto 用例不回归。

## 3. 词法与文法

### 3.1 lexer.l 新增

关键字（沿用现有大小写不敏感风格）：

```
select  distinct  where  and  or  not  is  null  order  by  asc  desc  limit  offset  as
```

运算符 token（现文件 lexer.l:66 只有 `+ - * / ( ) , ;`）：

```
"=="|"="   → EQ          "<>"|"!="  → NE
"<="       → LE          ">="       → GE
"<"        → '<' (字面量 token)      ">" → '>'
```

注意：`null` 的 token 名用 `NULL_T`，避免与常见宏 `NULL` 冲突。

### 3.2 parser.y 文法

token 声明新增：

```
%token SELECT DISTINCT WHERE AND OR NOT IS NULL_T ORDER BY ASC DESC LIMIT OFFSET AS
%token EQ NE LE GE
```

运算符优先级（完整替换现有块，新条目在低优先端）：

```
%left OR
%left AND
%left NOT
%nonassoc EQ NE '<' LE '>' GE
%left '+' '-'
%left '*' '/'
%right UMINUS
```

比较运算取 `%nonassoc`：`a = b = c` 直接语法报错，而非运行期报 bool 参与比较。`NOT` 低于比较，`NOT a = 1` 解析为 `NOT (a = 1)`。

新增规则（variant 语义值新类型 `SelectItem`、`OrderItem`、`std::pair<std::optional<long long>, std::optional<long long>>` 需在 `%code requires` 处保证完整类型，pair/optional 的 include 补在 ast.hh）：

```
statement:
    ... | select_statement  { $$ = std::move($1); }
    ;

select_statement:
    SELECT distinct_opt select_list FROM IDENTIFIER where_opt order_by_opt limit_opt {
        $$ = std::make_unique<SelectStmt>(std::move($5), $1, std::move($3), std::move($6),
                                         std::move($7), $8.first, $8.second);
    }
    ;

distinct_opt:  /* 空 */ { $$ = false; } | DISTINCT { $$ = true; } ;

select_list:
    '*' { $$ = std::vector<SelectItem>{ SelectItem{ nullptr, "", true } }; }
  | select_items { $$ = std::move($1); }
    ;

select_items:
    select_items ',' select_item { $1.push_back(std::move($3)); $$ = std::move($1); }
  | select_item { $$ = std::vector<SelectItem>{ std::move($1) }; }
    ;

select_item:
    expression alias_opt {
        $$ = SelectItem{ std::move($1), std::move($2), false };
    }
    ;

alias_opt:  /* 空 */ { $$ = std::string(); } | AS IDENTIFIER { $$ = std::move($2); } ;

where_opt:  /* 空 */ { $$ = nullptr; } | WHERE expression { $$ = std::move($2); } ;

order_by_opt:  /* 空 */ { $$ = std::vector<OrderItem>{}; }
  | ORDER BY order_items { $$ = std::move($3); } ;

order_items:
    order_items ',' order_item { $1.push_back(std::move($3)); $$ = std::move($1); }
  | order_item { $$ = std::vector<OrderItem>{ std::move($1) }; }
    ;

order_item:
    IDENTIFIER dir_opt { $$ = OrderItem{ std::move($1), $2 }; }
    ;

dir_opt:  /* 空 */ { $$ = false; } | ASC { $$ = false; } | DESC { $$ = true; } ;

limit_opt:  /* 空 */ { $$ = {}; }
  | LIMIT INTEGER { $$ = { std::optional<long long>($2), std::nullopt }; }
  | LIMIT INTEGER OFFSET INTEGER { $$ = { std::optional<long long>($2), std::optional<long long>($4) }; }
    ;

expression:  /* 现有规则之外新增 */
    expression EQ expression  { $$ = std::make_unique<CompareExpr>(CmpOp::Eq, std::move($1), std::move($3)); }
  | expression NE expression  { $$ = std::make_unique<CompareExpr>(CmpOp::Ne, std::move($1), std::move($3)); }
  | expression '<' expression { $$ = std::make_unique<CompareExpr>(CmpOp::Lt, std::move($1), std::move($3)); }
  | expression LE expression  { $$ = std::make_unique<CompareExpr>(CmpOp::Le, std::move($1), std::move($3)); }
  | expression '>' expression { $$ = std::make_unique<CompareExpr>(CmpOp::Gt, std::move($1), std::move($3)); }
  | expression GE expression  { $$ = std::make_unique<CompareExpr>(CmpOp::Ge, std::move($1), std::move($3)); }
  | expression AND expression { $$ = std::make_unique<LogicExpr>(LogicOp::And, std::move($1), std::move($3)); }
  | expression OR expression  { $$ = std::make_unique<LogicExpr>(LogicOp::Or, std::move($1), std::move($3)); }
  | NOT expression            { $$ = std::make_unique<NotExpr>(std::move($2)); }
  | expression IS null_not_opt NULL_T { $$ = std::make_unique<IsNullExpr>(std::move($1), $3); }
  | NULL_T                    { $$ = std::make_unique<NullExpr>(); }
    ;

null_not_opt:  /* 空 */ { $$ = false; } | NOT { $$ = true; } ;
```

### 3.3 语法层语义限制

- 别名仅显式 `AS`：`select_item: expression IDENTIFIER` 会与表达式尾部的列引用产生移进/归约冲突，不支持。
- `*` 仅允许独占整个投影列表（文法结构保证，不需要运行期校验）。
- ORDER BY 键仅支持列名或别名，不支持位置序号（`ORDER BY 1` 会按整数常量表达式处理，与语义不符，由文法排除——只接受 IDENTIFIER）。
- LIMIT/OFFSET 仅接受非负整数字面量（INTEGER 无法带负号，语法层即约束）。

## 4. AST 扩展（ast.hh）

ExprKind 与 StmtKind 新增种类：`Compare`、`Logic`、`Not`、`IsNull`、`Null`；`StmtKind::Select`。

表达式节点沿用现有 struct 风格：

```cpp
// 比较运算种类
enum class CmpOp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

// 逻辑运算种类
enum class LogicOp : uint8_t { And, Or };

// 比较运算节点: = <> < <= > >=
struct CompareExpr : Expr {
    CmpOp op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    CompareExpr(CmpOp op_, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r)
        : op(op_), left(std::move(l)), right(std::move(r)) {}
    // print/kind 与现有节点同形, 略
};

// 逻辑运算节点: AND / OR
struct LogicExpr : Expr {
    LogicOp op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

// 逻辑非节点(NOT 单独成节点, 避免二元节点带空指针)
struct NotExpr : Expr {
    std::unique_ptr<Expr> operand;
};

// IS [NOT] NULL 判空节点
struct IsNullExpr : Expr {
    std::unique_ptr<Expr> operand;
    bool negate;   // true 表示 IS NOT NULL
};

// NULL 字面量
struct NullExpr : Expr {};
```

语句侧新类型与节点（语句节点沿用本文件既有 class 风格）：

```cpp
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
    // 构造与只读访问器 table_name()/distinct()/items()/where_expr()/orders()/limit()/offset(),
    // 形式与 CreateTableStmt 一致
};
```

ast.hh 另新增一个反生成工具（free inline 函数），供输出列命名与日志使用：

```cpp
// 表达式反生成 SQL 文本: Int/Float 用 std::format("{}", v), String 带单引号,
// Identifier 原名, 二元/比较节点 "(l op r)", NOT 为 "(NOT x)", IS NULL 为 "(x IS [NOT] NULL)",
// NULL 字面量为 "NULL"
std::string expr_to_string(const Expr& e);
```

## 5. 表达式求值（executor）

### 5.1 求值值域与入口

存储层 `st::Value`（monostate/int64/double/string）不含 bool，而 WHERE 需要三值逻辑。求值器使用 executor 内部值域：

```cpp
// 求值值域: 在存储值上扩展 bool; monostate 表示 NULL(条件上下文即 UNKNOWN)
using EvalValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

// 行上下文: 列名 → 行内下标, 每条语句编译一次
using ColMap = std::unordered_map<std::string, size_t>;

// 统一求值入口: cols/row 同时为空表示常量折叠上下文(标识符不可用)
EvalValue eval_expr(const Expr& e, const ColMap* cols, const st::Row* row);

// 两个薄封装: 调用方不接触空指针
EvalValue eval_const(const Expr& e);                                        // INSERT VALUES 用
EvalValue eval_row(const Expr& e, const ColMap& cols, const st::Row& row);  // SELECT 用
```

现有 `eval_const_expr`/`eval_unary`/`eval_binary` 重构到 EvalValue 上：算术语义不变（int/int 向零截断、溢出/除零报 `ArithError`、字符串参与算术报 `ValueMismatch`），bool/monostate 参与算术同样报 `ValueMismatch`。INSERT 路径求值后经 `to_st_value(EvalValue)` 转回 `st::Value`：monostate → NULL（存储层 encode_row 已按 NOT NULL 校验），bool → 报错。

### 5.2 比较语义（CompareExpr）

| 操作数 | 语义 |
|--------|------|
| 任一为 NULL | 结果 UNKNOWN（monostate） |
| 数值 × 数值 | 提升为 double 后比较 |
| 字符串 × 字符串 | 逐字节字典序 |
| bool 参与比较、数值 × 字符串 | 当场报 `ValueMismatch`（无隐式转换） |

### 5.3 逻辑语义（LogicExpr / NotExpr）

操作数须为 bool 或 NULL，其余当场报 `ValueMismatch`：

| a | b | a AND b | a OR b |
|-----|-----|---------|--------|
| T | T | T | T |
| T | F | F | T |
| T | NULL | NULL | T |
| F | F | F | F |
| F | NULL | F | NULL |
| NULL | NULL | NULL | NULL |

NOT：T→F、F→T、NULL→NULL。

### 5.4 判空与 WHERE 谓词

- IS NULL / IS NOT NULL：对任意类型操作数直接产出 bool，无 UNKNOWN。
- WHERE 行保留条件：求值结果为 `bool true`；false 与 NULL 丢弃该行；其他类型报 `ValueMismatch`。

## 6. SELECT 执行流水线（executor）

### 6.1 执行层接口改造

`execute` 返回值由 `std::string` 改为结构化结果（唯一调用方是 session.cpp）：

```cpp
// 执行结果: 状态文本或结果集, 二选一
struct ExecResult {
    bool is_result_set = false;
    std::string status;                          // is_result_set=false 时有效
    std::vector<std::string> col_names;          // 结果集列名
    std::vector<std::vector<st::Value>> rows;    // 结果集行值(monostate 即 NULL)
};

ExecResult execute(ct::Catalog& db, const SQLStatement& stmt);
```

投影求值结果为 bool 时报 `ValueMismatch`（SELECT 输出不含布尔列，待将来函数/聚合扩展时再定）；NULL 可输出。

### 6.2 存储层唯一改动

executor 需要 TableMeta 做列名解析。storage.h 的私有 `get_table` 更名为公开 `table_meta`（实现不动，只挪到 public 区）：

```cpp
// 按表名取表元数据(只读), 表不存在当场报错
const TableMeta& table_meta(const std::string& name) const;
```

### 6.3 流水线

```
编译期(每语句一次): ColMap 构建; star 展开为全部列; 排序键定位(别名 → 源表列名)
物化期(逐行):       Scanner.next → WHERE 求值过滤 → 投影求值 → 排序键预计算
整形期(一次性):     DISTINCT 排序去重 → ORDER BY 排序 → OFFSET/LIMIT 截断
```

物化行结构（排序键预计算一次，比较器内不再求值）：

```cpp
// 物化行: 投影输出值 + 预计算的排序键值
struct SelectRow {
    std::vector<EvalValue> out;
    std::vector<EvalValue> keys;   // 无 ORDER BY/DISTINCT 时为空
};
```

值比较器（DISTINCT 判等与 ORDER BY 排序共用）：

```cpp
// 值排序比较: NULL 最大; 数值提升(1 与 1.0 相等); 字符串字典序; 类型不可比当场报错
// 返回 -1/0/1
int value_cmp(const EvalValue& a, const EvalValue& b);
```

- **ORDER BY**：`std::stable_sort` 按键序列逐键比较（desc 键取反 value_cmp 结果），同键行保持扫描序，保证输出确定。键解析顺序：先按别名匹配投影项（取该项求值结果），再按源表列名匹配（取原始行列值，需要保留原始行到物化结束）。
- **DISTINCT 约束**：DISTINCT 时排序键必须能定位到输出列（别名或投影中的同名列引用），否则报 `UnknownColumn`；此时物化只需输出值，不保留原始行。
- **DISTINCT 去重**：按 [排序键..., 全部输出列...] 排序后 adjacent-unique（判等用 value_cmp==0，1 与 1.0 视为重复）。无 ORDER BY 时输出按输出列值序，均为确定序。
- **LIMIT/OFFSET**：整形期最后截断，先跳过 offset 行再取至多 limit 行。

### 6.4 输出列命名

| 投影项 | 列名 |
|--------|------|
| star | 各列原名 |
| 有 AS 别名 | 别名 |
| 纯 IdentifierExpr | 列名 |
| 其他表达式 | `expr_to_string` 反生成文本 |

## 7. 协议：ResultSet 编码（proto.h）

`st::Value` 与单元格值同构，proto 内自定义等价 variant，不引入对 storage 的依赖：

```cpp
// 结果集单元格值(与 st::Value 同构, proto 层独立定义避免依赖)
using CellVal = std::variant<std::monostate, int64_t, double, std::string>;

// 结果集: 列名 + 行值
struct ResultSet {
    std::vector<std::string> cols;
    std::vector<std::vector<CellVal>> rows;
};

// ResultSet 编成 body / body 解回 ResultSet; 长度或结构非法 decode 返回 false
std::string encode_result_set(const ResultSet& rs);
bool decode_result_set(std::string_view body, ResultSet& out);
```

body 布局（大端网络序）：

```
[列数 u32]
  每列: [列名长度 u16][列名字节]
[行数 u32]
  每行每列一个单元格: [tag u8][payload]
    tag 0 = NULL   无 payload
    tag 1 = int64  8B 网络序
    tag 2 = double 8B 位模式按 u64 网络序(memcpy 后整型化再转网络序)
    tag 3 = string [长度 u16][字节]
```

## 8. 服务端会话改动（session.cpp）

handle_client 执行段（现 session.cpp:79-88）改为按结果类型分流：

```cpp
exec::ExecResult result = exec::execute(*db, *stmt);
if (result.is_result_set) {
    // st::Value → proto::CellVal 逐格转换后成帧
    proto::ResultSet rs;
    rs.cols = result.col_names;
    // ... 逐格转换 rows
    proto::send_frame(client_socket, proto::MsgType::ResultSet, proto::encode_result_set(rs));
} else {
    proto::send_frame(client_socket, proto::MsgType::Ok, result.status);
}
```

空语句回显原文、quit/exit、错误路径（DbError/std::exception catch 转 Error 帧）均不变。SELECT 成功在 EXECUTOR 模块记 INFO 一条“返回 N 行”。

## 9. 客户端渲染（client.cpp）

收帧分支新增 ResultSet 处理，与 Ok/Error 并列；渲染规则全程确定，保证 golden 文件可全文 diff：

- 列宽 = max(表头宽, 该列各单元格宽)，按字节计；全部左对齐，列间 ` | ` 分隔；
- 表头下分隔行：每列 `-` × 列宽，列间以 `+` 连接；
- 单元格显示：NULL 显示 `NULL`；整数十进制；浮点 `std::format("{}", v)` 最短表示；字符串原样；
- 末行输出 `(N 行)`；空结果仅表头 + `(0 行)`。

渲染示例：

```
 id | name  | score
----+-------+-------
  1 | Alice | 95.5
  2 | NULL  | 90.5
(2 行)
```

`-c` 模式走同一渲染路径；ResultSet 不影响退出码。解码失败（decode 返回 false）直接 `std::cerr` 报错并置 `sql_failed`。

## 10. 错误码与日志

`db::ErrCode` 新增一项（同步补 errCodeName）：

```cpp
UnknownColumn,  // 引用了不存在的列
```

各报错点：

| 场景 | 错误码 | 层 |
|------|--------|----|
| SELECT 的表不存在 | TableNotFound | storage(table_meta) |
| WHERE/投影/排序键引用未知列 | UnknownColumn | executor |
| DISTINCT 时 ORDER BY 键不在输出列 | UnknownColumn | executor |
| 类型不可比、逻辑/WHERE 谓词非布尔、投影输出 bool | ValueMismatch | executor |
| 算术溢出/除零（沿用） | ArithError | executor |

列解析在编译期一次完成；排序比较器内类型不可比在首个冲突对上即报。

## 11. 测试计划

SQL e2e 新建功能目录 `test/e2e/sql/select/`，用例编写遵循 ai_docs/testcase.md：

| 文件 | 覆盖 | 期 |
|------|------|----|
| select_basic.sql | `SELECT *`、列投影、常量算术表达式、别名、空表、不存在的表 | M-S1 |
| select_where.sql | 各比较符、AND/OR/NOT 组合、三值逻辑（NULL 参与）、IS NULL/IS NOT NULL、NULL 字面量（含 INSERT NULL）、类型不可比报错、WHERE 非布尔报错 | M-S2 |
| select_order_limit.sql | 单/多键排序、ASC/DESC、NULL 排序位置、LIMIT/OFFSET、LIMIT 0 | M-S3 |
| select_distinct.sql | 去重、与 ORDER BY 组合、排序键不在输出列报错 | M-S3 |

- `test/e2e/sql/CLAUDE.md` 增补 select 目录说明；`test/CMakeLists.txt` 逐文件 add_test。
- 不新增 storage 用例：table_meta 为只读转发，Scanner 语义未动。

## 12. 限制与后续方向

- 单表查询；JOIN（嵌套循环算子 + 限定名列 `t.col`）、聚合与 GROUP BY（有状态求值）、子查询（可重入执行框架）各自另立设计。
- 无索引（存储层现状）：WHERE/ORDER BY 均为全表扫描 + 内存排序，功能正确、性能受限。
- scan() 不加锁：并发 DML/DDL 期间的 SELECT 为未定义行为（存储既有约定）。
- 结果集单帧物化，超大结果集受服务端内存限制。
- ORDER BY 不支持位置序号与任意表达式；别名仅显式 AS；SELECT 投影不支持 bool 结果列。
