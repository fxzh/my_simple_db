# analyzer
绑定层：把 parser 产出的 AST 经 catalog 元数据绑定为 BoundStmt

- BoundStmt 的表达式指针指向 AST 原节点，生命周期由 execute() 调用期持有的语句保证(planner 的 PlanNode 同理)
- 保留表策略：db_table/db_column 的 drop/insert/delete 拒绝、create 报已存在、select 可查
- 校验分层：仅 WHERE 在本层编译期校验(空表也报未知列)；select 投影列留待执行期逐行求值时报错(维持现状)
