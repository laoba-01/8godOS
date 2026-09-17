# M6 — IDT 与 CPU 异常处理 实施计划

> **For agentic workers:** 本计划的执行者是**项目作者本人**（项目约定：内核代码由作者亲手编写，AI 只做设计/审阅/验证标准）。每个任务末尾是验收动作与期望输出，不是"跑测试套件"。步骤用 `- [ ]` 跟踪。

**Goal:** 让内核第一次能"看见"CPU 异常——建 IDT、把 32 个异常经统一的汇编 stub 交给 C 侧表驱动分发，命中时 panic 打印向量号/名称/错误码/CR2 而不是黑屏重启。

**Architecture:** NASM 宏生成 32 个异常 stub，每个先把 `(vector, error_code)` 压成规整栈帧，再跳统一的 common stub 保存 15 个通用寄存器，最后把 `rsp` 当 `regs_t *` 传给 C 的 `isr_handler()`。C 侧只有两张表（`isr_stub_table` 填门、`exception_names` 取名字），32 个向量全部走 panic，暂时没有函数指针分发表。

**Tech Stack:** NASM 2.16 / GCC 13.3（`-ffreestanding -nostdlib -mno-sse -mno-red-zone`）/ ld / QEMU 8.2.2 / GRUB multiboot2 / WSL2 Ubuntu 24.04

**Spec:** `计划书.md` §M6（`### M6 — IDT 与 CPU 异常处理`）

## Global Constraints

- 所有构建与运行**在 WSL 里**执行（`cd /mnt/d/8GodOS`）。Windows 侧只编辑文件。
- `make` 必须**零警告**通过（`-Wall -Wextra`）。
- **全程 `cli`，不碰 PIC，不执行 `sti`。** CPU 异常是同步的，不受 `IF` 影响。
- **不依赖 `.bss` 为 0**（`_start64` 没有清 BSS 的代码）。
- `regs_t` 字段顺序与 common stub 的压栈顺序**严格一致**，用 `_Static_assert` 守。
- 两种异常路径的压栈总量都是 **20 qword**（偶数）——16 字节对齐靠这个性质保持，common stub 里要写注释记下来。
- panic 输出是**固定格式文本**，落在显存里；取证一律 `memsave 0xb8000 4000` + `python3 tools/decode_vga.py`，**不新增取证脚本**。
- **一次运行只验一条用例**：异常处理器返回不到故障指令之后（`iretq` 会重新执行那条出错指令）。

---

## File Structure

| 文件 | 动作 | 职责 |
|------|------|------|
| `src/vga.c` | 改 | 实现 `vga_printf`（第 0 步） |
| `src/kernel.c` | 改 | 自测调用、`idt_init()` 调用、异常触发开关 |
| `src/isr.asm` | 新增 | 宏生成 32 个 stub + common stub + `isr_stub_table` |
| `src/idt.h` | 新增 | `regs_t` 栈帧结构、`idt_entry_t` 门描述符、`idt_init()` 声明 |
| `src/idt.c` | 新增 | IDT 表、`idt_init()`、`exception_names[]`、`isr_handler()`、panic |
| `Makefile` | 改 | `isr.o` 的 nasm 规则与链接项 |

---

## Task 1: `vga_printf` 骨架与字符串类转换

**Files:**
- Modify: `src/vga.c`（`vga_printf` 在 `vga.c:106`，当前是空壳）
- Modify: `src/kernel.c`（加自测调用）

**Interfaces:**
- Consumes: `vga_putchar(char)`（`vga.c:81`）、`vga_puts(const char*)`（`vga.c:100`）
- Produces: `void vga_printf(const char *fmt, ...)`（已在 `vga.h:33` 声明，不改头文件）

- [x] **Step 1: 先写"会失败"的验收**

在 `kmain` 里 `vga_clear()` 之后加一行，只覆盖字符串类转换：

```c
vga_printf("A=%c S=%s P=%%\n", 'x', "str");
```

- [x] **Step 2: 确认它现在确实失败**

```bash
make && make iso
{ sleep 8; echo 'memsave 0xb8000 4000 build/m6.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m6.bin
```

