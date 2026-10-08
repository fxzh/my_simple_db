# utils(utils.h/utils.cpp)
跨工具通用函数：exe_dir/companion_path(可执行目录与伴生文件定位)、control_send_recv/control_shutdown(控制通道客户端)

- 零依赖：不链接 log/common/config，供 config 与 tool 保持轻量链接