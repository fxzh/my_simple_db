# analyzer
语义分析层：把 parser 产出的 AST 经 catalog 元数据绑定为 BoundStmt

- BoundStmt 持有绑定表达式树(bound_expr.h)，名字已在绑定期解析为行内下标，绑定后不依赖 AST(planner 的 PlanNode 同理)
- DML 限定名在绑定期一次解析为表句柄(TableHandle：元数据+限定名文本)随 BoundStmt/PlanNode 带到执行期，语句事务锁保证解析结果到执行不变；DELETE 全表也在此解析(表不存在从执行期报错提前到绑定期)
- 未限定表名在绑定期填入会话 current_schema 后进入 TableRef，catalog 不再有"空 schema 默认 system"语义
- 保留表策略：仅 system 名下的 db_table/db_column/db_schema/db_index/db_version 的 drop/insert/delete/update 拒绝、create 报已存在、select 可查；其他 schema 同名表不受限
- 校验分层：名字解析与表达式类型推导均在本层编译期完成(空表也报)；WHERE 须布尔、INSERT 值类型/长度/范围/NOT NULL/个数/常量性均在本层报；UPDATE 右值行上下文绑定、静态类型匹配与纯常量右值的 NOT NULL/长度在本层报；执行期仅留数据相关错误(溢出/除零)与 UPDATE 列引用右值的空值/超长/越界
