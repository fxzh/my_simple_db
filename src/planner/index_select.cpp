// index_select.cpp: 索引选择实现: Filter(SeqScan) 改写为 残Filter(Fetch(IndexScan))
#include "planner.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ast.hh"
#include "bound_expr.h"
#include "catalog.h"
#include "common/err.h"
#include "log/log.h"

namespace pl {

namespace {

// NULL 键: 全序排最大, 键值部分无意义
constexpr st::IndexKey k_null_key{0, true};

// 下界取更紧者: 键更大更紧, 同键开界更紧
std::optional<st::ScanBound> tighter_lo(const std::optional<st::ScanBound>& a,
                                        const std::optional<st::ScanBound>& b)
{
    if (!a.has_value() || !b.has_value()) {
        return a.has_value() ? a : b;
    }
    const int c = st::key_cmp(a->key, b->key);
    if (c != 0) {
        return c > 0 ? a : b;
    }
    return a->inclusive ? b : a;
}

// 上界取更紧者: 键更小更紧, 同键开界更紧
std::optional<st::ScanBound> tighter_hi(const std::optional<st::ScanBound>& a,
                                        const std::optional<st::ScanBound>& b)
{
    if (!a.has_value() || !b.has_value()) {
        return a.has_value() ? a : b;
    }
    const int c = st::key_cmp(a->key, b->key);
    if (c != 0) {
        return c < 0 ? a : b;
    }
    return a->inclusive ? b : a;
}

// 区间交: 下界越过上界(同键时含开界矛盾)即交空
std::optional<IndexRange> range_meet(const IndexRange& a, const IndexRange& b)
{
    IndexRange r{tighter_lo(a.lo, b.lo), tighter_hi(a.hi, b.hi)};
    if (r.lo.has_value() && r.hi.has_value()) {
        const int c = st::key_cmp(r.lo->key, r.hi->key);
        if (c > 0 || (c == 0 && !(r.lo->inclusive && r.hi->inclusive))) {
            return std::nullopt;
        }
    }
    return r;
}

// 区间集交: 双方按键序升序时结果保序
std::vector<IndexRange> ranges_meet(const std::vector<IndexRange>& a, const std::vector<IndexRange>& b)
{
    std::vector<IndexRange> out;
    for (const IndexRange& x : a) {
        for (const IndexRange& y : b) {
            if (std::optional<IndexRange> r = range_meet(x, y)) {
                out.push_back(std::move(*r));
            }
        }
    }
    return out;
}

// 单列比较合取项的精确区间: 谓词语义排除 NULL 行, 无上界段截到 NULL 键开
std::vector<IndexRange> cmp_ranges(CmpOp op, const st::IndexKey& k)
{
    const st::ScanBound closed_k{k, true};
    const st::ScanBound open_k{k, false};
    const st::ScanBound open_null{k_null_key, false};
    switch (op) {
    case CmpOp::Eq:
        return {{closed_k, closed_k}};
    case CmpOp::Lt:
        return {{std::nullopt, open_k}};
    case CmpOp::Le:
        return {{std::nullopt, closed_k}};
    case CmpOp::Gt:
        return {{open_k, open_null}};
    case CmpOp::Ge:
        return {{closed_k, open_null}};
    case CmpOp::Ne:
        return {{std::nullopt, open_k}, {open_k, open_null}};
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "index_select: 未知比较种类");
}

// 常量与列同族才可入键: int64↔Int/BigInt, double↔Double(Float 列不参与), bool↔Bool
bool key_of(const st::ColumnSpec& col, const st::Value& v, st::IndexKey& out)
{
    if (st::value_is_null(v)) {
        return false;
    }
    if (std::get_if<int64_t>(&v.box) != nullptr) {
        if (col.type != st::ColType::Int && col.type != st::ColType::BigInt) {
            return false;
        }
    } else if (std::get_if<double>(&v.box) != nullptr) {
        if (col.type != st::ColType::Double) {
            return false;
        }
    } else if (std::get_if<bool>(&v.box) != nullptr) {
        if (col.type != st::ColType::Bool) {
            return false;
        }
    } else {
        return false;
    }
    out = st::encode_key(col.type, v);
    return true;
}

// 比较符镜像: 常量在左时翻转取界
CmpOp mirror(CmpOp op)
{
    switch (op) {
    case CmpOp::Eq:
    case CmpOp::Ne:
        return op;
    case CmpOp::Lt:
        return CmpOp::Gt;
    case CmpOp::Le:
        return CmpOp::Ge;
    case CmpOp::Gt:
        return CmpOp::Lt;
    case CmpOp::Ge:
        return CmpOp::Le;
    }
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "index_select: 未知比较种类");
}

// 命中合取项: 合取项下标 + 精确区间 + 渲染数据
struct Hit {
    size_t conj = 0;
    bool eq = false;
    std::vector<IndexRange> ranges;
    IndexCond cond;
};

// 合取项对索引列命中: 形态(ColRef <op> Const 含镜像 / IS [NOT] NULL)与常量族均符才命中
bool match_hit(const ana::BoundExpr& e, const st::ColumnSpec& col, uint16_t ordinal, size_t conj,
               Hit& out)
{
    if (e.kind() == ana::BoundExprKind::Cmp) {
        const auto& c = static_cast<const ana::BoundCmp&>(e);
        const bool lcol = c.left->kind() == ana::BoundExprKind::ColRef
                          && static_cast<const ana::BoundColRef&>(*c.left).col_idx == ordinal;
        const bool rcol = c.right->kind() == ana::BoundExprKind::ColRef
                          && static_cast<const ana::BoundColRef&>(*c.right).col_idx == ordinal;
        if (lcol == rcol) {
            return false;
        }
        const ana::BoundConst* cst = nullptr;
        CmpOp op = CmpOp::Eq;
        if (lcol) {
            if (c.right->kind() == ana::BoundExprKind::Const) {
                cst = static_cast<const ana::BoundConst*>(c.right.get());
                op = c.op;
            }
        } else {
            if (c.left->kind() == ana::BoundExprKind::Const) {
                cst = static_cast<const ana::BoundConst*>(c.left.get());
                op = mirror(c.op);
            }
        }
        if (cst == nullptr) {
            return false;
        }
        st::IndexKey k;
        if (!key_of(col, cst->value, k)) {
            return false;
        }
        out.conj = conj;
        out.eq = op == CmpOp::Eq;
        out.ranges = cmp_ranges(op, k);
        out.cond = IndexCond{col.name, op, cst->value, false, false};
        return true;
    }
    if (e.kind() == ana::BoundExprKind::IsNull) {
        const auto& i = static_cast<const ana::BoundIsNull&>(e);
        if (i.operand->kind() != ana::BoundExprKind::ColRef
            || static_cast<const ana::BoundColRef&>(*i.operand).col_idx != ordinal) {
            return false;
        }
        out.conj = conj;
        out.eq = false;
        if (i.negate) {
            out.ranges = {{std::nullopt, st::ScanBound{k_null_key, false}}};
        } else {
            out.ranges = {{st::ScanBound{k_null_key, true}, st::ScanBound{k_null_key, true}}};
        }
        out.cond = IndexCond{col.name, CmpOp::Eq, st::Value{}, true, i.negate};
        return true;
    }
    return false;
}

// 摊平 AND 树为合取项(只读)
void flatten_view(const ana::BoundExpr* e, std::vector<const ana::BoundExpr*>& out)
{
    if (e->kind() == ana::BoundExprKind::Logic
        && static_cast<const ana::BoundLogic&>(*e).op == LogicOp::And) {
        const auto& l = static_cast<const ana::BoundLogic&>(*e);
        flatten_view(l.left.get(), out);
        flatten_view(l.right.get(), out);
        return;
    }
    out.push_back(e);
}

// 摊平 AND 树为合取项(移出): 原树不可再用
void flatten_take(std::unique_ptr<ana::BoundExpr>& e, std::vector<std::unique_ptr<ana::BoundExpr>>& out)
{
    if (e->kind() == ana::BoundExprKind::Logic
        && static_cast<ana::BoundLogic&>(*e).op == LogicOp::And) {
        auto& l = static_cast<ana::BoundLogic&>(*e);
        flatten_take(l.left, out);
        flatten_take(l.right, out);
        return;
    }
    out.push_back(std::move(e));
}

// 合取项重组为左结合 AND 树
std::unique_ptr<ana::BoundExpr> join_and(std::vector<std::unique_ptr<ana::BoundExpr>> parts)
{
    std::unique_ptr<ana::BoundExpr> cur = std::move(parts.front());
    for (size_t i = 1; i < parts.size(); ++i) {
        cur = std::make_unique<ana::BoundLogic>(LogicOp::And, std::move(cur), std::move(parts[i]));
    }
    return cur;
}

// Filter(SeqScan) 的索引改写: 命中则整棵替换, 交空剪成空结果节点, 未命中保持原树
void rewrite_scan(ct::Catalog& db, std::unique_ptr<PlanNode>& plan)
{
    auto& f = static_cast<FilterPlan&>(*plan);
    if (f.child->kind() != PlanKind::SeqScan) {
        return;  // 无 FROM 的行源(DummyScan)无表可索引
    }
    const SeqScanPlan& scan = static_cast<const SeqScanPlan&>(*f.child);
    const std::vector<ct::IndexEntry> idxs = db.indexes(scan.table.meta);
    if (idxs.empty()) {
        return;
    }
    std::vector<const ana::BoundExpr*> conj;
    flatten_view(f.pred.get(), conj);

    // 选索引: 等值命中优先, 同级取列序号小者, 再取元数据序
    const ct::IndexEntry* best = nullptr;
    bool best_eq = false;
    std::vector<Hit> best_hits;
    for (const ct::IndexEntry& ent : idxs) {
        if (ent.col_ordinal >= scan.table.meta.cols.size()) {
            DB_CRITICAL(LogModule::PLANNER, "索引列序号越界: {}", ent.name);
        }
        std::vector<Hit> hits;
        for (size_t i = 0; i < conj.size(); ++i) {
            Hit h;
            if (match_hit(*conj[i], scan.table.meta.cols[ent.col_ordinal], ent.col_ordinal, i, h)) {
                hits.push_back(std::move(h));
            }
        }
        if (hits.empty()) {
            continue;
        }
        const bool has_eq = std::any_of(hits.begin(), hits.end(), [](const Hit& h) { return h.eq; });
        if (best != nullptr && !(has_eq && !best_eq) && !(has_eq == best_eq
                                                          && ent.col_ordinal < best->col_ordinal)) {
            continue;
        }
        best = &ent;
        best_eq = has_eq;
        best_hits = std::move(hits);
    }
    if (best == nullptr) {
        return;
    }

    // 命中区间求交, 交空剪成空结果节点
    std::vector<IndexRange> ranges = {{}};
    for (const Hit& h : best_hits) {
        ranges = ranges_meet(ranges, h.ranges);
        if (ranges.empty()) {
            break;
        }
    }
    if (ranges.empty()) {
        auto empty = std::make_unique<EmptyPlan>();
        empty->table = scan.table.display;
        plan = std::move(empty);
        return;
    }

    // 摘除命中合取项, 重组残余谓词
    std::vector<bool> hit_conj(conj.size(), false);
    for (const Hit& h : best_hits) {
        hit_conj[h.conj] = true;
    }
    std::vector<std::unique_ptr<ana::BoundExpr>> own;
    flatten_take(f.pred, own);
    std::vector<std::unique_ptr<ana::BoundExpr>> residual;
    for (size_t i = 0; i < own.size(); ++i) {
        if (!hit_conj[i]) {
            residual.push_back(std::move(own[i]));
        }
    }

    auto isp = std::make_unique<IndexScanPlan>();
    isp->table = scan.table;
    isp->index = best->name;
    isp->index_fid = best->file_id;
    isp->ranges = std::move(ranges);
    isp->conds.reserve(best_hits.size());
    for (const Hit& h : best_hits) {
        isp->conds.push_back(std::move(h.cond));
    }
    auto fetch = std::make_unique<FetchPlan>();
    fetch->table = scan.table;
    fetch->child = std::move(isp);
    if (!residual.empty()) {
        auto filter = std::make_unique<FilterPlan>();
        filter->pred = join_and(std::move(residual));
        filter->child = std::move(fetch);
        plan = std::move(filter);
    } else {
        plan = std::move(fetch);
    }
}

}  // namespace

