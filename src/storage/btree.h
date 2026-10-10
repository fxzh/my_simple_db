// btree.h: B+ 树(二级索引): 单元格 = [键值 8B][NULL 标志 1B][行定位 6B], 复合全序(NULL 最大), 数据全在叶子, 叶子经 next_page 单向成链
#ifndef STORAGE_BTREE_H
#define STORAGE_BTREE_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "buffer_pool.h"
#include "file_manager.h"
#include "page.h"
#include "types.h"

namespace st {

// 索引键: 值编码 + NULL 标志, 全序为值序且 NULL 排最大
struct IndexKey {
    uint64_t val = 0;   // 序保持编码值
    bool null = false;  // NULL 键标志
};

// 键序保持编码: 索引列值 → IndexKey, 编码由列类型驱动(赋值家族宽化使值的语义类型可与列类型不同)
// int/bigint 最高符号位翻转; float/double 按 IEEE754 位翻转(-0 归一为 +0), float 列先窄化到
// 单精度使各写入路径键一致; bool 编码 0/1
// NULL 入索引且排最大(PG 行为); char/varchar 变长键暂不支持, 当场报错
IndexKey encode_key(ColType type, const Value& v);

// 叶子单元格与内节点分隔项定长: [键值 8B][NULL 标志 1B][堆页号 4B][槽 2B]
constexpr uint16_t BTREE_CELL_SIZE = 15;

// 叶子条目: 编码键 + 行物理位置
struct BTreeEntry {
    IndexKey key;          // encode_key 产出的键, 键序 = 值序且 NULL 最大
    uint32_t page_no = 0;  // 堆数据页页号
    uint16_t slot = 0;     // 堆页槽位
};

// 子页分裂结果: split 为 false 表示未分裂
struct BTreeSplit {
    bool split = false;
    BTreeEntry sep;      // 上提分隔条目(右页首条目)
    uint32_t right = 0;  // 新右页页号
};

// B+ 树: 每文件一棵, 页 0 为文件头页存根页号; 公开方法须持锁调用(锁在 catalog)
// 树页统一魔数 MAGIC_BTREE_LEAF, 页 type 字段区分叶/内节点(MAGIC_BTREE_INTERNAL 预留未用)
// 页内布局(页头 24B 复用 PageHeader, free_begin/free_end 为两侧生长 frontier):
//   叶子: [页头][槽目录正向生长][自由区][单元格反向生长]
//         单元格 = [键值 8B][NULL 标志 1B][行定位 6B], 槽指向单元格, 槽序即 (键, 行定位) 复合序
//   内节点: [页头][分隔项数组正向生长][自由区][子指针数组反向生长]
//         分隔项 k_0..k_{n-1} 复合序升序, 子 c_0..c_n: c_0 子树 < k_0, c_i 子树 ∈ [k_{i-1}, k_i),
//         c_n 子树 ≥ k_{n-1}
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

    // 插入条目: (键, 行定位) 由调用方保证全局唯一, 无键冲突
    void insert(const BTreeEntry& e);

    // 递归插入, 返回本层分裂结果
    BTreeSplit insert_rec(uint32_t page_no, const BTreeEntry& e);
    // 分配新树页(pin 住返回, 调用方负责初始化与 unpin)
    std::pair<uint32_t, char*> new_page();
    // 读树页并校验页类型, pin 住返回
    char* read_tree(const PageId& pid);
    // 根页号写入文件头页
    void set_root(uint32_t no);
};

// B+ 树范围扫描: 定位到键下界后沿叶子链前进, 越过排他上界即止
struct BTreeScanner {
    // 从根下探定位 lo(缺省为最左叶子), hi 为排他上界键(缺省为无上界), NULL 键可作界
    BTreeScanner(BTree& tree, std::optional<IndexKey> lo, std::optional<IndexKey> hi);
    ~BTreeScanner();

    BTreeScanner(const BTreeScanner&) = delete;
    BTreeScanner& operator=(const BTreeScanner&) = delete;

    // 取下一叶子条目, 扫描结束返回 false; 页损坏当场报错
    bool next(BTreeEntry* out);
    void close();

    BufferPool& pool;
    FileManager& files;
    const uint64_t file_id;
    std::optional<IndexKey> hi_key;  // 排他上界键, nullopt 表示无上界
    char* cur = nullptr;     // 当前 pin 的叶子页
    PageId cur_page = INVALID_PAGE;
    uint32_t next_no = 0;    // 待加载的叶子页号, 0 表示链尾
    uint16_t slot_idx = 0;   // 当前叶子内的槽下标
    bool done = false;
};

}  // namespace st

#endif  // STORAGE_BTREE_H
