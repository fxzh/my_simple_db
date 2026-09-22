// engine.h: 文件引擎对外接口(M1: 堆页追加 + 全表扫描, 无索引), 公开原语全部须持锁(锁在 catalog)
#ifndef STORAGE_ENGINE_H
#define STORAGE_ENGINE_H

#include <string>
#include <unordered_map>
#include <vector>

#include "buffer_pool.h"
#include "file_manager.h"
#include "types.h"

namespace st {

class Engine;

// 全表扫描迭代器: 沿文件头页的 next_page 链读取数据页, 逐槽解码
class Scanner {
public:
    Scanner(Engine* engine, const TableMeta& meta);
    ~Scanner();

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    // 取下一行; 扫描结束或损坏返回 false
    bool next(Row* out);
    void close();

private:
    void advance_page();

    Engine* engine_;
    TableMeta meta_;      // 查表时的拷贝快照
    char* cur_data_ = nullptr;   // 当前 pin 的堆页
    PageId cur_page_ = INVALID_PAGE;
    uint16_t slot_ = 0;
    uint32_t next_page_no_ = 0;  // 待加载的数据页
    bool done_ = false;
};

// 文件引擎: 页/文件/缓冲池/堆, 目录(表名/列定义)不在本层
class Engine {
public:
    explicit Engine(std::string dir);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // 重置缓冲池与尾页跟踪, 进入打开状态
    void open();
    // 脏页与文件全部落盘
    void flush_all();
    // 落盘并关闭全部文件
    void close();
    bool table_file_exists(uint64_t tid) const;

    // 建表文件并初始化落盘文件头页
    void init_table_file(uint64_t tid);
    // 删表文件并清缓冲与尾页跟踪
    void remove_table_file(uint64_t tid);
    // 插行: 校验编码后追加, 用户插行与元数据表引导共用
    RowRef insert_row(uint64_t tid, const std::vector<ColumnSpec>& cols,
                      const std::vector<Value>& values);
    // 读取指定表全部存活行: 沿页链解码, 行损坏当场报错
    std::vector<std::vector<Value>> read_rows(uint64_t tid,
                                              const std::vector<ColumnSpec>& cols);
    // 删除单行(按物理位置), 无效/已删引用返回 0
    size_t delete_row(const RowRef& ref);
    // 清空指定表全部行, 返回删除行数
    size_t delete_all_rows(uint64_t tid);
    // 存活行数统计
    size_t row_count(uint64_t tid);

private:
    friend class Scanner;
    // 建首个数据页(页号 1)并链到文件头页, 返回新页号
    uint32_t link_header_to_first_data_page(uint64_t table_id);

    std::string dir_;
    FileManager files_;
    BufferPool pool_;
    std::unordered_map<uint64_t, uint32_t> tail_pages_;  // 表 -> 最高页号(含仅存内存的页)
    bool open_ = false;
};

}  // namespace st
#endif
