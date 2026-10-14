# storage
存储引擎：页/文件/缓冲池/记录编解码/B+树/WAL，堆页追加 + 全表扫描 + 索引原语 + 崩溃恢复经 Engine 提供

- 完整设计见 docs/storage-design.md
- 值级转换统一入口为 cast.h 的 st::cast_value，按 (源值表示,目标值表示,模式) 分派内置转换函数；隐式模式仅同族提升/收窄(求值提升与写列归一)，显式模式含跨族转换(CAST)，各模块的值转换经它执行
- numeric 值的声明目标(p,s)归一走 cast.h 的 st::coerce_numeric(四舍五入远离零)；幅值上界 38 位十进制见 types.h
- 类型身份为 db_type.type_id，常用属性内联为 st::TypeAttr 随 ColumnSpec/Value 携带，代码按属性分派；属性枚举取值集合(Family/Rep/Store)与内置类型种子数据 kTypeSeeds 见 types.h
