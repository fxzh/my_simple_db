// analyzer_expr.cpp: 表达式绑定: AST 表达式 → 绑定表达式树, 含类型工具与行结构构建
#include "analyzer_expr.h"

#include <charconv>
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
#include "storage/cast.h"

namespace ana {

namespace {

// 数值类判定(值族属性)
bool is_num_type(st::TypeAttr t)
{
    return t.family == st::Family::Num;
}

// 比较两侧静态类型可比性校验: 同族才可比, 任一侧无类型放行
void check_comparable(st::TypeAttr lt, st::TypeAttr rt)
{
    if (lt.id != 0 && rt.id != 0 && lt.family != rt.family) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                 "比较运算两侧须同为数值、字符串或布尔");
    }
}

// 内建函数绑定注册表项: id 与 FuncId 取值及 db_func.func_id 一致,
// derive 校验实参表示并推导结果域(表示不符当场报错)
struct FuncSpec {
    uint64_t id;
    size_t arity;
    st::TypeAttr (*derive)(const std::vector<st::TypeAttr>& arg_types, std::string_view fname);
};

// abs 结果域推导: 实参须为数值, 结果域镜像实参域
st::TypeAttr derive_abs(const std::vector<st::TypeAttr>& arg_types, std::string_view fname)
{
    if (arg_types[0].family != st::Family::Num) {
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
std::unique_ptr<BoundExpr> bind_func_call(const FuncCallExpr& f, const RowDesc* row_desc,
                                          ct::Catalog& db, st::TypeAttr& type)
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
    std::vector<st::TypeAttr> arg_types;
    args.reserve(f.args.size());
    for (const std::unique_ptr<Expr>& a : f.args) {
        st::TypeAttr at;
        args.push_back(bind_expr(*a, row_desc, db, at));
        if (at.id == 0) {
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

st::TypeAttr map_type_spec(ct::Catalog& db, const std::string& type,
                           std::optional<long long> length, std::optional<long long> scale,
                           uint16_t& len, uint8_t& scl)
{
    const ct::TypeRow row = db.find_type(type);
    if (!row.len_allowed && length.has_value()) {
        DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "类型不接受长度: {}", row.name);
    }
    const long long n = length.value_or(row.len_default);
    if (length.has_value() && (n <= 0 || n > row.len_max)) {
        if (row.scale_allowed) {
            DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "{} 精度非法: {}",
                     row.name, n);
        }
        DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "长度非法: {}", n);
    }
    if (!row.scale_allowed && scale.has_value()) {
        DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "类型不接受标度: {}", row.name);
    }
    const long long s = scale.value_or(0);
    if (s < 0 || s > n) {
        DB_RAISE(db::ErrCode::InvalidType, LogModule::ANALYZER, "{} 标度非法: {}", row.name, s);
    }
    len = static_cast<uint16_t>(n);
    scl = static_cast<uint8_t>(s);
    return row.attr;
}

// ==================== 表达式绑定 ====================

