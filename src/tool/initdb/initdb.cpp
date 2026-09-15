// initdb: 初始化工具, 在 -D 指定的数据目录内生成默认配置文件 db.conf
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
# control_socket 控制通道 socket 路径, 不配置时为数据目录/server.sock

port = 8123
)";

// 在指定路径写入默认配置文件, 已存在或写入失败返回 false 并填充错误描述
bool write_default_conf(const std::string& path, std::string& error)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        error = "配置文件已存在: " + path;
        return false;
    }
    if (ec) {
        error = "无法访问路径: " + path;
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
        std::filesystem::remove(path, ec);  // 清理写坏的残留文件
        error = "写入配置文件失败: " + path;
        return false;
    }
    return true;
}

// 数据目录就绪检查: 不存在则创建, 路径不可用或目录非空返回 false 并填充错误描述
bool prepare_data_dir(const std::string& dir, std::string& error)
{
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        if (!std::filesystem::is_directory(dir, ec)) {
            error = "路径已存在且不是目录: " + dir;
            return false;
        }
        if (!std::filesystem::is_empty(dir, ec)) {
            error = "目录非空: " + dir;
            return false;
        }
        return true;
    }
    if (ec) {
        error = "无法访问路径: " + dir;
        return false;
    }
    std::filesystem::create_directories(dir, ec);
    if (ec || !std::filesystem::is_directory(dir, ec)) {
        error = "无法创建数据目录: " + dir;
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[])
{
    std::string data_dir_arg;
    if (argc == 3 && std::string(argv[1]) == "-D") {
        data_dir_arg = argv[2];
    }
    if (data_dir_arg.empty()) {
        std::cerr << "用法: initdb -D <数据目录>" << std::endl;
        return 2;
    }

    // 相对路径基于当前工作目录
    std::string dir = std::filesystem::absolute(data_dir_arg).string();

    std::string error;
    if (!prepare_data_dir(dir, error)) {
        std::cerr << error << std::endl;
        return 1;
    }
    std::string path = config::conf_path(dir);
    if (!write_default_conf(path, error)) {
        std::cerr << error << std::endl;
        return 1;
    }
    std::cout << "已初始化数据目录: " << dir << std::endl;
    std::cout << "可编辑配置后执行 serverctl -D " << dir << " start 启动服务" << std::endl;
    return 0;
}
