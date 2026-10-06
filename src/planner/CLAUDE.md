# planner
计划层：把 analyzer 产出的 BoundStmt 转成计划节点树(PlanNode)，现阶段逻辑计划即物理计划

- pl::build 为纯结构变换：不访问 catalog、不求值
- pl::optimize 是计划优化唯一入口，由 executor 在 build 后调用，pass 在函数内串接
- 常量折叠用 expr 求值器把纯常量子树替换为常量节点，常量运算错误(溢出/除零)因此在计划期报错，EXPLAIN 渲染折叠后计划
