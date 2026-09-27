// operator.h: 迭代子算子: 计划节点树逐节点映射为拉取式算子(open/next/close)
#ifndef EXECUTOR_OPERATOR_H
#define EXECUTOR_OPERATOR_H

#include <memory>

#include "storage/types.h"

namespace ct {
class Catalog;  // 目录层门面, 完整定义见 catalog.h
}

namespace pl {
struct PlanNode;  // 计划节点基类, 完整定义见 plan.h
}

namespace exec {

// 算子基类: open 建立迭代状态, next 逐行拉取(无更多行返回 false), close 释放资源
struct Operator {
    virtual ~Operator() = default;
    virtual void open() = 0;
    virtual bool next(st::Row* out) = 0;
    virtual void close() = 0;
};

// 计划节点树 → 算子树(子节点递归先建), 叶子计划(DDL/INSERT)不进算子树
std::unique_ptr<Operator> make_operator(ct::Catalog& db, const pl::PlanNode& node);

}  // namespace exec

#endif  // EXECUTOR_OPERATOR_H
