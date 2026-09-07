# 存储引擎设计文档

## 1. 目标与范围

本模块为 my_simple_db 提供"数据落盘 + 按主键定位"的物理存储能力，技术上采用 **B+ 树聚簇索引** 路线（InnoDB 风格，简化版）。

范围：

- 表/列的元数据持久化（目录 catalog）
- 行数据的持久化：插入、按 rowid 点查、全表扫描
- 索引：隐式 rowid 为聚簇键的 B+ 树
- 崩溃恢复：WAL + 检查点

不在本设计范围内：

- 执行器（将来连接 parser 与 storage 的中间层）
- 复杂并发控制（MVCC 列为后续里程碑）
- SELECT 等未进文法的新语法

## 2. 总体架构

```
server(每客户端一线程)
   │  sql::parse → AST  →  (未来: 执行器把 AST 转成 storage 调用)
   ▼
src/storage(静态库 storage)
   ├── storage.h       对外唯一入口(Database), 只依赖 types.h
   ├── types.h         ColType/Value/Schema/TableMeta/RowId
   ├── codec.h/.cpp    行序列化/反序列化
   ├── page.h          页头与槽(Slot)内存布局
   ├── file_manager    每表一个文件的读写原语(pread/pwrite)
   ├── buffer_pool     页缓存, LRU/Clock 淘汰
   ├── catalog         表元数据: 启动加载, DDL 时改内存再重写文件
   ├── btree           B+ 树(叶/内节点, 查找/插入/分裂)
   ├── wal             追加式日志, 检查点
   └── recovery        启动时崩溃恢复
```

依赖关系：`storage` 不依赖 `parser`。storage 用自己的 `Schema`/`Value` 类型（types.h），执行器将来负责把 AST 的 `ColumnDef` 转换过来；只有 `ColumnDef{name, type字符串}` 与 `Value{int64/double/string}` 两处需要转换，转换逻辑放执行器。

> 格式演化约定：不做版本与迁移，各结构体字段只放当前里程碑实际用到的；后续里程碑需要新字段时直接增删改，不保留旧格式。

对外接口 (storage.h)：

```cpp
namespace st {

Status create_table(std::string_view name, const std::vector<ColumnSpec>& cols);
Status drop_table(std::string_view name);

// 插入一行, 返回分配的 rowid
Status insert(std::string_view table, const std::vector<Value>& row, RowId* rid);

// 点查: 按 rowid 走 B+ 树; 未实现索引前为全表扫描
Status point_get(std::string_view table, RowId rid, Row* out);

// 顺序扫描: 叶子链向前扫描; 返回迭代器句柄
Scan*   scan(std::string_view table);

}
```

`Storage` 内部成员：`BufferPool`、`FileManager`、`Catalog`、`BTreeMgr`（每表一棵树）、`Wal`。所有修改先经 WAL 再改页（见 §9）。

## 3. 文件布局

数据目录（运行时参数指定，如 `./data`）：

```
data/
  catalog.dat     目录文件: 所有表的元数据, 见 §6
  wal.log         WAL 日志(所有表共享)
  t_<table_id>.dat  每表一个数据文件, 全部由 4KB 页组成
```

页号编码（跨表全局）：

```
page_id(uint64) = (table_id:uint32 << 32) | page_no:uint32
```

page_no 从 0 开始；文件内物理偏移 = `page_no * PAGE_SIZE`。数据文件第 0 页为"文件头页"（magic、版本、table_id、下一个空闲页链表头），之后是数据页。

**每表独立文件**的好处：DROP TABLE = 直接删文件；单表调试/导出方便；页分配只看本地文件长度。单文件设计（SQLite/InnoDB tablespace）留给将来可选。

页分配：

- 新页：优先空闲页链表，空了就 `ftruncate` 扩展文件一个页
- 释放：页标记 FREE，挂回空闲链表，链头记录在文件头页

## 4. 页格式

`PAGE_SIZE = 4096`。每页有公共页头：

```cpp
struct PageHeader {
  uint32_t magic;      // 页魔数, 检测错位/垃圾页
  uint8_t  type;       // PageType: FILE_HEADER / HEAP / BTREE_LEAF / BTREE_INTERNAL / FREE
  uint8_t  pad[3];     // 对齐
  uint16_t slot_count; // 当前记录数
  uint16_t free_begin; // 行数据区起点(相对页首), 追加时从这里向下增长
  uint16_t free_end;   // 槽数组起点, 新槽从这里向 free_begin 靠拢
  uint32_t next_page;  // 链式堆页: 下一页页号(0 表示无)
  uint32_t checksum;   // 全页 checksum(CRC32)
};
```

页内布局（通用）：

```
[PageHeader][数据区 free_begin ────────────────→][槽数组 ← end][页尾]
```

槽项(Slot) 4 字节，从页尾向前生长：

```cpp
struct Slot {
  uint16_t off;   // 记录相对页首偏移
  uint16_t len;   // 记录长度
};
```

记录何时算空闲：`free_end - free_begin` 即为空洞 + 尾部剩余。页内整理（compact）在删除后触发。

## 5. 行格式与类型

