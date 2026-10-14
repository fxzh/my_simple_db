// render.h: 客户端渲染族: 回复帧文本展示与结果集表格渲染
#ifndef CLIENT_RENDER_H
#define CLIENT_RENDER_H

#include <cstdint>
#include <string>

#include "proto/proto.h"

// 消息帧打印: 级别名 + 文案; 是否发送已由 server 按会话级别过滤, 此处不再筛
void print_notice(uint8_t level, const std::string& text);

// 命令标签展示文本: 标签关键词, insert/delete/update 附影响行数
std::string command_display(proto::CommandTag tag, uint64_t count);

// 结果集渲染为表格: 列宽取表头与各单元格的最大字节宽, 数据行左对齐, 表头列内居中,
// 每列前后各一空格且补齐列宽, 列间 '|' 分隔, 表头下每列 '-' × (列宽+2) 以 '+' 连接,
// 末列不补尾空格, 末行输出 (N 行)
void render_result_set(const proto::ResultSet& rs);

// 结果集流式接收: 头帧后的批次帧物化累积, 结束帧校验总行数; 流中 Error 帧丢弃已收行
bool recv_result_stream(proto::ResultSet& rs);

#endif // CLIENT_RENDER_H
