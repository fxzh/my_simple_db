# parser
服务端 SQL 词法/语法解析：flex + bison，语法校验并返回首个语句 AST

- bison token 命名须避开 flex 宏：如 BEGIN 宏导致事务开始 token 用 BEGIN_TXN
