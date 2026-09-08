#ifndef CLIENT_H
#define CLIENT_H

#include <cstddef>
#include <string>

// 单条 SQL 字节上限, 超限时报错并丢弃本轮输入
constexpr std::size_t SQL_BUFFER_LIMIT = 10240;

enum ScannerState {
    STATE_INITIAL,
    STATE_SINGLE,
    STATE_DOUBLE,
    STATE_COMMENT
};

extern ScannerState scanner_state;
extern std::string sql_buffer;

void reset_sql_buffer();
void append_to_sql(const char* text, std::size_t len);
void send_to_server();

#endif // CLIENT_H