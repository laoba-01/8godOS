/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 本文件全部内容(2026-10-12, M7)。
 *   队员验证: 待 Task 2 交接提问检验后补记。
 */

#ifndef IRQ_H
#define IRQ_H

#include "idt.h"

/*
 * IRQ 处理函数的类型: 收 regs_t*(被打断现场的栈帧指针), 无返回值。
 *
 * 为什么把 regs_t* 透传下来而不是只传 irq 号:
 *   处理器将来可能需要读写被中断时的寄存器(比如报错时打印 rip、
 *   将来实现信号时改 rip 让进程重新执行)。现在没人用它, 但接口先留好 ——
 *   改函数签名比改实现贵得多。
 */
typedef void (*isr_reg_t)(regs_t *r);

/*
 * 中断/异常的 C 侧总入口, 由 isr.asm 的 common stub 调用。
 *   向量 < 32  → 走异常路径(打印现场后停机)
 *   向量 >= 32 → 走 IRQ 分发
 */
void irq_dispatch(regs_t *r);

#endif /* IRQ_H */
