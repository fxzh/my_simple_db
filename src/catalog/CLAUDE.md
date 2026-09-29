# catalog
目录层：元数据表逻辑 + 名字型门面，持全局锁把名字解析与引擎原语组合成原子操作

- 元数据表(db_table/db_column/db_schema)是目录唯一事实来源：无目录文件、无内存缓存，查找实时全扫
- db_table 记 (table_id, table_name, file_id, schema_id)；file_id 独立分配并决定表文件名，元数据表自身固定 file_id=table_id
- db_schema 记 (schema_id, schema_name)，引导写入唯一行 system(id=1)；db_table.schema_id 现阶段所有表一律挂 system 名下，schema 名字解析未接入
- 元数据表自身 schema 永远用 create() 的硬编码定义，不从 db_column 读自己(自举问题)
- create_table 先落盘表文件头页再写元数据行，drop_table 反向，避免"元数据有表但文件无效"
- scan() 返回的游标不持锁(仅持页 pin)，并发 DDL 期间扫描是未定义行为；其余门面方法持全局锁
