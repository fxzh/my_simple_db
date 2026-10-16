-- initdb 自举 SQL: 经 bootstrap 模式 server 执行, 为系统元数据表指定保留段 table_id
-- 伴生会话 current_schema 缺省 public, 建表语句须全部显式限定 system.
create schema public;

set table_id = 5;
create table system.db_index(table_id bigint not null,
                             index_name varchar(64) not null,
                             col_ordinal int not null,
                             file_id bigint not null);

set table_id = 6;
create table system.db_func(func_id bigint not null,
                            func_name varchar(64) not null,
                            arity int not null,
                            arg_0_type int not null,
                            arg_1_type int not null,
                            arg_2_type int not null,
                            ret_type int not null,
                            strict_null int not null);
insert into system.db_func values(1, 'abs', 1, 1, 0, 0, 1, 1);
insert into system.db_func values(2, 'abs', 1, 2, 0, 0, 2, 1);
insert into system.db_func values(3, 'abs', 1, 3, 0, 0, 3, 1);
insert into system.db_func values(4, 'abs', 1, 5, 0, 0, 5, 1);
insert into system.db_func values(5, 'abs', 1, 8, 0, 0, 8, 1);

-- db_version 完成标记表永远位于文件结尾
set table_id = 7;
create table system.db_version(version bigint not null);
