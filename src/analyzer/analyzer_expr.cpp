// analyzer_expr.cpp: 表达式绑定: AST 表达式 → 绑定表达式树, 含类型工具与行结构构建
#include "analyzer_expr.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ast.hh"
#include "bound_expr.h"
#include "catalog.h"
#include "common/err.h"
#include "log/log.h"

namespace ana {

namespace {

// 数值类域等级: Int<BigInt<Float<Double, 非数值为 0(Null 单独判)
int num_rank(st::ColType t)
{
    switch (t) {
    case st::ColType::Int: return 1;
    case st::ColType::BigInt: return 2;
    case st::ColType::Float: return 3;
    case st::ColType::Double: return 4;
    default: return 0;
    }
}

// 数值类(Int/BigInt/Float/Double)判定
bool is_num_type(st::ColType t)
{
    return num_rank(t) > 0;
}

// 比较两侧静态类型可比性校验: 同族才可比, 任一侧无类型(Null)放行
void check_comparable(st::ColType lt, st::ColType rt)
{
    const bool same_num = is_num_type(lt) && is_num_type(rt);
    const bool same_str = is_text_type(lt) && is_text_type(rt);
    const bool same_bool = lt == st::ColType::Bool && rt == st::ColType::Bool;
    if (lt != st::ColType::Null && rt != st::ColType::Null && !same_num && !same_str
        && !same_bool) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                 "比较运算两侧须同为数值、字符串或布尔");
    }
}

// 内建函数绑定注册表项: id 与 FuncId 取值及 db_func.func_id 一致,
// derive 校验实参形态并推导结果域(形态不符当场报错)
struct FuncSpec {
    uint64_t id;
    size_t arity;
    st::ColType (*derive)(const std::vector<st::ColType>& arg_types, std::string_view fname);
};

// abs 结果域推导: 实参须为数值, 结果域镜像实参域
st::ColType derive_abs(const std::vector<st::ColType>& arg_types, std::string_view fname)
{
    if (!is_num_type(arg_types[0])) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                 "函数实参类型不符: {} 须为数值", fname);
    }
    return arg_types[0];
}

// 内建函数绑定注册表, 增改函数须同步 FuncId 枚举与 bootstrap.sql
constexpr FuncSpec kFuncSpecs[] = {
    {1, 1, derive_abs},  // abs
};

// 按 db_func 函数 id 查绑定注册表, 未命中返回空
const FuncSpec* find_func_spec(uint64_t id)
{
    for (const FuncSpec& s : kFuncSpecs) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

// 函数调用绑定: db_func 按名解析为内建函数, 实参递归绑定后按注册表校验个数并推导结果域
std::unique_ptr<BoundExpr> bind_func_call(const FuncCallExpr& f, const Schema* schema,
                                          ct::Catalog& db, st::ColType& type)
{
    const std::optional<uint64_t> fid = db.func_id(f.name);
    if (!fid.has_value()) {
        DB_RAISE(db::ErrCode::UnknownFunction, LogModule::ANALYZER, "函数不存在: {}", f.name);
    }
    // db_func 数据行与内建函数清单一致, id 未注册即内部错误
    const FuncSpec* spec = find_func_spec(*fid);
    if (spec == nullptr) {
        DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "db_func 函数 id 未注册: {}", *fid);
    }
    if (f.args.size() != spec->arity) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                 "函数参数个数不符: {} 须 {} 个实参", f.name, spec->arity);
    }
    std::vector<std::unique_ptr<BoundExpr>> args;
    std::vector<st::ColType> arg_types;
    args.reserve(f.args.size());
    for (const std::unique_ptr<Expr>& a : f.args) {
        st::ColType at;
        args.push_back(bind_expr(*a, schema, db, at));
        if (at == st::ColType::Null) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "函数实参类型未知, 无法推导结果域: {}", f.name);
        }
        arg_types.push_back(at);
    }
    type = spec->derive(arg_types, f.name);
    return std::make_unique<BoundFuncCall>(static_cast<FuncId>(spec->id), std::move(args), type);
}

}  // namespace

// ==================== 类型工具 ====================

bool is_int_type(st::ColType t)
{
    return t == st::ColType::Int || t == st::ColType::BigInt;
}

