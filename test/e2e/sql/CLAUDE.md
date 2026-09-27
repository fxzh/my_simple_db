sql 用例按功能分目录

# ddl
表结构定义

# dml
数据行增删: insert(含 NULL 插入与 NULL 参与算术), delete(无条件全删与条件删除)

# select
单表查询

# where
条件过滤: 基础比较与逻辑运算 / NULL 三值逻辑 / char 与 varchar 比较语义

# proto
协议层

# types
列类型

# reserved
保留表拦截

# semantic
编译期语义检查, 列不存在/表不存在等错误统一在此覆盖
