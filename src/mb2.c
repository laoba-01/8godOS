/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 1 (2026-11-01) 整写。
 *
 * 只懂 GRUB 的数据格式, 不懂物理内存怎么管:
 *   - 信息结构开头是 uint32_t total_size; uint32_t reserved;
 *   - 之后是 tag 链表, 每个 tag 以 uint32_t type; uint32_t size; 开头,
 *     按 8 字节对齐前进, type == 0 结束;
 *   - mmap tag(type == 6)在 tag 头之后还有 entry_size / entry_version,
 *     条目按 entry_size 字节排列, 前 24 字节是 base/length/type/reserved。
 *
 * 本文件全程恒等映射, 所以物理地址可以直接当指针用 ——
 * 这是 M8 的便利, 将来高半区重定位后失效。
 */
#include "mb2.h"

#define MB2_TAG_END              0
#define MB2_TAG_MMAP             6
#define MB2_MMAP_TYPE_AVAILABLE  1

/* tag 之间按 8 字节对齐。 */
static uint64_t mb2_align_up8(uint64_t x) {
    return (x + 7) & ~(uint64_t)7;
}

int mb2_init(uint64_t info_addr, mb2_range_t *ranges, int max_ranges,
             uint64_t *info_end) {
    if (ranges == 0 || info_end == 0 || max_ranges <= 0) {
        return -1;
    }

    const uint8_t *base = (const uint8_t *)(uintptr_t)info_addr;
    uint32_t total_size = *(const uint32_t *)base;

    /* total_size 是整块结构的长度, 不可能超过 1MB。 */
    if (total_size == 0 || total_size > 0x100000u) {
        return -1;
    }

    uint64_t end = info_addr + (uint64_t)total_size;
    *info_end = end;

    int count = 0;
    uint64_t p = info_addr + 8;   /* 跳过 total_size + reserved */

    while (p + 8 <= end) {
        uint32_t type = *(const uint32_t *)(uintptr_t)p;
        uint32_t size = *(const uint32_t *)(uintptr_t)(p + 4);

        if (type == MB2_TAG_END) {
            break;
        }
        /* 每个 tag 至少 8 字节, 且不能越过整块结构。 */
        if (size < 8 || p + (uint64_t)size > end) {
            return -1;
        }

        if (type == MB2_TAG_MMAP) {
            if (size < 16) {
                return -1;
            }

            uint32_t entry_size = *(const uint32_t *)(uintptr_t)(p + 8);
            if (entry_size < 24) {
                return -1;
            }

            uint64_t entries = ((uint64_t)size - 16) / entry_size;
            for (uint64_t i = 0; i < entries; i++) {
                uint64_t e = p + 16 + i * (uint64_t)entry_size;

                /* 最后一条可能被 padding 截短, 读之前再确认一次。 */
                if (e + 24 > p + (uint64_t)size) {
                    return -1;
                }

                uint64_t range_base = *(const uint64_t *)(uintptr_t)e;
                uint64_t range_len = *(const uint64_t *)(uintptr_t)(e + 8);
                uint32_t range_type = *(const uint32_t *)(uintptr_t)(e + 16);

                if (range_type != MB2_MMAP_TYPE_AVAILABLE) {
                    continue;
                }

                /* 数组满: 宁可报错, 也不静默丢掉可用内存。 */
                if (count >= max_ranges) {
                    return -1;
                }

                ranges[count].base = range_base;
                ranges[count].length = range_len;
                count++;
            }
        }

        p = mb2_align_up8(p + (uint64_t)size);
    }

    return count;
}
