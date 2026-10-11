; 来源声明 —— 详见 docs/AI-使用记录.md
;   队员编写: M1/M2 的引导流程(multiboot2 头 / GDT / 长模式切换 / 页表)。
;   AI 生成: M7 (2026-10-11) 把引导期结构(pml4/pdpt/pd/内核栈)从 .bss
;            迁入 .bootstrap 段, 并在 _start64 里清真正的 .bss。
;   队员验证: nm 核对 __bss_start 边界; QEMU 启动输出与 M6 一致。
;
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
extern __bss_start      ; 由 linker.ld 定义(是符号, 不是变量)
extern __bss_end

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

   ; M7: 清真正的 .bss(idt[256]、将来的键盘环形缓冲等)。
   ; 边界由 linker.ld 夹住 —— 页表与内核栈住在 .bootstrap 段, 不在此范围内。
   ; 此刻分页已开: 万一边界夹到 pd, 第一轮就把 2MB 大页条目写 0,
   ; 下一条取指即 #PF(而 IDT 还没装) → triple fault 黑屏重启。
   mov rdi, __bss_start
   mov rcx, __bss_end
   sub rcx, rdi            ; 待清字节数
   xor eax, eax            ; al = 0
   rep stosb

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

section .bootstrap nobits write
align 4096
pml4:  resb 4096
pdpt:  resb 4096
pd:    resb 4096
stack_bottom:
      resb 0x4000      ; 16KB 栈空间
stack_top:

