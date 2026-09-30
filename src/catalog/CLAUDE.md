# catalog
目录层：元数据表逻辑 + 名字型门面，持全局锁把名字解析与引擎原语组合成原子操作

- 元数据表(db_table/db_column/db_schema)是目录唯一事实来源：无目录文件、无内存缓存，查找实时全扫
- 初始化完成标记：bootstrap.sql 永远以建 db_version 结尾，正常模式 open() 在扫 db_table 的同一循环按名检查其存在，缺失报"未初始化或初始化未完成"，不做前向兼容
- bootstrap 模式 create table 走 SET table_id 的显式 id 路径：未 set(值 0)或 id 被占用报错，查重扫 db_table；正常模式自动分配不变
- db_table 记 (table_id, table_name, file_id, schema_id)；table_id/file_id 均为 open() 扫 db_table(已分配 id 的唯一登记处)取最大值、内存原子递增不回收；file_id 决定表文件名，元数据表自身固定 file_id=table_id，用户 file_id 不走 1~20000 保留段，可与保留段数值重叠；用户 table_id 走保留段，起点不低于 kFirstUserTableId
- db_schema 记 (schema_id, schema_name)，引导写入唯一行 system(id=1)；db_table.schema_id 现阶段所有表一律挂 system 名下，schema 名字解析未接入
- 元数据表自身 schema 永远用 create() 的硬编码定义，不从 db_column 读自己(自举问题)
- create_table 先落盘表文件头页再写元数据行，drop_table 反向，避免"元数据有表但文件无效"
- scan() 返回的游标不持锁(仅持页 pin)，并发 DDL 期间扫描是未定义行为；其余门面方法持全局锁
