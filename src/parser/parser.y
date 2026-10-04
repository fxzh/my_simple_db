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
%token CREATE TABLE DROP SCHEMA INSERT INTO VALUES DELETE FROM UPDATE INDEX ON
%token INT BIGINT FLOAT CHAR VARCHAR DOUBLE
%token SELECT AS NULL_T WHERE AND OR NOT IS SET
%token BEGIN_TXN START TRANSACTION COMMIT WORK ROLLBACK
%token EQ NE LE GE

%token <long long> INTEGER
%token <double> FLOAT_NUM
%token <std::string> IDENTIFIER
%token <std::string> STRING

// 预期移进/归约冲突数, 实际超出即报错
%expect 0

// 运算符优先级: OR < AND < NOT < IS(非结合) < 比较(非结合) < 加减 < 乘除
%left OR
%left AND
%left NOT
%nonassoc IS
%nonassoc EQ NE '<' LE '>' GE
%left '+' '-'
%left '*' '/'
%right UMINUS

// 类型声明
%type <std::unique_ptr<SQLStatement>> statement create_statement drop_statement insert_statement delete_statement update_statement create_table_statement drop_table_statement create_schema_statement drop_schema_statement create_index_statement drop_index_statement select_statement set_statement txn_statement begin_statement commit_statement rollback_statement
%type <std::vector<ColumnDef>> column_definitions
%type <ColumnDef> column_definition
%type <TypeInfo> type_specifier
%type <std::vector<std::unique_ptr<Expr>>> value_list
%type <std::vector<std::vector<std::unique_ptr<Expr>>>> values_rows
%type <std::unique_ptr<Expr>> value expression where_opt
%type <bool> null_not_opt not_null_opt
%type <std::string> set_value
%type <std::vector<SelectItem>> select_list select_items
%type <SelectItem> select_item
%type <std::vector<UpdateItem>> update_assignments
%type <UpdateItem> update_assignment
%type <std::string> alias_opt
%type <std::vector<std::string>> columns_opt column_name_list

%%

// 一条消息: 若干以 ';' 结尾的语句(允许空输入和空语句)
input: /* empty */
    |   input line
    ;

