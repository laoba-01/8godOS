/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   队员编写: M1/M2 的初始版本(0x100000 起、各输出段)。
 *   AI 生成: M7 (2026-10-11) 新增 .bootstrap 输出段, 并给 .bss 加
 *            __bss_start / __bss_end 夹子。
 *   队员验证: nm 核对三条边界; QEMU 启动输出与 M6 一致(页表未被误清)。
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS
{
   . = 0x100000;
      .multiboot2 : {
          *(.multiboot2)
      }
     .text : {
        *(.text)
     } 
     .rodata : {
        *(.rodata*)
     }
     .data : {
      *(.data)
     }
     .bootstrap : {
        *(.bootstrap)
     }
     .bss : {
        __bss_start = .;
        *(.bss)
        __bss_end = .;
     }
}