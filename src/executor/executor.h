// executor.h: 执行层入口: 把 parser 的 AST 转成 storage 调用, 返回结构化执行结果
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>
#include <vector>

#include "storage/types.h"

namespace st {
class Database;  // 存储引擎门面, 完整定义见 storage.h
}

class SQLStatement;  // AST 语句基类, 完整定义见 ast.hh

namespace exec {

// 执行结果: 状态文本或结果集, 二选一
struct ExecResult {
    bool is_result_set = false;
    std::string status;                          // is_result_set=false 时有效
    std::vector<std::string> col_names;          // 结果集列名
    std::vector<std::vector<st::Value>> rows;    // 结果集行值(monostate 即 NULL)
};

// 执行一条已解析的语句:
// 建表/删表/插行成功回状态 "OK", 删行回 "OK (删除 N 行)", 查询回结果集;
// 执行错误当场经 log.h 记录后以 std::exception 抛出, 由调用方回客户端 "ERROR: <原因>"
ExecResult execute(st::Database& db, const SQLStatement& stmt);

}  // namespace exec

#endif  // EXECUTOR_H
