CREATE TABLE ei_t (a int, b int, c double);
CREATE INDEX ei_a ON ei_t (a);
CREATE INDEX ei_b ON ei_t (b);
-- 等值命中: 索引扫描 + 回表
EXPLAIN SELECT * FROM ei_t WHERE a = 1;
-- 命中项摘除后残余谓词分离为 Filter
EXPLAIN SELECT * FROM ei_t WHERE a = 1 AND b > 2;
-- 同列区间交
EXPLAIN SELECT * FROM ei_t WHERE a > 1 AND a < 5;
-- ne 拆两段
EXPLAIN SELECT * FROM ei_t WHERE a <> 1;
-- 判空条件
EXPLAIN SELECT * FROM ei_t WHERE a IS NULL;
EXPLAIN SELECT * FROM ei_t WHERE a IS NOT NULL;
-- 多索引选择: 等值优先于区间
EXPLAIN SELECT * FROM ei_t WHERE b > 1 AND a = 2;
-- 多索引选择: 同级取列序号小者
EXPLAIN SELECT * FROM ei_t WHERE a = 1 AND b = 2;
-- 交空剪成空结果
EXPLAIN SELECT * FROM ei_t WHERE a = 1 AND a > 3;
-- 无索引谓词走顺序扫描
EXPLAIN SELECT * FROM ei_t WHERE c > 1.0;
-- between 展开两比较同列命中, 区间交
EXPLAIN SELECT * FROM ei_t WHERE a BETWEEN 1 AND 5;
-- between 命中后残余谓词分离
EXPLAIN SELECT * FROM ei_t WHERE a BETWEEN 1 AND 5 AND b > 2;
-- 空区间剪成空结果
EXPLAIN SELECT * FROM ei_t WHERE a BETWEEN 5 AND 1;
-- not between 不入索引, 顺序扫描
EXPLAIN SELECT * FROM ei_t WHERE a NOT BETWEEN 1 AND 5;
-- DML 改写形态
EXPLAIN DELETE FROM ei_t WHERE a = 1;
EXPLAIN UPDATE ei_t SET c = 0.0 WHERE a = 1;
EXPLAIN DELETE FROM ei_t WHERE a BETWEEN 1 AND 5;
DROP TABLE ei_t;
