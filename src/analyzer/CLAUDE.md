# analyzer
语义分析层：把 parser 产出的 AST 经 catalog 元数据绑定为 BoundStmt

- BoundStmt 的表达式指针指向 AST 原节点，生命周期由 execute() 调用期持有的语句保证(planner 的 PlanNode 同理)
- 保留表策略：db_table/db_column 的 drop/insert/delete 拒绝、create 报已存在、select 可查
- 校验分层：名字解析与表达式类型推导均在本层编译期完成(空表也报)；WHERE 须布尔、投影禁常量布尔、INSERT 值类型/长度/范围/NOT NULL/个数/常量性均在本层报；执行期仅留数据相关错误(溢出/除零)
