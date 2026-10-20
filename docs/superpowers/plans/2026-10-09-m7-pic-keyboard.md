# M7 — 外部中断:PIC 与键盘 实施计划

> **For agentic workers:** 本计划的执行者是**项目作者本人**（项目约定：内核代码由作者亲手编写，AI 只做设计/审阅/验证标准）。每个任务末尾是验收动作与期望输出，不是"跑测试套件"。步骤用 `- [ ]` 跟踪。
> **每步标注档位**：🔴 作者写 / 🟡 AI 骨架 + 作者填空 / ⚪ AI 写样板（判据：这段代码是否承载 M7 要教的概念）。

**Goal:** 让内核第一次接上**外部硬件中断**——把 8259A PIC 重映射到 `0x20~0x2F`、IDT 补 `32~47` 门、`isr_handler` 加函数指针分发表（`≥32` 走 IRQ 分发，`<32` 仍 panic），键盘按键经 IRQ1 → 环形缓冲 → 主循环回显到屏幕。

**Architecture:** 分三层，各自只有一件事：`pic.c` 芯片层（ICW 序列 / EOI / 屏蔽位），`irq.c` 分发层（`irq_handlers[16]` + 注册 + 分发 + 兜底 EOI），`keyboard.c` 设备层（scancode 状态机 + SPSC 环形缓冲）。中断路径**完全复用 M6 的 stub/common**，只在 C 侧多一个分支。生产者（中断）与消费者（主循环）通过 `volatile` 环形缓冲解耦，主循环用 `sti; hlt` 等待。

**Tech Stack:** NASM 2.16 / GCC 13.3（`-ffreestanding -nostdlib -mno-sse -mno-red-zone -mcmodel=large`）/ ld / QEMU 8.2.2 / GRUB multiboot2 / WSL2 Ubuntu 24.04

**Spec:** `计划书.md` §M7（`### M7 — 外部中断:PIC 与键盘`）

## Global Constraints

- 所有构建与运行**在 WSL 里**执行（`cd /mnt/d/8GodOS`）。Windows 侧只编辑文件。
- `make` 必须**零警告**通过（`-Wall -Wextra`）；`ld` 仍是 M5 那两条，**不新增**。
- **运行期初始化顺序不可颠倒**：IDT 装门（含 `32~47`）→ PIC 重映射 → 解屏蔽 → `sti`。理由见 Task 4 Step 1。
- **只解屏蔽 IRQ1**（IRQ0 留给后续定时器里程碑）；IMR 期望 `0xFD`。
- 中断处理器（IRQ1）里**只做四件事**：读 `0x60` → 转 ASCII → 入环形缓冲 → 发 EOI。**不在中断里碰 `vga_*`**。
- 错误码两类、栈帧契约、`_Static_assert`、20 qword 对齐性质 —— **M6 建立的规矩原样继承**，本里程碑不新增寄存器、不改 `regs_t` 字段。
- 取证一律 `sendkey` / `memsave 0xb8000 4000` + `python3 tools/decode_vga.py` / `-d int`，**不新增取证脚本**。
- **基线（2026-10-09 实测，`make -B`）**：`gcc`/`nasm` **零警告**；`ld` **两条**警告（`boot.o` 缺 `.note.GNU-stack`；`kernel.elf` 有 RWX LOAD 段）。这两条是 M5 遗留，本里程碑**不修**。

## Review Focus

规格没写、但最可能咬到人的五类输入/失败模式，各自在所属任务里钉一条验收：

1. **按键快于主循环（缓冲满）** —— `kb_put` 必须丢新字符、不越界、不死锁，静默丢即可但**不能崩**。
2. **转换表未覆盖的 scancode**（功能键、数字小键盘、`0xE0` 前缀后的扩展键）—— 必须显式映射为"无字符"，**不能读到表外**。
3. **松键（release）与 `0xE0` 前缀** —— 松键不能产生字符；`0xE0` 后的第二字节不能当独立 make code 处理。
4. **未注册 / 假 IRQ**（`0xE0` 之外，例如 spurious IRQ7）—— `irq_dispatch` 遇到没注册的向量要能安全返回并**照发 EOI**，不能静默卡死。
5. **`sendkey` 的按键集限制** —— 验收脚本只用 QEMU monitor 认得的键名（`h` / `i` / `ret` / `backspace` / `tab` / `shift-h`）；不支持的键会产生"没反应"的**假阴性**。

