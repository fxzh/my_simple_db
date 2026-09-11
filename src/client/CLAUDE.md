# 客户端(client 可执行程序)
交互式 SQL 客户端：readline 读取输入，flex 扫描器(client.l)把输入累积到 sql_buffer，
识别 '--' 注释与单/双引号字符串，遇到 ';' 才把整条 SQL 经 TCP(端口缺省 8123)发往服务端并阻塞等待回显；
累积超过 SQL_BUFFER_LIMIT 时客户端报错并丢弃本轮输入。

# 启动参数
./client [-p 端口号] [-h 主机地址]    缺省端口 8123、主机 127.0.0.1；也支持黏连写法
未知参数、重复指定参数、部分值非法/超范围：报错并打印用法退出
参数解析为选项表驱动：新增参数只需在 kOptions 表添加表项与对应解析函数

# 文件
parse_args.cpp  启动参数解析：Options 结果结构、选项表 kOptions 与 parse_args 解析循环，解析失败打印错误与用法
parse_args.h    声明 Options 与 parse_args，kDefaultPort 缺省端口常量
client.cpp   主程序：连接 127.0.0.1:8123、readline 循环、收发消息；持有全程复用的 yyFlexLexer 实例与
             sql_buffer/sql_overflow 状态；append_to_sql 触发上限时报错并置溢出标志跳过发送流程；
             quit/exit 单独短路直接发送
client.h     SQL_BUFFER_LIMIT 常量、ScannerState 枚举与 sql_buffer/append_to_sql 等声明，供 client.cpp 与扫描器共用
client.l     flex 扫描器：状态机(COMMENT/STRING_SINGLE/STRING_DOUBLE)累积 SQL，
             ';' 触发 send_to_server()；生成的 client_lex.yy.cc 在构建目录

# 交互
scanner_state 决定 readline 提示符("SQL> " / "SQL>' " / "SQL>\" ")，实现多行输入感知；
输入未到 ';' 时只积累不发送。服务端回显原样 SQL 或 "ERROR: ..."