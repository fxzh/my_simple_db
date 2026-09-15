#ifndef CONFIG_H
#define CONFIG_H

#include <string>

namespace config {

// 服务端配置; 字段初值即缺省值(配置项未出现时使用)
struct Config {
    int port = 8123;  // 监听端口
    std::string control_socket;  // 控制通道 socket 路径; 空串表示未配置, 缺省为数据目录/server.sock
};

// 返回数据目录内 db.conf 路径
std::string conf_path(const std::string& data_dir);

// 从配置文件加载配置: 逐行解析, 未知/重复配置项、非法值、格式错误均报错返回 false
bool load(const std::string& path, Config& cfg, std::string& error);

}

#endif