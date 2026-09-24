// executor.cpp: 执行层实现: AST 转 catalog 调用
#include "executor.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ast.hh"
#include "codec.h"
#include "common/err.h"
#include "log/log.h"
#include "catalog.h"

namespace exec {

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
    }
    DB_RAISE(db::ErrCode::InvalidType, LogModule::EXECUTOR, "未映射的列类型: {}", static_cast<int>(type));
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
                DB_RAISE(db::ErrCode::InvalidType, LogModule::EXECUTOR, "长度非法: {}", *def.length);
            }
            len = static_cast<uint16_t>(*def.length);
        } else if (def.type == DataType::Char) {
            len = 1;
        }
        cols.emplace_back(st::ColumnSpec{def.name, map_col_type(def.type), len});
    }
}

// ==================== 表达式求值 ====================

// 求值字符串值: 记录是否来自 char 定长列, 该侧比较按 PAD SPACE 语义处理
struct StrVal {
    std::string text;
    bool from_char = false;
};

// 求值值域: 在存储值上扩展 bool; monostate 表示 NULL(条件上下文即 UNKNOWN)
using EvalValue = std::variant<std::monostate, bool, int64_t, double, StrVal>;

// 行上下文: 列名定位表 + char 定长列标记, 每条语句编译一次
using ColMap = std::unordered_map<std::string, size_t>;
struct RowCtx {
    ColMap cols;                 // 列名 → 行内下标
    std::vector<bool> char_col;  // char 定长列标记, 与行内下标对应
};

EvalValue eval_expr(const Expr& expr, const RowCtx* ctx, const st::Row* row);

// 数值转 double(整型提升)
double to_double(const EvalValue& v)
{
    if (const auto* d = std::get_if<double>(&v)) {
        return *d;
    }
    return static_cast<double>(std::get<int64_t>(v));
}

// 值是否数值(int64/double)
bool is_number(const EvalValue& v)
{
    return std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v);
}

// 值是否 NULL(monostate)
bool is_null(const EvalValue& v)
{
    return std::holds_alternative<std::monostate>(v);
}

// 去尾随空格(比较 PAD SPACE 语义用)
std::string_view rtrim_space(std::string_view s)
{
    while (!s.empty() && s.back() == ' ') {
        s.remove_suffix(1);
    }
    return s;
}

// 一元数值运算: NULL 传播, '+' 原值返回, '-' 取反
EvalValue eval_unary(char op, const Expr& operand, const RowCtx* ctx, const st::Row* row)
{
    const EvalValue v = eval_expr(operand, ctx, row);
    if (is_null(v)) {
        return v;  // NULL 传播
    }
    if (!is_number(v)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "一元运算操作数不是数值");
    }
    if (op == '+') {
        return v;
    }
    if (const auto* d = std::get_if<double>(&v)) {
        return EvalValue{-*d};  // 有限值取反仍有限
    }
    const int64_t i = std::get<int64_t>(v);
    if (i == INT64_MIN) {
        DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型取反溢出");
    }
    return EvalValue{-i};
}

