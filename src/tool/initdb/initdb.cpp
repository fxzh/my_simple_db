// initdb: 初始化工具, 在 -D 指定的数据目录内生成默认配置文件 db.conf 与空目录文件 catalog.dat
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "log/log.h"
#include "server/config.h"
#include "storage.h"

namespace {

// 默认配置文件内容
constexpr char kDefaultConf[] = R"(# my_simple_db 服务端配置
# 语法: 一行一项 key = value, '#' 之后为注释
# port           监听端口, 1~65535
# control_socket 控制通道 socket 路径, 不配置时为数据目录/server.sock

port = 8123
)";

// initdb 失败时日志的保留路径
constexpr char kFailedLogPath[] = "/tmp/simple.log";

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

// 数据目录就绪检查: 不存在则创建(created 置真), 路径不可用或目录非空返回 false 并填充错误描述
bool prepare_data_dir(const std::string& dir, bool& created, std::string& error)
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
    created = true;
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
    bool dir_created = false;
    if (!prepare_data_dir(dir, dir_created, error)) {
        std::cerr << error << std::endl;
        return 1;
    }
    // storage 报错走日志宏, 先设置日志路径
    Logger::initPath((std::filesystem::path(dir) / "simple.log").string());

    // 失败回滚: 日志移至 /tmp 保留, 清空目录内其余内容, 目录为本次创建则连目录一起删, 返回是否保留
    const auto rollback = [&dir, dir_created]() -> bool {
        std::error_code ec;
        std::filesystem::rename(std::filesystem::path(dir) / "simple.log", kFailedLogPath, ec);
        const bool log_kept = !ec;
        if (dir_created) {
            std::filesystem::remove_all(dir, ec);
            return log_kept;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::filesystem::remove_all(entry.path(), ec);
        }
        return log_kept;
    };

    std::string path = config::conf_path(dir);
    if (!write_default_conf(path, error)) {
        rollback();
        std::cerr << error << std::endl;
        return 1;
    }
    // 生成空目录文件, 兜 std::exception(目录不可写时 Logger 构造亦抛异常)
    try {
        st::Database db(dir);
        db.create();
    } catch (const std::exception& e) {
        const bool log_kept = rollback();
        std::cerr << e.what() << std::endl;
        if (log_kept) {
            std::cerr << "详细信息可查看 " << kFailedLogPath << std::endl;
        }
        return 1;
    }
    std::cout << "已初始化数据目录: " << dir << std::endl;
    std::cout << "可编辑配置后执行 serverctl -D " << dir << " start 启动服务" << std::endl;
    return 0;
}
