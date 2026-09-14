#include "vga.h"

  /* VGA 文本缓冲与终端状态 */
  static volatile uint16_t *vga_buffer = (volatile uint16_t *)0xB8000;
  static const int VGA_WIDTH  = 80;
  static const int VGA_HEIGHT = 25;
  static int vga_row = 0;           // 光标行
  static int vga_col = 0;           // 光标列
  static uint8_t vga_color = 0x07;  // 默认灰字黑底
 /* 拼一个字符 + 颜色成一个 16 位缓冲项 */
  static inline uint16_t vga_entry(char c, uint8_t color) {
      return (uint16_t)c | (uint16_t)color << 8;
  }

  void vga_clear(void) {
      for(int i=0;i<80*25;i++)
      {
      vga_buffer[i]=vga_entry(' ',vga_color);
        vga_row=0;
        vga_col=0;
      }
    /* TODO */
  }

  void vga_set_color(uint8_t attr) {
    vga_color = attr;
      /* TODO */
  }

  void vga_putchar(char c) {
      /* TODO */
      switch (c)
      {
        case '\n': vga_col=0; vga_row++; break;
        case '\r': vga_col=0;  break; 
        case '\t': vga_col=(vga_col+8)&~7; break;
        case '\b': if(vga_col>0) vga_col--; break;
         default:    // 可打印字符
        vga_buffer[vga_row * 80 + vga_col] = vga_entry(c, vga_color);
        vga_col++;
        if (vga_col == 80) { vga_col = 0; vga_row++; }      // 到行尾自动换行
      }
  }

  void vga_puts(const char *s) {
    while(*s)
    {
     vga_putchar(*s++);
    }  
    /* TODO */
  }

  void vga_printf(const char *fmt, ...) {
      /* TODO —— 第 6 步再实现 */
  }