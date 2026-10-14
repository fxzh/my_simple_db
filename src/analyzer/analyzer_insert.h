// analyzer_insert.h: 语义分析目录内共享声明(INSERT 绑定), 仅供本目录使用
#ifndef ANALYZER_ANALYZER_INSERT_H
#define ANALYZER_ANALYZER_INSERT_H

#include <memory>
#include <string>
#include <vector>

#include "ast.hh"
#include "bound_expr.h"
#include "catalog.h"

namespace ana {

// ==================== INSERT 绑定 ====================

// 值类型与列规格匹配(常量上下文): 恒 NULL 常量树免检(NOT NULL 已在前置检查拦截),
// 家族内匹配, 字面量范围/长度随类型一并校验; 非字面量常量(如 CAST 产物)的范围/长度
// 由存储编码与 cast 收窄检查
void check_value_type(const Expr& expr, st::TypeAttr t, const st::ColumnSpec& col);
// 行上下文右值与列的静态类型匹配: 家族内匹配, 无类型放行, 空值/长度/范围留待执行期
void check_update_type(st::TypeAttr t, const st::ColumnSpec& col);

// INSERT 值到列的定位结果: 由列清单一次性解析, 各行共用
struct InsertTargets {
    std::vector<size_t> target;    // 值序 → 行内列下标
    std::vector<bool> specified;   // 各列是否被值指定
};
// 解析列清单: 空清单按表全列, 指定清单逐名解析并查重
InsertTargets resolve_insert_targets(const std::vector<std::string>& columns,
                                     const st::TableMeta& meta);
// 单行 INSERT 值绑定: 推导/个数/NOT NULL/类型匹配;
// 产出与表列等宽的值序列, 未指定列补 NULL
std::vector<std::unique_ptr<BoundExpr>> bind_insert_row(
    const std::vector<std::unique_ptr<Expr>>& values, const InsertTargets& t,
    const st::TableMeta& meta, ct::Catalog& db);

}  // namespace ana

#endif  // ANALYZER_ANALYZER_INSERT_H
