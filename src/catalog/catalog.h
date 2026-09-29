// catalog.h: 目录层对外接口(元数据表逻辑 + 名字型门面), 复合操作持全局锁
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

// system schema 固定 id: 引导写入 db_schema 首行, 现阶段所有表的 schema_id 均挂其名下
constexpr uint64_t kSystemSchemaId = 1;
constexpr const char* kSystemSchemaName = "system";

// 数据目录门面: 打开/关闭, 建表/删表/插入/删除/全表扫描
// 元数据以 db_table/db_column/db_schema 三张表为唯一事实来源, 查找实时扫描, 无目录文件与内存缓存
class Catalog {
public:
    explicit Catalog(std::string dir);

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    // 初始化数据目录: 引导两张元数据表, 目录须已存在且未初始化, 不进入打开状态
    void create();
    // 打开已初始化的数据目录(以元数据表文件存在为准), 元数据表缺失当场报错
    void open();
    // 刷盘并关闭
    void close();

    uint64_t create_table(const std::string& name, const std::vector<st::ColumnSpec>& cols);
    // 删表, 保留段表拒绝删除
    void drop_table(const std::string& name);
    // 建 schema, 重名拒绝
    void create_schema(const std::string& name);
    // 删 schema, 不存在的拒绝, 非空拒绝(不级联)
    void drop_schema(const std::string& name);
    st::RowId insert(const std::string& table, const std::vector<st::Value>& values);
    // 删除单行(按扫描得到的物理位置), 已删引用返回 0, 无效引用报错
    size_t delete_by_ref(const st::RowRef& ref);
    // 删除表中全部行, 返回删除行数
    size_t delete_all(const std::string& table);

    std::unique_ptr<st::Scanner> scan(const std::string& table);
    // 存活行数统计(便利函数, 供测试与将来执行层使用)
    size_t row_count(const std::string& table);
    // 按表名取表元数据(实时扫描元数据表), 表不存在当场报错
    st::TableMeta table_meta(const std::string& name);

private:
    // 建表公共路径(须持锁): 校验后按指定 table_id 建数据文件、写元数据行, file_id 内部分配
    uint64_t create_table_impl(const std::string& name, const std::vector<st::ColumnSpec>& cols,
                               uint64_t tid);
    // 写入指定表的元数据行(须持锁): db_table 一行, db_column 每列一行, 引导与建表共用
    void write_meta_rows(uint64_t sid, uint64_t tid, uint64_t fid, const std::string& name,
                         const std::vector<st::ColumnSpec>& cols);
    // 引导元数据表: 直接建数据文件并写入自描述行与 system schema 行, 不经过元数据表查找
    void bootstrap_meta_tables();
    // 删除指定表的元数据行(须持锁): 按 table_id 匹配 db_table/db_column
    void delete_meta_rows(uint64_t tid);
    // 按表名查元数据(须持锁): db_table 定位 id, db_column 收集列并按 ordinal 排序,
    // 表不存在或元数据行非法当场报错
    st::TableMeta find_table_meta(const std::string& name);
    // 表名是否已存在(须持锁): 全扫 db_table 匹配
    bool has_table_name(const std::string& name);
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
    std::mutex mutex_;   // 序列化所有复合操作(并发演化见存储设计文档 §10)
    std::atomic<uint64_t> next_file_id_{0};   // 下一个 file_id, open() 扫 db_table 取最大值+1 初始化
    std::atomic<uint64_t> next_table_id_{0};   // 下一个 table_id, open() 扫 db_table 取最大值+1 初始化, 不低于 kFirstUserTableId
    std::atomic<uint64_t> next_schema_id_{0};  // 下一个 schema_id, open() 扫 db_schema 取最大值+1 初始化
};

}  // namespace ct
#endif
