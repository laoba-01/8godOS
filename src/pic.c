/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 2026-10-20, M7 Task 4 骨架 + ICW 数值 + 注释。
 *            原档位 🟡(骨架 AI 写, 数值队员填) / 🔴(从片 EOI 分支队员补);
 *            队员走出效率出口「数值填好并逐段讲」, 故数值与从片分支由 AI 一并写。
 *   队员验证: Task 4 验收 A(IMR=0xFD)、B(sendkey h → v=21)、C(连按三次)。
 */

#include "pic.h"
#include "io.h"

/*
 * 8259A 初始化 = 向两片各连发 4 个 ICW。
 * 四个 ICW 的顺序由芯片状态机决定, 不能调换; ICW1 走命令口, 其余走数据口。
 *
 *   ICW1(命令口): "开始初始化", 同时定下触发方式 / 是否级联 / 后面要不要 ICW4。
 *       0x11 = 0001_0001
 *         bit4 = 1   固定标志: 这是 ICW1
 *         bit3 = 0   LTIM: 边沿触发(ISA 设备的 IRQ 线用边沿)
 *         bit2 = 0   ADI : 8086 模式忽略
 *         bit1 = 0   SNGL: 0 = 级联(有从片)
 *         bit0 = 1   IC4 : 1 = 后面还要发 ICW4
 *
 *   ICW2(数据口): 向量基址。高 5 位是 IRQ0 的向量号, 低 3 位由 IRQ 号自动填。
 *       主片 0x20 → IRQ0..7  = 0x20..0x27
 *       从片 0x28 → IRQ8..15 = 0x28..0x2F
 *       **这就是重映射**: 出厂值是 IRQ0..7 → 0x08..0x0F, 会撞上 CPU 异常。
 *       软件侧 IDT 的 32~47 号门(Task 2)必须和这里的落点一致。
 *
 *   ICW3(数据口): 级联连接。
 *       主片: 8 位掩码, bit n = 1 表示 IRQn 上挂了从片 → 0x04 (bit2)
 *       从片: 低 3 位 = "我接在主片的哪条线上" → 0x02
 *
 *   ICW4(数据口): 工作模式。
 *       0x01 = 0000_0001
 *         bit0 = 1   uPM : x86 模式
 *         bit1 = 0   AEOI: 不自动 EOI —— 要在 handler 读完设备后再手动发
 *         bit3 = 0   BUF : 非缓冲模式
 *       从片的这个值一样: BUF=0 时 M/S 位不参与判断。
 */
void pic_init(void) {
    /* ICW1 先发给两片: 从这一刻起, 两片各自进入自己的 ICW 序列 */
    outb(PIC1_CMD, PIC_ICW1_INIT);
    outb(PIC2_CMD, PIC_ICW1_INIT);

    /* ICW2: 向量基址 —— 硬件侧的落点, 必须和 idt.h 的 IDT_IRQ_BASE(32) 对齐 */
    outb(PIC1_DATA, PIC_ICW2_MASTER);
    outb(PIC2_DATA, PIC_ICW2_SLAVE);

    /* ICW3: 主从怎么接 */
    outb(PIC1_DATA, PIC_ICW3_MASTER);
    outb(PIC2_DATA, PIC_ICW3_SLAVE);

    /* ICW4: x86 模式 + 手动 EOI */
    outb(PIC1_DATA, PIC_ICW4_8086);
    outb(PIC2_DATA, PIC_ICW4_8086);

    /*
     * 初始化完先全屏蔽。解屏蔽是显式动作, 只由 pic_unmask 做 ——
     * 这样"哪条线能进来"在代码里只有一处说了算。
     *
     * 顺序之所以重要: 重映射(ICW2)必须在任何一条线被放行之前完成。
     * 否则窗口期内硬件仍按出厂落点 0x08~0x0F 发货, IRQ 会冒充 CPU 异常。
     */
    outb(PIC1_DATA, PIC_MASK_ALL);
    outb(PIC2_DATA, PIC_MASK_ALL);
}

/*
 * 读-改-写 IMR。
 * 这里没有临界区: 调用只发生在 sti 之前(初始化阶段)。
 * 将来若在运行期动态改屏蔽, 读和写之间要 cli/sti, 否则可能丢掉另一个 CPU
 * 或中断路径同时改的位(当前单核、初始化期, 不构成问题)。
 */
static void pic_update_mask(uint8_t irq, int masked) {
    if (irq >= 16) {
        return;
    }

    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7u));
    uint8_t  old  = inb(port);

    outb(port, masked ? (uint8_t)(old | bit) : (uint8_t)(old & (uint8_t)~bit));
}

void pic_mask(uint8_t irq) {
    pic_update_mask(irq, 1);
}

void pic_unmask(uint8_t irq) {
    if (irq >= 8 && irq < 16) {
        /*
         * 从片的请求要从主片 IRQ2 这条"电话线"转上来。
         * 只放开从片的 mask、主片 IRQ2 仍屏蔽着, 从片中断永远到不了 CPU。
         * 现在没有从片设备, 这一支走不到; 写对是为了将来接鼠标/RTC 时不踩坑。
         */
        pic_update_mask(2, 0);
    }
    pic_update_mask(irq, 0);
}

/*
 * EOI: 告诉 PIC "这条线服务完了", 清掉 ISR 里对应的位。
 *
 * 从片(IRQ >= 8)要发两次:
 *   1. 先给从片 0xA0 发 → 清从片自己的 ISR 位;
 *   2. 再给主片 0x20 发 → 清主片 IRQ2(级联线)的 ISR 位。
 * 顺序不能反: 先放主片, 从片还认为自己在服务中, 两边的账就不一致了。
 *
 * 现在没有从片设备, 这一支不可观测; Task 5 之后接鼠标/RTC 时它必须是对的。
 */
void pic_send_eoi(uint8_t irq) {
    if (irq >= 16) {
        return;
    }
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}
