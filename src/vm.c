/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 3 (2026-11-07) 四级页表遍历器 + vm_map/vm_unmap + TLB 失效 + 自检;
 *            M8 Task 4 (2026-11-07) 地址空间创建(vm_create_space) + CR3 切换 +
 *            每空间独立映射的自检;
 *            M8 Task 5 (2026-11-07) #PF 恢复 + 堆区按需分页 + 缺页计数。
 *
 * 本文件是虚拟层, 只回答一个问题: 虚拟地址怎么映射到物理帧?
 *   - 它不动位图(那是 pmm.c 的事), 只通过 pmm_alloc_frame 要页表页;
 *   - 它不管字节级分配(那是 heap.c 的事);
 *   - 它不碰 #PF(Task 5 才加)。
 *
 * 本里程碑全程恒等映射, 所以 pmm_alloc_frame 返回的物理地址可以直接当指针用。
 * 这是 M8 的便利, 将来高半区重定位后会失效。
 */
#include "vm.h"
#include "pmm.h"
#include "vga.h"

/* 内核地址空间(引导期用的 PML4), 所有地址空间都共享它的内核映射。 */
static page_table_t *vm_kernel_space_ptr;

/* #PF 统计与防重入标志: 处理器自己缺页时不能再进一次, 否则递归到 triple fault。 */
static uint64_t vm_faults;
static int vm_in_fault;

