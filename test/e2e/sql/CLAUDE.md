sql用例功能目录说明

# ddl
表结构定义（create / drop table）

# dml
数据行增删（insert / delete）

# select
单表查询（M-S1：select 投影 / 别名 / 常量表达式；where、排序、distinct 随后续里程碑扩展）

# proto
协议层（超长 SQL 整帧收发不截断）

# types
列类型（bigint、char(n) 定长补空格、varchar(n) 变长、超长拒绝）

# reserved
保留表拦截（db_table/db_column 的 drop/insert/delete 一律拒绝；create 报表已存在；select 可查元数据）
