// catalog.h: 目录层对外接口(元数据表逻辑 + 名字型/句柄型门面), 复合操作持全局锁
#ifndef CATALOG_CATALOG_H
#define CATALOG_CATALOG_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"

namespace ct {

// id 保留段: 1~20000 留给系统元数据 table_id, 用户 table_id 从 20001 起分配
// table_id/file_id 分配: open() 扫 db_table 取现存最大值作起点原子递增不回收; file_id 不走保留段, 用户文件 id 可与保留段数值重叠
// schema_id 分配: open() 扫 db_schema 取现存最大值+1 作起点原子递增不回收, 无保留段
constexpr uint64_t kReservedMaxTableId = 20000;
constexpr uint64_t kFirstUserTableId = 20001;

// 元数据表(保留段固定 id): db_table 记表名, db_column 记列定义, db_schema 记 schema, 引导期 schema 硬编码
constexpr uint64_t kTableMetaId = 1;
constexpr uint64_t kColumnMetaId = 2;
constexpr uint64_t kSchemaMetaId = 3;
constexpr const char* kTableMetaName = "db_table";
constexpr const char* kColumnMetaName = "db_column";
constexpr const char* kSchemaMetaName = "db_schema";

// bootstrap.sql 创建的系统表(文档性 id 与文件内 set table_id 值一致), 列定义唯一事实来源在 bootstrap.sql
constexpr uint64_t kIndexMetaId = 4;
constexpr const char* kIndexMetaName = "db_index";

// bootstrap.sql 末尾创建的完成标记表: 行存在即代表初始化全程成功, 正常模式 open() 按名检查
constexpr const char* kVersionMetaName = "db_version";

// system schema 固定 id: 引导写入 db_schema 首行, 元数据表均挂其名下
constexpr uint64_t kSystemSchemaId = 1;
constexpr const char* kSystemSchemaName = "system";

// public schema: bootstrap.sql 经正常路径创建, 是会话 current_schema 的缺省值;
// 无固定 id, 允许删除与重建
constexpr const char* kPublicSchemaName = "public";

// 限定表名: 名字解析产物; SQL 路径的未限定名由 analyzer 填入 current_schema,
// 空 schema 仅来自 catalog 直连调用方的构造错误
struct TableRef {
    std::string schema;
    std::string name;
};

// 限定表名转文本(保留输入形态, 未限定仅表名): 供打印与日志使用
inline std::string table_ref_to_string(const TableRef& t)
{
    return t.schema.empty() ? t.name : t.schema + "." + t.name;
}

// 表句柄: 限定名一次解析的产物(元数据+限定名文本), 由语义分析层产出随语句带到执行期,
// 语句全程持事务锁故解析结果到执行期不变; 显示名仅供报错与 EXPLAIN 文本
struct TableHandle {
    st::TableMeta meta;
    std::string display;
};

// 单行更新任务: 旧行物理位置 + 新行全量值(赋值右值已按旧行求值完毕)
struct RowUpdate {
    st::RowRef ref;
    std::vector<st::Value> values;
};

// 索引目录条目: 索引定义 + db_index 内的行位置
struct IndexEntry {
    std::string name;
    uint16_t col_ordinal = 0;
    uint64_t file_id = 0;
    st::RowRef row_ref;
};

// 数据目录门面: 打开/关闭, 建表/删表/插入/删除/更新/全表扫描
// 元数据以 db_table/db_column/db_schema 三张表为唯一事实来源, 查找实时扫描, 无目录文件与内存缓存
// 名字只经 create_table 与 table_meta 进入, DML 门面按绑定层一次解析的元数据进入
class Catalog {
public:
    explicit Catalog(std::string dir, bool bootstrap_mode = false);

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    // 初始化数据目录: 引导两张元数据表, 目录须已存在且未初始化, 不进入打开状态
    void create();
    // 打开已初始化的数据目录, 缺失当场报错: 正常模式按名检查 db_version 完成标记, bootstrap 模式检查三张自举表文件
    void open();
    // 刷盘并关闭
    void close();
    // 开启事务: 全局锁由本事务长持至 commit_txn/rollback_txn, 事务内门面方法
    // 同线程递归重入, 其他会话的语句阻塞在锁上
    void begin_txn();
    // 提交事务: 有记录则追加 Commit 并 fsync(持久化边界), WAL 自上次清空累计
    // 字节达到阈值时做运行期检查点, 解除全局锁
    void commit_txn();
    // 回滚事务: 按 undo 逆序复原本事务已发生的修改后解除全局锁
    void rollback_txn();

