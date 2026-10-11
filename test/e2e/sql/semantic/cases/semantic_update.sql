UPDATE no_table SET a = 1;
UPDATE system.db_table SET table_name = 'x';
CREATE TABLE t_sem (id int NOT NULL, name varchar(8), score double);
UPDATE t_sem SET no_col = 1;
UPDATE t_sem SET id = 1, id = 2;
UPDATE t_sem SET id = NULL;
UPDATE t_sem SET id = 'abc';
UPDATE t_sem SET id = 1.5;
UPDATE t_sem SET name = 'abcdefghijk';
UPDATE t_sem SET id = id > 1;
UPDATE t_sem SET id = 1 WHERE id + 1;
DROP TABLE t_sem;
-- 函数右值类型检查, 绑定通过后执行期报未实现
CREATE TABLE t_suf (i int, s char(4));
INSERT INTO t_suf VALUES (1, 'a');
UPDATE t_suf SET i = abs('x');
UPDATE t_suf SET s = abs(1);
UPDATE t_suf SET i = abs(1);
DROP TABLE t_suf;