---

## File Structure

| 文件 | 动作 | 档 | 职责 |
|------|------|----|------|
| `src/linker.ld` | 改 | 🔴 | 引导期结构独立成段（排在 `.bss` 前）+ 导出 `__bss_start`/`__bss_end` |
| `src/boot.asm` | 改 | 🔴 | 引导期结构迁入新段；`_start64` 加 `.bss` 清零循环 |
| `src/isr.asm` | 改 | ⚪ | `32~47` 全走 `ISR_NOERR`；`isr_stub_table` 从 32 扩到 48 |
| `src/idt.h` | 改 | ⚪ | `IDT_IRQ_BASE`(32) / `IDT_IRQ_LAST`(47) 等常量 |
| `src/idt.c` | 改 | 🟡+🔴 | `idt_init` 填 `32~47` 门（🟡 AI）；`isr_handler` 加 `vector >= 32` 分支（🔴 作者） |
| `src/irq.h` | 新增 | ⚪ | `isr_reg` 类型、`irq_register` / `irq_dispatch` 原型 |
| `src/irq.c` | 新增 | 🔴 | 分发表 + 注册 + 分发（Task 2 先给 ⚪ 占位，Task 3 作者重写） |
| `src/pic.h` | 新增 | ⚪ | 端口与 ICW 常量 |
| `src/pic.c` | 新增 | 🟡 | ICW 序列 / EOI / 屏蔽位（AI 骨架 + 注释，作者填 `outb` 的值） |
| `src/keyboard.h` | 新增 | ⚪ | `keyboard_init` 原型、`KB_BUF_SIZE` |
| `src/keyboard.c` | 新增 | 🔴 | scancode 状态机 + 环形缓冲两端（AI 只给两张纯数据查表 ⚪） |
| `src/kernel.c` | 改 | 🟡 | 初始化序列（AI 注释）+ 主循环（作者写） |
| `Makefile` | 改 | ⚪ | `pic.o` / `irq.o` / `keyboard.o` 规则与链接项 |

**分层的理由（要能讲出来）**：`pic.c` 是"读硬件手册"的一层，`irq.c` 是纯软件的一层，`keyboard.c` 是"设备语义"的一层。三者边界清楚，能各自单独讲、单独换（比如将来上 APIC 只换 `pic.c`）。

---

## Task 1: 引导期段分离与 `.bss` 清零

> 🔴 两个文件都由**作者**写。AI 只给边界条件与事实，不给"清哪儿"的答案。

**Files:**
- Modify: `src/linker.ld`
- Modify: `src/boot.asm`（`.bss` 段在 `boot.asm:106-113`；`_start64` 在 `:81`）

**Interfaces:**
- Produces: 链接脚本符号 `__bss_start` / `__bss_end`（**是符号，不是变量**），供 `boot.asm` 清零循环取地址。

**已知事实（照抄，别重新猜）：**
- `.bss` 里现在住着 `pml4` / `pdpt` / `pd`（各 4096 字节）与 `stack_bottom..stack_top`（`0x4000` 字节），见 `boot.asm:106-113`。
- `_start` 在 **32 位、开分页之前**已经用 `rep stosd`（`ecx = 3072` dword = 3 页）清过 `pml4/pdpt/pd`，见 `boot.asm:42-45`。**这段正确且必要，不要动它。**
- 要修的只是：`_start64`（64 位、分页**已开**）里没有清 `.bss` 的代码。

- [ ] **Step 1: 先想清楚"哪些 `.bss` 有资格被清"**

写下一句话，放进 commit message：__bss_start 必须落在**页表与内核栈之后**。

- **页表**：已经在分页开启前归零，此后绝不能碰。
- **内核栈**：你正站在上面，清它无事但无意义。
- **C 侧 `.bss`**（`idt[256]`、将来的 `kb_buf`/`kb_head`/`kb_tail`）：**这才是要清的**。

- [ ] **Step 2: 改 `linker.ld`**

给引导期结构开一个独立的输出段，**排在 `.bss` 之前**；同时导出 `__bss_start` / `__bss_end` 夹住**真正的 `.bss`**。
（具体段名、`*(...)` 的匹配、符号用 `PROVIDE` 还是裸符号 —— 由你决定并写注释说明。）

- [ ] **Step 3: 改 `boot.asm`**

把 `pml4` / `pdpt` / `pd` / `stack_bottom...stack_top` 从 `section .bss` 迁到你新加的段（名字与 Step 2 一致）；在 `_start64` 里、`call kmain` **之前**加清零循环，边界取 `__bss_start` / `__bss_end`。

- [ ] **Step 4: 验收 A —— 符号边界对不对（不启动就能查）**

```bash
make && make iso
nm build/kernel.elf | grep -E '__bss_start|__bss_end| pml4| pd$|stack_top'
```

Expected（**三条都要成立**）：
- `__bss_start` 的地址 **≥ `pd` 的地址 + 0x1000**（页表在它之前）
- `__bss_start` 的地址 **≥ `stack_top` 的地址**（内核栈在它之前）
- `__bss_end` > `__bss_start`

**若 `__bss_start` 落在 `pml4` 或 `pd` 上，停 —— 这就是 Task 1 的陷阱。**

- [ ] **Step 5: 验收 B —— 内核仍能启动（这是"没把页表清零"的硬证据）**

```bash
{ sleep 8; echo 'memsave 0xb8000 4000 build/m7_t1.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m7_t1.bin
```

Expected: 与基线**同样的输出**（`Hello, kernel!` 等既有内容），**不黑屏、不重启**。

**为什么这一条就是证据**：清零循环跑在分页已开之后。若边界夹住了 `pd`，第一轮就把 2MB 大页条目写 0 → 下一条取指 `#PF`（此时 IDT 未装）→ triple fault。**"它还能打印"= 你没把页表清掉。**

> **诚实标注**：GRUB 的 ELF 加载器多半已按规范把 `.bss` 归零，所以本次改动**大概率观察不到行为差异**。它的收益是**自举确定性**（换加载器/换链接布局都不依赖这个巧合），不是修一个正在犯的 bug。别在文档里吹成"修了个严重 bug"。

- [ ] **Step 6: Commit**

```bash
git add src/linker.ld src/boot.asm
git commit -m "M7: 引导期结构独立成段 + _start64 清真正的 .bss"
```

---

## Task 2: IDT 扩到 48 门 + stub + `isr_handler` 分支

> 混合：`isr.asm` / `idt.h` / `idt_init` 填门是 ⚪🟡（AI 写）；`isr_handler` 的分支是 🔴（作者写）。

**Files:**
- Modify: `src/isr.asm`（⚪）、`src/idt.h`（⚪）、`src/idt.c`（🟡 `idt_init` + 🔴 `isr_handler`）
- Create: `src/irq.h`（⚪）、`src/irq.c`（⚠ **本任务先落一份 ⚪ 占位实现**，Task 3 由作者重写）
- Modify: `Makefile`（加 `irq.o`）

**Interfaces:**
- Consumes: M6 的 `regs_t`、`isr_stub_table`、common stub 栈帧契约（**不改**）
- Produces:
  - `extern void *isr_stub_table[48];`（从 32 扩到 48）
  - `#define IDT_IRQ_BASE 32`、`#define IDT_IRQ_LAST 47`（`idt.h`）
  - `typedef void (*isr_reg_t)(regs_t *);`（`irq.h`）
  - `void irq_dispatch(regs_t *r);`（`irq.h` 声明；本任务给占位实现，Task 3 重写）

- [ ] **Step 1: `isr.asm` 表从 32 扩到 48（⚪）**

`16~31` 仍是 CPU 异常（保留原样）；`32~47` 全部按 **`ISR_NOERR`** 生成（IRQ **不带**错误码，宏补的 dummy `0` 正好补平差异）。表也扩到 48 项。

**不要**改 common stub 的压栈/恢复逻辑 —— 它已经完整（M6 特意写全了）。只在 common 里核对一行注释：**总压栈仍是 20 qword（偶数）**，对齐假设继续成立。

- [ ] **Step 2: `idt.h` 加常量，`idt_init` 填 `32~47` 门（⚪）**

`idt_init()` 现在：先清 `0~255`，再填 `0~31`。新增填 `32~47`（用 `isr_stub_table[32..47]`，门参数与异常门**完全相同**：`selector=0x08`、`type_attr=0x8E`、`ist=0`）。

**别破坏既有的 `0~255` 清零**——两者的先后要理清（先清零，后填门；`32~47` 填在清零之后）。

- [ ] **Step 3: `irq.h` + `irq.c` 占位（⚪，明确标注"临时"）**

`irq.c` 先只放一个最小实现，让本任务**能链接、能验收**：

