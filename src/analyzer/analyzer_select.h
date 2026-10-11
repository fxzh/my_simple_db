// analyzer_select.h: 语义分析目录内共享声明(SELECT 绑定), 仅供本目录使用
#ifndef ANALYZER_ANALYZER_SELECT_H
#define ANALYZER_ANALYZER_SELECT_H

#include <vector>

#include "analyzer_expr.h"
#include "ast.hh"
#include "bound.h"
#include "catalog.h"

namespace ana {

// ==================== SELECT 绑定 ====================

// 投影产物: 投影列 + 对齐的源 AST 表达式(star 展开列为空, 供排序键重绑投影表达式)
struct ProjBind {
    std::vector<ProjCol> projs;
    std::vector<const Expr*> src;
};
// SELECT 投影: star 按表列展开
ProjBind build_projs(const SelectStmt& ss, const st::TableMeta& meta, const Schema& schema,
                     ct::Catalog& db);
// 绑定无 FROM 的 SELECT 投影: 无列可解析, star 无表可展开
ProjBind build_projs_no_from(const SelectNoFromStmt& ss, const Schema& schema, ct::Catalog& db);
// SELECT 排序键: 全部键命中输出列时以输出列下标承载(排序在投影后), 否则全部键转
// 行上下文表达式(排序在投影前), 命中输出列的键重绑其源表达式或取 star 列引用
std::vector<BoundOrderItem> bind_orders(const std::vector<OrderItem>& items, const ProjBind& pb,
                                        const Schema& schema, bool& sort_on_output,
                                        ct::Catalog& db);

}  // namespace ana

#endif  // ANALYZER_ANALYZER_SELECT_H
