// types.h: 存储引擎公共类型定义
#ifndef STORAGE_TYPES_H
#define STORAGE_TYPES_H

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace st {

constexpr uint32_t PAGE_SIZE = 4096;

// 页位置: 文件 id + 页号, file_id 0 为无效页
struct PageId {
    uint64_t file_id = 0;
    uint32_t page_no = 0;
    bool operator==(const PageId&) const = default;
};
constexpr PageId INVALID_PAGE{};

// 页类型
enum class PageType : uint8_t { FileHeader = 1, Heap = 2, BTreeLeaf = 3, BTreeInternal = 4 };

// 列类型
enum class ColType : uint8_t {
    Null = 0,   // 无类型: 仅用于值域, ColumnSpec.type 永不取该值
    Int = 1,
    BigInt = 2,
    Double = 3,
    VarChar = 4,
    Float = 5,  // 单精度 4B
    Char = 6,   // 定长文本
    Bool = 7,   // 布尔 1B
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
    uint64_t file_id = 0;
    std::string name;
    std::vector<ColumnSpec> cols;
};

// 行值: 语义类型 + 宽化存储; type 与 box 形态对应: Int/BigInt→int64_t,
// Float/Double→double, Char/VarChar→string, Bool→bool; 构造一律走下方工厂, 禁止聚合直造,
// 缺省即 (Null, monostate)
struct Value {
    ColType type = ColType::Null;
    std::variant<std::monostate, bool, int64_t, double, std::string> box;
};

// 值工厂: 类型与存储形态绑定, 防裸构造陷阱(字符串字面量会静默选中 bool 备选)
inline Value int_val(int64_t v) { return Value{ColType::Int, v}; }
inline Value bigint_val(int64_t v) { return Value{ColType::BigInt, v}; }
inline Value float_val(double v) { return Value{ColType::Float, v}; }
inline Value double_val(double v) { return Value{ColType::Double, v}; }
inline Value char_val(std::string v) { return Value{ColType::Char, std::move(v)}; }
inline Value str_val(std::string v) { return Value{ColType::VarChar, std::move(v)}; }
inline Value bool_val(bool v) { return Value{ColType::Bool, v}; }
inline Value null_val() { return Value{}; }  // 无类型 NULL(字面量/缺省列)
inline Value typed_null(ColType t) { return Value{t, std::monostate{}}; }  // 携带列类型的 NULL(解码)

// 判空: 仅看 box; NULL 的 type 可为 Null(无类型语境)或列类型(解码产物),
// 消费点一律先判空再按 type 分派
inline bool value_is_null(const Value& v) { return std::holds_alternative<std::monostate>(v.box); }

// 行标识: 表内单调递增, 由文件头页计数器分配
using RowId = uint64_t;

// 行物理位置: (页, 槽)
struct RowRef {
    PageId page = INVALID_PAGE;
    uint16_t slot = 0;
};

// 扫描出来的一行
struct Row {
    RowId rid = 0;  // 行标识, 堆表扫描不填充
    RowRef ref;
    std::vector<Value> values;
};

}  // namespace st
#endif