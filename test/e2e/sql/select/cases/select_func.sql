-- abs 执行: 结果域镜像数值实参域, NULL 严格传布
CREATE TABLE t_fa (i int, b bigint, f float, d double);
INSERT INTO t_fa VALUES (-5, -9000000000, -1.5, -2.25);
INSERT INTO t_fa VALUES (3, NULL, NULL, NULL);
SELECT i, abs(i), b, abs(b), f, abs(f), d, abs(d) FROM t_fa ORDER BY i;
SELECT abs(i) + 1, abs(i + 10) FROM t_fa ORDER BY abs(i);
SELECT i FROM t_fa WHERE abs(i) > 3;
SELECT i FROM t_fa ORDER BY abs(i);
-- 常量调用计划期折叠为常量
SELECT abs(-1), abs(-1.5), abs(1 + NULL);
EXPLAIN SELECT abs(-1);
EXPLAIN SELECT i FROM t_fa WHERE abs(i) > 3;
-- 恒真函数谓词折叠后被剪除, 折叠期溢出在 EXPLAIN 即报
EXPLAIN SELECT 1 WHERE abs(-1) > 0;
EXPLAIN SELECT abs(-9223372036854775807 - 1);
-- 常量上下文求值
CREATE TABLE t_fb (i int);
INSERT INTO t_fb VALUES (abs(-7));
UPDATE t_fb SET i = abs(i);
SELECT i FROM t_fb;
DROP TABLE t_fb;
-- 溢出: bigint 最小值绝对值不可表示, 折叠期与执行期报错
SELECT abs(-9223372036854775807 - 1);
CREATE TABLE t_fc (i bigint);
INSERT INTO t_fc VALUES (-9223372036854775807 - 1);
SELECT abs(i) FROM t_fc;
DROP TABLE t_fc;
-- 溢出: int 域最小值绝对值超 int32 域, 执行期报错
CREATE TABLE t_fd (i int);
INSERT INTO t_fd VALUES (-2147483647 - 1);
SELECT abs(i) FROM t_fd;
DROP TABLE t_fd;
DROP TABLE t_fa;
