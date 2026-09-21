// storage.h: 存储引擎对外接口(M1: 堆页追加 + 全表扫描, 无索引)
#ifndef STORAGE_H
#define STORAGE_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "buffer_pool.h"
#include "file_manager.h"
#include "types.h"

namespace st {

class Database;

// 全表扫描迭代器: 沿文件头页的 next_page 链读取数据页, 逐槽解码
class Scanner {
public:
    Scanner(Database* db, const TableMeta& meta);
    ~Scanner();

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    // 取下一行; 扫描结束或损坏返回 false
    bool next(Row* out);
    void close();

private:
    void advance_page();

    Database* db_;
    TableMeta meta_;      // 查表时的拷贝快照
    char* cur_data_ = nullptr;   // 当前 pin 的堆页
    PageId cur_page_ = INVALID_PAGE;
    uint16_t slot_ = 0;
    uint32_t next_page_no_ = 0;  // 待加载的数据页
    bool done_ = false;
};

// 数据目录门面: 打开/关闭, 建表/删表/插入/删除/全表扫描
// 元数据以 db_table/db_column 两张表为唯一事实来源, 查找实时扫描, 无目录文件与内存缓存
class Database {
public:
    explicit Database(std::string dir);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 初始化数据目录: 引导两张元数据表, 目录须已存在且未初始化, 不进入打开状态
    void create();
    // 打开已初始化的数据目录(以元数据表文件存在为准), 元数据表缺失当场报错
    void open();
    // 刷盘并关闭
    void close();

    uint64_t create_table(const std::string& name, const std::vector<ColumnSpec>& cols);
    // 删表, 保留段表拒绝删除
    void drop_table(const std::string& name);
    RowRef insert(const std::string& table, const std::vector<Value>& values);
    // 删除单行(按扫描得到的物理位置), 无效/已删引用返回 0
    size_t delete_by_ref(const RowRef& ref);
    // 删除表中全部行, 返回删除行数
    size_t delete_all(const std::string& table);

    std::unique_ptr<Scanner> scan(const std::string& table);
    // 存活行数统计(便利函数, 供测试与将来执行层使用)
    size_t row_count(const std::string& table);
    // 按表名取表元数据(实时扫描元数据表), 表不存在当场报错
    TableMeta table_meta(const std::string& name);

private:
    friend class Scanner;
    // 建首个数据页(页号 1)并链到文件头页, 返回新页号
    uint32_t link_header_to_first_data_page(uint64_t table_id);
    // 建表公共路径(须持锁): 校验后按指定 id 建数据文件、写元数据行
    uint64_t create_table_impl(const std::string& name, const std::vector<ColumnSpec>& cols,
                               uint64_t tid);
    // 物理建表(须持锁): 建数据文件并初始化落盘文件头页
    void init_table_file(uint64_t tid);
    // 插行公共路径(须持锁): 校验编码后追加, 用户插行与元数据表引导共用
    RowRef insert_impl(uint64_t table_id, const std::vector<ColumnSpec>& cols,
                       const std::vector<Value>& values);
    // 写入指定表的元数据行(须持锁): db_table 一行, db_column 每列一行, 引导与建表共用
    void write_meta_rows(uint64_t tid, const std::string& name,
                         const std::vector<ColumnSpec>& cols);
    // 引导元数据表: 直接建数据文件并写入自描述行, 不经过元数据表查找
    void bootstrap_meta_tables();
    // 删除指定表的元数据行(须持锁): 按 table_id 匹配 db_table/db_column
    void delete_meta_rows(uint64_t tid);
    // 读取指定表全部存活行(须持锁): 沿页链解码, 行损坏当场报错
    std::vector<std::vector<Value>> read_rows(uint64_t table_id,
                                              const std::vector<ColumnSpec>& cols);
    // 按表名查元数据(须持锁): db_table 定位 id, db_column 收集列并按 ordinal 排序,
    // 表不存在或元数据行非法当场报错
    TableMeta find_table_meta(const std::string& name);
    // 表名是否已存在(须持锁): 全扫 db_table 匹配
    bool has_table_name(const std::string& name);
    // table_id 是否已存在(须持锁): 全扫 db_table 匹配
    bool table_id_exists(uint64_t table_id);
    // 用户段分配(须持锁): max(当前最大表 id + 1, kFirstUserTableId)
    uint64_t alloc_table_id();

    std::string dir_;
    FileManager files_;
    BufferPool pool_;
    std::unordered_map<uint64_t, uint32_t> tail_pages_;  // 表 -> 最高页号(含仅存内存的页)
    std::mutex mutex_;   // 序列化所有操作(并发演化见设计文档 §10)
    bool open_ = false;
};

}  // namespace st
#endif