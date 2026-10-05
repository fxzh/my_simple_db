-- initdb 自举 SQL: 经 bootstrap 模式 server 执行, 为系统元数据表指定保留段 table_id
-- 伴生会话 current_schema 缺省 public, 建表语句须全部显式限定 system.
create schema public;

set table_id = 4;
create table system.db_index(table_id bigint not null,
                             index_name varchar(64) not null,
                             col_ordinal int not null,
                             file_id bigint not null);

-- db_version 完成标记表永远位于文件结尾
set table_id = 5;
create table system.db_version(version bigint not null);
