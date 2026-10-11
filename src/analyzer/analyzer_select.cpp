// analyzer_select.cpp: SELECT 绑定: 投影展开与排序键绑定
#include "analyzer_select.h"

#include <memory>
#include <string>
#include <vector>

#include "analyzer_expr.h"
#include "ast.hh"
#include "bound.h"
#include "bound_expr.h"
#include "catalog.h"
#include "common/err.h"
#include "log/log.h"

namespace ana {

namespace {

// 绑定单个投影项(非 star): 绑定表达式并定输出列名, 列名取别名>列名>表达式文本
ProjCol bind_proj_item(const SelectItem& item, const Schema& schema, ct::Catalog& db)
{
    st::ColType t;
    std::unique_ptr<BoundExpr> e = bind_expr(*item.expr, &schema, db, t);
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

// 排序键解析中间结果: 命中输出列为下标, 否则为行上下文绑定表达式
struct OrderKey {
    bool is_out = false;
    size_t out_idx = 0;
    std::unique_ptr<BoundExpr> expr;
};

// 解析单个排序键: 整数常量按输出列序号; 裸名先按输出列名解析(命中多个报歧义, 未命中转
// 行上下文绑定); 非整数裸常量报错; 其余表达式按行上下文绑定
OrderKey resolve_order_key(const OrderItem& item, const ProjBind& pb, const Schema& schema,
                           ct::Catalog& db)
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
    st::ColType t;
    k.expr = bind_expr(e, &schema, db, t);
    return k;
}

}  // namespace

// ==================== 投影绑定 ====================

ProjBind build_projs(const SelectStmt& ss, const st::TableMeta& meta, const Schema& schema,
                     ct::Catalog& db)
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
        out.projs.push_back(bind_proj_item(item, schema, db));
        out.src.push_back(item.expr.get());
    }
    return out;
}

ProjBind build_projs_no_from(const SelectNoFromStmt& ss, const Schema& schema, ct::Catalog& db)
{
    ProjBind out;
    for (const SelectItem& item : ss.items()) {
        if (item.star) {
            DB_RAISE(db::ErrCode::StarNoFrom, LogModule::ANALYZER, "无 FROM 的 SELECT 不允许 *");
        }
        out.projs.push_back(bind_proj_item(item, schema, db));
        out.src.push_back(item.expr.get());
    }
    return out;
}

// ==================== 排序绑定 ====================

std::vector<BoundOrderItem> bind_orders(const std::vector<OrderItem>& items, const ProjBind& pb,
                                        const Schema& schema, bool& sort_on_output,
                                        ct::Catalog& db)
{
    std::vector<OrderKey> keys;
    keys.reserve(items.size());
    bool all_out = true;
    for (const OrderItem& item : items) {
        keys.push_back(resolve_order_key(item, pb, schema, db));
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
            st::ColType t;
            o.expr = bind_expr(*pb.src[keys[i].out_idx], &schema, db, t);
        } else if (keys[i].is_out) {
            // star 展开列(仅带 FROM 的 SELECT 存在): 取行内列引用
            const size_t idx = pb.projs[keys[i].out_idx].col_idx;
            o.expr = std::make_unique<BoundColRef>(idx, schema.col_types[idx]);
        } else {
            o.expr = std::move(keys[i].expr);
        }
        orders.push_back(std::move(o));
    }
    return orders;
}

}  // namespace ana
