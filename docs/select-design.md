# SELECT 支持设计文档（剩余项：DISTINCT 与 LIMIT/OFFSET）

## 1. 背景与范围

单表 SELECT 的其余子集均已落地：`*`/列投影/算术与常量表达式、显式 AS 别名、WHERE 三值逻辑
（比较/AND/OR/NOT/IS NULL/BETWEEN/NULL 与布尔字面量）、ORDER BY（序号/输出列名/别名/任意表达式键）、
无 FROM 的 SELECT、索引扫描与 EXPLAIN。本文档只设计尚未实现的两项，目标语法增量：

```sql
SELECT [DISTINCT] 投影列表 FROM 表名 [WHERE] [ORDER BY ...] [LIMIT n [OFFSET m]]
SELECT 投影列表 [WHERE] [ORDER BY ...] [LIMIT n [OFFSET m]]      -- 无 FROM 形态
```

不在本设计范围内（各自另立设计）：多表 JOIN、聚合与 GROUP BY/HAVING、子查询、内置函数、
隐式类型转换、`LIMIT o, n` 形式。

无新增第三方依赖。

## 2. 现状锚点

```
client: 三帧流式接收(ResultSetHead/Batch/End) → 物化渲染
server session.cpp: ExecResult.is_result_set → send_result_stream 逐行拉取累积成批发帧(256 行/批)
executor: execute = ana::analyze → pl::build → pl::optimize → make_operator → open
          (查询子树为迭代算子 open/next/close, 叶子计划直接转 catalog 调用)
planner:  PlanNode 树, select 形如 [Sort](Project([Filter](SeqScan | Fetch(IndexScan))))
          优化 pass: 常量折叠/布尔化简/NOT 消除/过滤器剪除/索引选择
analyzer: AST + catalog 元数据 → BoundStmt(名字解析/类型检查/投影展开/排序键定位)
expr:     绑定表达式求值(eval_const/eval_row/where_match)与值三向比较 value_cmp(st::Value)
```

SelectStmt 已带 distinct_/limit_/offset_ 成员但文法未接入（构造恒 false/nullopt）；
SelectNoFromStmt 三者皆无。proto/session/client 本设计零改动。

## 3. 词法与文法

### 3.1 lexer.l 新增

关键字（沿用大小写不敏感风格）：

```
distinct  limit  offset
```

### 3.2 parser.y 文法增量

token 声明新增 `DISTINCT LIMIT OFFSET`；variant 语义值新类型
`std::pair<std::optional<long long>, std::optional<long long>>` 依赖的 optional/pair
头 ast.hh 已包含。

```
select_statement:
    SELECT distinct_opt select_list FROM qualified_name where_opt order_by_opt limit_opt {
        $$ = std::make_unique<SelectStmt>(std::move($5), $2, std::move($3), std::move($6),
                                         std::move($7), $8.first, $8.second);
    }
  | SELECT select_list where_opt order_by_opt limit_opt {
        $$ = std::make_unique<SelectNoFromStmt>(std::move($2), std::move($3), std::move($4),
                                                $5.first, $5.second);
    }
  ;

distinct_opt:  /* 空 */ { $$ = false; } | DISTINCT { $$ = true; } ;

limit_opt:  /* 空 */ { $$ = {}; }
  | LIMIT INTEGER { $$ = { std::optional<long long>($2), std::nullopt }; }
  | LIMIT INTEGER OFFSET INTEGER { $$ = { std::optional<long long>($2), std::optional<long long>($4) }; }
  ;
```

### 3.3 语法层语义限制

- LIMIT/OFFSET 仅接受非负整数字面量（INTEGER 不带符号，语法层即约束）；
  OFFSET 不可脱离 LIMIT 单独出现（limit_opt 无此产生式）。
- 无 FROM 形态不接入 DISTINCT：恒输出一行，去重无效果；其星号投影仍被 StarNoFrom 拦截（现状不变）。

## 4. 语义分析（analyzer）

- BoundSelect 新增 `bool distinct` 与 `std::optional<uint64_t> limit/offset` 透传；
  BoundSelectNoFrom 新增 limit/offset（LIMIT 0 对恒一行行源同样有语义）。
- DISTINCT 时 ORDER BY 键必须命中输出列（整数序号或输出列名，含别名），走行上下文绑定的键
  （未命中输出列的列名/任意表达式）一律报 UnknownColumn：去重先于排序发生，行上下文键在
  去重后无从取值。现有 resolve_order_key 的输出列命中路径即该约束的载体。
- LIMIT/OFFSET 非负由语法保证，绑定期直接折入计划。

## 5. 计划与执行（planner/executor）

挂载顺序（外 → 内）：Limit → Distinct → Project →（Filter → Scan）。
现状 Sort 两种形态（sort_on_output 投影后 / 行上下文投影前）不变；DISTINCT 时排序并入去重
算子（见 5.2），不再叠加独立 Sort。

### 5.1 LIMIT/OFFSET

- 新计划节点 `LimitPlan{child, limit, offset}`（均为 optional，缺省 nullopt）。
- 算子 LimitOp：next 跳过前 offset 行，其后放行至多 limit 行；流式计数，无须物化。
- EXPLAIN 渲染 `Limit` 行。

### 5.2 DISTINCT

- 新计划节点 `DistinctPlan{child}`，挂 Project 之上，对输出行去重。
- 算子 DistinctOp：物化子树全部输出行，按 [ORDER BY 键（命中输出列的下标序列，无 ORDER BY
  时为空）..., 全部输出列...] 排序后相邻去重，输出即满足 ORDER BY 序——排序与去重一次完成。
- 判等语义：判等用 expr::value_cmp == 0，同族数值相等即重复（1 与 1.0 视为重复）；
  双 NULL 相等，NULL 归并为一个值（PG 行为）。
- 无 ORDER BY 时排序键为空，输出按全部输出列值全序，结果确定；SortOp 同键行次序不保证的
  问题因全部输出列并入排序键而消失（同键行彼此全等）。

## 6. 错误码

无新增错误码。DISTINCT 时 ORDER BY 键未命中输出列 → UnknownColumn（analyzer）。

## 7. 测试计划

test/e2e/sql/select/ 新增用例（经 test/CMakeLists.txt 的 add_sql_case 注册，编写遵循
ai_docs/testcase.md）：

| 文件 | 覆盖 |
|------|------|
| select_distinct.sql | 去重、与 ORDER BY 组合、排序键不在输出列报错、NULL 归并、1 与 1.0 视为重复、EXPLAIN 渲染 |
| select_limit.sql | LIMIT/OFFSET、LIMIT 0、与 ORDER BY/DISTINCT 组合、无 FROM 形态、OFFSET 脱离 LIMIT 语法报错 |

## 8. 限制与后续方向

- DISTINCT 为物化排序去重，内存上限为全部输出行。
- LIMIT 仅整数字面量，不支持 `LIMIT o, n` 形式与表达式。
- 无 FROM 形态不支持 DISTINCT（恒一行，无语义）。