```c
/* ⚠ 占位：Task 3 由作者重写为真正的分发表。仅为让 Task 2 可独立链接与验收。 */
void irq_dispatch(regs_t *r) { ... 打印 vector 后返回 ... }
```

文件顶部写一行注释说明它是占位。

- [ ] **Step 4: `isr_handler` 加分支（🔴 作者写）**

在 `idt.c` 的 `isr_handler` 最前面加一条分支：**`vector >= 32` → `irq_dispatch(r)` 并返回；否则走原有的 panic 路径。**

**这一条分支就是 M7 的全部意义所在**：M6 里 32 个向量全 panic，因为"没有内存管理，一个异常都修不了"；现在外部中断（可恢复）第一次和 CPU 异常（不可恢复）分道扬镳。

- [ ] **Step 5: 用「软件中断」独立验证分发路径（本任务的关键设计）**

**不需要 PIC、不需要 `sti`** —— `int $0x21` 是**软件**产生的，走同一张 IDT、同一个 stub、同一个 common，直接进 `isr_handler`。在 `kmain` 里用编译期开关加：

```c
#define M7_CASE 1   /* 1=软件 int 0x21 探路  0=正常 */
#if M7_CASE == 1
    __asm__ volatile ("int $0x21");
#endif
```

```bash
make && make iso
{ sleep 8; echo 'memsave 0xb8000 4000 build/m7_t2.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m7_t2.bin
```

Expected: 屏幕出现来自 `irq_dispatch` 的输出（占位版的 vector 号，应为 `33` = `0x21`），**然后内核继续正常跑**（不是 panic）。

- [ ] **Step 6: 回归 —— M6 没被弄坏（🔴 分支最容易写反的地方）**

拔掉 `M7_CASE`（回到 0）/ 或切到 M6 原有的 `#DE` 触发，重跑解码。
Expected: 仍打出 `EXCEPTION 0x00 #DE Divide Error` + `error=0x0000000000000000`。

**若 `#DE` 走进了 `irq_dispatch`，说明分支判据（`>= 32`）写反了。**

- [ ] **Step 7: Commit**

```bash
git add src/isr.asm src/idt.h src/idt.c src/irq.h src/irq.c Makefile
git commit -m "M7: IDT 扩到 48 门 + stub 补齐 + isr_handler 加 IRQ 分支 (+ 占位 irq_dispatch)"
```

**交接提问检验（作者答，2~3 题）**：
- `32~47` 为什么全走 `ISR_NOERR`，压栈总量还是 20 qword？
- 软件 `int $0x21` 和硬件 IRQ1 走的是同一条路径吗？哪一段开始不同？

---

## Task 3: `irq.c` —— 分发层（作者从零重写）

> 🔴 作者从零写。AI 只说这几个函数该干什么。

**Files:**
- Modify: `src/irq.c`（把 Task 2 的占位重写）、`src/idt.c`（若分支签名需要微调）

**Interfaces:**
- Produces:
  - `void irq_register(uint8_t irq, isr_reg_t fn);`
  - `void irq_dispatch(regs_t *r);`
  - （可选，Task 4 需要）`void irq_unregister(uint8_t irq);`

- [ ] **Step 1: 想清楚三件事再动手**

1. **分发表放哪**：`static isr_reg_t irq_handlers[16];` —— 它也在 `.bss`，Task 1 之后才有归零保证。
2. **`irq` 怎么算**：`irq = vector - IDT_IRQ_BASE`。vector 超出 `32~47` 时要**安全返回**（Review Focus #4），不要越界索引。
3. **EOI 放哪**：在 handler **之后**。（Task 4 会问：为什么不能放之前？）

- [ ] **Step 2: 实现**

只做分发，不碰芯片寄存器（那是 `pic.c` 的事）。**未注册的 IRQ 要打印一行可辨认的提示后安全返回。**

- [ ] **Step 3: 验收 —— 注册一个处理函数，软件 `int` 打到它**

在 `kmain` 里 `irq_register(1, my_probe)`（`my_probe` 打一行固定文本），`M7_CASE=1` 触发 `int $0x21`。
Expected: 屏上出现 `my_probe` 的固定文本（证明"注册 → 分发 → 调用"整条通），而不是占位版那行。

- [ ] **Step 4: 验收 —— 未注册的 IRQ 不炸**

