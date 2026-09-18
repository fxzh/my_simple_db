# 存储引擎(storage 静态库，executor 链接)
M1 = 堆页追加 + 全表扫描；M3 起加入 B+树聚簇索引，M4 加 WAL。完整设计见 docs/storage-design.md；
结构体字段按"不做版本与迁移"约定，只存放当前里程碑实际用到的。

# 文件
types.h         公共类型：PageId(table_id<<32|page_no)、ColType、Value(variant, monostate 即 NULL)、TableMeta、Row
page.h/.cpp     页头 24B(magic/type/slot_count/free_begin/free_end/next_page/checksum)+槽(off,len)
                槽从页尾向 free_begin 生长；页尾 CRC32 校验，数据页损坏按尾部截断重建空页
codec.h/.cpp    记录[长度 u16][NULL 位图][列数据]序列化；类型名解析 int/bigint/float/double/char(n)/varchar(n)
file_manager    每表一个 t_<table_id>.dat，pread/pwrite 页级 IO，fsync/flush，fd 按需打开缓存
                使用 POSIX 文件 IO(open/pread/pwrite/fsync)，标准 C++ 无跨平台替代
buffer_pool     定长帧缓存(默认 128)：Clock 淘汰 + pin 计数 + dirty 页写回；并发由上层锁保证
catalog.h/.cpp  目录文件[CATD][表数量][TableMeta×N]，启动加载进内存，DDL 全量重写
storage.h/.cpp  Database 门面：create_table/drop_table/insert/scan/table_meta(公开只读元数据)；
                全局 mutex 串行化；
                tail_pages_ 跟踪"仅存内存的尾页"，新页号取 max(磁盘页数, 尾页+1)

# 要点/限制
- M1 无索引/无主键，scan 全表扫，insert 返回 RowRef(页,槽)；重启后数据仍在(appended 截断容忍)
- crate/drop 顺序：先落盘文件头页再写目录，避免"目录有表但文件无效"
- scan() 不加锁(游标持有页 pin)，并发 DDL 期间扫描未定义行为；row_count() 加锁
- 记录 ≤4068B，NULL 经记录头位图存储(NULL 列不占字节, NOT NULL 列拒绝 NULL)，表最多约 4000B/行；跨页记录不支持(将来 M3 树内处理)
- 超长 varchar 系统定位 clamp 在记录上限内由 insert 运行时拒绝