    // 建表: 挂限定名所属 schema(schema 须存在), 表名 schema 内唯一,
    // bootstrap 模式用 SET 的显式 table_id(未 set/重复 id 报错), 正常模式自动分配
    uint64_t create_table(const TableRef& table, const std::vector<st::ColumnSpec>& cols);
    // 删表(按绑定层句柄), 保留段表拒绝删除
    void drop_table(const TableHandle& table);
    // 建 schema, 重名拒绝
    void create_schema(const std::string& name);
    // 删 schema, 不存在的拒绝, 非空拒绝(不级联)
    void drop_schema(const std::string& name);
    // 建索引(按绑定层句柄): 建索引文件并全表回填后写 db_index 行, 索引名表内唯一, 列序号由绑定层解析
    void create_index(const TableHandle& table, const std::string& index, uint16_t col_ordinal);
    // 删索引(按绑定层句柄), 索引不存在当场报错, 先删 db_index 行再删索引文件
    void drop_index(const TableHandle& table, const std::string& index);
    // 插入行(按绑定层元数据)
    st::RowId insert(const st::TableMeta& meta, const std::vector<st::Value>& values);
    // 删除单行(按扫描得到的物理位置), 已删引用返回 0, 无效引用报错
    size_t delete_by_ref(const st::RowRef& ref);
    // 批量更新: 旧行位置打墓碑后追加新值行, 新行全量双写该表全部索引(旧行索引条目残留,
    // 回表按墓碑过滤), 返回更新行数; 值合法性由存储层编码校验, 引用已删/指向他表当场报错
    size_t update_rows(const st::TableMeta& meta, const std::vector<RowUpdate>& rows);
    // 删除表中全部行, 返回删除行数
    size_t delete_all(const st::TableMeta& meta);

    // 按绑定层元数据开扫描, 游标不持锁(仅持页 pin), 并发 DDL 期间扫描是未定义行为
    std::unique_ptr<st::Scanner> scan(const st::TableMeta& meta);
    // 该表全部索引(按绑定层元数据): 扫 db_index 匹配 table_id, 供计划层选择索引
    std::vector<IndexEntry> indexes(const st::TableMeta& meta);
    // 按索引文件 id 开范围扫描, 边界缺省为最左/无上界; 游标不持锁(仅持页 pin),
    // 须在语句事务锁内调用(首开索引文件会写引擎树表)
    std::unique_ptr<st::BTreeScanner> index_scan(uint64_t index_fid, std::optional<st::ScanBound> lo,
                                                 std::optional<st::ScanBound> hi);
    // 回表: 按行物理位置直读堆页取行, 已删/槽位越界返回 false, 无效引用当场报错
    bool read_row(const st::RowRef& ref, const std::vector<st::ColumnSpec>& cols, st::Row* out);
    // 存活行数统计(便利函数, 供测试与将来执行层使用)
    size_t row_count(const st::TableMeta& meta);
    // 按限定名取表元数据(实时扫描元数据表), 表不存在当场报错
    st::TableMeta table_meta(const TableRef& table);

