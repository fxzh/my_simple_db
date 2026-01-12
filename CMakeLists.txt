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
    message(WARNING "Boost.Stacktrace not found. Install libboost-stacktrace-dev (Ubuntu) or provide Boost. Continuing without stacktrace support.")
endif()

# 在 UNIX（非 MSVC）上添加 -rdynamic 以便运行时能打印符号表（Boost.Stacktrace 需要）
if(UNIX AND NOT MSVC)
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
endif()

# 添加子目录
add_subdirectory(src/server)
add_subdirectory(src/client)