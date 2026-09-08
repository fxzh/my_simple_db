# SQL 解析库(sql_parser 静态库，server 链接)
flex(c++ 模式) + bison(c++ 模式) 生成的最小 SQL 解析，当前只做语法校验：AST 构建完即丢弃，不执行、不落库。

# 文件
lexer.l         扫描器：关键字/标识符/数字/字符串字面量，非法字符记入 sql_parse_error 并返回 TOK_ERROR
parser.y        文法(%language "c++"、variant 语义值、%locations)：支持 CREATE TABLE / DROP TABLE /
                INSERT INTO ... VALUES，含 + - * / 与一元 +/- 的完整表达式层
ast.hh          AST 节点(Expr/SQLStatement 派生)；bison variant 析构内联要求其先于生成头被 include
sql_parser.h/.cpp   唯一对外入口 sql::parse(stmt, error, stmt_kind)；stmt_kind 输出识别出的首个语句种类
                (create table/drop table/insert into, 空语句为空)；flex/bison 依赖全局状态
                (lexer/yylval/yylloc)，用静态 mutex 串行化整个解析，可多线程调用

# 注意
- parser.tab.cc/hh、lex.yy.cc 由 bison/flex 生成到构建目录，不提交、不改
- 词法/语法错误统一写入 sql_parse_error，格式 "行.列: 描述"
- 扩展语法：同时改 lexer.l、parser.y、ast.hh，token 声明与 variant 类型需对应