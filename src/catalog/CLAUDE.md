# catalog
目录层：元数据表逻辑 + 名字型门面，持全局锁把名字解析与引擎原语组合成原子操作

- 元数据表(db_table/db_column/db_schema)是目录唯一事实来源：无目录文件、无内存缓存，查找实时全扫
- 初始化完成标记：bootstrap.sql 永远以建 db_version 结尾，正常模式 open() 在扫 db_table 的同一循环按 system 名下检查其存在(用户 schema 同名表不算数)，缺失报"未初始化或初始化未完成"，不做前向兼容
- bootstrap 模式 create table 走 SET table_id 的显式 id 路径：未 set(值 0)或 id 被占用报错，查重扫 db_table；正常模式自动分配不变
- db_table 记 (table_id, table_name, file_id, schema_id)；table_id/file_id 均为 open() 扫 db_table(已分配 id 的唯一登记处)取最大值、内存原子递增不回收；file_id 决定表文件名，元数据表自身固定 file_id=table_id，用户 file_id 不走 1~20000 保留段，可与保留段数值重叠；用户 table_id 走保留段，起点不低于 kFirstUserTableId
- db_schema 记 (schema_id, schema_name)，引导写入唯一行 system(id=1)；门面按限定名(TableRef)解析，schema 为空即未限定、默认挂 system，表名 schema 内唯一，create table 挂限定名所属 schema
- 三张自举表(db_table/db_column/db_schema)自身 schema 永远用 create() 的硬编码定义，不从 db_column 读自己(自举问题)；db_index 是经正常建表路径建的保留表，列定义不硬编码，open() 从 db_column 载入内存缓存后读写均用缓存，bootstrap 建表时直接填充(此时该表尚不存在)
- create_table 先落盘表文件头页再写元数据行，drop_table 反向（先删元数据行再删文件），避免"元数据有表但文件无效"；索引同序：create_index 先建索引文件并全表回填再写 db_index 行，drop_index/drop_table 级联删索引反向
- db_index 记 (table_id, index_name, col_ordinal, file_id)——列定义的唯一事实来源是 bootstrap.sql 经 db_column 登记，索引名表内唯一(跨表可同名)，查重/存在性校验在 catalog 持锁完成；insert/update 双写该表全部索引条目，update 为墓碑+追加(旧行条目残留，回表按墓碑过滤)；open 扫 db_table+db_index 取 file_id 分配起点(bootstrap 模式 db_index 由 bootstrap.sql 在 open 后创建，缺失跳过；正常模式缺失 DB_CRITICAL 退出进程)
- scan() 返回的游标不持锁(仅持页 pin)，并发 DDL 期间扫描是未定义行为；其余门面方法持全局锁
- 落盘性门面(建删表/建删 schema/插删改行)须在活动事务内调用(事务外报 Internal)，事务由调用方经 begin_txn/commit_txn/rollback_txn 门面包裹：锁随事务长持(mutex 为 recursive_mutex，门面方法同线程递归重入)，commit_txn 是唯一提交点(Commit 记录 fsync + WAL 阈值检查点)；调用方约定见 server 层——会话按语句级自动提交包裹，显式事务状态机随事务层后续里程碑接入
