%{
#include <string>
#include <sstream>
#include "parser.tab.hh"
#include "sql_scanner.h"

// C++ 扫描器: 词法状态全部存放于 SQLScanner 实例成员(见 sql_scanner.h),
// 无模块级全局变量, 可与其他解析并发执行

// 位置更新宏: 在每个token匹配时写入扫描器实例成员(与 yylloc/line/column 配套)
#define YY_USER_ACTION \
    yylloc->begin.line = line; \
    yylloc->begin.column = column; \
    yylloc->end.line = line; \
    yylloc->end.column = column + static_cast<unsigned>(yyleng); \
    column += static_cast<unsigned>(yyleng);
%}

%option noyywrap
%option nounput
%option noinput
%option c++
%option yyclass="SQLScanner"
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
    line++;
    column = 1;
}

{WS}+ {
    // 忽略空白
}

. {
    // 非法字符: 记录错误位置(错误信息由 sql::parse 返回给调用方),
    // 返回 TOK_ERROR 触发语法错误
    std::ostringstream oss;
    oss << *yylloc << ": invalid character '" << yytext[0] << "'";
    *err_buf = oss.str();
    return yy::parser::token::TOK_ERROR;
}

%%
