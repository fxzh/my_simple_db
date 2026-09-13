# 远程连接
代码在linux虚拟机中运行
ip 192.168.31.231
用户 hz
密码 hz（已配置免密登录）
注意：虚拟机并不是永远开着的，如果无法连接就不要再尝试了

# 远程同步
代码路径 /home/hz/test
/home/hz/updatewin.sh 用于增量同步windows代码
/home/hz/recopywin.sh 用于删除一切后全量同步windows代码
/home/hz/win 为windows挂载的共享目录，实际指向当前代码仓库

# 远程启停
当前已支持服务端控制工具 serverctl
./serverctl start 启动数据库
./serverctl stop 关闭数据库