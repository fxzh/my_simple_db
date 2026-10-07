// explain.cpp: EXPLAIN 渲染实现: 计划树 → PG 风格文本行(节点行/属性行/"->" 子树缩进)
#include "explain.h"

#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast.hh"
#include "bound_expr.h"
#include "catalog.h"
#include "common/err.h"
#include "log/log.h"
#include "operator.h"
#include "plan.h"

namespace exec {

namespace {

// 物化行算子: EXPLAIN 文本行源, next 顺序吐出, 无外部资源
struct RowsOp : Operator {
    std::vector<st::Row> rows;
    size_t pos = 0;

    void open() override { pos = 0; }
    bool next(st::Row* out) override
    {
        if (pos >= rows.size()) {
            return false;
        }
        *out = rows[pos++];
        return true;
    }
    void close() override {}
};

// 绑定表达式转文本: 格式与 ast.hh 的 expr_to_string 对齐, 列下标经 names 回填列名
std::string bound_expr_to_string(const ana::BoundExpr& e, const std::vector<std::string>& names)
{
    switch (e.kind()) {
    case ana::BoundExprKind::Const: {
        const auto& v = static_cast<const ana::BoundConst&>(e).value;
        if (const auto* i = std::get_if<int64_t>(&v)) {
            return std::format("{}", *i);
        }
        if (const auto* d = std::get_if<double>(&v)) {
            return std::format("{}", *d);
        }
        if (const auto* s = std::get_if<std::string>(&v)) {
            return "'" + *s + "'";
        }
        if (const auto* b = std::get_if<bool>(&v)) {
            return *b ? "true" : "false";
        }
        return "NULL";
    }
    case ana::BoundExprKind::ColRef:
        return names[static_cast<const ana::BoundColRef&>(e).col_idx];
    case ana::BoundExprKind::Arith: {
        const auto& a = static_cast<const ana::BoundArith&>(e);
        return "(" + bound_expr_to_string(*a.left, names) + " " + a.op + " "
               + bound_expr_to_string(*a.right, names) + ")";
    }
    case ana::BoundExprKind::Neg:
        return "(-" + bound_expr_to_string(*static_cast<const ana::BoundNeg&>(e).operand, names)
               + ")";
    case ana::BoundExprKind::Cmp: {
        const auto& c = static_cast<const ana::BoundCmp&>(e);
        return "(" + bound_expr_to_string(*c.left, names) + " " + cmp_to_string(c.op) + " "
               + bound_expr_to_string(*c.right, names) + ")";
    }
    case ana::BoundExprKind::Logic: {
        const auto& l = static_cast<const ana::BoundLogic&>(e);
        return "(" + bound_expr_to_string(*l.left, names)
               + (l.op == LogicOp::And ? " AND " : " OR ")
               + bound_expr_to_string(*l.right, names) + ")";
    }
    case ana::BoundExprKind::Not:
        return "(NOT " + bound_expr_to_string(*static_cast<const ana::BoundNot&>(e).operand, names)
               + ")";
    case ana::BoundExprKind::IsNull: {
        const auto& i = static_cast<const ana::BoundIsNull&>(e);
        return "(" + bound_expr_to_string(*i.operand, names)
               + (i.negate ? " IS NOT NULL" : " IS NULL") + ")";
    }
    }
    // 不可达: 全部绑定表达式种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "explain: 未知绑定表达式节点");
}

// 表列名序列: 与行内下标对应, 供列引用回填文本
std::vector<std::string> table_col_names(ct::Catalog& db, const ct::TableRef& table)
{
    const st::TableMeta meta = db.table_meta(table);
    std::vector<std::string> names;
    names.reserve(meta.cols.size());
    for (const st::ColumnSpec& c : meta.cols) {
        names.push_back(c.name);
    }
    return names;
}

// 节点输出列名序列: 供谓词/赋值的列引用回填文本; 过滤节点透传子节点输出
std::vector<std::string> output_names(ct::Catalog& db, const pl::PlanNode& node)
{
    switch (node.kind()) {
    case pl::PlanKind::SeqScan:
        return table_col_names(db, static_cast<const pl::SeqScanPlan&>(node).table);
    case pl::PlanKind::DummyScan:
        return {};  // 单行零列, 谓词不含列引用
    case pl::PlanKind::Filter:
        return output_names(db, *static_cast<const pl::FilterPlan&>(node).child);
    default:
        break;
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "explain: 节点无输出列名");
}

// 节点头文本(不含缩进): 按计划种类生成
std::string node_header(ct::Catalog& db, const pl::PlanNode& node)
{
    switch (node.kind()) {
    case pl::PlanKind::SeqScan:
        return "Seq Scan on "
               + table_ref_to_string(static_cast<const pl::SeqScanPlan&>(node).table);
    case pl::PlanKind::DummyScan:
        return "Dummy Scan";
    case pl::PlanKind::Empty:
        return "Empty Result";
    case pl::PlanKind::Filter: {
        const auto& f = static_cast<const pl::FilterPlan&>(node);
        const std::vector<std::string> names = output_names(db, *f.child);
        return "Filter: " + bound_expr_to_string(*f.pred, names);
    }
    case pl::PlanKind::Project:
        return "Project";
    case pl::PlanKind::Insert:
        return "Insert on " + table_ref_to_string(static_cast<const pl::InsertPlan&>(node).table);
    case pl::PlanKind::Delete:
        return "Delete on " + table_ref_to_string(static_cast<const pl::DeletePlan&>(node).table);
    case pl::PlanKind::Update:
        return "Update on " + table_ref_to_string(static_cast<const pl::UpdatePlan&>(node).table);
    case pl::PlanKind::CreateTable:
        return "Create Table "
               + table_ref_to_string(static_cast<const pl::CreateTablePlan&>(node).table);
    case pl::PlanKind::DropTable:
        return "Drop Table "
               + table_ref_to_string(static_cast<const pl::DropTablePlan&>(node).table);
    case pl::PlanKind::CreateSchema:
        return "Create Schema " + static_cast<const pl::CreateSchemaPlan&>(node).schema;
    case pl::PlanKind::DropSchema:
        return "Drop Schema " + static_cast<const pl::DropSchemaPlan&>(node).schema;
    case pl::PlanKind::CreateIndex: {
        const auto& p = static_cast<const pl::CreateIndexPlan&>(node);
        const st::TableMeta meta = db.table_meta(p.table);
        return "Create Index " + p.index + " on " + table_ref_to_string(p.table) + " ("
               + meta.cols[p.col_ordinal].name + ")";
    }
    case pl::PlanKind::DropIndex: {
        const auto& p = static_cast<const pl::DropIndexPlan&>(node);
        return "Drop Index " + p.index + " on " + table_ref_to_string(p.table);
    }
    case pl::PlanKind::Explain:
        return "Explain";
    case pl::PlanKind::Set:
        break;  // 语法层已将 SET 排除出可解释范围, 不可达
    }
    // 不可达: 全部计划种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::EXECUTOR, "explain: 计划节点不可解释");
}

// 节点属性行文本(不含缩进): 无属性的节点返回空
std::vector<std::string> node_props(ct::Catalog& db, const pl::PlanNode& node)
{
    std::vector<std::string> props;
    switch (node.kind()) {
    case pl::PlanKind::Project: {
        const auto& p = static_cast<const pl::ProjectPlan&>(node);
        std::string out;
        for (size_t i = 0; i < p.projs.size(); ++i) {
            if (i > 0) {
                out += ", ";
            }
            out += p.projs[i].name;
        }
        props.push_back("Output: " + out);
        break;
    }
    case pl::PlanKind::Update: {
        const auto& p = static_cast<const pl::UpdatePlan&>(node);
        const std::vector<std::string> names = table_col_names(db, p.table);
        std::string set;
        for (size_t i = 0; i < p.assigns.size(); ++i) {
            if (i > 0) {
                set += ", ";
            }
            set += names[p.assigns[i].col_idx] + " = "
                   + bound_expr_to_string(*p.assigns[i].value, names);
        }
        props.push_back("Set: " + set);
        break;
    }
    default:
        break;
    }
    return props;
}

// 渲染节点子树: 节点行(node_prefix 已含缩进或 "->  " 箭头前缀) + 属性行(缩进 prop_indent)
// + 子节点("->" 前缀于父属性行缩进处, 子树属性行缩进 +6)
void render_node(ct::Catalog& db, const pl::PlanNode& node, const std::string& node_prefix,
                 size_t prop_indent, std::vector<std::string>& lines)
{
    lines.push_back(node_prefix + node_header(db, node));
    const std::string prop_pad(prop_indent, ' ');
    for (const std::string& prop : node_props(db, node)) {
        lines.push_back(prop_pad + prop);
    }
    const pl::PlanNode* child = nullptr;
    if (node.kind() == pl::PlanKind::Filter) {
        child = static_cast<const pl::FilterPlan&>(node).child.get();
    } else if (node.kind() == pl::PlanKind::Project) {
        child = static_cast<const pl::ProjectPlan&>(node).child.get();
    } else if (node.kind() == pl::PlanKind::Delete) {
        const auto& p = static_cast<const pl::DeletePlan&>(node);
        child = p.child.get();  // 全表删除无子树
    } else if (node.kind() == pl::PlanKind::Update) {
        child = static_cast<const pl::UpdatePlan&>(node).child.get();
    } else if (node.kind() == pl::PlanKind::Explain) {
        child = static_cast<const pl::ExplainPlan&>(node).child.get();
    }
    if (child != nullptr) {
        render_node(db, *child, prop_pad + "->  ", prop_indent + 6, lines);
    }
}

}  // namespace

ExecResult run_explain(ct::Catalog& db, const pl::ExplainPlan& plan)
{
    // 顶层 Explain 节点自身不渲染(单层输出即被解释计划), 嵌套 Explain 显示为普通节点
    std::vector<std::string> lines;
    render_node(db, *plan.child, "", 2, lines);

    auto op = std::make_unique<RowsOp>();
    op->rows.reserve(lines.size());
    for (std::string& line : lines) {
        st::Row row;
        row.values.push_back(st::Value(std::move(line)));
        op->rows.push_back(std::move(row));
    }
    ExecResult r;
    r.is_result_set = true;
    r.col_names = {"QUERY PLAN"};
    r.stream = std::move(op);
    r.stream->open();
    return r;
}

}  // namespace exec
