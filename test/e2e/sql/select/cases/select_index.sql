CREATE TABLE si_t (id int, v bigint, f double, flag bool);
CREATE INDEX si_id ON si_t (id);
CREATE INDEX si_v ON si_t (v);
CREATE INDEX si_flag ON si_t (flag);
INSERT INTO si_t VALUES (1, 10, 1.5, true);
INSERT INTO si_t VALUES (2, 20, 2.5, false);
INSERT INTO si_t VALUES (3, NULL, NULL, NULL);
INSERT INTO si_t VALUES (4, 40, 4.5, true);
INSERT INTO si_t VALUES (2, 25, 2.5, false);
-- 等值命中(重复键全返回)
SELECT * FROM si_t WHERE id = 2 ORDER BY v;
-- 区间命中(NULL 行不入)
SELECT * FROM si_t WHERE v > 15 ORDER BY v;
-- 同列多谓词区间交
SELECT * FROM si_t WHERE id >= 2 AND id < 4 AND id <> 3 ORDER BY v;
-- ne 两段扫描
SELECT * FROM si_t WHERE id <> 2 ORDER BY v;
-- 判空条件命中
SELECT * FROM si_t WHERE v IS NULL;
SELECT * FROM si_t WHERE v IS NOT NULL ORDER BY v;
-- 常量在左的镜像形态
SELECT * FROM si_t WHERE 2 = id ORDER BY v;
-- 等值索引优先于区间索引, 其余谓词留作残余过滤
SELECT * FROM si_t WHERE id = 2 AND v = 20 ORDER BY v;
-- bool 列等值命中
SELECT * FROM si_t WHERE flag = true ORDER BY v;
-- 未建索引的列走顺序扫描
SELECT * FROM si_t WHERE f > 2.0 ORDER BY v;
-- 跨族常量(浮点常量对整型列)不命中索引
SELECT * FROM si_t WHERE v = 20.5 ORDER BY v;
-- 命中区间交空为空结果
SELECT * FROM si_t WHERE id = 1 AND id > 3;
DROP TABLE si_t;
CREATE TABLE si_f (x float);
CREATE INDEX si_fx ON si_f (x);
INSERT INTO si_f VALUES (1.5);
INSERT INTO si_f VALUES (2.5);
-- float 列不参与索引选择
SELECT * FROM si_f WHERE x > 2.0;
DROP TABLE si_f;
