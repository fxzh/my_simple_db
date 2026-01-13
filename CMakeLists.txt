cmake_minimum_required(VERSION 3.10)
project(my_simple_db VERSION 0.3.0 LANGUAGES C CXX)

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
# Add an option to treat warnings as errors: `WARNINGS_AS_ERRORS` (default OFF)

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

option(WARNINGS_AS_ERRORS "Treat warnings as errors (add -Werror or /WX)" OFF)

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
    message(WARNING "Boost.Stacktrace not found.")
endif()

# 在 UNIX（非 MSVC）上添加 -rdynamic 以便运行时能打印符号表（Boost.Stacktrace 需要）
if(UNIX AND NOT MSVC)
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
endif()

# 添加子目录
add_subdirectory(src/server)
add_subdirectory(src/client)