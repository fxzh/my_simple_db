// page.cpp: 页/槽操作与页校验
#include "page.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace st {

namespace {

// CRC32 查表(多项式 0xedb88320), 静态初始化只执行一次
const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

}  // namespace

uint32_t page_checksum(const char* page) {
    const auto& table = crc_table();
    uint32_t crc = 0xffffffffu;
    const size_t cksum_off = offsetof(PageHeader, checksum);
    const size_t cksum_end = cksum_off + sizeof(PageHeader::checksum);
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        uint8_t b = static_cast<uint8_t>(page[i]);
        if (cksum_off <= i && i < cksum_end) {
            b = 0;
        }
        crc = table[(crc ^ b) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}

void init_page(char* page, uint32_t magic, PageType type) {
    std::memset(page, 0, PAGE_SIZE);
    PageHeader* h = header(page);
    h->magic = magic;
    h->type = static_cast<uint8_t>(type);
    h->free_begin = PAGE_HEADER_SIZE;
    h->free_end = PAGE_SIZE;
    h->checksum = page_checksum(page);
}

bool page_valid(const char* page, uint32_t magic) {
    if (header(page)->magic != magic) {
        return false;
    }
    return header(page)->checksum == page_checksum(page);
}

PageHeader* header(char* page) {
    return reinterpret_cast<PageHeader*>(page);
}

const PageHeader* header(const char* page) {
    return reinterpret_cast<const PageHeader*>(page);
}

Slot* slot_at(char* page, uint16_t i) {
    const size_t off = PAGE_SIZE - static_cast<size_t>(SLOT_SIZE) * (i + 1);
    return reinterpret_cast<Slot*>(page + off);
}

const Slot* slot_at(const char* page, uint16_t i) {
    const size_t off = PAGE_SIZE - static_cast<size_t>(SLOT_SIZE) * (i + 1);
    return reinterpret_cast<const Slot*>(page + off);
}

const uint8_t* record(const char* page, uint16_t i) {
    const Slot* s = slot_at(page, i);
    return reinterpret_cast<const uint8_t*>(page) + s->off;
}

uint16_t free_space(const char* page) {
    const PageHeader* h = header(page);
    return static_cast<uint16_t>(h->free_end - h->free_begin);
}

bool heap_append(char* page, const uint8_t* rec, uint16_t rec_len, uint16_t* slot_out) {
    PageHeader* h = header(page);
    const uint32_t avail = static_cast<uint32_t>(h->free_end) - h->free_begin;
    if (static_cast<uint32_t>(rec_len) + SLOT_SIZE > avail) {
        return false;
    }
    const uint16_t off = h->free_begin;
    std::memcpy(page + off, rec, rec_len);
    slot_at(page, h->slot_count)->off = off;
    slot_at(page, h->slot_count)->len = rec_len;
    h->free_begin = static_cast<uint16_t>(off + rec_len);
    h->free_end = static_cast<uint16_t>(h->free_end - SLOT_SIZE);
    h->slot_count = static_cast<uint16_t>(h->slot_count + 1);
    h->checksum = page_checksum(page);
    if (slot_out != nullptr) {
        *slot_out = static_cast<uint16_t>(h->slot_count - 1);
    }
    return true;
}

bool slot_tombstone(const char* page, uint16_t slot) {
    const Slot* s = slot_at(page, slot);
    return s->off == 0 && s->len == 0;
}

void heap_delete(char* page, uint16_t slot) {
    PageHeader* h = header(page);
    Slot* s = slot_at(page, slot);
    s->off = 0;
    s->len = 0;
    while (h->slot_count > 0 && slot_tombstone(page, h->slot_count - 1)) {
        --h->slot_count;
        h->free_end = static_cast<uint16_t>(h->free_end + SLOT_SIZE);
    }
    h->checksum = page_checksum(page);
}

void heap_compact(char* page) {
    PageHeader* h = header(page);
    uint16_t off = PAGE_HEADER_SIZE;
    uint16_t dst = 0;
    for (uint16_t i = 0; i < h->slot_count; ++i) {
        const Slot* s = slot_at(page, i);
        if (s->off == 0 && s->len == 0) {
            continue;  // 墓碑跳过
        }
        const uint16_t n = s->len;
        std::memmove(page + off, page + s->off, n);
        Slot* d = slot_at(page, dst);
        d->off = off;
        d->len = n;
        off = static_cast<uint16_t>(off + n);
        ++dst;
    }
    h->slot_count = dst;
    h->free_begin = off;
    h->free_end = static_cast<uint16_t>(PAGE_SIZE - static_cast<uint16_t>(dst) * SLOT_SIZE);
    h->checksum = page_checksum(page);
}

}  // namespace st