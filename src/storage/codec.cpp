// codec.cpp: 行序列化/反序列化
#include "codec.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "common/err.h"
#include "log/log.h"

namespace st {

namespace {

namespace mp = boost::multiprecision;

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
    const int64_t* p = std::get_if<int64_t>(&v.box);
    if (p == nullptr) {
        return false;
    }
    *out = *p;
    return true;
}

inline bool get_double(const Value& v, double* out)
{
    const double* p = std::get_if<double>(&v.box);
    if (p == nullptr) {
        return false;
    }
    *out = *p;
    return true;
}

inline bool get_string(const Value& v, std::string* out)
{
    const std::string* p = std::get_if<std::string>(&v.box);
    if (p == nullptr) {
        return false;
    }
    *out = *p;
    return true;
}

inline bool get_bool(const Value& v, bool* out)
{
    const bool* p = std::get_if<bool>(&v.box);
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
        if (std::holds_alternative<std::monostate>(values[i].box)) {
            if (col.not_null) {
                return false;  // NOT NULL 列拒绝 NULL
            }
            out[2 + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            continue;
        }
        switch (col.type.rep) {
            case Rep::Int64: {
                int64_t v = 0;
                if (!get_int64(values[i], &v)) {
                    return false;
                }
                if (col.type.store != Store::Fixed) {
                    DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                             "encode: 存储形态与值形态不符");
                }
                if (col.type.width == 4) {
                    if (v < INT32_MIN || v > INT32_MAX) {
                        return false;
                    }
                    put_u32(out, static_cast<uint32_t>(static_cast<int32_t>(v)));
                } else if (col.type.width == 8) {
                    put_u32(out, static_cast<uint32_t>(v));
                    put_u32(out, static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32));
                } else {
                    DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                             "encode: 定宽字节数不在词表内: {}", col.type.width);
                }
                break;
            }
            case Rep::Double: {
                double v = 0.0;
                if (!get_double(values[i], &v) || !std::isfinite(v)) {
                    return false;  // 形态不匹配或非有限值
                }
                if (col.type.store != Store::Fixed || col.type.width != 8) {
                    DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                             "encode: 属性组合不在词表内");
                }
                uint64_t bits = 0;
                std::memcpy(&bits, &v, sizeof(bits));
                put_u32(out, static_cast<uint32_t>(bits));
                put_u32(out, static_cast<uint32_t>(bits >> 32));
                break;
            }
            case Rep::Str: {
                std::string v;
                if (!get_string(values[i], &v)) {
                    return false;
                }
                if (col.type.store == Store::VarLen) {
                    if (v.size() > UINT16_MAX) {
                        return false;
                    }
                    if (col.length > 0 && v.size() > col.length) {
                        return false;  // 超出声明长度拒绝
                    }
                    put_u16(out, static_cast<uint16_t>(v.size()));
                    out.insert(out.end(), v.begin(), v.end());
                } else if (col.type.store == Store::CharPad) {
                    const uint16_t n = col.length;
                    if (v.size() > n) {
                        return false;  // 超长拒绝
                    }
                    out.insert(out.end(), v.begin(), v.end());
                    out.insert(out.end(), static_cast<size_t>(n) - v.size(), ' ');  // 不足补空格
                } else {
                    DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                             "encode: 存储形态与值形态不符");
                }
                break;
            }
            case Rep::Float: {
                const float* f = std::get_if<float>(&values[i].box);
                if (f == nullptr || !std::isfinite(*f)) {
                    return false;  // 形态不匹配或非有限值
                }
                if (col.type.store != Store::Fixed || col.type.width != 4) {
                    DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                             "encode: 属性组合不在词表内");
                }
                uint32_t bits = 0;
                std::memcpy(&bits, f, sizeof(bits));
                put_u32(out, bits);
                break;
            }
            case Rep::Bool: {
                bool v = false;
                if (!get_bool(values[i], &v)) {
                    return false;
                }
                if (col.type.store != Store::Fixed || col.type.width != 1) {
                    DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                             "encode: 属性组合不在词表内");
                }
                out.push_back(static_cast<uint8_t>(v ? 1 : 0));
                break;
            }
            case Rep::Dec: {
                const DecimalVal* d = std::get_if<DecimalVal>(&values[i].box);
                if (d == nullptr || mp::abs(d->unscaled) >= kNumericUnscaledMax) {
                    return false;
                }
                if (col.type.store != Store::Decimal) {
                    DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                             "encode: 属性组合不在词表内");
                }
                // 16B 小端补码 unscaled + 1B scale
                const mp::uint256_t mag = static_cast<mp::uint256_t>(mp::abs(d->unscaled));
                uint8_t b[16] = {};
                for (int k = 0; k < 16; ++k) {
                    b[k] = static_cast<uint8_t>((mag >> (k * 8)).convert_to<unsigned>());
                }
                if (d->unscaled < 0) {
                    // 负数整体取反加一得补码
                    for (int k = 0; k < 16; ++k) {
                        b[k] = static_cast<uint8_t>(~b[k]);
                    }
                    for (int k = 0; k < 16; ++k) {
                        if (++b[k] != 0) {
                            break;
                        }
                    }
                }
                out.insert(out.end(), b, b + 16);
                out.push_back(d->scale);
                break;
            }
            case Rep::None:
                DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                         "encode: 列无类型");
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
        if ((static_cast<unsigned>(bitmap[i / 8]) >> (i % 8)) & 1u) {
            out.push_back(typed_null(col.type));  // NULL: 携带列类型
            continue;
        }
        switch (col.type.rep) {
            case Rep::Int64: {
                if (col.type.width == 4) {
                    if (pos + 4 > len) {
                        return false;
                    }
                    const int32_t v = static_cast<int32_t>(read_u32(data + pos));
                    out.push_back(int_val(col.type, static_cast<int64_t>(v)));
                    pos += 4;
                } else if (col.type.width == 8) {
                    if (pos + 8 > len) {
                        return false;
                    }
                    const uint64_t lo = read_u32(data + pos);
                    const uint64_t hi = read_u32(data + pos + 4);
                    out.push_back(int_val(col.type, static_cast<int64_t>(lo | (hi << 32))));
                    pos += 8;
                } else {
                    DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                             "decode: 定宽字节数不在词表内: {}", col.type.width);
                }
                break;
            }
            case Rep::Double: {
                if (pos + 8 > len) {
                    return false;
                }
                const uint64_t bits = read_u32(data + pos) |
                                        (static_cast<uint64_t>(read_u32(data + pos + 4)) << 32);
                double v = 0.0;
                std::memcpy(&v, &bits, sizeof(v));
                out.push_back(double_val(col.type, v));
                pos += 8;
                break;
            }
            case Rep::Str: {
                if (col.type.store == Store::VarLen) {
                    if (pos + 2 > len) {
                        return false;
                    }
                    const uint16_t slen = read_u16(data + pos);
                    pos += 2;
                    if (pos + slen > len) {
                        return false;
                    }
                    out.push_back(str_val(col.type,
                                          std::string(reinterpret_cast<const char*>(data + pos),
                                                      slen)));
                    pos += slen;
                } else if (col.type.store == Store::CharPad) {
                    if (pos + col.length > len) {
                        return false;
                    }
                    std::string v(reinterpret_cast<const char*>(data + pos), col.length);
                    pos += col.length;
                    // 定长读出, 保留尾部填充空格
                    out.push_back(str_val(col.type, std::move(v)));
                } else {
                    DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                             "decode: 存储形态与值形态不符");
                }
                break;
            }
            case Rep::Float: {
                if (pos + 4 > len) {
                    return false;
                }
                uint32_t bits = read_u32(data + pos);
                float f = 0.0f;
                std::memcpy(&f, &bits, sizeof(f));
                out.push_back(float_val(col.type, f));
                pos += 4;
                break;
            }
            case Rep::Bool: {
                if (pos + 1 > len) {
                    return false;
                }
                const uint8_t b = data[pos];
                if (b > 1) {
                    return false;  // 非 0/1 视为损坏
                }
                out.push_back(bool_val(col.type, b != 0));
                pos += 1;
                break;
            }
            case Rep::Dec: {
                if (pos + 17 > len) {
                    return false;
                }
                mp::uint256_t mag = 0;
                for (size_t k = 16; k-- > 0;) {
                    mag = (mag << 8) | static_cast<unsigned>(data[pos + k]);
                }
                // 128 位补码: 符号位为第 127 位, 取负(模 2^128)得幅值
                mp::int256_t u = 0;
                if ((mag >> 127).convert_to<unsigned>() != 0) {
                    mag = (mp::uint256_t(1) << 128) - mag;
                    u = -static_cast<mp::int256_t>(mag);
                } else {
                    u = static_cast<mp::int256_t>(mag);
                }
                if (mp::abs(u) >= kNumericUnscaledMax) {
                    return false;  // 幅值超 38 位十进制视为损坏
                }
                out.push_back(numeric_val(col.type, u, data[pos + 16]));
                pos += 17;
                break;
            }
            case Rep::None:
                DB_RAISE(db::ErrCode::Internal, LogModule::STORAGE,
                         "decode: 列无类型");
        }
    }
    return true;
}

}  // namespace st