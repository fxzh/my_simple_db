// catalog.h: 表元数据目录, 启动加载进内存, DDL 时全量重写文件
#ifndef STORAGE_CATALOG_H
#define STORAGE_CATALOG_H

#include <string>
#include <vector>

#include "types.h"

namespace st {

constexpr uint32_t CATALOG_MAGIC = 0x43415444;  // "CATD"

// 目录文件格式: [magic][表数量 uint32][TableMeta × N]
// 表数量小, 查找用线性扫描
class Catalog {
public:
    // 目录文件不存在则视为空目录; 文件损坏抛 std::runtime_error
    void load(const std::string& path);
    // 全量重写目录文件
    void save(const std::string& path) const;

    const TableMeta* find(const std::string& name) const;
    const TableMeta* find_by_id(uint32_t table_id) const;
    // 当前最大表 id + 1, 空目录返回 1
    uint32_t alloc_table_id() const;

    void add_or_update(const TableMeta& meta);
    void erase(const std::string& name);

    const std::vector<TableMeta>& all() const { return tables_; }
    size_t size() const { return tables_.size(); }

private:
    std::vector<TableMeta> tables_;
};

}  // namespace st
#endif