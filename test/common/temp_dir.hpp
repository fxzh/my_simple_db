#ifndef TEST_COMMON_TEMP_DIR_H
#define TEST_COMMON_TEMP_DIR_H

#include <string>

namespace tcommon {

// 唯一临时目录: mkdtemp 创建, 析构按 keep 决定删除或保留现场
struct TempDir {
    std::string path;   // 目录绝对路径; 空串表示未创建
    bool keep = false;  // true 时析构保留目录并打印路径

    TempDir() = default;
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    // 在系统临时目录下创建 "<prefix>_XXXXXX" 唯一子目录
    bool create(const std::string& prefix, std::string& error);

    // 递归删除目录, 失败返回 false 并填充 error
    bool remove(std::string& error);

    ~TempDir();
};

// 读取文件末尾至多 max_bytes 字节, 供失败诊断时附上服务端日志摘录
bool read_file_tail(const std::string& path, size_t max_bytes, std::string& content,
                    std::string& error);

}

#endif
