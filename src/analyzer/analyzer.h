// analyzer.h: 绑定层入口: AST + catalog 元数据 → 名字解析后的绑定语句
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
// 列名解析与 WHERE 列校验(select/delete 带 WHERE)、投影展开(select);
// 绑定错误当场经 DB_RAISE 记日志后抛出;
// 产物中的表达式指针指向 stmt 原节点, 生命周期由调用方保证
std::unique_ptr<BoundStmt> analyze(ct::Catalog& db, const SQLStatement& stmt);

}  // namespace ana

#endif  // ANALYZER_ANALYZER_H
