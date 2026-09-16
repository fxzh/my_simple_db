#ifndef PARSE_ARGS_H
#define PARSE_ARGS_H

#include <string>

// 缺省监听端口
inline constexpr int kDefaultPort = 8123;

// 启动参数解析结果
struct Options {
    int port = kDefaultPort;
    std::string host = "127.0.0.1";
    std::string sql;    // -c 载荷, 未指定时为空
};

// 解析命令行参数, 失败时已在 stderr 打印错误与用法
bool parse_args(int argc, char* argv[], Options& opts);

#endif // PARSE_ARGS_H