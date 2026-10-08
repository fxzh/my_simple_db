# src 模块划分
client   客户端可执行程序
parser   服务端 SQL 词法/语法解析
analyzer 语义分析层
expr     表达式求值
planner  计划层
executor 执行层
server   服务端可执行程序
log      日志
common    跨层错误库
utils    通用函数
config    配置
proto    帧协议头文件
storage  文件引擎
catalog  目录层
tool     独立工具
