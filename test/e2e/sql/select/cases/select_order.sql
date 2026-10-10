-- 单键升/降序与 NULL 位次
CREATE TABLE t_ord (i int, s char(4), f double);
INSERT INTO t_ord VALUES (3, 'dd', 1.5);
INSERT INTO t_ord VALUES (1, 'ab', 3.5);
INSERT INTO t_ord VALUES (2, NULL, 2.5);
INSERT INTO t_ord VALUES (1, 'bc', 0.5);
INSERT INTO t_ord VALUES (NULL, 'aa', 4.5);
SELECT i FROM t_ord ORDER BY i;
SELECT i FROM t_ord ORDER BY i DESC;
-- 序号键与别名键
SELECT i, s FROM t_ord ORDER BY 2;
SELECT s AS x FROM t_ord ORDER BY x;
-- 表达式键与不在输出的列键
SELECT i FROM t_ord ORDER BY f * 2;
SELECT i FROM t_ord ORDER BY s DESC;
-- 多键混合方向
SELECT i, f FROM t_ord ORDER BY i DESC, f ASC;
-- 别名命中与未命中键混用, 键转行上下文
SELECT i AS n FROM t_ord ORDER BY n DESC, f;
-- 过滤后排序与空结果排序
SELECT i FROM t_ord WHERE f > 1.0 ORDER BY i DESC;
SELECT i FROM t_ord WHERE i > 100 ORDER BY i;
-- 无 FROM 单行排序
SELECT 3 AS x, 1 AS y ORDER BY y;
SELECT 2 AS a, 1 AS b ORDER BY 2 DESC;
DROP TABLE t_ord;
