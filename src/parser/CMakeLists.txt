# SQL 词法/语法解析库(flex c++ + bison c++)
# 目前只做语法校验, 不含存储/执行

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

add_library(sql_parser STATIC
    sql_parser.cpp
    sql_scanner.cpp
    ${BISON_DBParser_OUTPUTS}
    ${FLEX_DBLexer_OUTPUTS}
)

target_include_directories(sql_parser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# flex/bison 生成代码不检查警告
if(MSVC)
    set_source_files_properties(${BISON_DBParser_OUTPUTS} ${FLEX_DBLexer_OUTPUTS}
        PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${BISON_DBParser_OUTPUTS} ${FLEX_DBLexer_OUTPUTS}
        PROPERTIES COMPILE_OPTIONS "-w")
endif()
