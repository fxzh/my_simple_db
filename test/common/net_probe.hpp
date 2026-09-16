#ifndef TEST_COMMON_NET_PROBE_H
#define TEST_COMMON_NET_PROBE_H

#include <string>

namespace tcommon {

// 探测 TCP 端口是否可连(host 为 IPv4 点分地址), 单次尝试含 200ms 连接等待
bool port_is_open(const std::string& host, int port);

// 轮询等待端口就绪(每 50ms 一次, 对齐 pg_regress 节奏), 超时返回 false 并在 error
// 中给出 host:port 与超时时长
bool wait_port_ready(const std::string& host, int port, int timeout_ms, std::string& error);

}

#endif
