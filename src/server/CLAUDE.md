# 服务端(server 可执行程序)
多线程 TCP 服务器，监听端口 8123，每客户端一个 detached 线程(阻塞 read)，MAX_CLIENTS=100。

# 请求处理
1. read 一段消息(单次至多 1023 字节，无长度前缀/粘包处理)
2. "quit"/"exit" → 回"再见!"并断开；其余交给 sql::parse(msg_str, err)
3. 解析合法 → 区分语句种类：create table/drop table/insert into 回 "ERROR: xxx 暂不支持"；
   空语句(空输入或仅";")原样回显；非法 → 回显 "ERROR: <信息>"(错误格式 "行.列: 描述")
4. 全程记录日志(LogModule::NETWORK / PARSER)

# 要点
- 全局状态：clients 表(shared_ptr<ClientInfo>+mutex)、client_counter、server_running
- 每线程阻塞在 read() 上，等待期间不响应其他请求
- log.h 在 ERROR 级会抛 std::runtime_error，handle_client 内有 try/catch 兜底