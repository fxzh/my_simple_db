-- round/floor/ceil 执行: 结果域镜像实参域, NULL 严格传布
CREATE TABLE t_fm1 (i int, b bigint, f float, d double, n numeric(10,4));
INSERT INTO t_fm1 VALUES (-5, -9000000000, -1.5, -2.25, -123.4567);
INSERT INTO t_fm1 VALUES (3, 7, 1.5, 2.25, 123.4567);
INSERT INTO t_fm1 VALUES (2, NULL, NULL, NULL, NULL);
SELECT round(i), round(b), round(f), round(d), round(n) FROM t_fm1 ORDER BY i;
SELECT floor(i), floor(b), floor(f), floor(d), floor(n) FROM t_fm1 ORDER BY i;
SELECT ceil(i), ceil(b), ceil(f), ceil(d), ceil(n) FROM t_fm1 ORDER BY i;
-- mod 执行: 符号随被除数
SELECT f, mod(f, CAST(2 AS FLOAT)), d, mod(d, CAST(2 AS DOUBLE)) FROM t_fm1 ORDER BY i;
SELECT mod(7, 3), mod(-7, 3), mod(7, -3), mod(-7, -3);
SELECT i, mod(i, 3), b, mod(b, CAST(3 AS BIGINT)) FROM t_fm1 ORDER BY i;
SELECT i FROM t_fm1 WHERE mod(i, 2) = 1 ORDER BY i;
-- mod numeric: 结果标度取两操作数标度较大者
SELECT mod(7.5, 2.0), mod(-7.5, 2.0), mod(10.5, 3.0);
SELECT n, mod(n, n) FROM t_fm1 ORDER BY i;
CREATE TABLE t_fm2 (a numeric(10,4), b numeric(5,2));
INSERT INTO t_fm2 VALUES (10.5000, 3.00);
SELECT a, b, mod(a, b) FROM t_fm2;
DROP TABLE t_fm2;
-- round 双参: 结果标度 n, n 不小于原标度时原值返回, 负 n 舍入到十位
SELECT n, round(n, 2), round(n, 0), round(n, -1), round(n, 4), round(n, 6) FROM t_fm1 ORDER BY i;
SELECT round(0.5), round(2.5), round(-0.5), round(-2.5), round(1.234, 2), round(1.235, 2);
SELECT round(55.0, -1), round(-55.0, -1), round(54.0, -1), round(50.0, -1);
-- bigint 最小值对 -1 取余数学结果为 0
CREATE TABLE t_fm3 (b bigint);
INSERT INTO t_fm3 VALUES (-9223372036854775807 - 1);
SELECT mod(b, CAST(-1 AS BIGINT)), mod(b, CAST(1 AS BIGINT)) FROM t_fm3;
DROP TABLE t_fm3;
-- 标度 39 的 numeric 值幅值小于 1: 取整与舍入边界
SELECT floor(0.0000000000000000000000000000000000000001), ceil(0.0000000000000000000000000000000000000001);
SELECT floor(-0.0000000000000000000000000000000000000001), ceil(-0.0000000000000000000000000000000000000001);
SELECT round(0.0000000000000000000000000000000000000001, 0);
-- 除零: 整型/浮点/numeric 取余除零
SELECT mod(5, 0);
SELECT mod(CAST(5.5 AS DOUBLE), CAST(0 AS DOUBLE));
SELECT mod(5.5, 0.0);
-- 负 n 进位使 numeric 结果达 38 位十进制超界
SELECT round(99999999999999999999999999999999999999., -1);
-- 绑定失败: 双参 round 仅收 numeric 首参, mod 仅同型配对, ceiling 未注册
SELECT round(55, -1);
SELECT round(1.5, 2.5);
SELECT mod(7, 2.5);
SELECT mod(7);
SELECT round('x');
SELECT ceiling(1.5);
-- 常量调用计划期折叠为常量
SELECT round(1.5), floor(-1.5), ceil(-1.5), mod(7, 3);
EXPLAIN SELECT round(1.5);
EXPLAIN SELECT n FROM t_fm1 WHERE ceil(n) > 123;
-- 常量上下文求值
CREATE TABLE t_fm4 (n1 numeric(10,2), n2 numeric(10,2));
INSERT INTO t_fm4 VALUES (round(1.6), round(1.234, 2));
SELECT * FROM t_fm4;
UPDATE t_fm4 SET n2 = round(n2 * 3.0, 1);
SELECT * FROM t_fm4;
DROP TABLE t_fm4;
DROP TABLE t_fm1;
