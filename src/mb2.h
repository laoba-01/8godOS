/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 1 (2026-11-01) 整写。
 */
#ifndef MB2_H
#define MB2_H

#include <stdint.h>

/* 调用者数组的上限。GRUB 在常见配置下只会给出 2~4 个可用区间。 */
#define MB2_MAX_RANGES 32

/* 一个可用物理区间。[base, base + length) */
typedef struct {
    uint64_t base;
    uint64_t length;
} mb2_range_t;

/*
 * 解析 multiboot2 信息结构, 把 type == 1(可用 RAM)的区间复制进 ranges。
 *   info_addr  multiboot2 信息结构的物理地址(_start 从 EBX 存下的)
 *   ranges     调用者提供的数组
 *   max_ranges 数组容量
 *   info_end   回填 info_addr + total_size, 供 pmm 保留整个信息结构
 * 返回可用区间个数(>= 0);失败返回 -1。
 *
 * 前置条件: [info_addr, info_addr + total_size) 必须已经建立映射 ——
 * 解析本身要读这块内存, 否则会 #PF。
 */
int mb2_init(uint64_t info_addr, mb2_range_t *ranges, int max_ranges,
             uint64_t *info_end);

#endif /* MB2_H */
