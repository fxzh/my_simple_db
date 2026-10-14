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

// 行结构(名字解析结果): 列名定位表 + 列静态类型, 绑定表达式树时使用
using ColMap = std::unordered_map<std::string, size_t>;
struct RowDesc {
    ColMap cols;                         // 列名 → 行内下标
    std::vector<st::ColType> col_types;  // 列静态类型, 与行内下标对应
};

// 绑定行结构: 列名定位表 + 列静态类型
RowDesc build_row_desc(const st::TableMeta& meta);

// ==================== 类型工具 ====================

// 整型类(Int/BigInt)判定
bool is_int_type(st::ColType t);
// 浮点类(Float/Double)判定
bool is_float_type(st::ColType t);
// 文本类(Char/VarChar)判定
bool is_text_type(st::ColType t);
// 语法层类型定则映射存储层: 域 + 长度 + 标度
// (char 显式长度须 1..65535, 未声明缺省 1; numeric 显式精度须 1..38 且标度不超精度,
// 未声明精度为 0 即无约束; 其余类型长度/标度恒 0)
st::ColType map_type_spec(DataType type, std::optional<long long> length,
                          std::optional<long long> scale, uint16_t& len, uint8_t& scl);

// ==================== 表达式绑定 ====================

// 表达式静态类型直接用列类型推导: 字面量/列引用/算术按 ColType 级定型, Null 表示无类型
// (NULL 字面量, 比较时通配); 推导规则与求值器行为逐点对齐, 绑定层放过的表达式执行层不因类型报错
std::unique_ptr<BoundExpr> bind_expr(const Expr& expr, const RowDesc* row_desc, ct::Catalog& db,
                                     st::ColType& type);
// WHERE 条件绑定: 须为布尔(NULL 视为 UNKNOWN 放行), 返回绑定谓词树
std::unique_ptr<BoundExpr> bind_where(const Expr& where, const RowDesc& row_desc, ct::Catalog& db);
// 常量表达式求值结果是否可能为 NULL: 树中含 NULL 字面量即可能(IS NULL 结果必为布尔)
bool may_be_null(const Expr& expr);
// 表达式是否引用列: 含标识符节点即非纯常量, 可空性与字面量长度/范围不再可静态判定
bool has_col_ref(const Expr& expr);

}  // namespace ana

#endif  // ANALYZER_ANALYZER_EXPR_H
