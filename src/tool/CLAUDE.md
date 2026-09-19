# 工具
serverctl   服务端控制工具:独立可执行程序,-D <数据目录> 必选(可与子命令任意先后),子命令 start/stop/status,
            通过 unix domain socket 控制通道管理 server,start 时以 -D exec server
initdb      初始化工具:独立可执行程序,-D <数据目录> 必选;目录不存在则多级创建,存在则须为空,
            在目录内生成默认 db.conf 与空 catalog.dat(非空或 db.conf 已存在则报错退出),
            失败时回滚本次写入(目录为本次创建则连目录一起删)