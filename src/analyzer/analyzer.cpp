// analyzer.cpp: 语义分析层实现: AST + catalog 元数据 → BoundStmt
#include "analyzer.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hh"
#include "bound_expr.h"
#include "catalog.h"
#include "common/err.h"
#include "log/log.h"

namespace ana {

namespace {

// 语法层类型枚举映射到存储层列类型
st::ColType map_col_type(DataType type)
{
    switch (type) {
    case DataType::Int: return st::ColType::Int;
    case DataType::BigInt: return st::ColType::BigInt;
    case DataType::Float: return st::ColType::Float;
    case DataType::Double: return st::ColType::Double;
    case DataType::Char: return st::ColType::Char;
    case DataType::VarChar: return st::ColType::VarChar;
    case DataType::Bool: return st::ColType::Bool;
    }
    DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "未映射的列类型: {}", static_cast<int>(type));
}

// AST 列定义转存储层列规格; 显式长度非法时当场记录并抛出
void convert_columns(const std::vector<ColumnDef>& defs, std::vector<st::ColumnSpec>& cols)
{
    cols.reserve(defs.size());
    for (const ColumnDef& def : defs) {
        // 显式长度取值 1..65535; 未声明时 char 缺省 1, varchar 缺省 0(动态), 其余为 0
        uint16_t len = 0;
        if (def.length) {
            if (*def.length <= 0 || *def.length > UINT16_MAX) {
                DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "长度非法: {}", *def.length);
            }
            len = static_cast<uint16_t>(*def.length);
        } else if (def.type == DataType::Char) {
            len = 1;
        }
        cols.emplace_back(st::ColumnSpec{def.name, map_col_type(def.type), len, def.not_null});
    }
}

// 行结构(名字解析结果): 列名定位表 + 列静态类型 + char 定长列标记, 绑定表达式树时使用
using ColMap = std::unordered_map<std::string, size_t>;
struct Schema {
    ColMap cols;                         // 列名 → 行内下标
    std::vector<st::ColType> col_types;  // 列静态类型, 与行内下标对应
    std::vector<bool> char_col;          // char 定长列标记, 与行内下标对应
};

// 表达式静态类型: 推导规则与求值器行为逐点对齐, 绑定层放过的表达式执行层不因类型报错
enum class ExprType : uint8_t { Null, Int, Double, String, Bool };

std::unique_ptr<BoundExpr> bind_expr(const Expr& expr, const Schema* schema, ExprType& type);

// 数值类(Int/Double)判定
bool is_num_type(ExprType t)
{
    return t == ExprType::Int || t == ExprType::Double;
}

