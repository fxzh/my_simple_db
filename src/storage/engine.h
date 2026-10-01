// engine.h: 文件引擎对外接口(堆页追加 + 全表扫描 + 二级索引原语), 公开原语全部须持锁(锁在 catalog)
#ifndef STORAGE_ENGINE_H
#define STORAGE_ENGINE_H

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "btree.h"
#include "buffer_pool.h"
#include "file_manager.h"
#include "types.h"
#include "wal.h"

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

// 文件引擎: 页/文件/缓冲池/堆/索引树, 目录(表名/列定义/索引定义)不在本层
class Engine {
public:
    explicit Engine(std::string dir);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // 重置缓冲池与尾页跟踪, 进入打开状态; 打开前先重放 WAL 完成崩溃恢复
    void open();
    // 检查点: 刷全部脏页并按 WAL 先于数据文件的顺序落盘, 然后清空日志,
    // 下次启动零重放; 干净关闭、目录初始化完成与运行期 WAL 达到阈值时调用
    void checkpoint();
    // 落盘并关闭全部文件, 收尾做检查点: 清空 WAL, 下次启动零重放
    void close();
    // 语句提交点: fsync WAL, 此前修改掉电不丢(由 session 层在回 Ok 前调用)
    void sync_wal();
    // WAL 自上次清空以来的累计写入字节数, 提交点的运行期检查点阈值判断用
    uint64_t wal_bytes_since_reset() const;
    bool table_file_exists(uint64_t fid) const;

    // 建表文件并初始化落盘文件头页
    void init_table_file(uint64_t fid);
    // 删表文件并清缓冲与尾页跟踪
    void remove_table_file(uint64_t fid);
    // 插行: 值合法性由调用方保证, 编码追加并分配 rowid, ref 输出新行物理位置, 用户插行与元数据表引导共用
    RowId insert_row(uint64_t fid, const std::vector<ColumnSpec>& cols,
                     const std::vector<Value>& values, RowRef* ref);
    // 读取指定表全部存活行: 沿页链解码, 行损坏当场报错
    std::vector<std::vector<Value>> read_rows(uint64_t fid,
                                              const std::vector<ColumnSpec>& cols);
    // 回表: 按行物理位置直读堆页取行, 已删/槽位越界返回 false, 无效引用与行损坏当场报错
    bool read_row(const RowRef& ref, const std::vector<ColumnSpec>& cols, Row* out);
    // 删除单行(按物理位置), 已删/槽位越界返回 0, 无效引用当场报错
    size_t delete_row(const RowRef& ref);
    // 清空指定表全部行, 返回删除行数
    size_t delete_all_rows(uint64_t fid);
    // 存活行数统计
    size_t row_count(uint64_t fid);

    // 建索引文件并初始化空树: 文件头页与空叶根落盘
    void init_index_file(uint64_t fid);
    // 删索引文件并清缓冲与树跟踪
    void remove_index_file(uint64_t fid);
    // 索引条目插入: (键, 行定位) 唯一性由调用方保证
    void index_insert(uint64_t fid, const IndexKey& key, const RowRef& ref);
    // 索引范围扫描: 定位 lo(缺省为最左) 后沿叶子链前进, hi 为排他上界(缺省为无上界);
    // 迭代器不持锁(仅持页 pin), 并发 DDL 期间扫描是未定义行为
    std::unique_ptr<BTreeScanner> index_scan(uint64_t fid, std::optional<IndexKey> lo,
                                             std::optional<IndexKey> hi);
    // 建索引回填: 全表扫描堆页, 逐行取指定列编码入树, 返回条目数
    size_t build_index(uint64_t table_fid, const std::vector<ColumnSpec>& cols, uint16_t ordinal,
                       uint64_t index_fid);

private:
    friend class Scanner;
    // 取指定索引的树: 未跟踪时打开已有索引文件
    BTree& tree_for(uint64_t fid);
    // 建首个数据页(页号 1)并链到文件头页, 返回新页号
    uint32_t link_header_to_first_data_page(uint64_t file_id);
    // 校验行引用页位置: 缺页位置/页 0/页号越界当场报错
    void check_row_ref(const RowRef& ref) const;

    std::string dir_;
    FileManager files_;
    Wal wal_;                                            // 预写日志, 须先于 pool_ 声明(池持其引用)
    BufferPool pool_;
    std::unordered_map<uint64_t, uint32_t> tail_pages_;  // 文件 -> 最高页号(含仅存内存的页)
    std::unordered_map<uint64_t, BTree> trees_;          // 索引文件 -> 树(根页号与页分配跟踪)
    bool open_ = false;
};

}  // namespace st
#endif