触发 `int $0x22`（没有注册）。
Expected: 打印"未处理"提示并返回内核继续跑，**不卡死、不 panic**。

- [ ] **Step 5: Commit**

```bash
git add src/irq.c src/idt.c src/kernel.c
git commit -m "M7: irq.c 分发层 (分发表 + 注册 + 兜底)"
```

**交接提问检验**：
- "EOI 为什么放在 handler 之后而不是之前？"
- "`irq_dispatch` 里对没注册的向量，你为什么还要返回而不是 panic？"

---

## Task 4: `pic.c` —— 芯片层（AI 骨架 + 作者填空）

> 🟡 AI 写骨架与每步注释，**作者填 `outb` 的值、补从片 EOI 分支**。

**Files:**
- Create: `src/pic.h`（⚪ 常量）、`src/pic.c`（🟡）
- Modify: `src/kernel.c`（初始化序列 + 临时的 `sti; hlt` 死循环）
- Modify: `Makefile`

**Interfaces:**
- Produces:
  - `void pic_init(void);` —— 重映射 + 屏蔽全部（掩码 `0xFF`）
  - `void pic_unmask(uint8_t irq);` / `void pic_mask(uint8_t irq);`
  - `void pic_send_eoi(uint8_t irq);` —— **从片（IRQ ≥ 8）时要给从片也发 EOI**（作者补这一段）

- [ ] **Step 1: 为什么必须重映射（先答出来再动手）**

PIC **出厂默认**把 IRQ0-7 映射到向量 `0x08~0x0F`，正好压在 `#DF/#TS/#NP/#SS/#GP/#PF` 上。M6 全程 `cli`，所以这条边界不存在；M7 一引入 `sti`，IRQ0-7 悬着就会**冒充 CPU 异常**跑进异常处理器。**这才是"必须重映射"的实质原因，不只是习惯。**

- [ ] **Step 2: AI 落骨架 + ICW 注释（⚪🟡）**

骨架给出：四个 ICW 的**顺序与含义注释**、每个 `outb` 该写哪个端口。**`outb` 的数值由作者填**（这是本任务的 🔴 部分）—— 对照 ICW1 `0x11`（初始化 + 级联 + 需要 ICW4）等手册事实，自己推。

- [ ] **Step 3: 作者补 `pic_send_eoi` 的从片分支**

主片 EOI 端口 `0x20`，从片 `0xA0`。IRQ ≥ 8 时要**两个都发**。现在没有从片设备 → 这一支**不可观测**，但必须写对（写注释说明它为什么现在走不到）。

- [ ] **Step 4: `kernel.c` 初始化序列 + 临时死循环**

顺序**不可颠倒**：`idt_init()`（含 `32~47` 门）→ `pic_init()` → `pic_unmask(1)` → `sti`。
`kmain` 末尾先用临时的：

```c
for (;;) { __asm__ volatile ("sti; hlt" ::: "memory"); }
```

（Task 5 会换成真正的主循环。）

- [ ] **Step 5: 验收 A —— 重映射生效、屏蔽位对（不需要按键）**

在 `kmain` 里临时用 `vga_printf` 打出 `inb(0x21)`（IMR）。

```bash
make && make iso
{ sleep 8; echo 'memsave 0xb8000 4000 build/m7_t4.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m7_t4.bin
```

Expected: `IMR=0xFD`（`1111_1101` —— 只有 IRQ1 解屏蔽）。

- [ ] **Step 6: 验收 B —— `sendkey h` 真的能到 guest，且走的是向量 `0x21`（证据阶梯 0）**

```bash
{ sleep 8
  echo 'sendkey h'; sleep 0.5
  echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256 -d int -D build/int.log
grep 'v=21' build/int.log
```

Expected: 日志里出现 `v=21` 的中断记录。

**这一条的价值**：它证明 **PIC → CPU → IDT → stub → 分发** 整条硬件通路是通的 —— 此刻**键盘驱动一行都还没写**。

- [ ] **Step 7: 验收 C —— 连续按键不会卡死（EOI 对的佐证）**

```bash
{ sleep 8; echo 'sendkey h'; sleep 0.5; echo 'sendkey h'; sleep 0.5; echo 'sendkey h'; sleep 0.5
  echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256 -d int -D build/int2.log
grep -c 'v=21' build/int2.log
```