bool is_float_type(st::ColType t)
{
    return t == st::ColType::Float || t == st::ColType::Double;
}

bool is_text_type(st::ColType t)
{
    return t == st::ColType::Char || t == st::ColType::VarChar;
}

st::ColType map_type_spec(DataType type, std::optional<long long> length, uint16_t& len)
{
    st::ColType t = st::ColType::Null;
    switch (type) {
    case DataType::Int: t = st::ColType::Int; break;
    case DataType::BigInt: t = st::ColType::BigInt; break;
    case DataType::Float: t = st::ColType::Float; break;
    case DataType::Double: t = st::ColType::Double; break;
    case DataType::Char: t = st::ColType::Char; break;
    case DataType::VarChar: t = st::ColType::VarChar; break;
    case DataType::Bool: t = st::ColType::Bool; break;
    }
    const long long n = length.value_or(type == DataType::Char ? 1 : 0);
    if (length.has_value() && (n <= 0 || n > UINT16_MAX)) {
        DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "长度非法: {}", n);
    }
    len = static_cast<uint16_t>(n);
    return t;
}

// ==================== 表达式绑定 ====================

std::unique_ptr<BoundExpr> bind_expr(const Expr& expr, const Schema* schema, ct::Catalog& db,
                                     st::ColType& type)
{
    switch (expr.kind()) {
    case ExprKind::Int: {
        const int64_t v = static_cast<const IntExpr&>(expr).value;
        // int32 范围内定型 Int, 超界定型 BigInt
        if (v >= INT32_MIN && v <= INT32_MAX) {
            type = st::ColType::Int;
            return std::make_unique<BoundConst>(st::int_val(v));
        }
        type = st::ColType::BigInt;
        return std::make_unique<BoundConst>(st::bigint_val(v));
    }
    case ExprKind::Float:
        // 词法浮点值即 double, 字面量定型 Double
        type = st::ColType::Double;
        return std::make_unique<BoundConst>(
            st::double_val(static_cast<const FloatExpr&>(expr).value));
    case ExprKind::String:
        type = st::ColType::VarChar;
        return std::make_unique<BoundConst>(
            st::str_val(static_cast<const StringExpr&>(expr).value));
    case ExprKind::Null:
        type = st::ColType::Null;
        return std::make_unique<BoundConst>(st::null_val());
    case ExprKind::Bool:
        type = st::ColType::Bool;
        return std::make_unique<BoundConst>(st::bool_val(static_cast<const BoolExpr&>(expr).value));
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
        type = schema->col_types[it->second];
        return std::make_unique<BoundColRef>(it->second, type);
    }
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        st::ColType lt;
        st::ColType rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, schema, db, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, schema, db, rt);
        if ((!is_num_type(lt) && lt != st::ColType::Null)
            || (!is_num_type(rt) && rt != st::ColType::Null)) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "算术运算操作数不是数值");
        }
        // 结果取两侧较高数值域
        type = num_rank(lt) >= num_rank(rt) ? lt : rt;
        return std::make_unique<BoundArith>(e.op, std::move(l), std::move(r), type);
    }
    case ExprKind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        st::ColType t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, schema, db, t);
        if (!is_num_type(t) && t != st::ColType::Null) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "一元运算操作数不是数值");
        }
        type = t;
        if (e.op == '+') {
            return operand;  // 一元正号原值返回, 绑定期折为操作数本身
        }
        return std::make_unique<BoundNeg>(std::move(operand), t);
    }
    case ExprKind::Compare: {
        const auto& e = static_cast<const CompareExpr&>(expr);
        st::ColType lt;
        st::ColType rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, schema, db, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, schema, db, rt);
        check_comparable(lt, rt);
        type = st::ColType::Bool;
        return std::make_unique<BoundCmp>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        st::ColType lt;
        st::ColType rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, schema, db, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, schema, db, rt);
        if ((lt != st::ColType::Bool && lt != st::ColType::Null)
            || (rt != st::ColType::Bool && rt != st::ColType::Null)) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "逻辑运算操作数不是布尔值");
        }
        type = st::ColType::Bool;
        return std::make_unique<BoundLogic>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::Not: {
        const auto& e = static_cast<const NotExpr&>(expr);
        st::ColType t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, schema, db, t);
        if (t != st::ColType::Bool && t != st::ColType::Null) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "逻辑运算操作数不是布尔值");
        }
        type = st::ColType::Bool;
        return std::make_unique<BoundNot>(std::move(operand));
    }
    case ExprKind::IsNull: {
        const auto& e = static_cast<const IsNullExpr&>(expr);
        st::ColType t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, schema, db, t);
        type = st::ColType::Bool;
        return std::make_unique<BoundIsNull>(std::move(operand), e.negate);
    }
    case ExprKind::Between: {
        const auto& e = static_cast<const BetweenExpr&>(expr);
        // 被测式绑定两份, 分别作两个比较的左操作数
        st::ColType xt;
        st::ColType lot;
        st::ColType hit;
        std::unique_ptr<BoundExpr> x1 = bind_expr(*e.expr, schema, db, xt);
        std::unique_ptr<BoundExpr> lo = bind_expr(*e.lo, schema, db, lot);
        std::unique_ptr<BoundExpr> x2 = bind_expr(*e.expr, schema, db, xt);
        std::unique_ptr<BoundExpr> hi = bind_expr(*e.hi, schema, db, hit);
        check_comparable(xt, lot);
        check_comparable(xt, hit);
        type = st::ColType::Bool;
        auto ge = std::make_unique<BoundCmp>(CmpOp::Ge, std::move(x1), std::move(lo));
        auto le = std::make_unique<BoundCmp>(CmpOp::Le, std::move(x2), std::move(hi));
        return std::make_unique<BoundLogic>(LogicOp::And, std::move(ge), std::move(le));
    }
    case ExprKind::FuncCall:
        return bind_func_call(static_cast<const FuncCallExpr&>(expr), schema, db, type);
    case ExprKind::Cast: {
        const auto& c = static_cast<const CastExpr&>(expr);
        st::ColType ot;
        std::unique_ptr<BoundExpr> operand = bind_expr(*c.operand, schema, db, ot);
        uint16_t len = 0;
        type = map_type_spec(c.target.type, c.target.length, len);
        // 恒等转换且无长度语义(非文本或文本不限长)时折为操作数本身
        if (ot == type && (!is_text_type(type) || (type == st::ColType::VarChar && len == 0))) {
            return operand;
        }
        return std::make_unique<BoundCast>(std::move(operand), type, len);
    }
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

