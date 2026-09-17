; 异常 stub: 把 (向量号, 错误码) 压成规整栈帧, 再跳统一的 common stub 保存现场。
;
; 错误码分两类 —— CPU 只对一部分向量压错误码:
;   压错误码的 10 个: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
;   不压的 22 个:     其余
; 不压的那些必须由 stub 先补一个 dummy 0, 否则 common 看到的栈帧整体错位,
; 症状是"有些异常正常、有些异常 RIP 完全不对", 极难查。

bits 64

extern isr_handler
global isr_stub_table

section .rodata
isr_stub_table:
%assign i 0
%rep 32
    dq isr_stub_ %+ i
%assign i i+1
%endrep

section .text

; 不压错误码的向量: 先补 dummy 0, 再压向量号
%macro ISR_NOERR 1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

; 压错误码的向量: CPU 已经把错误码压好了, 这里只补充向量号
%macro ISR_ERR 1
isr_stub_%1:
    push qword %1
    jmp isr_common
%endmacro

; 32 个异常向量。哪几个带错误码是 CPU 的硬性约定, 不能按"看起来该不该有"猜。
ISR_NOERR 0     ; #DE Divide Error
ISR_NOERR 1     ; #DB Debug
ISR_NOERR 2     ; NMI Interrupt
ISR_NOERR 3     ; #BP Breakpoint
ISR_NOERR 4     ; #OF Overflow
ISR_NOERR 5     ; #BR BOUND Range Exceeded
ISR_NOERR 6     ; #UD Invalid Opcode
ISR_NOERR 7     ; #NM Device Not Available
ISR_ERR   8     ; #DF Double Fault
ISR_NOERR 9     ; Coprocessor Segment Overrun
ISR_ERR   10    ; #TS Invalid TSS
ISR_ERR   11    ; #NP Segment Not Present
ISR_ERR   12    ; #SS Stack-Segment Fault
ISR_ERR   13    ; #GP General Protection
ISR_ERR   14    ; #PF Page Fault
ISR_NOERR 15    ; Reserved
ISR_NOERR 16    ; #MF x87 FPU Error
ISR_ERR   17    ; #AC Alignment Check
ISR_NOERR 18    ; #MC Machine Check
ISR_NOERR 19    ; #XM SIMD FP Exception
ISR_NOERR 20    ; #VE Virtualization Exception
ISR_ERR   21    ; #CP Control Protection Exception
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29    ; #VC VMM Communication
ISR_ERR   30    ; #SX Security Exception
ISR_NOERR 31    ; Reserved

isr_common:
    ; x86_64 没有 pusha/popa, 15 个通用寄存器手动压。
    ; 顺序必须与 regs_t 从低地址到高地址一致: regs_t 开头是 r15、结尾是 rax,
    ; 而 push 是往低地址走 —— 所以先压 rax, 最后压 r15,
    ; r15 落在最低地址, 正好是 regs_t 的 offset 0。
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; 此刻栈上共 20 qword = 160 字节(偶数):
    ;   15 个通用寄存器 + 向量号 + 错误码/0 = 17, 加上 CPU 压的 rip/cs/rflags = 20。
    ; 160 是 16 的整数倍, 所以进来时的 16 字节对齐被原样保持;
    ; 下面 call 再压 8 字节返回地址, 函数入口 RSP % 16 == 8, 正是 ABI 期望的状态。
    ; 以后往 common 里加/减寄存器要重新数 —— 压栈总数变成奇数就得补一次对齐。

    cld                 ; 进入 handler 时 DF 的值没有保证
    mov rdi, rsp        ; 栈顶就是 regs_t
    call isr_handler

    ; ---- 恢复路径。M6 里 32 个向量全部 panic, 走不到这里;
    ;      完整写出来, M7 加 PIC 时只差"加一张函数指针分发表"这一步。 ----
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16         ; 弹掉错误码和向量号
    iretq               ; 是 iretq, 不是 iret

section .note.GNU-stack noalloc noexec nowrite progbits
