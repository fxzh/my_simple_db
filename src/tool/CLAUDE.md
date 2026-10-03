# tool
独立工具集：initdb 初始化数据目录、serverctl 经控制通道管理 server 的 start/stop/status

- initdb 失败时清空目录内容(目录为本次创建则连目录一起删)，日志移至 /tmp/simple.log 保留
- initdb 的 bootstrap 阶段拉起同目录伴生 server(--bootstrap)，读到端口行后经伴生 client 执行伴生 bootstrap.sql，再经控制通道关闭
