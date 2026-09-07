// codec.cpp: 行序列化/反序列化
#include "codec.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace st {

namespace {

inline void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

inline void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                  (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool get_int64(const Value& v, int64_t* out) {
    const int64_t* p = std::get_if<int64_t>(&v);
    if (p == nullptr) {
        return false;
    }
    *out = *p;
    return true;
}

inline bool get_double(const Value& v, double* out) {
    const double* p = std::get_if<double>(&v);
    if (p == nullptr) {
        return false;
    }
    *out = *p;
    return true;
}

inline bool get_string(const Value& v, std::string* out) {
    const std::string* p = std::get_if<std::string>(&v);
    if (p == nullptr) {
        return false;
    }
    *out = *p;
    return true;
}

}  // namespace

bool encode_row(const std::vector<ColumnSpec>& cols,
                                const std::vector<Value>& values,
                                std::vector<uint8_t>& out) {
    if (cols.size() != values.size()) {
        return false;
    }
    out.clear();
    out.reserve(16 + values.size() * 8);
    out.push_back(0);  // 长度占位(低 8 位)
    out.push_back(0);  // 长度占位(高 8 位)

    for (size_t i = 0; i < cols.size(); ++i) {
        const ColumnSpec& col = cols[i];
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
                put_u16(out, static_cast<uint16_t>(v.size()));
                out.insert(out.end(), v.begin(), v.end());
                break;
            }
        }
    }

    const uint16_t body_len = static_cast<uint16_t>(out.size() - 2);
    out[0] = static_cast<uint8_t>(body_len & 0xff);
    out[1] = static_cast<uint8_t>((body_len >> 8) & 0xff);
    return true;
}

bool decode_row(const std::vector<ColumnSpec>& cols,
                                const uint8_t* data,
                                size_t len,
                                std::vector<Value>& out) {
    if (len < 2) {
        return false;
    }
    const uint16_t body_len = read_u16(data);
    if (static_cast<size_t>(body_len) + 2 != len) {
        return false;
    }
    out.clear();
    out.reserve(cols.size());
    size_t pos = 2;
    for (const ColumnSpec& col : cols) {
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
        }
    }
    return true;
}

bool parse_column_type(std::string_view type_str, ColType* type, uint16_t* varchar_len) {
    if (type_str == "int") {
        *type = ColType::Int;
        return true;
    }
    if (type_str == "bigint") {
        *type = ColType::BigInt;
        return true;
    }
    if (type_str == "double") {
        *type = ColType::Double;
        return true;
    }
    if (type_str == "varchar") {
        *type = ColType::VarChar;
        *varchar_len = 0;  // 动态大小
        return true;
    }
    return false;
}

}  // namespace st