// page.h: 页头与槽的磁盘布局
#ifndef STORAGE_PAGE_H
#define STORAGE_PAGE_H

#include <cstdint>
#include "types.h"

namespace st {

constexpr uint32_t PAGE_HEADER_SIZE = 24;

// 页头(定长 24 字节)
struct PageHeader {
    uint32_t magic;      // 页魔数, 校验错位/垃圾页
    uint8_t  type;       // PageType
    uint8_t  pad0;       // 对齐
    uint8_t  pad1;
    uint8_t  pad2;
    uint16_t slot_count; // 记录数
    uint16_t free_begin; // 数据区起点
    uint16_t free_end;   // 槽数组起点
    uint32_t next_page;  // 下一页页号, 0 表示无
    uint32_t checksum;   // 全页校验
};
static_assert(sizeof(PageHeader) == PAGE_HEADER_SIZE, "PageHeader must be 24 bytes");

// 页内槽项(4 字节): 记录相对页首偏移 + 记录长度
struct Slot {
    uint16_t off;
    uint16_t len;
};
static_assert(sizeof(Slot) == 4, "Slot must be 4 bytes");

constexpr uint32_t SLOT_SIZE = 4;
constexpr uint16_t MAX_RECORD_LEN = 4096 - PAGE_HEADER_SIZE - SLOT_SIZE;

// 页魔数
constexpr uint32_t MAGIC_FILE_HEADER = 0x54444146;  // "FADT"
constexpr uint32_t MAGIC_HEAP = 0x50414548;         // "HEAP"

// 初始化空闲页
void init_page(char* page, uint32_t magic, PageType type);

// 计算页校验和(checksum 字段按 0 参与计算)
uint32_t page_checksum(const char* page);

// 校验页: 魔数与校验和同时正确
bool page_valid(const char* page, uint32_t magic);

// 页头指针
PageHeader* header(char* page);
const PageHeader* header(const char* page);

// 槽数组区从页尾向 free_begin 生长, 槽 i 位于页尾 - (i+1)*SLOT_SIZE
Slot* slot_at(char* page, uint16_t i);
const Slot* slot_at(const char* page, uint16_t i);

// 槽 i 指向的记录字节
const uint8_t* record(const char* page, uint16_t i);

// 堆页剩余可用空间(字节)
uint16_t free_space(const char* page);

// 堆追加: 记录贴到数据区末尾并追加槽, 空间不足返回 false
bool heap_append(char* page, const uint8_t* rec, uint16_t rec_len, uint16_t* slot_out);

}  // namespace st
#endif