line: statement ';'
        {
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
    |   update_statement  { $$ = std::move($1); }
    |   select_statement  { $$ = std::move($1); }
    |   set_statement     { $$ = std::move($1); }
    |   txn_statement     { $$ = std::move($1); }
    ;

create_statement:
        create_table_statement { $$ = std::move($1); }
    |   create_schema_statement { $$ = std::move($1); }
    |   create_index_statement { $$ = std::move($1); }
    ;

create_table_statement:
        CREATE TABLE IDENTIFIER '(' column_definitions ')'
        {
            $$ = std::make_unique<CreateTableStmt>(std::move($3), std::move($5));
        }
    ;

// create schema ...
create_schema_statement:
        CREATE SCHEMA IDENTIFIER
        {
            $$ = std::make_unique<CreateSchemaStmt>(std::move($3));
        }
    ;

// create index 索引名 on 表名 (单列名)
create_index_statement:
        CREATE INDEX IDENTIFIER ON IDENTIFIER '(' IDENTIFIER ')'
        {
            $$ = std::make_unique<CreateIndexStmt>(std::move($3), std::move($5), std::move($7));
        }
    ;

column_definitions:
        column_definitions ',' column_definition
        {
            $1.push_back(std::move($3));
            $$ = std::move($1);
        }
    |   column_definition
        {
            $$ = std::vector<ColumnDef>{ std::move($1) };
        }
    ;

column_definition:
        IDENTIFIER type_specifier not_null_opt
        {
            $$ = ColumnDef{ std::move($1), $2.type, $2.length, $3 };
        }
    ;

// 列定义可选 NOT NULL 约束后缀
not_null_opt:
        /* empty */ { $$ = false; }
    |   NOT NULL_T { $$ = true; }
    ;

// drop table / drop schema / drop index ...
drop_statement:
        drop_table_statement { $$ = std::move($1); }
    |   drop_schema_statement { $$ = std::move($1); }
    |   drop_index_statement { $$ = std::move($1); }
    ;

drop_table_statement:
        DROP TABLE IDENTIFIER
        {
            $$ = std::make_unique<DropTableStmt>(std::move($3));
        }
    ;

// drop schema ...
drop_schema_statement:
        DROP SCHEMA IDENTIFIER
        {
            $$ = std::make_unique<DropSchemaStmt>(std::move($3));
        }
    ;

// drop index 索引名 on 表名
drop_index_statement:
        DROP INDEX IDENTIFIER ON IDENTIFIER
        {
            $$ = std::make_unique<DropIndexStmt>(std::move($3), std::move($5));
        }
    ;

// delete from 表名 [where 条件]
delete_statement:
        DELETE FROM IDENTIFIER where_opt
        {
            $$ = std::make_unique<DeleteStmt>(std::move($3), std::move($4));
        }
    ;

// update 表名 set 赋值列表 [where 条件](语法已接入, 语义暂缺)
update_statement:
        UPDATE IDENTIFIER SET update_assignments where_opt
        {
            $$ = std::make_unique<UpdateStmt>(std::move($2), std::move($4), std::move($5));
        }
    ;

// update 赋值列表: 逗号连接的 列 = 表达式
update_assignments:
        update_assignments ',' update_assignment
        {
            $1.push_back(std::move($3));
            $$ = std::move($1);
        }
    |   update_assignment
        {
            $$ = std::vector<UpdateItem>();
            $$.push_back(std::move($1));
        }
    ;

// 单个赋值: 列 = 表达式
update_assignment:
        IDENTIFIER EQ expression
        {
            $$ = UpdateItem{ std::move($1), std::move($3) };
        }
    ;

// select 投影列表 FROM 表名 [where 条件](基础闭环: ORDER BY/LIMIT 随后续里程碑接入)
select_statement:
        SELECT select_list FROM IDENTIFIER where_opt
        {
            $$ = std::make_unique<SelectStmt>(std::move($4), false, std::move($2), std::move($5),
                                             std::vector<OrderItem>{}, std::nullopt, std::nullopt);
        }
    ;

// set 变量 = 值(bootstrap 变量与会话变量共用语法)
set_statement:
        SET IDENTIFIER EQ set_value
        {
            $$ = std::make_unique<SetStmt>(std::move($2), $4);
        }
    ;

// set 值: 整数(可带符号)、标识符或字符串字面量, 值域不在语法层校验
set_value:
        INTEGER      { $$ = std::to_string($1); }
    |   '-' INTEGER  { $$ = std::to_string(-$2); }
    |   '+' INTEGER  { $$ = std::to_string($2); }
    |   IDENTIFIER   { $$ = std::move($1); }
    |   STRING       { $$ = std::move($1); }
    ;

// 事务控制语句: begin / commit / rollback(会话层短路处理, 不进执行层)
txn_statement:
        begin_statement    { $$ = std::move($1); }
    |   commit_statement   { $$ = std::move($1); }
    |   rollback_statement { $$ = std::move($1); }
    ;

begin_statement:
        BEGIN_TXN transaction_opt { $$ = std::make_unique<BeginStmt>(); }
    |   START TRANSACTION         { $$ = std::make_unique<BeginStmt>(); }
    ;

commit_statement:
        COMMIT work_opt { $$ = std::make_unique<CommitStmt>(); }
    ;

rollback_statement:
        ROLLBACK work_opt { $$ = std::make_unique<RollbackStmt>(); }
    ;

transaction_opt:
        /* empty */
    |   TRANSACTION
    ;

work_opt:
        /* empty */
    |   WORK
    ;

// 可选 where 子句: 空时语义值为空指针
where_opt:
        /* empty */ { $$ = nullptr; }
    |   WHERE expression { $$ = std::move($2); }
    ;

select_list:
        '*'
        {
            $$ = std::vector<SelectItem>();
            $$.push_back(SelectItem{ nullptr, "", true });
        }
    |   select_items { $$ = std::move($1); }
    ;

select_items:
        select_items ',' select_item { $1.push_back(std::move($3)); $$ = std::move($1); }
    |   select_item
        {
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

// insert into 表名 [(列清单)] values 值行列表
insert_statement:
        INSERT INTO IDENTIFIER columns_opt VALUES values_rows
        {
            $$ = std::make_unique<InsertStmt>(std::move($3), std::move($4), std::move($6));
        }
    ;

// 可选列清单: 空表示按表全列插入
columns_opt:
        /* empty */ { $$ = std::vector<std::string>(); }
    |   '(' column_name_list ')' { $$ = std::move($2); }
    ;

column_name_list:
        column_name_list ',' IDENTIFIER
        {
            $1.push_back(std::move($3));
            $$ = std::move($1);
        }
    |   IDENTIFIER
        {
            $$ = std::vector<std::string>();
            $$.push_back(std::move($1));
        }
    ;

// 值行列表: 逗号连接的 '(值列表)', 支持一条 insert 插入多行
values_rows:
        values_rows ',' '(' value_list ')'
        {
            $1.push_back(std::move($4));
            $$ = std::move($1);
        }
    |   '(' value_list ')'
        {
            $$ = std::vector<std::vector<std::unique_ptr<Expr>>>();
            $$.push_back(std::move($2));
        }
    ;

value_list:
        value_list ',' value
        {
            $1.push_back(std::move($3));
            $$ = std::move($1);
        }
    |   value
        {
            $$ = std::vector<std::unique_ptr<Expr>>();
            $$.push_back(std::move($1));
        }
    ;

value:
      expression { $$ = std::move($1); }
    ;

type_specifier:
        INT    { $$ = TypeInfo{ DataType::Int, std::nullopt }; }
    |   BIGINT { $$ = TypeInfo{ DataType::BigInt, std::nullopt }; }
    |   FLOAT  { $$ = TypeInfo{ DataType::Float, std::nullopt }; }
    |   CHAR   { $$ = TypeInfo{ DataType::Char, std::nullopt }; }
    |   DOUBLE { $$ = TypeInfo{ DataType::Double, std::nullopt }; }
    |   VARCHAR { $$ = TypeInfo{ DataType::VarChar, std::nullopt }; }
    |   CHAR '(' INTEGER ')'    { $$ = TypeInfo{ DataType::Char, $3 }; }
    |   VARCHAR '(' INTEGER ')' { $$ = TypeInfo{ DataType::VarChar, $3 }; }
    ;

expression:
        INTEGER                     { $$ = std::make_unique<IntExpr>($1); }
    |   FLOAT_NUM                   { $$ = std::make_unique<FloatExpr>($1); }
    |   STRING                      { $$ = std::make_unique<StringExpr>(std::move($1)); }
    |   NULL_T                      { $$ = std::make_unique<NullExpr>(); }
    |   IDENTIFIER                  { $$ = std::make_unique<IdentifierExpr>(std::move($1)); }
    |   expression '+' expression   { $$ = std::make_unique<BinaryOpExpr>('+', std::move($1), std::move($3)); }
    |   expression '-' expression   { $$ = std::make_unique<BinaryOpExpr>('-', std::move($1), std::move($3)); }
    |   expression '*' expression   { $$ = std::make_unique<BinaryOpExpr>('*', std::move($1), std::move($3)); }
    |   expression '/' expression   { $$ = std::make_unique<BinaryOpExpr>('/', std::move($1), std::move($3)); }
    |   '(' expression ')'          { $$ = std::move($2); }
    |   '-' expression %prec UMINUS { $$ = std::make_unique<UnaryOpExpr>('-', std::move($2)); }
    |   '+' expression %prec UMINUS { $$ = std::make_unique<UnaryOpExpr>('+', std::move($2)); }
    |   expression EQ expression
        {
            $$ = std::make_unique<CompareExpr>(CmpOp::Eq, std::move($1), std::move($3));
        }
    |   expression NE expression
        {
            $$ = std::make_unique<CompareExpr>(CmpOp::Ne, std::move($1), std::move($3));
        }
    |   expression '<' expression
        {
            $$ = std::make_unique<CompareExpr>(CmpOp::Lt, std::move($1), std::move($3));
        }
    |   expression LE expression
        {
            $$ = std::make_unique<CompareExpr>(CmpOp::Le, std::move($1), std::move($3));
        }
    |   expression '>' expression
        {
            $$ = std::make_unique<CompareExpr>(CmpOp::Gt, std::move($1), std::move($3));
        }
    |   expression GE expression
        {
            $$ = std::make_unique<CompareExpr>(CmpOp::Ge, std::move($1), std::move($3));
        }
    |   expression AND expression
        {
            $$ = std::make_unique<LogicExpr>(LogicOp::And, std::move($1), std::move($3));
        }
    |   expression OR expression
        {
            $$ = std::make_unique<LogicExpr>(LogicOp::Or, std::move($1), std::move($3));
        }
    |   NOT expression { $$ = std::make_unique<NotExpr>(std::move($2)); }
    |   expression IS null_not_opt NULL_T { $$ = std::make_unique<IsNullExpr>(std::move($1), $3); }
    ;

null_not_opt:
        /* empty */ { $$ = false; }
    |   NOT { $$ = true; }
    ;

%%

// 对外入口 sql::parse 在 sql_parser.cpp 中
