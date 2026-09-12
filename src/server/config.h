#ifndef CONFIG_H
#define CONFIG_H

#include <string>

namespace config {

// 服务端配置; 字段初值即缺省值(配置项未出现时使用)
struct Config {
    int port = 8123;  // 监听端口
    std::string data_dir = "data";  // 数据目录: 存储引擎数据文件与目录文件所在目录
};

// 返回 db.conf 完整路径(可执行文件同目录), 失败返回 false 并填充错误描述
bool db_conf_path(std::string& path, std::string& error);

// 从配置文件加载配置: 逐行解析, 未知/重复配置项、非法值、格式错误均报错返回 false
bool load(const std::string& path, Config& cfg, std::string& error);

}

#endif