Expected: 第 0 行是空的或只有 `Hello, kernel!` 系列原内容 —— 自测那行**打不出来**（`vga_printf` 是空壳）。这就是本任务要让它变绿的那个红。

- [x] **Step 3: 实现骨架**

在 `src/vga.c` 里实现。要点：

- 引入 `<stdarg.h>` 用 `va_start` / `va_arg` / `va_end`。**`stdarg.h` 是编译器提供的头，`-ffreestanding` 下可用**，不违反"不用 libc"。
- **不能调 `vsnprintf`** —— `-nostdlib` 下没有 libc，必须自己逐字符吐到 `vga_putchar`。
- `%c` 取参用 `va_arg(ap, int)`，**不是 `char`**：默认实参提升把 `char` 提升成了 `int`，写 `char` 在 x86_64 上会取错字节。
- `%%` 输出一个字面 `%`。

- [x] **Step 4: 验收**

重跑 Step 2 的两条命令。Expected: 解码结果第 0 行是 `A=x S=str P=%`。

- [x] **Step 5: Commit**

```bash
git add src/vga.c src/kernel.c
git commit -m "M6: vga_printf 骨架与字符串类转换 (%c %s %%)"
```

---

## Task 2: 数字转换与格式（第 0 步闸门）

**Files:**
- Modify: `src/vga.c`
- Modify: `src/kernel.c`（扩自测样本）

**Interfaces:**
- Produces: `vga_printf` 的完整能力集 —— 转换 `%c %s %d %u %x %p %%`；长度修饰符 `l` / `ll` / `z`（三者统一按 64 位取值）；标志只支持 `0`（零填充）+ 十进制宽度；不做左对齐、精度、浮点。

- [x] **Step 1: 扩自测样本，覆盖全部类型与零填充**

```c
vga_printf("d=%d u=%u x=%x X=%016lx p=%p w=%010lu\n",
           -42, 42u, 0xdeadbeef, 0x1234ul, (void *)0xb8000, 7ul);
```

- [x] **Step 2: 确认失败**

按 Task 1 Step 2 的命令重跑。Expected: 字符串部分对了，数字部分打不出来或全是乱值。

- [x] **Step 3: 实现数字转换与格式解析**

要点：

- `%d`：负数先输出 `-`，再对 `unsigned` 取反加一得到数值，之后走无条件十进制。
- `%u` 十进制、`%x` 小写十六进制。
- `%p`：固定输出 `0x` + **16 位零填充**十六进制。注意这**故意不同于 glibc**（glibc 打 `(nil)`），行为要确定、可比对。
- 长度修饰符 `l` / `ll` / `z` 都按 64 位取值（x86_64 上 `long`、`long long`、`size_t` 都是 64 位）。**`%x` 与 `%lx` 取值宽度不同，必须区分**：传 `uint64_t` 却写 `%x` 会打错。
- 格式解析顺序：`%` → 可选的 `0` 标志 → 可选的十进制宽度 → 可选的长度修饰符 → 转换字符。

- [x] **Step 4: 闸门验收**

重跑解码，逐字比对 Step 1 那一行，期望：

```
d=-42 u=42 x=deadbeef X=0000000000001234 p=0x00000000000b8000 w=0000000007
```

**这一条不过，不进 Task 3。** panic 输出打歪了，你分不清是 printf 的问题还是栈帧的问题。

- [x] **Step 5: Commit**

```bash
git add src/vga.c src/kernel.c
git commit -m "M6: vga_printf 数字转换与零填充格式 (第 0 步闸门通过)"
```

---

## Task 3: 单向量打通（#DE）

用**一个手工 stub** 把整条链路走通：门构造 → `lidt` → 除零 → stub → common → C。这一步不做宏，不做 32 向量——只证明机制成立。

**Files:**
- Create: `src/isr.asm`, `src/idt.h`, `src/idt.c`
- Modify: `src/kernel.c`, `Makefile`

**Interfaces:**

`src/idt.h` 必须定义（字段顺序即内存顺序，从低地址往高）：

