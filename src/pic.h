/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 本文件全部内容(2026-10-20, M7 Task 4)。
 *            原档位 ⚪(常量与原型由 AI 写); 经队员同意, ICW 数值一并填入。
 *   队员验证: Task 4 验收 A/B/C(IMR=0xFD、v=21、连按三次三次都到)。
 */

#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/*
 * 8259A 的两组端口。
 * 每片有两个口: A0=0 是命令口(收 ICW1 / OCW2 / OCW3),
 *               A0=1 是数据口(收 ICW2~4 / OCW1)。
 */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

/* OCW2: 非特定 EOI —— "当前正在服务的最高优先级中断结束了" */
#define PIC_EOI    0x20

/*
 * 四个 ICW 的数值。逐位含义写在 pic.c 的 pic_init() 上方,
 * 这里只留"一句话索引", 方便对照。
 */
#define PIC_ICW1_INIT    0x11   /* 0001_0001: IC4=1, SNGL=0(级联), LTIM=0(边沿) */
#define PIC_ICW2_MASTER  0x20   /* IRQ0~7  → 向量 0x20~0x27 */
#define PIC_ICW2_SLAVE   0x28   /* IRQ8~15 → 向量 0x28~0x2F */
#define PIC_ICW3_MASTER  0x04   /* 主片: IRQ2 上挂从片(bit2=1) */
#define PIC_ICW3_SLAVE   0x02   /* 从片: 我挂在主片的 IRQ2 上 */
#define PIC_ICW4_8086    0x01   /* uPM=1(x86), AEOI=0(手动 EOI), BUF=0(非缓冲) */

#define PIC_MASK_ALL     0xFF

void pic_init(void);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_send_eoi(uint8_t irq);

#endif /* PIC_H */