// 双目算术: 任一操作数为 NULL 结果 NULL, 非 NULL 操作数须数值; 类型驱动提升(int/int 向零截断), 溢出/除零当场报错
EvalValue eval_binary(char op, const Expr& le, const Expr& re, const RowCtx* ctx, const st::Row* row)
{
    const EvalValue lv = eval_expr(le, ctx, row);
    const EvalValue rv = eval_expr(re, ctx, row);
    if ((!is_null(lv) && !is_number(lv)) || (!is_null(rv) && !is_number(rv))) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "算术运算操作数不是数值");
    }
    if (is_null(lv) || is_null(rv)) {
        return EvalValue{};  // NULL 传播, 短路于除零/溢出检查
    }
    if (std::holds_alternative<double>(lv) || std::holds_alternative<double>(rv)) {
        double out = 0.0;
        switch (op) {
        case '+': out = to_double(lv) + to_double(rv); break;
        case '-': out = to_double(lv) - to_double(rv); break;
        case '*': out = to_double(lv) * to_double(rv); break;
        case '/': out = to_double(lv) / to_double(rv); break;
        }
        if (!std::isfinite(out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "浮点运算溢出或除零");
        }
        return EvalValue{out};
    }
    const int64_t l = std::get<int64_t>(lv);
    const int64_t r = std::get<int64_t>(rv);
    int64_t out = 0;
    switch (op) {
    case '+':
        if (__builtin_add_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型加法溢出");
        }
        break;
    case '-':
        if (__builtin_sub_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型减法溢出");
        }
        break;
    case '*':
        if (__builtin_mul_overflow(l, r, &out)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型乘法溢出");
        }
        break;
    case '/':
        if (r == 0) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型除零");
        }
        if (l == INT64_MIN && r == -1) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "整型除法溢出");
        }
        out = l / r;  // 向零截断
        break;
    }
    return EvalValue{out};
}

// 字符串比较: 来自 char 定长列的一侧去尾随空格(PAD SPACE 语义)
int cmp_str(const StrVal& l, const StrVal& r)
{
    const std::string_view lv = l.from_char ? rtrim_space(l.text) : l.text;
    const std::string_view rv = r.from_char ? rtrim_space(r.text) : r.text;
    return lv.compare(rv);
}

// 比较: 任一侧 NULL 即 NULL; 数值提升为 double 比较, 字符串按 PAD SPACE 语义, 跨类当场报错
EvalValue eval_compare(CmpOp op, const Expr& le, const Expr& re, const RowCtx* ctx,
                       const st::Row* row)
{
    const EvalValue lv = eval_expr(le, ctx, row);
    const EvalValue rv = eval_expr(re, ctx, row);
    if (is_null(lv) || is_null(rv)) {
        return EvalValue{};  // NULL 传播, 求值结果即 UNKNOWN
    }
    const StrVal* ls = std::get_if<StrVal>(&lv);
    const StrVal* rs = std::get_if<StrVal>(&rv);
    int c = 0;
    if (ls != nullptr || rs != nullptr) {
        if (ls == nullptr || rs == nullptr) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "比较运算两侧须同为数值或字符串");
        }
        c = cmp_str(*ls, *rs);
    } else if (is_number(lv) && is_number(rv)) {
        const double l = to_double(lv);
        const double r = to_double(rv);
        c = l < r ? -1 : (l > r ? 1 : 0);
    } else {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "比较运算两侧须同为数值或字符串");
    }
    switch (op) {
    case CmpOp::Eq: return EvalValue{c == 0};
    case CmpOp::Ne: return EvalValue{c != 0};
    case CmpOp::Lt: return EvalValue{c < 0};
    case CmpOp::Le: return EvalValue{c <= 0};
    case CmpOp::Gt: return EvalValue{c > 0};
    case CmpOp::Ge: return EvalValue{c >= 0};
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 未知比较种类");
}

// AND/OR: 三值逻辑, AND 有 false 即 false / OR 有 true 即 true, 其余 NULL 传播; 非 NULL 操作数须为 bool
EvalValue eval_logic(LogicOp op, const Expr& le, const Expr& re, const RowCtx* ctx,
                     const st::Row* row)
{
    const EvalValue lv = eval_expr(le, ctx, row);
    const EvalValue rv = eval_expr(re, ctx, row);
    const bool* lb = std::get_if<bool>(&lv);
    const bool* rb = std::get_if<bool>(&rv);
    if ((lb == nullptr && !is_null(lv)) || (rb == nullptr && !is_null(rv))) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "逻辑运算操作数不是布尔值");
    }
    if (op == LogicOp::And) {
        if ((lb != nullptr && !*lb) || (rb != nullptr && !*rb)) {
            return EvalValue{false};  // 有 false 即 false
        }
        if (lb == nullptr || rb == nullptr) {
            return EvalValue{};  // 有 UNKNOWN 即 UNKNOWN
        }
        return EvalValue{true};
    }
    if ((lb != nullptr && *lb) || (rb != nullptr && *rb)) {
        return EvalValue{true};  // 有 true 即 true
    }
    if (lb == nullptr || rb == nullptr) {
        return EvalValue{};  // 有 UNKNOWN 即 UNKNOWN
    }
    return EvalValue{false};
}

