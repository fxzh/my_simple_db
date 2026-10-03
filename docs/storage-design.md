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

数据目录（运行时参数指定，如 `./data`）：元数据表文件 t_1/t_2/t_3.dat（见 §6）、每表/每索引一个数据文件 t_<file_id>.dat（4KB 页流，页号从 0 起、文件内偏移 = 页号 × 页大小，第 0 页为文件头页）、wal.log（M4 已实现，所有表与索引共享）。表文件与索引文件同构：表文件头页存 rowid 计数器，索引文件头页存 B+ 树根页号（§8）。文件级读写原语已实现（src/storage/file_manager.h）。

**每表/每索引独立文件**：DROP = 直接删文件；页分配只看本地文件长度，当前为文件尾追加。空闲页链表（释放页标记 FREE 挂回链表、链头记录在文件头页）为后续设计，接入后删除空间才可按页回收；单文件设计（SQLite/InnoDB tablespace）留给将来可选。

## 4. 页格式

`PAGE_SIZE = 4096`，页头/槽布局、堆页追加、墓碑删除、页校验和均已实现，字段与语义见 src/storage/page.h（页头 24B 定长，槽数组自页尾反向生长，槽指向数据区记录）。页内整理 compact 已实现但 DB 层未接入，删除空间暂不回用；FREE 页类型与空闲链表为后续设计（§3）。

## 5. 行格式与类型

列类型与记录编解码已实现（src/storage/codec.h）：int/bigint/float/double/char/varchar 六类，记录 = [长度 u16][NULL 位图][列数据区]（NULL 不占字节，char 定长补空格，varchar 长度前缀），单行约 4KB 上限不跨页，NOT NULL 列拒绝 NULL；其他类型（date/datetime/bool）后续按需加。

- 页内定位用 `(page_id, slot_index)`，即 `RowRef`；二级索引条目以 `(列值编码, RowRef)` 定位行，回表按 RowRef 直读堆页（见 §8）

rowid：表内自增 int64，由表文件头页计数器分配并随 insert 返回；不参与索引定位（索引按 RowRef 物理定位，见 §8），保留作将来显式主键/逻辑行标识的基础。

## 6. 目录（元数据表）

目录不是独立文件, 而是保留段元数据表, schema 硬编码引导, 不存于自身; 目录逻辑位于 src/catalog(ct::Catalog), 存储引擎只提供须持锁的按 file_id 原语。db_table(table_id, table_name, file_id, schema_id) 与 db_column(table_id, col_name, ordinal, type, length, not_null) 已实现: 元数据行为唯一事实来源, 无内存缓存, 查找实时全扫; create_table 先建数据文件再写元数据行, drop_table 反向; 引导与 open 校验见 catalog。db_schema(schema_id, schema_name) 引导写入唯一行 system(id=1); db_table.schema_id 现阶段所有表一律挂 system 名下, schema 名字解析未接入。

M3 新增 db_index(table_id, index_name, col_ordinal, file_id): 每索引一行, 单列索引, col_ordinal 指向 db_column, 索引名表内唯一(跨表可同名)。DDL 规则(已实现): create_index 先建索引文件并全表回填再写元数据行, drop_index 与 drop_table(连带删该表全部索引)反向; insert 对该表全部索引双写条目; open 扫 db_table 与 db_index 取 file_id 分配起点(bootstrap 模式 db_index 由 bootstrap.sql 在 open 后创建, 缺失跳过; 正常模式缺失即元数据损坏)。

## 7. 缓冲池 Buffer Pool

已实现（src/storage/buffer_pool.h）：定长帧数组（帧数经 db.conf 的 buffer_pool_frames 配置，默认 8192）+ 哈希页表（PageId→帧下标，unpin/mark_dirty 经数据指针换算帧下标）+ Clock 淘汰 + pin 引用计数，read/allocate/unpin/mark_dirty/flush 原语；pin > 0 的帧不可淘汰；页校验失败即报错（截断容忍只在崩溃恢复重放路径）；内部不加锁，串行化由上层全局锁保证（§10）。

M4 已接入 WAL：帧增加 before 基线快照（页内容最近一次与"已记日志状态"一致时的副本，只在页新进池/write_back 落盘后/mark_dirty 记完补丁后更新，read 命中已缓存帧时不动，否则两次 pin 之间的修改会漏出 diff）；mark_dirty 以 8 字节字长粒度 diff(before, data) 产生 OP_PAGE_PATCH（write-ahead 不变量，见 §9）；write_back 落盘前断言 before 与 data 一致，存在未记日志的修改当场报错。

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

## 9. WAL 与崩溃恢复（M4 已实现）

WHY: B+ 树原地改写页 + 缓冲池延迟写盘（性能），若不做日志，崩溃点数据页处于任意中间状态 -> 丢失且不可判定。WAL 让"允许延迟刷脏页"与"绝不丢失已确认数据"同时成立。

