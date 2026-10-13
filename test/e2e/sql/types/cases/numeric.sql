-- numeric/decimal 列声明形态
CREATE TABLE t_num_bare (a numeric);
DROP TABLE t_num_bare;
CREATE TABLE t_num_p (a numeric(5));
DROP TABLE t_num_p;
CREATE TABLE t_num_ps (a numeric(5, 2), b decimal(10, 4));
INSERT INTO t_num_ps (a) VALUES (5);
INSERT INTO t_num_ps (a) VALUES (1.5);
UPDATE t_num_ps SET a = 2;
UPDATE t_num_ps SET a = 1.5;
SELECT * FROM t_num_ps;
DROP TABLE t_num_ps;
-- 精度/标度域校验: 超上限/标度超精度/精度为零
CREATE TABLE t_num_bad (a numeric(39));
CREATE TABLE t_num_bad (a numeric(5, 6));
CREATE TABLE t_num_bad (a numeric(0));
-- cast 目标为 numeric/decimal
SELECT CAST(1 AS numeric(5, 2));
SELECT CAST('1.5' AS decimal);
-- 非法参数形态为语法错误: 空参数/三参数/负数
CREATE TABLE t_numeric (a numeric());
CREATE TABLE t_numeric (a numeric(5, 2, 3));
CREATE TABLE t_numeric (a numeric(-5));
