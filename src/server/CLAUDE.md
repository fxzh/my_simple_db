# 服务端(server 可执行程序)
多线程 TCP 服务器，监听端口由 db.conf 配置(位于可执行文件同目录, 缺省 8123)，
每客户端一个 detached 线程(阻塞 read)，MAX_CLIENTS=100。

# 配置(db.conf)
- 位置：可执行文件同目录，文件名固定 db.conf；缺失/无法读取时报错退出
- 语法：一行一项 "key = value"；空行忽略；'#' 起始为整行注释；'#' 可跟在值后作行内注释
- 未知配置项、重复配置项、值非法、行格式错误：带行号报错退出
- 当前配置项：port(监听端口, 1~65535)，缺失时默认 8123

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