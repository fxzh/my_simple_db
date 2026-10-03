#include "config/config.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <charconv>
#include <system_error>

namespace config {

// 全局配置变量, 声明见 config.h
Config cfg;

namespace {

// db.conf 的文件名
constexpr char kDbConfFile[] = "db.conf";

// 整型配置项的键名与取值范围; 字符串项不进表, 单独处理
struct IntSpec {
    std::string_view key;
    long long min;
    long long max;
};

constexpr IntSpec kIntSpecs[] = {
    {"port", 1, 65535},
    {"buffer_pool_frames", 16, 1048576},
    {"wal_checkpoint_bytes", 64 * 1024, 1024 * 1024 * 1024},
};

// 按键名查整型配置项范围, 无则返回 nullptr
const IntSpec* int_spec(std::string_view key)
{
    for (const auto& spec : kIntSpecs) {
        if (spec.key == key) {
            return &spec;
        }
    }
    return nullptr;
}

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

// 日志级别名集合(小写), 与 log 模块 LogLevel 枚举名一致, 修改须两侧同步
constexpr std::string_view kLogLevelNames[] = {
    "debug5", "debug4", "debug3", "debug2", "debug", "info",
    "notice", "warning", "error", "critical",
};

// 判断值是否为合法日志级别名, 大小写不敏感
bool is_log_level(std::string_view value)
{
    const auto lower = [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    };
    for (std::string_view known : kLogLevelNames) {
        bool match = value.size() == known.size();
        for (std::size_t j = 0; match && j < value.size(); ++j) {
            match = lower(value[j]) == known[j];
        }
        if (match) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string conf_path(const std::string& data_dir)
{
    return (std::filesystem::path(data_dir) / kDbConfFile).string();
}

bool load(const std::string& path, std::string& error)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "无法打开配置文件: " + path;
        return false;
    }

    std::vector<std::string> seen;  // 已出现的配置项名
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

        for (const auto& s : seen) {
            if (s == key) {
                error = "第 " + std::to_string(line_no) + " 行: 重复配置项 " + std::string(key);
                return false;
            }
        }

        // 整型项: 查范围表统一解析与校验
        const IntSpec* spec = int_spec(key);
        long long num = 0;
        if (spec != nullptr) {
            const char* begin = value.data();
            auto result = std::from_chars(begin, begin + value.size(), num);
            if (result.ec != std::errc() || result.ptr != begin + value.size()) {
                error = "第 " + std::to_string(line_no) + " 行: " + std::string(key) + " 值非法";
                return false;
            }
            if (num < spec->min || num > spec->max) {
                error = "第 " + std::to_string(line_no) + " 行: " + std::string(key)
                        + " 超出范围 " + std::to_string(spec->min) + "~" + std::to_string(spec->max);
                return false;
            }
        }

        if (key == "port") {
            cfg.port = static_cast<int>(num);
        } else if (key == "buffer_pool_frames") {
            cfg.buffer_pool_frames = static_cast<size_t>(num);
        } else if (key == "wal_checkpoint_bytes") {
            cfg.wal_checkpoint_bytes = static_cast<size_t>(num);
        } else if (key == "control_socket") {
            cfg.control_socket = std::string(value);
        } else if (key == "server_log_level") {
            if (!is_log_level(value)) {
                error = "第 " + std::to_string(line_no) + " 行: " + std::string(key)
                        + " 值非法";
                return false;
            }
            cfg.server_log_level = std::string(value);
        } else {
            error = "第 " + std::to_string(line_no) + " 行: 未知配置项 " + std::string(key);
            return false;
        }
        seen.emplace_back(key);
    }
    // 配置项未出现时保留字段默认值
    return true;
}

}  // namespace config