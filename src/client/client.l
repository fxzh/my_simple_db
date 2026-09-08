%{
#include "client.h"
%}

%option noyywrap
%option nounput
%option noinput
%option caseless
%option c++

%x COMMENT
%x STRING_SINGLE
%x STRING_DOUBLE

SPACE       [ \t\r\n]
SEMICOLON   ;
SINGLE_QUOTE \'
DOUBLE_QUOTE \"
MINUS       -

%%

<COMMENT>{
    [^\n]+ {
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
    }

    \n {
        BEGIN(INITIAL);
        scanner_state = STATE_INITIAL;
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
    }
}

<INITIAL>{
    {SPACE}+ {
        if (!sql_buffer.empty()) {
            append_to_sql(yytext, static_cast<std::size_t>(yyleng));
        }
    }

    {SINGLE_QUOTE} {
        // 进入单引号字符串
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
        BEGIN(STRING_SINGLE);
        scanner_state = STATE_SINGLE;
    }

    {DOUBLE_QUOTE} {
        // 进入双引号字符串
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
        BEGIN(STRING_DOUBLE);
        scanner_state = STATE_DOUBLE;
    }

    {MINUS}{MINUS} {
        // 进入单行注释
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
        BEGIN(COMMENT);
        scanner_state = STATE_COMMENT;
    }

    {SEMICOLON} {
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));  // 分号计入SQL
        send_to_server();       // 发送到服务端
    }

    . {
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
    }
}

<STRING_SINGLE>{
    {SINGLE_QUOTE}{SINGLE_QUOTE} {
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
    }

    {SINGLE_QUOTE} {
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
        BEGIN(INITIAL);
        scanner_state = STATE_INITIAL;
    }

    \n {
        // 保留换行在字符串中，不退出字符串
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
    }

    . {
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
    }
}


<STRING_DOUBLE>{
    {DOUBLE_QUOTE}{DOUBLE_QUOTE} {
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
    }

    {DOUBLE_QUOTE} {
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
        BEGIN(INITIAL);
        scanner_state = STATE_INITIAL;
    }

    \n {
        // 保留换行在字符串中，不退出字符串
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
    }

    . {
        append_to_sql(yytext, static_cast<std::size_t>(yyleng));
    }
}

<<EOF>> {
    yyterminate();
}

%%