// 列静态类型映射: 整型归 Int, 浮点归 Double, 文本归 String
ExprType col_expr_type(st::ColType type)
{
    switch (type) {
    case st::ColType::Int:
    case st::ColType::BigInt: return ExprType::Int;
    case st::ColType::Float:
    case st::ColType::Double: return ExprType::Double;
    case st::ColType::Char:
    case st::ColType::VarChar: return ExprType::String;
    case st::ColType::Bool: return ExprType::Bool;
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未映射的列类型");
}

// 绑定表达式: 递归建树并推导类型, 名字解析当场完成; schema 为空表示常量上下文(禁止引用列)
std::unique_ptr<BoundExpr> bind_expr(const Expr& expr, const Schema* schema, ExprType& type)
{
    switch (expr.kind()) {
    case ExprKind::Int:
        type = ExprType::Int;
        return std::make_unique<BoundConst>(static_cast<const IntExpr&>(expr).value);
    case ExprKind::Float:
        type = ExprType::Double;
        return std::make_unique<BoundConst>(static_cast<const FloatExpr&>(expr).value);
    case ExprKind::String:
        type = ExprType::String;
        return std::make_unique<BoundConst>(static_cast<const StringExpr&>(expr).value);
    case ExprKind::Null:
        type = ExprType::Null;
        return std::make_unique<BoundConst>(std::monostate{});
    case ExprKind::Bool:
        type = ExprType::Bool;
        return std::make_unique<BoundConst>(static_cast<const BoolExpr&>(expr).value);
    case ExprKind::Identifier: {
        const auto& id = static_cast<const IdentifierExpr&>(expr);
        if (schema == nullptr) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "常量上下文不允许引用列: {}", id.name);
        }
        const auto it = schema->cols.find(id.name);
        if (it == schema->cols.end()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER, "列不存在: {}", id.name);
        }
        type = col_expr_type(schema->col_types[it->second]);
        return std::make_unique<BoundColRef>(it->second, schema->char_col[it->second]);
    }
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        ExprType lt;
        ExprType rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, schema, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, schema, rt);
        if ((!is_num_type(lt) && lt != ExprType::Null)
            || (!is_num_type(rt) && rt != ExprType::Null)) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "算术运算操作数不是数值");
        }
        if (lt == ExprType::Double || rt == ExprType::Double) {
            type = ExprType::Double;
        } else {
            type = ExprType::Int;
        }
        return std::make_unique<BoundArith>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        ExprType t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, schema, t);
        if (!is_num_type(t) && t != ExprType::Null) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "一元运算操作数不是数值");
        }
        type = t;
        if (e.op == '+') {
            return operand;  // 一元正号原值返回, 绑定期折为操作数本身
        }
        return std::make_unique<BoundNeg>(std::move(operand));
    }
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        ExprType lt;
        ExprType rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, schema, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, schema, rt);
        const bool same_num = is_num_type(lt) && is_num_type(rt);
        const bool same_str = lt == ExprType::String && rt == ExprType::String;
        const bool same_bool = lt == ExprType::Bool && rt == ExprType::Bool;
        if (lt != ExprType::Null && rt != ExprType::Null && !same_num && !same_str
            && !same_bool) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "比较运算两侧须同为数值、字符串或布尔");
        }
        type = ExprType::Bool;
        return std::make_unique<BoundCmp>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        ExprType lt;
        ExprType rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, schema, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, schema, rt);
        if ((lt != ExprType::Bool && lt != ExprType::Null)
            || (rt != ExprType::Bool && rt != ExprType::Null)) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "逻辑运算操作数不是布尔值");
        }
        type = ExprType::Bool;
        return std::make_unique<BoundLogic>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::Not: {
        const auto& e = static_cast<const NotExpr&>(expr);
        ExprType t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, schema, t);
        if (t != ExprType::Bool && t != ExprType::Null) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "逻辑运算操作数不是布尔值");
        }
        type = ExprType::Bool;
        return std::make_unique<BoundNot>(std::move(operand));
    }
    case ExprKind::IsNull: {
        const auto& e = static_cast<const IsNullExpr&>(expr);
        ExprType t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, schema, t);
        type = ExprType::Bool;
        return std::make_unique<BoundIsNull>(std::move(operand), e.negate);
    }
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

// 常量表达式求值结果是否可能为 NULL: 树中含 NULL 字面量即可能(IS NULL 结果必为布尔)
bool may_be_null(const Expr& expr)
{
    switch (expr.kind()) {
    case ExprKind::Null: return true;
    case ExprKind::Int:
    case ExprKind::Float:
    case ExprKind::String:
    case ExprKind::Bool:
    case ExprKind::Identifier:
        return false;
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        return may_be_null(*e.left) || may_be_null(*e.right);
    }
    case ExprKind::UnaryOp:
        return may_be_null(*static_cast<const UnaryOpExpr&>(expr).operand);
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        return may_be_null(*e.left) || may_be_null(*e.right);
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        return may_be_null(*e.left) || may_be_null(*e.right);
    }
    case ExprKind::Not:
        return may_be_null(*static_cast<const NotExpr&>(expr).operand);
    case ExprKind::IsNull:
        return false;
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

// 表达式是否引用列: 含标识符节点即非纯常量, 可空性与字面量长度/范围不再可静态判定
bool has_col_ref(const Expr& expr)
{
    switch (expr.kind()) {
    case ExprKind::Int:
    case ExprKind::Float:
    case ExprKind::String:
    case ExprKind::Bool:
    case ExprKind::Null:
        return false;
    case ExprKind::Identifier:
        return true;
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        return has_col_ref(*e.left) || has_col_ref(*e.right);
    }
    case ExprKind::UnaryOp:
        return has_col_ref(*static_cast<const UnaryOpExpr&>(expr).operand);
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        return has_col_ref(*e.left) || has_col_ref(*e.right);
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        return has_col_ref(*e.left) || has_col_ref(*e.right);
    }
    case ExprKind::Not:
        return has_col_ref(*static_cast<const NotExpr&>(expr).operand);
    case ExprKind::IsNull:
        return has_col_ref(*static_cast<const IsNullExpr&>(expr).operand);
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

