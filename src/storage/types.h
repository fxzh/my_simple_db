// types.h: 存储引擎公共类型定义
#ifndef STORAGE_TYPES_H
#define STORAGE_TYPES_H

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace st {

constexpr uint32_t PAGE_SIZE = 4096;

// 页位置: 表 id + 页号, table_id 0 为无效页
struct PageId {
    uint64_t table_id = 0;
    uint32_t page_no = 0;
    bool operator==(const PageId&) const = default;
};
constexpr PageId INVALID_PAGE{};

// table_id 保留段: 1~20000 留给系统元数据对象, 用户对象从 20001 起分配
constexpr uint64_t kReservedMaxTableId = 20000;
constexpr uint64_t kFirstUserTableId = 20001;

// 元数据表(保留段固定 id): db_table 记表名, db_column 记列定义, 引导期 schema 硬编码
constexpr uint64_t kTableMetaId = 1;
constexpr uint64_t kColumnMetaId = 2;
constexpr const char* kTableMetaName = "db_table";
constexpr const char* kColumnMetaName = "db_column";

// 页类型
enum class PageType : uint8_t { FileHeader = 1, Heap = 2 };

// 列类型
enum class ColType : uint8_t {
    Int = 1,
    BigInt = 2,
    Double = 3,
    VarChar = 4,
    Float = 5,  // 单精度 4B
    Char = 6,   // 定长文本
};

// 列定义
struct ColumnSpec {
    std::string name;
    ColType type;
    uint16_t length = 0;  // Char/VarChar 的声明长度
    bool not_null = false;  // NOT NULL 约束
};

// 表元数据(目录条目)
struct TableMeta {
    uint64_t table_id = 0;
    std::string name;
    std::vector<ColumnSpec> cols;
};

// 行值: 与 parser 的字面量对应, monostate 表示 NULL
using Value = std::variant<std::monostate, int64_t, double, std::string>;

// 行物理位置: (页, 槽)
struct RowRef {
    PageId page = INVALID_PAGE;
    uint16_t slot = 0;
};

// 扫描出来的一行
struct Row {
    RowRef ref;
    std::vector<Value> values;
};

}  // namespace st
#endif