CREATE TABLE es_t (a int, b int);
CREATE INDEX es_a ON es_t (a);
-- eq 自比较改写为 IS NOT NULL 并命中索引
EXPLAIN SELECT * FROM es_t WHERE a = a;
-- 无索引列保持 Filter
EXPLAIN SELECT * FROM es_t WHERE b = b;
-- lt 自比较折 false, 剪成空结果
EXPLAIN SELECT * FROM es_t WHERE a < a;
-- NOT(eq) 经 NOT 消除折 false, 剪成空结果
EXPLAIN SELECT * FROM es_t WHERE NOT (a = a);
-- 子树自比较改写, 算术操作数不入索引
EXPLAIN SELECT * FROM es_t WHERE a + 1 = a + 1;
-- 与其他谓词组合, 判空入索引残余谓词分离
EXPLAIN SELECT * FROM es_t WHERE a = a AND b > 1;
-- DML 过滤改写
EXPLAIN DELETE FROM es_t WHERE a = a;
DROP TABLE es_t;
