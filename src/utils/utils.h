// utils.h: 跨工具通用函数: 可执行目录定位与控制通道客户端
#ifndef UTILS_UTILS_H
#define UTILS_UTILS_H

#include <string>

namespace utils {

// 定位当前可执行文件所在目录, 失败返回 false 并填充错误描述
bool exe_dir(std::string& dir, std::string& error);

// 定位与当前可执行文件同目录的伴生文件, 缺失或不可访问返回 false 并填充错误描述
bool companion_path(const char* name, std::string& path, std::string& error);

// 经控制通道发送一条命令并读取全部回复直到 EOF, 失败返回 false 并填充错误描述
bool control_send_recv(const std::string& socket_path, const std::string& cmd,
                       std::string& reply, std::string& error);

// 经控制通道发送 shutdown 并确认回复 OK
bool control_shutdown(const std::string& sock_path, std::string& error);

}  // namespace utils

#endif  // UTILS_UTILS_H
