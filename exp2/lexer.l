%{
#include <iostream>
#include <string>
#include <cstdlib>
#include "parser.tab.hh"

// 跟踪行和列
int yylineno = 1;
int yycolumn = 1;

// 位置信息
yy::parser::semantic_type* yylval;
yy::parser::location_type* yylloc;

// 位置更新宏 - 在每个token匹配时自动更新位置
#define YY_USER_ACTION \
    yylloc->begin.line = yylineno; \
    yylloc->begin.column = yycolumn; \
    yylloc->end.line = yylineno; \
    yylloc->end.column = yycolumn + yyleng; \
    yycolumn += yyleng;
%}

%option noyywrap
%option nounput
%option noinput
%option c++
%option case-insensitive

DIGIT    [0-9]
ID       [a-zA-Z_][a-zA-Z0-9_]*
WS       [ \t\r]

%%

"create"   { return yy::parser::token::CREATE; }
"table"    { return yy::parser::token::TABLE; }
"drop"     { return yy::parser::token::DROP; }
"insert"   { return yy::parser::token::INSERT; }
"into"     { return yy::parser::token::INTO; }
"values"   { return yy::parser::token::VALUES; }
"int"      { return yy::parser::token::INT; }
"float"    { return yy::parser::token::FLOAT; }
"char"     { return yy::parser::token::CHAR; }
"double"   { return yy::parser::token::DOUBLE; }
"show"     { return yy::parser::token::TOK_SHOW; }
"quit"     { return yy::parser::token::TOK_QUIT; }
"exit"     { return yy::parser::token::TOK_EXIT; }

{DIGIT}+"."{DIGIT}*|"."{DIGIT}+ {
  yylval->build<double>(std::stod(yytext));
  return yy::parser::token::FLOAT_NUM;
}

{DIGIT}+ {
  yylval->build<long long>(std::stoll(yytext));
  return yy::parser::token::INTEGER;
}

'[^'\n]*' {
  // 去掉首尾引号(bison 的 build 只接受单个参数, 需先显式构造 std::string)
  yylval->build<std::string>(std::string(yytext + 1, yyleng - 2));
  return yy::parser::token::STRING;
}

{ID} {
  yylval->build<std::string>(yytext);
  return yy::parser::token::IDENTIFIER;
}

"+"|"-"|"*"|"/"|"("|")"|","|";" {
  return yytext[0];
}

\n {
  yylineno++;
  yycolumn = 1;
}

{WS}+ {
  // 忽略空白
}

. {
  std::cerr << "Invalid character: '" << yytext[0] << "'" << std::endl;
  return yy::parser::token::TOK_ERROR;
}

%%