Expected: `v=21` 出现 **3 次**（而不是 1 次）。
**只出现 1 次 = 典型故障**——但现在还没读 `0x60`，所以 8042 本来就不会再抬高 IRQ1；本步若只有 1 次，**先别慌**，Task 5 读完 `0x60` 后再回来重测这一条。

> **订正（2026-10-20 实测）**：QEMU `sendkey` 每次按键产生 **make + release 两个 scancode**。Task 5 的 handler 读 `0x60` 之后，每次按键是 **2 次** IRQ1；三次按键共 **6 次** `v=21`，不是 3 次。Task 4 阶段只收到 1 次，是因为 release 字节卡在 8042 输出缓冲里、不会再次抬高 IRQ1 —— 这正好是「只收到第一个键」的原因之一。

- [ ] **Step 8: Commit**

```bash
git add src/pic.h src/pic.c src/kernel.c Makefile
git commit -m "M7: pic.c 芯片层 (重映射 + 屏蔽位 + EOI) + 临时 sti;hlt"
```

**交接提问检验**：
- "PIC 为什么要重映射？不重映射会发生什么？"
- "`pic_send_eoi(1)` 现在走主片还是从片分支？为什么那一支现在测不到？"

---

## Task 5: `keyboard.c` + 主循环 —— 端到端

> 🔴 作者写主体；两张 scancode 查表是 ⚪ AI 给的纯数据。

**Files:**
- Create: `src/keyboard.h`（⚪）、`src/keyboard.c`（🔴）
- Modify: `src/kernel.c`（主循环 🟡 骨架 + 🔴 作者填）、`Makefile`

**Interfaces:**
- Produces:
  - `void keyboard_init(void);` —— 注册 IRQ1 处理函数
  - `void keyboard_handler(regs_t *r);` —— 中断上下文，只做四件事
  - `int kb_get(void);` —— 主循环调用；**空返回 `-1`**
  - `#define KB_BUF_SIZE 64`（**必须是 2 的幂**）

- [ ] **Step 1: 环形缓冲（作者写两端）**

```c
static volatile char     kb_buf[KB_BUF_SIZE];   /* 住在 .bss —— Task 1 之后才有归零保证 */
static volatile unsigned kb_head, kb_tail;      /* head 只在中断写, tail 只在主循环写 */
```

- **`volatile` 省不掉**：`kb_head` 只在中断里被改，编译器看主循环那条路径时完全有理由认定它不变，把 `kb_head == kb_tail` 提前。不加的典型症状是"中断明明在跑，主循环像没看见"。
- **SPSC 单核下不需要 `cli`**：读一个 `volatile unsigned` 是原子的。把这条线画准：**什么时候才真的需要临界区**。
- **`& (KB_BUF_SIZE - 1)` 替代 `%`** —— 代价是 SIZE 必须 2 的幂。
- **满时丢新字符**（Review Focus #1）：先算 `next`，`next != kb_tail` 才写入。

- [ ] **Step 2: scancode 状态机（作者写）**

- **release 位**：make code 的 bit7 置 1 表示松键 → **不产生字符**（Review Focus #3）。
- **`0xE0` 前缀**：吃掉它，下一字节按扩展键处理（或显式丢弃）。
- **Shift**：记一个 `static volatile int shift_pressed;`，两张表切换大小写。
- **未覆盖的 scancode 必须显式映射为 0**（Review Focus #2），不能读到表外。

两张纯数据查表（基础 + shifted，各 128 项）由 **AI 写（⚪）**。

- [ ] **Step 3: 主循环（作者写）**

```c
for (;;) {
    int c;
    while ((c = kb_get()) >= 0) vga_putchar((char)c);
    __asm__ volatile ("sti; hlt" ::: "memory");
}
```

**`sti` 与 `hlt` 必须是相邻的一条 asm。** 否则存在窗口：主循环查完缓冲为空 → 中断插入把字符塞进缓冲 → 执行 `hlt` → 没有 pending 中断 → **CPU 睡死，数据永远等不到人取**。

- [ ] **Step 4: 验收 —— 端到端（证据阶梯 5、6）**

```bash
make && make iso
{ sleep 8
  echo 'sendkey h';   sleep 0.3
  echo 'sendkey i';   sleep 0.3
  echo 'sendkey ret'; sleep 0.5
  echo 'memsave 0xb8000 4000 build/m7_t5.bin'; sleep 0.5
  echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m7_t5.bin
```

Expected: 屏幕出现 `hi`，光标跟在后面。

