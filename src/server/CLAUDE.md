# server
服务端：多线程 TCP，-D <数据目录>，读目录内 db.conf 配置端口与控制通道

- 启动顺序：读 db.conf 在日志初始化之前，缺失/非法时报错只走控制台；simple.log 路径在配置加载后才设置
- 全进程单个 ct::Catalog 实例，主循环前 open、退出前 close，被所有客户端线程共享(靠其内部 mutex 串行化)
