#include "vga.h"
#include "io.h"

/* VGA 文本缓冲与终端状态 */
static volatile uint16_t *vga_buffer = (volatile uint16_t *)0xB8000;
static const int VGA_WIDTH  = 80;
static const int VGA_HEIGHT = 25;
static int vga_row = 0;           /* 光标行 */
static int vga_col = 0;           /* 光标列 */
static uint8_t vga_color = 0x07;  /* 默认灰字黑底 */

/* 拼一个字符 + 颜色成一个 16 位缓冲项 */
static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | (uint16_t)color << 8;
}

/* 把第 1~24 行整体上移一行,末行清空 */
static void vga_scroll(void) {
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
        vga_buffer[i] = vga_buffer[i + VGA_WIDTH];
    }
    for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        vga_buffer[i] = vga_entry(' ', vga_color);
    }
}

/* 换行:列归零、行下移,到底则整屏上滚 */
static void vga_newline(void) {
    vga_col = 0;
    vga_row++;
    if (vga_row >= VGA_HEIGHT) {
        vga_scroll();
        vga_row = VGA_HEIGHT - 1;
    }
}

/*
 * 硬件光标不在显存里,而在 CRTC 的寄存器里,靠一对端口间接访问:
 * 先往索引口 0x3D4 写寄存器编号,再从数据口 0x3D5 读写该寄存器的内容。
 * 索引口是有状态的 —— 每写一次数据口之前都要重新选一次寄存器。
 * 用到的编号:
 *   0x0A  光标起始扫描线(低 5 位)+ 关闭位(bit5)
 *   0x0E  光标位置高 8 位
 *   0x0F  光标位置低 8 位
 */

/* 打开硬件光标,并设成标准下划线(扫描线 14~15) */
static void vga_enable_cursor(void) {
    outb(0x3D4, 0x0A);
    uint8_t start = inb(0x3D5);
    /* 保留高 2 位,其余重写为 0x0E:bit5=0(打开光标)+ 起始扫描线 14 */
    outb(0x3D5, (uint8_t)((start & 0xC0) | 0x0E));
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);   /* 结束扫描线 15 */
}

/* 让硬件光标跟到 vga_row / vga_col 指的位置 */
static void vga_update_cursor(void) {
    uint16_t pos = (uint16_t)(vga_row * VGA_WIDTH + vga_col);

    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
}

void vga_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = vga_entry(' ', vga_color);
    }
    vga_row = 0;
    vga_col = 0;
    vga_enable_cursor();
    vga_update_cursor();
}

void vga_set_color(uint8_t attr) {
    vga_color = attr;
}

void vga_putchar(char c) {
    switch (c) {
        case '\n': vga_newline(); break;
        case '\r': vga_col = 0; break;
        case '\t':
            vga_col = (vga_col + 8) & ~7;
            if (vga_col >= VGA_WIDTH) vga_newline();
            break;
        case '\b': if (vga_col > 0) vga_col--; break;
        default:
            vga_buffer[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_color);
            vga_col++;
            if (vga_col >= VGA_WIDTH) vga_newline();
            break;
    }
    /* 放在 switch 外面:此时 vga_col 一定已回到合法范围 */
    vga_update_cursor();
}

void vga_puts(const char *s) {
    while (*s) {
        vga_putchar(*s++);
    }
}

void vga_printf(const char *fmt, ...) {
    /* TODO —— 第 6 步再实现 */
    (void)fmt;
}
