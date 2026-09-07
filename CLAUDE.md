这里是一个用 c++20 编写的数据库代码仓库，当前仅完成了客户端词法解析，服务端少量的词法语法解析，两端之间的网络通信，最基础的日志功能
本环境是windows环境，不具备编译功能，因此修改后无需寻找编译工具进行编译
设计要求：尽可能规避 fallback 设计，如果无法规避，停下设计并向用户说明原因
代码要求：尽可能使用 c++20 进行编写，如果某处必须使用 c语言，需要在总结时说明原因
注释要求：使用中文进行注释，注释必须简洁，注释禁止写入修改原因，注释禁止写入"如果不这样做就会xxx"类似的语句

# 目录结构
src/client   交互式客户端：readline 收输入，flex(client.l) 累积 SQL，遇 ';' 发往服务端
src/parser   服务端 SQL 解析静态库(sql_parser)：flex c++ + bison c++，仅语法校验，被 server 链接
src/server   多线程 TCP 服务端：每客户端一个线程，sql::parse 校验后原样回显或返回 ERROR
src/log      日志库(log.h)：纯头文件单例，异步队列写 simple.log
src/storage 存储引擎静态库(storage)：M1 已实现堆页追加+全表扫描(页/缓冲池/目录/编解码)，B+树与WAL为后续里程碑，设计见 docs/storage-design.md

# 构建与依赖
CMake + flex/bison + readline；顶层强制要求 Boost.Stacktrace(缺失即报错)。
客户端词法器生成 C，服务端解析器生成 C++，因此编译选项对 C/C++ 分开设置

# 远程连接
代码在linux虚拟机中运行，ip 192.168.31.231，用户 hz，密码 hz，但要注意虚拟机并不是永远开着的，如果无法连接就不要再尝试了