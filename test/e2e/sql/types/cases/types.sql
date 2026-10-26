CREATE TABLE t_types (id bigint, c char(5), v varchar(10));
INSERT INTO t_types VALUES (9223372036854775807, 'abc', 'hello');
INSERT INTO t_types VALUES (1, 'abcde', '0123456789');
INSERT INTO t_types VALUES (2, 'abcdef', 'x');
INSERT INTO t_types VALUES (3, 'ok', '01234567890');
SELECT * FROM t_types ORDER BY id;
DROP TABLE t_types;
-- float/double 列: 单精度显示保真与取值边界
CREATE TABLE t_fd (f float, d double);
INSERT INTO t_fd VALUES (0.1, 0.1), (1.0, 1.0), (-2.5, -2.5), (NULL, NULL);
INSERT INTO t_fd VALUES (1, 2.0);
INSERT INTO t_fd VALUES (390000000000000000000000000000000000000.0, 1.0);
SELECT * FROM t_fd ORDER BY f, d;
-- float 列与 double 字面量提升 double 域精确比较: 非精确字面量不命中, 精确字面量与写足位数命中
SELECT * FROM t_fd WHERE f = 0.1;
SELECT * FROM t_fd WHERE f = 1.0 ORDER BY d;
SELECT * FROM t_fd WHERE f = 0.10000000149011612;
-- float 列区间比较同域提升: 0.1f 提升后大于 double 0.1
SELECT * FROM t_fd WHERE f > 0.1 AND f <= 1.0 ORDER BY f, d;
-- float 域算术(整型字面量)逐运算单精度舍入
SELECT (f * 3) / 3 FROM t_fd WHERE f > -3.0 ORDER BY f;
-- float 与 double 混合算术提升 double 域
SELECT f + d FROM t_fd WHERE f < 0.5 ORDER BY f;
-- UPDATE 写入 float 列: 字面量静态拒绝
UPDATE t_fd SET f = 390000000000000000000000000000000000000.0 WHERE f = 1.0;
-- UPDATE 从 double 表达式取值写入 float 列: 运行期收窄
UPDATE t_fd SET f = d + 1.5 WHERE f = 1.0;
SELECT * FROM t_fd WHERE f = 2.5;
-- 运行期收窄越出 float 可表示范围报错
INSERT INTO t_fd VALUES (NULL, 390000000000000000000000000000000000000.0);
UPDATE t_fd SET f = d WHERE d = 390000000000000000000000000000000000000.0;
DROP TABLE t_fd;
-- int 列收 bigint 值: 域内成功, 超界运行期报错
CREATE TABLE t_ib (i int, b bigint);
INSERT INTO t_ib VALUES (1, 20000000000);
UPDATE t_ib SET i = b;
UPDATE t_ib SET i = b / 1000000000;
SELECT * FROM t_ib;
DROP TABLE t_ib;
-- float 列收 numeric 值与 double 值运行期收窄越界
CREATE TABLE t_fn (f float, n numeric);
INSERT INTO t_fn VALUES (CAST(2.5 AS NUMERIC), 4);
UPDATE t_fn SET f = n;
SELECT * FROM t_fn;
UPDATE t_fn SET f = CAST('3.9e38' AS DOUBLE);
SELECT * FROM t_fn;
DROP TABLE t_fn;
-- 非关键字类型名与无参类型带参: 均为语法错(类型名为关键字)
CREATE TABLE t_bad (x foo);
CREATE TABLE t_bad (x int(5));
-- 整型写入浮点列: 常量与列值均按目标精度舍入
CREATE TABLE t_assign_fp (id int, i int, b bigint, f float, d double);
INSERT INTO t_assign_fp VALUES (1, 16777217, 9007199254740993, 16777217, 9007199254740993);
INSERT INTO t_assign_fp VALUES (2, 7, 7, CAST(7 AS BIGINT), 7);
SELECT id, f, d FROM t_assign_fp ORDER BY id;
UPDATE t_assign_fp SET f = i, d = b;
SELECT id, f, d FROM t_assign_fp ORDER BY id;
UPDATE t_assign_fp SET f = b, d = i;
SELECT id, f, d FROM t_assign_fp ORDER BY id;
UPDATE t_assign_fp SET f = 1, d = CAST(7 AS BIGINT) WHERE id = 1;
UPDATE t_assign_fp SET f = NULL, d = NULL WHERE id = 2;
SELECT id, f, d FROM t_assign_fp ORDER BY id;
DROP TABLE t_assign_fp;
-- bigint 写入 int 列: 常量、常量表达式和列值采用相同的 int32 范围检查
CREATE TABLE t_assign_int (id int, i int, b bigint);
INSERT INTO t_assign_int VALUES (1, CAST(7 AS BIGINT), 7);
INSERT INTO t_assign_int VALUES (2, 5000000000 / 1000000000, 5);
INSERT INTO t_assign_int VALUES (3, CAST(2147483647 AS BIGINT), 2147483647);
INSERT INTO t_assign_int VALUES (4, CAST(-2147483648 AS BIGINT), -2147483648);
UPDATE t_assign_int SET i = CAST(7 AS BIGINT) WHERE id = 1;
UPDATE t_assign_int SET i = 5000000000 / 1000000000 WHERE id = 2;
UPDATE t_assign_int SET i = b;
SELECT * FROM t_assign_int ORDER BY id;
INSERT INTO t_assign_int (id, i) VALUES (5, 2147483648);
INSERT INTO t_assign_int (id, i) VALUES (6, -2147483649);
UPDATE t_assign_int SET i = CAST(2147483648 AS BIGINT) WHERE id = 1;
UPDATE t_assign_int SET i = CAST(-2147483649 AS BIGINT) WHERE id = 1;
UPDATE t_assign_int SET b = 2147483648 WHERE id = 1;
UPDATE t_assign_int SET i = b WHERE id = 1;
UPDATE t_assign_int SET b = -2147483649 WHERE id = 1;
UPDATE t_assign_int SET i = b WHERE id = 1;
SELECT * FROM t_assign_int ORDER BY id;
DROP TABLE t_assign_int;
-- 文本赋值: char 目标补空格, char 写入 varchar 去尾空格且保留左空格
CREATE TABLE t_assign_text (id int, c char(5), v varchar(3), src char(7), sv varchar(7));
INSERT INTO t_assign_text VALUES (1, CAST('a' AS CHAR(5)), CAST('a  ' AS CHAR(5)), ' a', 'xy');
INSERT INTO t_assign_text VALUES (2, CAST('xy' AS VARCHAR(5)), CAST('   ' AS CHAR(5)), '', '');
SELECT id, c, v, v = 'a' AS trimmed, v = '' AS empty FROM t_assign_text ORDER BY id;
UPDATE t_assign_text SET c = CAST('b' AS CHAR(5)), v = CAST(' b  ' AS CHAR(5)) WHERE id = 1;
SELECT id, c, v, v = ' b' AS trimmed FROM t_assign_text ORDER BY id;
UPDATE t_assign_text SET v = src;
UPDATE t_assign_text SET c = sv;
UPDATE t_assign_text SET c = c;
SELECT id, c, v, v = ' a' AS trimmed, v = '' AS empty FROM t_assign_text ORDER BY id;
UPDATE t_assign_text SET v = c;
UPDATE t_assign_text SET c = v;
SELECT id, c, v, v = 'xy' AS trimmed, v = '' AS empty FROM t_assign_text ORDER BY id;
-- 文本超目标长度: 常量与列值均拒绝, 失败后原值保留
INSERT INTO t_assign_text (id, c) VALUES (3, CAST('abcdef' AS CHAR(7)));
INSERT INTO t_assign_text (id, v) VALUES (4, CAST('abcd' AS CHAR(7)));
UPDATE t_assign_text SET c = CAST('abcdef' AS CHAR(7)) WHERE id = 1;
UPDATE t_assign_text SET v = CAST('abcd' AS CHAR(7)) WHERE id = 1;
UPDATE t_assign_text SET src = 'abcdef', sv = 'abcd' WHERE id = 1;
UPDATE t_assign_text SET c = src WHERE id = 1;
UPDATE t_assign_text SET v = src WHERE id = 1;
UPDATE t_assign_text SET v = sv WHERE id = 1;
SELECT id, c, v, v = '' AS empty FROM t_assign_text ORDER BY id;
DROP TABLE t_assign_text;
