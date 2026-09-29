# 存储引擎设计文档

## 1. 目标与范围

本模块为 my_simple_db 提供"数据落盘 + 按索引定位"的物理存储能力，技术上采用 **堆表 + B+ 树二级索引** 路线（PostgreSQL 风格，简化版）：行数据存堆页，索引条目以 (列值, 行物理位置) 记录定位，查得后回表读堆。

范围：

- 表/列/索引的元数据持久化（元数据表 catalog）
- 行数据的持久化：插入、删除、全表扫描
- 索引：二级 B+ 树（本次接入存储层；文法与执行器接入见 §11）
- 崩溃恢复：WAL + 检查点

不在本设计范围内：

- CREATE/DROP INDEX 文法与索引访问路径选择（§11 规划，M5 实现）
- 多列复合索引（后续里程碑）
- 复杂并发控制（MVCC 列为后续里程碑）

## 2. 总体架构

```
server(每客户端一线程)
   │  sql::parse → AST  →  executor(把 AST 转成 catalog 门面调用)
   ▼
src/catalog(静态库 catalog)
   └── catalog.h/.cpp  目录门面(Catalog): 元数据表逻辑 + 名字型门面 + 全局锁
   ▼
src/storage(静态库 storage)
   ├── engine.h/.cpp   文件引擎入口(Engine, 原语须持锁), 只依赖 types.h
   ├── types.h         ColType/Value/Schema/TableMeta/RowId
   ├── codec.h/.cpp    行序列化/反序列化
   ├── page.h          页头与槽(Slot)内存布局
   ├── file_manager    每表/每索引文件的读写原语(pread/pwrite)
   ├── buffer_pool     页缓存, Clock 淘汰
   ├── btree           B+ 树(叶/内节点, 查找/插入/分裂)
   ├── wal             追加式日志, 检查点
   └── recovery        启动时崩溃恢复
```

依赖关系：executor → catalog → storage 单向；`catalog` 与 `storage` 均不依赖 `parser`。storage 用自己的 `Schema`/`Value` 类型（types.h），执行器负责把 AST 的 `ColumnDef` 转换过来；只有 `ColumnDef{name, type字符串}` 与 `Value{int64/double/string}` 两处需要转换，转换逻辑放执行器。

> 格式演化约定：不做版本与迁移，各结构体字段只放当前里程碑实际用到的；后续里程碑需要新字段时直接增删改，不保留旧格式。

对外接口 (catalog.h, 名字型门面; engine.h 提供须持锁的按 file_id 原语)：

```cpp
namespace st {

Status create_table(std::string_view name, const std::vector<ColumnSpec>& cols);
Status drop_table(std::string_view name);

// 建索引: 建索引文件 + 全表扫描回填 + 写 db_index 元数据行
Status create_index(std::string_view table, std::string_view col, std::string_view index_name);
Status drop_index(std::string_view index_name);

// 插入一行: 堆页追加 + 该表全部索引的条目插入, 返回分配的 rowid
Status insert(std::string_view table, const std::vector<Value>& row, RowId* rid);

// 索引扫描: 键区间 [lo, hi) 沿叶子链前进, 迭代产出 RowRef, 回表取整行
Scan*   index_scan(std::string_view index_name, const Value* lo, const Value* hi);

// 全表扫描: 沿堆页链前进, 不经索引; 返回迭代器句柄
Scan*   scan(std::string_view table);

}
```

`Storage` 内部成员：`BufferPool`、`FileManager`、`BTreeMgr`（每索引一棵树）、`Wal`。所有修改先经 WAL 再改页（见 §9）。

## 3. 文件布局

数据目录（运行时参数指定，如 `./data`）：元数据表文件 t_1/t_2/t_3.dat（见 §6）、每表/每索引一个数据文件 t_<file_id>.dat（4KB 页流，页号从 0 起、文件内偏移 = 页号 × 页大小，第 0 页为文件头页）、wal.log（M4，所有表与索引共享）。表文件与索引文件同构：表文件头页存 rowid 计数器，索引文件头页存 B+ 树根页号（§8）。文件级读写原语已实现（src/storage/file_manager.h）。

