// file_manager.cpp: 文件打开/页读写/文件大小管理
#include "file_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include "types.h"

namespace st {

namespace {

[[noreturn]] void throw_errno(const std::string& what, int err) {
    throw std::runtime_error(what + std::strerror(err));
}

}  // namespace

FileManager::FileManager(std::string dir) : dir_(std::move(dir)) {}

FileManager::~FileManager() { close_all(); }

std::string FileManager::table_file_path(uint32_t table_id) const {
    return dir_ + "/t_" + std::to_string(table_id) + ".dat";
}

int FileManager::fd_for(uint32_t table_id) {
    auto it = fds_.find(table_id);
    if (it != fds_.end()) {
        return it->second;
    }
    const std::string path = table_file_path(table_id);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        throw_errno("打开表文件失败: " + path + " ", errno);
    }
    fds_.emplace(table_id, fd);
    return fd;
}

uint64_t FileManager::file_size(uint32_t table_id) const {
    auto it = fds_.find(table_id);
    if (it == fds_.end()) {
        struct stat st {};
        if (::stat(table_file_path(table_id).c_str(), &st) != 0) {
            return 0;
        }
        return static_cast<uint64_t>(st.st_size);
    }
    struct stat st {};
    if (::fstat(it->second, &st) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(st.st_size);
}

void FileManager::create_table_file(uint32_t table_id) {
    const int fd = fd_for(table_id);
    if (::ftruncate(fd, static_cast<off_t>(PAGE_SIZE)) != 0) {
        throw_errno("初始化表文件失败 ", errno);
    }
}

void FileManager::remove_table_file(uint32_t table_id) {
    if (fds_.count(table_id) != 0) {
        ::close(fds_[table_id]);
        fds_.erase(table_id);
    }
    const std::string path = table_file_path(table_id);
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        throw_errno("删除表文件失败: " + path + " ", errno);
    }
}

bool FileManager::table_file_exists(uint32_t table_id) const {
    struct stat st {};
    return ::stat(table_file_path(table_id).c_str(), &st) == 0;
}

uint32_t FileManager::page_count(uint32_t table_id) const {
    return static_cast<uint32_t>(file_size(table_id) / PAGE_SIZE);
}

void FileManager::read_page(uint32_t table_id, uint32_t page_no, char* out) {
    const int fd = fd_for(table_id);
    const off_t off = static_cast<off_t>(static_cast<uint64_t>(page_no) * PAGE_SIZE);
    ssize_t n = ::pread(fd, out, PAGE_SIZE, off);
    if (n < 0) {
        throw_errno("读页失败 ", errno);
    }
    if (static_cast<size_t>(n) < PAGE_SIZE) {
        std::memset(out + n, 0, PAGE_SIZE - static_cast<size_t>(n));
    }
}

void FileManager::write_page(uint32_t table_id, uint32_t page_no, const char* data) {
    const int fd = fd_for(table_id);
    const off_t off = static_cast<off_t>(static_cast<uint64_t>(page_no) * PAGE_SIZE);
    ssize_t n = ::pwrite(fd, data, PAGE_SIZE, off);
    if (n < 0) {
        throw_errno("写页失败 ", errno);
    }
    if (n != static_cast<ssize_t>(PAGE_SIZE)) {
        throw std::runtime_error("写页不完整");
    }
}

uint32_t FileManager::append_page(uint32_t table_id, const char* data) {
    const uint32_t new_page_no = page_count(table_id);
    write_page(table_id, new_page_no, data);
    return new_page_no;
}

void FileManager::flush(uint32_t table_id) {
    auto it = fds_.find(table_id);
    if (it == fds_.end()) {
        return;
    }
    if (::fsync(it->second) != 0) {
        throw_errno("fsync 失败 ", errno);
    }
}

void FileManager::flush_all() {
    for (const auto& [table_id, fd] : fds_) {
        (void)table_id;
        if (::fsync(fd) != 0) {
            throw_errno("fsync 失败 ", errno);
        }
    }
}

void FileManager::close_all() {
    for (auto& [table_id, fd] : fds_) {
        (void)table_id;
        ::close(fd);
    }
    fds_.clear();
}

}  // namespace st