- [ ] **Step 5: 验收 —— Shift 与 release（阶梯 7）**

`sendkey shift-h` → 出 `H`；单独 `sendkey h` → 出 `h` 且**松开不产生额外字符**。

- [ ] **Step 6: 验收 —— M5 的终端代码第一次被真的调用（阶梯 8）**

`a` → `backspace` → 应擦掉 `a`；`tab` → 跳列；`ret` → 换行。
**这是 M7 最漂亮的地方**：M5 里写的 `\b`/`\t`/换行边界处理，从写完那天起**没有任何调用者** —— 直到键盘接通的这一刻。

> **订正（2026-10-20 实测）**：`vga_putchar('\b')` **只把光标左移一位，不擦字符**（`src/vga.c:98`）。实测 `a` + `backspace` 后屏幕上 `a` 仍在；随后输入 `b` 时 `b` 覆盖它，所以终端功能仍可验收。要真正擦除，主循环需要发 `\b \b`，并且要知道当前列号以保护行首 —— 属于行编辑逻辑，本里程碑不做（规格也禁止改 `vga_*`）。本条的期望应从「擦掉」改为「光标回退、后续字符覆盖」。

- [ ] **Step 7: 验收 —— M6 回归（阶梯 9）**

切到 `#DE` 触发。Expected: 仍正确 panic。
**加 IRQ 分支把异常路径搞坏是这类改动最常见的事故，而且它安静 —— 不测不会知道。**

- [ ] **Step 8: Commit**

```bash
git add src/keyboard.h src/keyboard.c src/kernel.c Makefile
git commit -m "M7: keyboard.c (scancode 状态机 + 环形缓冲) + 主循环端到端"
```

**交接提问检验**：
- "`kb_head` 不加 `volatile` 会怎样 —— 说一个具体的编译结果，别说'有 bug'。"
- "为什么 `kb_get` 里不用关中断？"
- "`sti; hlt` 拆成两条会出什么问题？"

---

## Task 6: 收尾

**Files:**
- Modify: `计划书.md`、`README.md`、`docs/AI-使用记录.md`
- Create: `docs/学习笔记/M7-串联.md`（**作者亲手写**）
- Verify: 全仓库

- [ ] **Step 1: 确认零警告**

```bash
make clean && make 2>&1 | grep -i warning
```

Expected: 只有 M5 遗留的那两条 `ld` 警告，**没有任何 `gcc`/`nasm` 编译警告**。
（`make clean` 会删掉 `build/`，跑之前先确认里面没有要留的东西 —— 这是 M6 的教训。）

- [ ] **Step 2: 里程碑收尾检验（二选一）**

- **A. 埋错找 bug** —— AI 在代码上**只改一行**，只给症状，作者定位。素材池（前两条**症状相同、原因不同**）：

  | 埋法 | 症状 |
  |---|---|
  | 删掉 `pic_send_eoi()` | 只收到第一个键 |
  | 删掉 `inb(0x60)` | 只收到第一个键（**同一症状！**） |
  | `translate()` 忘过滤 release 位 | 每个键出两个字符 |
  | 环形缓冲空/满判断写反 | 永远读到空，或哈希 |
  | 去掉 `volatile` | `-O2` 下主循环像没看见按键 |
  | `sti; hlt` 拆成两条 | 理论丢唤醒 —— **大概率测不出来，诚实标注为"讲得通但验不到"** |

- **B. 讲给我听** —— 不看代码回答："从按下 `h` 到屏幕出现 `h`，经过哪些寄存器、哪些内存？中断进来的那一刻，栈上长什么样？画出来。"

- [ ] **Step 3: 回填文档**

- `计划书.md` §M7：状态改 ✅ 完成；补**「实施中踩到并解决的关键点」**与**「教训」**（照 M5/M6 的写法，写你**实际**踩到的，不是照抄设计）。
- `README.md` 进度表加 M7 行。
- `docs/AI-使用记录.md`：记本次 AI 参与范围（哪些是 ⚪/🟡 由 AI 写、哪些是 🔴 由作者写、哪里被作者改了、AI 错在哪）。
- `docs/学习笔记/M7-串联.md`：**作者亲手写**（与 AI 的 `M0-M6-知识总结(AI参考版).md` 分开放）。

- [ ] **Step 4: Commit**

```bash
git add 计划书.md README.md docs/AI-使用记录.md docs/学习笔记/M7-串联.md
git commit -m "docs: 里程碑更新到 M7 (外部中断 PIC + 键盘完成)"
```

