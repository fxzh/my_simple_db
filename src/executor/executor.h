// executor.h: 执行层入口: 把 parser 的 AST 转成 catalog 调用, 返回结构化执行结果
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "operator.h"
#include "plan.h"
#include "proto/proto.h"

namespace ct {
class Catalog;  // 目录层门面, 完整定义见 catalog.h
}

class SQLStatement;  // AST 语句基类, 完整定义见 ast.hh

namespace exec {

// 执行结果: 命令标签或流式结果集, 二选一
struct ExecResult {
    bool is_result_set = false;
    proto::CommandTag tag = proto::CommandTag::Empty;  // is_result_set=false 时有效
    uint64_t count = 0;                                // insert/delete 影响行数
    std::vector<std::string> col_names;                // 结果集列名
    std::unique_ptr<pl::PlanNode> plan;                // 结果集计划树, 算子引用其中数据
    std::unique_ptr<Operator> stream;                  // 已 open 的结果集算子, 拉尽后 close
};

// 执行一条已解析的语句:
// 建表/删表/插行/删行回命令标签与影响行数(insert 为实际行数, delete 为实际删除数);
// 查询回列名与已 open 的算子(计划树随行携带, 算子引用其中数据), 调用方逐行拉取流式发送, 用毕 close;
// 执行错误当场经 log.h 记录后以 std::exception 抛出, 由调用方回客户端 "ERROR: <原因>"
ExecResult execute(ct::Catalog& db, const SQLStatement& stmt);

}  // namespace exec

#endif  // EXECUTOR_H
