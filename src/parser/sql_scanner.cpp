// sql_scanner.cpp: SQLScanner 的构造与词法接口
#include "sql_scanner.h"

SQLScanner::SQLScanner(std::istream* in, std::string* err)
    : yyFlexLexer(in), err_buf(err)
{
}

SQLScanner::~SQLScanner() = default;

int SQLScanner::yylex(yy::parser::semantic_type* lval, yy::parser::location_type* lloc)
{
    yylval = lval;
    yylloc = lloc;
    // 进入 flex 生成的扫描主循环(生成于 lex.yy.cc)
    return yylex();
}
