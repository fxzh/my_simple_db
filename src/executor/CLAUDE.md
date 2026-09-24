# executor
执行层：语句先经 ana::analyze 绑定、pl::build 生成计划，再把计划节点树转成 catalog 调用，返回命令标签或结果集

- 求值器两上下文共用同一实现：常量上下文(insert VALUES，禁止标识符引用)与行上下文(select 投影/WHERE)
- 已知接受：int64 与 double 混合比较经 double 提升存在精度损失
- select 暂无排序/distinct，随里程碑扩展
