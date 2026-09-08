#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include <string>

namespace sql {

// 解析一段 SQL 文本(语句以 ';' 结尾, 允许空输入/空语句/多条语句)
// 内部基于全局状态(lexer/yylval/yylloc), 用锁串行化, 可多线程调用
// 解析成功返回 true; 失败返回 false, error 为 "行.列: 错误描述" 形式的信息
// 成功时 stmt_kind 为识别出的首个语句种类(create table/drop table/insert into),
// 空输入或仅空语句(";")时 stmt_kind 为空
bool parse(const std::string& stmt, std::string& error, std::string& stmt_kind);

}  // namespace sql

#endif  // SQL_PARSER_H