// WHERE 条件绑定: 须为布尔(NULL 视为 UNKNOWN 放行), 返回绑定谓词树
std::unique_ptr<BoundExpr> bind_where(const Expr& where, const Schema& schema)
{
    ExprType t;
    std::unique_ptr<BoundExpr> pred = bind_expr(where, &schema, t);
    if (t != ExprType::Bool && t != ExprType::Null) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "WHERE 条件不是布尔表达式");
    }
    return pred;
}

// 值类型与列规格匹配: NULL 值放行(NOT NULL 已在前置检查拦截), 字面量范围/长度随类型一并校验
void check_value_type(const Expr& expr, ExprType t, const st::ColumnSpec& col)
{
    if (may_be_null(expr)) {
        return;
    }
    bool ok = false;
    switch (col.type) {
    case st::ColType::Int: {
        ok = t == ExprType::Int;
        // 字面量须在 int32 范围内, 折叠表达式的范围检查留待执行期
        if (ok && expr.kind() == ExprKind::Int) {
            const int64_t v = static_cast<const IntExpr&>(expr).value;
            ok = v >= INT32_MIN && v <= INT32_MAX;
        }
        break;
    }
    case st::ColType::BigInt:
        ok = t == ExprType::Int;
        break;
    case st::ColType::Double:
    case st::ColType::Float: {
        ok = t == ExprType::Double;
        // float 列字面量须可被 float 表示
        if (ok && col.type == st::ColType::Float && expr.kind() == ExprKind::Float) {
            const double d = static_cast<const FloatExpr&>(expr).value;
            ok = !std::isfinite(d) || std::isfinite(static_cast<float>(d));
        }
        break;
    }
    case st::ColType::Char:
        // 常量上下文的 String 必为字面量, 须不超声明长度
        ok = t == ExprType::String
             && static_cast<const StringExpr&>(expr).value.size() <= col.length;
        break;
    case st::ColType::VarChar:
        ok = t == ExprType::String
             && (col.length == 0
                 || static_cast<const StringExpr&>(expr).value.size() <= col.length);
        break;
    case st::ColType::Bool:
        ok = t == ExprType::Bool;
        break;
    }
    if (!ok) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "值与列类型不匹配");
    }
}

// 行上下文右值与列的静态类型匹配: NULL 静态类型放行, 空值/长度/范围留待执行期
void check_update_type(ExprType t, const st::ColumnSpec& col)
{
    bool ok = false;
    switch (col.type) {
    case st::ColType::Int:
    case st::ColType::BigInt:
        ok = t == ExprType::Int || t == ExprType::Null;
        break;
    case st::ColType::Float:
    case st::ColType::Double:
        ok = t == ExprType::Double || t == ExprType::Null;
        break;
    case st::ColType::Char:
    case st::ColType::VarChar:
        ok = t == ExprType::String || t == ExprType::Null;
        break;
    case st::ColType::Bool:
        ok = t == ExprType::Bool || t == ExprType::Null;
        break;
    }
    if (!ok) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "值与列类型不匹配");
    }
}

// INSERT 值到列的定位结果: 由列清单一次性解析, 各行共用
struct InsertTargets {
    std::vector<size_t> target;    // 值序 → 行内列下标
    std::vector<bool> specified;   // 各列是否被值指定
};

// 解析列清单: 空清单按表全列, 指定清单逐名解析并查重
InsertTargets resolve_insert_targets(const std::vector<std::string>& columns,
                                     const st::TableMeta& meta)
{
    InsertTargets t;
    t.specified.assign(meta.cols.size(), false);
    if (columns.empty()) {
        t.target.resize(meta.cols.size());
        for (size_t i = 0; i < meta.cols.size(); ++i) {
            t.target[i] = i;
            t.specified[i] = true;
        }
        return t;
    }
    ColMap cols;
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        cols.emplace(meta.cols[i].name, i);
    }
    for (const std::string& name : columns) {
        const auto it = cols.find(name);
        if (it == cols.end()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER, "列不存在: {}", name);
        }
        if (t.specified[it->second]) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "插入列清单中列重复: {}", name);
        }
        t.specified[it->second] = true;
        t.target.push_back(it->second);
    }
    return t;
}

