// analyzer_internal.h: 语义分析目录内共享声明(行结构/表达式绑定/INSERT/SELECT 绑定), 仅供本目录使用
#ifndef ANALYZER_ANALYZER_INTERNAL_H
#define ANALYZER_ANALYZER_INTERNAL_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hh"
#include "bound.h"
#include "bound_expr.h"
#include "catalog.h"

namespace ana {

// ==================== 行结构 ====================

// 行结构(名字解析结果): 列名定位表 + 列静态类型, 绑定表达式树时使用
using ColMap = std::unordered_map<std::string, size_t>;
struct Schema {
    ColMap cols;                         // 列名 → 行内下标
    std::vector<st::ColType> col_types;  // 列静态类型, 与行内下标对应
};

// 绑定行结构: 列名定位表 + 列静态类型
Schema build_schema(const st::TableMeta& meta);

// ==================== 类型工具 ====================

// 整型类(Int/BigInt)判定
bool is_int_type(st::ColType t);
// 浮点类(Float/Double)判定
bool is_float_type(st::ColType t);
// 文本类(Char/VarChar)判定
bool is_text_type(st::ColType t);

// ==================== 表达式绑定 ====================

// 表达式静态类型直接用列类型推导: 字面量/列引用/算术按 ColType 级定型, Null 表示无类型
// (NULL 字面量, 比较时通配); 推导规则与求值器行为逐点对齐, 绑定层放过的表达式执行层不因类型报错
std::unique_ptr<BoundExpr> bind_expr(const Expr& expr, const Schema* schema, ct::Catalog& db,
                                     st::ColType& type);
// WHERE 条件绑定: 须为布尔(NULL 视为 UNKNOWN 放行), 返回绑定谓词树
std::unique_ptr<BoundExpr> bind_where(const Expr& where, const Schema& schema, ct::Catalog& db);
// 常量表达式求值结果是否可能为 NULL: 树中含 NULL 字面量即可能(IS NULL 结果必为布尔)
bool may_be_null(const Expr& expr);
// 表达式是否引用列: 含标识符节点即非纯常量, 可空性与字面量长度/范围不再可静态判定
bool has_col_ref(const Expr& expr);

// ==================== INSERT 绑定 ====================

// 值类型与列规格匹配(常量上下文): NULL 值放行(NOT NULL 已在前置检查拦截), 家族内匹配,
// 字面量范围/长度随类型一并校验
void check_value_type(const Expr& expr, st::ColType t, const st::ColumnSpec& col);
// 行上下文右值与列的静态类型匹配: 家族内匹配, NULL 静态类型放行, 空值/长度/范围留待执行期
void check_update_type(st::ColType t, const st::ColumnSpec& col);

// INSERT 值到列的定位结果: 由列清单一次性解析, 各行共用
struct InsertTargets {
    std::vector<size_t> target;    // 值序 → 行内列下标
    std::vector<bool> specified;   // 各列是否被值指定
};
// 解析列清单: 空清单按表全列, 指定清单逐名解析并查重
InsertTargets resolve_insert_targets(const std::vector<std::string>& columns,
                                     const st::TableMeta& meta);
// 单行 INSERT 值绑定: 推导/个数/NOT NULL/类型匹配, 检查顺序与原执行链一致, 文案不变;
// 产出与表列等宽的值序列, 未指定列补 NULL
std::vector<std::unique_ptr<BoundExpr>> bind_insert_row(
    const std::vector<std::unique_ptr<Expr>>& values, const InsertTargets& t,
    const st::TableMeta& meta, ct::Catalog& db);

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

#endif  // ANALYZER_ANALYZER_INTERNAL_H
