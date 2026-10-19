/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   队员编写: 分发表 + irq_register + irq_dispatch 函数体
 *             (2026-10-19, M7 Task 3)。
 *   AI 生成: 2026-10-12 的占位实现(已删除); 2026-10-19 采用"逐行带写" ——
 *             AI 把实现拆成 5 步、给出每行的写法与解释、跑验收命令;
 *             队员逐行键入代码。收尾的格式整理、注释与来源声明由 AI 完成。
 *   队员验证: M7_CASE=1 → my_probe called;
 *             M7_CASE=2 → unhandled IRQ 2 后内核继续运行。
 *   ⚠ 交接提问检验(2 题)顺延至 2026-10-20, 尚欠。
 */

#include "irq.h"
#include "vga.h"

/*
 * IRQ 分发表: 第 n 格存 IRQ n 的处理函数, 未注册的格子为 NULL。
 * 它住在 .bss —— 初值全 0 由 M7 Task 1 的 _start64 清零循环保证,
 * 不依赖加载器是否按 ELF 规范清零。
 * 不需要 volatile: 注册发生在 sti 之前, 之后表只读不写。
 */
static isr_reg_t irq_handlers[IDT_IRQ_COUNT];

void irq_register(uint8_t irq, isr_reg_t fn) {
    /* 合法下标是 0~15。越界注册是代码 bug, 这里静默拒绝, 绝不越界写表。 */
    if (irq >= IDT_IRQ_COUNT) {
        return;
    }
    irq_handlers[irq] = fn;
}

void irq_dispatch(regs_t *r) {
    /* 向量号 → IRQ 号: 33(0x21) → 1。用 uint64_t 接收:
       vector < 32 时无符号减法会下溢成巨大的数, 下一行的检查正好拦住它。 */
    uint64_t irq = r->vector - IDT_IRQ_BASE;

    if (irq >= IDT_IRQ_COUNT) {
        return;
    }

    isr_reg_t fn = irq_handlers[irq];

    if (fn != NULL) {
        fn(r);
    } else {
        /* 外部中断是硬件发来的, 未注册不是致命错误: 提示后正常返回。
           注意: 这是唯一允许在中断上下文调用 vga_* 的路径, 仅用于诊断;
           真正的 IRQ 处理函数不碰 VGA(Task 5)。 */
        vga_printf("unhandled IRQ %lu\n", (unsigned long)irq);
    }

    /* EOI 不在这里发 —— 等 Task 4 引入 pic.c 后, 由 pic_send_eoi()
       在 handler 之后调用。软件 int 不经过 8259A, 现在发也没有意义。 */
}
