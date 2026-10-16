/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 本文件全部内容(2026-09-17, M6);
 *            M7 (2026-10-12) 加 IDT_IRQ_BASE / IDT_IRQ_LAST / IDT_IRQ_COUNT。
 *   队员验证: regs_t 的字段顺序契约由 _Static_assert 在编译期强制;
 *             四条用例的 rip/cs/rflags 取值与预期相符。
 */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>
#include <stddef.h>

/*
 * 异常入口的栈帧。字段顺序 == 内存顺序(从低地址往高):
 *
 *     [15 个通用寄存器][向量号][错误码/0][rip][cs][rflags]
 *
 * 低地址那 15 个是 common stub 压的, 后面 5 个是 stub / CPU 压的。
 * ring0 下 CPU 不压 RSP 和 SS, 所以这里没有这两个字段 ——
 * 若要显示"被打断的 rsp", 用 (uint64_t)&r->rflags + 8 从栈帧推算,
 * 那是算出来的, 不是 CPU 压栈字段。
 *
 * 谁改了字段顺序、或在 common stub 里多压一个寄存器,
 * 下面那组 _Static_assert 会在编译期炸, 不用等到某个异常打出一串莫名其妙的 RIP。
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags;
} regs_t;

_Static_assert(offsetof(regs_t, vector)     == 15 * 8, "regs_t 与 common stub 压栈顺序不一致");
_Static_assert(offsetof(regs_t, error_code) == 16 * 8, "同上");
_Static_assert(offsetof(regs_t, rip)        == 17 * 8, "同上");
_Static_assert(offsetof(regs_t, cs)         == 18 * 8, "同上");
_Static_assert(offsetof(regs_t, rflags)     == 19 * 8, "同上");
_Static_assert(sizeof(regs_t)               == 160,    "栈帧大小应为 20 qword");

/* 64 位中断门描述符(Intel 手册 Vol.3 图 6-8) */
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

_Static_assert(sizeof(idt_entry_t) == 16, "IDT 门描述符必须是 16 字节");

/*
 * 外部中断(IRQ)占用的向量号区间。
 *
 * 出厂时 8259A 把 IRQ0~7 映射在向量 0x08~0x0F, 正好压在 #DF/#TS/#NP/#SS/#GP/#PF
 * 这些 CPU 异常上 —— 所以用之前**必须重映射**, 让 IRQ0~15 整体挪到 32~47。
 * 这里是"软件侧的地址"; 硬件侧的落点由 pic.c 的 ICW2 决定, 两边必须一致。
 */
#define IDT_IRQ_BASE  32                              /* IRQ0 */
#define IDT_IRQ_LAST  47                              /* IRQ15 */
#define IDT_IRQ_COUNT (IDT_IRQ_LAST - IDT_IRQ_BASE + 1)   /* 16 条线 */

void idt_init(void);

#endif /* IDT_H */
