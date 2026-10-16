/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 本文件**当前内容**是占位实现(2026-10-12, M7 Task 2)。
 *   ⚠ 本文件属 🔴 档 —— 分发表 + 注册 + 分发由队员从零重写(Task 3)。
 *     此处只为了让 Task 2 能独立链接、独立验收, 不是最终实现。
 *     Task 3 重写后, 本文件的来源声明要一并改写。
 *
 * 占位实现故意做的事: 把进来的向量号打到屏幕上 —— 这是 Task 2 的验收手段
 * (软件 `int $0x21` 打到它, 证明"门 → stub → common → isr_handler → 分发"整条通)。
 * 注意: 正式实现里**中断上下文不许碰 vga_***, 占位版为了取证暂时违反这条规矩。
 */

#include "irq.h"
#include "vga.h"

void irq_dispatch(regs_t *r) {
    vga_printf("IRQ vector=0x%02lX (占位: 分发表未实现)\n", r->vector);
}
