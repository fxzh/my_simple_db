# 帧协议(proto, header-only)
client 与 server 的 TCP 消息格式：[4B 网络序 payload 长度][1B 消息类型][body]。
无 CMake 目标，经顶层 include_directories(src) 以 "proto/proto.h" 引用；不依赖 log/common，client 端可直接使用

# 消息类型
Query(请求方向, body=SQL 原文)、Ok(状态文本)、Error(错误文案)、ResultSet(结果集, SELECT 接入时启用, body 未定义)

# 收发
- send_frame/recv_frame 处理部分读写与 EINTR；recv_frame 的 max_payload 传 0 表示不限
- 服务端请求校验 MAX_REQUEST_PAYLOAD=10240(与 client 的 SQL_BUFFER_LIMIT 一致)，长度为 0/超限或类型非 Query 直接断连，不回帧
- 回复方向暂无长度上限，结果集整段物化后一次成帧

# 演进约定
SELECT 接入时启用 ResultSet 并定义 body 结构与 client 渲染，帧格式不变；流式分帧(分段+结束标记)留作将来扩展
