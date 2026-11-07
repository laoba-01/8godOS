/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 6 (2026-11-07) first-fit + 切分 + 合并 + 自检。
 *
 * 本文件是堆层, 只回答一个问题: 一块虚拟区怎么切成字节块?
 *   - 物理页由 Task 5 的 #PF 处理器在第一次写时补上, 这里不碰 pmm;
 *   - 地址空间/页表由 vm.c 管。
 *
 * 块布局(固定, 16 字节头):
 *   [magic u32][free u32][size u64][payload ...]
 *   size = 数据区大小(不含头), 16 字节对齐。
 *   最后一个块的 next 正好等于 arena_end, 不额外放哨兵块。
 */
#include "heap.h"
#include "vm.h"
#include "vga.h"

#define HEAP_MAGIC 0x48454150u   /* "HEAP" */

typedef struct {
    uint32_t magic;
    uint32_t free;
    uint64_t size;
} block_t;

static block_t *heap_start;
static block_t *heap_end;

_Static_assert(sizeof(block_t) == 16, "堆块头必须是 16 字节");

/* 堆被写坏 / double free: 学习期宁可炸响, 不要静默。 */
static void heap_panic(const char *why) {
    vga_printf("%s\n", why);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

int heap_init(void) {
    heap_start = (block_t *)(uintptr_t)VM_HEAP_BASE;
    heap_end = (block_t *)(uintptr_t)(VM_HEAP_BASE + VM_HEAP_SIZE);

    /* 第一次写堆区会触发 #PF, 处理器补页后这条赋值才完成。 */
    heap_start->magic = HEAP_MAGIC;
    heap_start->free = 1;
    heap_start->size = VM_HEAP_SIZE - sizeof(block_t);
    return 0;
}

void *kmalloc(size_t size) {
    if (size == 0 || size > VM_HEAP_SIZE || heap_start == 0) {
        return 0;
    }

    /* 16 字节对齐, 用位运算不用 %。 */
    size_t need = (size + 15) & ~(size_t)15;
    block_t *b = heap_start;

    while ((uint8_t *)b < (uint8_t *)heap_end) {
        if (b->magic != HEAP_MAGIC) {
            heap_panic("M8/HEAP: corrupt magic in kmalloc");
        }

        if (b->free && b->size >= need) {
            size_t remain = b->size - need;

            /* 剩余空间还够放一个块头 + 至少 16 字节数据, 才值得切分。 */
            if (remain >= sizeof(block_t) + 16) {
                block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + need);
                nb->magic = HEAP_MAGIC;
                nb->free = 1;
                nb->size = remain - sizeof(block_t);
                b->size = need;
            }

            b->free = 0;
            return (uint8_t *)b + sizeof(block_t);
        }

        block_t *next = (block_t *)((uint8_t *)b + sizeof(block_t) + b->size);
        if ((uint8_t *)next >= (uint8_t *)heap_end) {
            break;
        }
        b = next;
    }

    return 0;
}

void kfree(void *ptr) {
    if (ptr == 0) {
        return;
    }

    block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    if ((uint8_t *)b < (uint8_t *)heap_start || (uint8_t *)b >= (uint8_t *)heap_end) {
        heap_panic("M8/HEAP: kfree out of range");
    }
    if (b->magic != HEAP_MAGIC) {
        heap_panic("M8/HEAP: kfree bad magic");
    }
    if (b->free) {
        heap_panic("M8/HEAP: double free");
    }

    b->free = 1;

    /* 向后合并: 只改 size/free, 不搬数据。 */
    for (;;) {
        block_t *next = (block_t *)((uint8_t *)b + sizeof(block_t) + b->size);
        if ((uint8_t *)next >= (uint8_t *)heap_end) {
            break;
        }
        if (next->magic != HEAP_MAGIC) {
            heap_panic("M8/HEAP: bad magic in forward merge");
        }
        if (!next->free) {
            break;
        }
        b->size += sizeof(block_t) + next->size;
    }

    /* 向前合并: 从头走到 b 的前一块。 */
    block_t *prev = 0;
    block_t *cur = heap_start;
    while ((uint8_t *)cur < (uint8_t *)b) {
        prev = cur;
        cur = (block_t *)((uint8_t *)cur + sizeof(block_t) + cur->size);
    }
    if (prev != 0 && prev->free) {
        prev->size += sizeof(block_t) + b->size;
    }
}

size_t heap_free_bytes(void) {
    size_t total = 0;
    block_t *b = heap_start;

    while ((uint8_t *)b < (uint8_t *)heap_end) {
        if (b->magic != HEAP_MAGIC) {
            heap_panic("M8/HEAP: corrupt magic in free_bytes");
        }
        if (b->free) {
            total += b->size;
        }

        block_t *next = (block_t *)((uint8_t *)b + sizeof(block_t) + b->size);
        if ((uint8_t *)next >= (uint8_t *)heap_end) {
            break;
        }
        b = next;
    }

    return total;
}

