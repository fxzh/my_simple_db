# 存储引擎设计文档

## 1. 目标与范围

本模块为 my_simple_db 提供"数据落盘 + 按索引定位"的物理存储能力，技术上采用 **堆表 + B+ 树二级索引** 路线（PostgreSQL 风格，简化版）：行数据存堆页，索引条目以 (列值, 行物理位置) 记录定位，查得后回表读堆。

范围：

- 表/列/索引的元数据持久化（元数据表 catalog）
- 行数据的持久化：插入、删除、全表扫描
- 索引：二级 B+ 树（本次接入存储层；文法与执行器接入见 §11）
- 崩溃恢复：WAL + 检查点

不在本设计范围内：

- CREATE/DROP INDEX 文法与索引访问路径选择（见 §11，已实现）
- 多列复合索引（后续里程碑）
- 复杂并发控制（MVCC 列为后续里程碑）

## 2. 总体架构

```
server(每客户端一线程)
   │  sql::parse → AST → exec::execute(内部: ana::analyze → pl::build/pl::optimize → 算子)
   ▼
src/analyzer + src/planner + src/executor
   ▼
src/catalog(静态库 catalog)
   └── catalog.h/.cpp  目录门面(ct::Catalog): 元数据表逻辑 + 名字/句柄型门面 + 事务粒度全局锁
   ▼
src/storage(静态库 storage)
   ├── engine.h/.cpp   文件引擎入口(Engine, 原语须持锁), 只依赖 types.h
   ├── types.h         类型体系(TypeAttr/Family/Rep/Store/内置类型种子数据 kTypeSeeds)/ColumnSpec/Value/TableMeta/RowId/Row/RowRef
   ├── codec.h/.cpp    行序列化/反序列化
   ├── page.h          页头与槽(Slot)内存布局
   ├── file_manager    每表/每索引文件的读写原语(pread/pwrite)
   ├── buffer_pool     页缓存, Clock 淘汰
   ├── btree           B+ 树(叶/内节点, 查找/插入/分裂/开闭边界范围扫描)
   ├── wal             追加式日志, 检查点
   └── recovery        启动时崩溃恢复
```

依赖关系：parser → analyzer → planner → executor → catalog → storage 单向；`catalog` 与 `storage` 均不依赖 `parser`。storage 用自己的 `ColumnSpec`/`Value` 类型（types.h），analyzer 负责把 AST 的 `ColumnDef`（类型名字符串 + 长度/标度）经 catalog 查 db_type 定型为 `st::TypeAttr` 再组装 `st::ColumnSpec`；求值产物即 `st::Value`（携带类型属性 TypeAttr），无第二套值域。

> 格式演化约定：不做版本与迁移，各结构体字段只放当前里程碑实际用到的；后续里程碑需要新字段时直接增删改，不保留旧格式。

对外接口为 ct::Catalog 类（catalog.h，名字/句柄型门面，错误当场抛出不返回 Status），要点（完整签名见 catalog.h；engine.h 提供须持锁的按 file_id 原语）：

```cpp
class Catalog {
public:
    // 生命周期与事务
    void create();  void open();  void close();
    void begin_txn();  void commit_txn();  void rollback_txn();

    // DDL: 建表(名字型, 挂限定名所属 schema) / 建删 schema / 建删索引(句柄型)
    uint64_t create_table(const TableRef&, const std::vector<st::ColumnSpec>&);
    void drop_table(const TableHandle&);
    void create_index(const TableHandle&, const std::string& index, uint16_t col_ordinal);
    void drop_index(const TableHandle&, const std::string& index);

    // DML(按绑定层句柄/元数据进入): 插入返回分配的 rowid
    st::RowId insert(const st::TableMeta&, const std::vector<st::Value>&);
    size_t delete_by_ref(const st::RowRef&);
    size_t update_rows(const st::TableMeta&, const std::vector<RowUpdate>&);
    size_t delete_all(const st::TableMeta&);

    // 读: 全表扫描 / 该表全部索引(供计划层选择) / 按索引文件 id 开范围扫描 /
    //     回表 / 行数 / 按限定名取元数据
    std::unique_ptr<st::Scanner> scan(const st::TableMeta&);
    std::vector<IndexEntry> indexes(const st::TableMeta&);
    std::unique_ptr<st::BTreeScanner> index_scan(uint64_t index_fid,
                                                 std::optional<st::ScanBound> lo,
                                                 std::optional<st::ScanBound> hi);
    bool read_row(const st::RowRef&, const std::vector<st::ColumnSpec>&, st::Row*);
    size_t row_count(const st::TableMeta&);
    st::TableMeta table_meta(const TableRef&);

    // 类型: 按名(规范名或别名)/按 type_id 查 db_type 行 / 按内部名取属性
    TypeRow find_type(const std::string& name);
    TypeRow find_type_by_id(uint16_t id);
    st::TypeAttr require_type(const std::string& name);
};
```

