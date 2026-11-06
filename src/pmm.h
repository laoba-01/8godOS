/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 1 (2026-11-01) 解析接口;
 *            M8 Task 2 (2026-11-02) 位图帧分配器接口与常量。
 */
#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE   4096ULL
#define PMM_MAX_PHYS     0x40000000ULL                  /* 1GB: 一个 PD 的覆盖上限 */
#define PMM_FRAME_COUNT  (PMM_MAX_PHYS / PMM_FRAME_SIZE)
#define PMM_BITMAP_BYTES (PMM_FRAME_COUNT / 8)

/*
 * 初始化: 解析 multiboot2 memory map → 位图 → 恒等映射扩展。
 *   mb_info  multiboot2 信息结构的物理地址(kmain 的第一个参数)
 * 返回 0 成功, -1 失败(调用者负责停机)。
 */
int pmm_init(uint64_t mb_info);

/*
 * 分配一个 4KB 物理帧, 返回物理地址; 0 表示 OOM(帧 0 永远保留)。
 * 空闲帧由位图记录, 分配结果一定落在 memory map 的可用区间内。
 */
uint64_t pmm_alloc_frame(void);

/* 归还一个 4KB 物理帧。非法地址(未对齐/越界/本来就空)直接忽略。 */
void pmm_free_frame(uint64_t pa);

uint64_t pmm_free_count(void);
uint64_t pmm_total_count(void);

/* 自检: 512 帧分配/写图案/释放/复用。0 通过, -1 失败。 */
int pmm_self_test(void);

#endif /* PMM_H */
