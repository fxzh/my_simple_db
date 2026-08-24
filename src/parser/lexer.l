%{
#include <string>
#include <sstream>
#include "parser.tab.hh"

// 跟踪行和列(不用 yylineno 命名: C++ scanner 中它是 yyFlexLexer 的成员名, 会被遮蔽)
unsigned curr_line = 1;
unsigned curr_column = 1;

// 位置信息(parser.y 的 yylex 包装函数在每次调用前赋值)
yy::parser::semantic_type* yylval;
yy::parser::location_type* yylloc;

// 语法错误信息缓冲(定义在 sql_parser.cpp)
extern std::string sql_parse_error;

// 每次解析一条新消息前重置行列计数(sql_parser.cpp 调用)
void reset_lexer_location() {
  curr_line = 1;
  curr_column = 1;
}

// 位置更新宏 - 在每个token匹配时自动更新位置
#define YY_USER_ACTION \
    yylloc->begin.line = curr_line; \
    yylloc->begin.column = curr_column; \
    yylloc->end.line = curr_line; \
    yylloc->end.column = curr_column + static_cast<unsigned>(yyleng); \
    curr_column += static_cast<unsigned>(yyleng);
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

{DIGIT}+"."{DIGIT}*|"."{DIGIT}+ {
  yylval->build<double>(std::stod(yytext));
  return yy::parser::token::FLOAT_NUM;
}

{DIGIT}+ {
  yylval->build<long long>(std::stoll(yytext));
  return yy::parser::token::INTEGER;
}

'[^'\n]*' {
  // 去掉首尾引号
  yylval->build<std::string>(std::string(yytext + 1, static_cast<std::size_t>(yyleng) - 2));
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
  curr_line++;
  curr_column = 1;
}

{WS}+ {
  // 忽略空白
}

. {
  // 非法字符: 记录错误位置(错误信息由 sql::parse 返回给调用方),
  // 返回 TOK_ERROR 触发语法错误
  std::ostringstream oss;
  oss << *yylloc << ": invalid character '" << yytext[0] << "'";
  sql_parse_error = oss.str();
  return yy::parser::token::TOK_ERROR;
}

%%
