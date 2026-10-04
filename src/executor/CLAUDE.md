# executor
执行层：语句先经 ana::analyze 绑定、pl::build 生成计划，查询子树经迭代子算子(open/next/close)逐节点拉取执行，叶子计划(DDL/INSERT)直接转 catalog 调用，返回命令标签或流式结果集(SELECT 返回已 open 的算子，由调用方逐行拉取后 close)

- 算子工厂(operator.cpp)是执行器唯一的节点类型分派点；执行器不感知计划形态，形态由 planner 决定
- 隐式契约：谓词/投影的列下标语义 = 其子节点的输出行(单表阶段即基表行)；接入 Join 前需在 analyzer 层按节点输出 schema 绑定下标
- 求值器两上下文共用同一实现：常量上下文(insert VALUES，禁止标识符引用)与行上下文(select 投影/WHERE、UPDATE 赋值)
- 类型与约束检查在语义分析层完成；求值器仅报数据相关错误(溢出/除零/浮点范围)，类型类分支为 Internal 防御
- 已知接受：int64 与 double 混合比较经 double 提升存在精度损失
- select 暂无排序/distinct，随里程碑扩展
