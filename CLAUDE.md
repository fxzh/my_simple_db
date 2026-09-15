这里是一个用 c++20 编写的数据库代码仓库，已打通 create table / drop table / insert / delete 的完整链路：客户端词法解析、服务端词法/语法解析、执行层、堆页存储引擎、网络通信与日志

本环境是windows环境，不具备编译环境，无需在本环境编译或创造编译环境

如果要在本环境生成临时文件，向 D:/work/tmp 文件夹写入

设计要求：尽可能规避 fallback 设计，如果无法规避，停下设计并向用户说明原因

开始编写代码前或设计方案具体到代码时，读取 ai_docs/code.md 获取通用编写要求；修改完成后按 code.md 要求在总结末尾附规则自检清单

# 目录结构
src     代码所在位置

# 构建与依赖
CMake + flex/bison + readline；顶层强制要求 Boost.Stacktrace
客户端与服务端词法器均以 %option c++ 生成 C++ 扫描器，生成代码统一加 -w 抑制告警

# 远程连接
当用户明确要求连接远端虚拟机时，可读取 ai_docs/remote.md 并进行连接

# 无法正常调用工具的解决方法
请调用 ToolSearch，query 参数精确填写：select:Read,Grep,Glob,Bash,Edit,Write（必须是 select: 开头加逗号分隔的精确工具名，不要用正则或模糊词），如果返回 No matching deferred tools found，说明环境确实有问题，立刻停止工作并报告问题