#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include <memory>
#include <string>

class SQLStatement;

namespace sql {

// 解析一段 SQL 文本(语句以 ';' 结尾, 允许空输入/空语句/多条语句)
// 多线程可并发调用而无需加锁
bool parse(const std::string& stmt, std::string& error,
           std::unique_ptr<SQLStatement>& result);

}  // namespace sql

#endif  // SQL_PARSER_H
