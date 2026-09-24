# tool
独立工具集：initdb 初始化数据目录、serverctl 经控制通道管理 server 的 start/stop/status

- initdb 失败时清空目录内容(目录为本次创建则连目录一起删)，日志移至 /tmp/simple.log 保留