```c
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags;
} regs_t;

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

void idt_init(void);
```

- `src/isr.asm` 导出 `isr_stub_table`（32 个函数指针），导入 `isr_handler`
- `src/idt.c` 定义 `void isr_handler(regs_t *r)`（**非 static**，供汇编 `call`）
- 门参数：`selector = 0x08`、`type_attr = 0x8E`（P=1, DPL=0, 64 位中断门）、`ist = 0`

- [x] **Step 1: Makefile 加 nasm 规则与链接项**

加 `build/isr.o` 目标（`$(AS) $(ASFLAGS) src/isr.asm -o $@`），并把它加进 `$(KERNEL)` 的链接行。`src/isr.asm` 要写进依赖里。

- [x] **Step 2: 写 `idt.h`，并用 `_Static_assert` 锁死栈帧契约**

除了上面的结构体，加上：

```c
_Static_assert(offsetof(regs_t, vector)     == 15 * 8, "regs_t 与 common 压栈顺序不一致");
_Static_assert(offsetof(regs_t, error_code) == 16 * 8, "同上");
_Static_assert(offsetof(regs_t, rip)        == 17 * 8, "同上");
_Static_assert(offsetof(regs_t, cs)         == 18 * 8, "同上");
_Static_assert(offsetof(regs_t, rflags)     == 19 * 8, "同上");
_Static_assert(sizeof(regs_t)               == 160,    "栈帧大小应为 20 qword");
```

需要 `#include <stddef.h>` 取 `offsetof`。**这是本任务最有价值的一步**：以后谁改了字段顺序、或在 common 里多压一个寄存器，编译期就炸，不用等到某个异常打出一串莫名其妙的 RIP。

- [x] **Step 3: 写 `isr.asm` 的最小版**

- 文件开头 `bits 64`；stub 地址表放 `section .rodata`；`extern isr_handler`、`global isr_stub_table`。
- 先只做一个真的 stub（向量 0，`#DE`）：**先压一个 dummy `0`**（`#DE` 属于"CPU 不压错误码"那类），再压向量号 `0`，然后 `jmp` 到 common。
- 表里 32 项**暂时全部指向这一个 stub**（占位）。
- common stub 按顺序：压 15 个通用寄存器（顺序必须与 `regs_t` 从低到高一致：先压 `rax`，最后压 `r15`）→ **`cld`** → `mov rdi, rsp` → `call isr_handler` → 恢复段寄存器与 15 个通用寄存器 → `add rsp, 16`（弹掉向量号和错误码）→ `iretq`。
- 在 common stub 里写一行注释：**总压栈 20 qword（偶数），16 字节对齐靠这个性质保持；加寄存器要重新数。**
- 坑：**x86_64 没有 `pusha`/`popa`**，15 个寄存器手动压。**返回用 `iretq` 不是 `iret`。**

- [x] **Step 4: 写 `idt.c` 最小版**

- `static idt_entry_t idt[256] __attribute__((aligned(16)));` 放 `.bss`。
- `idt_init()`：先把 256 项**全部门清 0**，再循环 32 次按 `isr_stub_table[i]` 填门（本步表里 32 项指向同一个 stub，没关系）；最后 `lidt` 一个 `{limit = sizeof(idt) - 1, base = &idt[0]}` 的结构（`lidt` 可以用内联汇编，不必写进 `isr.asm`）。注意 **limit 是字节数减一：`256 * 16 - 1 = 4095 = 0x0FFF`**。
- `isr_handler(regs_t *r)` 最小版：只打一行 `EXCEPTION 0x00\n`，然后 `cli; hlt` 死循环。暂时不读 `r` 的其它字段。

- [x] **Step 5: `kernel.c` 接线**

`vga_clear()` 之后、自测行之前调 `idt_init()`；自测那一行先注释掉或保留，末尾加除零触发：

```c
volatile int a = 1, b = 0;
volatile int c = a / b;
(void)c;
```

**两个变量都要 `volatile`**，否则 `-O2` 会在编译期把这次除法优化掉，你会得到"什么都没发生"。

