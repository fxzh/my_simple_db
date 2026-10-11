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
SELECT * FROM t_fd ORDER BY f;
-- float 列等值比较: 字面量语境收窄为单精度后命中
SELECT * FROM t_fd WHERE f = 0.1;
-- float 域算术(整型字面量)逐运算单精度舍入
SELECT (f * 3) / 3 FROM t_fd WHERE f = 0.1;
-- float 与 double 混合算术提升 double 域
SELECT f + d FROM t_fd WHERE f = 0.1;
-- UPDATE 写入 float 列: 字面量静态拒绝
UPDATE t_fd SET f = 390000000000000000000000000000000000000.0 WHERE f = 0.1;
-- UPDATE 从 double 表达式取值写入 float 列: 运行期收窄
UPDATE t_fd SET f = d + 1.5 WHERE f = 0.1;
SELECT * FROM t_fd WHERE f = 1.6;
-- 运行期收窄越出 float 可表示范围报错
INSERT INTO t_fd VALUES (NULL, 390000000000000000000000000000000000000.0);
UPDATE t_fd SET f = d WHERE d = 390000000000000000000000000000000000000.0;
DROP TABLE t_fd;
