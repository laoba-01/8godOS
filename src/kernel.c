/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   队员编写: M3 的 kmain 与 vga_puts() 调用。
 *   AI 生成: M5 结构调整后的版本(2026-09-15); M6 的 idt_init() 调用与
 *            异常用例开关 m6_trigger() / M6_CASE(2026-09-17);
 *            M7 (2026-10-12) 软件中断探路开关 m7_probe() / M7_CASE。
 *   AI 删除: 2026-10-13 应队员要求删去 kmain 开头的 M5 遗留输出 —— 两行
 *            vga_printf 自检(原先逐条过 %c/%s/%%/%d/%u/%x/%X/%p)与
 *            "M5: VGA terminal" 一行。副作用: 启动路径上不再有非十六进制
 *            转换符的回归覆盖, 需要时从 git 取回即可。
 *   队员验证: 各里程碑由队员在 QEMU 中运行判读屏幕输出。
 */

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

/*
 * M7 探路开关 —— 用**软件中断**验证分发路径, 不需要 PIC、不需要 sti。
 *
 * 原理: `int $0x21` 是 CPU 自己产生的, 但它和硬件 IRQ1 走的是**同一条路** ——
 * 查同一张 IDT、进同一个 stub(第 33 号)、同一个 common、同一个 isr_handler。
 * 唯一的分岔点是"谁把向量号送进 CPU": 软件指令给立即数, 8259A 给 IRQ 号+ICW2 偏移。
 * 所以这一步能先把软件侧全部验通, 再引入硬件的不确定性(重映射、屏蔽、EOI)。
 *
 *   0 = 不触发(默认)
 *   1 = int $0x21 → IRQ1, 有注册(Task 2 验分发通路 / Task 3 验注册生效)
 *   2 = int $0x22 → IRQ2, 没注册(Task 3 验未注册不炸)
 */
#define M7_CASE 0

static void m7_probe(void) {
#if M7_CASE == 1
    __asm__ volatile ("int $0x21");
#elif M7_CASE == 2
    __asm__ volatile ("int $0x22");
#endif
}

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

    vga_puts("Hello, kernel!\n");
    vga_puts("This is 8GodOS\n");

    m6_trigger();
    m7_probe();

    for (;;) { __asm__ volatile("hlt"); }
}
