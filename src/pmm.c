/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 1 (2026-11-01) 整写(解析 + 恒等映射补洞);
 *            M8 Task 2 (2026-11-02) 位图帧分配器 + 恒等映射扩展 + 自检。
 *
 * 本文件是物理层, 只回答一个问题: 哪些 4KB 物理帧空着?
 *   - 它不懂虚拟地址(那是 vm.c);
 *   - 它不懂字节级分配(那是 heap.c);
 *   - 它只认 memory map 给出的可用区间, 用位图记账。
 *
 * Task 1 的 ensure_2mb_mapped() 是引导期的一次性补洞;
 * Task 3 的 vm_init()/vm_current_space() 落地后, 页表遍历会集中到 vm.c。
 * 本文件对 CR3 的访问是临时的。
 */
#include "pmm.h"
#include "mb2.h"
#include "vga.h"

#define PMM_PTE_PRESENT  0x001ULL
#define PMM_PTE_RW       0x002ULL
#define PMM_PTE_HUGE     0x080ULL
#define PMM_PTE_ADDR     0x000FFFFFFFFFF000ULL
#define PMM_PTE_KERNEL   (PMM_PTE_PRESENT | PMM_PTE_RW | PMM_PTE_HUGE)  /* 0x83 */
#define PMM_2MB          (2ULL * 1024 * 1024)
#define PMM_LOW_RESERVED 0x100000ULL   /* 低 1MB: BIOS/VGA/EBDA */

#define PMM_SELF_TEST_FRAMES 512

/* 1GB / 4KB / 8 = 32KB, 住在 .bss。 */
static uint8_t pmm_bitmap[PMM_BITMAP_BYTES];

static uint64_t pmm_free_frames;
static uint64_t pmm_hint;        /* 下次从这里开始找, 避免每次从头扫 */
static uint64_t pmm_mb_info;     /* multiboot2 信息结构起止, 供保留与自检 */
static uint64_t pmm_info_end;
static uint64_t pmm_max_end;     /* memory map 中可用区间的最大末端 */

extern char __bss_end[];         /* linker.ld 导出, .bss 是最后一个输出段 */

static inline uint64_t pmm_read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void pmm_write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline uint64_t pmm_align_up_4k(uint64_t x) {
    return (x + PMM_FRAME_SIZE - 1) & ~(PMM_FRAME_SIZE - 1);
}

/* ---- 位图操作: 1 = 不可用, 0 = 空闲 ---- */

static int pmm_bit_test(uint64_t frame) {
    return (pmm_bitmap[frame >> 3] >> (frame & 7)) & 1;
}

static void pmm_bit_set(uint64_t frame) {
    pmm_bitmap[frame >> 3] |= (uint8_t)(1u << (frame & 7));
}

static void pmm_bit_clear(uint64_t frame) {
    pmm_bitmap[frame >> 3] &= (uint8_t)~(1u << (frame & 7));
}

/* 把 [base, base+len) 覆盖到的完整帧标为空闲。 */
static void pmm_mark_free(uint64_t base, uint64_t len) {
    if (len == 0 || base >= PMM_MAX_PHYS) {
        return;
    }

    uint64_t end = base + len;
    if (end < base || end > PMM_MAX_PHYS) {
        end = PMM_MAX_PHYS;
    }

    /* 只放行完整落在区间内的帧。 */
    uint64_t first = pmm_align_up_4k(base);
    uint64_t last = end & ~(PMM_FRAME_SIZE - 1);

    for (uint64_t pa = first; pa < last; pa += PMM_FRAME_SIZE) {
        pmm_bit_clear(pa / PMM_FRAME_SIZE);
    }
}

/* 把 [base, base+len) 覆盖到的所有帧标为不可用(按帧向外取整)。 */
static void pmm_mark_used(uint64_t base, uint64_t len) {
    if (len == 0 || base >= PMM_MAX_PHYS) {
        return;
    }

    uint64_t end = base + len;
    if (end < base || end > PMM_MAX_PHYS) {
        end = PMM_MAX_PHYS;
    }

    uint64_t first = base & ~(PMM_FRAME_SIZE - 1);
    uint64_t last = pmm_align_up_4k(end);
    if (last > PMM_MAX_PHYS) {
        last = PMM_MAX_PHYS;
    }

    for (uint64_t pa = first; pa < last; pa += PMM_FRAME_SIZE) {
        pmm_bit_set(pa / PMM_FRAME_SIZE);
    }
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
            pd[idx] = (idx << 21) | PMM_PTE_KERNEL;
        }

        if (a == last) {
            break;
        }
    }

    /* 写回同一个 CR3 刷新 TLB;否则新映射可能不生效。 */
    pmm_write_cr3(pmm_read_cr3());
    return 0;
}

/*
 * 把恒等映射从引导期的前 2MB 扩到覆盖 [0, max_end)。
 * 分配器会返回高地址的帧, 没有映射, 一写就 #PF。
 */
