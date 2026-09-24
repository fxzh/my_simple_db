# proto
TCP 帧协议头文件：长度前缀 + 消息类型

- MAX_REQUEST_PAYLOAD 与 client 的 SQL_BUFFER_LIMIT 取值一致(10240)，改动需两侧同步
- 结果集整段物化后单帧回复，回复方向暂无长度上限；流式分帧(分段+结束标记)留作将来扩展，帧格式预期不变
