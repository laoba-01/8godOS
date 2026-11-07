/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: M8 Task 6 (2026-11-07) 内核堆接口与块布局。
 */
#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

/* 初始化 16MB 内核堆; 第一次写会触发按需缺页。0 成功。 */
int heap_init(void);

/* 按字节分配, 16 字节对齐。失败或 size==0 返回 NULL。 */
void *kmalloc(size_t size);

/* 释放; NULL 是空操作。double free / 坏指针会报错停机。 */
void kfree(void *ptr);

/* 当前空闲字节数(诊断用)。 */
size_t heap_free_bytes(void);

/* 自检: 对齐/重叠/图案/合并/OOM/跨页缺页。0 通过, -1 失败。 */
int heap_self_test(void);

#endif /* HEAP_H */
