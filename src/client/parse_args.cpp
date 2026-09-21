#include <charconv>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "parse_args.h"

using ParseFn = bool (*)(std::string_view value, Options& opts);

struct OptionSpec {
    const char* flag;         // 形如 "-p"
    const char* name;         // 中文名, 用于报错与用法提示
    bool takes_value;         // 是否取值, 无值选项仅支持独立形式
    ParseFn parse;            // 取值并校验, 失败时已打印错误并返回 false
};

static bool parse_port(std::string_view value, Options& opts)
{
    int parsed = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        std::cerr << "错误: 端口值非法: " << value << std::endl;
        return false;
    }
    if (parsed < 1 || parsed > 65535) {
        std::cerr << "错误: 端口超出范围 1~65535: " << parsed << std::endl;
        return false;
    }
    opts.port = parsed;
    return true;
}

static bool parse_host(std::string_view value, Options& opts)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, std::string(value).c_str(), &addr) != 1) {
        std::cerr << "错误: 主机地址非法: " << value << std::endl;
        return false;
    }
    opts.host.assign(value);
    return true;
}

static bool parse_sql(std::string_view value, Options& opts)
{
    if (value.empty()) {
        std::cerr << "错误: SQL文本为空" << std::endl;
        return false;
    }
    opts.sql.assign(value);
    return true;
}

// -a 无值选项, 置位回显开关
static bool parse_echo(std::string_view, Options& opts)
{
    opts.echo_all = true;
    return true;
}

static const OptionSpec kOptions[] = {
    {"-p", "端口号", true, parse_port},
    {"-h", "主机地址", true, parse_host},
    {"-c", "SQL文本", true, parse_sql},
    {"-a", "回显原始SQL", false, parse_echo},
};

// 打印命令行用法
static void print_usage(const char* prog)
{
    std::cerr << "用法: " << prog;
    for (const auto& spec : kOptions) {
        if (spec.takes_value) {
            std::cerr << " [" << spec.flag << " " << spec.name << "]";
        } else {
            std::cerr << " [" << spec.flag << "]";
        }
    }
    std::cerr << std::endl;
}

bool parse_args(int argc, char* argv[], Options& opts)
{
    bool seen[sizeof(kOptions) / sizeof(kOptions[0])] = {false};

    // 按选项表逐项解析, 取值参数支持 -p 8123 与 -p8123 两种写法, 无值参数仅支持独立形式
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        const OptionSpec* spec = nullptr;
        std::string_view value;

        // 匹配选项: 独立形式 "-p" 值取下一参数, 黏连形式 "-p8123" 取前缀之后的字段
        for (const auto& candidate : kOptions) {
            std::string_view flag(candidate.flag);
            if (arg == flag) {
                spec = &candidate;
                if (candidate.takes_value) {
                    if (i + 1 >= argc) {
                        std::cerr << "错误: " << flag << " 后缺少" << candidate.name << std::endl;
                        print_usage(argv[0]);
                        return false;
                    }
                    value = argv[++i];
                }
                break;
            }
            if (candidate.takes_value && arg.size() > flag.size()
                && arg.starts_with(candidate.flag)) {
                spec = &candidate;
                value = arg.substr(flag.size());
                break;
            }
        }
        if (spec == nullptr) {
            std::cerr << "错误: 未知参数: " << arg << std::endl;
            print_usage(argv[0]);
            return false;
        }
        std::size_t index = static_cast<std::size_t>(spec - kOptions);
        if (seen[index]) {
            std::cerr << "错误: 重复指定" << spec->name << std::endl;
            print_usage(argv[0]);
            return false;
        }
        if (!spec->parse(value, opts)) {
            print_usage(argv[0]);
            return false;
        }
        seen[index] = true;
    }
    return true;
}