# storage
存储引擎：页/文件/缓冲池/记录编解码/B+树/WAL，堆页追加 + 全表扫描 + 索引原语 + 崩溃恢复经 Engine 提供

- 完整设计见 docs/storage-design.md；里程碑路线：M1 堆页追加+全表扫描 → M3 存储层二级索引(堆保持主存储，树键=列值编码+RowRef 复合序，回表经 RowRef) → M4 WAL → M5 SQL 链路 → M6 全类型键与条目删除
- 结构体字段按"不做版本与迁移"约定，只存放当前里程碑实际用到的
- 表文件 t_<file_id>.dat，引擎原语与 PageId 均按 file_id 寻址，与逻辑 table_id 解耦
- 公开原语须持锁调用(锁在 catalog)，落盘性原语还须在活动事务内(Engine::begin_txn/commit_txn/rollback_txn 管理事务上下文与 undo 收集)，本库内部不加锁
- M1 持久性语义：追加写容忍尾部截断，损坏数据页按尾部截断重建空页，重启后已追加数据仍在；记录不跨页(单行约 4KB 上限，超长拒绝)
- btree 已接入 engine(建/删索引文件、条目插入、范围扫描、建索引回填经 Engine 原语)，catalog 侧待 db_index：树页统一用 MAGIC_BTREE_LEAF 魔数(type 字段区分叶/内节点，MAGIC_BTREE_INTERNAL 预留)，文件头页页头之后存根页号；树实例由 Engine 按索引文件跟踪(根页号/页分配提示，重开清空后惰性重建)，公开原语须持锁
- WAL(M4 起，事务层扩展)：记录=页补丁(after/before 双向 diff，mark_dirty 时经帧内 before 基线算出，before 侧兼作 undo 收进事务上下文)+删文件+Commit/Abort(事务号在记录头，Wal 运行期分配)；建表/元数据行修改本身是页补丁，无逻辑 DDL 记录；无 page_lsn——补丁幂等(重放/撤销=字节覆盖)且写者串行，write-ahead 由"mark_dirty 先记补丁、write_back 断言基线一致、flush 时 WAL 先于数据文件 fsync"三点保证；提交点=commit_txn 内 append(Commit)+fsync；删文件 unlink 延迟到 Commit fsync 之后(pending_drops)；delete_all_rows 逐行墓碑(ftruncate 截断不可回滚)；检查点=干净关闭/恢复完成后清空 wal.log + 运行期按累计字节触发(sync 锁外查 wal 自上次清空的字节数, 达 wal_checkpoint_bytes 阈值时取锁 checkpoint, 64KB~1GB 缺省 16MB)；崩溃恢复=Engine::open 时三遍——收集 committed/aborted 集合、顺序重放 committed、逆序撤销"无 Commit 且无 Abort"的崩溃中止事务(有 Abort 的运行期已复原，须跳过，否则覆盖后续已提交字节)，绕过缓冲池直接 pread/pwrite，尾部半记录按截断丢弃；教学式原理注释在 wal.h/recovery.cpp 文件头
