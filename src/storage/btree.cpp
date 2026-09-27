// btree.cpp: B+ 树实现: 下探/插入/分裂/点查/叶子链扫描
#include "btree.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "common/err.h"
#include "log/log.h"

namespace st {

namespace {

// 单元格内键字节数
constexpr uint16_t CELL_KEY_SIZE = 8;

// 文件头页根页号: 存于页头之后(u32)
uint32_t file_root(const char* page)
{
    uint32_t v = 0;
    std::memcpy(&v, page + PAGE_HEADER_SIZE, sizeof(v));
    return v;
}

void set_file_root(char* page, uint32_t v)
{
    std::memcpy(page + PAGE_HEADER_SIZE, &v, sizeof(v));
}

// ==================== 叶子页访问 ====================

// 叶子槽目录: 页头后正向生长, 槽 i 即键序第 i 条
Slot* leaf_slot(char* page, uint16_t i)
{
    return reinterpret_cast<Slot*>(page + PAGE_HEADER_SIZE + static_cast<size_t>(i) * SLOT_SIZE);
}

const Slot* leaf_slot(const char* page, uint16_t i)
{
    return reinterpret_cast<const Slot*>(page + PAGE_HEADER_SIZE + static_cast<size_t>(i) * SLOT_SIZE);
}

// 槽 i 指向的单元格键
int64_t leaf_key(const char* page, uint16_t i)
{
    int64_t k = 0;
    std::memcpy(&k, page + leaf_slot(page, i)->off, sizeof(k));
    return k;
}

// 首个键 ≥ key 的槽位
uint16_t leaf_lower_bound(const char* page, int64_t key)
{
    uint16_t lo = 0;
    uint16_t hi = header(page)->slot_count;
    while (lo < hi) {
        const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
        if (leaf_key(page, mid) < key) {
            lo = static_cast<uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return lo;
}

// 槽位 pos 插入键与 payload: 槽目录腾位、单元格写入自由区尾, 不重算校验和(调用方收尾统一算)
void leaf_insert_at(char* page, uint16_t pos, int64_t key, const uint8_t* payload, uint16_t len)
{
    PageHeader* h = header(page);
    const uint16_t n = h->slot_count;
    std::memmove(leaf_slot(page, static_cast<uint16_t>(pos + 1)), leaf_slot(page, pos),
                 static_cast<size_t>(n - pos) * SLOT_SIZE);
    const uint16_t cell = static_cast<uint16_t>(CELL_KEY_SIZE + len);
    h->free_end = static_cast<uint16_t>(h->free_end - cell);
    std::memcpy(page + h->free_end, &key, sizeof(key));
    std::memcpy(page + h->free_end + sizeof(key), payload, len);
    Slot* s = leaf_slot(page, pos);
    s->off = h->free_end;
    s->len = cell;
    h->free_begin = static_cast<uint16_t>(h->free_begin + SLOT_SIZE);
    h->slot_count = static_cast<uint16_t>(n + 1);
}

// 整页重建叶子: 重置后顺序写入条目区间 [from, to)
void leaf_build(char* page, const std::vector<BTreeEntry>& entries, uint16_t from, uint16_t to)
{
    init_page(page, MAGIC_BTREE_LEAF, PageType::BTreeLeaf);
    for (uint16_t i = from; i < to; ++i) {
        const BTreeEntry& e = entries[static_cast<size_t>(i)];
        leaf_insert_at(page, static_cast<uint16_t>(i - from), e.key, e.payload.data(),
                       static_cast<uint16_t>(e.payload.size()));
    }
}

// ==================== 内节点页访问 ====================

// 内节点键: 页头后正向生长
int64_t node_key(const char* page, uint16_t i)
{
    int64_t k = 0;
    std::memcpy(&k, page + PAGE_HEADER_SIZE + static_cast<size_t>(i) * sizeof(int64_t), sizeof(k));
    return k;
}

void set_node_key(char* page, uint16_t i, int64_t key)
{
    std::memcpy(page + PAGE_HEADER_SIZE + static_cast<size_t>(i) * sizeof(int64_t), &key, sizeof(key));
}

// 内节点子指针: 页尾反向生长, 子 j 位于页尾 - 4*(j+1)
uint32_t node_child(const char* page, uint16_t j)
{
    uint32_t c = 0;
    std::memcpy(&c, page + PAGE_SIZE - static_cast<size_t>(j + 1) * sizeof(uint32_t), sizeof(c));
    return c;
}

void set_node_child(char* page, uint16_t j, uint32_t child)
{
    std::memcpy(page + PAGE_SIZE - static_cast<size_t>(j + 1) * sizeof(uint32_t), &child, sizeof(child));
}

// key 所属子树的下标: 首个键 > key 的位置
uint16_t node_child_index(const char* page, int64_t key)
{
    uint16_t lo = 0;
    uint16_t hi = header(page)->slot_count;
    while (lo < hi) {
        const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
        if (node_key(page, mid) <= key) {
            lo = static_cast<uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return lo;
}

// 键位 i 插入分隔键与右子(来自子 i 的分裂), 不重算校验和(调用方收尾统一算)
void node_insert_at(char* page, uint16_t i, int64_t sep, uint32_t right)
{
    PageHeader* h = header(page);
    const uint16_t n = h->slot_count;
    // 键 [i..n) 右移一格
    std::memmove(page + PAGE_HEADER_SIZE + static_cast<size_t>(i + 1) * sizeof(int64_t),
                 page + PAGE_HEADER_SIZE + static_cast<size_t>(i) * sizeof(int64_t),
                 static_cast<size_t>(n - i) * sizeof(int64_t));
    // 子 [i+1..n+1] 各下移一格
    std::memmove(page + PAGE_SIZE - static_cast<size_t>(n + 2) * sizeof(uint32_t),
                 page + PAGE_SIZE - static_cast<size_t>(n + 1) * sizeof(uint32_t),
                 static_cast<size_t>(n - i) * sizeof(uint32_t));
    set_node_key(page, i, sep);
    set_node_child(page, static_cast<uint16_t>(i + 1), right);
    h->free_begin = static_cast<uint16_t>(h->free_begin + sizeof(int64_t));
    h->free_end = static_cast<uint16_t>(h->free_end - sizeof(uint32_t));
    h->slot_count = static_cast<uint16_t>(n + 1);
}

// 整页重建内节点: 重置后写入键区间 [from, to) 与子区间 [from, to]
void node_build(char* page, const std::vector<int64_t>& keys, const std::vector<uint32_t>& children,
                uint16_t from, uint16_t to)
{
    init_page(page, MAGIC_BTREE_LEAF, PageType::BTreeInternal);
    for (uint16_t i = from; i < to; ++i) {
        set_node_key(page, static_cast<uint16_t>(i - from), keys[static_cast<size_t>(i)]);
        set_node_child(page, static_cast<uint16_t>(i - from), children[static_cast<size_t>(i)]);
    }
    set_node_child(page, static_cast<uint16_t>(to - from), children[static_cast<size_t>(to)]);
    PageHeader* h = header(page);
    const uint16_t m = static_cast<uint16_t>(to - from);
    h->free_begin = static_cast<uint16_t>(PAGE_HEADER_SIZE + static_cast<size_t>(m) * sizeof(int64_t));
    h->free_end = static_cast<uint16_t>(PAGE_SIZE - static_cast<size_t>(m + 1) * sizeof(uint32_t));
    h->slot_count = m;
}

// 页类型是否为树页
bool tree_page_type(uint8_t type)
{
    return type == static_cast<uint8_t>(PageType::BTreeLeaf)
           || type == static_cast<uint8_t>(PageType::BTreeInternal);
}

}  // namespace

// ==================== BTree ====================

BTree::BTree(BufferPool& p, FileManager& f, uint64_t fid) : pool(p), files(f), file_id(fid) {}

void BTree::create()
{
    files.create_table_file(file_id);
    const PageId pid0{file_id, 0};
    char* h0 = pool.allocate(pid0, files);
    init_page(h0, MAGIC_FILE_HEADER, PageType::FileHeader);
    char* root = pool.allocate(PageId{file_id, 1}, files);
    init_page(root, MAGIC_BTREE_LEAF, PageType::BTreeLeaf);
    set_file_root(h0, 1);
    header(h0)->checksum = page_checksum(h0);
    pool.mark_dirty(h0);
    pool.unpin(h0);
    pool.flush(pid0, files);
    pool.unpin(root);
    root_page = 1;
    next_page_hint = 2;
}

void BTree::open()
{
    if (!files.table_file_exists(file_id)) {
        DB_RAISE(db::ErrCode::CorruptData, LogModule::STORAGE, "B+树文件缺失: file={}", file_id);
    }
    const PageId pid0{file_id, 0};
    char* h0 = pool.read(pid0, MAGIC_FILE_HEADER, files);
    const uint32_t root = file_root(h0);
    pool.unpin(h0);
    if (root == 0) {
        DB_RAISE(db::ErrCode::CorruptData, LogModule::STORAGE, "B+树根页号无效: file={}", file_id);
    }
    root_page = root;
    next_page_hint = std::max(files.page_count(file_id), root + 1);
}

std::pair<uint32_t, char*> BTree::new_page()
{
    const uint32_t no = std::max(files.page_count(file_id), next_page_hint);
    char* pg = pool.allocate(PageId{file_id, no}, files);
    next_page_hint = no + 1;
    return {no, pg};
}

char* BTree::read_tree(const PageId& pid)
{
    char* pg = pool.read(pid, MAGIC_BTREE_LEAF, files);
    if (!tree_page_type(header(pg)->type)) {
        const uint8_t type = header(pg)->type;
        pool.unpin(pg);
        DB_RAISE(db::ErrCode::CorruptData, LogModule::STORAGE,
                 "B+树页类型损坏: file={} page={} type={}", pid.file_id, pid.page_no,
                 static_cast<unsigned>(type));
    }
    return pg;
}

void BTree::set_root(uint32_t no)
{
    const PageId pid0{file_id, 0};
    char* h0 = pool.read(pid0, MAGIC_FILE_HEADER, files);
    set_file_root(h0, no);
    header(h0)->checksum = page_checksum(h0);
    pool.mark_dirty(h0);
    pool.unpin(h0);
    root_page = no;
}

void BTree::insert(int64_t key, const uint8_t* payload, uint16_t len)
{
    if (len > BTREE_MAX_PAYLOAD) {
        DB_RAISE(db::ErrCode::RecordTooLong, LogModule::STORAGE, "B+树记录超长");
    }
    BTreeSplit r = insert_rec(root_page, key, payload, len);
    if (!r.split) {
        return;
    }
    // 根分裂: 新根内节点承载上提分隔键与新旧两子
    std::vector<int64_t> keys{r.sep};
    std::vector<uint32_t> children{root_page, r.right};
    auto [no, pg] = new_page();
    node_build(pg, keys, children, 0, 1);
    header(pg)->checksum = page_checksum(pg);
    pool.mark_dirty(pg);
    pool.unpin(pg);
    set_root(no);
}

bool BTree::lookup(int64_t key, std::vector<uint8_t>& out)
{
    uint32_t no = root_page;
    for (;;) {
        char* pg = read_tree(PageId{file_id, no});
        const PageHeader* h = header(pg);
        if (h->type == static_cast<uint8_t>(PageType::BTreeLeaf)) {
            const uint16_t pos = leaf_lower_bound(pg, key);
            const bool found = pos < h->slot_count && leaf_key(pg, pos) == key;
            if (found) {
                const Slot* s = leaf_slot(pg, pos);
                const uint8_t* cell = reinterpret_cast<const uint8_t*>(pg) + s->off;
                out.assign(cell + CELL_KEY_SIZE, cell + s->len);
            }
            pool.unpin(pg);
            return found;
        }
        no = node_child(pg, node_child_index(pg, key));
        pool.unpin(pg);
    }
}

BTreeSplit BTree::insert_rec(uint32_t page_no, int64_t key, const uint8_t* payload, uint16_t len)
{
    char* pg = read_tree(PageId{file_id, page_no});
    PageHeader* h = header(pg);

    // 叶子: 二分定位, 有空即插, 页满整页分裂
    if (h->type == static_cast<uint8_t>(PageType::BTreeLeaf)) {
        const uint16_t n = h->slot_count;
        const uint16_t pos = leaf_lower_bound(pg, key);
        if (pos < n && leaf_key(pg, pos) == key) {
            pool.unpin(pg);
            DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE, "B+树键重复: {}", key);
        }
        const uint32_t need = static_cast<uint32_t>(SLOT_SIZE) + CELL_KEY_SIZE + len;
        if (need <= free_space(pg)) {
            leaf_insert_at(pg, pos, key, payload, len);
            h->checksum = page_checksum(pg);
            pool.mark_dirty(pg);
            pool.unpin(pg);
            return {};
        }
        // 收集含新条目的全量, 以中点分裂, 左右两页整页重建
        std::vector<BTreeEntry> entries;
        entries.reserve(static_cast<size_t>(n) + 1);
        for (uint16_t i = 0; i < n; ++i) {
            const Slot* s = leaf_slot(pg, i);
            const uint8_t* cell = reinterpret_cast<const uint8_t*>(pg) + s->off;
            BTreeEntry e;
            e.key = leaf_key(pg, i);
            e.payload.assign(cell + CELL_KEY_SIZE, cell + s->len);
            entries.push_back(std::move(e));
        }
        entries.insert(entries.begin() + pos,
                       BTreeEntry{key, std::vector<uint8_t>(payload, payload + len)});
        const uint16_t cnt = static_cast<uint16_t>(entries.size());
        const uint16_t mid = static_cast<uint16_t>(cnt / 2);
        const uint32_t old_next = h->next_page;
        BTreeSplit res;
        res.split = true;
        res.sep = entries[static_cast<size_t>(mid)].key;

        auto [rno, rpg] = new_page();
        leaf_build(rpg, entries, mid, cnt);
        header(rpg)->next_page = old_next;
        header(rpg)->checksum = page_checksum(rpg);
        pool.mark_dirty(rpg);
        pool.unpin(rpg);

        leaf_build(pg, entries, 0, mid);
        header(pg)->next_page = rno;
        header(pg)->checksum = page_checksum(pg);
        pool.mark_dirty(pg);
        pool.unpin(pg);

        res.right = rno;
        return res;
    }

    // 内节点: 先递归子页, 子分裂时在本层落分隔键与右子
    const uint16_t j = node_child_index(pg, key);
    BTreeSplit r = insert_rec(node_child(pg, j), key, payload, len);
    if (!r.split) {
        pool.unpin(pg);
        return {};
    }
    const uint16_t n = h->slot_count;
    const uint32_t need = static_cast<uint32_t>(sizeof(int64_t)) + sizeof(uint32_t);
    if (need <= free_space(pg)) {
        node_insert_at(pg, j, r.sep, r.right);
        h->checksum = page_checksum(pg);
        pool.mark_dirty(pg);
        pool.unpin(pg);
        return {};
    }
    // 收集含新键/新子的全量, 以中点分裂, 分隔键上提不驻留子页两侧
    std::vector<int64_t> keys;
    std::vector<uint32_t> children;
    keys.reserve(static_cast<size_t>(n) + 1);
    children.reserve(static_cast<size_t>(n) + 2);
    for (uint16_t i = 0; i < j; ++i) {
        keys.push_back(node_key(pg, i));
    }
    keys.push_back(r.sep);
    for (uint16_t i = j; i < n; ++i) {
        keys.push_back(node_key(pg, i));
    }
    for (uint16_t c = 0; c <= j; ++c) {
        children.push_back(node_child(pg, c));
    }
    children.push_back(r.right);
    for (uint16_t c = static_cast<uint16_t>(j + 1); c <= n; ++c) {
        children.push_back(node_child(pg, c));
    }
    const uint16_t cnt = static_cast<uint16_t>(keys.size());
    const uint16_t mid = static_cast<uint16_t>(cnt / 2);
    BTreeSplit res;
    res.split = true;
    res.sep = keys[static_cast<size_t>(mid)];

    auto [rno, rpg] = new_page();
    node_build(rpg, keys, children, static_cast<uint16_t>(mid + 1), cnt);
    header(rpg)->checksum = page_checksum(rpg);
    pool.mark_dirty(rpg);
    pool.unpin(rpg);

    node_build(pg, keys, children, 0, mid);
    header(pg)->checksum = page_checksum(pg);
    pool.mark_dirty(pg);
    pool.unpin(pg);

    res.right = rno;
    return res;
}

// ==================== BTreeScanner ====================

BTreeScanner::BTreeScanner(BufferPool& p, FileManager& f, uint64_t fid, uint32_t root)
        : pool(p), files(f), file_id(fid)
{
    // 从根沿最左子指针下探到最左叶子
    uint32_t no = root;
    char* pg = pool.read(PageId{file_id, no}, MAGIC_BTREE_LEAF, files);
    while (header(pg)->type == static_cast<uint8_t>(PageType::BTreeInternal)) {
        no = node_child(pg, 0);
        pool.unpin(pg);
        pg = pool.read(PageId{file_id, no}, MAGIC_BTREE_LEAF, files);
    }
    if (header(pg)->type != static_cast<uint8_t>(PageType::BTreeLeaf)) {
        pool.unpin(pg);
        DB_RAISE(db::ErrCode::CorruptData, LogModule::STORAGE,
                 "B+树页类型损坏: file={} page={}", file_id, no);
    }
    cur = pg;
    cur_page = PageId{file_id, no};
    next_no = header(pg)->next_page;
}

BTreeScanner::~BTreeScanner()
{
    close();
}

void BTreeScanner::close()
{
    if (cur != nullptr) {
        pool.unpin(cur);
        cur = nullptr;
    }
    done = true;
}

bool BTreeScanner::next(BTreeEntry* out)
{
    if (done) {
        return false;
    }
    for (;;) {
        if (cur == nullptr) {
            // 加载链上下一页
            if (next_no == 0) {
                done = true;
                return false;
            }
            const uint32_t no = next_no;
            cur = pool.read(PageId{file_id, no}, MAGIC_BTREE_LEAF, files);
            if (header(cur)->type != static_cast<uint8_t>(PageType::BTreeLeaf)) {
                pool.unpin(cur);
                cur = nullptr;
                DB_RAISE(db::ErrCode::CorruptData, LogModule::STORAGE,
                         "B+树页类型损坏: file={} page={}", file_id, no);
            }
            cur_page = PageId{file_id, no};
            next_no = header(cur)->next_page;
            slot_idx = 0;
        }
        const PageHeader* h = header(cur);
        if (slot_idx < h->slot_count) {
            const Slot* s = leaf_slot(cur, slot_idx);
            const uint8_t* cell = reinterpret_cast<const uint8_t*>(cur) + s->off;
            out->key = leaf_key(cur, slot_idx);
            out->payload.assign(cell + CELL_KEY_SIZE, cell + s->len);
            ++slot_idx;
            return true;
        }
        pool.unpin(cur);
        cur = nullptr;
    }
}

}  // namespace st
