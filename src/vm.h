/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 3 (2026-11-07) 页表项标志、地址掩码、虚拟区常量与 vm_* 原型;
 *            M8 Task 4 (2026-11-07) 地址空间创建/切换原型。
 *   (Task 5 会继续往这里加 #PF 接口。)
 */
#ifndef VM_H
#define VM_H

#include <stdint.h>

#define VM_PAGE_SIZE  4096ULL
#define VM_PTE_PRESENT (1ULL << 0)
#define VM_PTE_WRITE   (1ULL << 1)
#define VM_PTE_USER    (1ULL << 2)
#define VM_PTE_HUGE    (1ULL << 7)
#define VM_ADDR_MASK   0x000FFFFFFFFFF000ULL

/* 内核堆 / 按需分页区: PML4[256] 起的 16MB。 */
#define VM_HEAP_BASE   0xFFFF800000000000ULL
#define VM_HEAP_SIZE   0x01000000ULL

/* map/unmap 自检用的固定虚拟地址。 */
#define VM_SCRATCH_BASE 0xFFFF800001000000ULL

/* 每个地址空间独立的测试区: PML4[128], 创建空间时不复制。 */
#define VM_SPACE_TEST_BASE 0x0000400000000000ULL

/* 一张页表恰好 512 项, 每项 8 字节 = 4KB, 与页大小一致。 */
typedef struct {
    uint64_t entry[512];
} page_table_t;

/* 读 CR3 记录当前内核地址空间; 必须在 pmm_init 之前、idt_init 之后调用。 */
int vm_init(void);

page_table_t *vm_kernel_space(void);

/* 读当前 CR3 对应的 PML4。 */
page_table_t *vm_current_space(void);

/*
 * 建立 4KB 映射。返回 0 成功, -1 失败。
 * 拒绝: 未对齐 / pa 超过 PMM_MAX_PHYS / va 落在低 1GB 恒等映射区 /
 *       目标 PTE 已存在 / 中途遇到大页。
 */
int vm_map(page_table_t *space, uint64_t va, uint64_t pa, uint64_t flags);

/* 拆除映射, 返回旧物理地址; 未映射返回 0。不释放物理帧。 */
uint64_t vm_unmap(page_table_t *space, uint64_t va);

/*
 * 创建一个新地址空间(PML4)。
 *   - 复制 kernel_space->entry[0](恒等映射), 否则切 CR3 后取指就 #PF;
 *   - 复制 entry[256..511](内核高半区);
 *   - 不复制 entry[128] —— 每空间独立测试区。
 * 失败返回 NULL。本里程碑不做地址空间回收。
 */
page_table_t *vm_create_space(void);

/* 写 CR3 切换地址空间(会刷新全部非全局 TLB 项)。 */
void vm_switch(page_table_t *space);

/* 切回内核地址空间。 */
void vm_switch_kernel(void);

int vm_self_test(void);

#endif /* VM_H */
