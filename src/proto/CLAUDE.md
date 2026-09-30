# proto
TCP 帧协议头文件：长度前缀 + 消息类型

- MAX_REQUEST_PAYLOAD 与 client 的 SQL_BUFFER_LIMIT 取值一致(10240)，改动需两侧同步
- 结果集三帧流式：ResultSetHead(列名) → 若干 ResultSetBatch(攒批行，RS_BATCH_MAX_ROWS 为发送侧批上限，接收侧不感知) → ResultSetEnd(总行数)；空结果只有头帧+结束帧；错误帧可出现在流中间，客户端此时丢弃已收行
