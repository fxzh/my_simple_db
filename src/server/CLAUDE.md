# 服务端(server 可执行程序)
多线程 TCP 服务器，监听端口由 db.conf 配置(位于可执行文件同目录, 缺省 8123)，
每客户端一个 detached 线程(阻塞 read)，MAX_CLIENTS=100。

# 配置(db.conf)
- 位置：可执行文件同目录，文件名固定 db.conf；缺失/无法读取时报错退出（缺失时提示先运行 initdb 生成）
- 语法：一行一项 "key = value"；空行忽略；'#' 起始为整行注释；'#' 可跟在值后作行内注释
- 未知配置项、重复配置项、值非法、行格式错误：带行号报错退出
- 当前配置项：port(监听端口, 1~65535，缺失时默认 8123)；data_dir(数据目录，存储引擎数据文件所在，
  缺失时默认 "data")

# 请求处理
1. read 一段消息(单次至多 1023 字节，无长度前缀/粘包处理)
2. "quit"/"exit" → 回"再见!"并断开；其余交给 sql::parse(msg_str, err, stmt)
3. 解析合法 → 非空语句交给 exec::execute(共享的 st::Database, *stmt)执行：
   create/drop table/insert 成功回 "OK"，delete 回 "OK (删除 N 行)"；
   执行/存储错误以 DB_RAISE 抛 DbError，handle_client 统一 catch 回客户端 "ERROR: <文案>"
4. 全程记录日志

# 要点
- 全局状态：clients 表(shared_ptr<ClientInfo>+mutex)、client_counter、server_running
- 存储引擎：一个 st::Database 实例(数据目录来自 db.conf 的 data_dir)在 main 中 open/close，
  主循环前 open、退出前 close；所有客户端线程共享它，内部 mutex 串行化
- 每线程阻塞在 read() 上，等待期间不响应其他请求
- 报错统一走 common/error.h 的 DB_RAISE：源头记一条 ERROR(带错误码与堆栈)并抛 DbError；
  handle_client 的 catch(const db::DbError&) 只把 what() 回客户端，不再重复记日志；
  非 DbError 的底层异常降级为 WARNING 记录并回客户端