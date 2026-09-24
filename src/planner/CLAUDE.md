# 计划层(planner 静态库，executor 链接)
把 analyzer 产出的 BoundStmt 转成计划节点树(PlanNode)，现阶段逻辑计划即物理计划；
纯结构变换，不访问 catalog、不求值

# 文件
planner.h         唯一入口 pl::build(bound) → unique_ptr<PlanNode>
plan.h            计划层类型：PlanKind 与 PlanNode 系列——SeqScanPlan(全表扫描)、
                 FilterPlan(谓词+行结构)、ProjectPlan(投影列+行结构)、InsertPlan(值表达式)、
                 DeletePlan(child 空即全表删除)、CreateTablePlan/DropTablePlan(叶子)；
                 表达式指针指向 AST 原节点，生命周期由 execute() 调用期持有的语句保证
planner.cpp       计划分发：select → Project(Filter(SeqScan))(无 WHERE 省 Filter)，
                 delete 带 WHERE → Delete(Filter(SeqScan))、无 WHERE 为无 child 叶子，
                 其余语句转叶子计划
CMakeLists.txt   链接 analyzer(PUBLIC), log/common(PRIVATE)

# 注意
- 优化器为将来的独立 pass，插入点在 pl::build 之后
