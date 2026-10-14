# expr
表达式求值层：对绑定表达式树(ana::BoundExpr)求值，planner(常量折叠)与 executor(运行期)共用

- 已知接受：int64 与 double 混合比较经 double 提升存在精度损失；Float 域算术含整型操作数时经 int64→float 单次舍入存在精度损失；numeric 与 Float/Double 混合运算/比较提升 Double(PG 对齐行为)
- numeric 除法逐位长除到推导标度(max(s1,s2)+4)后四舍五入，禁止先移位放大再除(移位中间量可超 int256)
