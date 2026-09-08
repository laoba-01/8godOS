section .multiboot2
align 8

multiboot2_header:
    dd 0xE85250D6
    dd 0
    dd multiboot2_header_end-multiboot2_header
    dd -(0xE85250D6+0+(multiboot2_header_end-multiboot2_header))
    dw 0
    dw 0
    dd 8
multiboot2_header_end:

section .text
bits 32
global _start

_start:
    cli
.halt:
      hlt
      jmp .halt