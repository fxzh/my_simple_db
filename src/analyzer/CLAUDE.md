# 绑定层(analyzer 静态库，executor 链接)
把 parser 产出的 AST 经 catalog 元数据绑定为 BoundStmt：类型映射、名字解析、投影展开；
错误经 DB_RAISE 记日志后当场抛异常

# 文件
analyzer.h       唯一入口 ana::analyze(db, stmt) → unique_ptr<BoundStmt>
bound.h          绑定层类型：Schema(列名定位表+char 定长列标记)、ProjCol(star 展开后投影列)、
                BoundKind 与 Bound* 系列语句结构；表达式指针指向 AST 原节点，
                生命周期由 execute() 调用期持有的语句保证
analyzer.cpp     绑定分发：create 做 DataType→st::ColType 映射与长度校验(显式长度 1..65535,
                未声明时 char 缺省 1)；drop/insert/delete 拦截保留表名(db_table/db_column)，
                select 可查元数据，create 由存储层按表已存在拒绝；select/delete 带 WHERE 时
                绑定 Schema 并校验标识符存在性(空表也报未知列)；select 投影按 star 展开/
                输出列名取别名>列名>表达式文本；insert 的值表达式留待执行期常量求值
CMakeLists.txt   链接 sql_parser/storage/catalog(PUBLIC), log/common(PRIVATE)

# 注意
- select 投影中的未知列不在本层校验(维持执行期逐行求值报错现状)，仅 WHERE 编译期校验