- [x] **Step 6: 验收 A —— IDT 真的装上了**

```bash
make && make iso
nm build/kernel.elf | grep -i ' idt'
```

拿到 `idt` 符号的地址，然后：

```bash
{ sleep 8; echo 'info registers'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
```

Expected: 输出里 `IDT=` 那一行的 base **等于 `nm` 查到的地址**，limit 为 `00000fff`。

**这一步比"看着没崩"强得多** —— 它直接证明 `lidt` 装的是你那张表，而不是 BIOS 留下的旧表。

- [x] **Step 7: 验收 B —— 全链路打通**

```bash
{ sleep 8; echo 'memsave 0xb8000 4000 build/m6.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m6.bin
```

Expected: 屏幕上出现 `EXCEPTION 0x00`。

**若这里是 triple fault 重启（`-no-reboot` 下 QEMU 直接退出）**，按顺序查：`_Static_assert` 过了没有 → 门描述符的 `offset` 三段拼对了没有 → common 里压栈顺序和 `regs_t` 对不对 → `iretq` 是否写成了 `iret`。

- [x] **Step 8: Commit**

```bash
git add src/isr.asm src/idt.c src/idt.h src/kernel.c Makefile
git commit -m "M6: IDT 装载 + 单向量 (#DE) 全链路打通"
```

---

## Task 4: 32 个 stub、错误码两类、异常名表

**Files:**
- Modify: `src/isr.asm`, `src/idt.c`, `src/kernel.c`

**Interfaces:**
- Produces: `isr_stub_table[32]` 全量；`static const char *const exception_names[32]`
- 有错误码的向量（CPU 自己压）：**`8, 10, 11, 12, 13, 14, 17, 21, 29, 30`**；其余 22 个不压

- [x] **Step 1: 把单 stub 换成宏生成的 32 个**

用两个 NASM 宏分工，例如 `ISR_NOERR n`（先压 dummy `0`，再压 `n`）和 `ISR_ERR n`（只压 `n`，错误码由 CPU 已经压好），然后对 0~31 逐个实例化；或者用 `%assign` + `%rep` 配合条件判断。

表也用宏生成：

```nasm
isr_stub_table:
%assign i 0
%rep 32
    dq isr_stub_ %+ i
%assign i i+1
%endrep
```

**两条路径的分发写反，是本任务唯一但最致命的坑**：症状是"有些异常正常、有些异常 RIP 完全不对"。Step 4 的 `#GP` 验收就是专门抓它的。

- [x] **Step 2: `idt.c` 循环填 32 个门 + 建异常名表**

按 Intel 手册 Vol.3 表 6-1 填 `exception_names`（至少保证下表这几条准确，其余照抄手册）：

| 向量 | 助记符 | 名称 |
|------|--------|------|
| `0x00` | `#DE` | Divide Error |
| `0x01` | `#DB` | Debug |
| `0x02` | — | NMI Interrupt |
| `0x03` | `#BP` | Breakpoint |
| `0x04` | `#OF` | Overflow |
| `0x05` | `#BR` | BOUND Range Exceeded |
| `0x06` | `#UD` | Invalid Opcode |
| `0x07` | `#NM` | Device Not Available |
| `0x08` | `#DF` | Double Fault |
| `0x09` | — | Coprocessor Segment Overrun |
| `0x0A` | `#TS` | Invalid TSS |
| `0x0B` | `#NP` | Segment Not Present |
| `0x0C` | `#SS` | Stack-Segment Fault |
| `0x0D` | `#GP` | General Protection |
| `0x0E` | `#PF` | Page Fault |
| `0x0F` | — | Reserved |
| `0x10` | `#MF` | x87 FPU Error |
| `0x11` | `#AC` | Alignment Check |
| `0x12` | `#MC` | Machine Check |
| `0x13` | `#XM` | SIMD FP Exception |
| `0x14` | `#VE` | Virtualization Exception |
| `0x15` | `#CP` | Control Protection Exception |
| `0x16`~`0x1B` | — | Reserved |
| `0x1C` | `#HV` | Hypervisor Injection |
| `0x1D` | `#VC` | VMM Communication |
| `0x1E` | `#SX` | Security Exception |
| `0x1F` | — | Reserved |

