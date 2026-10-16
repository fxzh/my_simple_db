// analyzer_expr.h: 语义分析目录内共享声明(行结构/类型工具/表达式绑定), 仅供本目录使用
#ifndef ANALYZER_ANALYZER_EXPR_H
#define ANALYZER_ANALYZER_EXPR_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hh"
#include "bound_expr.h"
#include "catalog.h"

namespace ana {

// ==================== 行结构 ====================

// 行结构(名字解析结果): 列名定位表 + 列静态类型属性, 绑定表达式树时使用
using ColMap = std::unordered_map<std::string, size_t>;
struct RowDesc {
    ColMap cols;                            // 列名 → 行内下标
    std::vector<st::TypeAttr> col_types;    // 列静态类型属性, 与行内下标对应
};

// 绑定行结构: 列名定位表 + 列静态类型属性
RowDesc build_row_desc(const st::TableMeta& meta);

// ==================== 类型工具 ====================

// 整型类判定(int/bigint 共用盒形式)
inline bool is_int_type(st::TypeAttr t) { return t.rep == st::Rep::Int64; }
// 浮点类判定
inline bool is_float_type(st::TypeAttr t)
{
    return t.rep == st::Rep::Float || t.rep == st::Rep::Double;
}
// 文本类判定
inline bool is_text_type(st::TypeAttr t) { return t.family == st::Family::Text; }
// 无类型判定(仅值域 NULL)
inline bool is_untyped(st::TypeAttr t) { return t.id == 0; }

// 类型名经 db_type 定型为属性并校验长度/标度:
// 接受长度的类型显式长度须 1..上限, 未声明取缺省(如 char 缺省 1, numeric 缺省 0 即无约束);
// 接受标度的类型标度须不超长度
st::TypeAttr map_type_spec(ct::Catalog& db, const std::string& type,
                           std::optional<long long> length, std::optional<long long> scale,
                           uint16_t& len, uint8_t& scl);

// ==================== 表达式绑定 ====================

// 表达式静态类型直接用列类型推导: 字面量/列引用/算术按类型属性定型, 无类型表示 NULL 字面量
// (比较时通配); 推导规则与求值器行为逐点对齐, 绑定层放过的表达式执行层不因类型报错
std::unique_ptr<BoundExpr> bind_expr(const Expr& expr, const RowDesc* row_desc, ct::Catalog& db,
                                     st::TypeAttr& type);
// WHERE 条件绑定: 须为布尔(NULL 视为 UNKNOWN 放行), 返回绑定谓词树
std::unique_ptr<BoundExpr> bind_where(const Expr& where, const RowDesc& row_desc, ct::Catalog& db);
// 常量表达式求值结果是否可能为 NULL: 树中含 NULL 字面量即可能(IS NULL 结果必为布尔),
// 函数调用按 db_func 该名重载行分派: 严格则实参可能 NULL 即可能, 非严格静态不可断定
bool may_be_null(const Expr& expr, ct::Catalog& db);
// 表达式是否引用列: 含标识符节点即非纯常量, 可空性与字面量长度/范围不再可静态判定
bool has_col_ref(const Expr& expr);

}  // namespace ana

#endif  // ANALYZER_ANALYZER_EXPR_H
