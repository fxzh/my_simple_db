# parser
服务端 SQL 词法/语法解析：flex + bison，语法校验并返回首个语句 AST

- 无模块级全局变量，每次调用状态全在栈上实例，可无锁并发
- 一帧多条语句只取第一条(语句切分约定在 client)
- ast.hh 必须先于 bison 生成头被 include(variant 析构内联要求)；生成的 parser.tab.*、lex.yy.cc 在构建目录，不提交、不改
- 扩展语法需同时改 lexer.l、parser.y、ast.hh，token 声明与 variant 类型要对应
