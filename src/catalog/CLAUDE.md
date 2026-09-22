# 目录层(catalog 静态库，executor 链接)
元数据表逻辑 + 名字型门面：表名/列定义的查找与维护、DDL 校验、生命周期判定；
持全局 mutex，把"名字解析 + 引擎操作"复合成原子操作；报错使用 LogModule::CATALOG。

# 文件
catalog.h/.cpp  ct::Catalog 门面：create(引导两张元数据表, 落盘后返回)/open(以元数据表文件存在为准)/close、
                create_table(自动 id 从 kFirstUserTableId 起)/drop_table(保留段拒绝删除)/insert/
                delete_by_ref/delete_all/scan/row_count/table_meta(公开查找, 持锁实时扫描元数据表)；
                元数据表(db_table/db_column)为目录唯一事实来源, 无目录文件与内存缓存,
                内部查找路径 find_table_meta/has_table_name/table_id_exists/alloc_table_id 均须持锁；
                delete_meta_rows 经 Scanner 扫两张元数据表收集第 0 列等于 tid 的行引用后逐个物理删除；
                常量: 保留段(kReservedMaxTableId=20000, kFirstUserTableId=20001)、
                元数据表(kTableMetaId=1 db_table, kColumnMetaId=2 db_column)

# 要点
- 元数据表由 create() 硬编码 schema 引导：建文件+头页+自描述行；
  元数据表自身 schema 永远用硬编码定义, 不从 db_column 读自己
- create/drop 顺序：先落盘文件头页再写元数据行，避免"元数据有表但文件无效"
- 目录即元数据行：create_table 写 db_table/db_column 行并拒绝超长表/列名(64 字节)，
  drop_table 删除对应行；查找(表名→id、列定义收集)实时全扫两表, 按 ordinal 排序, 非法值报 CorruptCatalog
- scan() 游标不持锁(持有页 pin)，并发 DDL 期间扫描未定义行为；其余门面方法持锁

# 依赖
storage(PUBLIC, st::Engine 按持锁约定调用), log/common(PRIVATE, 报错/告警)