std::unique_ptr<BoundExpr> bind_expr(const Expr& expr, const RowDesc* row_desc, ct::Catalog& db,
                                     st::TypeAttr& type)
{
    switch (expr.kind()) {
    case ExprKind::Int: {
        const std::string& s = static_cast<const IntExpr&>(expr).text;
        long long v = 0;
        const auto conv = std::from_chars(s.data(), s.data() + s.size(), v);
        if (conv.ec != std::errc{} || conv.ptr != s.data() + s.size()) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::ANALYZER, "整数字面量超出可表示范围");
        }
        // int32 范围内定型 int, 超界定型 bigint
        type = db.require_type(v >= INT32_MIN && v <= INT32_MAX ? "int" : "bigint");
        return std::make_unique<BoundConst>(st::int_val(type, v));
    }
    case ExprKind::Float: {
        // 带指数字面量解析为 double, 定型 double
        const std::string& s = static_cast<const FloatExpr&>(expr).text;
        double v = 0;
        const auto conv = std::from_chars(s.data(), s.data() + s.size(), v);
        if (conv.ec != std::errc{} || conv.ptr != s.data() + s.size()) {
            DB_RAISE(db::ErrCode::ArithError, LogModule::ANALYZER, "浮点字面量超出可表示范围");
        }
        type = db.require_type("double");
        return std::make_unique<BoundConst>(st::double_val(type, v));
    }
    case ExprKind::Decimal: {
        // 无指数小数字面量解析为定点十进制, 定型 numeric, 标度随字面量
        const st::DecimalVal d =
            st::parse_decimal_text(static_cast<const DecimalExpr&>(expr).text);
        type = db.require_type("numeric");
        return std::make_unique<BoundConst>(st::numeric_val(type, d.unscaled, d.scale));
    }
    case ExprKind::String:
        type = db.require_type("varchar");
        return std::make_unique<BoundConst>(
            st::str_val(type, static_cast<const StringExpr&>(expr).value));
    case ExprKind::Null:
        type = st::TypeAttr{};
        return std::make_unique<BoundConst>(st::null_val());
    case ExprKind::Bool:
        type = db.require_type("boolean");
        return std::make_unique<BoundConst>(
            st::bool_val(type, static_cast<const BoolExpr&>(expr).value));
    case ExprKind::Identifier: {
        const auto& id = static_cast<const IdentifierExpr&>(expr);
        if (row_desc == nullptr) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER,
                     "常量上下文不允许引用列: {}", id.name);
        }
        const auto it = row_desc->cols.find(id.name);
        if (it == row_desc->cols.end()) {
            DB_RAISE(db::ErrCode::UnknownColumn, LogModule::ANALYZER, "列不存在: {}", id.name);
        }
        type = row_desc->col_types[it->second];
        return std::make_unique<BoundColRef>(it->second, type);
    }
    case ExprKind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        st::TypeAttr lt;
        st::TypeAttr rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, row_desc, db, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, row_desc, db, rt);
        if ((!is_num_type(lt) && lt.id != 0) || (!is_num_type(rt) && rt.id != 0)) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "算术运算操作数不是数值");
        }
        // 结果域: 按秩取较高侧, numeric 与 float 组合抬到 double
        if ((lt.rep == st::Rep::Dec && rt.rep == st::Rep::Float)
            || (lt.rep == st::Rep::Float && rt.rep == st::Rep::Dec)) {
            type = db.require_type("double");
        } else {
            type = lt.rank >= rt.rank ? lt : rt;
        }
        return std::make_unique<BoundArith>(e.op, std::move(l), std::move(r), type);
    }
    case ExprKind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        st::TypeAttr t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, row_desc, db, t);
        if (!is_num_type(t) && t.id != 0) {
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
        st::TypeAttr lt;
        st::TypeAttr rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, row_desc, db, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, row_desc, db, rt);
        check_comparable(lt, rt);
        type = db.require_type("boolean");
        return std::make_unique<BoundCmp>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::Logic: {
        const auto& e = static_cast<const LogicExpr&>(expr);
        st::TypeAttr lt;
        st::TypeAttr rt;
        std::unique_ptr<BoundExpr> l = bind_expr(*e.left, row_desc, db, lt);
        std::unique_ptr<BoundExpr> r = bind_expr(*e.right, row_desc, db, rt);
        if ((lt.family != st::Family::Bool && lt.id != 0)
            || (rt.family != st::Family::Bool && rt.id != 0)) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "逻辑运算操作数不是布尔值");
        }
        type = db.require_type("boolean");
        return std::make_unique<BoundLogic>(e.op, std::move(l), std::move(r));
    }
    case ExprKind::Not: {
        const auto& e = static_cast<const NotExpr&>(expr);
        st::TypeAttr t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, row_desc, db, t);
        if (t.family != st::Family::Bool && t.id != 0) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "逻辑运算操作数不是布尔值");
        }
        type = db.require_type("boolean");
        return std::make_unique<BoundNot>(std::move(operand));
    }
    case ExprKind::IsNull: {
        const auto& e = static_cast<const IsNullExpr&>(expr);
        st::TypeAttr t;
        std::unique_ptr<BoundExpr> operand = bind_expr(*e.operand, row_desc, db, t);
        type = db.require_type("boolean");
        return std::make_unique<BoundIsNull>(std::move(operand), e.negate);
    }
    case ExprKind::Between: {
        const auto& e = static_cast<const BetweenExpr&>(expr);
        // 被测表达式绑定两份, 分别作两个比较的左操作数
        st::TypeAttr xt;
        st::TypeAttr lot;
        st::TypeAttr hit;
        std::unique_ptr<BoundExpr> x1 = bind_expr(*e.expr, row_desc, db, xt);
        std::unique_ptr<BoundExpr> lo = bind_expr(*e.lo, row_desc, db, lot);
        std::unique_ptr<BoundExpr> x2 = bind_expr(*e.expr, row_desc, db, xt);
        std::unique_ptr<BoundExpr> hi = bind_expr(*e.hi, row_desc, db, hit);
        check_comparable(xt, lot);
        check_comparable(xt, hit);
        type = db.require_type("boolean");
        auto ge = std::make_unique<BoundCmp>(CmpOp::Ge, std::move(x1), std::move(lo));
        auto le = std::make_unique<BoundCmp>(CmpOp::Le, std::move(x2), std::move(hi));
        return std::make_unique<BoundLogic>(LogicOp::And, std::move(ge), std::move(le));
    }
    case ExprKind::FuncCall:
        return bind_func_call(static_cast<const FuncCallExpr&>(expr), row_desc, db, type);
    case ExprKind::Cast: {
        const auto& c = static_cast<const CastExpr&>(expr);
        st::TypeAttr ot;
        std::unique_ptr<BoundExpr> operand = bind_expr(*c.operand, row_desc, db, ot);
        uint16_t len = 0;
        uint8_t scl = 0;
        type = map_type_spec(db, c.target.type, c.target.length, c.target.scale, len, scl);
        // 恒等转换且目标无参数语义(非文本与 numeric 域, 文本不限长, 裸 numeric)时折为操作数本身
        bool plain = type.family != st::Family::Text
                     || (type.store == st::Store::VarLen && len == 0);
        if (type.rep == st::Rep::Dec) {
            plain = len == 0;
        }
        if (ot == type && plain) {
            return operand;
        }
        return std::make_unique<BoundCast>(std::move(operand), type, len, scl);
    }
    }
    // 不可达: 全部表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "analyzer: 未知表达式节点");
}

std::unique_ptr<BoundExpr> bind_where(const Expr& where, const RowDesc& row_desc, ct::Catalog& db)
{
    st::TypeAttr t;
    std::unique_ptr<BoundExpr> pred = bind_expr(where, &row_desc, db, t);
    if (t.family != st::Family::Bool && t.id != 0) {
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
    case ExprKind::Decimal:
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
    case ExprKind::Decimal:
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

RowDesc build_row_desc(const st::TableMeta& meta)
{
    RowDesc row_desc;
    row_desc.col_types.reserve(meta.cols.size());
    for (size_t i = 0; i < meta.cols.size(); ++i) {
        row_desc.cols.emplace(meta.cols[i].name, i);
        row_desc.col_types.push_back(meta.cols[i].type);
    }
    return row_desc;
}

}  // namespace ana