支持的列类型（与 parser 的字符串类型名映射）：

| 类型名 | ColType | 存储 | 大小 |
|---|---|---|---|
| int | Int | int32 小端 | 4 |
| bigint | BigInt | int64 小端 | 8 |
| double | Double | IEEE754 | 8 |
| varchar(n) | VarChar | uint16 长度前缀 + 字节 | ≤ 65535 |

其他类型（date/datetime/bool）后续按需加，每加一类只改 codec 的类型分派。

记录（Record）序列化格式：

```
[记录长度 uint16][列数据区]
列数据区 = 固定类型 inline 累加 + varchar 各带长度前缀
```

- 长度上限：`PAGE_SIZE - 页头 - 槽`，约 4000 字节；**超长行暂不支持**
- NULL 值暂不支持（类型枚举为 NULL 留一个枚举值，将来扩展类型直接扩枚举）
- 页内定位用 `(page_id, slot_index)`；B+ 树叶子用 rowid 定位

rowid（§8 详述）：表内自增 int64，是聚簇索引键。叶子节点里 **payload 不含 rowid**，rowid 在节点的 key 数组里，由 B+ 树迭代器补成 `Row{rid, values}`。

## 6. 目录 Catalog

`catalog.dat` 结构：`[文件头 magic/版本/记录数 checksum][TableMeta × N]`，每个 TableMeta 变长：

```cpp
struct TableMeta {
  uint32_t     table_id;           // 全局唯一, 自增
  std::string  name;
  std::vector<ColumnSpec> cols;    // {name, type, varchar_len}
};
```

读写规则：

- 启动时整文件读入内存（小库可接受）；发现损坏且无 WAL 可救时直接报错拒绝启动
- DDL（create/drop）流程：先写 WAL(OP_CREATE_TABLE / OP_DROP_TABLE) → 改内存 catalog → fsync 后整体重写 catalog.dat
- 引入 WAL 之前（M1），catalog 直接重写，靠文件内 checksum 检测残破，启动时若校验失败则丢弃（数据非事务性可接受）

## 7. 缓冲池 Buffer Pool

目标：屏蔽磁盘 IO，让 btree/heap 只操作内存页。

```
frame = { page_id, char data[PAGE_SIZE], dirty, pin_count, page_lsn }
BufferPool:
  frame 数组(容量可配, 默认 128)
  page_id → frame 索引 哈希表
  Clock/LRU 淘汰器, 时钟指针
```

接口（简化）：

```cpp
Page* read(page_id);      // pin +1; 未命中则从文件载入(淘汰一个 unpin 干净页或写回 dirty页)
Page* allocate(page_id);  // 取一个空闲页(新页或 FREE 链表), 清零
void  flush(page_id);     // 写盘(GROUP_COMMIT 前断言 WAL 已刷过本页 LSN, 见 §9)
void  unpin(Page*);       // pin -1, 到达 0 后进入可淘汰区
```

要点：

- pin 数 > 0 的页不可被淘汰，保护正在被 btree 使用的页
- 脏页淘汰前回调 `wal::flushed(p->lsn)` 确认对应日志已落盘（write-ahead 不变量）
- 缓冲池整体一把 `std::mutex`；页内容本身的串行化由上层（§10 锁模型）保证，页级闩锁留到并发里程碑

## 8. B+ 树（聚簇索引，数据即叶子）

每表一棵 B+ 树，键 = 该表 rowid（int64），值 = 完整行记录。叶子页存数据，内节点存键 + 子页指针。**所有数据都在叶子里**，这就是"聚簇"。

叶子页布局（槽目录式，支持变长 payload）：

```
[PageHeader][key 数组: key_0...key_{n-1}, 每个 8B, 从 free_begin 向后]
[自由区: 记录 payload 从 free_end 方向分配]
[slot 数组: slot_i 指向 payload_i]
```

- key 数组定长定序 → 记录少时也可以二分查找
- payload 变长 → 槽指向
- `next_page/prev_page` 承接叶子右/左兄弟，形成有序双向链表，scan 沿 next_page 前进

内节点布局（定长，纯索引）：

```
[PageHeader][keys: k_0..k_{m-1}][children: c_0..c_{m-1}]
语义: c_0 子树 < k_0; c_i 子树 ∈ [k_{i-1}, k_i); c_{m-1} 子树 ≥ k_{m-1}
```

插入时执行标准 B+ 树流程：定位叶 → 二分插入 key/slot → 页满分裂（从中间取分割点，key 上提）→ 内节点满了继续向上分裂 → 根满则增高一层。分裂点取 `slot_count/2`。

删除/下溢合并：**M3 暂不实现**，删除只做 slot 标记 + 页内 compact，空洞页最后按整页耗尽回收；孤儿记录靠叶子内排序保证 scan 正确。下溢合并放入后续里程碑（涉及兄弟页锁，先不做）。

性能预期（学习目标，非优化目标）：点查 O(log n) 页 IO，范围查询命中页数 = 数据页数。

## 9. WAL 与崩溃恢复

