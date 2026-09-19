# 服务端(server 可执行程序)
多线程 TCP 服务器，-D <数据目录> 必选(无 -D 拒绝启动)，--daemon 可选后台运行；
监听端口由数据目录内 db.conf 配置(缺省 8123)，
每客户端一个 detached 线程(阻塞 read)，MAX_CLIENTS=100。

# 配置(db.conf)
- 位置：数据目录内(-D 指定)，由 initdb -D 生成；缺失/无法读取时报错退出（缺失时提示先运行 initdb -D）；
  此时日志未初始化，报错只走控制台
- 语法：一行一项 "key = value"；空行忽略；'#' 起始为整行注释；'#' 可跟在值后作行内注释
- 未知配置项、重复配置项、值非法、行格式错误：带行号报错退出
- 当前配置项：port(监听端口, 1~65535，缺失时默认 8123)；control_socket(控制通道 socket 路径，
  缺省为数据目录/server.sock)

# 请求处理
1. 按帧收整条请求(proto)：[4B 长度][Query][SQL 原文]；长度为 0/超 MAX_REQUEST_PAYLOAD(10240) 或类型非 Query 即断连
2. "quit"/"exit" → 回 Ok 帧"再见!"并断开；其余交给 sql::parse(msg_str, err, stmt)
3. 解析合法 → 非空语句交给 exec::execute(共享的 st::Database, *stmt)执行，按 ExecResult 分流：
   create/drop table/insert 成功回 Ok 帧 "OK"，delete 回 Ok 帧 "OK (删除 N 行)"，
   select 回 ResultSet 帧(结果集，client 渲染，EXECUTOR 日志记返回行数)，空语句回显原文；
   解析失败回 Error 帧(文案)；执行/存储错误以 DB_RAISE 抛 DbError，handle_client 统一 catch 转 Error 帧
4. 全程记录日志

# 要点
- 全局状态：clients 表(shared_ptr<ClientInfo>+mutex)、client_counter、server_running
- 存储引擎：一个 st::Database 实例(数据目录来自 -D 参数)在 main 中 open/close，
  主循环前 open、退出前 close；目录未初始化(catalog.dat 缺失)时拒绝启动；
  所有客户端线程共享它，内部 mutex 串行化
- 日志：simple.log 位于数据目录内，配置加载完成后初始化日志路径
- 每线程阻塞在 recv 上，等待期间不响应其他请求
- 报错统一走 common/err.h 的 DB_RAISE：源头记一条 ERROR(带错误码与堆栈)并抛 DbError；
  handle_client 的 catch(const db::DbError&) 只把 what() 回客户端，不再重复记日志；
  非 DbError 的底层异常降级为 WARNING 记录并回客户端