// 单行 INSERT 值绑定: 推导/个数/NOT NULL/类型匹配, 检查顺序与原执行链一致, 文案不变;
// 产出与表列等宽的值序列, 未指定列补 NULL
std::vector<std::unique_ptr<BoundExpr>> bind_insert_row(
    const std::vector<std::unique_ptr<Expr>>& values, const InsertTargets& t,
    const st::TableMeta& meta)
{
    std::vector<std::unique_ptr<BoundExpr>> bound;
    std::vector<ExprType> types;
    types.reserve(values.size());
    for (const auto& v : values) {
        ExprType type;
        bound.push_back(bind_expr(*v, nullptr, type));  // 常量上下文: 禁止引用列
        types.push_back(type);
    }
    if (values.size() != t.target.size()) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "值的数量与列数不符");
    }
    for (size_t i = 0; i < values.size(); ++i) {
        if (meta.cols[t.target[i]].not_null && may_be_null(*values[i])) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "NOT NULL 列不允许 NULL: {}", meta.cols[t.target[i]].name);
        }
    }
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        if (!t.specified[i] && meta.cols[i].not_null) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "NOT NULL 列不允许 NULL: {}", meta.cols[i].name);
        }
    }
    for (size_t i = 0; i < values.size(); ++i) {
        check_value_type(*values[i], types[i], meta.cols[t.target[i]]);
    }
    // 归一化为全宽度: 值放到指定列, 其余列补 NULL 常量
    std::vector<std::unique_ptr<BoundExpr>> row(meta.cols.size());
    for (size_t i = 0; i < values.size(); ++i) {
        row[t.target[i]] = std::move(bound[i]);
    }
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        if (!row[i]) {
            row[i] = std::make_unique<BoundConst>(std::monostate{});
        }
    }
    return row;
}

// 保留表名拦截: system schema 下的元数据表禁止 drop/insert/delete/update
// (select 可查元数据, create 由 schema 内同名已存在拒绝; 其他 schema 同名表不受限)
void check_reserved_table(const ct::TableRef& table)
{
    if (table.schema != ct::kSystemSchemaName) {
        return;
    }
    if (table.name == ct::kTableMetaName || table.name == ct::kColumnMetaName
        || table.name == ct::kSchemaMetaName || table.name == ct::kIndexMetaName
        || table.name == ct::kVersionMetaName) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::ANALYZER, "保留表名禁止使用: {}",
                 table_ref_to_string(table));
    }
}

// AST 限定名转 catalog 表引用: 未限定名填入会话 current_schema
ct::TableRef to_table_ref(const QualifiedName& name, const std::string& current_schema)
{
    if (name.schema.empty()) {
        return {current_schema, name.name};
    }
    return {name.schema, name.name};
}

// 绑定行结构: 列名定位表 + 列静态类型 + char 定长列标记
Schema build_schema(const st::TableMeta& meta)
{
    Schema schema;
    schema.col_types.reserve(meta.cols.size());
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        schema.cols.emplace(meta.cols[i].name, i);
        schema.col_types.push_back(meta.cols[i].type);
        schema.char_col.push_back(meta.cols[i].type == st::ColType::Char);
    }
    return schema;
}

// 绑定单个投影项(非 star): 绑定表达式并定输出列名, 列名取别名>列名>表达式文本
ProjCol bind_proj_item(const SelectItem& item, const Schema& schema)
{
    ExprType t;
    std::unique_ptr<BoundExpr> e = bind_expr(*item.expr, &schema, t);
    std::string name;
    if (!item.alias.empty()) {
        name = item.alias;
    } else if (item.expr->kind() == ExprKind::Identifier) {
        name = static_cast<const IdentifierExpr&>(*item.expr).name;
    } else {
        name = expr_to_string(*item.expr);
    }
    return ProjCol{std::move(e), std::move(name), 0};
}

// 投影产物: 投影列 + 对齐的源 AST 表达式(star 展开列为空, 供排序键重绑投影表达式)
struct ProjBind {
    std::vector<ProjCol> projs;
    std::vector<const Expr*> src;
};

