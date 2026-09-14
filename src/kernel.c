#include "vga.h"

void kmain(void) {
    vga_clear();
    vga_puts("Hello, kernel!\n");
    vga_puts("This is 8GodOS\n");
    vga_puts("M5: VGA terminal\n");
    for (;;) { __asm__ volatile("hlt"); }
}