// NOT: 三值逻辑, NULL 传播, 非 NULL 操作数须为 bool
EvalValue eval_not(const Expr& operand, const RowCtx* ctx, const st::Row* row)
{
    const EvalValue v = eval_expr(operand, ctx, row);
    if (is_null(v)) {
        return v;
    }
    const bool* b = std::get_if<bool>(&v);
    if (b == nullptr) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "逻辑运算操作数不是布尔值");
    }
    return EvalValue{!*b};
}

// IS [NOT] NULL: 对任意类型操作数判空
EvalValue eval_is_null(const IsNullExpr& e, const RowCtx* ctx, const st::Row* row)
{
    const EvalValue v = eval_expr(*e.operand, ctx, row);
    return EvalValue{e.negate ? !is_null(v) : is_null(v)};
}

// 统一求值入口: ctx/row 同时为空表示常量上下文(标识符不可用)
EvalValue eval_expr(const Expr& expr, const RowCtx* ctx, const st::Row* row)
{
    switch (expr.kind()) {
    case ExprKind::Int:
        return EvalValue{static_cast<const IntExpr&>(expr).value};
    case ExprKind::Float: {
        const double d = static_cast<const FloatExpr&>(expr).value;
        if (!std::isfinite(d)) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::EXECUTOR, "浮点字面量超出可表示范围");
        }
        return EvalValue{d};
    }
    case ExprKind::String:
        return EvalValue{StrVal{static_cast<const StringExpr&>(expr).value, false}};
    case ExprKind::Null:
        return EvalValue{};
    case ExprKind::Identifier: {
        const auto& id = static_cast<const IdentifierExpr&>(expr);
        if (ctx == nullptr) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "常量上下文不允许引用列: {}",
                     id.name);
        }
        const auto it = ctx->cols.find(id.name);
        if (it == ctx->cols.end()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::EXECUTOR, "列不存在: {}", id.name);
        }
        const st::Value& raw = row->values[it->second];
        const bool from_char = ctx->char_col[it->second];
        return std::visit(
            [from_char](const auto& val) -> EvalValue {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return EvalValue{StrVal{val, from_char}};
                } else {
                    return val;
                }
            },
            raw);
    }
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        return eval_binary(e.op, *e.left, *e.right, ctx, row);
    }
    case ExprKind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        return eval_unary(e.op, *e.operand, ctx, row);
    }
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        return eval_compare(e.op, *e.left, *e.right, ctx, row);
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        return eval_logic(e.op, *e.left, *e.right, ctx, row);
    }
    case ExprKind::Not: {
        const auto& e = static_cast<const NotExpr&>(expr);
        return eval_not(*e.operand, ctx, row);
    }
    case ExprKind::IsNull:
        return eval_is_null(static_cast<const IsNullExpr&>(expr), ctx, row);
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 未知表达式节点");
}

// 常量上下文求值(INSERT VALUES 等无行上下文的场景)
EvalValue eval_const(const Expr& e)
{
    return eval_expr(e, nullptr, nullptr);
}

// 行上下文求值(SELECT 投影与 WHERE 过滤)
EvalValue eval_row(const Expr& e, const RowCtx& ctx, const st::Row& row)
{
    return eval_expr(e, &ctx, &row);
}

// 求值结果转存储/输出值: bool 不允许作为结果值, 其余原样(monostate 即 NULL)
st::Value to_st_value(const EvalValue& v)
{
    if (std::holds_alternative<bool>(v)) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "布尔值不可作为存储或输出值");
    }
    return std::visit(
        [](const auto& val) -> st::Value {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, StrVal>) {
                return val.text;
            } else {
                return val;
            }
        },
        v);
}

