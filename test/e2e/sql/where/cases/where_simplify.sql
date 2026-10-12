CREATE TABLE ws_t (id int, v int, f double, s varchar(8));
INSERT INTO ws_t VALUES (1, 10, 1.5, 'ab');
INSERT INTO ws_t VALUES (2, NULL, NULL, NULL);
INSERT INTO ws_t VALUES (3, 30, 2.5, 'cd');
-- eq/le/ge 自比较: 空安全改写为 IS NOT NULL, NULL 行被过滤
SELECT id FROM ws_t WHERE v = v ORDER BY id;
SELECT id FROM ws_t WHERE v <= v ORDER BY id;
SELECT id FROM ws_t WHERE v >= v ORDER BY id;
SELECT id FROM ws_t WHERE f = f ORDER BY id;
SELECT id FROM ws_t WHERE s = s ORDER BY id;
-- ne/lt/gt 自比较: 恒不满足
SELECT id FROM ws_t WHERE v <> v;
SELECT id FROM ws_t WHERE v < v;
SELECT id FROM ws_t WHERE v > v;
-- NOT 自比较经 NOT 消除后落入同一化简
SELECT id FROM ws_t WHERE NOT (v = v);
SELECT id FROM ws_t WHERE NOT (v < v) ORDER BY id;
-- 与 AND/OR 组合: false 被支配/恒等规则吸收
SELECT id FROM ws_t WHERE v = v AND id > 1 ORDER BY id;
SELECT id FROM ws_t WHERE v < v OR id = 2;
-- 子树结构相等: 算术/函数操作数自比较
SELECT id FROM ws_t WHERE v + 1 = v + 1 ORDER BY id;
SELECT id FROM ws_t WHERE abs(v) = abs(v) ORDER BY id;
-- 投影语境不做化简: NULL 行原样输出 NULL
SELECT id, v = v AS eq FROM ws_t ORDER BY id;
DROP TABLE ws_t;
