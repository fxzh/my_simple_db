// types.h: 存储引擎公共类型定义
#ifndef STORAGE_TYPES_H
#define STORAGE_TYPES_H

#include <boost/multiprecision/cpp_int.hpp>
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

// 值族: db_type 行 family 列取值, 代码侧词表
enum class Family : uint8_t { None = 0, Num = 1, Text = 2, Bool = 3 };

// 值形态: db_type 行 rep 列取值, 与 Value 盒形态一一对应, 代码侧词表
enum class Rep : uint8_t { None = 0, Int64 = 1, Float = 2, Double = 3, Str = 4, Bool = 5, Dec = 6 };

// 存储形态: db_type 行 store 列取值, 代码侧词表
enum class Store : uint8_t { None = 0, Fixed = 1, CharPad = 2, VarLen = 3, Decimal = 4 };

// 类型属性: db_type 行热字段的内联形态, 随 ColumnSpec/Value 携带供各层按属性分派;
// id 取 db_type.type_id, 0 为无类型(仅值域 NULL), 其余字段与 id 出自同一行
struct TypeAttr {
    uint16_t id = 0;
    Family family = Family::None;
    uint8_t rank = 0;   // 数值提升等级, 非数值 0
    Rep rep = Rep::None;
    Store store = Store::None;
    uint8_t width = 0;  // 定宽字节数, 变长 0
    bool operator==(const TypeAttr&) const = default;
};

// 自举内核: db_type 种子行数据, 引导写入 db_type 并供元数据表列定义与测试直接构造属性;
// 新增类型在此加行, 语法关键字与各层行为分派另行同步
struct TypeSeed {
    TypeAttr attr;
    const char* name;
    const char* alias;       // nullptr 表示无别名
    bool len_allowed;        // 是否接受声明长度
    uint16_t len_default;    // 未声明时的缺省长度
    uint16_t len_max;        // 长度上限
    bool scale_allowed;      // 是否接受声明标度
};

inline constexpr TypeSeed kTypeSeeds[] = {
        {{1, Family::Num, 1, Rep::Int64, Store::Fixed, 4}, "int", nullptr, false, 0, 0, false},
        {{2, Family::Num, 2, Rep::Int64, Store::Fixed, 8}, "bigint", nullptr, false, 0, 0, false},
        {{3, Family::Num, 5, Rep::Double, Store::Fixed, 8}, "double", nullptr, false, 0, 0, false},
        {{4, Family::Text, 0, Rep::Str, Store::VarLen, 0}, "varchar", nullptr, true, 0, 65535,
         false},
        {{5, Family::Num, 4, Rep::Float, Store::Fixed, 4}, "float", nullptr, false, 0, 0, false},
        {{6, Family::Text, 0, Rep::Str, Store::CharPad, 0}, "char", nullptr, true, 1, 65535,
         false},
        {{7, Family::Bool, 0, Rep::Bool, Store::Fixed, 1}, "boolean", "bool", false, 0, 0, false},
        {{8, Family::Num, 3, Rep::Dec, Store::Decimal, 0}, "numeric", "decimal", true, 0, 38,
         true},
};

// 自举内核各类型属性(命名常量)
inline constexpr TypeAttr kInt = kTypeSeeds[0].attr;
inline constexpr TypeAttr kBigint = kTypeSeeds[1].attr;
inline constexpr TypeAttr kDouble = kTypeSeeds[2].attr;
inline constexpr TypeAttr kVarChar = kTypeSeeds[3].attr;
inline constexpr TypeAttr kFloat = kTypeSeeds[4].attr;
inline constexpr TypeAttr kChar = kTypeSeeds[5].attr;
inline constexpr TypeAttr kBool = kTypeSeeds[6].attr;
inline constexpr TypeAttr kNumeric = kTypeSeeds[7].attr;

// 列定义
struct ColumnSpec {
    std::string name;
    TypeAttr type;
    uint16_t length = 0;  // Char/VarChar 的声明长度, Numeric 的声明精度(0 为无约束)
    bool not_null = false;  // NOT NULL 约束
    uint8_t scale = 0;  // Numeric 的声明标度, 须不超 length, 其余类型恒 0
};

// 表元数据(目录条目)
struct TableMeta {
    uint64_t table_id = 0;
    uint64_t file_id = 0;
    std::string name;
    std::vector<ColumnSpec> cols;
};

// numeric 幅值上界: unscaled 幅值须 < 10^38(38 位十进制)
inline const boost::multiprecision::int256_t kNumericUnscaledMax =
    boost::multiprecision::pow(boost::multiprecision::int256_t(10), 38);

// numeric 值形态: unscaled 为按 scale 放大的整数值, scale 随值走(0..255, 尾零不剥)
struct DecimalVal {
    boost::multiprecision::int256_t unscaled;
    uint8_t scale = 0;
    bool operator==(const DecimalVal&) const = default;
};

// 行值: 类型属性 + 存储形态; rep 与 box 形态对应: Int64→int64_t,
// Float→float, Double→double, Str→string, Bool→bool, Dec→DecimalVal;
// 构造一律走下方工厂, 禁止聚合直造, 缺省即无类型 NULL
struct Value {
    TypeAttr type;
    std::variant<std::monostate, bool, int64_t, double, float, std::string, DecimalVal> box;
};

// 值工厂: 属性与盒形态绑定, 防裸构造陷阱(字符串字面量会静默选中 bool 备选)
inline Value int_val(TypeAttr t, int64_t v) { return Value{t, v}; }
inline Value float_val(TypeAttr t, float v) { return Value{t, v}; }
inline Value double_val(TypeAttr t, double v) { return Value{t, v}; }
inline Value str_val(TypeAttr t, std::string v) { return Value{t, std::move(v)}; }
inline Value bool_val(TypeAttr t, bool v) { return Value{t, v}; }
inline Value numeric_val(TypeAttr t, boost::multiprecision::int256_t unscaled, uint8_t scale)
{
    return Value{t, DecimalVal{std::move(unscaled), scale}};
}
inline Value null_val() { return Value{}; }  // 无类型 NULL(字面量/缺省列)
inline Value typed_null(TypeAttr t) { return Value{t, std::monostate{}}; }  // 携带列类型的 NULL(解码)

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