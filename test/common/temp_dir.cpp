// temp_dir.cpp: 唯一临时目录与文件尾部读取
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <vector>

#include <stdlib.h>

#include "common/temp_dir.hpp"

namespace tcommon {

bool TempDir::create(const std::string& prefix, std::string& error)
{
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        error = "获取系统临时目录失败: " + ec.message();
        return false;
    }
    std::string tmpl = (base / (prefix + "_XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data()) == nullptr) {
        error = "mkdtemp 失败: " + std::string(std::strerror(errno));
        return false;
    }
    path = buf.data();
    keep = false;
    return true;
}

bool TempDir::remove(std::string& error)
{
    if (path.empty()) {
        error = "目录未创建";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        error = "删除临时目录失败: " + path + ": " + ec.message();
        return false;
    }
    return true;
}

TempDir::~TempDir()
{
    if (path.empty()) {
        return;
    }
    if (keep) {
        std::cerr << "保留临时目录现场: " << path << std::endl;
        return;
    }
    std::string error;
    if (!remove(error)) {
        std::cerr << "删除临时目录失败(现场保留): " << error << std::endl;
    }
}

bool read_file_tail(const std::string& path, size_t max_bytes, std::string& content,
                    std::string& error)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        error = "无法打开文件: " + path;
        return false;
    }
    std::streamoff size = in.tellg();
    if (size < 0) {
        error = "无法获取文件大小: " + path;
        return false;
    }
    std::streamoff begin = size > static_cast<std::streamoff>(max_bytes)
                                   ? size - static_cast<std::streamoff>(max_bytes)
                                   : static_cast<std::streamoff>(0);
    in.seekg(begin);
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}
