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
DROP TABLE t_fd;
