// initdb: 初始化工具, 在可执行文件同目录生成默认配置文件 db.conf
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "server/config.h"

namespace {

// 默认配置文件内容
constexpr char kDefaultConf[] = R"(# my_simple_db 服务端配置
# 语法: 一行一项 key = value, '#' 之后为注释
# port           监听端口, 1~65535
# data_dir       数据目录, 相对路径基于启动时工作目录
# control_socket 控制通道 socket 路径, 不配置时为 data_dir/server.sock

port = 8123
data_dir = data
)";

// 在指定路径写入默认配置文件, 已存在或写入失败返回 false 并填充错误描述
bool write_default_conf(const std::string& path, std::string& error)
{
    if (std::filesystem::exists(path)) {
        error = "配置文件已存在: " + path;
        return false;
    }
    std::ofstream out(path);
    if (!out.is_open()) {
        error = "无法创建配置文件: " + path;
        return false;
    }
    out << kDefaultConf;
    out.close();
    if (out.fail()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);  // 清理写坏的残留文件
        error = "写入配置文件失败: " + path;
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[])
{
    (void)argv;  // 参数不使用
    if (argc != 1) {
        std::cerr << "用法: initdb" << std::endl;
        return 2;
    }

    std::string path, error;
    if (!config::db_conf_path(path, error)) {
        std::cerr << error << std::endl;
        return 2;
    }
    if (!write_default_conf(path, error)) {
        std::cerr << error << std::endl;
        return 1;
    }
    std::cout << "已生成配置文件: " << path << std::endl;
    std::cout << "可编辑配置后执行 serverctl start 启动服务" << std::endl;
    return 0;
}
