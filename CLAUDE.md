这里是一个正在开发的数据库代码仓库，仅运行在linux上，目前正在快速演进中

本环境是windows环境，不具备编译环境，无需在本环境编译或创造编译环境

如果要在本环境生成临时文件，向 D:/work/tmp 文件夹写入，没有 D:/work/tmp 时，写入 C:/tmp 文件夹

# 目录结构
src     代码所在位置
test    测试用例
大部分目录下都存在对应的 CLAUDE.md 用来快速了解目录作用

# 编写代码 / 修改代码 / 设计方案具体到代码
读取 ai_docs/code.md

# 设计方案 / 分析方案
额外读取 ai_docs/design.md

# 新增/修改测试用例 / 手动测试
读取 ai_docs/testcase.md

# 构建与依赖
代码使用CMake构建，依赖于 flex/bison + readline + Boost.Stacktrace

# 远程连接
当用户明确要求连接远端虚拟机时，可读取 ai_docs/remote.md 并进行连接

# 记忆管理
不主动存储记忆，如果有记忆需要存储时，将需要存储的记忆在总结时报告出来

# 无法正常调用工具的解决方法
请调用 ToolSearch，query 参数精确填写：select:Read,Grep,Glob,Bash,Edit,Write（必须是 select: 开头加逗号分隔的精确工具名，不要用正则或模糊词）