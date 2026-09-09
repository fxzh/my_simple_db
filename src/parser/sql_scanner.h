// sql_scanner.h: 可重入的 flex C++ 扫描器
// flex/bison 生成的代码依赖本次解析的词法状态(语义值/位置/行/列/错误缓冲),
// 把这些状态作为 SQLScanner 实例成员, 经 %option yyclass 让 flex 把词法动作
// 生成进本类, 从而消除模块级全局变量, 解析可并发执行而无需加锁
#ifndef PARSER_SQL_SCANNER_H
#define PARSER_SQL_SCANNER_H

#include <istream>
#include <string>

#include "parser.tab.hh"  // yy::parser::semantic_type / location_type

// flex 生成的 lex.yy.cc 也会 include <FlexLexer.h>, 用守卫保证只定义一次
#if !defined(yyFlexLexerOnce)
#include <FlexLexer.h>
#endif

class SQLScanner : public yyFlexLexer
{
public:
    SQLScanner(std::istream* in, std::string* err);
    ~SQLScanner() override;

    // parser 调用的词法接口: 记录语义值/位置后进入 flex 生成的扫描主循环
    int yylex(yy::parser::semantic_type* lval, yy::parser::location_type* lloc);

private:
    // flex 生成的扫描主循环(实现 yyFlexLexer 的纯虚 yylex)
    int yylex() override;

    yy::parser::semantic_type* yylval = nullptr;
    yy::parser::location_type* yylloc = nullptr;
    std::string* err_buf = nullptr;  // 指向本次解析的错误缓冲
    unsigned line = 1;
    unsigned column = 1;
};

#endif  // PARSER_SQL_SCANNER_H
