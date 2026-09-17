#include "vga.h"
#include "idt.h"

/*
 * M6 用例开关。一次运行只能验一条:
 * 异常处理器返回不到故障指令之后(iretq 会重新执行那条出错指令), 所以四次运行,
 * 每次改这一个数字重新 make。
 *   0 = 不触发(默认, 正常启动)
 *   1 = #DE 除零          2 = #UD 非法指令
 *   3 = #GP 越界段选择子   4 = #PF 读未映射地址
 */
#define M6_CASE 0

static void m6_trigger(void) {
#if M6_CASE == 1
    /* 两个变量都要 volatile, 否则 -O2 会在编译期把这次除法优化掉, 表现是"什么都没发生" */
    volatile int a = 1, b = 0;
    volatile int c = a / b;
    (void)c;
#elif M6_CASE == 2
    __asm__ volatile ("ud2");
#elif M6_CASE == 3
    /* GDT 只有 3 项、limit=23, 选择子 0x30 越界。
       这一路错误码由 CPU 压, 值就是那个越界的段选择子 0x30 —— 是 M6 最强的判别点。 */
    __asm__ volatile ("mov $0x30, %%ax; mov %%ax, %%ds" ::: "ax");
#elif M6_CASE == 4
    /* 恒等映射只覆盖前 2MB(boot.asm 的 2MB 大页), 0x300000 对应的 PD[1] 为 0。
       指针和结果都要 volatile —— 否则 -O2 会把整条访存优化掉。 */
    volatile uint64_t *p = (volatile uint64_t *)0x300000UL;
    volatile uint64_t v = *p;
    (void)v;
#endif
}

void kmain(void) {
    vga_clear();
    idt_init();

    vga_printf("A=%c S=%s P=%%\n", 'x', "str");
    vga_printf("d=%d u=%u x=%x X=%016lx p=%p w=%010lu\n",
               -42, 42u, 0xdeadbeef, 0x1234ul, (void *)0xb8000, 7ul);
    vga_puts("Hello, kernel!\n");
    vga_puts("This is 8GodOS\n");
    vga_puts("M5: VGA terminal\n");

    m6_trigger();

    for (;;) { __asm__ volatile("hlt"); }
}
