# 帧协议(proto, header-only)
client 与 server 的 TCP 消息格式：[4B 网络序 payload 长度][1B 消息类型][body]。
无 CMake 目标，经顶层 include_directories(src) 以 "proto/proto.h" 引用；不依赖 log/common，client 端可直接使用

# 消息类型
Query(请求方向, body=SQL 原文)、Ok(命令标签, body=[tag u8][count u64] 大端序 9 字节, tag 为 CommandTag 枚举, count 为影响行数)、Error(错误文案)、ResultSet(结果集, body=列名+行值的大端序编码, 编解码函数与布局见 proto.h)

# 收发
- send_frame/recv_frame 处理部分读写与 EINTR；recv_frame 的 max_payload 传 0 表示不限
- 服务端请求校验 MAX_REQUEST_PAYLOAD=10240(与 client 的 SQL_BUFFER_LIMIT 一致)，长度为 0/超限或类型非 Query 直接断连，不回帧
- 回复方向暂无长度上限，结果集整段物化后一次成帧

# 演进约定
帧格式不变；结果集单帧物化，流式分帧(分段+结束标记)留作将来扩展