static int pmm_extend_identity(uint64_t max_end) {
    if (max_end == 0) {
        return 0;
    }
    if (max_end > PMM_MAX_PHYS) {
        max_end = PMM_MAX_PHYS;
    }

    /* 向上取整到 2MB, 最多一个 PD 的 512 项。 */
    uint64_t n = (max_end + PMM_2MB - 1) / PMM_2MB;
    if (n > 512) {
        n = 512;
    }

    uint64_t *pml4 = (uint64_t *)(uintptr_t)(pmm_read_cr3() & PMM_PTE_ADDR);
    if ((pml4[0] & PMM_PTE_PRESENT) == 0) {
        return -1;
    }

    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & PMM_PTE_ADDR);
    if ((pdpt[0] & PMM_PTE_PRESENT) == 0) {
        return -1;
    }

    /* 已经是 1GB 大页, 整个 1GB 都在映射里。 */
    if (pdpt[0] & PMM_PTE_HUGE) {
        pmm_write_cr3(pmm_read_cr3());
        return 0;
    }

    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[0] & PMM_PTE_ADDR);

    /* pd[0] 是引导期建的, 从 1 开始; 已存在的项不覆盖。 */
    for (uint64_t i = 1; i < n; i++) {
        if ((pd[i] & PMM_PTE_PRESENT) == 0) {
            pd[i] = (i << 21) | PMM_PTE_KERNEL;
        }
    }

    pmm_write_cr3(pmm_read_cr3());
    return 0;
}

/*
 * 位图初始化, 顺序不可颠倒:
 *   1. 全部置 1(默认不可用) —— memory map 不会枚举所有 MMIO 洞;
 *   2. 只把 type == 1 的可用区间清 0;
 *   3. 再把内核镜像/位图/栈、低 1MB、multiboot2 信息结构重新标 1。
 * 反过来做(先全 0 再标坏区间)会信任"坏区间已被完整枚举", 不成立。
 */
static void pmm_bitmap_init(const mb2_range_t *ranges, int count) {
    for (uint64_t i = 0; i < PMM_BITMAP_BYTES; i++) {
        pmm_bitmap[i] = 0xFF;
    }

    for (int i = 0; i < count; i++) {
        pmm_mark_free(ranges[i].base, ranges[i].length);
    }

    uint64_t kernel_end = pmm_align_up_4k((uint64_t)(uintptr_t)__bss_end);

    pmm_mark_used(0, PMM_LOW_RESERVED);
    if (kernel_end > PMM_LOW_RESERVED) {
        pmm_mark_used(PMM_LOW_RESERVED, kernel_end - PMM_LOW_RESERVED);
    }
    if (pmm_info_end > pmm_mb_info) {
        pmm_mark_used(pmm_mb_info, pmm_info_end - pmm_mb_info);
    }

    pmm_free_frames = 0;
    for (uint64_t frame = 0; frame < PMM_FRAME_COUNT; frame++) {
        if (!pmm_bit_test(frame)) {
            pmm_free_frames++;
        }
    }

    pmm_hint = 0;
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
    uint64_t max_end = 0;
    for (int i = 0; i < count; i++) {
        vga_printf("  [%d] base=%p len=0x%016llx\n", i,
                   (void *)(uintptr_t)ranges[i].base,
                   (unsigned long long)ranges[i].length);
        usable += ranges[i].length;

        uint64_t end = ranges[i].base + ranges[i].length;
        if (end < ranges[i].base || end > PMM_MAX_PHYS) {
            end = PMM_MAX_PHYS;
        }
        if (end > max_end) {
            max_end = end;
        }
    }

    /* 右移 20 位 = 除以 1MB。 */
    vga_printf("M8: usable=0x%016llx (~%llu MB)\n",
               (unsigned long long)usable, (unsigned long long)(usable >> 20));

    pmm_mb_info = mb_info;
    pmm_info_end = info_end;
    pmm_max_end = max_end;

    pmm_bitmap_init(ranges, count);

    if (pmm_extend_identity(max_end) != 0) {
        vga_printf("M8/PMM: cannot extend identity map to %p\n",
                   (void *)(uintptr_t)max_end);
        return -1;
    }

    return 0;
}

uint64_t pmm_alloc_frame(void) {
    if (pmm_free_frames == 0) {
        return 0;
    }

    /* 从 hint 扫到末尾, 再从头扫一遍(环形)。 */
    uint64_t start = pmm_hint;
    if (start >= PMM_FRAME_COUNT) {
        start = 0;
    }

    for (int pass = 0; pass < 2; pass++) {
        for (uint64_t frame = start; frame < PMM_FRAME_COUNT; frame++) {
            if (!pmm_bit_test(frame)) {
                pmm_bit_set(frame);
                pmm_free_frames--;
                pmm_hint = frame + 1;
                return frame * PMM_FRAME_SIZE;
            }
        }
        start = 0;
    }

    return 0;
}