- [x] **Step 3: `kernel.c` 加用例开关**

用一个编译期宏切换四种触发，四次运行每次只改一个数字：

```c
#define M6_CASE 1   /* 1=#DE 除零  2=#UD 非法指令  3=#GP 越界段选择子  4=#PF 读 0x300000 */
```

四种触发方式：

- `#DE`：`volatile int a = 1, b = 0; volatile int c = a / b;`
- `#UD`：`__asm__ volatile("ud2");`
- `#GP`：`__asm__ volatile("mov $0x30, %%ax; mov %%ax, %%ds" ::: "ax");` —— GDT 只有 3 项、limit=23，选择子 `0x30` 越界
- `#PF`：`volatile uint64_t *p = (volatile uint64_t *)0x300000; volatile uint64_t v = *p;` —— 恒等映射只覆盖前 2MB（`boot.asm:55-56` 的 2MB 大页），`PD[1]` 为 0

**每个触发的指针/结果都要 `volatile`**，否则 `-O2` 会把访存整条优化掉。

- [x] **Step 4: 让 panic 打出错误码并验收两条路径**

`isr_handler` 补上：按 `r->vector` 查名，打印向量号、名称、`error=0x%016lx`（`r->error_code`）。

验收（各跑一次，每次都改 `M6_CASE` 重新 `make`）：

| `M6_CASE` | Expected |
|---|---|
| `2`（`#UD`，不压错误码） | `EXCEPTION 0x06 #UD Invalid Opcode`，`error=0x0000000000000000` |
| `3`（`#GP`，CPU 压错误码） | `EXCEPTION 0x0D #GP General Protection`，**`error=0x0000000000000030`** |

**`#GP` 的 `error=0x30` 是整个 M6 最强的判别点**：`0x30` 就是那个越界的段选择子，它证明错误码来自 CPU 而不是 stub 补的 dummy `0`。宏的两类如果分反了，这一条立刻露馅。

- [x] **Step 5: Commit**

```bash
git add src/isr.asm src/idt.c src/kernel.c
git commit -m "M6: 宏生成 32 个 stub + 错误码两类处理 + 异常名表"
```

---

## Task 5: panic 输出完整化 + `32~255` 清零

**Files:**
- Modify: `src/idt.c`

- [x] **Step 1: 补齐 panic 输出**

固定成这个形状（`cr2` 行**只在 `vector == 14` 时**打印）：

```
EXCEPTION 0x0E #PF Page Fault
error=0x0000000000000000
cr2=0x0000000000300000
rip=0x0000000000100234 cs=0x0008 rflags=0x0000000000000092
```

- `rip` / `cs` / `rflags` 直接取 `r->rip` / `r->cs` / `r->rflags`。
- `cr2`：在 `idt.c` 里就地加一个 `static inline uint64_t read_cr2(void)`，用内联汇编 `mov %%cr2, %0`。**错误码只给访问类型（P/W/U 位），出错地址只在 CR2 里**。先不单开 `cpu.h`。
- （可选）要显示"被打断的 rsp"，用 `(uint64_t)&r->rflags + 8` 从栈帧推算——**这是算出来的，不是 CPU 压栈字段**（ring0 下 CPU 不压 `RSP`/`SS`），文案上要写清楚，免得以后被人当真字段用。

- [x] **Step 2: `idt_init()` 加 `32~255` 的清零循环**

`.bss` 不保证为 0（`_start64` 没有清 BSS），只写 32 个门就等于留下 224 个垃圾描述符。这个循环别漏。

- [x] **Step 3: 四条用例全跑，逐字比对**

四次运行、每次改 `M6_CASE`，与 `计划书.md` §M6 验证标准里那张表逐条比对：

