// executor.h: 执行层入口: 把 parser 的 AST 转成 storage 调用, 返回给 client 的回复文本
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>

namespace st {
class Database;  // 存储引擎门面, 完整定义见 storage.h
}

class SQLStatement;  // AST 语句基类, 完整定义见 ast.hh

namespace exec {

// 执行一条已解析的语句, 成功返回回复文本:
// 建表/删表成功回 "OK"; 尚未支持的语句种类回 "ERROR: <kind> 暂不支持";
// 执行错误当场经 log.h 记录后以 std::exception 抛出, 由调用方回客户端 "ERROR: <原因>"
std::string execute(st::Database& db, const SQLStatement& stmt);

}  // namespace exec

#endif  // EXECUTOR_H