// SELECT 投影: star 按表列展开
ProjBind build_projs(const SelectStmt& ss, const st::TableMeta& meta, const Schema& schema)
{
    ProjBind out;
    for (const SelectItem& item : ss.items()) {
        if (item.star) {
            for (size_t i = 0; i < meta.cols.size(); ++i) {
                out.projs.push_back(ProjCol{nullptr, meta.cols[i].name, i});
                out.src.push_back(nullptr);
            }
            continue;
        }
        out.projs.push_back(bind_proj_item(item, schema));
        out.src.push_back(item.expr.get());
    }
    return out;
}

// 绑定无 FROM 的 SELECT 投影: 无列可解析, star 无表可展开
ProjBind build_projs_no_from(const SelectNoFromStmt& ss, const Schema& schema)
{
    ProjBind out;
    for (const SelectItem& item : ss.items()) {
        if (item.star) {
            DB_RAISE(db::ErrCode::StarNoFrom, LogModule::ANALYZER, "无 FROM 的 SELECT 不允许 *");
        }
        out.projs.push_back(bind_proj_item(item, schema));
        out.src.push_back(item.expr.get());
    }
    return out;
}

// 排序键解析中间结果: 命中输出列为下标, 否则为行上下文绑定表达式
struct OrderKey {
    bool is_out = false;
    size_t out_idx = 0;
    std::unique_ptr<BoundExpr> expr;
};

// 解析单个排序键: 整数常量按输出列序号; 裸名先按输出列名解析(命中多个报歧义, 未命中转
// 行上下文绑定); 非整数裸常量报错; 其余表达式按行上下文绑定
OrderKey resolve_order_key(const OrderItem& item, const ProjBind& pb, const Schema& schema)
{
    const Expr& e = *item.expr;
    OrderKey k;
    if (e.kind() == ExprKind::Int) {
        const int64_t pos = static_cast<const IntExpr&>(e).value;
        if (pos < 1 || pos > static_cast<int64_t>(pb.projs.size())) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER,
                     "ORDER BY 序号不在输出列范围: {}", pos);
        }
        k.is_out = true;
        k.out_idx = static_cast<size_t>(pos - 1);
        return k;
    }
    if (e.kind() == ExprKind::Identifier) {
        const auto& id = static_cast<const IdentifierExpr&>(e);
        size_t hits = 0;
        for (size_t i = 0; i < pb.projs.size(); ++i) {
            if (pb.projs[i].name == id.name) {
                hits += 1;
                k.out_idx = i;
            }
        }
        if (hits > 1) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "ORDER BY 输出列名歧义: {}",
                     id.name);
        }
        if (hits == 1) {
            k.is_out = true;
            return k;
        }
        // 未命中输出列名: 落到下方行上下文按列名解析
    } else if (e.kind() == ExprKind::Float || e.kind() == ExprKind::String
               || e.kind() == ExprKind::Null || e.kind() == ExprKind::Bool) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "ORDER BY 不允许非整数常量");
    }
    ExprType t;
    k.expr = bind_expr(e, &schema, t);
    return k;
}

// SELECT 排序键: 全部键命中输出列时以输出列下标承载(排序在投影后), 否则全部键转
// 行上下文表达式(排序在投影前), 命中输出列的键重绑其源表达式或取 star 列引用
std::vector<BoundOrderItem> bind_orders(const std::vector<OrderItem>& items, const ProjBind& pb,
                                        const Schema& schema, bool& sort_on_output)
{
    std::vector<OrderKey> keys;
    keys.reserve(items.size());
    bool all_out = true;
    for (const OrderItem& item : items) {
        keys.push_back(resolve_order_key(item, pb, schema));
        all_out = all_out && keys.back().is_out;
    }
    sort_on_output = all_out;
    std::vector<BoundOrderItem> orders;
    orders.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        BoundOrderItem o;
        o.desc = items[i].desc;
        if (all_out) {
            o.out_idx = keys[i].out_idx;
        } else if (keys[i].is_out && pb.src[keys[i].out_idx] != nullptr) {
            ExprType t;
            o.expr = bind_expr(*pb.src[keys[i].out_idx], &schema, t);
        } else if (keys[i].is_out) {
            // star 展开列(仅带 FROM 的 SELECT 存在): 取行内列引用
            const size_t col = pb.projs[keys[i].out_idx].col_idx;
            o.expr = std::make_unique<BoundColRef>(col, schema.char_col[col]);
        } else {
            o.expr = std::move(keys[i].expr);
        }
        orders.push_back(std::move(o));
    }
    return orders;
}

}  // namespace