/* 两个区间是否相交(半开区间)。 */
static int ranges_overlap(const void *p, size_t plen,
                          const void *q, size_t qlen) {
    uintptr_t ps = (uintptr_t)p;
    uintptr_t pe = ps + plen;
    uintptr_t qs = (uintptr_t)q;
    uintptr_t qe = qs + qlen;
    return ps < qe && qs < pe;
}

static int write_read_pattern(volatile uint8_t *p, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++) {
        p[i] = (uint8_t)(seed ^ (uint8_t)i);
    }
    for (size_t i = 0; i < len; i++) {
        if (p[i] != (uint8_t)(seed ^ (uint8_t)i)) {
            return -1;
        }
    }
    return 0;
}

int heap_self_test(void) {
    int ok = 1;

    void *a = kmalloc(16);
    void *b = kmalloc(200);
    void *c = kmalloc(1000);
    if (a == 0 || b == 0 || c == 0) {
        vga_printf("M8/HEAP: FAIL alloc a=%p b=%p c=%p\n", a, b, c);
        return -1;
    }
    vga_printf("M8/HEAP: a=%p b=%p c=%p\n", a, b, c);

    /* 16 字节对齐。 */
    if (((uintptr_t)a & 15) != 0 || ((uintptr_t)b & 15) != 0 ||
        ((uintptr_t)c & 15) != 0) {
        vga_printf("M8/HEAP: FAIL align\n");
        ok = 0;
    } else {
        vga_printf("M8/HEAP: align PASS  ");
    }

    /* 必要的非重叠检查: 只比较实际请求的字节数。 */
    if (ranges_overlap(a, 16, b, 200) || ranges_overlap(a, 16, c, 1000) ||
        ranges_overlap(b, 200, c, 1000)) {
        vga_printf("M8/HEAP: FAIL overlap\n");
        ok = 0;
    } else {
        vga_printf("overlap PASS  ");
    }

    if (write_read_pattern((volatile uint8_t *)a, 16, 0xA0) != 0 ||
        write_read_pattern((volatile uint8_t *)b, 200, 0xB0) != 0 ||
        write_read_pattern((volatile uint8_t *)c, 1000, 0xC0) != 0) {
        vga_printf("FAIL pattern\n");
        ok = 0;
    } else {
        vga_printf("pattern PASS\n");
    }

    /* 请求整个堆, 任何剩余空洞都不可能装下。 */
    void *oom = kmalloc(VM_HEAP_SIZE);
    if (oom != 0) {
        vga_printf("M8/HEAP: FAIL oom returned %p\n", oom);
        ok = 0;
    } else {
        vga_printf("M8/HEAP: oom PASS\n");
    }

    /* 跨页: 大块分配后每 4KB 写一次, 缺页计数必须增加。 */
    uint64_t f0 = vm_fault_count();
    volatile uint8_t *big = kmalloc(100000);
    if (big == 0) {
        vga_printf("M8/HEAP: FAIL big alloc\n");
        ok = 0;
    } else {
        for (size_t i = 0; i < 100000; i += 4096) {
            big[i] = (uint8_t)(i >> 12);
        }
        int pattern_ok = 1;
        for (size_t i = 0; i < 100000; i += 4096) {
            if (big[i] != (uint8_t)(i >> 12)) {
                pattern_ok = 0;
            }
        }
        uint64_t f1 = vm_fault_count();
        if (!pattern_ok || f1 <= f0) {
            vga_printf("M8/HEAP: FAIL cross-page pattern=%d faults=%llu\n",
                       pattern_ok, (unsigned long long)(f1 - f0));
            ok = 0;
        } else {
            vga_printf("M8/HEAP: cross-page fault PASS (faults +%llu)\n",
                       (unsigned long long)(f1 - f0));
        }
    }

    /*
     * 合并测试。先把剩余空闲区整个占满, 否则 first-fit 会直接从尾部大块
     * 满足后面的分配, 根本用不到 a+b 合并, 测试会假通过。
     * a、b 相邻: 先释放 b 再释放 a, a 应向后吞掉 b 的块。
     * 合并后 a 块 = 16(a 数据) + 16(b 块头) + 208(b 数据, 200 对齐到 16) = 240。
     * d 取 224 而不是 200: 不合并时 b 块单独就有 208 字节, kmalloc(200) 照样成功;
     * 只有合并后的 240 才装得下 224。
     */
    if (ok) {
        size_t free_total = heap_free_bytes();
        void *fill = kmalloc(free_total);
        if (fill == 0 || heap_free_bytes() != 0) {
            vga_printf("M8/HEAP: FAIL fill (asked=0x%lx left=0x%lx)\n",
                       (unsigned long)free_total,
                       (unsigned long)heap_free_bytes());
            ok = 0;
        }
    }

    if (ok) {
        kfree(b);
        kfree(a);
        size_t free_after = heap_free_bytes();
        void *d = kmalloc(224);
        if (d == 0) {
            vga_printf("M8/HEAP: FAIL coalesce (free=0x%lx)\n",
                       (unsigned long)free_after);
            ok = 0;
        } else {
            vga_printf("M8/HEAP: coalesce PASS (free=0x%lx)\n",
                       (unsigned long)free_after);
        }
    }

    return ok ? 0 : -1;
}
