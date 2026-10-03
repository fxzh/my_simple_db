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

#include "common/err.h"
#include "log/log.h"
#include "types.h"

namespace st {

namespace {

// 记 ERROR 日志并抛出 DbError, noreturn 供编译期确认调用点终止
[[noreturn]] void throw_errno(const std::string& what, int err)
{
    const std::string msg = what + std::strerror(err);
    DB_RAISE(db::ErrCode::IoError, LogModule::STORAGE, "{}", msg);
}

// 记 CRITICAL 日志并退出进程, noreturn 供编译期确认调用点终止
[[noreturn]] void critical_errno(const std::string& what, int err)
{
    DB_CRITICAL(LogModule::STORAGE, "{}", what + std::strerror(err));
}

}  // namespace

FileManager::FileManager(std::string dir) : dir_(std::move(dir)) {}

FileManager::~FileManager() { close_all(); }

std::string FileManager::table_file_path(uint64_t file_id) const
{
    return dir_ + "/t_" + std::to_string(file_id) + ".dat";
}

int FileManager::fd_for(uint64_t file_id)
{
    auto it = fds_.find(file_id);
    if (it != fds_.end()) {
        return it->second;
    }
    const std::string path = table_file_path(file_id);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        throw_errno("打开表文件失败: " + path + " ", errno);
    }
    fds_.emplace(file_id, fd);
    return fd;
}

uint64_t FileManager::file_size(uint64_t file_id) const
{
    auto it = fds_.find(file_id);
    if (it == fds_.end()) {
        struct stat st {};
        if (::stat(table_file_path(file_id).c_str(), &st) != 0) {
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

void FileManager::create_table_file(uint64_t file_id)
{
    const int fd = fd_for(file_id);
    if (::ftruncate(fd, static_cast<off_t>(PAGE_SIZE)) != 0) {
        throw_errno("初始化表文件失败 ", errno);
    }
}

void FileManager::remove_table_file(uint64_t file_id)
{
    if (fds_.count(file_id) != 0) {
        ::close(fds_[file_id]);
        fds_.erase(file_id);
    }
    const std::string path = table_file_path(file_id);
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        throw_errno("删除表文件失败: " + path + " ", errno);
    }
}

bool FileManager::table_file_exists(uint64_t file_id) const
{
    struct stat st {};
    return ::stat(table_file_path(file_id).c_str(), &st) == 0;
}

uint32_t FileManager::page_count(uint64_t file_id) const
{
    return static_cast<uint32_t>(file_size(file_id) / PAGE_SIZE);
}

void FileManager::read_page(uint64_t file_id, uint32_t page_no, char* out)
{
    const int fd = fd_for(file_id);
    const off_t off = static_cast<off_t>(static_cast<uint64_t>(page_no) * PAGE_SIZE);
    ssize_t n = ::pread(fd, out, PAGE_SIZE, off);
    if (n < 0) {
        critical_errno("读页失败 ", errno);
    }
    if (static_cast<size_t>(n) < PAGE_SIZE) {
        std::memset(out + n, 0, PAGE_SIZE - static_cast<size_t>(n));
    }
}

void FileManager::write_page(uint64_t file_id, uint32_t page_no, const char* data)
{
    const int fd = fd_for(file_id);
    const off_t off = static_cast<off_t>(static_cast<uint64_t>(page_no) * PAGE_SIZE);
    ssize_t n = ::pwrite(fd, data, PAGE_SIZE, off);
    if (n < 0) {
        critical_errno("写页失败 ", errno);
    }
    if (n != static_cast<ssize_t>(PAGE_SIZE)) {
        DB_CRITICAL(LogModule::STORAGE, "写页不完整");
    }
}

uint32_t FileManager::append_page(uint64_t file_id, const char* data)
{
    const uint32_t new_page_no = page_count(file_id);
    write_page(file_id, new_page_no, data);
    return new_page_no;
}

void FileManager::flush(uint64_t file_id)
{
    auto it = fds_.find(file_id);
    if (it == fds_.end()) {
        return;
    }
    if (::fsync(it->second) != 0) {
        critical_errno("fsync 失败 ", errno);
    }
}

void FileManager::flush_all()
{
    for (const auto& [file_id, fd] : fds_) {
        (void)file_id;
        if (::fsync(fd) != 0) {
            critical_errno("fsync 失败 ", errno);
        }
    }
}

void FileManager::flush_dir()
{
    const int dfd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        throw_errno("打开数据目录失败 ", errno);
    }
    if (::fsync(dfd) != 0) {
        const int err = errno;
        ::close(dfd);
        critical_errno("fsync 数据目录失败 ", err);
    }
    ::close(dfd);
}

void FileManager::close_all()
{
    for (auto& [file_id, fd] : fds_) {
        (void)file_id;
        ::close(fd);
    }
    fds_.clear();
}

}  // namespace st