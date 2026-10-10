CREATE TABLE di_t (id int, v int);
CREATE INDEX di_id ON di_t (id);
CREATE INDEX di_v ON di_t (v);
INSERT INTO di_t VALUES (1, 10);
INSERT INTO di_t VALUES (2, 20);
INSERT INTO di_t VALUES (3, 30);
INSERT INTO di_t VALUES (2, 40);
-- 走索引删除
DELETE FROM di_t WHERE id = 2;
SELECT * FROM di_t WHERE id = 2;
-- 残留索引条目回表按墓碑过滤
SELECT * FROM di_t ORDER BY id;
-- 走索引更新: 新条目可查, 旧条目残留被跳过
UPDATE di_t SET v = 99 WHERE id = 1;
SELECT * FROM di_t WHERE v = 99;
SELECT * FROM di_t WHERE v = 10;
-- 未命中时删除行数为 0
DELETE FROM di_t WHERE id = 5;
DROP TABLE di_t;
