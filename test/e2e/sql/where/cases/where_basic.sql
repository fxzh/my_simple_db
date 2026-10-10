CREATE TABLE t_w (id int, name varchar(8), score double);
SELECT id FROM t_w WHERE id > 0;
INSERT INTO t_w VALUES (1, 'alice', 95.5);
INSERT INTO t_w VALUES (2, 'bob', 60.0);
INSERT INTO t_w VALUES (3, 'carol', 78.5);
INSERT INTO t_w VALUES (4, 'dave', 40.0);
SELECT id, name FROM t_w WHERE score >= 78.5 ORDER BY id;
SELECT id FROM t_w WHERE score > 60 AND id < 3;
SELECT id FROM t_w WHERE id = 1 OR id = 4 ORDER BY id;
SELECT * FROM t_w WHERE NOT (score >= 60);
SELECT id FROM t_w WHERE name = 'bob';
SELECT id FROM t_w WHERE name <> 'bob' AND score >= 78.5;
SELECT id FROM t_w WHERE score * 2 > 150;
-- 布尔化简: 常量侧化简后被支配侧不再求值(除零不报错)
SELECT id FROM t_w WHERE 1 = 1 AND score > 60;
SELECT id FROM t_w WHERE 1 = 2 AND score > 60;
SELECT id FROM t_w WHERE 2 > 1 OR score > 60 ORDER BY id;
SELECT id FROM t_w WHERE 1 = 2 AND 1 / (id - 1) = 1;
-- NULL 常量不可化简, 走三值逻辑
SELECT id FROM t_w WHERE NULL OR id = 1;
SELECT id FROM t_w WHERE NULL AND id = 1;
DROP TABLE t_w;
