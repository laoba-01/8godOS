#include "vga.h"
#include "io.h"
#include <stdarg.h>

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

/*
 * 把无符号数按 base 进制吐出去, 不足 width 位时在高位补 pad 字符。
 * 先把数字从低位到高位收进 digits, 再补 pad, 最后从高位往低位吐。
 */
static void vga_put_uint(uint64_t value, unsigned base, int width, char pad) {
    char digits[24];   /* 10 进制下 uint64 最长 20 位, 24 够 */
    int n = 0;

    if (value == 0) {
        digits[n++] = '0';
    } else {
        while (value > 0) {
            unsigned d = (unsigned)(value % base);
            digits[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
            value /= base;
        }
    }

    for (int i = n; i < width; i++) {
        vga_putchar(pad);
    }
    while (n > 0) {
        vga_putchar(digits[--n]);
    }
}

/*
 * 最小格式化输出。支持:
 *   转换:     %c %s %d %u %x %p %%
 *   长度修饰: l / ll / z —— 三者都按 64 位取值
 *             (x86_64 上 long / long long / size_t 都是 64 位)
 *   标志:     只支持 '0'(零填充), 只配十进制宽度
 * 解析顺序: % → 可选 '0' → 可选十进制宽度 → 可选长度修饰符 → 转换字符
 *
 * 两处刻意不同于 glibc, 为的是行为确定、可比对:
 *   - %p 固定打 0x + 16 位零填充(glibc 打 (nil))
 *   - 宽度只计数字位, 负号不算在宽度内
 * 不做左对齐 / 精度 / 浮点。
 *
 * -nostdlib 下没有 vsnprintf 可用, 只能自己 va_arg 逐字符吐到 vga_putchar。
 */
void vga_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            vga_putchar(*p);
            continue;
        }

        /* 跳过 '%'; 此后 p 指向标志位 */
        p++;

        int zero_pad = 0;
        if (*p == '0') {
            zero_pad = 1;
            p++;
        }

        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        int is64 = 0;
        if (*p == 'l' || *p == 'z') {
            is64 = 1;
            p++;
            if (*p == 'l') {   /* ll */
                p++;
            }
        }

        char pad = zero_pad ? '0' : ' ';

        switch (*p) {
            case 'c':
                /* char 经默认实参提升成了 int, 必须按 int 取 */
                vga_putchar((char)va_arg(ap, int));
                break;

            case 's':
                vga_puts(va_arg(ap, const char *));
                break;

            case '%':
                /* 字面 '%': 不消耗变参。这一路取错, 后面全错位 */
                vga_putchar('%');
                break;

            case 'd': {
                int64_t sv = is64 ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
                if (sv < 0) {
                    vga_putchar('-');
                    vga_put_uint((uint64_t)(~sv) + 1, 10, width, pad);  /* 取反加一, INT64_MIN 也不溢出 */
                } else {
                    vga_put_uint((uint64_t)sv, 10, width, pad);
                }
                break;
            }

            case 'u': {
                uint64_t uv = is64 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                vga_put_uint(uv, 10, width, pad);
                break;
            }

            case 'x': {
                uint64_t xv = is64 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                vga_put_uint(xv, 16, width, pad);
                break;
            }

            case 'p':
                vga_puts("0x");
                vga_put_uint((uint64_t)va_arg(ap, void *), 16, 16, '0');
                break;

            default:
                /* 未实现的转换符: 静默跳过, 不吐乱码 */
                break;
        }

        if (*p == '\0') {
            break;   /* 格式串以 '%' 收尾: 别让 for 的 p++ 越过 '\0' */
        }
    }

    va_end(ap);
}
