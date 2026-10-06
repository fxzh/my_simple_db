-- boolean/bool 同义, 无长度语法
CREATE TABLE t_bool (id int, flag boolean, ok bool);
CREATE TABLE t_boolbad (a bool(1));
-- 字面量与常量布尔表达式插入, NULL 渲染
INSERT INTO t_bool VALUES (1, true, TRUE);
INSERT INTO t_bool VALUES (2, false, FALSE);
INSERT INTO t_bool VALUES (3, NULL, null);
INSERT INTO t_bool VALUES (4, 1 > 2, NOT true);
SELECT * FROM t_bool;
-- 布尔投影: 列、表达式与字面量
SELECT flag FROM t_bool WHERE id = 1;
SELECT ok, NOT ok FROM t_bool WHERE id = 2;
SELECT true FROM t_bool WHERE id = 1;
SELECT flag < true FROM t_bool;
-- WHERE: 裸列、比较、逻辑、三值逻辑
SELECT id FROM t_bool WHERE flag;
SELECT id FROM t_bool WHERE NOT flag;
SELECT id FROM t_bool WHERE flag = false;
SELECT id FROM t_bool WHERE flag < true;
SELECT id FROM t_bool WHERE flag AND ok;
SELECT id FROM t_bool WHERE flag IS NULL;
SELECT id FROM t_bool WHERE flag IS NOT NULL;
SELECT id FROM t_bool WHERE flag = NULL;
SELECT id FROM t_bool WHERE true;
SELECT id FROM t_bool WHERE false;
-- 禁止整数/字符串与 bool 隐式转换
INSERT INTO t_bool VALUES (5, 1, true);
INSERT INTO t_bool VALUES (5, 'true', true);
SELECT id FROM t_bool WHERE flag = 1;
SELECT flag + 1 FROM t_bool;
UPDATE t_bool SET flag = 2 WHERE id = 1;
-- UPDATE 右值: 列引用与常量布尔表达式
UPDATE t_bool SET ok = flag WHERE id = 2;
UPDATE t_bool SET flag = NOT flag WHERE id = 1;
SELECT flag, ok FROM t_bool WHERE id <= 2;
-- bool 条件删除
DELETE FROM t_bool WHERE flag IS NULL;
SELECT * FROM t_bool;
-- bool 列建索引与索引列上的过滤
CREATE INDEX idx_bool ON t_bool (flag);
INSERT INTO t_bool VALUES (5, true, false);
SELECT id FROM t_bool WHERE flag = false;
SELECT id FROM t_bool WHERE flag = true;
DROP INDEX idx_bool ON t_bool;
-- EXPLAIN 渲染布尔常量
EXPLAIN SELECT id FROM t_bool WHERE flag = true;
DROP TABLE t_bool;
