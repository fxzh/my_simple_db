// codec.cpp: 行序列化/反序列化
#include "codec.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace st {

namespace {

inline void put_u16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

inline void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

inline uint16_t read_u16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t read_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                  (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool get_int64(const Value& v, int64_t* out)
{
    const int64_t* p = std::get_if<int64_t>(&v);
    if (p == nullptr) {
        return false;
    }
    *out = *p;
    return true;
}

inline bool get_double(const Value& v, double* out)
{
    const double* p = std::get_if<double>(&v);
    if (p == nullptr) {
        return false;
    }
    *out = *p;
    return true;
}

inline bool get_string(const Value& v, std::string* out)
{
    const std::string* p = std::get_if<std::string>(&v);
    if (p == nullptr) {
        return false;
    }
    *out = *p;
    return true;
}

}  // namespace

bool encode_row(const std::vector<ColumnSpec>& cols, const std::vector<Value>& values,
                std::vector<uint8_t>& out)
{
    if (cols.size() != values.size()) {
        return false;
    }
    out.clear();
    out.reserve(16 + values.size() * 8);
    out.push_back(0);  // 长度占位(低 8 位)
    out.push_back(0);  // 长度占位(高 8 位)
    // NULL 位图: 每列 1 bit, 1 表示 NULL, NULL 列不占列数据区字节
    out.insert(out.end(), (cols.size() + 7) / 8, 0);

    for (size_t i = 0; i < cols.size(); ++i) {
        const ColumnSpec& col = cols[i];
        if (std::holds_alternative<std::monostate>(values[i])) {
            if (col.not_null) {
                return false;  // NOT NULL 列拒绝 NULL
            }
            out[2 + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            continue;
        }
        switch (col.type) {
            case ColType::Int: {
                int64_t v = 0;
                if (!get_int64(values[i], &v) || v < INT32_MIN || v > INT32_MAX) {
                    return false;
                }
                put_u32(out, static_cast<uint32_t>(static_cast<int32_t>(v)));
                break;
            }
            case ColType::BigInt: {
                int64_t v = 0;
                if (!get_int64(values[i], &v)) {
                    return false;
                }
                put_u32(out, static_cast<uint32_t>(v));
                put_u32(out, static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32));
                break;
            }
            case ColType::Double: {
                double v = 0.0;
                if (!get_double(values[i], &v)) {
                    return false;
                }
                uint64_t bits = 0;
                std::memcpy(&bits, &v, sizeof(bits));
                put_u32(out, static_cast<uint32_t>(bits));
                put_u32(out, static_cast<uint32_t>(bits >> 32));
                break;
            }
            case ColType::VarChar: {
                std::string v;
                if (!get_string(values[i], &v)) {
                    return false;
                }
                if (v.size() > UINT16_MAX) {
                    return false;
                }
                if (col.length > 0 && v.size() > col.length) {
                    return false;  // 超出声明长度拒绝
                }
                put_u16(out, static_cast<uint16_t>(v.size()));
                out.insert(out.end(), v.begin(), v.end());
                break;
            }
            case ColType::Float: {
                double v = 0.0;
                if (!get_double(values[i], &v)) {
                    return false;
                }
                const float f = static_cast<float>(v);
                if (std::isfinite(v) && !std::isfinite(f)) {
                    return false;  // 超出 float 可表示范围
                }
                uint32_t bits = 0;
                std::memcpy(&bits, &f, sizeof(bits));
                put_u32(out, bits);
                break;
            }
            case ColType::Char: {
                std::string v;
                if (!get_string(values[i], &v)) {
                    return false;
                }
                const uint16_t n = col.length;
                if (v.size() > n) {
                    return false;  // 超长拒绝
                }
                out.insert(out.end(), v.begin(), v.end());
                out.insert(out.end(), static_cast<size_t>(n) - v.size(), ' ');  // 不足补空格
                break;
            }
        }
    }

    const uint16_t body_len = static_cast<uint16_t>(out.size() - 2);
    out[0] = static_cast<uint8_t>(body_len & 0xff);
    out[1] = static_cast<uint8_t>((body_len >> 8) & 0xff);
    return true;
}

bool decode_row(const std::vector<ColumnSpec>& cols, const uint8_t* data,
                                size_t len, std::vector<Value>& out)
{
    if (len < 2) {
        return false;
    }
    const uint16_t body_len = read_u16(data);
    if (static_cast<size_t>(body_len) + 2 != len) {
        return false;
    }
    // NULL 位图与编码侧同布局, 位图不完整视为损坏
    const size_t bitmap_len = (cols.size() + 7) / 8;
    if (2 + bitmap_len > len) {
        return false;
    }
    const uint8_t* bitmap = data + 2;
    out.clear();
    out.reserve(cols.size());
    size_t pos = 2 + bitmap_len;
    for (size_t i = 0; i < cols.size(); ++i) {
        const ColumnSpec& col = cols[i];
        if ((bitmap[i / 8] >> (i % 8)) & 1u) {
            out.emplace_back();  // NULL: monostate
            continue;
        }
        switch (col.type) {
            case ColType::Int: {
                if (pos + 4 > len) {
                    return false;
                }
                const int32_t v = static_cast<int32_t>(read_u32(data + pos));
                out.emplace_back(static_cast<int64_t>(v));
                pos += 4;
                break;
            }
            case ColType::BigInt: {
                if (pos + 8 > len) {
                    return false;
                }
                const uint64_t lo = read_u32(data + pos);
                const uint64_t hi = read_u32(data + pos + 4);
                out.emplace_back(static_cast<int64_t>(lo | (hi << 32)));
                pos += 8;
                break;
            }
            case ColType::Double: {
                if (pos + 8 > len) {
                    return false;
                }
                const uint64_t bits = read_u32(data + pos) |
                                        (static_cast<uint64_t>(read_u32(data + pos + 4)) << 32);
                double v = 0.0;
                std::memcpy(&v, &bits, sizeof(v));
                out.emplace_back(v);
                pos += 8;
                break;
            }
            case ColType::VarChar: {
                if (pos + 2 > len) {
                    return false;
                }
                const uint16_t slen = read_u16(data + pos);
                pos += 2;
                if (pos + slen > len) {
                    return false;
                }
                out.emplace_back(std::string(reinterpret_cast<const char*>(data + pos), slen));
                pos += slen;
                break;
            }
            case ColType::Float: {
                if (pos + 4 > len) {
                    return false;
                }
                uint32_t bits = read_u32(data + pos);
                float f = 0.0f;
                std::memcpy(&f, &bits, sizeof(f));
                out.emplace_back(static_cast<double>(f));
                pos += 4;
                break;
            }
            case ColType::Char: {
                if (pos + col.length > len) {
                    return false;
                }
                std::string v(reinterpret_cast<const char*>(data + pos), col.length);
                pos += col.length;
                // 去掉尾部填充空格(定长 char 语义)
                while (!v.empty() && v.back() == ' ') {
                    v.pop_back();
                }
                out.emplace_back(std::move(v));
                break;
            }
        }
    }
    return true;
}

}  // namespace st