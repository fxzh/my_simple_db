UPDATE no_table SET a = 1;
UPDATE system.db_table SET table_name = 'x';
CREATE TABLE t_sem (id int NOT NULL, name varchar(8), score double);
UPDATE t_sem SET no_col = 1;
UPDATE t_sem SET id = 1, id = 2;
-- 空表更新不产生待检查的列值
UPDATE t_sem SET id = NULL;
UPDATE t_sem SET id = 'abc';
UPDATE t_sem SET id = 1.5;
UPDATE t_sem SET name = 'abcdefghijk';
UPDATE t_sem SET id = id > 1;
UPDATE t_sem SET id = 1 WHERE id + 1;
DROP TABLE t_sem;
-- 函数右值类型检查
CREATE TABLE t_suf (i int, s char(4));
INSERT INTO t_suf VALUES (1, 'a');
UPDATE t_suf SET i = abs('x');
UPDATE t_suf SET s = abs(1);
DROP TABLE t_suf;
-- 常量与列引用的 NOT NULL 检查均按实际结果执行
CREATE TABLE t_null_update (id int, i int, n numeric, np numeric(6,2), flag boolean NOT NULL, src boolean);
INSERT INTO t_null_update VALUES (1, 10, 1.25, 1.25, false, true), (2, 20, 2.50, 2.50, true, false);
UPDATE t_null_update SET flag = false AND NULL;
SELECT id, flag FROM t_null_update ORDER BY id;
UPDATE t_null_update SET flag = true OR NULL;
UPDATE t_null_update SET flag = NULL AND false WHERE id = 1;
UPDATE t_null_update SET flag = NULL OR true WHERE id = 2;
SELECT id, flag FROM t_null_update ORDER BY id;
UPDATE t_null_update SET flag = true AND NULL;
UPDATE t_null_update SET flag = false OR NULL;
UPDATE t_null_update SET flag = CAST(NULL AS boolean);
UPDATE t_null_update SET flag = src OR NULL;
SELECT id, flag FROM t_null_update ORDER BY id;
UPDATE t_null_update SET flag = src OR NULL WHERE src;
UPDATE t_null_update SET flag = src AND NULL WHERE NOT src;
SELECT id, flag FROM t_null_update ORDER BY id;
-- 常量、有类型 NULL 与列引用均遵循相同的赋值类型规则
UPDATE t_null_update SET i = true OR NULL;
UPDATE t_null_update SET i = CAST(NULL AS boolean);
UPDATE t_null_update SET i = src OR NULL;
UPDATE t_null_update SET n = true OR NULL;
UPDATE t_null_update SET np = true OR NULL;
UPDATE t_null_update SET n = CAST(NULL AS boolean);
UPDATE t_null_update SET np = CAST(NULL AS boolean);
UPDATE t_null_update SET n = src OR NULL;
UPDATE t_null_update SET np = src OR NULL;
UPDATE t_null_update SET flag = 1 + NULL;
-- 可空列接受裸 NULL 与赋值兼容的有类型 NULL
UPDATE t_null_update SET i = NULL, n = NULL, np = NULL WHERE id = 1;
UPDATE t_null_update SET i = CAST(NULL AS int), n = CAST(NULL AS int), np = CAST(NULL AS numeric) WHERE id = 2;
SELECT * FROM t_null_update ORDER BY id;
DROP TABLE t_null_update;