void pmm_free_frame(uint64_t pa) {
    /* 中断上下文里不能 panic, 非法输入一律忽略。 */
    if (pa == 0 || (pa & (PMM_FRAME_SIZE - 1)) != 0 || pa >= PMM_MAX_PHYS) {
        return;
    }

    uint64_t frame = pa / PMM_FRAME_SIZE;
    if (!pmm_bit_test(frame)) {
        return;   /* 本来就空着 */
    }

    pmm_bit_clear(frame);
    pmm_free_frames++;

    if (frame < pmm_hint) {
        pmm_hint = frame;
    }
}

uint64_t pmm_free_count(void) {
    return pmm_free_frames;
}

uint64_t pmm_total_count(void) {
    return PMM_FRAME_COUNT;
}

int pmm_self_test(void) {
    uint64_t before = pmm_free_frames;
    uint64_t kernel_end = pmm_align_up_4k((uint64_t)(uintptr_t)__bss_end);
    /* 4KB 数组放 .bss 而不是内核栈(栈一共只有 16KB)。 */
    static uint64_t frames[PMM_SELF_TEST_FRAMES];
    uint64_t max_pa = 0;
    int allocated = 0;
    int ok = 1;

    vga_printf("M8/PMM: total=%llu free=%llu\n",
               (unsigned long long)PMM_FRAME_COUNT,
               (unsigned long long)before);

    for (int i = 0; i < PMM_SELF_TEST_FRAMES; i++) {
        uint64_t pa = pmm_alloc_frame();
        if (pa == 0) {
            vga_printf("M8/PMM: FAIL alloc frame %d\n", i);
            ok = 0;
            break;
        }

        frames[i] = pa;
        allocated++;

        if ((pa & (PMM_FRAME_SIZE - 1)) != 0) {
            vga_printf("M8/PMM: FAIL frame not aligned: %p\n",
                       (void *)(uintptr_t)pa);
            ok = 0;
            break;
        }
        if (pa < kernel_end) {
            vga_printf("M8/PMM: FAIL frame inside kernel: %p < %p\n",
                       (void *)(uintptr_t)pa, (void *)(uintptr_t)kernel_end);
            ok = 0;
            break;
        }
        if (pa < pmm_info_end && pa + PMM_FRAME_SIZE > pmm_mb_info) {
            vga_printf("M8/PMM: FAIL frame overlaps mb_info: %p\n",
                       (void *)(uintptr_t)pa);
            ok = 0;
            break;
        }

        if (pa > max_pa) {
            max_pa = pa;
        }
    }

    /* 恒等映射必须真的扩到了 2MB 以上, 否则写图案会 #PF。 */
    if (ok && max_pa <= PMM_2MB) {
        vga_printf("M8/PMM: FAIL no frame above 2MB (max_pa=%p)\n",
                   (void *)(uintptr_t)max_pa);
        ok = 0;
    }

    if (ok) {
        vga_printf("M8/PMM: 512 frames, max_pa=%p\n", (void *)(uintptr_t)max_pa);

        for (int i = 0; i < PMM_SELF_TEST_FRAMES; i++) {
            volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)frames[i];
            *p = 0xA5A5A5A5A5A5A5A5ULL ^ (uint64_t)i;
        }
        for (int i = 0; i < PMM_SELF_TEST_FRAMES; i++) {
            volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)frames[i];
            if (*p != (0xA5A5A5A5A5A5A5A5ULL ^ (uint64_t)i)) {
                vga_printf("M8/PMM: FAIL pattern mismatch at %p\n",
                           (void *)(uintptr_t)frames[i]);
                ok = 0;
                break;
            }
        }
        if (ok) {
            vga_printf("M8/PMM: pattern PASS\n");
        }
    }

    for (int i = 0; i < allocated; i++) {
        pmm_free_frame(frames[i]);
    }

    if (ok && pmm_free_frames != before) {
        vga_printf("M8/PMM: FAIL free-after-free=%llu, expected %llu\n",
                   (unsigned long long)pmm_free_frames,
                   (unsigned long long)before);
        ok = 0;
    } else if (ok) {
        vga_printf("M8/PMM: free-after-free=%llu\n",
                   (unsigned long long)pmm_free_frames);
    }

    /* 再分一遍, 证明释放的帧真的回到了池子里。 */
    if (ok) {
        int reuse = 0;
        for (int i = 0; i < PMM_SELF_TEST_FRAMES; i++) {
            uint64_t pa = pmm_alloc_frame();
            if (pa == 0) {
                vga_printf("M8/PMM: FAIL reuse alloc frame %d\n", i);
                ok = 0;
                break;
            }
            frames[reuse++] = pa;
        }
        for (int i = 0; i < reuse; i++) {
            pmm_free_frame(frames[i]);
        }
        if (ok) {
            vga_printf("M8/PMM: reuse PASS\n");
        }
    }

    return ok ? 0 : -1;
}