实现为**物理 redo+undo 补丁**：记录携带页字节补丁的 after/before 双向映像（兼作重做与撤销日志）与事务号（事务层引入；写者经 catalog 全局锁串行）。原理性教学注释见 src/storage/wal.h 与 recovery.cpp 文件头。

**write-ahead 不变量**（任何页字节落盘前，描述它的日志已写入 wal.log）由三点保证：

1. mark_dirty 在置脏前先 diff 帧内 before 基线，把补丁记入 WAL（所有修改点以"就地改字节 + mark_dirty"收尾，元数据表页/B+树页/堆页统一覆盖）；
2. write_back 落盘前断言 before 与 data 一致（防漏调 mark_dirty 的代码路径，当场报错）；
3. flush/close 的落盘顺序：刷脏页 -> fsync WAL -> fsync 数据文件 -> fsync 数据目录。

页头不加 page_lsn、帧只存 before 基线不存 LSN：补丁重放幂等（重放=字节覆盖，应用 0/1/N 次结果一致），无须"该补丁是否已应用"判定；写者串行使记日志与写页的先后即代码顺序。torn 页被恢复重放无害化（重放不校验页，直接覆盖后整页重写）。

所有表与索引共享一个 `wal.log`，记录流式追加：

```
+---------+---------+---------+-------+---------+----------------------+
| len u32 | lsn u64 | txn u64 | op u8 | crc u32 | payload (len 字节)  |
+---------+---------+---------+-------+---------+----------------------+

PagePatch(页补丁): [file_id u64][page_no u32][offset u32][len u32][after len B][before len B]
DropFile(删文件):  [file_id u64]
Commit(提交) / Abort(中止): 空 payload
```

- LSN 全局单调递增从 1 起，检查点清空文件不回退计数器；txn 号由 Wal 每次进程运行从 1 递增分配，不持久化
- 建表/建索引无专门记录：文件头页初始化本身就是补丁，重放经 fd_for 的 O_CREAT 隐式重建文件；元数据行插删（db_table/db_column/db_schema/db_index 也是堆页）同样由补丁覆盖，无需逻辑 DDL 记录
- 删表/删索引记 DropFile 并登记 pending_drops，unlink 延迟到 Commit 记录 fsync 之后：崩溃在 fsync 前则恢复视为未提交（DropFile 跳过，文件从未被删），之后则重放补删（幂等）

**提交语义**（事务层 v1）：持久化边界是 `Catalog::commit_txn()`——有记录则追加 Commit 并 fsync 后才向客户端回 Ok，自动提交语句即单语句事务，行为等价。语句/事务失败的半途修改由 rollback 按 before 映像撤销，不残留。

**检查点**（checkpoint）：干净关闭（Engine::close）与恢复重放完成后执行——刷全部脏页、fsync WAL、fsync 数据文件与目录、清空 wal.log，下次启动零重放。运行期按体量触发：Wal 累计自上次清空以来的写入字节（atomic 计数），`Catalog::commit_txn` 发现达到配置阈值 `wal_checkpoint_bytes`（64KB~1GB，缺省 16MB）时执行检查点——此刻事务记录已结束、锁仍持有，检查点不会打断进行中的事务；单个大事务期间 WAL 可超阈值无上限。

**启动恢复**（recovery.cpp，Engine::open 进入运行状态前）：

1. 第一遍（收集）：WalReader 从文件头顺序解析全部合法记录进内存（尾部半条记录即截断点，其后字节丢弃——被丢弃的必然是尚未 fsync 的修改），建立 committed（出现过 Commit 的 txn）与 aborted（出现过 Abort 的 txn）集合
2. 第二遍（重放）：按原顺序应用 committed 事务的记录——PagePatch 覆盖 after 字节（读原页不校验，短读补零，文件不存在则 O_CREAT 隐式建，覆盖后整页写回），DropFile 存在则删（幂等）；记录语义非法（区间越界/长度不符）当场报错
3. 第三遍（撤销）：按 LSN 逆序应用"无 Commit 且无 Abort"事务（崩溃中止）的 PagePatch before 字节，清除磁盘上残留的未提交修改；该类 DropFile 跳过（不删文件）；有 Abort 的事务跳过（其磁盘已在运行期回滚时复原，重放 before 会破坏后继已提交事务）
4. 重放绕过缓冲池直接 pread/pwrite：恢复期间不产生新 WAL 记录，结束后池仍为空，与新启动进程无异
5. 收尾顺序与检查点一致：fsync 数据文件 -> fsync 目录 -> 清空 WAL

注意事项：

- 文件扩展产生的"空洞页"没进 WAL：恢复读页时短读部分补零，补丁覆盖后合法
- 页校验和随补丁字节一起重放恢复，torn 页无需单独处理
- DDL 崩溃窗口：建表若崩溃在补丁 fsync 前，重放后无此表（未确认）；ftruncate 已建的空文件残留为孤儿文件，file_id 复用时被 ftruncate 覆盖，无害

## 10. 并发控制（分阶段）

当前 server 每客户端一线程，多线程并发会同时打 storage。正确性优先，按此顺序演化：

