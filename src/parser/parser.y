%require "3.2"
%language "c++"
%defines
%define api.value.type variant
%define parse.error verbose
%locations
%define api.namespace {yy}
%define api.parser.class {parser}

%code requires {
    // variant 语义值里出现的所有类型(包括 unique_ptr 指向的 Expr/SQLStatement)
    // 必须在这里就是完整类型: 生成的 basic_symbol 析构是内联的,
    // 会在所有 #include "parser.tab.hh" 的编译单元中被实例化,
    #include "ast.hh"

    class SQLScanner;  // %lex-param 只出现在生成实现的 yylex 调用中
}

%code {
    #include <sstream>
    #include "parser.tab.hh"  // 包含 bison 生成的头文件
    #include "sql_scanner.h"

    // 词法接口: 扫描器实例经 %lex-param 传入, 无需任何全局状态
    static int yylex(yy::parser::semantic_type* lval, yy::parser::location_type* lloc,
                     SQLScanner* scanner)
    {
        return scanner->yylex(lval, lloc);
    }

    // 语法错误信息写入 parse-param(lalr1.cc 将其存为 parser 成员),
    // 与词法器经 scanner 写入的是同一个缓冲
    void yy::parser::error(const yy::location& err_loc, const std::string& msg)
    {
        if (!sql_parse_error.empty()) {
            return;  // 词法器已记录过错误(如非法字符), 保留第一个错误
        }
        std::ostringstream oss;
        oss << err_loc << ": " << msg;
        sql_parse_error = oss.str();
    }
}

// 位置由 sql_parser.cpp 传入
%parse-param { yy::location& loc }
// 首个语句的 AST 输出, 由 sql::parse 传入并返回给调用方
%parse-param { std::unique_ptr<SQLStatement>& result }
// 语法错误缓冲: parser 直接使用, 词法器经 scanner 写入同一缓冲
%parse-param { std::string& sql_parse_error }
// 扫描器实例: 存为 parser 成员供 yylex 包装函数(%lex-param)引用
%parse-param { SQLScanner* scanner }
// 扫描器实例: 追加到 yylex 调用的实参
%lex-param { SQLScanner* scanner }

// Token定义
%token END 0 "end of file"
%token TOK_ERROR
%token CREATE TABLE DROP INSERT INTO VALUES DELETE FROM
%token INT BIGINT FLOAT CHAR VARCHAR DOUBLE
%token SELECT AS

%token <long long> INTEGER
%token <double> FLOAT_NUM
%token <std::string> IDENTIFIER
%token <std::string> STRING

// 运算符优先级
%left '+' '-'
%left '*' '/'
%right UMINUS

// 类型声明
%type <std::unique_ptr<SQLStatement>> statement create_statement drop_statement insert_statement delete_statement create_table_statement drop_table_statement select_statement
%type <std::vector<ColumnDef>> column_definitions
%type <ColumnDef> column_definition
%type <std::string> type_specifier
%type <std::vector<std::unique_ptr<Expr>>> value_list
%type <std::unique_ptr<Expr>> value expression
%type <std::vector<SelectItem>> select_list select_items
%type <SelectItem> select_item
%type <std::string> alias_opt

%%

// 一条消息: 若干以 ';' 结尾的语句(允许空输入和空语句)
input: /* empty */
    |   input line
    ;

line: statement ';' {
        // 语法校验: 语句树构建成功即合法; 只交出首个语句的 AST
            if (!result) {
                result = std::move($1);
            }
        }
    |   ';'          { }
    ;

statement:
        create_statement  { $$ = std::move($1); }
    |   drop_statement    { $$ = std::move($1); }
    |   insert_statement  { $$ = std::move($1); }
    |   delete_statement  { $$ = std::move($1); }
    |   select_statement  { $$ = std::move($1); }
    ;

// create table ...
create_statement:
        create_table_statement { $$ = std::move($1); }
    ;

create_table_statement:
        CREATE TABLE IDENTIFIER '(' column_definitions ')' {
            $$ = std::make_unique<CreateTableStmt>(std::move($3), std::move($5));
        }
    ;

