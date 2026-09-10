#include <stdint.h>   /* 可选。freestanding 下 gcc 自带,给你 uint8_t/uint16_t */

 
  static void vga_puts(const char *s) {
    volatile unsigned char *vga = (volatile unsigned char *)0xB8000;
  int pos=0;
  while (*s!=0)
  {
  
  char ch=*s;
  if(ch=='\n')
  {
    pos=((pos/80)+1)*80;
  }  
  else{
     vga[pos*2]     = ch;    // 低字节 = ASCII
      vga[pos*2 + 1] = 0x07;  // 高字节 = 颜色(灰字黑底)
      pos = pos + 1;    
  }
  s++;
  }
  

  }

  void kmain(void) {
      vga_puts("Hello, kernel!");
      for (;;) { __asm__ volatile("hlt"); }
  }
