/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 1 (2026-11-01) 整写。束分配器接口在 Task 2 补。
 */
#ifndef PMM_H
#define PMM_H

#include <stdint.h>

/*
 * M8 Task 1: 解析 multiboot2 信息结构并打印可用内存。
 *   mb_info  multiboot2 信息结构的物理地址(kmain 的第一个参数)
 * 返回 0 成功, -1 失败(调用者负责停机)。
 *
 * Task 2 起本函数还会初始化位图帧分配器;Task 3 起 vm_init() 必须在它之前调用。
 */
int pmm_init(uint64_t mb_info);

#endif /* PMM_H */
