# storage
存储引擎：页/文件/缓冲池/记录编解码/B+树，堆页追加 + 全表扫描 + 索引原语经 Engine 提供

- 完整设计见 docs/storage-design.md；里程碑路线：M1 堆页追加+全表扫描 → M3 存储层二级索引(堆保持主存储，树键=列值编码+RowRef 复合序，回表经 RowRef) → M4 WAL → M5 SQL 链路 → M6 全类型键与条目删除
- 结构体字段按"不做版本与迁移"约定，只存放当前里程碑实际用到的
- 表文件 t_<file_id>.dat，引擎原语与 PageId 均按 file_id 寻址，与逻辑 table_id 解耦
- 公开原语须持锁调用(锁在 catalog)，本库内部不加锁
- M1 持久性语义：追加写容忍尾部截断，损坏数据页按尾部截断重建空页，重启后已追加数据仍在；记录不跨页(单行约 4KB 上限，超长拒绝)
- btree 已接入 engine(建/删索引文件、条目插入、范围扫描、建索引回填经 Engine 原语)，catalog 侧待 db_index：树页统一用 MAGIC_BTREE_LEAF 魔数(type 字段区分叶/内节点，MAGIC_BTREE_INTERNAL 预留)，文件头页页头之后存根页号；树实例由 Engine 按索引文件跟踪(根页号/页分配提示，重开清空后惰性重建)，公开原语须持锁
