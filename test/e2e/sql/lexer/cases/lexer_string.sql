CREATE TABLE t_ls (id int, val char(16));
INSERT INTO t_ls VALUES (1, 'it''s');
SELECT id AS "my""id", "val" FROM t_ls;
SELECT * FROM "t_ls";
INSERT INTO t_ls VALUES (2, 'multi
line');
SELECT val FROM t_ls WHERE val = 'it''s';
SELECT id FROM t_ls WHERE val = 'multi
line';
DROP TABLE t_ls;
