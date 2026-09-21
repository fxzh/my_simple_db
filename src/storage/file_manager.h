// file_manager.h: 每表一个数据文件的读写原语
#ifndef STORAGE_FILE_MANAGER_H
#define STORAGE_FILE_MANAGER_H

#include <cstdint>
#include <string>
#include <unordered_map>

namespace st {

// 管理数据目录下所有表文件: t_<table_id>.dat, 提供页级随机读写
// 使用 POSIX 文件 IO(open/pread/pwrite), 便于后续接入 fsync 与持久化
class FileManager {
public:
    explicit FileManager(std::string dir);
    ~FileManager();

    FileManager(const FileManager&) = delete;
    FileManager& operator=(const FileManager&) = delete;

    std::string dir() const { return dir_; }
    std::string table_file_path(uint64_t table_id) const;

    // 建新表文件, 初始只含文件头页
    void create_table_file(uint64_t table_id);
    // 删除表文件(只删文件, 不动目录)
    void remove_table_file(uint64_t table_id);
    bool table_file_exists(uint64_t table_id) const;

    uint32_t page_count(uint64_t table_id) const;
    void read_page(uint64_t table_id, uint32_t page_no, char* out);
    void write_page(uint64_t table_id, uint32_t page_no, const char* data);
    // 在文件末尾追加一页, 返回新页页号
    uint32_t append_page(uint64_t table_id, const char* data);

    // fsync 指定表文件
    void flush(uint64_t table_id);
    void flush_all();

    // 关闭并释放所有文件描述符
    void close_all();

private:
    int fd_for(uint64_t table_id);
    uint64_t file_size(uint64_t table_id) const;

    std::string dir_;
    std::unordered_map<uint64_t, int> fds_;
};

}  // namespace st
#endif