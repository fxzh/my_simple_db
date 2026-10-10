-- 无 FROM 的 SELECT: 恒一行, 常量投影
SELECT 1;
SELECT 1 + 2 * 3, 'hi', 7.5;
SELECT NULL, true, 7 / 2, 1.0 + 1, 1 + NULL;
SELECT 1 AS one, 2 + 2 AS four;
-- WHERE 恒真出一行, 恒假/NULL 零行
SELECT 1 WHERE 1 = 1;
SELECT 1 WHERE 1 = 0;
SELECT 1 WHERE NULL;
-- 计划: 单行扫描行源, 常量过滤被剪枝
EXPLAIN SELECT 1;
EXPLAIN SELECT 1 WHERE 1 = 0;
-- 异常: 无 FROM 不允许星号与列引用, 常量运算错误计划期报
SELECT *;
SELECT a;
SELECT 1 WHERE 1 / 0 = 1;
-- Int 域 int32 溢出在折叠期报错
SELECT 2000000000 + 2000000000;
