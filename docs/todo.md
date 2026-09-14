# TODO

## 已知边界

- 词法层超范围字面量: lexer.l 中 std::stoll/std::stod 遇超出整型/浮点范围的字面量抛 std::out_of_range, 绕过错误缓冲经 server 通用 catch 回客户端裸文案 "ERROR: stoll"/"ERROR: stod"; 处理方向: 捕获后按 "行.列: 描述" 格式写入词法错误缓冲
- 浮点字面量不支持科学计数法: lexer.l 的 FLOAT_NUM 规则只匹配十进制小数, "1e5" 词法为整数 1 加标识符 e5, 触发语法错误; 处理方向: 词法规则补指数部分
- codec.cpp 的 encode_row 对 float 列有 isfinite 校验, double 列没有; SQL 路径经执行层有限性校验后已无法产生 inf, 该缺口仅影响直接调用存储层的场景
