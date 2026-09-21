# 存储引擎(storage 静态库，executor 链接)
M1 = 堆页追加 + 全表扫描；M3 起加入 B+树聚簇索引，M4 加 WAL。完整设计见 docs/storage-design.md；
结构体字段按"不做版本与迁移"约定，只存放当前里程碑实际用到的。

# 文件
types.h         公共类型：PageId(结构体 table_id:u64+page_no:u32)、ColType、Value(variant, monostate 即 NULL)、TableMeta、Row、
                table_id 保留段常量(kReservedMaxTableId=20000, kFirstUserTableId=20001)、
                元数据表常量(kTableMetaId=1 db_table 记表名, kColumnMetaId=2 db_column 记列定义)
page.h/.cpp     页头 24B(magic/type/slot_count/free_begin/free_end/next_page/checksum)+槽(off,len)
                槽从页尾向 free_begin 生长；页尾 CRC32 校验，数据页损坏按尾部截断重建空页
codec.h/.cpp    记录[长度 u16][NULL 位图][列数据]序列化；类型名解析 int/bigint/float/double/char(n)/varchar(n)
file_manager    每表一个 t_<table_id>.dat，pread/pwrite 页级 IO，fsync/flush，fd 按需打开缓存
                使用 POSIX 文件 IO(open/pread/pwrite/fsync)，标准 C++ 无跨平台替代
buffer_pool     定长帧缓存(默认 128)：Clock 淘汰 + pin 计数 + dirty 页写回；并发由上层锁保证
storage.h/.cpp  Database 门面：create(引导两张元数据表, 落盘后返回)/open(以元数据表文件存在为准)/close、
                create_table(自动 id 从 kFirstUserTableId 起)/drop_table(保留段拒绝删除)/insert/scan/
                table_meta(公开查找, 持锁实时扫描元数据表)；
                元数据表(db_table/db_column)为目录唯一事实来源, 无目录文件与内存缓存,
                内部查找路径 read_rows/find_table_meta/has_table_name/table_id_exists/alloc_table_id 均须持锁；
                全局 mutex 串行化；
                tail_pages_ 跟踪"仅存内存的尾页"，新页号取 max(磁盘页数, 尾页+1)；
                init_table_file/insert_impl 为须持锁的内部路径, 元数据表引导与用户路径共用

# 要点/限制
- M1 无索引/无主键，scan 全表扫，insert 返回 RowRef(页,槽)；重启后数据仍在(appended 截断容忍)
- 元数据表由 create() 硬编码 schema 引导：建文件+头页+自描述行，select 可查；
  执行层拦截保留表名 drop/insert/delete，存储层 drop 按保留段拒绝、create 靠重名拒绝；
  元数据表自身 schema 永远用硬编码定义, 不从 db_column 读自己
- 目录即元数据行：create_table 写 db_table/db_column 行并拒绝超长表/列名(64 字节)，
  drop_table 删除对应行；查找(表名→id、列定义收集)实时全扫两表, 按 ordinal 排序, 非法值报 CorruptCatalog
- crate/drop 顺序：先落盘文件头页再写元数据行，避免"元数据有表但文件无效"
- scan() 不加锁(游标持有页 pin)，并发 DDL 期间扫描未定义行为；row_count() 加锁
- 记录 ≤4068B，NULL 经记录头位图存储(NULL 列不占字节, NOT NULL 列拒绝 NULL)，表最多约 4000B/行；跨页记录不支持(将来 M3 树内处理)
- 超长 char(n)/varchar(n) 由 insert 按声明长度拒绝；未带长度的 varchar 以记录上限为界