# 源文件
set(SRC_FILES
    client.cpp
)

# 查找 flex
find_package(FLEX REQUIRED)

# 让 flex 生成 scanner
FLEX_TARGET(ClientLexer
    ${CMAKE_CURRENT_SOURCE_DIR}/client.l
    ${CMAKE_CURRENT_BINARY_DIR}/client_lex.c
)

# 可执行文件包含 flex 生成的 C 文件
add_executable(client
    ${SRC_FILES}
    ${CMAKE_CURRENT_BINARY_DIR}/client_lex.c
)

# 包含目录：当前源目录与生成目录
target_include_directories(client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# 链接 readline（或 libedit）在类 Unix 系统
find_library(READLINE_LIB NAMES readline)
if(READLINE_LIB)
    target_link_libraries(client PRIVATE ${READLINE_LIB})
else()
    find_library(LIBEDIT_LIB NAMES edit)
    if(LIBEDIT_LIB)
        target_link_libraries(client PRIVATE ${LIBEDIT_LIB})
    else()
        message(FATAL_ERROR "readline lib not found.")
    endif()
endif()

# 平台特定的链接库
if(WIN32)
    target_link_libraries(client ws2_32)
endif()

# 链接 Boost（如果顶层找到）
if(Boost_FOUND)
    target_include_directories(client PRIVATE ${Boost_INCLUDE_DIRS})
endif()

# 设置可执行文件的属性
set_target_properties(client PROPERTIES
    OUTPUT_NAME "client"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 可选的：安装规则
install(TARGETS client
    RUNTIME DESTINATION bin
    CONFIGURATIONS Release
)