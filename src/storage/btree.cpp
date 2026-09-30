// btree.cpp: B+ 树实现: 下探/插入/分裂/键编码/范围扫描
#include "btree.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "common/err.h"
#include "log/log.h"

namespace st {

namespace {

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

// 键三路比较: 非 NULL 按值序, NULL 排最大, 负/零/正 对应 小于/等于/大于
int key_cmp(const IndexKey& a, const IndexKey& b)
{
    if (a.null != b.null) {
        return a.null ? 1 : -1;
    }
    if (a.val != b.val) {
        return a.val < b.val ? -1 : 1;
    }
    return 0;
}

// 复合序三路比较: 先键(NULL 最大), 同键按行定位(页号, 槽), 负/零/正 对应 小于/等于/大于
int entry_cmp(const BTreeEntry& a, const BTreeEntry& b)
{
    if (const int c = key_cmp(a.key, b.key); c != 0) {
        return c;
    }
    if (a.page_no != b.page_no) {
        return a.page_no < b.page_no ? -1 : 1;
    }
    if (a.slot != b.slot) {
        return a.slot < b.slot ? -1 : 1;
    }
    return 0;
}

// 单元格字段偏移: [键值 8B][NULL 标志 1B][页号 4B][槽 2B](与 IndexKey 内存布局解耦)
constexpr uint16_t CELL_KEY_OFF = 0;
constexpr uint16_t CELL_NULL_OFF = sizeof(uint64_t);
constexpr uint16_t CELL_PAGE_OFF = CELL_NULL_OFF + sizeof(uint8_t);
constexpr uint16_t CELL_SLOT_OFF = CELL_PAGE_OFF + sizeof(uint32_t);

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

// 槽 i 指向的单元格条目
BTreeEntry leaf_entry(const char* page, uint16_t i)
{
    BTreeEntry e;
    const char* cell = page + leaf_slot(page, i)->off;
    std::memcpy(&e.key.val, cell + CELL_KEY_OFF, sizeof(e.key.val));
    std::memcpy(&e.key.null, cell + CELL_NULL_OFF, sizeof(e.key.null));
    std::memcpy(&e.page_no, cell + CELL_PAGE_OFF, sizeof(e.page_no));
    std::memcpy(&e.slot, cell + CELL_SLOT_OFF, sizeof(e.slot));
    return e;
}

// 首个条目 ≥ e 的槽位
uint16_t leaf_lower_bound(const char* page, const BTreeEntry& e)
{
    uint16_t lo = 0;
    uint16_t hi = header(page)->slot_count;
    while (lo < hi) {
        const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
        if (entry_cmp(leaf_entry(page, mid), e) < 0) {
            lo = static_cast<uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return lo;
}

// 槽位 pos 插入条目: 槽目录腾位、单元格写入自由区尾, 不重算校验和(调用方收尾统一算)
void leaf_insert_at(char* page, uint16_t pos, const BTreeEntry& e)
{
    PageHeader* h = header(page);
    const uint16_t n = h->slot_count;
    std::memmove(leaf_slot(page, static_cast<uint16_t>(pos + 1)), leaf_slot(page, pos),
                 static_cast<size_t>(n - pos) * SLOT_SIZE);
    h->free_end = static_cast<uint16_t>(h->free_end - BTREE_CELL_SIZE);
    char* cell = page + h->free_end;
    std::memcpy(cell + CELL_KEY_OFF, &e.key.val, sizeof(e.key.val));
    std::memcpy(cell + CELL_NULL_OFF, &e.key.null, sizeof(e.key.null));
    std::memcpy(cell + CELL_PAGE_OFF, &e.page_no, sizeof(e.page_no));
    std::memcpy(cell + CELL_SLOT_OFF, &e.slot, sizeof(e.slot));
    Slot* s = leaf_slot(page, pos);
    s->off = h->free_end;
    s->len = BTREE_CELL_SIZE;
    h->free_begin = static_cast<uint16_t>(h->free_begin + SLOT_SIZE);
    h->slot_count = static_cast<uint16_t>(n + 1);
}

// 整页重建叶子: 重置后顺序写入条目区间 [from, to)
void leaf_build(char* page, const std::vector<BTreeEntry>& entries, uint16_t from, uint16_t to)
{
    init_page(page, MAGIC_BTREE_LEAF, PageType::BTreeLeaf);
    for (uint16_t i = from; i < to; ++i) {
        leaf_insert_at(page, static_cast<uint16_t>(i - from), entries[static_cast<size_t>(i)]);
    }
}

// ==================== 内节点页访问 ====================

// 内节点分隔项: 页头后正向生长, 步长 BTREE_CELL_SIZE
BTreeEntry node_sep(const char* page, uint16_t i)
{
    BTreeEntry e;
    const char* p = page + PAGE_HEADER_SIZE + static_cast<size_t>(i) * BTREE_CELL_SIZE;
    std::memcpy(&e.key.val, p + CELL_KEY_OFF, sizeof(e.key.val));
    std::memcpy(&e.key.null, p + CELL_NULL_OFF, sizeof(e.key.null));
    std::memcpy(&e.page_no, p + CELL_PAGE_OFF, sizeof(e.page_no));
    std::memcpy(&e.slot, p + CELL_SLOT_OFF, sizeof(e.slot));
    return e;
}

void set_node_sep(char* page, uint16_t i, const BTreeEntry& e)
{
    char* p = page + PAGE_HEADER_SIZE + static_cast<size_t>(i) * BTREE_CELL_SIZE;
    std::memcpy(p + CELL_KEY_OFF, &e.key.val, sizeof(e.key.val));
    std::memcpy(p + CELL_NULL_OFF, &e.key.null, sizeof(e.key.null));
    std::memcpy(p + CELL_PAGE_OFF, &e.page_no, sizeof(e.page_no));
    std::memcpy(p + CELL_SLOT_OFF, &e.slot, sizeof(e.slot));
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

// 条目所属子树的下标: 首个分隔项 > e 的位置
uint16_t node_child_index(const char* page, const BTreeEntry& e)
{
    uint16_t lo = 0;
    uint16_t hi = header(page)->slot_count;
    while (lo < hi) {
        const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
        if (entry_cmp(node_sep(page, mid), e) <= 0) {
            lo = static_cast<uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return lo;
}

// 键位 i 插入分隔项与右子(来自子 i 的分裂), 不重算校验和(调用方收尾统一算)
void node_insert_at(char* page, uint16_t i, const BTreeEntry& sep, uint32_t right)
{
    PageHeader* h = header(page);
    const uint16_t n = h->slot_count;
    // 分隔项 [i..n) 右移一格
    std::memmove(page + PAGE_HEADER_SIZE + static_cast<size_t>(i + 1) * BTREE_CELL_SIZE,
                 page + PAGE_HEADER_SIZE + static_cast<size_t>(i) * BTREE_CELL_SIZE,
                 static_cast<size_t>(n - i) * BTREE_CELL_SIZE);
    // 子 [i+1..n+1] 各下移一格
    std::memmove(page + PAGE_SIZE - static_cast<size_t>(n + 2) * sizeof(uint32_t),
                 page + PAGE_SIZE - static_cast<size_t>(n + 1) * sizeof(uint32_t),
                 static_cast<size_t>(n - i) * sizeof(uint32_t));
    set_node_sep(page, i, sep);
    set_node_child(page, static_cast<uint16_t>(i + 1), right);
    h->free_begin = static_cast<uint16_t>(h->free_begin + BTREE_CELL_SIZE);
    h->free_end = static_cast<uint16_t>(h->free_end - sizeof(uint32_t));
    h->slot_count = static_cast<uint16_t>(n + 1);
}

// 整页重建内节点: 重置后写入分隔项区间 [from, to) 与子区间 [from, to]
void node_build(char* page, const std::vector<BTreeEntry>& seps, const std::vector<uint32_t>& children,
                uint16_t from, uint16_t to)
{
    init_page(page, MAGIC_BTREE_LEAF, PageType::BTreeInternal);
    for (uint16_t i = from; i < to; ++i) {
        set_node_sep(page, static_cast<uint16_t>(i - from), seps[static_cast<size_t>(i)]);
        set_node_child(page, static_cast<uint16_t>(i - from), children[static_cast<size_t>(i)]);
    }
    set_node_child(page, static_cast<uint16_t>(to - from), children[static_cast<size_t>(to)]);
    PageHeader* h = header(page);
    const uint16_t m = static_cast<uint16_t>(to - from);
    h->free_begin = static_cast<uint16_t>(PAGE_HEADER_SIZE + static_cast<size_t>(m) * BTREE_CELL_SIZE);
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

// ==================== 键编码 ====================

IndexKey encode_key(ColType type, const Value& v)
{
    constexpr uint64_t sign_flip = 0x8000'0000'0000'0000ULL;
    if (std::holds_alternative<std::monostate>(v)) {
        // NULL 入索引且排最大, 键值部分无意义置 0
        return IndexKey{0, true};
    }
    if (const int64_t* i = std::get_if<int64_t>(&v)) {
        if (type != ColType::Int && type != ColType::BigInt) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "索引键值与列类型不匹配");
        }
        // 最高符号位翻转, 无符号序即有符号数值序
        return IndexKey{static_cast<uint64_t>(*i) ^ sign_flip, false};
    }
    if (const double* d = std::get_if<double>(&v)) {
        if (type != ColType::Double && type != ColType::Float) {
            DB_RAISE(db::ErrCode::ValueMismatch, LogModule::STORAGE, "索引键值与列类型不匹配");
        }
        // -0.0 归一为 +0.0, 两零编码一致
        const double x = (*d == 0.0) ? 0.0 : *d;
        uint64_t bits = 0;
        std::memcpy(&bits, &x, sizeof(bits));
        // IEEE754: 负数按位取反、正数翻符号位, 无符号序即浮点全序
        return IndexKey{(bits >> 63) ? ~bits : (bits | sign_flip), false};
    }
    DB_RAISE(db::ErrCode::NotImplemented, LogModule::STORAGE, "索引键类型未支持(变长键)");
}

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
    pool.mark_dirty(root);
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

void BTree::insert(const BTreeEntry& e)
{
    BTreeSplit r = insert_rec(root_page, e);
    if (!r.split) {
        return;
    }
    // 根分裂: 新根内节点承载上提分隔条目与新旧两子
    std::vector<BTreeEntry> seps{r.sep};
    std::vector<uint32_t> children{root_page, r.right};
    auto [no, pg] = new_page();
    node_build(pg, seps, children, 0, 1);
    header(pg)->checksum = page_checksum(pg);
    pool.mark_dirty(pg);
    pool.unpin(pg);
    set_root(no);
}

BTreeSplit BTree::insert_rec(uint32_t page_no, const BTreeEntry& e)
{
    char* pg = read_tree(PageId{file_id, page_no});
    PageHeader* h = header(pg);

    // 叶子: 二分定位, 有空即插, 页满整页分裂
    if (h->type == static_cast<uint8_t>(PageType::BTreeLeaf)) {
        const uint16_t n = h->slot_count;
        const uint16_t pos = leaf_lower_bound(pg, e);
        const uint32_t need = static_cast<uint32_t>(SLOT_SIZE) + BTREE_CELL_SIZE;
        if (need <= free_space(pg)) {
            leaf_insert_at(pg, pos, e);
            h->checksum = page_checksum(pg);
            pool.mark_dirty(pg);
            pool.unpin(pg);
            return {};
        }
        // 收集含新条目的全量, 以中点分裂, 左右两页整页重建
        std::vector<BTreeEntry> entries;
        entries.reserve(static_cast<size_t>(n) + 1);
        for (uint16_t i = 0; i < n; ++i) {
            entries.push_back(leaf_entry(pg, i));
        }
        entries.insert(entries.begin() + pos, e);
        const uint16_t cnt = static_cast<uint16_t>(entries.size());
        const uint16_t mid = static_cast<uint16_t>(cnt / 2);
        const uint32_t old_next = h->next_page;
        BTreeSplit res;
        res.split = true;
        res.sep = entries[static_cast<size_t>(mid)];

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

    // 内节点: 先递归子页, 子分裂时在本层落分隔项与右子
    const uint16_t j = node_child_index(pg, e);
    BTreeSplit r = insert_rec(node_child(pg, j), e);
    if (!r.split) {
        pool.unpin(pg);
        return {};
    }
    const uint16_t n = h->slot_count;
    const uint32_t need = static_cast<uint32_t>(BTREE_CELL_SIZE) + sizeof(uint32_t);
    if (need <= free_space(pg)) {
        node_insert_at(pg, j, r.sep, r.right);
        h->checksum = page_checksum(pg);
        pool.mark_dirty(pg);
        pool.unpin(pg);
        return {};
    }
    // 收集含新分隔项/新子的全量, 以中点分裂, 分隔项上提不驻留子页两侧
    std::vector<BTreeEntry> seps;
    std::vector<uint32_t> children;
    seps.reserve(static_cast<size_t>(n) + 1);
    children.reserve(static_cast<size_t>(n) + 2);
    for (uint16_t i = 0; i < j; ++i) {
        seps.push_back(node_sep(pg, i));
    }
    seps.push_back(r.sep);
    for (uint16_t i = j; i < n; ++i) {
        seps.push_back(node_sep(pg, i));
    }
    for (uint16_t c = 0; c <= j; ++c) {
        children.push_back(node_child(pg, c));
    }
    children.push_back(r.right);
    for (uint16_t c = static_cast<uint16_t>(j + 1); c <= n; ++c) {
        children.push_back(node_child(pg, c));
    }
    const uint16_t cnt = static_cast<uint16_t>(seps.size());
    const uint16_t mid = static_cast<uint16_t>(cnt / 2);
    BTreeSplit res;
    res.split = true;
    res.sep = seps[static_cast<size_t>(mid)];

    auto [rno, rpg] = new_page();
    node_build(rpg, seps, children, static_cast<uint16_t>(mid + 1), cnt);
    header(rpg)->checksum = page_checksum(rpg);
    pool.mark_dirty(rpg);
    pool.unpin(rpg);

    node_build(pg, seps, children, 0, mid);
    header(pg)->checksum = page_checksum(pg);
    pool.mark_dirty(pg);
    pool.unpin(pg);

    res.right = rno;
    return res;
}

// ==================== BTreeScanner ====================

BTreeScanner::BTreeScanner(BTree& tree, std::optional<IndexKey> lo, std::optional<IndexKey> hi)
        : pool(tree.pool), files(tree.files), file_id(tree.file_id), hi_key(hi)
{
    // 无下界沿最左子指针下探; 有下界按 (键, 最小行定位) 复合序路由
    const BTreeEntry probe{lo.value_or(IndexKey{}), 0, 0};
    uint32_t no = tree.root_page;
    char* pg = pool.read(PageId{file_id, no}, MAGIC_BTREE_LEAF, files);
    while (header(pg)->type == static_cast<uint8_t>(PageType::BTreeInternal)) {
        const uint16_t child = lo.has_value() ? node_child_index(pg, probe) : 0;
        no = node_child(pg, child);
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
    slot_idx = lo.has_value() ? leaf_lower_bound(pg, probe) : 0;
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
            const BTreeEntry e = leaf_entry(cur, slot_idx);
            if (hi_key.has_value() && key_cmp(e.key, *hi_key) >= 0) {
                done = true;
                return false;  // 越过排他上界
            }
            *out = e;
            ++slot_idx;
            return true;
        }
        pool.unpin(cur);
        cur = nullptr;
    }
}

}  // namespace st