- **M1~M4（本次范围）**：`Storage` 内一把数据库级 `std::mutex` 串行化所有写；scan 持有页 pin。模型等价单写多读（读也串行，量小无影响）。
- **事务层 v1（已实现）**：catalog 锁升级为事务粒度长持（recursive_mutex），全库同一时刻至多一个活动事务；WAL 记录携带事务号与 Commit/Abort。
- **M7（后续）**：表级 `std::shared_mutex`（scan 共享、insert 独占）→ 缓冲池页帧闩锁 + B+树锁耦合（latch coupling）→ MVCC（行头加版本字段，读快照）。行格式届时按需扩展，不做兼容。

## 11. SQL 链路接入（M5，规划）

存储层索引就位后，文法与执行器按下述接入；文法/绑定/DDL 执行已接入，本节其余为规划，暂不实现：

- 文法（parser）：已接入 `CREATE INDEX name ON table (col);` 与 `DROP INDEX name ON table;`，单列，索引名表内唯一，UNIQUE 不做
- 语义分析（analyzer）：已接入绑定：表/列存在性、索引列类型限 int/bigint/float/double、保留表拦截；索引名表内查重与索引存在性校验在 catalog 持锁完成（DDL 执行已接入，executor 经 catalog 门面 create_index/drop_index）
- 计划（planner）：现有 Project[Filter[SeqScan]] 之上，Filter 含 `col θ const`（θ ∈ =, <, ≤, >, ≥, BETWEEN）且该列有索引时，生成 IndexScan{键下界, 上界} 替换 SeqScan 并摘除该谓词，其余谓词留在 Filter；无适用索引维持 SeqScan（访问路径选择，非降级）
- 执行（executor）：IndexScan 迭代 = 树范围扫描 → 回表取整行 → 堆槽墓碑跳过 → 残余 Filter 过滤 → 上抛 Project；UNIQUE 索引在 insert 前对键做等值预查，命中非墓碑行即拒绝
- 依赖方向不变：executor 经 catalog 新增门面（create_index/drop_index/索引扫描原语）访问存储层

## 12. 实施里程碑

| 阶段 | 内容 | 完成后可做 |
|---|---|---|
| M1 | types/codec/page 布局；file_manager 按页读写；buffer_pool；元数据表引导；heap 追加写 + 全扫描(无索引)；重启读回 | CREATE/DROP/INSERT 持久化，重启数据还在 |
| M2 | 删除(墓碑标记)；页 checksum 校验读盘 | DELETE 行 |
| M3 | 存储层二级索引：db_index 元数据表、索引文件生命周期、insert 双写、等值/范围查找与回表、建索引回填；键限 int/bigint/float/double 定长编码 | 存储层可建/维护/查询索引 |
| M4 | WAL + checkpoint + recovery，接入 buffer_pool 刷盘判定（含索引页补丁与索引 DDL 记录）。已实现：物理 redo 补丁、检查点、启动重放 | 抗崩溃，事务提交语义 |
| M5 | SQL 链路：CREATE/DROP INDEX 文法与绑定、DDL 执行已实现；planner 索引选择、IndexScan 执行、UNIQUE 索引未做（见 §11） | 客户端可建/删/维护索引 |
| M6 | 索引全类型键（char/varchar 变长编码，节点单元格布局）；条目删除与下溢合并；空闲页链表与 vacuum | varchar 索引、空间回收 |
| M7(可选) | 表级锁 → 页闩锁 → MVCC | 并发读/写正确性 |

每阶段独立可编译、可测试、可回滚。M1 不引入 WAL，崩溃恢复靠"长度前缀 + 页 checksum 截断检测"，数据按追加式可丢失尾部为准，属于可接受的简化，M4 兑现完整正确性。

## 13. 简化项与已知限制

- 超长行（>约 4000B）不支持，varchar(n) 需 n ≤ 4000
- 索引键首期限 int/bigint/float/double 定长编码，char/varchar 变长键 M6
- 索引条目删除未实现：DELETE 只做堆墓碑，索引条目滞留靠回表校验过滤；物理清理与下溢合并 M6
- M4（WAL）之前堆与索引双写无崩溃原子性：崩溃可致索引缺条目（等值查询漏行，堆链全表扫描不受影响）或悬空条目（回表按页损坏报错），重建索引可修复；M4 起补丁统一覆盖堆页与树页，双写崩溃一致性由重放保证
- 运行期检查点在 commit_txn 内触发（锁内、事务记录结束之后）：跨阈值事务的 Ok 前顺带刷盘，单个大事务（如建索引回填）期间 WAL 可超阈值无上限
- 仅单列索引，多列复合索引后续里程碑
- 无显式主键/唯一约束；rowid 照常分配但不参与定位，PRIMARY KEY/UNIQUE 于 M5 经索引落地
- 事务为全库串行实现（恒 SERIALIZABLE），无并发事务交错与 MVCC，演进路线见 §10
- 单文件单一目录，数据库互斥，未做多库