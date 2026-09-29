CREATE TABLE t_lc (id int, val char(16));
-- 建表完成
INSERT INTO t_lc VALUES (1, 'a'); -- 行尾注释
INSERT INTO t_lc VALUES (2, 'b'); /* 行尾块注释 */
SELECT id, val FROM t_lc; /* 行内块注释 */
/* 含分号的块注释 ; */ SELECT id FROM t_lc;
/* 跨行
块注释 */ SELECT val FROM t_lc;
SELECT id -- 1
FROM t_lc;
SELECT 1 - -1 FROM t_lc;
DROP TABLE t_lc;
SELECT 1 /* 未闭合
