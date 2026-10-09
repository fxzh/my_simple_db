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

// 语义分析函数
std::unique_ptr<BoundStmt> analyze(ct::Catalog& db, const SQLStatement& stmt,
                                   const std::string& current_schema);

}  // namespace ana

#endif  // ANALYZER_ANALYZER_H