---

## Self-Review

**1. Spec coverage** —— 逐条对 `计划书.md` §M7：

| Spec 要求 | 落在哪个任务 |
|---|---|
| 第 0 步补 `_start64` 清 `.bss` | Task 1 |
| PIC 重映射 `0x20~0x2F` | Task 4 |
| **只解屏蔽 IRQ1（IMR=0xFD）** | Task 4 Step 5 |
| IDT `32~47` 填门（不破坏 `32~255` 清零） | Task 2 Step 2 |
| `isr_handler` 加 `≥32` 分发分支、`<32` 仍 panic | Task 2 Step 4、Step 6 |
| 键盘 `0x60` → 转换 → 环形缓冲 | Task 5 |
| 主循环消费缓冲 | Task 5 Step 3 |
| 验证标准 2：阶梯 0~9 | 阶梯 0→Task 4 Step 6；1→Task 4 Step 5；2→（见下"缺口"）；3/4→Task 5 Step 4；5→Task 5 Step 4；6→Task 5 Step 4；7→Task 5 Step 5；8→Task 5 Step 6；9→Task 5 Step 7 |
| 硬约束：装门→重映射→解屏蔽→`sti` | Task 4 Step 4（Global Constraints 复述） |
| `.bss` 陷阱（`.bootstrap` 段 + 只夹真 `.bss`） | Task 1 Step 1~3、Step 4 |
| 20 qword 对齐 / 用户态时破 | Task 2 Step 1 |
| "只收到第一个键"两个原因 | Task 6 Step 2 素材池 |
| `sti; hlt` 相邻 | Task 5 Step 3 |
| 环形缓冲 `volatile` / SPSC / 2 的幂 | Task 5 Step 1 |
| 取证复用 `sendkey`/`decode_vga.py`/`-d int`、不新写脚本 | 各任务验收命令 |
| 分工 5 处 🔴 | Task 1（×2）、Task 2 Step 4、Task 3、Task 5 主体 |
| `make` 零警告、`ld` 不新增 | Task 6 Step 1 |

**已知缺口（需作者决定）**：阶梯 **2**（"第 33 号门指向正确的 stub"——`nm` 取 `isr_stub_33` 地址 + QEMU 读 IDT 内存、三段 offset 拼起来比对）**本计划只并入了 Task 2 的整体验收，没有单列成一步**。若作者要保留这条独立证据，在 Task 2 后加一步即可（命令形如 `nm build/kernel.elf | grep isr_stub_33` + `info registers` 读门）。

**2. Placeholder scan** —— 无 "TBD"/"TODO"；每个代码步骤给的是**契约、精确符号/端口事实、可执行验收命令与期望输出**。Task 2 Step 3 的 `irq.c` 占位是**明确标注的临时物**，且 Task 3 有"重写"的显式步骤，不是"以后再补"。

**3. Type consistency** —— `isr_stub_table[48]`、`irq_dispatch(regs_t *)`、`isr_reg_t`、`irq_register(uint8_t, isr_reg_t)`、`kb_get(void) -> int`、`KB_BUF_SIZE`、`pic_send_eoi(uint8_t)`、`IDT_IRQ_BASE` 在各任务中一致；`regs_t` 沿用 M6 定义未改字段。

**4. 与写作技能默认的偏差（项目约定，需作者确认）** —— 本计划按项目约定**不提供可直接粘贴的完整函数体/stub 体**，只给接口契约、精确符号与寄存器事实、验收命令与期望输出。这与 writing-plans 技能默认的"每步附完整代码"不同，是刻意为之（项目约定：内核代码由作者亲手写）。

**5. 与 09-21 已认可方案的偏差（需作者确认）** —— 两处，均为提高可测性：
- **编码顺序**：Task 3（`irq.c` 分发）排在 Task 4（`pic.c` 芯片）**之前**。原方案是 pic 先 irq 后。理由：`int $0x21` 是**软件**中断，让分发层在**不碰任何硬件、不 `sti`** 的情况下就能被独立验收。运行期初始化顺序（装门→重映射→解屏蔽→`sti`）**未变**。
- **Task 2 引入 ⚪ 占位 `irq.c`**，使"扩门 + 分支"能独立链接与验收；真正的分发层在 Task 3 由作者重写。原方案未指定这一过程细节。
