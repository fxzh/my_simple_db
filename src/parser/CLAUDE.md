# SQL 解析库(sql_parser 静态库，server 链接)
flex(c++ 模式) + bison(c++ 模式) 生成的最小 SQL 解析，语法校验并把识别出的首个语句 AST 交回调用方，
不执行、不落库(执行在 executor 层)。

# 文件
lexer.l         扫描器(经 %option yyclass 生成在 SQLScanner 类中)：关键字/标识符/数字/字符串/NULL 字面量，
                比较运算符 = == <> != < <= > >=，非法字符记入本次解析的错误缓冲并返回 TOK_ERROR
parser.y        文法(%language "c++"、variant 语义值、%locations)：支持 CREATE TABLE / DROP TABLE /
                INSERT INTO ... VALUES / DELETE FROM ... [WHERE] / SELECT 投影查询 [WHERE]
                (M-S1: 列投影/别名/常量表达式)，表达式层含 + - * /、一元 +/-、比较 = <> < <= > >=、
                AND/OR/NOT 与 IS [NOT] NULL，比较与 IS 为非结合。
                扫描器/错误缓冲经 %lex-param/%parse-param 传入，无模块级全局变量；
                首个语句的 AST 经 %parse-param 回传给调用方；多条语句只取第一条
sql_scanner.h/.cpp   SQLScanner 词法器：派生 yyFlexLexer，词法状态(语义值/位置/行列/错误缓冲)全为实例成员
ast.hh          AST 节点(Expr/SQLStatement 派生)；bison variant 析构内联要求其先于生成头被 include
sql_parser.h/.cpp   唯一对外入口 sql::parse(stmt, error, result)；result 为
                识别出的首个语句种类与 AST，空语句两者皆空；每次调用所有状态均为栈上实例，无锁并发

# 注意
- parser.tab.cc/hh、lex.yy.cc 由 bison/flex 生成到构建目录，不提交、不改
- 词法/语法错误统一写入同一个错误缓冲，格式 "行.列: 描述"
- 扩展语法：同时改 lexer.l、parser.y、ast.hh，token 声明与 variant 类型需对应