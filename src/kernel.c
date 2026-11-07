/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   队员编写: M3 的 kmain 与 vga_puts() 调用;
 *             2026-10-19 M7 Task 3 的 my_probe() 与 irq_register() 调用
 *             (逐行带写; 验收时 AI 临时将 M7_CASE 置 1/2, 已复位为 0)。
 *   AI 生成: M5 结构调整后的版本(2026-09-15); M6 的 idt_init() 调用与
 *            异常用例开关 m6_trigger() / M6_CASE(2026-09-17);
 *            M7 (2026-10-12) 软件中断探路开关 m7_probe() / M7_CASE。
 *   AI 删除: 2026-10-13 应队员要求删去 kmain 开头的 M5 遗留输出 —— 两行
 *            vga_printf 自检(原先逐条过 %c/%s/%%/%d/%u/%x/%X/%p)与
 *            "M5: VGA terminal" 一行。副作用: 启动路径上不再有非十六进制
 *            转换符的回归覆盖, 需要时从 git 取回即可。
 *   AI 生成: 2026-10-20 (M7 Task 4) pic_init()/pic_unmask(1) 初始化序列、
 *            临时的 IMR 打印与 sti;hlt 死循环; 同时删去 Task 3 的 my_probe
 *            (真实 IRQ1 已接通, 探针会以 VGA 输出干扰中断上下文)。
 *   AI 生成: 2026-10-20 (M7 Task 5) keyboard_init() 调用、真正的消费主循环;
 *            删去 Task 4 的临时 IMR 打印与 Task 3 的软件中断探路开关
 *            (M7_CASE/m7_probe) —— 键盘已接通, 不需要再用 int $0x21 探路。
 *   AI 生成: 2026-11-01 (M8 Task 1) kmain 改为接收 multiboot2 指针并调用 pmm_init()。
 *            2026-11-02 (M8 Task 2) keyboard_init() 之后调用 pmm_self_test()。
 *            2026-11-07 (M8 Task 3) idt_init() 之后、pmm_init() 之前调用 vm_init();
 *            自检顺序 pmm → vm。
 *   队员验证: 各里程碑由队员在 QEMU 中运行判读屏幕输出。
 */

#include "vga.h"
#include "idt.h"
#include "pmm.h"
#include "vm.h"
#include "pic.h"
#include "keyboard.h"

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

void kmain(uint64_t mb_info) {
    vga_clear();

    /*
     * 初始化顺序不可颠倒: 装门 → 解析内存 → 重映射 → 解屏蔽 → 注册 handler → sti。
     *   1. idt_init(): 门先就位, 否则解析内存时一旦缺页没有门可跳;
     *   2. pmm_init(): 解析 multiboot2 memory map(此时 IF=0, 不会被中断打断);
     *   3. pic_init(): 把 IRQ 从出厂落点 0x08~0x0F 挪到 0x20~0x2F;
     *   4. pic_unmask(1): 放行 IRQ1(键盘); 此刻 IF 仍是 0, 中断不会立刻来;
     *   5. keyboard_init(): 把 keyboard_handler 注册到分发表的第 1 格;
     *   6. 主循环里的 sti: 真正开门。
     * 反过来的话, 窗口期内硬件会按出厂落点发货 → IRQ 冒充 CPU 异常。
     */
    idt_init();

    /* 只读 CR3 记录内核 PML4, 必须在 pmm_init 之前:
       之后 #PF 处理器要靠一个有效的 kernel_space。 */
    if (vm_init() != 0) {
        vga_printf("M8: vm_init failed, halt\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    if (pmm_init(mb_info) != 0) {
        vga_printf("M8: pmm_init failed, halt\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    pic_init();
    pic_unmask(1);
    keyboard_init();

    /*
     * M8 自检: 顺序 pmm → vm → heap。
     * 放在 keyboard_init() 之后、主循环之前 —— 自检会分配帧、写图案,
     * 此时中断链已就绪但不进主循环, 输出不会被键盘输入打断。
     */
    if (pmm_self_test() != 0) {
        vga_printf("M8/PMM: self-test failed, halt\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    if (vm_self_test() != 0) {
        vga_printf("M8/VM: self-test failed, halt\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    vga_puts("Hello, kernel!\n");
    vga_puts("This is 8GodOS\n");

    m6_trigger();

    /*
     * 主循环 = 中断的下半部:
     *   - 中断只负责把字符塞进环形缓冲(上半部, 越快越好);
     *   - 这里负责取出来、调用 M5 的 vga_putchar 做真正的屏幕输出(慢活)。
     *
     * 缓冲非空时一次取干净, 然后 sti; hlt 睡到下一个中断。
     *
     * sti 与 hlt 必须在同一条 asm、相邻执行:
     * STI 后 CPU 有"中断影子"(下一条指令执行前不响应中断), 所以这两条
     * 合起来是一个原子动作 —— 不会出现"刚开中断、还没睡下, 中断先到"
     * 导致 hlt 睡死、缓冲里的字符没人取的情况。
     */
    for (;;) {
        int c;

        while ((c = kb_get()) >= 0) {
            vga_putchar((char)c);
        }

        __asm__ volatile ("sti; hlt" ::: "memory");
    }
}