static inline uint64_t vm_read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline uint64_t vm_read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline void vm_write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline void vm_invlpg(uint64_t va) {
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

int vm_init(void) {
    vm_kernel_space_ptr = (page_table_t *)(uintptr_t)(vm_read_cr3() & VM_ADDR_MASK);
    vm_faults = 0;
    vm_in_fault = 0;
    return 0;
}

page_table_t *vm_kernel_space(void) {
    return vm_kernel_space_ptr;
}

page_table_t *vm_current_space(void) {
    return (page_table_t *)(uintptr_t)(vm_read_cr3() & VM_ADDR_MASK);
}

/*
 * 四级遍历: 返回最后一级 PT 项的地址。
 *   create=1 时缺表就分配一帧并清零; create=0 或缺帧/OOM/遇到大页返回 NULL。
 * 分配出来的页表页不回收(M8 不做页表回收)。
 */
static uint64_t *vm_walk(page_table_t *space, uint64_t va, int create) {
    if (space == 0) {
        return 0;
    }

    uint64_t *table = space->entry;
    uint64_t *pte;

    /* PML4 -> PDPT */
    pte = &table[(va >> 39) & 0x1FF];
    if ((*pte & VM_PTE_PRESENT) == 0) {
        if (!create) {
            return 0;
        }
        uint64_t pa = pmm_alloc_frame();
        if (pa == 0) {
            return 0;
        }
        uint64_t *next = (uint64_t *)(uintptr_t)pa;
        for (int i = 0; i < 512; i++) {
            next[i] = 0;
        }
        *pte = (pa & VM_ADDR_MASK) | VM_PTE_PRESENT | VM_PTE_WRITE;
    } else if (*pte & VM_PTE_HUGE) {
        return 0;   /* 大页无法继续往下走 */
    }
    table = (uint64_t *)(uintptr_t)(*pte & VM_ADDR_MASK);

    /* PDPT -> PD */
    pte = &table[(va >> 30) & 0x1FF];
    if ((*pte & VM_PTE_PRESENT) == 0) {
        if (!create) {
            return 0;
        }
        uint64_t pa = pmm_alloc_frame();
        if (pa == 0) {
            return 0;
        }
        uint64_t *next = (uint64_t *)(uintptr_t)pa;
        for (int i = 0; i < 512; i++) {
            next[i] = 0;
        }
        *pte = (pa & VM_ADDR_MASK) | VM_PTE_PRESENT | VM_PTE_WRITE;
    } else if (*pte & VM_PTE_HUGE) {
        return 0;
    }
    table = (uint64_t *)(uintptr_t)(*pte & VM_ADDR_MASK);

    /* PD -> PT */
    pte = &table[(va >> 21) & 0x1FF];
    if ((*pte & VM_PTE_PRESENT) == 0) {
        if (!create) {
            return 0;
        }
        uint64_t pa = pmm_alloc_frame();
        if (pa == 0) {
            return 0;
        }
        uint64_t *next = (uint64_t *)(uintptr_t)pa;
        for (int i = 0; i < 512; i++) {
            next[i] = 0;
        }
        *pte = (pa & VM_ADDR_MASK) | VM_PTE_PRESENT | VM_PTE_WRITE;
    } else if (*pte & VM_PTE_HUGE) {
        return 0;
    }
    table = (uint64_t *)(uintptr_t)(*pte & VM_ADDR_MASK);

    /* PT 项就是最终映射。 */
    return &table[(va >> 12) & 0x1FF];
}

int vm_map(page_table_t *space, uint64_t va, uint64_t pa, uint64_t flags) {
    if (space == 0) {
        return -1;
    }
    if ((va & (VM_PAGE_SIZE - 1)) != 0 || (pa & (VM_PAGE_SIZE - 1)) != 0) {
        return -1;
    }
    if (pa >= PMM_MAX_PHYS) {
        return -1;
    }
    /* 低 1GB 是恒等映射区, 只允许 2MB 大页, 不做拆分。 */
    if (va < PMM_MAX_PHYS) {
        return -1;
    }

    uint64_t *pte = vm_walk(space, va, 1);
    if (pte == 0) {
        return -1;
    }
    if (*pte & VM_PTE_PRESENT) {
        return -1;   /* 不静默覆盖已有映射 */
    }

    *pte = (pa & VM_ADDR_MASK) | flags;
    vm_invlpg(va);
    return 0;
}

uint64_t vm_unmap(page_table_t *space, uint64_t va) {
    if (space == 0) {
        return 0;
    }

    uint64_t *pte = vm_walk(space, va, 0);
    if (pte == 0 || (*pte & VM_PTE_PRESENT) == 0) {
        return 0;
    }

    uint64_t old_pa = *pte & VM_ADDR_MASK;
    *pte = 0;
    vm_invlpg(va);

    /* 不释放物理帧: 帧的归属由调用者决定。 */
    return old_pa;
}

page_table_t *vm_create_space(void) {
    if (vm_kernel_space_ptr == 0) {
        return 0;
    }

    uint64_t pa = pmm_alloc_frame();
    if (pa == 0) {
        return 0;
    }

    page_table_t *space = (page_table_t *)(uintptr_t)pa;

    /* 新帧里是上一个使用者的垃圾, 必须逐项清零。 */
    for (int i = 0; i < 512; i++) {
        space->entry[i] = 0;
    }

    /*
     * entry[0]: 低 1GB 恒等映射 —— 不复制的话, 切 CR3 后第一条取指就 #PF,
     *           而此时 IDT 里的门也找不到, 直接 triple fault。
     * entry[256..511]: 内核高半区(堆等)共享。
     * entry[128] 故意不复制: 每个空间独立的测试区。
     */
    space->entry[0] = vm_kernel_space_ptr->entry[0];
    for (int i = 256; i < 512; i++) {
        space->entry[i] = vm_kernel_space_ptr->entry[i];
    }

    return space;
}

void vm_switch(page_table_t *space) {
    if (space == 0) {
        return;
    }
    /* 恒等映射下指针值就是物理地址。写 CR3 会刷新全部非全局 TLB 项。 */
    vm_write_cr3((uint64_t)(uintptr_t)space);
}

void vm_switch_kernel(void) {
    vm_switch(vm_kernel_space_ptr);
}

int vm_handle_page_fault(regs_t *r) {
    uint64_t va = vm_read_cr2();
    uint64_t err = r->error_code;

    /* 防重入: 处理器自己缺页时不能再进来一次, 否则会递归到 triple fault。 */
    if (vm_in_fault) {
        return 0;
    }
    /* bit0 P: 页已存在, 是保护错误(COW/只读), M8 不处理。 */
    if (err & 1) {
        return 0;
    }
    /* bit3 RSVD: 页表项保留位被写坏, 页表已经不可信。 */
    if (err & 8) {
        return 0;
    }
    /* bit4 I/D: 取指缺页, M8 不处理。 */
    if (err & 16) {
        return 0;
    }
    /* 只管堆区的按需分页; 其他地址仍交给 panic(M6 回归)。 */
    if (va < VM_HEAP_BASE || va >= VM_HEAP_BASE + VM_HEAP_SIZE) {
        return 0;
    }

    vm_in_fault = 1;

    uint64_t frame = pmm_alloc_frame();
    if (frame == 0) {
        vm_in_fault = 0;
        return 0;   /* OOM: 交给 panic */
    }

    /* 新帧必须先清零, 否则用户会读到上一个使用者的数据。 */
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)frame;
    for (int i = 0; i < 512; i++) {
        p[i] = 0;
    }

    /*
     * 必须映射到当前 CR3 指向的空间, 不能写死 kernel_space:
     * 缺页可能发生在测试空间 A/B, 写死内核空间会"补了页但当前空间仍缺页",
     * iretq 重试又缺页, 来回打转。
     */
    uint64_t page = va & ~(VM_PAGE_SIZE - 1);
    if (vm_map(vm_current_space(), page, frame,
               VM_PTE_PRESENT | VM_PTE_WRITE) != 0) {
        pmm_free_frame(frame);
        vm_in_fault = 0;
        return 0;
    }

    vm_faults++;
    vm_in_fault = 0;
    return 1;   /* 返回 common stub → iretq 重试出错指令 */
}

