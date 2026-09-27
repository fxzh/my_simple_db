// btree.h: B+ 树: 键 int64 升序去重, payload 变长记录字节, 数据全在叶子, 叶子经 next_page 单向成链
#ifndef STORAGE_BTREE_H
#define STORAGE_BTREE_H

#include <cstdint>
#include <utility>
#include <vector>

#include "buffer_pool.h"
#include "file_manager.h"
#include "page.h"
#include "types.h"

namespace st {

// 叶子单条 payload 上限: 保证任意两条记录总能同存一页, 页满分裂必然可行
constexpr uint16_t BTREE_MAX_PAYLOAD = 2024;

// 叶子条目: 键 + payload 字节
struct BTreeEntry {
    int64_t key = 0;
    std::vector<uint8_t> payload;
};

// 子页分裂结果: split 为 false 表示未分裂
struct BTreeSplit {
    bool split = false;
    int64_t sep = 0;     // 上提分隔键
    uint32_t right = 0;  // 新右页页号
};

// B+ 树: 每文件一棵, 页 0 为文件头页存根页号; 公开方法须持锁调用(锁在 catalog)
// 树页统一魔数 MAGIC_BTREE_LEAF, 页 type 字段区分叶/内节点(MAGIC_BTREE_INTERNAL 预留未用)
// 页内布局(页头 24B 复用 PageHeader, free_begin/free_end 为两侧生长 frontier):
//   叶子: [页头][槽目录正向生长][自由区][单元格反向生长]
//         单元格 = [键 8B][payload], 槽指向单元格, 槽序即键序
//   内节点: [页头][键数组正向生长][自由区][子指针数组反向生长]
//         键 k_0..k_{n-1} 升序, 子 c_0..c_n: c_0 子树 < k_0, c_i 子树 ∈ [k_{i-1}, k_i), c_n 子树 ≥ k_{n-1}
//   slot_count 叶子记槽数, 内节点记键数(子数 = 键数 + 1)
struct BTree {
    BufferPool& pool;
    FileManager& files;
    const uint64_t file_id;
    uint32_t root_page = 0;       // 当前根页号, 根分裂时同步写文件头页
    uint32_t next_page_hint = 0;  // 已分配最高页号 + 1(含未落盘页)

    BTree(BufferPool& p, FileManager& f, uint64_t fid);

    // 新建树文件: 文件头页 + 空叶根(页 1)
    void create();
    // 打开已有树文件, 读根页号; 文件缺失或根页号无效当场报错
    void open();

    // 插入: 键重复或 payload 超限当场报错
    void insert(int64_t key, const uint8_t* payload, uint16_t len);
    // 点查: 命中填充 payload 并返回 true
    bool lookup(int64_t key, std::vector<uint8_t>& out);

    // 递归插入, 返回本层分裂结果
    BTreeSplit insert_rec(uint32_t page_no, int64_t key, const uint8_t* payload, uint16_t len);
    // 分配新树页(pin 住返回, 调用方负责初始化与 unpin)
    std::pair<uint32_t, char*> new_page();
    // 读树页并校验页类型, pin 住返回
    char* read_tree(const PageId& pid);
    // 根页号写入文件头页
    void set_root(uint32_t no);
};

// B+ 树顺序扫描: 从最左叶子沿 next_page 前进
struct BTreeScanner {
    BTreeScanner(BufferPool& p, FileManager& f, uint64_t fid, uint32_t root);
    ~BTreeScanner();

    BTreeScanner(const BTreeScanner&) = delete;
    BTreeScanner& operator=(const BTreeScanner&) = delete;

    // 取下一叶子条目, 扫描结束返回 false; 页损坏当场报错
    bool next(BTreeEntry* out);
    void close();

    BufferPool& pool;
    FileManager& files;
    const uint64_t file_id;
    char* cur = nullptr;     // 当前 pin 的叶子页
    PageId cur_page = INVALID_PAGE;
    uint32_t next_no = 0;    // 待加载的叶子页号, 0 表示链尾
    uint16_t slot_idx = 0;   // 当前叶子内的槽下标
    bool done = false;
};

}  // namespace st

#endif  // STORAGE_BTREE_H
