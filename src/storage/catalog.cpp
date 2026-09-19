// catalog.cpp: 表元数据序列化与目录操作
#include "catalog.h"

#include <algorithm>
#include <fstream>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "common/err.h"
#include "log/log.h"

namespace st {

namespace {

void put_u16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

void put_bytes(std::vector<uint8_t>& out, const std::string& s)
{
    put_u16(out, static_cast<uint16_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

// 字节读取器, 越界返回 false
struct Reader {
    const uint8_t* p;
    size_t size;
    size_t pos = 0;

    bool u16(uint16_t* out)
    {
        if (pos + 2 > size) {
            return false;
        }
        *out = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8));
        pos += 2;
        return true;
    }

    bool u32(uint32_t* out)
    {
        if (pos + 4 > size) {
            return false;
        }
        *out = static_cast<uint32_t>(p[pos]) | (static_cast<uint32_t>(p[pos + 1]) << 8) |
                      (static_cast<uint32_t>(p[pos + 2]) << 16) | (static_cast<uint32_t>(p[pos + 3]) << 24);
        pos += 4;
        return true;
    }

    bool str(std::string* out)
    {
        uint16_t len;
        if (!u16(&len) || pos + len > size) {
            return false;
        }
        out->assign(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        return true;
    }
};

}  // namespace

void Catalog::load(const std::string& path)
{
    tables_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        DB_RAISE(db::ErrCode::CatalogMissing, LogModule::STORAGE, "目录文件不存在: {}", path);
    }
    in.seekg(0, std::ios::end);
    const std::streamoff fsize = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(fsize));
    if (fsize > 0) {
        in.read(reinterpret_cast<char*>(buf.data()), fsize);
    }

    Reader r{buf.data(), buf.size(), 0};
    uint32_t magic = 0;
    uint32_t count = 0;
    if (!r.u32(&magic) || magic != CATALOG_MAGIC || !r.u32(&count)) {
        DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "目录文件损坏");
    }
    for (uint32_t i = 0; i < count; ++i) {
        TableMeta meta;
        if (!r.u32(&meta.table_id) || !r.str(&meta.name)) {
            DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "目录条目损坏");
        }
        uint16_t col_count = 0;
        if (!r.u16(&col_count)) {
            DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "目录条目损坏");
        }
        for (uint16_t j = 0; j < col_count; ++j) {
            ColumnSpec col;
            if (!r.str(&col.name) || !r.u16(&col.length)) {
                DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "目录列损坏");
            }
            uint8_t type = 0;
            uint8_t flags = 0;
            if (r.pos + 2 > r.size) {
                DB_RAISE(db::ErrCode::CorruptCatalog, LogModule::STORAGE, "目录列损坏");
            }
            type = r.p[r.pos];
            flags = r.p[r.pos + 1];
            r.pos += 2;
            col.type = static_cast<ColType>(type);
            col.not_null = (flags & 0x01) != 0;
            meta.cols.push_back(std::move(col));
        }
        tables_.push_back(std::move(meta));
    }
}

void Catalog::save(const std::string& path) const
{
    std::vector<uint8_t> out;
    out.reserve(64 + tables_.size() * 128);
    put_u32(out, CATALOG_MAGIC);
    put_u32(out, static_cast<uint32_t>(tables_.size()));
    for (const TableMeta& meta : tables_) {
        put_u32(out, meta.table_id);
        put_bytes(out, meta.name);
        put_u16(out, static_cast<uint16_t>(meta.cols.size()));
        for (const ColumnSpec& col : meta.cols) {
            put_bytes(out, col.name);
            put_u16(out, col.length);
            out.push_back(static_cast<uint8_t>(col.type));
            out.push_back(col.not_null ? 0x01 : 0x00);  // flags: bit0 = NOT NULL
        }
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        DB_RAISE(db::ErrCode::IoError, LogModule::STORAGE, "无法写目录文件: {}", path);
    }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    f.close();
}

const TableMeta* Catalog::find(const std::string& name) const
{
    for (const auto& t : tables_) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

const TableMeta* Catalog::find_by_id(uint32_t table_id) const
{
    for (const auto& t : tables_) {
        if (t.table_id == table_id) {
            return &t;
        }
    }
    return nullptr;
}

uint32_t Catalog::alloc_table_id() const
{
    uint32_t max_id = 0;
    for (const auto& t : tables_) {
        if (t.table_id > max_id) {
            max_id = t.table_id;
        }
    }
    return max_id + 1;
}

void Catalog::add_or_update(const TableMeta& meta)
{
    for (auto& t : tables_) {
        if (t.table_id == meta.table_id) {
            t = meta;
            return;
        }
    }
    tables_.push_back(meta);
}

void Catalog::erase(const std::string& name)
{
    tables_.erase(std::remove_if(tables_.begin(), tables_.end(),
                                [&](const TableMeta& t) { return t.name == name; }),
                                tables_.end());
}

}  // namespace st