uint64_t vm_fault_count(void) {
    return vm_faults;
}

int vm_self_test(void) {
    page_table_t *ks = vm_kernel_space();
    int ok = 1;
    page_table_t *a = 0;
    page_table_t *b = 0;
    uint64_t fa = 0;
    uint64_t fb = 0;

    if (ks == 0) {
        vga_printf("M8/VM: FAIL no kernel space\n");
        return -1;
    }

    uint64_t frame = pmm_alloc_frame();
    if (frame == 0) {
        vga_printf("M8/VM: FAIL no free frame\n");
        return -1;
    }

    if (vm_map(ks, VM_SCRATCH_BASE, frame, VM_PTE_PRESENT | VM_PTE_WRITE) != 0) {
        vga_printf("M8/VM: FAIL map\n");
        pmm_free_frame(frame);
        return -1;
    }

    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)VM_SCRATCH_BASE;
    *p = 0x1122334455667788ULL;
    if (*p != 0x1122334455667788ULL) {
        vga_printf("M8/VM: FAIL map readback got %p\n", (void *)(uintptr_t)*p);
        ok = 0;
    } else {
        vga_printf("M8/VM: map PASS\n");
    }

    if (vm_unmap(ks, VM_SCRATCH_BASE) != frame) {
        vga_printf("M8/VM: FAIL unmap\n");
        ok = 0;
    } else {
        vga_printf("M8/VM: unmap PASS\n");
    }

    /* 低 1GB 只允许 2MB 大页, 4KB 映射必须被拒绝。 */
    if (vm_map(ks, 0x100000, frame, VM_PTE_PRESENT | VM_PTE_WRITE) != -1) {
        vga_printf("M8/VM: FAIL huge-page reject\n");
        ok = 0;
    } else {
        vga_printf("M8/VM: huge-page reject PASS\n");
    }

    /* 重复映射必须拒绝, 不能静默覆盖已有 PTE。 */
    if (vm_map(ks, VM_SCRATCH_BASE, frame, VM_PTE_PRESENT | VM_PTE_WRITE) != 0) {
        vga_printf("M8/VM: FAIL remap for double-map test\n");
        ok = 0;
    } else if (vm_map(ks, VM_SCRATCH_BASE, frame, VM_PTE_PRESENT | VM_PTE_WRITE) != -1) {
        vga_printf("M8/VM: FAIL double-map reject\n");
        ok = 0;
    } else {
        vga_printf("M8/VM: double-map reject PASS\n");
    }

    vm_unmap(ks, VM_SCRATCH_BASE);
    pmm_free_frame(frame);

    /*
     * 按需分页: 堆区最后一页第一次写必须触发一次 #PF, 处理器补页后重试;
     * 第二次写同一页不再缺页。
     */
    if (ok) {
        uint64_t c0 = vm_fault_count();
        volatile uint64_t *hp =
            (volatile uint64_t *)(uintptr_t)(VM_HEAP_BASE + VM_HEAP_SIZE - VM_PAGE_SIZE);
        *hp = 0x5A5A5A5A5A5A5A5AULL;
        uint64_t c1 = vm_fault_count();
        uint64_t hv = *hp;
        uint64_t c2 = vm_fault_count();

        if (c1 != c0 + 1 || c2 != c1 || hv != 0x5A5A5A5A5A5A5A5AULL) {
            vga_printf("M8/VM: FAIL demand fault c0=%llu c1=%llu c2=%llu got=%p\n",
                       (unsigned long long)c0, (unsigned long long)c1,
                       (unsigned long long)c2, (void *)(uintptr_t)hv);
            ok = 0;
        } else {
            vga_printf("M8/VM: demand fault c0=%llu c1=%llu c2=%llu PASS\n",
                       (unsigned long long)c0, (unsigned long long)c1,
                       (unsigned long long)c2);
        }
    }

    /*
     * 地址空间隔离: A、B 各自映射同一个虚拟地址, 必须看到各自的值。
     * 切 CR3 之后代码/栈/vga_printf 都靠 entry[0] 的恒等映射, 这正是
     * vm_create_space 复制内核映射的理由。
     */
    if (ok) {
        a = vm_create_space();
        b = vm_create_space();
        if (a == 0 || b == 0) {
            vga_printf("M8/VM: FAIL create space\n");
            ok = 0;
        }
    }

    if (ok) {
        fa = pmm_alloc_frame();
        fb = pmm_alloc_frame();
        if (fa == 0 || fb == 0) {
            vga_printf("M8/VM: FAIL space alloc frame\n");
            ok = 0;
        }
    }

    if (ok) {
        if (vm_map(a, VM_SPACE_TEST_BASE, fa, VM_PTE_PRESENT | VM_PTE_WRITE) != 0 ||
            vm_map(b, VM_SPACE_TEST_BASE, fb, VM_PTE_PRESENT | VM_PTE_WRITE) != 0) {
            vga_printf("M8/VM: FAIL space map\n");
            ok = 0;
        }
    }

    if (ok) {
        vm_switch(a);
        *(volatile uint64_t *)(uintptr_t)VM_SPACE_TEST_BASE = 0xAAAAAAAAAAAAAAAAULL;

        vm_switch(b);
        *(volatile uint64_t *)(uintptr_t)VM_SPACE_TEST_BASE = 0xBBBBBBBBBBBBBBBBULL;

        vm_switch(a);
        uint64_t va = *(volatile uint64_t *)(uintptr_t)VM_SPACE_TEST_BASE;

        vm_switch(b);
        uint64_t vb = *(volatile uint64_t *)(uintptr_t)VM_SPACE_TEST_BASE;

        vm_switch_kernel();

        if (va != 0xAAAAAAAAAAAAAAAAULL || vb != 0xBBBBBBBBBBBBBBBBULL) {
            vga_printf("M8/VM: FAIL space A=%p B=%p\n",
                       (void *)(uintptr_t)va, (void *)(uintptr_t)vb);
            ok = 0;
        } else {
            vga_printf("M8/VM: space A=%p B=%p PASS\n",
                       (void *)(uintptr_t)va, (void *)(uintptr_t)vb);
        }
    }

    /* 只回收数据帧; 地址空间及其页表页按计划允许泄漏。 */
    if (a != 0) {
        vm_unmap(a, VM_SPACE_TEST_BASE);
    }
    if (b != 0) {
        vm_unmap(b, VM_SPACE_TEST_BASE);
    }
    if (fa != 0) {
        pmm_free_frame(fa);
    }
    if (fb != 0) {
        pmm_free_frame(fb);
    }

    return ok ? 0 : -1;
}