`Engine` 内部成员：`FileManager`、`Wal`、`BufferPool`、`trees_`（每索引一棵 BTree）与 `tail_pages_`（文件尾页跟踪）。所有修改先经 WAL 再改页（见 §9）。

## 3. 文件布局

数据目录（运行时参数指定，如 `./data`）：元数据表文件 t_1~t_6.dat（db_table/db_column/db_schema/db_type 引导期直建，db_index/db_func/db_version 由 bootstrap.sql 经 bootstrap 模式创建，见 §6）、每表/每索引一个数据文件 t_<file_id>.dat（4KB 页流，页号从 0 起、文件内偏移 = 页号 × 页大小，第 0 页为文件头页）、wal.log（M4 已实现，所有表与索引共享）。表文件与索引文件同构：表文件头页存 rowid 计数器，索引文件头页存 B+ 树根页号（§8）。文件级读写原语已实现（src/storage/file_manager.h）。

**每表/每索引独立文件**：DROP = 记 DropFile 并延迟到事务提交后 unlink（见 §9）；页分配为文件尾追加，尾页由引擎内存跟踪（tail_pages_，打开时从磁盘最高页向下跳过全零页定尾，页分配回滚可恢复）。空闲页链表（释放页标记 FREE 挂回链表、链头记录在文件头页）为后续设计，接入后删除空间才可按页回收；单文件设计（SQLite/InnoDB tablespace）留给将来可选。

## 4. 页格式

`PAGE_SIZE = 4096`，页头/槽布局、堆页追加、墓碑删除、页校验和均已实现，字段与语义见 src/storage/page.h（页头 24B 定长，槽数组自页尾反向生长，槽指向数据区记录）。页内整理 compact 已实现但 DB 层未接入，删除空间暂不回用；FREE 页类型与空闲链表为后续设计（§3）。

## 5. 行格式与类型

类型体系已元数据化（types.h）：类型身份为 db_type.type_id（§6），常用属性内联为 st::TypeAttr（family 值族、rank 数值提升等级、rep 值表示、store 存储形式、width 定宽字节数）随 ColumnSpec/Value 携带，各层按属性分派，无 ColType 枚举；属性枚举取值集合（Family/Rep/Store）与内置类型种子数据 kTypeSeeds（int/bigint/double/varchar/float/char/boolean/numeric 八类，含别名与声明长度/标度规则）见 types.h，新增类型在种子数据加行、引导写入 db_type，语法关键字与各层行为分派另行同步。

记录编解码已实现（src/storage/codec.h）：记录 = [长度 u16][NULL 位图][列数据区]（NULL 不占字节），编解码按列属性分派，rep 定值表示、store/width 定字节形态：Int64 按 width 定宽 4B（int）/8B（bigint）小端补码，Float/Double 定宽 4B/8B IEEE754 位模式（非有限值拒绝），Str 按 store 取变长（varchar：长度前缀 u16 + 字节）或定长补空格（char，读回保留填充空格），Bool 1B，Dec（numeric）16B 小端补码 unscaled + 1B scale；值的属性可与列属性不同（同族类型拓宽合法，如 int 值入 bigint 列），表示不匹配/越界/NOT NULL 列为 NULL 返回 false，属性组合不在枚举取值集合内视为内部错误；解码产物携带列属性。单行约 4KB 上限不跨页；其他类型（date/datetime）后续在内置类型种子数据加行即可。

- 页内定位用 `(page_id, slot_index)`，即 `RowRef`；二级索引条目以 `(列值编码, RowRef)` 定位行，回表按 RowRef 直读堆页（见 §8）

rowid：表内自增 int64，由表文件头页计数器分配并随 insert 返回；不参与索引定位（索引按 RowRef 物理定位，见 §8），保留作将来显式主键/逻辑行标识的基础。

## 6. 目录（元数据表）

