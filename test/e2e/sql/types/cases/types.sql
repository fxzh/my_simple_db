CREATE TABLE t_types (id bigint, c char(5), v varchar(10));
INSERT INTO t_types VALUES (9223372036854775807, 'abc', 'hello');
INSERT INTO t_types VALUES (1, 'abcde', '0123456789');
INSERT INTO t_types VALUES (2, 'abcdef', 'x');
INSERT INTO t_types VALUES (3, 'ok', '01234567890');
SELECT * FROM t_types;
DROP TABLE t_types;
