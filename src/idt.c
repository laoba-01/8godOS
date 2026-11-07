/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 本文件全部内容(2026-09-17, M6);
 *            M7 (2026-10-12) idt_init 补填 32~47 门;
 *            M7 Task 2 Step 4 (2026-10-13) isr_handler 的 `vector >= IDT_IRQ_BASE`
 *            分支 —— **原档位 🔴(队员写), 由队员走出效率出口「我只会写伪代码,
 *            真代码你来写吧」, AI 代写**。队员已给出 `>=` 的比较方向。
 *   AI 生成: M8 Task 5 (2026-11-07) isr_handler 在 panic 之前加 #PF 恢复分支
 *            (`vector == 14 && vm_handle_page_fault(r)`)。
 *   队员验证: 四条异常用例(#DE/#UD/#GP/#PF)的解码结果逐字比对;
 *             #GP 的 error=0x30 作为「错误码来自 CPU」的判别点。
 *   ⚠ 该分支的交接提问检验见 docs/AI-使用记录.md 第九节(2026-10-13 行)。
 */

#include "idt.h"
#include "irq.h"
#include "vga.h"
#include "vm.h"

/* stub 地址表, 由 isr.asm 生成 —— M7 起是 48 项(0~31 异常, 32~47 外部中断) */
extern void *isr_stub_table[48];

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
    /* 先把 256 项全部清 0, 再填门 —— 顺序不能反(先填后清会把门擦掉)。
       清 0 的两层理由:
         1. 兜底。M7 Task 1 之后 _start64 已经清了真正的 .bss, 所以 idt[] 进来时
            本来就该是全 0; 但这层循环留着, 让 idt_init 的正确性不依赖引导期的行为。
         2. 未填的门必须是"不存在"(P=0)的, 否则被意外取到会跳进垃圾地址。 */
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

    /* 外部中断 32~47: 门参数与异常门**完全相同**(同选择子、同 type_attr、同 ist=0)。
       中断门(0x8E)与陷阱门的区别只在 CPU 是否自动清 IF —— 这里要 0x8E,
       让硬件中断处理期间 IF 自动清零, 防止 IRQ1 处理到一半被同类中断重入。 */
    for (int i = IDT_IRQ_BASE; i <= IDT_IRQ_LAST; i++) {
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
    /* M7 的分水岭: 外部中断(可恢复)与 CPU 异常(不可恢复)在这里分道扬镳。
       向量 32~47 是 8259A 送来的 IRQ —— 处理完要原路返回被中断的代码;
       向量 0~31 是 CPU 异常 —— 没有内存管理, 一个都修不了, 一律停机。
       判据只写 `>= IDT_IRQ_BASE` 而不写区间上界: 门的 48~255 项全是 P=0(不存在的门),
       硬件送不进来, 软件 `int $0x30` 会先撞 #GP, 所以到这里的一定落在 32~47。
       分支必须排在打印之前 —— 否则 IRQ 也会先打一行 "EXCEPTION", 且永远走到停机循环。 */
    if (r->vector >= IDT_IRQ_BASE) {
        irq_dispatch(r);
        return;      /* 返回 common stub → pop 掉现场 → iretq 回到被打断的那条指令之后 */
    }

    /* #PF(14) 先问内存层能不能补页; 补上了就返回, iretq 会重试出错指令。
       处理不了(堆区外/保护错误/重入)才落到下面的 panic。 */
    if (r->vector == 14 && vm_handle_page_fault(r)) {
        return;
    }

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
