-- numeric/decimal 各声明形态: 语法层收下, 语义层拒绝
CREATE TABLE t_numeric (a numeric);
CREATE TABLE t_numeric (a numeric(5));
CREATE TABLE t_numeric (a numeric(5, 2));
CREATE TABLE t_numeric (a decimal);
CREATE TABLE t_numeric (a DECIMAL(10, 4));
CREATE TABLE t_numeric (a numeric(3), b decimal(4, 1));
-- cast 目标为 numeric/decimal 同样在语义层拒绝
SELECT CAST(1 AS numeric(5, 2));
SELECT CAST('1.5' AS decimal);
-- 非法参数形态为语法错误: 空参数/三参数/负数
CREATE TABLE t_numeric (a numeric());
CREATE TABLE t_numeric (a numeric(5, 2, 3));
CREATE TABLE t_numeric (a numeric(-5));
