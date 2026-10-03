# config
配置静态库：db.conf 逐项解析写入全局变量 cfg，零依赖

- server_log_level 值校验用本模块内独立级别名表，与 log 模块 LogLevel 枚举名须保持一致
