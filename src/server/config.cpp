#include "config.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <charconv>
#include <system_error>
#include <unistd.h>

namespace config {

namespace {

// db.conf 的文件名
constexpr char kDbConfFile[] = "db.conf";

// 去掉首尾空白字符(空格/制表/换行/回车)
std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

// 获取可执行文件所在目录
bool exe_dir(std::string& dir, std::string& error)
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        error = "无法获取可执行文件路径(/proc/self/exe)";
        return false;
    }
    buf[n] = '\0';
    dir = std::filesystem::path(buf).parent_path().string();
    return true;
}

}  // namespace

bool db_conf_path(std::string& path, std::string& error)
{
    std::string dir;
    if (!exe_dir(dir, error)) {
        return false;
    }
    path = (std::filesystem::path(dir) / kDbConfFile).string();
    return true;
}

bool load(const std::string& path, Config& cfg, std::string& error)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "无法打开配置文件: " + path;
        return false;
    }

    bool seen_port = false;
    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;

        std::size_t offset = 0;
        if (line_no == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            offset = 3;  // 跳过 UTF-8 BOM
        }

        // 截断行内注释, '#' 之后的全部忽略
        std::string_view content(line);
        content.remove_prefix(offset);
        size_t hash_pos = content.find('#');
        if (hash_pos != std::string_view::npos) {
            content = content.substr(0, hash_pos);
        }
        content = trim(content);
        if (content.empty()) {
            continue;  // 空行或纯注释行
        }

        // 按第一个 '=' 切分 key / value
        size_t eq_pos = content.find('=');
        if (eq_pos == std::string_view::npos) {
            error = "第 " + std::to_string(line_no) + " 行: 缺少 '=', 应为 key = value";
            return false;
        }
        std::string_view key = trim(content.substr(0, eq_pos));
        std::string_view value = trim(content.substr(eq_pos + 1));
        if (key.empty()) {
            error = "第 " + std::to_string(line_no) + " 行: 缺少配置项名";
            return false;
        }
        if (value.empty()) {
            error = "第 " + std::to_string(line_no) + " 行: 缺少配置值";
            return false;
        }

        if (key == "port") {
            if (seen_port) {
                error = "第 " + std::to_string(line_no) + " 行: 重复配置项 port";
                return false;
            }
            int port = 0;
            const char* begin = value.data();
            auto result = std::from_chars(begin, begin + value.size(), port);
            if (result.ec != std::errc() || result.ptr != begin + value.size()) {
                error = "第 " + std::to_string(line_no) + " 行: port 值非法";
                return false;
            }
            if (port < 1 || port > 65535) {
                error = "第 " + std::to_string(line_no) + " 行: port 超出范围 1~65535";
                return false;
            }
            cfg.port = port;
            seen_port = true;
        } else {
            error = "第 " + std::to_string(line_no) + " 行: 未知配置项 " + std::string(key);
            return false;
        }
    }
    // port 未配置时保留默认值
    return true;
}

}  // namespace config