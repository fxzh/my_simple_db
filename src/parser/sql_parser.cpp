// sql_parser.cpp: 对外提供线程安全的 SQL 解析入口
// 全部解析状态(scanner/parser)均为本次调用的栈上实例, 无模块级共享状态,
// 多个线程可并发解析而无需加锁
#include <istream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "parser.tab.hh"
#include "sql_parser.h"
#include "sql_scanner.h"

namespace sql {

bool parse(const std::string& stmt, std::string& error, std::string& stmt_kind,
           std::unique_ptr<SQLStatement>& result)
{
    stmt_kind.clear();
    error.clear();
    result.reset();

    // 用字符串流作为本次解析的输入源
    std::istringstream stmt_stream(stmt);
    SQLScanner scanner(&stmt_stream, &error);  // 词法错误也写入 error 缓冲

    yy::location loc;
    yy::parser parser(loc, stmt_kind, result, error, &scanner);
    int ret = parser.parse();

    if (ret == 0 && error.empty()) {
        return true;
    }
    if (error.empty()) {
        error = "syntax error";
    }
    return false;
}

}  // namespace sql
