-- initdb 自举 SQL: 经 bootstrap 模式 server 执行, 为系统元数据表指定保留段 table_id
set table_id = 4;
create table db_index(index_id bigint not null,
                      index_name varchar(64) not null,
                      table_id bigint not null,
                      ordinal int not null);

-- db_version 完成标记表永远位于文件结尾
set table_id = 5;
create table db_version(version bigint not null);
