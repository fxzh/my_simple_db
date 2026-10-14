-- numeric/decimal 列声明形态
CREATE TABLE t_num_bare (a numeric);
DROP TABLE t_num_bare;
CREATE TABLE t_num_p (a numeric(5));
DROP TABLE t_num_p;
-- 声明 (精度,标度) 列: 赋值按标度舍入, 超精度拒绝
CREATE TABLE t_num_ps (a numeric(5, 2), b decimal(10, 4));
INSERT INTO t_num_ps (a) VALUES (5);
INSERT INTO t_num_ps (a) VALUES (1.5);
INSERT INTO t_num_ps (a) VALUES (1.555);
INSERT INTO t_num_ps (a) VALUES (1234.5);
SELECT * FROM t_num_ps;
UPDATE t_num_ps SET a = 2.005 WHERE a = 1.56;
UPDATE t_num_ps SET b = a;
SELECT * FROM t_num_ps;
DROP TABLE t_num_ps;
-- 裸 numeric: 标度随值, 尾零保留
CREATE TABLE t_num_scale (a numeric);
INSERT INTO t_num_scale VALUES (1.5), (1.50), (100), (-0.125);
SELECT * FROM t_num_scale;
SELECT a + 0.000 FROM t_num_scale;
DROP TABLE t_num_scale;
-- 算术: 纯 numeric 精确, 除法按推导标度舍入, 整型除法不受影响
SELECT 0.1 + 0.2, 1.50 + 1.5, 1.5 * 1.5, 10.00 / 4;
SELECT CAST(1 AS numeric) / 3, 7 / 2;
CREATE TABLE t_num_arith (a numeric(10, 4), b numeric(6, 2));
INSERT INTO t_num_arith VALUES (10.0000, 3.00);
SELECT a + b, a * b, a / b FROM t_num_arith;
DROP TABLE t_num_arith;
-- 混合: 整型保持 numeric, 浮点提升 Double(近似)
SELECT 1 + 0.5, 1e1 + 0.5, CAST(0.1 AS double) + 0.2;
-- 溢出与除零
SELECT 9999999999999999999999999999999999999.9 * 10.0;
SELECT 1.0 / 0.0;
-- 比较: numeric/整型精确路径
SELECT 0.1 + 0.2 = 0.3, 1.0 = 1, 1.50 < 1.5;
CREATE TABLE t_num_cmp (v numeric(10, 2));
INSERT INTO t_num_cmp VALUES (1.50), (2.50), (NULL);
SELECT v FROM t_num_cmp WHERE v > 1.5 ORDER BY v;
SELECT v FROM t_num_cmp WHERE v BETWEEN 1.0 AND 2.0 ORDER BY v;
SELECT v FROM t_num_cmp ORDER BY v;
DROP TABLE t_num_cmp;
-- 负号/abs/NULL 传播
SELECT -1.50, abs(-1.50), abs(1.5 + NULL);
-- 显式转换: numeric 目标舍入, 整型目标四舍五入
SELECT CAST(1 AS numeric(5, 2)), CAST('1.555' AS numeric(5, 2)), CAST(999.99 AS numeric(4, 2));
SELECT CAST(1.5 AS int), CAST(1.4 AS int), CAST(-1.5 AS int), CAST(2.5 AS bigint);
SELECT CAST(1.23456 AS double), CAST(0.1 AS varchar), CAST('1.5' AS double);
-- 精度/标度域校验: 超上限/标度超精度/精度为零
CREATE TABLE t_num_bad (a numeric(39));
CREATE TABLE t_num_bad (a numeric(5, 6));
CREATE TABLE t_num_bad (a numeric(0));
-- 非法参数形态为语法错误: 空参数/三参数/负数
CREATE TABLE t_numeric (a numeric());
CREATE TABLE t_numeric (a numeric(5, 2, 3));
CREATE TABLE t_numeric (a numeric(-5));
