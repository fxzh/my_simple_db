# expr
表达式求值层：对绑定表达式树(ana::BoundExpr)求值，planner(常量折叠)与 executor(运行期)共用

- 已知接受：int64 与 double 混合比较经 double 提升存在精度损失
