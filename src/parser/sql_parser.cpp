// sql_parser.cpp: 对外提供线程安全的 SQL 解析入口
// flex/bison 生成的代码依赖全局状态(lexer 指针/yylval/yylloc/错误缓冲),
// 因此用全局锁把整个解析过程串行化
#include <istream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <FlexLexer.h>

#include "parser.tab.hh"
#include "sql_parser.h"

// parser.y 的 %code 中定义
extern yyFlexLexer* lexer;
// lexer.l 中定义
extern void reset_lexer_location();
// 本文件定义, parser.y 的 error()/lexer.l 的非法字符规则写入
std::string sql_parse_error;

namespace sql {

bool parse(const std::string& stmt, std::string& error, std::string& stmt_kind)
{
    // flex/bison 的接口基于全局状态, 多线程必须串行访问
    static std::mutex parse_mutex;
    std::lock_guard<std::mutex> lock(parse_mutex);

    sql_parse_error.clear();
    stmt_kind.clear();
    reset_lexer_location();

    // 用字符串流作为本次解析的输入源
    std::istringstream stmt_stream(stmt);
    yyFlexLexer flexLexer(&stmt_stream);
    lexer = &flexLexer;

    yy::location loc;
    yy::parser parser(loc, stmt_kind);
    int ret = parser.parse();

    lexer = nullptr;  // flexLexer 离开作用域前先解除引用

    if (ret == 0 && sql_parse_error.empty()) {
        error.clear();
        return true;
    }
    error = sql_parse_error.empty() ? std::string("syntax error")
                                    : std::move(sql_parse_error);
    return false;
}

}  // namespace sql