std::unique_ptr<BoundExpr> bind_where(const Expr& where, const Schema& schema, ct::Catalog& db)
{
    st::ColType t;
    std::unique_ptr<BoundExpr> pred = bind_expr(where, &schema, db, t);
    if (t != st::ColType::Bool && t != st::ColType::Null) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "WHERE 条件不是布尔表达式");
    }
    return pred;
}

// ==================== AST 走查 ====================

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
    case ExprKind::Between: {
        const auto& e = static_cast<const BetweenExpr&>(expr);
        return may_be_null(*e.expr) || may_be_null(*e.lo) || may_be_null(*e.hi);
    }
    case ExprKind::FuncCall: {
        const auto& f = static_cast<const FuncCallExpr&>(expr);
        for (const auto& arg : f.args) {
            if (may_be_null(*arg)) {
                return true;
            }
        }
        return false;
    }
    case ExprKind::Cast:
        return may_be_null(*static_cast<const CastExpr&>(expr).operand);
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

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
    case ExprKind::Between: {
        const auto& e = static_cast<const BetweenExpr&>(expr);
        return has_col_ref(*e.expr) || has_col_ref(*e.lo) || has_col_ref(*e.hi);
    }
    case ExprKind::FuncCall: {
        const auto& f = static_cast<const FuncCallExpr&>(expr);
        for (const auto& arg : f.args) {
            if (has_col_ref(*arg)) {
                return true;
            }
        }
        return false;
    }
    case ExprKind::Cast:
        return has_col_ref(*static_cast<const CastExpr&>(expr).operand);
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

// ==================== 行结构 ====================

Schema build_schema(const st::TableMeta& meta)
{
    Schema schema;
    schema.col_types.reserve(meta.cols.size());
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        schema.cols.emplace(meta.cols[i].name, i);
        schema.col_types.push_back(meta.cols[i].type);
    }
    return schema;
}

}  // namespace ana