    // bootstrap 模式标志(server --bootstrap 启动时传入): 管 SET 语句门禁等
    bool bootstrap_mode() const { return bootstrap_mode_; }
    // SET table_id 变量: 指定下一条 create table 使用的 table_id, 0 表示未 set, 用后不清零
    void set_bootstrap_table_id(uint64_t v) { bootstrap_table_id_ = v; }

private:
    // 加载 db_index 并返回现存最大 file_id(open 期不持锁调用): 列定义载入内存缓存,
    // bootstrap 模式缺失返回 0(尚未由 bootstrap.sql 创建), 正常模式缺失报错
    int64_t load_index_meta();
    // 建表公共路径(须持锁): 校验后按指定 table_id 建数据文件、写元数据行, file_id 内部分配
    uint64_t create_table_impl(uint64_t sid, const TableRef& table,
                               const std::vector<st::ColumnSpec>& cols, uint64_t tid);
    // bootstrap 模式建表(须持锁): 用 SET 的显式 table_id, 未 set 或被占用报错,
    // 建 db_index 时填充内存列定义缓存
    uint64_t create_table_bootstrap(uint64_t sid, const TableRef& table,
                                    const std::vector<st::ColumnSpec>& cols);
    // 写入指定表的元数据行(须持锁): db_table 一行, db_column 每列一行, 引导与建表共用
    void write_meta_rows(uint64_t sid, uint64_t tid, uint64_t fid, const std::string& name,
                         const std::vector<st::ColumnSpec>& cols);
    // 引导元数据表: 直接建数据文件并写入自描述行与 system schema 行, 不经过元数据表查找
    void bootstrap_meta_tables();
    // 删除指定表的元数据行(须持锁): 按 table_id 匹配 db_table/db_column
    void delete_meta_rows(uint64_t tid);
    // 按限定名查元数据(须持锁): 解析 schema_id 后 db_table 按 (schema_id, 表名) 定位 id,
    // db_column 收集列并按 ordinal 排序, 表不存在或元数据行非法当场报错
    st::TableMeta find_table_meta(const TableRef& table);
    // 按限定名解析 schema_id(须持锁): schema 不存在当场报错, 空 schema 为调用方构造错误
    uint64_t resolve_schema_id(const TableRef& table);
    // schema 内表名是否已存在(须持锁): 扫 db_table 匹配 schema_id 与表名
    bool has_table_name(uint64_t sid, const std::string& name);
    // table_id 是否已被占用(须持锁): 全扫 db_table 匹配, bootstrap 显式 id 建表查重
    bool has_table_id(uint64_t tid);
    // schema 名是否已存在(须持锁): 全扫 db_schema 匹配
    bool has_schema_name(const std::string& name);
    // 用户段 table_id 分配: 原子自增返回, 依赖 open() 扫描初始化(不低于 kFirstUserTableId)
    uint64_t alloc_table_id();
    // file_id 分配: 原子自增返回, 依赖 open() 扫描初始化
    uint64_t alloc_file_id();
    // schema_id 分配: 原子自增返回, 依赖 open() 扫描初始化
    uint64_t alloc_schema_id();

    std::string dir_;
    st::Engine engine_;   // 文件引擎, 原语经本类持锁调用
    std::recursive_mutex mutex_;   // 序列化所有复合操作, 事务期间长持(并发演化见存储设计文档 §10)
    std::atomic<uint64_t> next_file_id_{0};   // 下一个 file_id, open() 扫 db_table 取最大值+1 初始化
    std::atomic<uint64_t> next_table_id_{0};   // 下一个 table_id, open() 扫 db_table 取最大值+1 初始化, 不低于 kFirstUserTableId
    std::atomic<uint64_t> next_schema_id_{0};  // 下一个 schema_id, open() 扫 db_schema 取最大值+1 初始化
    bool bootstrap_mode_ = false;        // bootstrap 模式标志, 构造时由 server --bootstrap 传入
    uint64_t bootstrap_table_id_ = 0;    // SET table_id 变量, 0 表示未 set, 与 next_table_id_ 分配器无关
    std::vector<st::ColumnSpec> index_cols_;  // db_index 列定义缓存, open 从 db_column 载入, bootstrap 建表时填充
};

}  // namespace ct
#endif