// 编译期校验表达式引用的列均存在: 空表时也能报出未知列
void validate_columns(const Expr& expr, const ColMap& cols)
{
    switch (expr.kind()) {
    case ExprKind::Int:
    case ExprKind::Float:
    case ExprKind::String:
    case ExprKind::Null:
        return;
    case ExprKind::Identifier: {
        const auto& id = static_cast<const IdentifierExpr&>(expr);
        if (cols.find(id.name) == cols.end()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::EXECUTOR, "列不存在: {}", id.name);
        }
        return;
    }
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        validate_columns(*e.left, cols);
        validate_columns(*e.right, cols);
        return;
    }
    case ExprKind::UnaryOp:
        validate_columns(*static_cast<const UnaryOpExpr&>(expr).operand, cols);
        return;
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        validate_columns(*e.left, cols);
        validate_columns(*e.right, cols);
        return;
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        validate_columns(*e.left, cols);
        validate_columns(*e.right, cols);
        return;
    }
    case ExprKind::Not:
        validate_columns(*static_cast<const NotExpr&>(expr).operand, cols);
        return;
    case ExprKind::IsNull:
        validate_columns(*static_cast<const IsNullExpr&>(expr).operand, cols);
        return;
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "executor: 未知表达式节点");
}

// WHERE 条件判定: 结果须为 bool, NULL(UNKNOWN) 视为不满足
bool where_match(const Expr& where, const RowCtx& ctx, const st::Row& row)
{
    const EvalValue v = eval_expr(where, &ctx, &row);
    if (is_null(v)) {
        return false;
    }
    const bool* b = std::get_if<bool>(&v);
    if (b == nullptr) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::EXECUTOR, "WHERE 条件不是布尔表达式");
    }
    return *b;
}

// 命令标签结果(非结果集语句)
ExecResult tag_result(proto::CommandTag tag, uint64_t count)
{
    ExecResult r;
    r.tag = tag;
    r.count = count;
    return r;
}

// 保留表名拦截: 元数据表禁止 drop/insert/delete
// (select 可查元数据, create 由存储层按表已存在拒绝)
void check_reserved_table(const std::string& name)
{
    if (name == ct::kTableMetaName || name == ct::kColumnMetaName) {
        DB_RAISE(db::ErrCode::ProtectedTable, LogModule::EXECUTOR, "保留表名禁止使用: {}", name);
    }
}

// ==================== SELECT 执行 ====================

// 投影输出列: star 展开的原始列直接取行值, 其余按表达式逐行求值
struct ProjCol {
    const Expr* expr = nullptr;  // 为空表示 star 展开的原始列
    std::string name;            // 输出列名
    size_t col_idx = 0;          // star 列的行内下标
};

// 编译行上下文: 列名定位表 + char 定长列标记
RowCtx build_row_ctx(const st::TableMeta& meta)
{
    RowCtx ctx;
    ctx.char_col.reserve(meta.cols.size());
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        ctx.cols.emplace(meta.cols[i].name, i);
        ctx.char_col.push_back(meta.cols[i].type == st::ColType::Char);
    }
    return ctx;
}

