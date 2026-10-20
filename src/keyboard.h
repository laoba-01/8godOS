/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 本文件全部内容(2026-10-20, M7 Task 5)。原档位 ⚪(原型与常量)。
 *   队员验证: Task 5 端到端验收(hi / Shift / 退格 / tab / 回车)。
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "idt.h"

/*
 * 环形缓冲容量。
 * **必须是 2 的幂** —— 索引回绕用 `& (KB_BUF_SIZE - 1)`, 比 `%` 便宜;
 * 代价是容量不能随便取。keyboard.c 里有 _Static_assert 在编译期兜底。
 */
#define KB_BUF_SIZE 64

/* 注册 IRQ1 处理函数。只在初始化阶段、sti 之前调用。 */
void keyboard_init(void);

/* 中断上下文: 读 0x60 → 翻译 → 入缓冲。由 irq_dispatch 调用。 */
void keyboard_handler(regs_t *r);

/* 主循环调用: 取一个字符; 缓冲空时返回 -1(不是 0 —— 0 是合法字符 '\0')。 */
int kb_get(void);

#endif /* KEYBOARD_H */
