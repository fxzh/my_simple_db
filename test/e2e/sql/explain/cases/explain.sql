CREATE TABLE t_ex (a int, b double, c varchar(10));
-- 计划树: 投影/过滤/扫描
EXPLAIN SELECT a, b FROM t_ex WHERE a > 1 AND b IS NOT NULL;
EXPLAIN SELECT * FROM t_ex;
-- DML 计划: 有 WHERE 与无 WHERE 的形态
EXPLAIN DELETE FROM t_ex WHERE a = 1;
EXPLAIN DELETE FROM t_ex;
EXPLAIN UPDATE t_ex SET b = b + 1.5 WHERE a > 0;
EXPLAIN INSERT INTO t_ex VALUES (1, 1.5, 'x');
-- DDL 计划
EXPLAIN CREATE INDEX idx_ex ON t_ex (a);
EXPLAIN DROP INDEX idx_ex ON t_ex;
-- 嵌套 explain
EXPLAIN EXPLAIN SELECT a FROM t_ex;
-- 静态校验与真实执行同文案
EXPLAIN SELECT no_col FROM t_ex;
EXPLAIN SELECT a FROM t_missing;
-- 常量折叠: 纯常量子树在计划期求值, 折叠期常量运算错误当场报错
EXPLAIN SELECT a FROM t_ex WHERE a = 1 + 1;
EXPLAIN UPDATE t_ex SET b = 2 * 3.5 WHERE a > 1 + 1;
EXPLAIN SELECT a FROM t_ex WHERE 1 = 1 AND a > 0;
EXPLAIN SELECT a FROM t_ex WHERE a > 1 + NULL;
EXPLAIN SELECT a FROM t_ex WHERE a = 1 / 0;
-- 布尔化简: 单侧常量 bool 按支配/恒等规则化简, NULL 常量不可化简
EXPLAIN SELECT a FROM t_ex WHERE 1 = 2 AND a > 0;
EXPLAIN SELECT a FROM t_ex WHERE a > 0 AND 2 > 3;
EXPLAIN SELECT a FROM t_ex WHERE 1 = 1 OR a > 0;
EXPLAIN SELECT a FROM t_ex WHERE 2 > 3 OR a > 0;
EXPLAIN SELECT a FROM t_ex WHERE NULL AND a > 0;
-- 事务内解释 DDL: 拒绝并回滚
BEGIN;
EXPLAIN CREATE TABLE t_txn (a int);
DROP TABLE t_ex;