**每表/每索引独立文件**：DROP = 直接删文件；页分配只看本地文件长度，当前为文件尾追加。空闲页链表（释放页标记 FREE 挂回链表、链头记录在文件头页）为后续设计，接入后删除空间才可按页回收；单文件设计（SQLite/InnoDB tablespace）留给将来可选。

## 4. 页格式

`PAGE_SIZE = 4096`，页头/槽布局、堆页追加、墓碑删除、页校验和均已实现，字段与语义见 src/storage/page.h（页头 24B 定长，槽数组自页尾反向生长，槽指向数据区记录）。页内整理 compact 已实现但 DB 层未接入，删除空间暂不回用；FREE 页类型与空闲链表为后续设计（§3）。

## 5. 行格式与类型

列类型与记录编解码已实现（src/storage/codec.h）：int/bigint/float/double/char/varchar 六类，记录 = [长度 u16][NULL 位图][列数据区]（NULL 不占字节，char 定长补空格，varchar 长度前缀），单行约 4KB 上限不跨页，NOT NULL 列拒绝 NULL；其他类型（date/datetime/bool）后续按需加。

- 页内定位用 `(page_id, slot_index)`，即 `RowRef`；二级索引条目以 `(列值编码, RowRef)` 定位行，回表按 RowRef 直读堆页（见 §8）

rowid：表内自增 int64，由表文件头页计数器分配并随 insert 返回；不参与索引定位（索引按 RowRef 物理定位，见 §8），保留作将来显式主键/逻辑行标识的基础。

## 6. 目录（元数据表）

目录不是独立文件, 而是保留段元数据表, schema 硬编码引导, 不存于自身; 目录逻辑位于 src/catalog(ct::Catalog), 存储引擎只提供须持锁的按 file_id 原语。db_table(table_id, table_name, file_id, schema_id) 与 db_column(table_id, col_name, ordinal, type, length, not_null) 已实现: 元数据行为唯一事实来源, 无内存缓存, 查找实时全扫; create_table 先建数据文件再写元数据行, drop_table 反向; 引导与 open 校验见 catalog。db_schema(schema_id, schema_name) 引导写入唯一行 system(id=1); db_table.schema_id 现阶段所有表一律挂 system 名下, schema 名字解析未接入。

M3 新增 db_index(table_id, index_name, col_ordinal, file_id): 每索引一行, 单列索引, col_ordinal 指向 db_column。DDL 规则: create_index 先建索引文件并全表回填再写元数据行, drop_index 反向, drop_table 连带删该表全部索引; 引导与 open 校验随之扩为四表。

## 7. 缓冲池 Buffer Pool

已实现（src/storage/buffer_pool.h）：定长帧数组（默认 128）+ 哈希页表（PageId→帧下标，unpin/mark_dirty 经数据指针换算帧下标）+ Clock 淘汰 + pin 引用计数，read/allocate/unpin/mark_dirty/flush 原语；pin > 0 的帧不可淘汰；数据页校验失败按尾部截断重建空页，文件头页校验失败报错；内部不加锁，串行化由上层全局锁保证（§10）。

M4 接入 WAL 时帧增加 page_lsn，脏页落盘前确认覆盖该 LSN 的日志已 fsync（write-ahead 不变量，见 §9）。

## 8. B+ 树（二级索引）

每索引一棵 B+ 树，存于独立索引文件（t_<file_id>.dat，页 0 文件头页存根页号）。树内只有索引条目，行数据始终在堆页：堆是表，树是路标。

条目与比较序：

- 单元格 = [键: 索引列值的序保持字节编码 + NULL 标志 1B][行定位: RowRef(页号 u32 + 槽 u16)]
- 全序 = 先按键序（非 NULL 按值序，NULL 排最大，PG 行为），同键按 RowRef 数值序；(键, RowRef) 全局唯一（堆槽不复用，RowRef 不撞键），插入无键冲突
- 路由与分裂按完整复合序，分隔键取右页首条目的复合键，同键多行跨分隔键不丢失

键编码（首期定长数值列 int/bigint/float/double，编码后统一 8B 值 + 1B NULL 标志）：

- 整型：最高符号位翻转，无符号字节序即有符号数值序
- 浮点：IEEE754 正数翻符号位、负数按位取反，字节序即数值序；-0.0 归一为 +0.0，两零编码一致
- NULL：入索引且排最大（PG 行为），标志位置 1、键值部分无意义置 0
- char/varchar 变长键延后（M6）：届时叶/内节点改单元格布局或做键前缀截断

