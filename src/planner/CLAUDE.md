# planner
计划层：把 analyzer 产出的 BoundStmt 转成计划节点树(PlanNode)，现阶段逻辑计划即物理计划

- 纯结构变换：不访问 catalog、不求值(求值在 executor)
- 优化器为将来的独立 pass，插入点在 pl::build 之后
