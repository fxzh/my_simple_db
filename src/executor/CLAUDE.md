# executor
执行层：语句先经 ana::analyze 绑定、pl::build 生成计划、pl::optimize 优化，查询子树经迭代子算子(open/next/close)逐节点拉取执行，叶子计划(DDL/INSERT)直接转 catalog 调用，返回命令标签或流式结果集(SELECT 返回已 open 的算子，由调用方逐行拉取后 close)

- 算子工厂(operator.cpp)是执行器唯一的节点类型分派点；执行器不感知计划形态，形态由 planner 决定
- 隐式契约：谓词/投影的列下标语义 = 其子节点的输出行(单表阶段即基表行)；接入 Join 前需在 analyzer 层按节点输出 schema 绑定下标
- 表达式求值经 expr 模块：WHERE/投影/UPDATE 赋值为行上下文，INSERT VALUES 为常量上下文
- select 暂无排序/distinct，随里程碑扩展
- EXPLAIN(explain.cpp)把被解释语句的计划树渲染为 PG 风格文本行结果集，不执行被解释语句；顶层 Explain 节点不渲染自身，嵌套 Explain 显示为普通节点；谓词/赋值的列下标经表元数据回填列名
