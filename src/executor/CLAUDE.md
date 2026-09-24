# 执行层(executor 静态库，server 链接)
语句先经 analyzer 绑定成 BoundStmt、planner 生成计划节点树，再转成 catalog 门面调用，返回结构化执行结果(命令标签或结果集)；
错误经 DB_RAISE 记日志后当场抛异常，由调用方回客户端 "ERROR: <原因>"

# 文件
executor.h      唯一入口 exec::execute(db, stmt) + ExecResult：is_result_set=false 带命令标签
                (proto::CommandTag + count)，
                true 带结果集(col_names + rows, Value 的 monostate 即 NULL)
executor.cpp    execute() 三段式：ana::analyze 绑定、pl::build 生成计划后按 PlanKind 分发；create/drop/insert 直调
                catalog(insert VALUES 经常量求值器取值)，select 解构 Project[Filter[SeqScan]] 计划
                全表扫描逐行物化(WHERE 逐行求值过滤，NULL 即 UNKNOWN 不满足)，delete 无 WHERE(无 child)全表删除，
                带 WHERE 抽干过滤子树收集行引用后逐个物理删除，回实际删除行数
expr_eval.h/.cpp 表达式求值器：EvalValue 为 null/bool/int64/double/StrVal(文本+char 列来源) 的 variant，
                常量上下文(insert VALUES, 标识符引用禁止)与行上下文(select 投影/WHERE)共用同一求值，
                算术 NULL 传播(任一操作数为 NULL 结果为 NULL，短路于除零/溢出)，非 NULL 操作数须为数值，
                类型驱动提升(int/int 向零截断)，溢出/除零/INT64_MIN 取反当场报错；
                比较/逻辑三值逻辑(NULL 即 UNKNOWN)：比较数值提升 double、字符串 PAD SPACE
                (char 定长列来源一侧去尾随空格)、跨类报错；逻辑/NOT 非 bool 操作数报错；
                bool 不可作为存储或输出值；行上下文类型为绑定层 ana::Schema(analyzer/bound.h)
CMakeLists.txt  链接 planner/analyzer/sql_parser/catalog/storage(PUBLIC), log/common(PRIVATE)

# 注意
- 无自有状态，全部数据经 ct::Catalog 访问；列类型映射/长度校验/保留表拦截/WHERE 列校验在 analyzer 层完成
- select 无排序/distinct，随里程碑扩展；int64 与 double 混合比较经 double 存在精度损失(已知接受)
