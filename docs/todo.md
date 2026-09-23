# TODO

- 词法层超范围字面量: lexer.l 中 std::stoll/std::stod 遇超出整型/浮点范围的字面量抛 std::out_of_range, 绕过错误缓冲经 server 通用 catch 回客户端裸文案 "ERROR: stoll"/"ERROR: stod"; 处理方向: 捕获后按 "行.列: 描述" 格式写入词法错误缓冲
- 浮点字面量不支持科学计数法: lexer.l 的 FLOAT_NUM 规则只匹配十进制小数, "1e5" 词法为整数 1 加标识符 e5, 触发语法错误; 处理方向: 词法规则补指数部分
- codec.cpp 的 encode_row 对 float 列有 isfinite 校验, double 列没有; SQL 路径经执行层有限性校验后已无法产生 inf, 该缺口仅影响直接调用存储层的场景
- server.cpp 收尾的 2 秒盲睡(server.cpp:329)不是真等待: 客户端线程全 detached, 无人 join; 慢 SQL 超 2s 时 db.close()/静态析构与残留线程竞争(TOCTOU + 全局对象 UAF), 删掉则窗口更大, 保留也只是概率性绷带; 处理方向: handle_client erase 后 notify cv, 收尾改 clients_cv.wait 等 clients 清空
- `a IS NULL IS NULL` 当前可解析为嵌套判空(IsNull(IsNull)): 标准 SQL 中 null predicate 的操作数是 row value predicand(值表达式), 不允许谓词或布尔表达式, 链式写法应报语法错误(Postgres/MySQL 均拒绝, 括号形式 `(a IS NULL) IS NULL` 才合法); 当前 IS 规则的归约状态为纯归约态, 仅靠 %nonassoc IS 无法触发报错; 处理方向: 将 IS 判空拆为独立谓词层非终结符, 操作数限定为算术/值表达式, 链式写法自然成为语法错误, 随 M-S2 求值接入一并处理
- shutdown 无法中断执行中的 SQL: shutdown(SHUT_RDWR) 只能唤醒阻塞在 read 的线程, 执行路径(executor/storage)内无任何取消点, 强杀线程不可行(锁不释放), 只能服务端做协作式中断, 客户端无需改动; 处理方向: 存储层注入取消谓词(依赖方向不允许引 server 头文件, 传参绑定 server_running), insert/delete_all/row_count 的页遍历循环边界检查, 命中即 DB_RAISE 新错误码(如 QueryCanceled), 会话层现有 catch 自然接管; 注意无事务时取消半程 delete_all 留部分删除, M4 WAL 前无解