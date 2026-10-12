# storage
存储引擎：页/文件/缓冲池/记录编解码/B+树/WAL，堆页追加 + 全表扫描 + 索引原语 + 崩溃恢复经 Engine 提供

- 完整设计见 docs/storage-design.md
- 值级隐式转换统一入口为 cast.h 的 st::cast_value，按 (源域,目标域) 分派内置转换函数，各模块的值转换经它执行
