%{
#include <charconv>
#include <string>
#include <sstream>
#include "parser.tab.hh"
#include "sql_scanner.h"

// C++ 扫描器: 词法状态全部存放于 SQLScanner 实例成员(见 sql_scanner.h),
// 无模块级全局变量, 可与其他解析并发执行

// 位置更新宏: 在每个token匹配时写入扫描器实例成员(与 yylloc/line/column 配套),
// 匹配文本内含换行时同步推进行列, 保证跨行 token(字符串/块注释)位置正确
#define YY_USER_ACTION \
    { \
        unsigned nl = 0; \
        unsigned last_nl = 0; \
        for (int i = 0; i < yyleng; ++i) { \
            if (yytext[i] == '\n') { \
                ++nl; \
                last_nl = static_cast<unsigned>(i); \
            } \
        } \
        yylloc->begin.line = line; \
        yylloc->begin.column = column; \
        if (nl == 0) { \
            yylloc->end.line = line; \
            yylloc->end.column = column + static_cast<unsigned>(yyleng); \
            column = yylloc->end.column; \
        } else { \
            line += nl; \
            yylloc->end.line = line; \
            yylloc->end.column = static_cast<unsigned>(yyleng) - last_nl; \
            column = yylloc->end.column; \
        } \
    }
%}

%option noyywrap
%option nounput
%option noinput
%option c++
%option yyclass="SQLScanner"
%option case-insensitive

%x COMMENT_BLOCK

DIGIT    [0-9]
ID       [a-zA-Z_][a-zA-Z0-9_]*
WS       [ \t\r]

%%

"create"   { return yy::parser::token::CREATE; }
"table"    { return yy::parser::token::TABLE; }
"schema"   { return yy::parser::token::SCHEMA; }
"index"    { return yy::parser::token::INDEX; }
"on"       { return yy::parser::token::ON; }
"drop"     { return yy::parser::token::DROP; }
"delete"   { return yy::parser::token::DELETE; }
"from"     { return yy::parser::token::FROM; }
"insert"   { return yy::parser::token::INSERT; }
"into"     { return yy::parser::token::INTO; }
"values"   { return yy::parser::token::VALUES; }
"update"   { return yy::parser::token::UPDATE; }
"int"      { return yy::parser::token::INT; }
"bigint"   { return yy::parser::token::BIGINT; }
"float"    { return yy::parser::token::FLOAT; }
"char"     { return yy::parser::token::CHAR; }
"varchar"  { return yy::parser::token::VARCHAR; }
"double"   { return yy::parser::token::DOUBLE; }
"boolean"  { return yy::parser::token::BOOLEAN; }
"bool"     { return yy::parser::token::BOOL; }
"explain"  { return yy::parser::token::EXPLAIN; }
"select"   { return yy::parser::token::SELECT; }
"as"       { return yy::parser::token::AS; }
"order"    { return yy::parser::token::ORDER; }
"by"       { return yy::parser::token::BY; }
"asc"      { return yy::parser::token::ASC; }
"desc"     { return yy::parser::token::DESC; }
"null"     { return yy::parser::token::NULL_T; }
"true"     { return yy::parser::token::TRUE_T; }
"false"    { return yy::parser::token::FALSE_T; }
"where"    { return yy::parser::token::WHERE; }
"and"      { return yy::parser::token::AND; }
"or"       { return yy::parser::token::OR; }
"not"      { return yy::parser::token::NOT; }
"is"       { return yy::parser::token::IS; }
"set"      { return yy::parser::token::SET; }
"begin"    { return yy::parser::token::BEGIN_TXN; }
"start"    { return yy::parser::token::START; }
"transaction" { return yy::parser::token::TRANSACTION; }
"commit"   { return yy::parser::token::COMMIT; }
"rollback" { return yy::parser::token::ROLLBACK; }
"work"     { return yy::parser::token::WORK; }

{DIGIT}+"."{DIGIT}*|"."{DIGIT}+ {
    // 超出 double 可表示范围(上溢/下溢)走词法错误通道报错
    double val = 0;
    const auto conv = std::from_chars(yytext, yytext + yyleng, val);
    if (conv.ec == std::errc::result_out_of_range) {
        std::ostringstream oss;
        oss << *yylloc << ": float literal out of range: " << yytext;
        *err_buf = oss.str();
        return yy::parser::token::TOK_ERROR;
    }
    yylval->build<double>(val);
    return yy::parser::token::FLOAT_NUM;
}

{DIGIT}+ {
    // 超出 int64 范围走词法错误通道报错
    long long val = 0;
    const auto conv = std::from_chars(yytext, yytext + yyleng, val);
    if (conv.ec == std::errc::result_out_of_range) {
        std::ostringstream oss;
        oss << *yylloc << ": integer literal out of range: " << yytext;
        *err_buf = oss.str();
        return yy::parser::token::TOK_ERROR;
    }
    yylval->build<long long>(val);
    return yy::parser::token::INTEGER;
}

'([^']|'')*' {
    // 去掉首尾引号, 折叠 '' 转义为单个引号
    std::string val;
    for (int i = 1; i < yyleng - 1; ++i) {
        val += yytext[i];
        if (yytext[i] == '\'' && yytext[i + 1] == '\'') {
            ++i;
        }
    }
    yylval->build<std::string>(std::move(val));
    return yy::parser::token::STRING;
}

\"([^"]|\"\")*\" {
    // 界定标识符: 去掉首尾引号, 折叠 "" 转义为单个引号
    std::string val;
    for (int i = 1; i < yyleng - 1; ++i) {
        val += yytext[i];
        if (yytext[i] == '"' && yytext[i + 1] == '"') {
            ++i;
        }
    }
    yylval->build<std::string>(std::move(val));
    return yy::parser::token::IDENTIFIER;
}

{ID} {
    // 裸标识符: ASCII 大写字母折叠为小写
    std::string val;
    for (int i = 0; i < yyleng; ++i) {
        char c = yytext[i];
        val += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    yylval->build<std::string>(std::move(val));
    return yy::parser::token::IDENTIFIER;
}

"<="       { return yy::parser::token::LE; }
">="       { return yy::parser::token::GE; }
"=="|"="   { return yy::parser::token::EQ; }
"<>"|"!="  { return yy::parser::token::NE; }
"+"|"-"|"*"|"/"|"("|")"|","|";"|"."|"<"|">" {
    return yytext[0];
}

\n {
    // 换行: 行列推进由 YY_USER_ACTION 统一处理
}

{WS}+ {
    // 忽略空白
}

"--"[^\n]* {
    // 单行注释: 跳过
}

"/*" {
    // 块注释开始
    BEGIN(COMMENT_BLOCK);
}

<COMMENT_BLOCK>{
    "*"+"/" {
        // 块注释结束
        BEGIN(INITIAL);
    }

    [^*]+|"*"+ {
        // 注释内容: 跳过
    }

    <<EOF>> {
        // 块注释未闭合即到达输入末尾
        std::ostringstream oss;
        oss << *yylloc << ": unterminated block comment";
        *err_buf = oss.str();
        return yy::parser::token::TOK_ERROR;
    }
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
