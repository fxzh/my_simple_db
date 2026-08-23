# exp2: C++ 风格的 SQL 解析器实验(flex c++ + bison c++ + AST + 内存数据库)
# 独立于主工程, 单独构建:
#   cmake -S exp2 -B exp2/build && cmake --build exp2/build
#   ./exp2/build/exp2_db
cmake_minimum_required(VERSION 3.10)
project(exp2_db LANGUAGES CXX)

# 与主工程保持一致的 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 bison 和 flex
find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

# 让 bison 生成 c++ 版 parser (%defines 会生成 parser.tab.hh)
BISON_TARGET(DBParser
    ${CMAKE_CURRENT_SOURCE_DIR}/parser.y
    ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.cc
)

# 让 flex 生成 c++ 版 scanner (%option c++)
FLEX_TARGET(DBLexer
    ${CMAKE_CURRENT_SOURCE_DIR}/lexer.l
    ${CMAKE_CURRENT_BINARY_DIR}/lex.yy.cc
)

add_executable(exp2_db
    main.cc
    ${BISON_DBParser_OUTPUTS}
    ${FLEX_DBLexer_OUTPUTS}
)

# main.cc 里 #include "parser.tab.hh", 头文件生成在构建目录
target_include_directories(exp2_db PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
