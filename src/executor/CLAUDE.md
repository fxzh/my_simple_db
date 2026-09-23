# 执行层(executor 静态库，server 链接)
把 parser 产出的 AST 转成 catalog 门面调用，返回结构化执行结果(命令标签或结果集)；
错误经 DB_RAISE 记日志后当场抛异常，由调用方回客户端 "ERROR: <原因>"

# 文件
executor.h      唯一入口 exec::execute(db, stmt) + ExecResult：is_result_set=false 带命令标签
                (proto::CommandTag + count)，
                true 带结果集(col_names + rows, Value 的 monostate 即 NULL)
executor.cpp    语句分发 create/drop/insert/delete/select → catalog；drop/insert/delete 拦截保留表名
                (db_table/db_column)，select 可查元数据，create 由存储层按表已存在拒绝；
                表达式求值：EvalValue 为 null/bool/int64/double/string 的 variant，
                常量上下文(insert VALUES, 标识符引用禁止)与行上下文(select)共用同一求值，
                算术 NULL 传播(任一操作数为 NULL 结果为 NULL，短路于除零/溢出)，非 NULL 操作数须为数值，
                类型驱动提升(int/int 向零截断)，溢出/除零/INT64_MIN 取反当场报错，
                bool 不可作为存储或输出值；
                select 编译投影(列定位表/star 展开/输出列名按 别名>列名>表达式文本)后全表扫描逐行物化；
                delete 为全表删除，回删除行数
CMakeLists.txt  链接 sql_parser/catalog/storage(PUBLIC), log/common(PRIVATE)

# 注意
- 无自有状态，全部数据经 ct::Catalog 访问；CREATE 的列类型枚举映射与长度校验在本层完成
- select 无 where/排序/distinct(M-S1 仅投影)，delete 无条件删除，随里程碑扩展
