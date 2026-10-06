cmake_minimum_required(VERSION 3.16)
project(my_simple_db LANGUAGES C CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置输出目录
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# 项目选项
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(ENABLE_TESTING "Enable testing" OFF)

# Use -O2 for Release builds (prefer -O2 over -O3), and add many warning flags

# Ensure Release uses -O2 instead of -O3 for non-MSVC, and /O2 for MSVC
if(NOT MSVC)
    if(CMAKE_CXX_FLAGS_RELEASE MATCHES "-O3")
        string(REPLACE "-O3" "-O2" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
    endif()
else()
    # MSVC: ensure /O2 present in Release flags
    if(NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "/O2")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2")
    endif()
endif()

option(WARNINGS_AS_ERRORS "Treat warnings as errors (add -Werror or /WX)" ON)

# Warning flags per compiler
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(CXX_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wold-style-cast
        -Woverloaded-virtual
        -Wnon-virtual-dtor
        -Wformat=2
        -Wnull-dereference
        -Wdouble-promotion
        -Wmissing-include-dirs
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wcast-align
        -Winit-self
        # 内存/生命周期
        -Wmismatched-new-delete
        -Wdelete-non-virtual-dtor
        -Wdelete-incomplete
        -Wuse-after-free=3
        -Wfree-nonheap-object
        -Wplacement-new=2
        -Wzero-length-bounds
        # 悬垂引用/指针与移动语义
        -Wdangling-reference
        -Wdangling-pointer
        -Winit-list-lifetime
        -Wself-move
        -Wredundant-move
        -Wpessimizing-move
        -Wrange-loop-construct
        # cast 强化
        -Wuseless-cast
        -Wcast-qual
        -Wcast-user-defined
        -Wsign-promo
        -Winvalid-offsetof
        -Warith-conversion
        # 溢出/越界(-O2 下生效)
        -Warray-bounds=2
        -Wformat-overflow=2
        -Wformat-truncation=2
        -Wformat-signedness
        -Wstringop-overflow=4
        -Wstrict-overflow=2
        -Wshift-overflow=2
        -Warray-parameter=2
        # 预处理/词法安全
        -Wundef
        -Wkeyword-macro
        -Wchanges-meaning
        -Wexpansion-to-defined
        -Wunused-macros
        -Wbidi-chars=any
        -Winvalid-utf8
        -Wnormalized
        # VLA/alloca 禁用
        -Wvla
        -Walloca
        -Walloc-zero
        # C++ 语义/ABI
        -Wvolatile
        -Wregister
        -Wterminate
        -Wcatch-value=3
        -Wvexing-parse
        -Wctad-maybe-unsupported
        -Wcomma-subscript
        -Wvirtual-move-assign
        -Winaccessible-base
        -Wsuggest-override
        -Wredundant-decls
        -Wnoexcept-type
        -Wtemplate-id-cdtor
        -Wmissing-template-keyword
        -Wmissing-requires
        -Wextra-semi
        -Wliteral-suffix
        -Wmultichar
        -Wzero-as-null-pointer-constant
        -Wstrict-null-sentinel
        -Wconditionally-supported
        -Winterference-size
        -Wnon-c-typedef-for-linkage
        -Wattribute-alias=2
        -Wimplicit-fallthrough=5
        -Wdate-time
        -Wmissing-noreturn
        -Wdeprecated-enum-enum-conversion
        -Wdeprecated-enum-float-conversion
        -Wflex-array-member-not-at-end
        -Wtrampolines
        -Wpacked
        -Wmissing-declarations
        -Wredundant-tags
    )
    set(C_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wformat=2
        -Wdouble-promotion
        -Wmissing-include-dirs
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wcast-align
        -Winit-self
        # 与 CXX 侧同源
        -Wuseless-cast
        -Wcast-qual
        -Wundef
        -Wunused-macros
        -Wvla
        -Walloca
        -Walloc-zero
        -Wuse-after-free=3
        -Wfree-nonheap-object
        -Wzero-length-bounds
        -Wdangling-pointer
        -Wflex-array-member-not-at-end
        -Wbidi-chars=any
        -Wkeyword-macro
        -Wexpansion-to-defined
        -Wattribute-alias=2
        -Wimplicit-fallthrough=5
        -Wmultichar
        -Wzero-as-null-pointer-constant
        -Wformat-overflow=2
        -Wformat-truncation=2
        -Wformat-signedness
        -Wstringop-overflow=4
        -Wstrict-overflow=2
        -Wshift-overflow=2
        -Warray-parameter=2
        -Warray-bounds=2
        -Wdate-time
        -Wmissing-noreturn
        -Wredundant-decls
        -Winvalid-utf8
        -Wnormalized
        -Wtrampolines
        -Wpacked
        -Wno-unused-function    # flex/bison may generate unused functions
    )

    if(WARNINGS_AS_ERRORS)
        list(APPEND CXX_WARNING_FLAGS -Werror)
        list(APPEND C_WARNING_FLAGS -Werror)
    endif()
elseif(MSVC)
    # /W4 is the highest generally safe warning level; add /permissive- to enforce standard conformance
    set(CXX_WARNING_FLAGS /W4 /permissive-)
    set(C_WARNING_FLAGS /W4 /permissive-)
    if(WARNINGS_AS_ERRORS)
        list(APPEND CXX_WARNING_FLAGS /WX)
        list(APPEND C_WARNING_FLAGS /WX)
    endif()
else()
    set(CXX_WARNING_FLAGS )
    set(C_WARNING_FLAGS )
endif()

# Apply the warning flags globally
if(CXX_WARNING_FLAGS OR C_WARNING_FLAGS)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:${CXX_WARNING_FLAGS}>"
        "$<$<COMPILE_LANGUAGE:C>:${C_WARNING_FLAGS}>"
    )
endif()

# ---------------------------------------------------------------------------

# 默认构建类型（针对单配置生成器）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# 查找 Boost（只需要头文件）
find_path(BOOST_STACKTRACE_INCLUDE_DIR
    NAMES boost/stacktrace.hpp
    HINTS /usr/include /usr/local/include
)

if(BOOST_STACKTRACE_INCLUDE_DIR)
    set(BoostFound TRUE)
    set(Boost_INCLUDE_DIRS ${BOOST_STACKTRACE_INCLUDE_DIR})
else()
    message(FATAL_ERROR "Boost.Stacktrace not found.")
endif()

# 在 UNIX（非 MSVC）上添加 -rdynamic 以便运行时能打印符号表（Boost.Stacktrace 需要）
if(UNIX AND NOT MSVC)
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
endif()

# 添加子目录
add_subdirectory(src/log)
add_subdirectory(src/common)
add_subdirectory(src/config)
add_subdirectory(src/utils)
add_subdirectory(src/parser)
add_subdirectory(src/storage)
add_subdirectory(src/catalog)
add_subdirectory(src/analyzer)
add_subdirectory(src/expr)
add_subdirectory(src/planner)
add_subdirectory(src/executor)
add_subdirectory(src/server)
add_subdirectory(src/client)
add_subdirectory(src/tool)

# 测试目录（ENABLE_TESTING=ON 时启用）
if(ENABLE_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()