| 用例 | Expected |
|---|---|
| `#DE` | `EXCEPTION 0x00 #DE Divide Error`，`error=0x0000000000000000` |
| `#UD` | `EXCEPTION 0x06 #UD Invalid Opcode`，`error=0x0000000000000000` |
| `#GP` | `EXCEPTION 0x0D #GP General Protection`，`error=0x0000000000000030` |
| `#PF` | `EXCEPTION 0x0E #PF Page Fault`，`error` 的 **bit0 = 0**（页不存在），`cr2=0x0000000000300000` |

- [x] **Step 4: Commit**

```bash
git add src/idt.c
git commit -m "M6: panic 输出完整化 (错误码/rip/cs/rflags/cr2) + 未定义向量清零"
```

---

## Task 6: 收尾与文档

**Files:**
- Modify: `计划书.md`, `README.md`
- Verify: 全仓库

- [x] **Step 1: 确认零警告**

```bash
make clean && make 2>&1 | grep -i warning
```

Expected: 只有 M5 遗留的那两条 `ld` 警告，**没有任何 `gcc`/`nasm` 的编译警告**。

- [x] **Step 2: 回填计划书**

- `计划书.md` §M6：状态改成 ✅ 完成，补上**「实施中踩到并解决的关键点」**（照 M5 的写法，记录你实际踩到的坑，不是照抄设计）和**「教训」**。
- 三条遗留项确认在册：`_start64` 未清 `.bss`（补之前要先给 `linker.ld` 加 `__bss_start`/`__bss_end`）、`#DF` 无 IST 保护栈、`ld` 两条警告。
- `README.md` 进度表加 M6 行。

- [x] **Step 3: Commit**

```bash
git add 计划书.md README.md
git commit -m "docs: 里程碑更新到 M6 (IDT 与 CPU 异常处理完成)"
```

---

## Self-Review

**1. Spec coverage** —— 逐条对 `计划书.md` §M6：

| Spec 要求 | 落在哪个任务 |
|---|---|
| 第 0 步 `vga_printf` | Task 1、Task 2 |
| `vga_printf` 最小能力集（转换/长度修饰符/零填充） | Task 2 Step 3 |
| 第 0 步单独验、不过不进 IDT | Task 2 Step 4（闸门） |
| IDT 256 项、`0~31` 填异常 | Task 3 Step 4、Task 4 Step 2 |
| `32~255` 显式清零 | Task 5 Step 2 |
| 宏生成 32 个 stub + common stub | Task 3 Step 3（单向量）、Task 4 Step 1（全量） |
| C 侧两张表 | Task 4 Step 1、Step 2 |
| `_Static_assert` 锁栈帧契约 | Task 3 Step 2 |
| 20 qword 对齐性质写注释 | Task 3 Step 3 |
| 错误码两类 | Task 4 Step 1 |
| `#DE`/`#UD`/`#GP`/`#PF` 四条用例 | Task 4 Step 4、Task 5 Step 3 |
| panic 固定格式 + `cr2` 仅 `vector==14` | Task 5 Step 1 |
| `(uint64_t)&r->rflags + 8` 及文案说明 | Task 5 Step 1 |
| 复用 `decode_vga.py`、不新写脚本 | 每个任务的验收命令 |
| `make` 零警告 | Task 6 Step 1；各任务内隐式 |
| 不碰 PIC / 不 `sti` / 不做 IST / 不做恢复 | Global Constraints + Task 3 Step 4（全程 `cli`） |
| 遗留项三条 | Task 6 Step 2 |

无遗漏。

**2. Placeholder scan** —— 无 "TBD"/"TODO"/"implement later"；每个代码步骤都给了契约、精确符号或可直接执行的验收命令。

**3. Type consistency** —— `regs_t` 字段名（`r15..rax, vector, error_code, rip, cs, rflags`）在 Task 3/4/5 中一致；`isr_stub_table`、`isr_handler`、`idt_init`、`exception_names` 命名前后一致；`read_cr2` 只在 Task 5 引入、无前置引用。

**4. 与项目约定的偏差（需作者确认）** —— 本计划按项目约定**不提供可直接粘贴的完整函数体/ stub 体**，只给接口契约、精确符号与寄存器事实、验收命令与期望输出。这与 writing-plans 技能默认的"每步附完整代码"不同，是刻意为之。
