// storage.h: 存储引擎对外接口(M1: 堆页追加 + 全表扫描, 无索引)
#ifndef STORAGE_H
#define STORAGE_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "buffer_pool.h"
#include "catalog.h"
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
    TableMeta meta_;      // 拷贝, 避免目录内指针失效
    char* cur_data_ = nullptr;   // 当前 pin 的堆页
    PageId cur_page_ = INVALID_PAGE;
    uint16_t slot_ = 0;
    uint32_t next_page_no_ = 0;  // 待加载的数据页
    bool done_ = false;
};

// 数据目录门面: 打开/关闭, 建表/删表/插入/删除/全表扫描
class Database {
public:
    explicit Database(std::string dir);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 打开数据目录(不存在则创建)并加载目录
    void open();
    // 刷盘并关闭
    void close();

    uint32_t create_table(const std::string& name, const std::vector<ColumnSpec>& cols);
    void drop_table(const std::string& name);
    RowRef insert(const std::string& table, const std::vector<Value>& values);
    // 删除单行(按扫描得到的物理位置), 无效/已删引用返回 0
    size_t delete_by_ref(const RowRef& ref);
    // 删除表中全部行, 返回删除行数
    size_t delete_all(const std::string& table);

    std::unique_ptr<Scanner> scan(const std::string& table);
    // 存活行数统计(便利函数, 供测试与将来执行层使用)
    size_t row_count(const std::string& table);

private:
    friend class Scanner;
    const TableMeta& get_table(const std::string& name) const;
    // 建首个数据页(页号 1)并链到文件头页, 返回新页号
    uint32_t link_header_to_first_data_page(uint32_t table_id);

    std::string dir_;
    Catalog catalog_;
    FileManager files_;
    BufferPool pool_;
    std::unordered_map<uint32_t, uint32_t> tail_pages_;  // 表 -> 最高页号(含仅存内存的页)
    std::mutex mutex_;   // 序列化所有操作(并发演化见设计文档 §10)
    bool open_ = false;
};

}  // namespace st
#endif