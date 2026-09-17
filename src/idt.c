/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 本文件全部内容(2026-09-17, M6)。
 *   队员验证: 四条异常用例(#DE/#UD/#GP/#PF)的解码结果逐字比对;
 *             #GP 的 error=0x30 作为「错误码来自 CPU」的判别点。
 */

#include "idt.h"
#include "vga.h"

/* 32 个异常 stub 的地址表, 由 isr.asm 生成 */
extern void *isr_stub_table[32];

/* 门参数: 内核代码段选择子; P=1, DPL=0, 64 位中断门 */
#define IDT_SELECTOR  0x08
#define IDT_TYPE_ATTR 0x8E

#define IDT_ENTRIES   256
#define IDT_EXCEPTIONS 32

static idt_entry_t idt[IDT_ENTRIES] __attribute__((aligned(16)));

/* Intel 手册 Vol.3 表 6-1 */
static const char *const exception_names[IDT_EXCEPTIONS] = {
    "#DE Divide Error",
    "#DB Debug",
    "NMI Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR BOUND Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection",
    "#PF Page Fault",
    "Reserved",
    "#MF x87 FPU Error",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD FP Exception",
    "#VE Virtualization Exception",
    "#CP Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "#HV Hypervisor Injection",
    "#VC VMM Communication",
    "#SX Security Exception",
    "Reserved",
};

/* 缺页的出错地址只在 CR2 里, 错误码只给访问类型(P/W/U 位) */
static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

/* 把第 vec 号门填成指向 handler。64 位 offset 要拆成低/中/高三段, 是 x86 的历史遗留布局。 */
static void set_gate(int vec, uint64_t handler) {
    idt[vec].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[vec].selector    = IDT_SELECTOR;
    idt[vec].ist         = 0;                    /* 不做 IST, 用当前栈 */
    idt[vec].type_attr   = IDT_TYPE_ATTR;
    idt[vec].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[vec].zero        = 0;
}

void idt_init(void) {
    /* 必须先把 256 项全部清 0: .bss 不保证为 0(_start64 没清 BSS),
       只写 0~31 会留下 224 个垃圾描述符。 */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0;
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].zero        = 0;
    }

    for (int i = 0; i < IDT_EXCEPTIONS; i++) {
        set_gate(i, (uint64_t)isr_stub_table[i]);
    }

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {
        .limit = sizeof(idt) - 1,        /* 256 * 16 - 1 = 4095 = 0x0FFF */
        .base  = (uint64_t)&idt[0],
    };

    __asm__ volatile ("lidt %0" :: "m"(idtr));
}

/*
 * 由 common stub `call` 进来, 必须非 static。
 * 入参就是栈顶, 即 regs_t 的首地址。
 *
 * M6 里 32 个向量全部走到这里 panic —— 没有内存管理, 一个异常都修不了。
 * 之所以要完整打印现场而不是只报个号, 是因为这些字段本身就是 M6 的验收目标:
 * 向量号/名称证明分发对了, 错误码证明两类 stub 分对了, cr2 证明 #PF 取对了地址。
 */
void isr_handler(regs_t *r) {
    vga_printf("EXCEPTION 0x%02lX %s\n",
               r->vector, exception_names[r->vector & 31]);
    vga_printf("error=0x%016lx\n", r->error_code);
    if (r->vector == 14) {
        vga_printf("cr2=0x%016lx\n", read_cr2());
    }
    vga_printf("rip=0x%016lx cs=0x%04lx rflags=0x%016lx\n",
               r->rip, r->cs, r->rflags);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