// SELECT 执行: 编译投影(列定位/stars 展开/输出列名)与 WHERE 列校验后全表扫描逐行物化
ExecResult exec_select(ct::Catalog& db, const SelectStmt& ss)
{
    const st::TableMeta meta = db.table_meta(ss.table_name());
    const RowCtx ctx = build_row_ctx(meta);
    if (ss.where_expr() != nullptr) {
        validate_columns(*ss.where_expr(), ctx.cols);
    }

    // 编译期: 投影展开
    std::vector<ProjCol> projs;
    for (const SelectItem& item : ss.items()) {
        if (item.star) {
            for (size_t i = 0; i < meta.cols.size(); ++i) {
                projs.push_back(ProjCol{nullptr, meta.cols[i].name, i});
            }
            continue;
        }
        std::string name;
        if (!item.alias.empty()) {
            name = item.alias;
        } else if (item.expr->kind() == ExprKind::Identifier) {
            name = static_cast<const IdentifierExpr&>(*item.expr).name;
        } else {
            name = expr_to_string(*item.expr);
        }
        projs.push_back(ProjCol{item.expr.get(), std::move(name), 0});
    }

    ExecResult result;
    result.is_result_set = true;
    result.col_names.reserve(projs.size());
    for (const ProjCol& p : projs) {
        result.col_names.push_back(p.name);
    }

    // 物化期: 扫描一行求值一行, WHERE 不满足即跳过
    const Expr* where = ss.where_expr();
    std::unique_ptr<st::Scanner> scanner = db.scan(ss.table_name());
    st::Row row;
    while (scanner->next(&row)) {
        if (where != nullptr && !where_match(*where, ctx, row)) {
            continue;
        }
        std::vector<st::Value> out;
        out.reserve(projs.size());
        for (const ProjCol& p : projs) {
            out.push_back(p.expr != nullptr ? to_st_value(eval_row(*p.expr, ctx, row))
                                            : row.values[p.col_idx]);
        }
        result.rows.push_back(std::move(out));
    }
    return result;
}

// DELETE ... WHERE: 列校验后扫描收集满足条件的行引用, 再逐个物理删除, 返回实际删除行数
uint64_t exec_delete_where(ct::Catalog& db, const Expr& where, const std::string& table)
{
    const st::TableMeta meta = db.table_meta(table);
    const RowCtx ctx = build_row_ctx(meta);
    validate_columns(where, ctx.cols);
    std::vector<st::RowRef> refs;
    std::unique_ptr<st::Scanner> scanner = db.scan(table);
    st::Row row;
    while (scanner->next(&row)) {
        if (where_match(where, ctx, row)) {
            refs.push_back(row.ref);
        }
    }
    uint64_t deleted = 0;
    for (const st::RowRef& ref : refs) {
        deleted += db.delete_by_ref(ref);
    }
    return deleted;
}

}  // namespace

ExecResult execute(ct::Catalog& db, const SQLStatement& stmt)
{
    switch (stmt.kind()) {
    case StmtKind::CreateTable: {
        const auto& cs = static_cast<const CreateTableStmt&>(stmt);
        std::vector<st::ColumnSpec> cols;
        convert_columns(cs.column_defs(), cols);
        db.create_table(cs.table_name(), cols);
        return tag_result(proto::CommandTag::CreateTable, 0);
    }
    case StmtKind::DropTable: {
        const auto& ds = static_cast<const DropTableStmt&>(stmt);
        check_reserved_table(ds.table_name());
        db.drop_table(ds.table_name());
        return tag_result(proto::CommandTag::DropTable, 0);
    }
    case StmtKind::Insert: {
        const auto& is = static_cast<const InsertStmt&>(stmt);
        check_reserved_table(is.table_name());
        std::vector<st::Value> values;
        values.reserve(is.values().size());
        for (const auto& v : is.values()) {
            values.push_back(to_st_value(eval_const(*v)));
        }
        db.insert(is.table_name(), values);
        return tag_result(proto::CommandTag::Insert, 1);
    }
    case StmtKind::Delete: {
        const auto& ds = static_cast<const DeleteStmt&>(stmt);
        check_reserved_table(ds.table_name());
        const uint64_t n = ds.where_expr() != nullptr
                ? exec_delete_where(db, *ds.where_expr(), ds.table_name())
                : db.delete_all(ds.table_name());
        return tag_result(proto::CommandTag::Delete, n);
    }
    case StmtKind::Select: {
        const auto& ss = static_cast<const SelectStmt&>(stmt);
        return exec_select(db, ss);
    }
    }
    // 不可达: 全部语句种类已在上方穷尽
    DB_RAISE(db::ErrCode::UnknownStmt, LogModule::EXECUTOR, "executor: 未知语句种类");
}

}  // namespace exec