// 索引选择: 自顶向下找 Filter(SeqScan) 改写, 其余节点只下钻
void select_index(ct::Catalog& db, std::unique_ptr<PlanNode>& plan)
{
    switch (plan->kind()) {
    case PlanKind::Filter:
        rewrite_scan(db, plan);
        return;
    case PlanKind::Project:
        select_index(db, static_cast<ProjectPlan&>(*plan).child);
        return;
    case PlanKind::Sort:
        select_index(db, static_cast<SortPlan&>(*plan).child);
        return;
    case PlanKind::Delete: {
        auto& d = static_cast<DeletePlan&>(*plan);
        if (d.child != nullptr) {  // child 为空表示全表删除
            select_index(db, d.child);
        }
        return;
    }
    case PlanKind::Update:
        select_index(db, static_cast<UpdatePlan&>(*plan).child);
        return;
    case PlanKind::Explain:
        select_index(db, static_cast<ExplainPlan&>(*plan).child);
        return;
    case PlanKind::SeqScan:
    case PlanKind::IndexScan:
    case PlanKind::Fetch:
    case PlanKind::DummyScan:
    case PlanKind::Empty:
    case PlanKind::Insert:
    case PlanKind::CreateTable:
    case PlanKind::DropTable:
    case PlanKind::CreateSchema:
    case PlanKind::DropSchema:
    case PlanKind::CreateIndex:
    case PlanKind::DropIndex:
    case PlanKind::Set:
        return;  // 无过滤子树
    }
    // 不可达: 全部计划种类已在上方穷尽
    DB_RAISE(db::ErrCode::Internal, LogModule::PLANNER, "index_select: 未知计划种类");
}

}  // namespace pl
