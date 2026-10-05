-- create/drop schema 生命周期与重名/保护行为
CREATE SCHEMA s_test;
CREATE SCHEMA s_test;
SELECT schema_name FROM system.db_schema;
DROP SCHEMA system;
DROP SCHEMA s_test;
DROP SCHEMA s_test;
-- 限定名建表挂所属 schema, 未限定名默认挂 current_schema(public), 表名 schema 内唯一
CREATE SCHEMA s_q;
CREATE TABLE s_q.t_q (id int not null, name varchar(8));
CREATE TABLE s_q.t_q (id int not null, name varchar(8));
CREATE TABLE t_q (id int not null, name varchar(8));
INSERT INTO s_q.t_q VALUES (1, 'a'), (2, 'b');
INSERT INTO t_q VALUES (9, 'z');
UPDATE s_q.t_q SET name = 'x' WHERE id = 1;
DELETE FROM s_q.t_q WHERE id = 2;
SELECT * FROM s_q.t_q;
SELECT * FROM t_q;
SELECT * FROM public.t_q;
SELECT * FROM system.t_q;
DROP SCHEMA s_q;
DROP TABLE s_q.t_q;
DROP TABLE t_q;
DROP SCHEMA s_q;
-- 未限定名解析到 current_schema, schema 不存在当场报错
CREATE SCHEMA s_u;
CREATE TABLE s_u.t_u (id int);
SELECT * FROM t_u;
SELECT * FROM no_such.t_u;
CREATE TABLE no_such.t_u (id int);
DROP TABLE no_such.t_u;
DROP TABLE s_u.t_u;
DROP SCHEMA s_u;
-- 保留表拦截仅限 system 名下
CREATE SCHEMA s_r;
CREATE TABLE s_r.db_table (id int);
INSERT INTO s_r.db_table VALUES (1);
SELECT * FROM s_r.db_table;
DROP TABLE system.db_table;
DROP TABLE s_r.db_table;
DROP SCHEMA s_r;
-- current_schema 切换: 允许切向不存在的 schema, 未限定名随之后解析
SET current_schema = s_u;
CREATE TABLE t_cs (id int);
CREATE SCHEMA s_cs;
SET current_schema = s_cs;
CREATE TABLE t_cs (id int);
INSERT INTO t_cs VALUES (1);
SELECT * FROM t_cs;
SET current_schema = public;
DROP TABLE t_cs;
DROP TABLE s_cs.t_cs;
DROP SCHEMA s_cs;
-- public 可删除, 删除后未限定名报 schema 不存在, 重建后恢复
DROP SCHEMA public;
CREATE TABLE t_p (id int);
CREATE SCHEMA public;
CREATE TABLE t_p (id int);
INSERT INTO t_p VALUES (1);
SELECT * FROM t_p;
DROP TABLE t_p;
