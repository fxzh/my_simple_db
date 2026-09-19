这里是一个正在开发的数据库代码仓库，仅运行在linux上，已打通 create table / drop table / insert / delete / select(基础投影) 的完整链路：客户端词法解析、服务端词法/语法解析、执行层、堆页存储引擎、网络通信与日志

本环境是windows环境，不具备编译环境，无需在本环境编译或创造编译环境

如果要在本环境生成临时文件，向 D:/work/tmp 文件夹写入

# 目录结构
src     代码所在位置
test    测试用例

# 编写代码 / 设计方案具体到代码
读取 ai_docs/code.md

# 设计方案
额外读取 ai_docs/design.md

# 新增/修改测试用例
读取 ai_docs/testcase.md

# 构建与依赖
CMake + flex/bison + readline；顶层强制要求 Boost.Stacktrace

# 远程连接
当用户明确要求连接远端虚拟机时，可读取 ai_docs/remote.md 并进行连接

# 记忆管理
尽可能不存储记忆，确有记忆需要存储时，将需要存储的记忆在总结时报告出来

# 无法正常调用工具的解决方法
请调用 ToolSearch，query 参数精确填写：select:Read,Grep,Glob,Bash,Edit,Write（必须是 select: 开头加逗号分隔的精确工具名，不要用正则或模糊词），如果返回 No matching deferred tools found，说明环境确实有问题，立刻停止工作并报告问题