目录不是独立文件, 而是保留段元数据表, 引导期 schema 硬编码, 不存于自身; 目录逻辑位于 src/catalog(ct::Catalog), 存储引擎只提供须持锁的按 file_id 原语。db_table(table_id, table_name, file_id, schema_id)、db_column(table_id, col_name, ordinal, type, length, scale, not_null) 与 db_type(type_id, type_name, alias, family, rank, len_allowed, len_default, len_max, scale_allowed, rep, store, width) 已实现: 元数据行为唯一事实来源, 无内存缓存, 查找实时全扫; create_table 先建数据文件再写元数据行, drop_table 反向; 引导与 open 校验见 catalog。类型已元数据化: db_type 每类型一行, 种子行由内置类型种子数据 kTypeSeeds(types.h) 在 Catalog::create 引导写入; db_column.type 存 type_id, 读回列定义时逐行匹配 db_type 取属性内联为 st::TypeAttr(行值非法或属性不在枚举取值集合内当场报错); 按名查类型(规范名或别名, 供 DDL 定型与 CAST 目标)走 find_type, 按 id 查类型(名称渲染)走 find_type_by_id, 按内部名取属性走 require_type。db_schema(schema_id, schema_name) 引导写入 system 行(id=1), public schema 由 bootstrap.sql 经正常路径创建。schema 功能已完整接入: create/drop schema(非空拒绝删除), 未限定名解析到会话变量 current_schema(缺省 public), 限定名 schema.table 全链路支持。open 校验: 正常模式扫 db_table 按名检查 db_version 完成标记表(bootstrap.sql 末尾创建, 行存在即初始化全程成功), bootstrap 模式检查四张自举表文件存在。

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

键编码（按列属性 rep 分派，定长列 int/bigint/float/double/bool，编码后统一 8B 值 + 1B NULL 标志）：

- 整型：最高符号位翻转，无符号字节序即有符号数值序
- 浮点：IEEE754 正数翻符号位、负数按位取反，字节序即数值序；-0.0 归一为 +0.0，两零编码一致；float 先宽化为 double 再变换
- 布尔：false/true 编码为 0/1
- NULL：入索引且排最大（PG 行为），标志位置 1、键值部分无意义置 0
- char/varchar 变长键与 numeric 列暂不支持：语义层按 rep 拒绝 Str/Dec，encode_key 对 Dec 兜底报错；变长键延后（M6），届时叶/内节点改单元格布局或做键前缀截断

页布局与插入/分裂/范围扫描已在 src/storage/btree.cpp 实现二级索引形态：叶子单元格 [键值 8B][NULL 标志 1B][RowRef 6B]、内节点分隔项步长 15B、(键, RowRef) 复合全序（NULL 排最大）并放开同键去重；槽序即 (键, RowRef) 序，`next_page` 单向成链，分裂（分隔项上提）与根增高流程不变。内节点路由语义：c_0 子树 < k_0；c_i 子树 ∈ [k_{i-1}, k_i)；c_m 子树 ≥ k_{m-1}。键序保持编码 encode_key（含 NULL 键与 bool 键）与回表原语 Engine::read_row 已实现；engine/catalog 接入已完成：索引文件生命周期（init_index_file/remove_index_file，unlink 延迟到提交后）、insert/update 新行全量双写该表全部索引、create_index 全表回填（Engine::build_index）。

查找与回表：

- 等值：下探到叶后二分定位键下界，沿槽扫过全部键相等条目，逐条回表
- 范围：定位键下界后沿叶子链前进，边界开闭由 ScanBound.inclusive 决定（到达开上界或越过闭上界即止），NULL 键可作界
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

**检查点**（checkpoint）：干净关闭（Engine::close）、恢复重放完成后与目录初始化（initdb）完成时执行——刷全部脏页、fsync WAL、fsync 数据文件与目录、清空 wal.log，下次启动零重放。运行期按体量触发：Wal 累计自上次清空以来的写入字节（atomic 计数），`Catalog::commit_txn` 发现达到配置阈值 `wal_checkpoint_bytes`（64KB~1GB，缺省 16MB）时执行检查点——此刻事务记录已结束、锁仍持有，检查点不会打断进行中的事务；单个大事务期间 WAL 可超阈值无上限。

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

- **现状（事务层 v1，已实现）**：catalog 一把 `std::recursive_mutex`，事务粒度长期持有——自动提交语句为单语句事务，显式事务首条语句取锁长期持有至 COMMIT/ROLLBACK，全库同一时刻至多一个活动事务；WAL 记录携带事务号与 Commit/Abort。扫描游标不持锁（仅持页 pin），并发 DDL 期间扫描是未定义行为；自动提交 SELECT 在算子 open 后即提交释放锁，结果集锁外流式拉取。
- **M7（后续）**：表级 `std::shared_mutex`（scan 共享、insert 独占）→ 缓冲池页帧闩锁 + B+树锁耦合（latch coupling）→ MVCC（行头加版本字段，读快照）。行格式届时按需扩展，不做兼容。

## 11. SQL 链路接入（M5，已实现除 UNIQUE）