页布局与插入/分裂/范围扫描已在 src/storage/btree.cpp 实现二级索引形态：叶子单元格 [键值 8B][NULL 标志 1B][RowRef 6B]、内节点分隔项步长 15B、(键, RowRef) 复合全序（NULL 排最大）并放开同键去重；槽序即 (键, RowRef) 序，`next_page` 单向成链，分裂（分隔项上提）与根增高流程不变。内节点路由语义：c_0 子树 < k_0；c_i 子树 ∈ [k_{i-1}, k_i)；c_m 子树 ≥ k_{m-1}。键序保持编码 encode_key（含 NULL 键）与回表原语 Engine::read_row 已实现；engine/catalog 接入（索引文件生命周期、insert 双写、建索引回填）未开始。

查找与回表：

- 等值：下探到叶后二分定位键下界，沿槽扫过全部键相等条目，逐条回表
- 范围：定位键下界后沿叶子链前进，越过上界即止
- 回表：RowRef → buffer_pool 读堆页取槽解码；槽为墓碑则跳过（索引条目滞后于堆删除，见 §13）

维护：

- insert：堆页追加得 RowRef → 编码索引列值 → 该表各索引树插入条目
- delete：首期不删索引条目，堆墓碑 + 回表校验兜底；条目物理清理与下溢合并一并延后（M6，涉及兄弟页锁，先不做）
- create_index：持全局锁全表扫描堆页，逐行以 (列值, RowRef) 插入树

性能预期（学习目标，非优化目标）：等值查找 O(log n) 页 IO + 每命中行回表 1 页；范围查询页 IO ∝ 命中条目数；全表扫描走堆页链，不经索引。

## 9. WAL 与崩溃恢复

WHY: B+ 树原地改写页 + 缓冲池延迟写盘（性能），若不做日志，崩溃点数据页处于任意中间状态 -> 丢失且不可判定。WAL 让"允许延迟刷脏页"与"绝不丢失已确认数据"同时成立。

**写前不变量**：任何页落盘前，必须已经 fsync 过覆盖该页 `page_lsn` 的 WAL。实现：缓冲池全局维护 `flushed_lsn`（WAL 已 fsync 到哪），flush 脏页前 `fsync 到 max(flushed_lsn, 页.lsn)`。

所有共享一个 `wal.log`。记录格式：

