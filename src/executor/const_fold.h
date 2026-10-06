// const_fold.h: 计划期常量折叠: 计划树内纯常量子树替换为常量节点
#ifndef EXECUTOR_CONST_FOLD_H
#define EXECUTOR_CONST_FOLD_H

#include "plan.h"

namespace exec {

// 就地折叠计划树各节点持有的绑定表达式(谓词/投影/赋值/插入值), 常量运算错误当场报错
void fold_const(pl::PlanNode& plan);

}  // namespace exec

#endif  // EXECUTOR_CONST_FOLD_H