WHY: B+ 树原地改写页 + 缓冲池延迟写盘（性能），若不做日志，崩溃点数据页处于任意中间状态 -> 丢失且不可判定。WAL 让"允许延迟刷脏页"与"绝不丢失已确认数据"同时成立。

**写前不变量**：任何页落盘前，必须已经 fsync 过覆盖该页 `page_lsn` 的 WAL。实现：缓冲池全局维护 `flushed_lsn`（WAL 已 fsync 到哪），flush 脏页前 `fsync 到 max(flushed_lsn, 页.lsn)`。

所有共享一个 `wal.log`。记录格式：

```
[记录头 20B][LSN uint64][len uint16][op uint8][txn_id int64][page_id uint64]...
体:
  OP_PAGE_PATCH   [offset uint32][len uint16][字节]   # 物理重做, 直接补页区域
  OP_CREATE_TABLE [name 长度前缀 + name][schema...]
  OP_DROP_TABLE   [table_id uint32]
  OP_CHECKPOINT   (仅出现在日志头部位置)
```

`LSN` 全局单调递增（每个记录 +len+20）。页头届时新增 `page_lsn` 字段 = 对该页最后一条日志的 LSN。

**提交语义**（M4 之前无显式事务，每条 SQL 视为单语句自动提交事务）：

```
insert:
  1) 以 txn_id 组装 OP_PAGE_PATCH(涉及多个页就多条)
  2) fsync WAL 至该 txn 的 LSN → 此时可回复客户端"成功"
  3) 后台/淘汰时刷脏页, 无需即刻
```

**检查点**（checkpoint）：定时/按体积触发——flush 全部脏页（每个先保证 WAL 刷过），fsync 数据文件，然后重写 wal.log 头部 `start_lsn` 并把文件截断到一条新的 OP_CHECKPOINT 记录。

**启动恢复流程**（recovery.cpp）：

1. 加载 catalog.dat
2. 打开 wal.log；若头部 `start_lsn` 有效且日志非空 → 从该 LSN 起顺序扫描重放：
   - OP_CREATE_TABLE / OP_DROP_TABLE：重放目录变化
   - OP_PAGE_PATCH：读页，若 `页.page_lsn < 记录LSN` 且页当前存在（表未被后续 DROP）→ 应用字节补丁、置 page_lsn，标记脏
   - 日志尾部不完整（无 OP_PAGE_PATCH 全长）→ 截断丢弃，属正常崩溃边界
3. 回放结束后 flush 脏页、写检查点、清空日志

注意事项：

- 文件扩展产生的"空洞页"没进 WAL：恢复时将未触及的文件区域视为全零页，合法空页
- 页校验和：每次写盘前算，读盘后校验，防错位/坏块
- 首次 fsync 前崩溃 → 数据文件保持旧状态，与目录半新状态由下一条规则处理：DDL 的 WAL 记录在 fsync 后再改内存并重写 catalog，二者成对回放，不会出现"目录有新表但数据文件没有/反过来"

## 10. 并发控制（分阶段）

当前 server 每客户端一线程，多线程并发会同时打 storage。正确性优先，按此顺序演化：

- **M1~M4（本次范围）**：`Storage` 内一把数据库级 `std::mutex` 串行化所有写；scan 持有页 pin。模型等价单写多读（读也串行，量小无影响）。WAL 的 txn_id 恒为 0，无冲突。
- **M5（后续）**：表级 `std::shared_mutex`（scan 共享、insert 独占）→ 缓冲池页帧闩锁 + B+树锁耦合（latch coupling）→ MVCC（行头加版本字段，读快照）。行格式届时按需扩展，不做兼容。

## 11. 实施里程碑

| 阶段 | 内容 | 完成后可做 |
|---|---|---|
| M1 | types/codec/page 布局；file_manager 按页读写；buffer_pool；catalog 建表落盘；heap 追加写 + 全扫描(无索引)；重启读回 | CREATE/DROP/INSERT 持久化，重启数据还在 |
| M2 | 空闲页链表；删除(标记+compact)；页 checksum 校验读盘 | DELETE 行、碎片整理 |
| M3 | B+树叶子+内节点、插入/分裂、按 rowid 点查、叶子链 scan；行内不再带 rowid | point_get、有序全表扫描 |
| M4 | WAL + checkpoint + recovery，接入 buffer_pool 刷盘判定 | 抗崩溃，事务提交语义 |
| M5(可选) | 表级锁 → 页闩锁 → MVCC | 并发读/写正确性 |

每阶段独立可编译、可测试、可回滚。M1 不引入 WAL，崩溃恢复靠"长度前缀 + 页 checksum 截断检测"，数据按追加式可丢失尾部为准，属于可接受的简化，M4 兑现完整正确性。

## 12. 简化项与已知限制

- 超长行（>约 4000B）不支持，varchar(n) 需 n ≤ 4000
- 无 NULL 值（预留位）
- B+ 树删除不做下溢合并（标记删除 + compact）
- 无主键约束（用隐式 rowid 聚簇；grammar 支持 PRIMARY KEY 后，加"主键 → rowid"二级索引，主键 B+树不变）
- 事务仅自动提交；无 MVCC
- 单文件单一目录，数据库互斥，未做多库