```
[记录头 20B][LSN uint64][len uint16][op uint8][txn_id int64][page_id uint64]...
体:
  OP_PAGE_PATCH   [offset uint32][len uint16][字节]   # 物理重做, 直接补页区域
  OP_CREATE_TABLE [name 长度前缀 + name][schema...]
  OP_DROP_TABLE   [file_id uint64]
  OP_CREATE_INDEX [name 长度前缀 + name][table_id][col_ordinal][file_id]
  OP_DROP_INDEX   [file_id uint64]
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

1. 读取元数据表
2. 打开 wal.log；若头部 `start_lsn` 有效且日志非空 → 从该 LSN 起顺序扫描重放：
   - OP_CREATE_TABLE / OP_DROP_TABLE / OP_CREATE_INDEX / OP_DROP_INDEX：重放元数据行变化
   - OP_PAGE_PATCH：读页，若 `页.page_lsn < 记录LSN` 且页当前存在（表未被后续 DROP）→ 应用字节补丁、置 page_lsn，标记脏
   - 日志尾部不完整（无 OP_PAGE_PATCH 全长）→ 截断丢弃，属正常崩溃边界
3. 回放结束后 flush 脏页、写检查点、清空日志

注意事项：

- 文件扩展产生的"空洞页"没进 WAL：恢复时将未触及的文件区域视为全零页，合法空页
- 页校验和：每次写盘前算，读盘后校验，防错位/坏块
- 首次 fsync 前崩溃 → 数据文件保持旧状态，与元数据半新状态由下一条规则处理：DDL 的 WAL 记录在 fsync 后再写元数据行，二者成对回放，不会出现"元数据有新表但数据文件没有/反过来"

## 10. 并发控制（分阶段）

当前 server 每客户端一线程，多线程并发会同时打 storage。正确性优先，按此顺序演化：

- **M1~M4（本次范围）**：`Storage` 内一把数据库级 `std::mutex` 串行化所有写；scan 持有页 pin。模型等价单写多读（读也串行，量小无影响）。WAL 的 txn_id 恒为 0，无冲突。
- **M7（后续）**：表级 `std::shared_mutex`（scan 共享、insert 独占）→ 缓冲池页帧闩锁 + B+树锁耦合（latch coupling）→ MVCC（行头加版本字段，读快照）。行格式届时按需扩展，不做兼容。

## 11. SQL 链路接入（M5，规划）

存储层索引就位后，文法与执行器按下述接入；本节为规划，暂不实现：

- 文法（parser）：`CREATE [UNIQUE] INDEX name ON table (col);` 与 `DROP INDEX name;`，单列，语句风格随现有 DDL
- 语义分析（analyzer）：绑定表/列存在性；索引列类型须属于当前键支持集；索引名经 db_index 查重；UNIQUE 标志届时按需加字段记入 db_index
- 计划（planner）：现有 Project[Filter[SeqScan]] 之上，Filter 含 `col θ const`（θ ∈ =, <, ≤, >, ≥, BETWEEN）且该列有索引时，生成 IndexScan{键下界, 上界} 替换 SeqScan 并摘除该谓词，其余谓词留在 Filter；无适用索引维持 SeqScan（访问路径选择，非降级）
- 执行（executor）：IndexScan 迭代 = 树范围扫描 → 回表取整行 → 堆槽墓碑跳过 → 残余 Filter 过滤 → 上抛 Project；UNIQUE 索引在 insert 前对键做等值预查，命中非墓碑行即拒绝
- 依赖方向不变：executor 经 catalog 新增门面（create_index/drop_index/索引扫描原语）访问存储层

## 12. 实施里程碑

| 阶段 | 内容 | 完成后可做 |
|---|---|---|
| M1 | types/codec/page 布局；file_manager 按页读写；buffer_pool；元数据表引导；heap 追加写 + 全扫描(无索引)；重启读回 | CREATE/DROP/INSERT 持久化，重启数据还在 |
| M2 | 删除(墓碑标记)；页 checksum 校验读盘 | DELETE 行 |
| M3 | 存储层二级索引：db_index 元数据表、索引文件生命周期、insert 双写、等值/范围查找与回表、建索引回填；键限 int/bigint/float/double 定长编码 | 存储层可建/维护/查询索引 |
| M4 | WAL + checkpoint + recovery，接入 buffer_pool 刷盘判定（含索引页补丁与索引 DDL 记录） | 抗崩溃，事务提交语义 |
| M5 | SQL 链路：CREATE/DROP INDEX 文法与绑定、planner 索引选择、IndexScan 执行、UNIQUE 索引（见 §11） | 客户端可建/用索引 |
| M6 | 索引全类型键（char/varchar 变长编码，节点单元格布局）；条目删除与下溢合并；空闲页链表与 vacuum | varchar 索引、空间回收 |
| M7(可选) | 表级锁 → 页闩锁 → MVCC | 并发读/写正确性 |

每阶段独立可编译、可测试、可回滚。M1 不引入 WAL，崩溃恢复靠"长度前缀 + 页 checksum 截断检测"，数据按追加式可丢失尾部为准，属于可接受的简化，M4 兑现完整正确性。

## 13. 简化项与已知限制

- 超长行（>约 4000B）不支持，varchar(n) 需 n ≤ 4000
- 索引键首期限 int/bigint/float/double 定长编码，char/varchar 变长键 M6
- 索引条目删除未实现：DELETE 只做堆墓碑，索引条目滞留靠回表校验过滤；物理清理与下溢合并 M6
- M4（WAL）之前堆与索引双写无崩溃原子性：崩溃可致索引缺条目（等值查询漏行，堆链全表扫描不受影响）或悬空条目（回表按页损坏报错），重建索引可修复
- 仅单列索引，多列复合索引后续里程碑
- 无显式主键/唯一约束；rowid 照常分配但不参与定位，PRIMARY KEY/UNIQUE 于 M5 经索引落地
- 事务仅自动提交；无 MVCC
- 单文件单一目录，数据库互斥，未做多库