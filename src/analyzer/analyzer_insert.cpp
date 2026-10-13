// analyzer_insert.cpp: INSERT 绑定: 值与列类型校验, 列清单解析, 行值绑定
#include "analyzer_insert.h"

#include <charconv>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "analyzer_expr.h"
#include "ast.hh"
#include "bound_expr.h"
#include "catalog.h"
#include "common/err.h"
#include "log/log.h"

namespace ana {

// ==================== 值与列类型校验 ====================

void check_value_type(const Expr& expr, st::ColType t, const st::ColumnSpec& col)
{
    if (may_be_null(expr)) {
        return;  // 恒 NULL 常量树免检(NOT NULL 已在前置检查拦截)
    }
    bool ok = false;
    switch (col.type) {
    case st::ColType::Int:
        // Int 列仅收 Int, 超界字面量已定型 BigInt 在此拒绝
        ok = t == st::ColType::Int;
        break;
    case st::ColType::BigInt:
        ok = is_int_type(t);
        break;
    case st::ColType::Double:
    case st::ColType::Float: {
        ok = t == st::ColType::Double;
        // float 列字面量须可被 float 表示
        if (ok && col.type == st::ColType::Float
            && (expr.kind() == ExprKind::Float || expr.kind() == ExprKind::Decimal)) {
            const std::string& s = expr.kind() == ExprKind::Float
                                       ? static_cast<const FloatExpr&>(expr).text
                                       : static_cast<const DecimalExpr&>(expr).text;
            double d = 0;
            const auto conv = std::from_chars(s.data(), s.data() + s.size(), d);
            if (conv.ec != std::errc{} || conv.ptr != s.data() + s.size()) {
                DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "字面量文本解析失败");
            }
            ok = !std::isfinite(d) || std::isfinite(static_cast<float>(d));
        }
        break;
    }
    case st::ColType::Char:
    case st::ColType::VarChar:
        ok = t == st::ColType::VarChar;
        // 字面量长度当场校验, 非字面量文本常量(如 CAST 产物)长度由存储编码检查
        if (ok && expr.kind() == ExprKind::String) {
            const std::string& s = static_cast<const StringExpr&>(expr).value;
            ok = col.length == 0 || s.size() <= col.length;
        }
        break;
    case st::ColType::Bool:
        ok = t == st::ColType::Bool;
        break;
    case st::ColType::Numeric:
        ok = false;
        break;
    case st::ColType::Null:
        DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "列类型域不含 Null");
    }
    if (!ok) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "值与列类型不匹配");
    }
}

void check_update_type(st::ColType t, const st::ColumnSpec& col)
{
    bool ok = false;
    switch (col.type) {
    case st::ColType::Int:
    case st::ColType::BigInt:
        ok = is_int_type(t) || t == st::ColType::Null;
        break;
    case st::ColType::Float:
    case st::ColType::Double:
        ok = is_float_type(t) || t == st::ColType::Null;
        break;
    case st::ColType::Char:
    case st::ColType::VarChar:
        ok = is_text_type(t) || t == st::ColType::Null;
        break;
    case st::ColType::Bool:
        ok = t == st::ColType::Bool || t == st::ColType::Null;
        break;
    case st::ColType::Numeric:
        ok = false;
        break;
    case st::ColType::Null:
        DB_RAISE(db::ErrCode::Internal, LogModule::ANALYZER, "列类型域不含 Null");
    }
    if (!ok) {
        DB_RAISE(db::ErrCode::ValueMismatch, LogModule::ANALYZER, "值与列类型不匹配");
    }
}

// ==================== INSERT 绑定 ====================

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

std::vector<std::unique_ptr<BoundExpr>> bind_insert_row(
    const std::vector<std::unique_ptr<Expr>>& values, const InsertTargets& t,
    const st::TableMeta& meta, ct::Catalog& db)
{
    std::vector<std::unique_ptr<BoundExpr>> bound;
    std::vector<st::ColType> types;
    types.reserve(values.size());
    for (const auto& v : values) {
        st::ColType type;
        bound.push_back(bind_expr(*v, nullptr, db, type));  // 常量上下文: 禁止引用列
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
            row[i] = std::make_unique<BoundConst>(st::null_val());
        }
    }
    return row;
}

}  // namespace ana
