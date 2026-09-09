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
    cmp eax,0x36D76289
    jne .error
pushfd
pop eax
mov ecx, eax
xor eax, 0x200000
push eax
popfd 
pushfd
pop eax
xor eax,ecx
jz .error

mov eax, 0x80000000
cpuid
cmp eax, 0x80000001
jb .error
mov eax,0x80000001
cpuid 
test edx,0x20000000
jz .error

mov edi, pml4
xor eax, eax
mov ecx, 3072
rep stosd

mov eax,pdpt
or eax, 0x003
mov dword [pml4], eax

mov eax, pd
or eax, 0x003
mov dword [pdpt], eax

 mov eax, 0x83
      mov dword [pd], eax

      mov eax, pml4
      mov cr3, eax
      mov eax, cr4
or eax, 0x20
mov cr4, eax

mov ecx, 0xC0000080
rdmsr
or eax, 0x100
wrmsr

mov eax, cr0
or eax, 0x80000000
mov cr0, eax

lgdt [gdt64_ptr]

jmp 0x08:_start64

.error:
   hlt
   jmp .error
bits 64
_start64:
   mov rsp,stack_top

   mov ax, 0x10
   mov ds, ax
   mov es, ax
   mov fs, ax
   mov gs, ax
   mov ss, ax

   extern kmain 
   call kmain
.halt64:
      hlt
      jmp .halt64
section .rodata          ; 第 3 段：只读数据
  gdt64:
      dq 0
      dq 0x00AF9A000000FFFF
      dq 0x00AF92000000FFFF
  gdt64_end:
  gdt64_ptr:
      dw gdt64_end - gdt64 - 1
      dd gdt64

section .bss
align 4096
pml4:  resb 4096
pdpt:  resb 4096
pd:    resb 4096
stack_bottom:
      resb 0x4000      ; 16KB 栈空间
stack_top:

