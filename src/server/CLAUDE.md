# server
服务端：多线程 TCP，-D <数据目录>，读目录内 db.conf 配置端口与控制通道

- 启动顺序：读 db.conf 在日志初始化之前，缺失/非法时报错只走控制台；simple.log 路径在配置加载后才设置
- 全进程单个 ct::Catalog 实例，主循环前 open、退出前 close，被所有客户端线程共享
- --bootstrap：前台自举模式，仅监听 127.0.0.1，端口由内核临时分配(配置 port 忽略)
- 事务: 锁在事务首条语句取；显式事务内任何语句报错（含解析失败）整事务立即回滚并结束，错误帧文案带"事务已回滚"；事务内拒绝 DDL 与 SET；bootstrap 会话拒绝 BEGIN；断连/quit 清理代码回滚未结束事务
- 会话变量 client_msg_level（缺省 info，值为 LogLevel 名）
- 全局变量 server_log_level（缺省 info，任意会话可 SET 调整，立即全局生效）
- 错误帧带 wire 码：db::ErrCode 经 to_wire 穷尽 switch 映射；解析失败/事务控制/受理层超限在会话层直接定码，非 DbError 异常降级为 Internal
- 自动提交语句为单语句事务：execute 失败回滚后异常上抛，成功 commit_txn(持久化边界)后才回 Ok；SELECT 在算子 open 后先提交释放锁，结果集锁外流式发送，显式事务内的 SELECT 在锁内流式发送
