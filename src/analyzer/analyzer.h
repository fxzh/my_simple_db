// analyzer.h: 语义分析层入口: AST + catalog 元数据 → 名字解析与类型检查后的绑定语句
#ifndef ANALYZER_ANALYZER_H
#define ANALYZER_ANALYZER_H

#include <memory>

#include "ast.hh"
#include "bound.h"

namespace ct {
class Catalog;  // 目录层门面, 完整定义见 catalog.h
}

namespace ana {

// 绑定一条语句: 类型映射与长度校验(create)、保留表名拦截(drop/insert/delete)、
// 表存在性/值类型/长度/范围/NOT NULL/个数/常量性检查(insert)、表达式类型推导与
// WHERE 布尔校验(select/delete 带 WHERE)、投影展开与投影类型检查(select);
// 绑定错误当场经 DB_RAISE 记日志后抛出;
// 产物中的表达式指针指向 stmt 原节点, 生命周期由调用方保证
std::unique_ptr<BoundStmt> analyze(ct::Catalog& db, const SQLStatement& stmt);

}  // namespace ana

#endif  // ANALYZER_ANALYZER_H
