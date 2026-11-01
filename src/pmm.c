/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 1 (2026-11-01) 整写。位图与 alloc/free 在 Task 2 补。
 *
 * Task 1 只做三件事:
 *   1. 把 multiboot2 信息结构所在的 2MB 区间映射上(GRUB 可能把它放在 2MB 以外);
 *   2. 解析 memory map, 拿到可用物理区间;
 *   3. 打印结果。
 *
 * 这里的指针都是物理地址直接当地址用 —— 因为引导期只建了恒等映射。
 * 本文件对 CR3 的访问是临时的;Task 3 的 vm_init()/vm_current_space() 落地后,
 * 页表遍历的逻辑会集中到 vm.c。
 */
#include "pmm.h"
#include "mb2.h"
#include "vga.h"

#define PMM_PTE_PRESENT  0x001ULL
#define PMM_PTE_HUGE     0x080ULL
#define PMM_PTE_ADDR     0x000FFFFFFFFFF000ULL
#define PMM_2MB          (2ULL * 1024 * 1024)
#define PMM_MAX_PHYS     0x40000000ULL   /* 1GB: 一个页目录用 2MB 大页的覆盖上限 */

static inline uint64_t pmm_read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void pmm_write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

/*
 * 保证 [addr, addr + len) 覆盖到的每个 2MB 区间都有映射。
 * 只服务引导期: 只处理 addr < 1GB, 只建 2MB 大页, 不拆已有的 4KB 映射。
 * 返回 0 成功, -1 失败。
 */
static int ensure_2mb_mapped(uint64_t addr, uint64_t len) {
    if (len == 0) {
        return 0;
    }
    /* 先挡住溢出和超过 1GB 的情况。 */
    if (addr >= PMM_MAX_PHYS || len > PMM_MAX_PHYS - addr) {
        return -1;
    }

    uint64_t *pml4 = (uint64_t *)(uintptr_t)(pmm_read_cr3() & PMM_PTE_ADDR);
    if ((pml4[0] & PMM_PTE_PRESENT) == 0) {
        return -1;
    }

    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & PMM_PTE_ADDR);
    if ((pdpt[0] & PMM_PTE_PRESENT) == 0) {
        return -1;
    }

    /* 若 PDPT[0] 已经是 1GB 大页, 整段都已映射, 不用碰 PD。 */
    if (pdpt[0] & PMM_PTE_HUGE) {
        return 0;
    }

    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[0] & PMM_PTE_ADDR);

    uint64_t first = addr & ~(PMM_2MB - 1);
    uint64_t last = (addr + len - 1) & ~(PMM_2MB - 1);

    for (uint64_t a = first; ; a += PMM_2MB) {
        uint64_t idx = a >> 21;   /* 2MB = 2^21 */
        if (idx >= 512) {
            return -1;
        }

        uint64_t entry = pd[idx];
        if (entry & PMM_PTE_PRESENT) {
            /* 引导期不该出现 4KB 映射;遇到了说明假设已被打破。 */
            if ((entry & PMM_PTE_HUGE) == 0) {
                return -1;
            }
        } else {
            pd[idx] = (idx << 21) | PMM_PTE_PRESENT | 0x002ULL | PMM_PTE_HUGE;
        }

        if (a == last) {
            break;
        }
    }

    /* 写回同一个 CR3 刷新 TLB;否则新映射可能不生效。 */
    pmm_write_cr3(pmm_read_cr3());
    return 0;
}

int pmm_init(uint64_t mb_info) {
    if (mb_info == 0) {
        vga_printf("M8: mb_info is NULL\n");
        return -1;
    }

    /* 先只映射读 total_size 需要的 8 字节。 */
    if (ensure_2mb_mapped(mb_info, 8) != 0) {
        vga_printf("M8: cannot map mb_info header at %p\n",
                   (void *)(uintptr_t)mb_info);
        return -1;
    }

    uint32_t total_size = *(const uint32_t *)(uintptr_t)mb_info;
    if (total_size == 0 || total_size > 0x100000u) {
        vga_printf("M8: bad multiboot2 total_size=0x%x\n", total_size);
        return -1;
    }

    /* 再映射整块信息结构, 之后才能安全遍历 tag。 */
    if (ensure_2mb_mapped(mb_info, (uint64_t)total_size) != 0) {
        vga_printf("M8: cannot map mb_info body (%u bytes)\n", total_size);
        return -1;
    }

    mb2_range_t ranges[MB2_MAX_RANGES];
    uint64_t info_end = 0;
    int count = mb2_init(mb_info, ranges, MB2_MAX_RANGES, &info_end);
    if (count < 0) {
        vga_printf("M8: mb2_init failed\n");
        return -1;
    }

    vga_printf("M8: mb_info=%p total_size=0x%016llx\n",
               (void *)(uintptr_t)mb_info, (unsigned long long)total_size);
    vga_printf("M8: usable ranges=%d\n", count);

    uint64_t usable = 0;
    for (int i = 0; i < count; i++) {
        vga_printf("  [%d] base=%p len=0x%016llx\n", i,
                   (void *)(uintptr_t)ranges[i].base,
                   (unsigned long long)ranges[i].length);
        usable += ranges[i].length;
    }

    /* 右移 20 位 = 除以 1MB。 */
    vga_printf("M8: usable=0x%016llx (~%llu MB)\n",
               (unsigned long long)usable, (unsigned long long)(usable >> 20));

    (void)info_end;   /* Task 2 会用它保留信息结构所在的帧 */
    return 0;
}