- 文法（parser）：已接入 `CREATE INDEX name ON table (col);` 与 `DROP INDEX name ON table;`，单列，索引名表内唯一，UNIQUE 不做
- 语义分析（analyzer）：已接入绑定：表/列存在性、索引列类型限 int/bigint/float/double/bool、保留表拦截；索引名表内查重与索引存在性校验在 catalog 持锁完成（executor 经 catalog 门面 create_index/drop_index）
- 计划（planner，src/planner/index_select.cpp）：Filter(SeqScan) 中命中索引列的合取项改写为 残余 Filter(Fetch(IndexScan))。命中形态：`col θ const` 及镜像 `const θ col`（θ ∈ =, <>, <, ≤, >, ≥）、IS [NOT] NULL（NULL 键排最大，区间精确等于谓词）；BETWEEN 经语义层展开为两个比较后按合取项命中。常量与列同族才可进入索引键推导（int64↔Int/BigInt、double↔Double、float↔Float、bool↔Bool）。被摘除合取项的精确区间交集为键序升序的不相交区间集，交空剪成空结果节点；无适用索引维持 SeqScan（访问路径选择，非降级）
- 执行（executor）：IndexScan 迭代 = 树范围扫描 → 回表取整行（堆槽墓碑跳过）→ 残余 Filter 过滤 → 上抛 Project；SELECT/UPDATE/DELETE 的过滤子树同走此路径；EXPLAIN 渲染 Index Cond（命中条件）。UNIQUE 索引在 insert 前对键做等值预查，命中非墓碑行即拒绝（未实现）
- 依赖方向不变：executor 经 catalog 门面（create_index/drop_index/indexes/index_scan）访问存储层

## 12. 实施里程碑

| 阶段 | 内容 | 完成后可做 |
|---|---|---|
| M1 | types/codec/page 布局；file_manager 按页读写；buffer_pool；元数据表引导；heap 追加写 + 全扫描(无索引)；重启读回 | CREATE/DROP/INSERT 持久化，重启数据还在 |
| M2 | 删除(墓碑标记)；页 checksum 校验读盘 | DELETE 行 |
| M3 | 存储层二级索引：db_index 元数据表、索引文件生命周期、insert 双写、等值/范围查找与回表、建索引回填；键限 int/bigint/float/double/bool 定长编码 | 存储层可建/维护/查询索引 |
| M4 | WAL + checkpoint + recovery，接入 buffer_pool 刷盘判定（含索引页补丁与索引 DDL 记录）。已实现：物理 redo 补丁、检查点、启动重放 | 抗崩溃，事务提交语义 |
| M5 | SQL 链路：CREATE/DROP INDEX 文法与绑定、DDL 执行、planner 索引选择、IndexScan 执行已实现；UNIQUE 索引未做（见 §11） | 客户端可建/删/维护索引 |
| M6 | 索引全类型键（char/varchar 变长编码，节点单元格布局）；条目删除与下溢合并；空闲页链表与 vacuum | varchar 索引、空间回收 |
| M7(可选) | 表级锁 → 页闩锁 → MVCC | 并发读/写正确性 |

每阶段独立可编译、可测试、可回滚。M1 不引入 WAL，崩溃恢复靠"长度前缀 + 页 checksum 截断检测"，数据按追加式可丢失尾部为准，属于可接受的简化，M4 兑现完整正确性。

## 13. 简化项与已知限制

- 超长行（>约 4000B）不支持，varchar(n) 需 n ≤ 4000
- 索引键限 int/bigint/float/double/bool 定长编码，char/varchar 变长键 M6；numeric 列暂不支持索引
- 索引条目删除未实现：DELETE/UPDATE 旧行只做堆墓碑（UPDATE 新行双写新条目），旧条目滞留靠回表校验过滤；物理清理与下溢合并 M6
- M4（WAL）之前堆与索引双写无崩溃原子性：崩溃可致索引缺条目（等值查询漏行，堆链全表扫描不受影响）或悬空条目（回表按页损坏报错），重建索引可修复；M4 起补丁统一覆盖堆页与树页，双写崩溃一致性由重放保证
- 运行期检查点在 commit_txn 内触发（锁内、事务记录结束之后）：跨阈值事务的 Ok 前顺带刷盘，单个大事务（如建索引回填）期间 WAL 可超阈值无上限
- 仅单列索引，多列复合索引后续里程碑
- 无显式主键/唯一约束；rowid 照常分配但不参与定位，PRIMARY KEY/UNIQUE 待 UNIQUE 索引预查机制落地（见 §11）
- 事务为全库串行实现（恒 SERIALIZABLE），无并发事务交错与 MVCC，演进路线见 §10
- 单文件单一目录，数据库互斥，未做多库