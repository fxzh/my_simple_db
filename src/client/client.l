%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client.h"

void append_to_sql(const char* text) {
    int len = strlen(text);
    if (sql_pos + len < sizeof(sql_buffer) - 1) {
        strcpy(sql_buffer + sql_pos, text);
        sql_pos += len;
    }
}

%}

%option noyywrap
%option caseless
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
        append_to_sql(yytext);
    }

    \n {
        BEGIN(INITIAL);
        scanner_state = STATE_INITIAL;
        append_to_sql(yytext);
    }
}

<INITIAL>{
    {SPACE}+ {
        if (sql_pos == 0) {
            // 忽略多余空白
        } else {
            append_to_sql(yytext);
        }
    }

    {SINGLE_QUOTE} {
        // 进入单引号字符串
        append_to_sql(yytext);
        BEGIN(STRING_SINGLE);
        scanner_state = STATE_SINGLE;
    }

    {DOUBLE_QUOTE} {
        // 进入双引号字符串
        append_to_sql(yytext);
        BEGIN(STRING_DOUBLE);
        scanner_state = STATE_DOUBLE;
    }

    {MINUS}{MINUS} {
        // 进入注释
        append_to_sql(yytext);
        BEGIN(COMMENT);
        scanner_state = STATE_COMMENT;
    }

    {SEMICOLON} {
        append_to_sql(yytext);  // 分号计入SQL
        send_to_server();       // 发送到服务端
    }

    . {
        append_to_sql(yytext);
    }
}

<STRING_SINGLE>{
    {SINGLE_QUOTE}{SINGLE_QUOTE} {
        append_to_sql(yytext);
    }

    {SINGLE_QUOTE} {
        append_to_sql(yytext);
        BEGIN(INITIAL);
        scanner_state = STATE_INITIAL;
    }

    \n {
        /* 保留换行在字符串中，不退出字符串 */
        append_to_sql(yytext);
    }

    . {
        append_to_sql(yytext);
    }
}


<STRING_DOUBLE>{
    {DOUBLE_QUOTE}{DOUBLE_QUOTE} {
        append_to_sql(yytext);
    }

    {DOUBLE_QUOTE} {
        append_to_sql(yytext);
        BEGIN(INITIAL);
        scanner_state = STATE_INITIAL;
    }

    \n {
        /* 保留换行在字符串中，不退出字符串 */
        append_to_sql(yytext);
    }

    . {
        append_to_sql(yytext);
    }
}

<<EOF>> {
    yyterminate();
}

%%

// 重置状态函数
void reset_parser() {
    reset_sql_buffer();
    BEGIN(INITIAL);
    scanner_state = STATE_INITIAL;
}
