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

// 绑定一条语句: 限定表名转 TableRef(未限定填 current_schema)、
// 类型映射与长度校验(create)、system 名下保留表名拦截(drop/insert/delete/update)、
// SET 模式门禁与变量名绑定(仅 bootstrap 模式可用, 正常模式直接报错)、
// 表存在性/值类型/长度/范围/NOT NULL/个数/常量性检查(insert)、赋值绑定: 行上下文右值/
// 静态类型匹配/纯常量右值的 NOT NULL 与长度检查, 列引用右值留执行期、
// 表达式类型推导与 WHERE 布尔校验(select/delete/update 带 WHERE)、投影展开与投影类型检查(select);
// 绑定错误当场经 DB_RAISE 记日志后抛出;
// 产物持有绑定表达式树(名字已解析为行内下标), 绑定后不依赖 stmt
std::unique_ptr<BoundStmt> analyze(ct::Catalog& db, const SQLStatement& stmt,
                                   const std::string& current_schema);

}  // namespace ana

#endif  // ANALYZER_ANALYZER_H
