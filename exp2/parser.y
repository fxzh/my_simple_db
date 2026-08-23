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
  // 若此处只有前置声明, 就会报 "invalid application of 'sizeof' to incomplete type"
  #include "ast.hh"

  class yyFlexLexer;
}

%code {
  #include <iostream>
  #include <FlexLexer.h>
  #include "parser.tab.hh"  // 包含 bison 生成的头文件

  extern yy::parser::semantic_type* yylval;
  extern yy::parser::location_type* yylloc;
  yyFlexLexer* lexer = nullptr;

  static int yylex(yy::parser::semantic_type* lval,
                   yy::parser::location_type* lloc)
  {
      yylval = lval;
      yylloc = lloc;
      return lexer->yylex();
  }

  void yy::parser::error(const yy::location& loc,
                         const std::string& msg)
  {
      std::cerr << loc << ": " << msg << std::endl;
  }

  // 全局数据库(ast.hh 中只有 extern 声明, 定义放在这里)
  Database database;
}

// 修改参数声明
%parse-param { yy::location& loc }

// Token定义
%token END 0 "end of file"
%token TOK_ERROR
%token CREATE TABLE DROP INSERT INTO VALUES
%token INT FLOAT CHAR DOUBLE
%token TOK_SHOW
%token TOK_QUIT
%token TOK_EXIT

%token <long long> INTEGER
%token <double> FLOAT_NUM
%token <std::string> IDENTIFIER
%token <std::string> STRING

// 运算符优先级
%left '+' '-'
%left '*' '/'
%right UMINUS

// 类型声明
%type <std::unique_ptr<SQLStatement>> statement create_statement drop_statement insert_statement create_table_statement drop_table_statement
%type <std::vector<ColumnDef>> column_definitions
%type <ColumnDef> column_definition
%type <std::string> type_specifier
%type <std::vector<std::unique_ptr<Expr>>> value_list
%type <std::unique_ptr<Expr>> value expression

%%

// 交互式输入: 每条语句以 ';' 结尾, 语句可以跨行
input: /* empty */
     | input line
     ;

line: statement ';' {
         $1->execute();
         std::cout << "> " << std::flush;
       }
    | TOK_SHOW ';' {
         database.print();
         std::cout << "> " << std::flush;
       }
    | TOK_QUIT ';' { std::cout << "Goodbye!" << std::endl; YYACCEPT; }
    | TOK_EXIT ';' { std::cout << "Goodbye!" << std::endl; YYACCEPT; }
    | ';'          { std::cout << "> " << std::flush; }
    | error ';'    { yyerrok; std::cout << "> " << std::flush; }
    ;

statement:
      create_statement  { $$ = std::move($1); }
    | drop_statement    { $$ = std::move($1); }
    | insert_statement  { $$ = std::move($1); }
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
    | column_definition {
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
    | value {
        $$ = std::vector<std::unique_ptr<Expr>>();
        $$.push_back(std::move($1));
      }
    ;

value:
      expression { $$ = std::move($1); }
    ;

type_specifier:
      INT    { $$ = std::string("int"); }
    | FLOAT  { $$ = std::string("float"); }
    | CHAR   { $$ = std::string("char"); }
    | DOUBLE { $$ = std::string("double"); }
    ;

expression:
      INTEGER                     { $$ = std::make_unique<IntExpr>($1); }
    | FLOAT_NUM                   { $$ = std::make_unique<FloatExpr>($1); }
    | STRING                      { $$ = std::make_unique<StringExpr>(std::move($1)); }
    | IDENTIFIER                  { $$ = std::make_unique<IdentifierExpr>(std::move($1)); }
    | expression '+' expression   { $$ = std::make_unique<BinaryOpExpr>('+', std::move($1), std::move($3)); }
    | expression '-' expression   { $$ = std::make_unique<BinaryOpExpr>('-', std::move($1), std::move($3)); }
    | expression '*' expression   { $$ = std::make_unique<BinaryOpExpr>('*', std::move($1), std::move($3)); }
    | expression '/' expression   { $$ = std::make_unique<BinaryOpExpr>('/', std::move($1), std::move($3)); }
    | '(' expression ')'          { $$ = std::move($2); }
    | '-' expression %prec UMINUS { $$ = std::make_unique<UnaryOpExpr>('-', std::move($2)); }
    | '+' expression %prec UMINUS { $$ = std::make_unique<UnaryOpExpr>('+', std::move($2)); }
    ;

%%

// main 函数在 main.cc 中