std::unique_ptr<BoundStmt> analyze(ct::Catalog& db, const SQLStatement& stmt,
                                   const std::string& current_schema)
{
    switch (stmt.kind()) {
    case StmtKind::CreateTable: {
        const auto& cs = static_cast<const CreateTableStmt&>(stmt);
        auto b = std::make_unique<BoundCreateTable>();
        b->table = to_table_ref(cs.table_name(), current_schema);
        convert_columns(cs.column_defs(), b->cols);
        return b;
    }
    case StmtKind::DropTable: {
        const auto& ds = static_cast<const DropTableStmt&>(stmt);
        const ct::TableRef table = to_table_ref(ds.table_name(), current_schema);
        check_reserved_table(table);
        auto b = std::make_unique<BoundDropTable>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        return b;
    }
    case StmtKind::CreateSchema: {
        const auto& cs = static_cast<const CreateSchemaStmt&>(stmt);
        auto b = std::make_unique<BoundCreateSchema>();
        b->schema = cs.schema_name();
        return b;
    }
    case StmtKind::DropSchema: {
        const auto& ds = static_cast<const DropSchemaStmt&>(stmt);
        auto b = std::make_unique<BoundDropSchema>();
        b->schema = ds.schema_name();
        return b;
    }
    case StmtKind::CreateIndex: {
        const auto& cs = static_cast<const CreateIndexStmt&>(stmt);
        const ct::TableRef table = to_table_ref(cs.table_name(), current_schema);
        check_reserved_table(table);
        st::TableMeta meta = db.table_meta(table);
        size_t ordinal = meta.cols.size();
        for (size_t i = 0; i < meta.cols.size(); ++i) {
            if (meta.cols[i].name == cs.column_name()) {
                ordinal = i;
                break;
            }
        }
        if (ordinal == meta.cols.size()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER, "列不存在: {}",
                     cs.column_name());
        }
        const st::ColType type = meta.cols[ordinal].type;
        if (type != st::ColType::Int && type != st::ColType::BigInt && type != st::ColType::Float
            && type != st::ColType::Double && type != st::ColType::Bool) {
            DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "索引列类型不支持: {}",
                     cs.column_name());
        }
        auto b = std::make_unique<BoundCreateIndex>();
        b->table = ct::TableHandle{std::move(meta), table_ref_to_string(table)};
        b->index = cs.index_name();
        b->col_ordinal = static_cast<uint16_t>(ordinal);
        return b;
    }
    case StmtKind::DropIndex: {
        const auto& ds = static_cast<const DropIndexStmt&>(stmt);
        const ct::TableRef table = to_table_ref(ds.table_name(), current_schema);
        check_reserved_table(table);
        auto b = std::make_unique<BoundDropIndex>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        b->index = ds.index_name();
        return b;
    }
    case StmtKind::Insert: {
        const auto& is = static_cast<const InsertStmt&>(stmt);
        const ct::TableRef table = to_table_ref(is.table_name(), current_schema);
        check_reserved_table(table);
        st::TableMeta meta = db.table_meta(table);
        const InsertTargets targets = resolve_insert_targets(is.columns(), meta);
        auto b = std::make_unique<BoundInsert>();
        b->rows.reserve(is.rows().size());
        for (const auto& row : is.rows()) {
            b->rows.push_back(bind_insert_row(row, targets, meta));
        }
        b->table = ct::TableHandle{std::move(meta), table_ref_to_string(table)};
        return b;
    }
    case StmtKind::Delete: {
        const auto& ds = static_cast<const DeleteStmt&>(stmt);
        const ct::TableRef table = to_table_ref(ds.table_name(), current_schema);
        check_reserved_table(table);
        auto b = std::make_unique<BoundDelete>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        if (ds.where_expr() != nullptr) {
            b->where = bind_where(*ds.where_expr(), build_schema(b->table.meta));
        }
        return b;
    }
    case StmtKind::Update: {
        const auto& us = static_cast<const UpdateStmt&>(stmt);
        const ct::TableRef table = to_table_ref(us.table_name(), current_schema);
        check_reserved_table(table);
        auto b = std::make_unique<BoundUpdate>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        const Schema schema = build_schema(b->table.meta);
        b->assigns.reserve(us.assignments().size());
        for (const UpdateItem& item : us.assignments()) {
            const auto it = schema.cols.find(item.column);
            if (it == schema.cols.end()) {
                DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER, "列不存在: {}",
                         item.column);
            }
            for (const BoundUpdateItem& a : b->assigns) {
                if (a.col_idx == it->second) {
                    DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                             "更新列清单中列重复: {}", item.column);
                }
            }
            BoundUpdateItem a;
            a.col_idx = it->second;
            ExprType t;
            // 行上下文: 右值基于旧行求值, 赋值间互不可见
            a.value = bind_expr(*item.value, &schema, t);
            const st::ColumnSpec& col = b->table.meta.cols[it->second];
            if (has_col_ref(*item.value)) {
                check_update_type(t, col);
            } else {
                // 纯常量右值: 与插入同构的 NOT NULL/长度/范围检查
                if (col.not_null && may_be_null(*item.value)) {
                    DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                             "NOT NULL 列不允许 NULL: {}", col.name);
                }
                check_value_type(*item.value, t, col);
            }
            b->assigns.push_back(std::move(a));
        }
        if (us.where_expr() != nullptr) {
            b->where = bind_where(*us.where_expr(), schema);
        }
        return b;
    }
    case StmtKind::Select: {
        const auto& ss = static_cast<const SelectStmt&>(stmt);
        const ct::TableRef table = to_table_ref(ss.table_name(), current_schema);
        auto b = std::make_unique<BoundSelect>();
        b->table = ct::TableHandle{db.table_meta(table), table_ref_to_string(table)};
        const Schema schema = build_schema(b->table.meta);
        if (ss.where_expr() != nullptr) {
            b->where = bind_where(*ss.where_expr(), schema);
        }
        ProjBind pb = build_projs(ss, b->table.meta, schema);
        if (!ss.orders().empty()) {
            b->orders = bind_orders(ss.orders(), pb, schema, b->sort_on_output);
        }
        b->projs = std::move(pb.projs);
        return b;
    }
    case StmtKind::SelectNoFrom: {
        const auto& ss = static_cast<const SelectNoFromStmt&>(stmt);
        auto b = std::make_unique<BoundSelectNoFrom>();
        const Schema schema;  // 空行结构: 无列可解析, 列引用按列不存在报
        if (ss.where_expr() != nullptr) {
            b->where = bind_where(*ss.where_expr(), schema);
        }
        ProjBind pb = build_projs_no_from(ss, schema);
        if (!ss.orders().empty()) {
            b->orders = bind_orders(ss.orders(), pb, schema, b->sort_on_output);
        }
        b->projs = std::move(pb.projs);
        return b;
    }
    case StmtKind::Explain: {
        const auto& es = static_cast<const ExplainStmt&>(stmt);
        auto b = std::make_unique<BoundExplain>();
        b->inner = analyze(db, es.inner(), current_schema);
        return b;
    }
    case StmtKind::Set: {
        const auto& ss = static_cast<const SetStmt&>(stmt);
        if (!db.bootstrap_mode()) {
            DB_RAISE(db::ErrCode::NotImplemented, LogModule::ANALYZER, "SET 仅 bootstrap 模式可用");
        }
        auto b = std::make_unique<BoundSet>();
        if (ss.var_name() == "table_id") {
            b->var = SetVar::TableId;
        } else {
            DB_RAISE(db::ErrCode::UnknownVar, LogModule::ANALYZER, "未知 SET 变量: {}",
                     ss.var_name());
        }
        // 值文本转 uint64, 非十进制非负整数即报错(bootstrap.sql 为受控文件)
        uint64_t value = 0;
        const char* begin = ss.value().data();
        const char* end = begin + ss.value().size();
        const auto conv = std::from_chars(begin, end, value);
        if (conv.ec != std::errc{} || conv.ptr != end) {
            DB_RAISE(db::ErrCode::InvalidVarValue, LogModule::ANALYZER, "table_id 值非法: {}",
                     ss.value());
        }
        b->value = value;
        return b;
    }
    case StmtKind::Begin:
    case StmtKind::Commit:
    case StmtKind::Rollback:
        // 事务控制语句在会话层短路处理, 不进语义分析
        break;
    }
    // 不可达: 全部语句种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::ANALYZER, "analyzer: 未知语句种类");
}

}  // namespace ana
