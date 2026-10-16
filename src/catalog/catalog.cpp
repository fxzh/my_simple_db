// catalog.cpp: 目录层生命周期与事务: 构造/create/open/close, 全局锁事务三段
#include "catalog.h"

#include <algorithm>
#include <utility>

#include "catalog_meta.h"
#include "common/err.h"
#include "config/config.h"
#include "log/log.h"

namespace ct {

// ==================== 生命周期 ====================

Catalog::Catalog(std::string dir, bool bootstrap_mode)
        : dir_(dir), engine_(std::move(dir)), bootstrap_mode_(bootstrap_mode) {}

void Catalog::create()
{
    if (engine_.table_file_exists(kTableMetaId)) {
        DB_RAISE(db::ErrCode::CatalogExists, LogModule::CATALOG, "数据目录已初始化: {}", dir_);
    }
    // 自举全程一个事务: 引擎持久化原语须在活动事务内调用
    begin_txn();
    bootstrap_meta_tables();
    commit_txn();
    // create 不进入打开状态, 检查点收尾(含清空 WAL)后返回, bootstrap 阶段从零日志起步
    engine_.checkpoint();
}

void Catalog::open()
{
    if (bootstrap_mode_) {
        // bootstrap 模式: 目录刚由 initdb 引导, 检查自举表文件存在
        if (!engine_.table_file_exists(kTableMetaId) || !engine_.table_file_exists(kColumnMetaId)
            || !engine_.table_file_exists(kSchemaMetaId)
            || !engine_.table_file_exists(kTypeMetaId)) {
            DB_RAISE(db::ErrCode::CatalogMissing, LogModule::CATALOG, "数据目录未初始化: {}", dir_);
        }
    } else if (!engine_.table_file_exists(kTableMetaId)) {
        // 正常模式: db_table 文件缺失无法扫描, 完成标记检查在下方扫 db_table 时按名进行
        DB_RAISE(db::ErrCode::CatalogMissing, LogModule::CATALOG,
                 "数据目录未初始化或初始化未完成: {}", dir_);
    }
    engine_.open();
    // 此处先于客户端线程, 不持锁; 扫 db_table 取现存最大 file_id/table_id, 扫 db_schema 取现存最大
    // schema_id, 扫 db_index 把索引文件 id 并入 file_id 分配起点
    int64_t max_fid = 0;
    int64_t max_tid = 0;
    bool has_version = false;
    for (const std::vector<st::Value>& row : engine_.read_rows(kTableMetaId, table_meta_cols())) {
        max_fid = std::max(max_fid, row_int(row, 2));
        max_tid = std::max(max_tid, row_int(row, 0));
        if (row_str(row, 1) == kVersionMetaName && row_int(row, 3) == kSystemSchemaId) {
            has_version = true;
        }
    }
    // 完成标记: system 名下的 db_version 行存在代表 bootstrap.sql 全部执行成功,
    // 用户 schema 同名表不算数, 仅正常模式要求
    if (!bootstrap_mode_ && !has_version) {
        DB_RAISE(db::ErrCode::CatalogMissing, LogModule::CATALOG, "数据目录未初始化或初始化未完成: {}",
                 dir_);
    }
    // 索引文件同占 file_id 段: 并入分配起点
    max_fid = std::max(max_fid, load_index_meta());
    load_func_meta();
    next_file_id_.store(static_cast<uint64_t>(max_fid) + 1);
    next_table_id_.store(static_cast<uint64_t>(
            std::max(max_tid + 1, static_cast<int64_t>(kFirstUserTableId))));
    int64_t max_sid = 0;
    for (const std::vector<st::Value>& row : engine_.read_rows(kSchemaMetaId, schema_meta_cols())) {
        max_sid = std::max(max_sid, row_int(row, 0));
    }
    next_schema_id_.store(static_cast<uint64_t>(max_sid) + 1);
}

// 加载 db_index 元数据并返回现存最大 file_id: 列定义从 db_column 载入内存缓存;
// bootstrap 模式 db_index 由 bootstrap.sql 在 open 之后创建, 缺失返回 0;
// 正常模式初始化完成后必存在, 缺失即元数据损坏
int64_t Catalog::load_index_meta()
{
    if (!engine_.table_file_exists(kIndexMetaId)) {
        if (bootstrap_mode_) {
            return 0;
        }
        DB_CRITICAL(LogModule::CATALOG, "db_index 元数据表缺失");
    }
    // db_index 列定义不硬编码: 从 db_column 载入内存缓存, 后续读写均用缓存
    index_cols_ = collect_columns(engine_, kIndexMetaId, kIndexMetaName);
    int64_t max_fid = 0;
    for (const std::vector<st::Value>& row : engine_.read_rows(kIndexMetaId, index_cols_)) {
        max_fid = std::max(max_fid, row_int(row, 3));
    }
    return max_fid;
}

// 加载 db_func 列定义入缓存并校验全量行: bootstrap 模式 db_func 由 bootstrap.sql 在
// open 之后创建, 缺失直接返回; 正常模式初始化完成后必存在, 缺失即元数据损坏
void Catalog::load_func_meta()
{
    if (!engine_.table_file_exists(kFuncMetaId)) {
        if (bootstrap_mode_) {
            return;
        }
        DB_CRITICAL(LogModule::CATALOG, "db_func 元数据表缺失");
    }
    func_cols_ = collect_columns(engine_, kFuncMetaId, kFuncMetaName);
    validate_func_rows(engine_, func_cols_);
}

void Catalog::close()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    engine_.close();
}

// ==================== 事务 ====================

// 开启事务: 全局锁长期持有至事务结束, 事务内门面方法同线程递归重入
void Catalog::begin_txn()
{
    mutex_.lock();
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::adopt_lock);
    engine_.begin_txn();
    lock.release();
}

// 提交事务: Engine 侧追加 Commit 并 fsync(持久化边界), 之后 WAL 自上次清空累计
// 字节达到阈值则做运行期检查点——此刻事务记录已结束、锁仍持有, 检查点不可能
// 打断进行中的事务; 守卫保证任意退出路径都解除全局锁
void Catalog::commit_txn()
{
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::adopt_lock);
    try {
        engine_.commit_txn();
    } catch (...) {
        // 提交失败: 残留引擎事务按回滚清理, 异常原样上抛
        if (engine_.in_txn()) {
            engine_.rollback_txn();
        }
        throw;
    }
    const uint64_t bytes = engine_.wal_bytes_since_reset();
    if (bytes >= config::cfg.wal_checkpoint_bytes) {
        engine_.checkpoint();
        LOG_INFO(LogModule::CATALOG,
                 "运行期检查点: WAL 自上次清空累计 %llu 字节达到阈值, 已刷脏页并清空日志",
                 static_cast<unsigned long long>(bytes));
    }
}

// 回滚事务: Engine 侧按 undo 逆序复原已发生的修改, 守卫保证异常路径也解除全局锁
void Catalog::rollback_txn()
{
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::adopt_lock);
    engine_.rollback_txn();
}

}  // namespace ct
