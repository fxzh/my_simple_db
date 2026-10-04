#ifndef NET_H
#define NET_H

#include <string>

// 创建 TCP 监听 socket: bootstrap 模式仅本机监听且端口由内核分配; 实际监听端口写回 listen_port,
// 失败经 DB_CRITICAL 记日志并退出进程, 仅成功返回
int create_tcp_listener(bool bootstrap_mode, int& listen_port);

// 创建控制通道监听 socket: 清理残留路径后绑定, 仅属主可读写; 失败经 DB_CRITICAL 退出进程
int create_control_listener(const std::string& ctl_sock);

// 受理一条控制通道连接并处理命令, 不建线程; 返回是否收到 shutdown
bool accept_control_command(int control_fd);

// 登记控制通道 status 回复所需的实际监听端口并记录就绪时刻, 进入主循环前调用一次
void set_control_status_info(int listen_port);

#endif
