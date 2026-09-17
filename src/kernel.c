#include "vga.h"

void kmain(void) {
    vga_clear();
    vga_printf("A=%c S=%s P=%%\n", 'x', "str");
    vga_printf("d=%d u=%u x=%x X=%016lx p=%p w=%010lu\n",
               -42, 42u, 0xdeadbeef, 0x1234ul, (void *)0xb8000, 7ul);
    vga_puts("Hello, kernel!\n");
    vga_puts("This is 8GodOS\n");
    vga_puts("M5: VGA terminal\n");
    for (;;) { __asm__ volatile("hlt"); }
}
