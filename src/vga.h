/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 本文件由 AI 于 2026-09-14 整写(队员要求「vga.h写吧,头文件何函数都还没声明」)。
 *   队员验证: 接口与调色板常量经队员审阅; M5/M6 各里程碑使用中确认。
 */

#ifndef VGA_H
#define VGA_H

#include <stdint.h>

/* VGA 标准 16 色调色板(低 4 位前景,高 4 位背景) */
enum vga_color {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_YELLOW        = 14,
    VGA_COLOR_WHITE         = 15,
};

/* 前景 fg + 背景 bg 拼成一个颜色属性字节 */
#define VGA_ATTR(fg, bg) ((uint8_t)((bg) << 4 | (fg)))

void vga_clear(void);
void vga_set_color(uint8_t attr);
void vga_putchar(char c);
void vga_puts(const char *s);
void vga_printf(const char *fmt, ...);

#endif /* VGA_H */