column_definitions:
        column_definitions ',' column_definition {
            $1.push_back(std::move($3));
            $$ = std::move($1);
        }
    |   column_definition {
            $$ = std::vector<ColumnDef>{ std::move($1) };
        }
    ;

column_definition:
        IDENTIFIER type_specifier {
            $$ = ColumnDef{ std::move($1), std::move($2) };
        }
    ;

// drop table ...
drop_statement:
        drop_table_statement { $$ = std::move($1); }
    ;

drop_table_statement:
        DROP TABLE IDENTIFIER {
            $$ = std::make_unique<DropTableStmt>(std::move($3));
        }
    ;

// delete from 表名
delete_statement:
        DELETE FROM IDENTIFIER {
            $$ = std::make_unique<DeleteStmt>(std::move($3));
        }
    ;

// select 投影列表 FROM 表名(基础闭环: WHERE/ORDER BY/LIMIT 随后续里程碑接入)
select_statement:
        SELECT select_list FROM IDENTIFIER {
            $$ = std::make_unique<SelectStmt>(std::move($4), false, std::move($2), nullptr,
                                             std::vector<OrderItem>{}, std::nullopt, std::nullopt);
        }
    ;

select_list:
        '*' {
            $$ = std::vector<SelectItem>();
            $$.push_back(SelectItem{ nullptr, "", true });
        }
    |   select_items { $$ = std::move($1); }
    ;

select_items:
        select_items ',' select_item { $1.push_back(std::move($3)); $$ = std::move($1); }
    |   select_item {
            $$ = std::vector<SelectItem>();
            $$.push_back(std::move($1));
        }
    ;

select_item:
        expression alias_opt { $$ = SelectItem{ std::move($1), std::move($2), false }; }
    ;

alias_opt:
        /* empty */ { $$ = std::string(); }
    |   AS IDENTIFIER { $$ = std::move($2); }
    ;

// insert into ... values (...)
insert_statement:
        INSERT INTO IDENTIFIER VALUES '(' value_list ')' {
            $$ = std::make_unique<InsertStmt>(std::move($3), std::move($6));
        }
    ;

value_list:
        value_list ',' value {
            $1.push_back(std::move($3));
            $$ = std::move($1);
        }
    |   value {
            $$ = std::vector<std::unique_ptr<Expr>>();
            $$.push_back(std::move($1));
        }
    ;

value:
      expression { $$ = std::move($1); }
    ;

type_specifier:
        INT    { $$ = std::string("int"); }
    |   BIGINT { $$ = std::string("bigint"); }
    |   FLOAT  { $$ = std::string("float"); }
    |   CHAR   { $$ = std::string("char"); }
    |   DOUBLE { $$ = std::string("double"); }
    |   VARCHAR { $$ = std::string("varchar"); }
    |   CHAR '(' INTEGER ')'    { $$ = "char(" + std::to_string($3) + ")"; }
    |   VARCHAR '(' INTEGER ')' { $$ = "varchar(" + std::to_string($3) + ")"; }
    ;

expression:
        INTEGER                     { $$ = std::make_unique<IntExpr>($1); }
    |   FLOAT_NUM                   { $$ = std::make_unique<FloatExpr>($1); }
    |   STRING                      { $$ = std::make_unique<StringExpr>(std::move($1)); }
    |   IDENTIFIER                  { $$ = std::make_unique<IdentifierExpr>(std::move($1)); }
    |   expression '+' expression   { $$ = std::make_unique<BinaryOpExpr>('+', std::move($1), std::move($3)); }
    |   expression '-' expression   { $$ = std::make_unique<BinaryOpExpr>('-', std::move($1), std::move($3)); }
    |   expression '*' expression   { $$ = std::make_unique<BinaryOpExpr>('*', std::move($1), std::move($3)); }
    |   expression '/' expression   { $$ = std::make_unique<BinaryOpExpr>('/', std::move($1), std::move($3)); }
    |   '(' expression ')'          { $$ = std::move($2); }
    |   '-' expression %prec UMINUS { $$ = std::make_unique<UnaryOpExpr>('-', std::move($2)); }
    |   '+' expression %prec UMINUS { $$ = std::make_unique<UnaryOpExpr>('+', std::move($2)); }
    ;

%%

// 对外入口 sql::parse 在 sql_parser.cpp 中
