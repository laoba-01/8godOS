# M8 — 内存管理(物理 + 虚拟 + 按需分页 + kmalloc)实施计划

> **执行者**:项目作者本人。项目约定:**内核代码由作者亲手写**,AI 只做设计/审阅/验收标准。
> **档位**:🔴 作者写 / 🟡 AI 骨架 + 作者填空 / ⚪ AI 写样板(判据:这段代码是否承载 M8 要教的概念)。
> **步骤**:每个任务末尾是验收动作与期望输出,不是"跑测试套件"。
> **效率出口**:可随时说"你直接写",但要报一句原档位,并记入 `docs/AI-使用记录.md`。

**Goal:** 让内核第一次拥有可分配的物理页、可建立的虚拟映射、可恢复的缺页处理,以及一个按字节分配的内核堆。

**Architecture:** 分四层,各自只做一件事:
1. `pmm.c` 物理层:multiboot2 memory map → 位图帧分配器 → 恒等映射扩展;
2. `vm.c` 虚拟层:四级页表遍历器、`map`/`unmap`、地址空间(CR3)、`#PF` 恢复;
3. `heap.c` 堆层:在 16MB 虚拟区上做 first-fit + 切分 + 合并的 `kmalloc`/`kfree`;
4. `mb2.c` 解析层:只负责把 GRUB 的信息结构翻译成"可用物理区间"。

物理 → 虚拟 → 按需分页 → 堆,依赖严格单向:后一层用前一层的接口,前一层的验收不过,后一层不开始。

**Tech Stack:** NASM 2.16 / GCC 13.3(`-ffreestanding -nostdlib -mno-sse -mno-red-zone -mcmodel=large -O2`)/ ld / QEMU 8.2.2 / GRUB multiboot2 / WSL2 Ubuntu 24.04

**Spec:** 本计划 §0「范围与目标」+ `计划书.md` §7 后续里程碑第 3 项。M8 完成后,把 §0 回填进 `计划书.md` 作为 §M8。

---

## 0. 范围与目标(本计划的 spec)

### 0.1 做什么

- **物理内存**:保存 multiboot2 信息指针 → 解析 memory map → 位图帧分配器(4KB 帧)→ 把恒等映射从 2MB 扩到覆盖可用 RAM(封顶 1GB)。
- **虚拟内存**:四级页表遍历器(缺中间表就分配)→ `vm_map` / `vm_unmap` → `invlpg` → 地址空间(PML4 + CR3 切换 + 内核映射共享)。
- **按需分页**:把 `#PF` 从"一律 panic"改成"堆区内的缺页 → 分配帧、建映射、返回重试";堆区外仍 panic。
- **内核堆**:在 `0xFFFF800000000000` 起 16MB 虚拟区上实现 `kmalloc`/`kfree`(first-fit + 切分 + 合并 + 对齐)。

### 0.2 明确不做

- 高半区重定位(内核仍恒等映射在低地址)
- 页面置换、swap、后备存储
- 分段、段页式
- COW、mmap 文件
- 用户态、多任务、系统调用
- 5 级页表、PCID、全局页、NX
- 伙伴系统、连续物理分配
- 页表页/地址空间的回收(`vm_destroy_space` 不做,测试中允许泄漏)

### 0.3 物理内存布局(现状,已核实)

| 区域 | 地址 | 说明 |
|---|---|---|
| 内核镜像 | `0x100000 ~ 0x10A100` | `.text/.rodata/.data/.bootstrap/.bss` |
| 页表 | `.bootstrap`: `0x102000 / 0x103000 / 0x104000` | pml4 / pdpt / pd,引导期已清零 |
| 内核栈 | `0x105000 ~ 0x109000` | 16KB |
| `.bss` | `0x109000 ~ 0x10A100` | `idt[256]`、`irq_handlers[16]`、键盘缓冲 |
| VGA | `0xB8000` | 在低 1MB 保留区内 |

M8 新增的 `pmm_bitmap[32768]`(1GB / 4KB / 8)住在 `.bss`,把 `__bss_end` 推到约 `0x112100`,仍在 2MB 以内。

### 0.4 虚拟地址布局(M8 目标)

| 区域 | 虚拟地址 | PML4 项 | 谁共享 |
|---|---|---|---|
| 内核恒等映射 | `0 ~ max_end`(封顶 1GB) | `0` | 所有地址空间(创建时复制) |
| 内核堆 / 按需分页区 | `0xFFFF800000000000` 起 16MB | `256` | 所有地址空间(创建时复制) |
| map/unmap 测试区 | `0xFFFF800001000000` 起 2MB | `256` | 所有地址空间(创建时复制) |
| 每空间隔离测试区 | `0x0000400000000000` 起 2MB | `128` | **不复制**,每个空间独立 |

### 0.5 关键常量

| 常量 | 值 | 来源/理由 |
|---|---|---|
| 页/帧大小 | `4096` | x86_64 4KB 页 |
| 位图覆盖上限 | `0x40000000`(1GB) | 一个 PD 512 项 × 2MB |
| 帧数 | `262144` | 1GB / 4KB |
| 位图字节数 | `32768` | 262144 / 8 |
| 堆基址 | `0xFFFF800000000000` | PML4[256],标准高半区窗口 |
| 堆大小 | `0x01000000`(16MB) | 足够测试,不占物理内存直到触碰 |
| 地址掩码 | `0x000FFFFFFFFFF000` | 页表项 bit 12~51 |
| 恒等映射页标志 | `0x83` | P + RW + PS(2MB 大页) |

---

## Global Constraints

- 所有构建与运行**在 WSL 里**执行(`cd /mnt/d/8GodOS`)。Windows 侧只编辑文件。
- `make` 必须**零警告**通过(`-Wall -Wextra`);`ld` 仍是 M5 那两条(`boot.o` 缺 `.note.GNU-stack`;`kernel.elf` 有 RWX LOAD 段),**不新增**。
- **初始化顺序不可颠倒**:`idt_init()` → `vm_init()` → `pmm_init()` → `heap_init()` → `pic_init()` → `pic_unmask(1)` → `keyboard_init()` → 主循环里的 `sti`。
  - `idt_init` 最先:任何缺页都要有门可进。
  - `vm_init` 在 `pmm_init` 之前:只读 CR3,给 `#PF` 处理器一个有效的 `kernel_space`。
  - `pmm_init` 在 `heap_init` 之前:堆的按需缺页要靠帧分配器。
- **自检顺序**:`pmm_self_test()` → `vm_self_test()` → `heap_self_test()`,在 `keyboard_init()` 之后、主循环之前;任一条失败打印 `FAIL` 并停机。
- 中断路径(M6 stub/common、`regs_t` 20 qword、22 qword 完整栈帧、16 字节对齐)**原样继承**,本里程碑不改 `isr.asm`、不改 `regs_t`。
- 物理地址用 `uint64_t` 表示。M8 全程恒等映射,所以 `(void *)(uintptr_t)pa` 可当指针用 —— 这是**本里程碑的便利**,代码注释里要标明,将来高半区会失效。
- 恒等映射区(低 1GB)只允许 2MB 大页。`vm_map` 的 4KB 映射**禁止**进入该区,不做大页拆分。
- 新地址空间创建时复制 `kernel_space->entry[0]` 与 `entry[256..511]`。创建之后新增的内核映射不会自动同步。
  - M8 的地址空间测试只用 `entry[128]`(每空间独立区),不依赖堆;
  - 若将来要在新空间里访问堆,必须在 `heap_init` 之后创建空间(或在创建后补同步)。
- 取证一律复用:`sendkey` / `memsave 0xb8000 4000` + `python3 tools/decode_vga.py` / `-d int` / QEMU monitor `info mem`、`info tlb`。**不新增取证脚本**。
- `make clean` 会删掉整个 `build/`;跑之前先确认里面没有要留的东西(M6 的教训)。
- 每个任务结束提交一次;commit message 用 `M8: ...` 前缀。作者本人提交,AI 生成的代码也以作者名义提交,来源以文件头声明与 `docs/AI-使用记录.md` 为准。

---

## Review Focus

规格没写、但最可能咬到人的五类失败模式,各自在所属任务里钉一条验收:

1. **multiboot2 信息结构在 2MB 以外** —— 解析它之前必须先把它所在的 2MB 区间映射上,否则"读 memory map"本身就会 `#PF`(此时 `vm_init` 已跑,但故障地址不在堆区,`#PF` 处理器会交给 panic)。→ Task 1 Step 2/4。
2. **保留区漏标** —— 分配器把内核镜像、位图、内核栈或 multiboot 信息所在帧分出去,一写就崩,而且症状可能是"随机时刻黑屏"。→ Task 2 Step 1/4/6。
3. **新地址空间没复制内核映射** —— 切 CR3 的瞬间取指 `#PF`,IDT 里的门也找不到 → triple fault 黑屏。→ Task 4 Step 1/5。
4. **`#PF` 处理器重入或处理过头** —— 处理器自己缺页 → 递归 → triple fault;或者把"已存在但只读"的保护错误也当成缺页来补页。→ Task 5 Step 1/5。
5. **kmalloc 合并写坏相邻块** —— 边界算错一个块头,`kfree` 后下一次分配覆盖别人的数据,症状是"前面都对,后面某个结构突然变了"。→ Task 6 Step 3/7。

---

## File Structure

| 文件 | 动作 | 档 | 职责 |
|---|---|---|---|
| `src/boot.asm` | 改 | 🔴 | `_start` 保存 EBX(multiboot2 指针)到 ESI;`_start64` 清完 `.bss` 后经 RDI 传给 `kmain` |
| `src/mb2.h` | 新增 | ⚪ | multiboot2 tag 常量、mmap 结构、`mb2_range_t`、`mb2_init` 原型 |
| `src/mb2.c` | 新增 | 🔴 | tag 遍历 + memory map 解析 + 可用区间复制 |
| `src/pmm.h` | 新增 | ⚪ | 帧大小/上限/位图大小常量、`pmm_*` 原型 |
| `src/pmm.c` | 新增 | 🔴 | 信息结构映射 + memory map → 位图 → alloc/free + 恒等映射扩展 + 自检(自检 🟡) |
| `src/vm.h` | 新增 | ⚪ | 页表项标志、地址掩码、四个虚拟区常量、`page_table_t`、`vm_*` 原型 |
| `src/vm.c` | 新增 | 🔴 | 四级遍历、map/unmap、地址空间、CR3 切换、`#PF` 恢复 + 自检(自检 🟡) |
| `src/heap.h` | 新增 | ⚪ | `kmalloc`/`kfree`/`heap_init` 原型 |
| `src/heap.c` | 新增 | 🔴 | 块头、first-fit、切分、合并、对齐、double-free 检测 + 自检(自检 🟡) |
| `src/idt.c` | 改 | 🔴 | `isr_handler` 在 panic 之前加 `vector == 14 && vm_handle_page_fault(r)` 分支 |
| `src/kernel.c` | 改 | 🟡 | `kmain(uint64_t mb_info)`、初始化顺序注释、自检调用、主循环保持不变 |
| `Makefile` | 改 | ⚪ | `mb2.o` / `pmm.o` / `vm.o` / `heap.o` 规则与链接项 |
| `src/linker.ld` | 不改 | — | 复用已有的 `__bss_end` 作为内核镜像末端;`.bss` 是最后一个输出段 |
| `docs/学习笔记/M8-串联.md` | 新增 | 🔴 | **作者亲手写**,T7 交付 |

**分层的理由(要能讲出来)**:
- `mb2.c` 只懂 GRUB 的数据格式,不懂物理内存怎么管;
- `pmm.c` 只懂"哪些帧空着",不懂虚拟地址;
- `vm.c` 只懂"虚拟地址怎么映射",不懂字节级分配;
- `heap.c` 只懂"一块虚拟区怎么切",物理帧由缺页处理器在背后补。
换引导器只动 `mb2.c`,换分配算法只动 `pmm.c`,换堆算法只动 `heap.c`。

---

## Task 0: 前置欠账(不写 M8 代码)

> 🔴 的两份笔记由作者写;🟡 的文档由 AI 代笔、作者核对。

**Files:**
- Modify: `计划书.md` §M7、`README.md`、`docs/AI-使用记录.md`
- Create: `docs/学习笔记/M7-串联.md`(**作者亲手写**)

**目的:** M8 要在一套"已经验收过"的地基上开工。M7 的代码已提交,但收尾未做;M6 的实现层检验也还欠着。

- [ ] **Step 1: 确认零警告基线**

```bash
cd /mnt/d/8GodOS
make clean && make 2>&1 | grep -i warning
```

Expected: 只有 M5 遗留的两条 `ld` 警告;**没有任何 `gcc`/`nasm` 警告**。
(`make clean` 前先 `ls build/`,确认没有要留的取证文件。)

- [ ] **Step 2: M7 收尾「埋错找 bug」**

AI 在 M7 代码上**只改一行**,只给症状,作者定位。素材池(前两条症状相同、原因不同):

| 埋法 | 症状 |
|---|---|
| 删掉 `pic_send_eoi()` | 只收到第一个键 |
| 删掉 `inb(0x60)` | 只收到第一个键(同一症状) |
| `translate()` 忘过滤 release 位 | 每个键出两个字符 |
| 环形缓冲空/满判断写反 | 永远读到空,或哈希 |
| 去掉 `volatile` | `-O2` 下主循环像没看见按键 |

完成后把改动还原。

- [ ] **Step 3: M6 实现层补检(合上屏幕)**

不看代码,画出异常入口的完整栈帧:

- 22 qword = 176 字节;
- 15 个通用寄存器的顺序;
- `vector` / `error_code` / `rip` / `cs` / `rflags` 的偏移;
- CPU 压的 `rsp` / `ss` 在哪两个槽位(offset 160 / 168)。

画完对照 `src/idt.h` 的 `_Static_assert` 与注释核对。

- [ ] **Step 4: 补 5 个概念缺口(不写代码)**

1. multiboot2 tag 怎么遍历(8 字节对齐、`type=0` 结束、mmap tag 的 `entry_size`);
2. 页表遍历器怎么从 VA 切出四级索引,缺表时怎么分配;
3. PTE 的地址掩码 `& 0x000FFFFFFFFFF000` 与 4KB 对齐 `& ~0xFFF`;
4. kmalloc 的块头、切分、合并、对齐;
5. QEMU `info mem` / `info tlb` / `-d int` 各自能证明什么。

- [ ] **Step 5: 回填文档**

- `计划书.md` §M7:状态改 ✅;补「实施中踩到并解决的关键点」与「教训」(写实际踩到的);
- `README.md` 进度表加 M7 行;
- `docs/AI-使用记录.md`:M7 的 AI 参与范围、效率出口、尚未销的账(M6 实现层补检、M7 交接提问);
- `docs/学习笔记/M7-串联.md`:作者亲手写。

- [ ] **Step 6: Commit**

```bash
git add 计划书.md README.md docs/AI-使用记录.md docs/学习笔记/M7-串联.md
git commit -m "docs: M7 收尾 + M6 栈帧补检 + 学习笔记"
```

---

## Task 1: multiboot2 指针与可用内存

> 🔴 作者写 `boot.asm` 的保存/传递与 `mb2.c` 的解析;🟡 `pmm_init` 骨架与打印;⚪ 头文件与 Makefile。

**Files:**
- Modify: `src/boot.asm`(`_start` 在 26 行;`_start64` 清 `.bss` 在 104-108 行,`call kmain` 在 111 行)
- Create: `src/mb2.h`、`src/mb2.c`
- Create: `src/pmm.h`、`src/pmm.c`(本任务只做"映射信息结构 + 解析 + 打印",分配器在 Task 2)
- Modify: `src/kernel.c`(`kmain` 签名 + 调用)
- Modify: `Makefile`

**Interfaces:**
- Produces:
  - `typedef struct { uint64_t base, length; } mb2_range_t;`
  - `#define MB2_MAX_RANGES 32`
  - `int mb2_init(uint64_t info_addr, mb2_range_t *ranges, int max_ranges, uint64_t *info_end);`
    - 返回可用区间个数(≥0);失败返回 `-1`。
    - `info_end` 回填 `info_addr + total_size`,供 `pmm` 保留整个信息结构。
  - `int pmm_init(uint64_t mb_info);`(本任务只解析/打印,返回 0 成功)
- Consumes: `read_cr3()` 与 `boot.asm` 建立的 `pml4 → pdpt → pd` 恒等映射。

**已知事实(照抄,别重新猜):**
- `_start` 进来时:`EAX = 0x36D76289`,`EBX = multiboot2 信息结构的物理地址`。
- 现有代码在 `cpuid` 处会覆盖 `EBX`,必须在 `cli` 之后、任何 `cpuid` 之前把 `EBX` 存进 `ESI`。
- `ESI` 不被 `cpuid`、`rep stosd`、`rep stosb` 使用;`_start64` 里用 `RDI` 清零,所以 `mov rdi, rsi` 要放在清零循环**之后**、`call kmain` 之前。
- 32 位模式下写 `ESI` 会把 `RSI` 高 32 位清零,正好是 32 位物理地址。
- multiboot2 信息结构:`uint32_t total_size; uint32_t reserved;` 之后是 tag 链表;每个 tag `uint32_t type; uint32_t size;`,按 **8 字节对齐**前进,`type == 0` 结束。
- mmap tag:`type == 6`;tag 头之后是 `uint32_t entry_size; uint32_t entry_version;`,再后面是条目;每个条目 `entry_size` 字节,前 24 字节为 `uint64_t base; uint64_t length; uint32_t type; uint32_t reserved;`。
- 只有 `type == 1` 表示可用 RAM;其它类型(ACPI、NVS、坏内存)本里程碑一律不用。
- GRUB 通常把信息结构放在低 1MB 或内核之后,但不能假设它一定在 2MB 内。

- [ ] **Step 1: 先写"信息指针能到 C"的最小验证(🟡)**

`boot.asm` 保存 `EBX` → `ESI`;`_start64` 清零后 `mov rdi, rsi`;`kernel.c` 改成 `void kmain(uint64_t mb_info)` 并先打印 `mb_info`。

验收(先不做解析):

```bash
make && make iso
{ sleep 8; echo 'memsave 0xb8000 4000 build/m8_t1a.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m8_t1a.bin
```

Expected: 屏幕上 `mb_info=0x0000000000xxxxxx`(非 0),内核仍正常启动。

- [ ] **Step 2: 实现"信息结构所在 2MB 区间"的映射(🔴)**

在 `pmm.c` 里写一个只服务引导期的静态函数(不对外暴露):

```
ensure_2mb_mapped(addr, len)
  读 CR3 → pml4 → entry[0] → pdpt → entry[0] → pd
  对 [addr, addr+len) 覆盖到的每个 2MB 区间:
      若 pd[i] 的 P=0 → 写 (i << 21) | 0x83
      若已存在且 PS=0 → 返回错误(引导期不该有 4KB 映射)
  全部写完后 reload CR3(写回同一个值)刷 TLB
```

要求:
- 只处理 `addr < 1GB` 的情况;超出返回错误。
- 不覆盖已有的大页项。
- 先映射 `[mb_info, mb_info + 8)` 读到 `total_size`,再映射 `[mb_info, mb_info + total_size)`。

- [ ] **Step 3: 实现 `mb2_init`(🔴)**

按上面的 tag/mmap 格式遍历;把 `type == 1` 的区间复制进调用者数组;最多 `MB2_MAX_RANGES` 个;回填 `info_end`。
**不打印、不分配** —— 打印留给 `pmm_init`。

边界检查:
- `total_size == 0` 或 `> 1MB` → 返回 -1;
- 每个 tag 的 `size >= 8`,且 `tag + size` 不越界;
- mmap 的 `entry_size >= 24`;
- 条目数 = `(tag.size - 16) / entry_size`。

- [ ] **Step 4: 实现 `pmm_init` 的解析与打印(🟡)**

顺序:

1. `ensure_2mb_mapped(mb_info, 8)`;
2. 读 `total_size`;
3. `ensure_2mb_mapped(mb_info, total_size)`;
4. `mb2_init(...)` 拿区间;
5. 打印:`mb_info`、`total_size`、区间个数、每个区间的 `base/length`、可用总 MB。
6. 返回 0/-1。

打印格式固定,便于文本比对:

```
M8: mb_info=0x0000000000xxxxxx total_size=0x0000000000000xxx
M8: usable ranges=2
  [0] base=0x0000000000000000 len=0x000000000009fc00
  [1] base=0x0000000000100000 len=0x000000000ff00000
M8: usable=0x000000000ff00000 (~255 MB, 具体值随 QEMU memory map)
```

- Step 4 起,`kmain` 不再直接打印 `mb_info`,而是调用 `pmm_init(mb_info)`;Step 1 的临时打印删掉。`pmm_init` 返回 -1 时打印错误并停机。

- [ ] **Step 5: 验收 A —— 解析对,且不是写死**

```bash
make && make iso

# -m 256
{ sleep 8; echo 'memsave 0xb8000 4000 build/m8_t1_256.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m8_t1_256.bin

# -m 512
{ sleep 8; echo 'memsave 0xb8000 4000 build/m8_t1_512.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 512
python3 tools/decode_vga.py build/m8_t1_512.bin
```

Expected:
- 两次的 `usable=` 不同(256MB vs 512MB 左右),证明数值来自 memory map,不是常量;
- 区间数与 QEMU 的 memory map 相符;
- 没有任何 `#PF`(屏幕不是 panic 输出)。

- [ ] **Step 6: 验收 B —— 信息结构在 2MB 以外的负对照(可选,但推荐)**

临时把 `ensure_2mb_mapped` 的调用注释掉,重跑。Expected: 若 GRUB 把信息放在 2MB 以外,会出现 `#PF`;若恰好仍在 2MB 内,本负对照观察不到 —— **诚实记录实测结果**,不要假装测到了。

- [ ] **Step 7: Commit**

```bash
git add src/boot.asm src/mb2.h src/mb2.c src/pmm.h src/pmm.c src/kernel.c Makefile
git commit -m "M8: 保存 multiboot2 指针 + 解析 memory map"
```

---

## Task 2: 位图帧分配器 + 恒等映射扩展

> 🔴 作者写位图与 alloc/free;🟡 自检;⚪ 常量与 Makefile(已在 Task 1 加过规则)。

**Files:**
- Modify: `src/pmm.c`、`src/pmm.h`、`src/kernel.c`
- Verify: `Makefile` 已含 `pmm.o`

**Interfaces:**
- Produces:
  - `#define PMM_FRAME_SIZE   4096`
  - `#define PMM_MAX_PHYS     0x40000000ULL`(1GB)
  - `#define PMM_FRAME_COUNT  (PMM_MAX_PHYS / PMM_FRAME_SIZE)`
  - `#define PMM_BITMAP_BYTES (PMM_FRAME_COUNT / 8)`
  - `uint64_t pmm_alloc_frame(void);` —— 返回物理地址;`0` 表示 OOM(帧 0 永远保留)
  - `void pmm_free_frame(uint64_t pa);`
  - `uint64_t pmm_free_count(void);`
  - `uint64_t pmm_total_count(void);`
  - `int pmm_self_test(void);` —— 0 通过,-1 失败

- [ ] **Step 1: 位图初始化(🔴)**

`static uint8_t pmm_bitmap[PMM_BITMAP_BYTES];` 住在 `.bss`(Task 0 已确认 `.bss` 会清零,但不要依赖它 —— 进函数先显式全置 1)。

内核镜像末端用 `extern char __bss_end[];` 取地址(`linker.ld` 已导出,`.bss` 是最后一个输出段);保留区间上界是 `align_up((uint64_t)__bss_end, 4096)`。

顺序(不可颠倒):

1. **全部置 1**(默认不可用);
2. 对每个 `type == 1` 区间:`pmm_mark_range_free(base, len)`;区间要按帧向下/向上取整,并 clamp 到 `PMM_MAX_PHYS`;
3. 重新保留:
   - `[0, 0x100000)`(BIOS/VGA/EBDA)
   - `[0x100000, align_up((uint64_t)__bss_end, 4096))`(内核镜像 + 位图 + 页表 + 栈)
   - `[mb_info, info_end)`(信息结构)
4. 统计空闲帧数。

**为什么不是"先全 0,再把坏区间置 1"**:memory map 不一定枚举所有 MMIO 洞;未知区间必须默认不可用。反过来做,分配器可能把 VGA/BIOS 区或内核自己的页分出去。

- [ ] **Step 2: 恒等映射扩展(🔴)**

用 `read_cr3()` 拿到 pml4 → `entry[0]` → pdpt → `entry[0]` → pd。
从 memory map 求 `max_end` = 所有可用区间 `base + length` 的最大值,clamp 到 `PMM_MAX_PHYS`。
`n = align_up(max_end, 2MB) / 2MB`,cap 到 512。
对 `i = 1 .. n-1`:`pd[i] = (i << 21) | 0x83`(若已有项则跳过)。
写完后 reload CR3 刷 TLB。

- **为什么必须做**:分配器会返回 2MB 以上的帧;没有映射,写它立刻 `#PF`,分配出来也用不了。

- [ ] **Step 3: `pmm_alloc_frame` / `pmm_free_frame`(🔴)**

- alloc:从 `pmm_hint` 开始扫描 0 位;找到就置 1、更新 hint、返回 `i * 4096`;扫到末尾再从头扫一遍;没有返回 0。
- free:校验 4KB 对齐、`pa < PMM_MAX_PHYS`、位是 1;清 0;更新 hint 与空闲计数。非法输入直接忽略(不要 panic 在中断上下文里)。
- `pmm_free_count` / `pmm_total_count` 只读。

- [ ] **Step 4: 自检(🟡)**

`pmm_init` 要把 `mb_info` / `info_end` 存进 `pmm.c` 的 static 变量,供自检核对"分配出来的帧没有落进信息结构"。

`pmm_self_test` 必须打印并检查:

1. `total` 与 `free`(启动时);
2. 连续分配 **512 帧**,记录地址:
   - 每个地址 4KB 对齐;
   - 每个地址 `>= align_up(__bss_end, 4096)`;
   - 不在 `[mb_info, info_end)`;
   - **至少一帧 > `0x200000`**;
3. 对每帧写 `0xA5A5A5A5A5A5A5A5 ^ i`,读回比对;
4. 全部释放,`free` 回到分配前的值;
5. 再分配 512 帧,验证能复用(不要求顺序一致)。

输出格式:

```
M8/PMM: total=262144 free=<实际值, 256MB 时约 65000>
M8/PMM: 512 frames, max_pa=0x0000000000312000
M8/PMM: pattern PASS
M8/PMM: free-after-free=<与分配前相同>
M8/PMM: reuse PASS
```

任何一条失败:打印 `M8/PMM: FAIL <原因>` 并停机。

- [ ] **Step 5: 验收**

```bash
make && make iso
{ sleep 8; echo 'memsave 0xb8000 4000 build/m8_t2.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m8_t2.bin
```

Expected: 上述输出全部 PASS;`max_pa > 0x200000`;两次 `-m 256` / `-m 512` 的 `free` 不同。

- [ ] **Step 6: 负对照(必做一条)**

临时把保留内核镜像的那一行注释掉,重启。Expected: `pmm_self_test` 的"地址 `>= __bss_end`"检查失败,或写图案时立刻崩 —— **这证明保留区确实在起作用**。测完还原。

- [ ] **Step 7: Commit**

```bash
git add src/pmm.c src/pmm.h src/kernel.c
git commit -m "M8: 位图帧分配器 + 恒等映射扩展到可用 RAM"
```

---

## Task 3: 页表遍历器 + `vm_map`/`vm_unmap` + TLB

> 🔴 作者写遍历器与映射操作;🟡 自检;⚪ `vm.h` 常量。

**Files:**
- Create: `src/vm.h`、`src/vm.c`
- Modify: `src/kernel.c`(在 `pmm_init` 之前调用 `vm_init`)
- Modify: `Makefile`

**Interfaces:**
- Produces:
  - `#define VM_PAGE_SIZE 4096ULL`
  - `#define VM_PTE_PRESENT (1ULL << 0)`
  - `#define VM_PTE_WRITE   (1ULL << 1)`
  - `#define VM_PTE_USER    (1ULL << 2)`
  - `#define VM_PTE_HUGE    (1ULL << 7)`
  - `#define VM_ADDR_MASK   0x000FFFFFFFFFF000ULL`
  - `#define VM_HEAP_BASE       0xFFFF800000000000ULL`
  - `#define VM_HEAP_SIZE       0x01000000ULL`
  - `#define VM_SCRATCH_BASE    0xFFFF800001000000ULL`
  - `#define VM_SPACE_TEST_BASE 0x0000400000000000ULL`
  - `typedef struct { uint64_t entry[512]; } page_table_t;`
  - `int vm_init(void);` —— 读 CR3,记录 `kernel_space`,清零 `fault_count`
  - `page_table_t *vm_kernel_space(void);`
  - `page_table_t *vm_current_space(void);` —— 读当前 CR3 对应的 PML4
  - `int vm_map(page_table_t *space, uint64_t va, uint64_t pa, uint64_t flags);` —— 0 成功,-1 失败
  - `uint64_t vm_unmap(page_table_t *space, uint64_t va);` —— 返回旧物理地址;未映射返回 0
  - `int vm_self_test(void);`
- Consumes: `pmm_alloc_frame` / `pmm_free_frame`(Task 2)

- [ ] **Step 1: `vm_init` 与 CR3 读写(🟡)**

- `read_cr3` / `write_cr3` 内联汇编;
- `vm_init`:`kernel_space = (page_table_t *)(read_cr3() & VM_ADDR_MASK); fault_count = 0;`
- `vm_kernel_space` 返回它。

- [ ] **Step 2: 四级遍历器(🔴)**

```
vm_walk(space, va, create) -> uint64_t *
  索引:PML4=(va>>39)&0x1FF, PDPT=(va>>30)&0x1FF, PD=(va>>21)&0x1FF, PT=(va>>12)&0x1FF
  PML4 项:
      不存在且 create → 分配帧、清零、写 0x3 | P
      不存在且 !create → NULL
      PS=1 → NULL(大页,不能继续往 PT 走)
  PDPT、PD 同理
  返回最后一级 PT 项的地址
```

- 分配出来的表页必须**先清零**再用。
- `pmm_alloc_frame()` 返回 0(OOM)→ 返回 NULL;已分配的上级表页不回收(M8 不做)。
- 任何一级遇到 `PS=1` 都返回 NULL(`vm_map` 据此拒绝)。
- `invlpg(va)` 内联汇编:`invlpg (%0)`。

- [ ] **Step 3: `vm_map` / `vm_unmap`(🔴)**

`vm_map` 规则:
- `va`、`pa` 必须 4KB 对齐;
- `pa < PMM_MAX_PHYS`;
- **拒绝 `va < PMM_MAX_PHYS`(低 1GB)**:恒等映射区只允许 2MB 大页,不做拆分;
- 目标 PTE 已存在 → 返回 -1(不静默覆盖);
- 写入 `(pa & VM_ADDR_MASK) | flags`,然后 `invlpg(va)`。

`vm_unmap`:
- 找不到或 P=0 → 返回 0;
- 记录旧 `pa`,`*pte = 0`,`invlpg(va)`,返回旧 `pa`;
- **不释放物理帧** —— 帧的归属由调用者决定(堆的帧由按需分页分配,暂不回收)。

- [ ] **Step 4: 自检(🟡)**

1. `frame = pmm_alloc_frame()`;`vm_map(kernel_space, VM_SCRATCH_BASE, frame, P|RW)` 返回 0;
2. `volatile uint64_t *p = (uint64_t *)VM_SCRATCH_BASE; *p = 0x1122334455667788;` 读回比对;
3. `vm_unmap` 返回 `frame`;
4. 大页拒绝:`vm_map(kernel_space, 0x100000, frame, P|RW)` 返回 -1;
5. 重复映射:`vm_map` 同一 VA 返回 -1;
6. 释放 `frame`。

- [ ] **Step 5: 验收**

```bash
make && make iso
{ sleep 8
  echo 'info mem'; sleep 0.5
  echo 'memsave 0xb8000 4000 build/m8_t3.bin'; sleep 0.5
  echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256 \
  | tee build/m8_t3_monitor.txt
python3 tools/decode_vga.py build/m8_t3.bin
```

Expected:
- 屏幕:`M8/VM: map PASS`、`unmap PASS`、`huge-page reject PASS`、`double-map reject PASS`;
- `info mem` 在自检运行期间/之后不再列出 `VM_SCRATCH_BASE`(自检最后 unmap 了);若想看到映射,把 `info mem` 放在 unmap 之前,或临时加一个不 unmap 的探针;
- 写读值一致。

- [ ] **Step 6: Commit**

```bash
git add src/vm.h src/vm.c src/kernel.c Makefile
git commit -m "M8: 页表遍历器 + vm_map/vm_unmap + TLB 失效"
```

---

## Task 4: 地址空间 + CR3 切换

> 🔴 作者写空间创建与切换;🟡 自检。

**Files:**
- Modify: `src/vm.c`、`src/vm.h`、`src/kernel.c`

**Interfaces:**
- Produces:
  - `page_table_t *vm_create_space(void);` —— 失败返回 NULL
  - `void vm_switch(page_table_t *space);`
  - `void vm_switch_kernel(void);`

- [ ] **Step 1: 创建空间(🔴)**

1. `pmm_alloc_frame()` 拿一帧作为 PML4;
2. 清零;
3. `space->entry[0] = kernel_space->entry[0];`(共享恒等映射);
4. `for (i = 256; i < 512; i++) space->entry[i] = kernel_space->entry[i];`(共享内核高半区);
5. **不复制 `entry[128]`** —— 这是每空间独立区。

- **为什么必须复制**:不复制 `entry[0]`,切 CR3 后第一条取指就 `#PF`;此时 IDT 的门指向的内核代码也不在映射里 → `#DF` → triple fault。

- [ ] **Step 2: 切换(🔴)**

- `vm_switch(space)`:写 CR3;写 CR3 会刷新全部非全局 TLB 项。
- `vm_switch_kernel()`:`vm_switch(kernel_space)`。

- [ ] **Step 3: 自检(🟡)**

1. `a = vm_create_space(); b = vm_create_space();`
2. `fa = pmm_alloc_frame(); fb = pmm_alloc_frame();`
3. `vm_map(a, VM_SPACE_TEST_BASE, fa, P|RW); vm_map(b, VM_SPACE_TEST_BASE, fb, P|RW);`
4. `vm_switch(a); *(volatile uint64_t *)VM_SPACE_TEST_BASE = 0xAAAAAAAAAAAAAAAA;`
5. `vm_switch(b); *(volatile uint64_t *)VM_SPACE_TEST_BASE = 0xBBBBBBBBBBBBBBBB;`
6. `vm_switch(a);` 读回,必须是 `0xAAAA...`;
7. `vm_switch(b);` 读回,必须是 `0xBBBB...`;
8. `vm_switch_kernel();`
9. 打印 PASS/FAIL。测试帧允许泄漏(没有 destroy)。

- **关键**:第 4~8 步的代码、栈、`vga_printf` 都在恒等映射区,`a`/`b` 必须共享这些映射 —— 这正是 Step 1 的复制在保证的。

- [ ] **Step 4: 验收**

```bash
make && make iso
{ sleep 8; echo 'memsave 0xb8000 4000 build/m8_t4.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256
python3 tools/decode_vga.py build/m8_t4.bin
```

Expected: `M8/VM: space A=0xAAAA... B=0xBBBB... PASS`;内核没有黑屏/重启。

- [ ] **Step 5: 负对照(推荐)**

临时不复制 `entry[0]`,重跑。Expected: 切 CR3 后立刻 triple fault(黑屏)。这证明"复制内核映射"不是形式主义。测完还原。

- [ ] **Step 6: Commit**

```bash
git add src/vm.c src/vm.h src/kernel.c
git commit -m "M8: 地址空间 + CR3 切换 + 每空间独立映射"
```

---

## Task 5: `#PF` 恢复 + 按需分页

> 🔴 作者写缺页处理器与 `idt.c` 的一行分支;🟡 自检。

**Files:**
- Modify: `src/vm.c`、`src/vm.h`
- Modify: `src/idt.c`(`isr_handler`,panic 打印之前)
- Modify: `src/kernel.c`(自检)

**Interfaces:**
- Produces:
  - `int vm_handle_page_fault(regs_t *r);` —— 1 = 已处理,0 = 交给 panic
  - `uint64_t vm_fault_count(void);`
- Consumes: `regs_t`(`idt.h`)、`read_cr2`、`pmm_alloc_frame`、`vm_map`。

**已知事实:**
- `#PF` = 向量 14;出错地址在 **CR2**,不在错误码里。
- 错误码:bit0 P(0=页不存在,1=权限);bit1 W/R;bit2 U/S;bit3 RSVD;bit4 I/D。
- CPU 压的 RIP 指向**出错的那条指令**;`iretq` 返回后会**重新执行**它 —— 这正是"补页后重试"的机制。
- `regs_t` 的 `error_code` 字段就是 CPU 压的错误码。

- [ ] **Step 1: 处理器骨架(🔴)**

规则:

```
va  = read_cr2()
err = r->error_code

若 in_fault 已置位        → 返回 0(防重入,交给 panic)
若 err bit0 == 1          → 返回 0(页存在,保护错误;M8 不处理 COW/只读)
若 err bit3 == 1          → 返回 0(保留位被写坏,页表损坏,交给 panic)
若 err bit4 == 1          → 返回 0(取指缺页)
若 va 不在 [VM_HEAP_BASE, VM_HEAP_BASE + VM_HEAP_SIZE) → 返回 0
置 in_fault
frame = pmm_alloc_frame(); 失败 → 清 in_fault,返回 0(OOM → panic)
清零 frame(逐字节/逐 qword 循环)
page = va & ~0xFFF
vm_map(vm_current_space(), page, frame, P|RW); 失败 → 释放 frame,清 in_fault,返回 0
fault_count++
清 in_fault
返回 1
```

- **必须映射到当前 CR3 指向的空间**,不能写死 `kernel_space`。若缺页发生在测试空间 A/B,写死 `kernel_space` 会"补了页但当前空间仍缺页",`iretq` 重试又缺页,来回打转。
- 不在处理器里调用 `vga_printf`(已处理的缺页不打屏)。
- 不调用可能再次缺页的堆函数。
- 若当前 IF=1,键盘 IRQ 可能嵌套进来;`keyboard_handler` 只碰环形缓冲(`.bss`,已映射),不会碰堆,安全。**若将来有 IRQ 处理器也分配内存,这条要重新审。**

- [ ] **Step 2: `idt.c` 分支(🔴)**

在 `isr_handler` 里、`EXCEPTION` 打印**之前**加:

```c
if (r->vector == 14 && vm_handle_page_fault(r)) {
    return;   /* common stub → 恢复现场 → iretq 重试出错指令 */
}
```

`vector >= 32` 的 IRQ 分支保持原样;`< 32` 的非 `#PF` 异常仍 panic。

- [ ] **Step 3: 自检(🟡)**

1. 记录 `c0 = vm_fault_count()`;
2. 取堆区最后一页 `VM_HEAP_BASE + VM_HEAP_SIZE - 4096`,用 `volatile` 指针写入并读回;
3. `c1` 应等于 `c0 + 1`;
4. 再写同一页,`c2` 应等于 `c1`(第二次不再缺页);
5. 打印 `#PF` 次数与写读结果。

- [ ] **Step 4: 验收 A —— 缺页真的发生了,而且恢复了**

```bash
make && make iso
{ sleep 8
  echo 'memsave 0xb8000 4000 build/m8_t5.bin'; sleep 0.5
  echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256 \
    -d int -D build/m8_t5_int.log
python3 tools/decode_vga.py build/m8_t5.bin
grep -n 'v=0e' build/m8_t5_int.log | head
```

Expected:
- 屏幕:`M8/VM: demand fault c0=0 c1=1 c2=1 PASS`,写读值一致;
- `-d int` 日志里至少出现一次 `v=0e`,且发生在堆区地址;
- 内核继续运行(不是 panic)。

- [ ] **Step 5: 验收 B —— 堆区外的 `#PF` 仍然 panic(M6 回归)**

把 `kernel.c` 的 `M6_CASE` 临时改成 `4`(读 `0x300000`),重跑并解码。

Expected: 仍打出 `EXCEPTION 0x0E #PF Page Fault`、`cr2=0x0000000000300000`,然后停机。测完改回 `0`。

- [ ] **Step 6: 验收 C —— 键盘回归**

`sendkey h`,解码,Expected: 屏幕出现 `h`(按需缺页没有把 IRQ 路径弄坏)。

- [ ] **Step 7: Commit**

```bash
git add src/vm.c src/vm.h src/idt.c src/kernel.c
git commit -m "M8: #PF 恢复 + 堆区按需分页"
```

---

## Task 6: `kmalloc` / `kfree`

> 🔴 作者写 first-fit / 切分 / 合并 / 对齐;🟡 自检;⚪ 头文件。

**Files:**
- Create: `src/heap.h`、`src/heap.c`
- Modify: `src/kernel.c`(`heap_init()` 调用 + 自检)
- Modify: `Makefile`

**Interfaces:**
- Produces:
  - `int heap_init(void);` —— 0 成功
  - `void *kmalloc(size_t size);` —— 失败返回 NULL;`size == 0` 返回 NULL
  - `void kfree(void *ptr);` —— `NULL` 是空操作
  - `size_t heap_free_bytes(void);` —— 诊断用
  - `int heap_self_test(void);`
- Consumes: `vm.h` 的 `VM_HEAP_BASE` / `VM_HEAP_SIZE`;堆页由 Task 5 的缺页处理器在第一次写时补上。

**块布局(固定,后续任务按这个算):**

```
block_t (16 字节,16 字节对齐):
  uint32_t magic;      /* 0x48454150 = "HEAP" */
  uint32_t free;       /* 1 = 空闲, 0 = 已分配 */
  uint64_t size;       /* 数据区大小(不含块头), 16 字节对齐 */

[block_t][payload ...][block_t][payload ...] ... 直到 VM_HEAP_BASE + VM_HEAP_SIZE
```

- `next = (block_t *)((uint8_t *)b + sizeof(block_t) + b->size)`;
- 最后一个块的 `next` 正好等于 `arena_end`(哨兵靠边界判断,不额外放哨兵块);
- 返回值 = `(uint8_t *)b + sizeof(block_t)`,天然 16 字节对齐。

- [ ] **Step 1: `heap_init`(🟡)**

在 `VM_HEAP_BASE` 写一个覆盖整个 16MB 的 free 块:

```
b->magic = MAGIC; b->free = 1; b->size = VM_HEAP_SIZE - sizeof(block_t);
```

**这次写会触发第一次按需缺页** —— 这是 M8 的端到端演示:T6 的堆第一次落地,靠 T5 的处理器补页。

- [ ] **Step 2: `kmalloc`(🔴)**

```
若 size == 0 → NULL
若 size > VM_HEAP_SIZE → NULL
need = align_up(size, 16)
从 heap_start 开始遍历:
    若 b >= arena_end → 停止,返回 NULL
    若 b->magic != MAGIC → 报错停机(堆被写坏)
    若 b->free && b->size >= need:
        若 b->size >= need + sizeof(block_t) + 16:
            切分:在 b + sizeof(block_t) + need 处构造新块
            新块 size = 旧 size - need - sizeof(block_t),free = 1
            b->size = need
        b->free = 0
        返回 (uint8_t *)b + sizeof(block_t)
    next = (block_t *)((uint8_t *)b + sizeof(block_t) + b->size)
    若 next >= arena_end → 停止,返回 NULL
    b = next
```

- 不调用 `pmm_alloc_frame` —— 物理页由缺页处理器补。
- 不用 `%`,用 `& ~15` / `+15` 做对齐。

- [ ] **Step 3: `kfree`(🔴)**

```
若 ptr == NULL → 返回
b = (block_t *)((uint8_t *)ptr - sizeof(block_t))
校验 b 在 [heap_start, arena_end)、magic 正确、free == 0
若 b->free 已为 1 → 打印 "HEAP: double free" 并停机
b->free = 1
向后合并:while next < arena_end && next->magic == MAGIC && next->free:
    b->size += sizeof(block_t) + next->size
向前合并:从 heap_start 开始,沿 next 走,直到 next == b,记 prev 为 b 的前一块;
    若 prev->free && prev + sizeof(block_t) + prev->size == b:
        prev->size += sizeof(block_t) + b->size
        b = prev
```

- 合并只改 `size` 与 `free`,不搬数据。
- 越界指针/坏 magic:`vga_printf` 报错并停机(学习期宁可炸响,不要静默)。

- [ ] **Step 4: `heap_free_bytes`(🟡)**

从 `heap_start` 走到 `arena_end`,累加 `b->free ? b->size : 0`。用于自检与诊断。

- [ ] **Step 5: 自检(🟡)**

1. `a = kmalloc(16); b = kmalloc(200); c = kmalloc(1000);`
2. 检查 `a/b/c != NULL`,且三者都 16 字节对齐、互不重叠(用 `[a, a+16)`、`[b, b+200)`、`[c, c+1000)` 两两不相交做必要条件);
3. 对三者写图案并读回;
4. `kfree(b); kfree(a);`(a、b 相邻)→ 记录 `heap_free_bytes()`;
5. `d = kmalloc(200);` 应成功。合并后的块大小为 `16(a 数据) + 16(块头) + 200(b 数据) = 232`,足够 200;不合并时 a、b 各自太小,first-fit 找不到 200 的整块;
6. `kmalloc(VM_HEAP_SIZE)` 应返回 NULL(OOM);
7. 分配 `100000` 字节并每隔 4KB 写一个值,验证跨页的按需缺页(`vm_fault_count` 增加);
8. 打印 PASS/FAIL。

输出格式:

```
M8/HEAP: a=0xffff800000000010 b=... c=...
M8/HEAP: align PASS  overlap PASS  pattern PASS
M8/HEAP: coalesce PASS (free=0x... )
M8/HEAP: oom PASS
M8/HEAP: cross-page fault PASS (faults +N)
```

- [ ] **Step 6: 验收**

```bash
make && make iso
{ sleep 8; echo 'memsave 0xb8000 4000 build/m8_t6.bin'; sleep 0.3; echo quit; } \
  | qemu-system-x86_64 -cdrom build/os.iso -display none -monitor stdio -no-reboot -m 256 \
    -d int -D build/m8_t6_int.log
python3 tools/decode_vga.py build/m8_t6.bin
grep -c 'v=0e' build/m8_t6_int.log
```

Expected: 所有 PASS;`v=0e` 出现多次(堆初始化 + 跨页测试),但内核没有 panic。

- [ ] **Step 7: 负对照(必做一条)**

临时把向后合并的循环删掉,重跑自检。Expected: `coalesce`  FAIL(第 5 步 `d = kmalloc(200)` 失败)。测完还原。

- [ ] **Step 8: Commit**

```bash
git add src/heap.h src/heap.c src/kernel.c Makefile
git commit -m "M8: kmalloc/kfree (first-fit + 切分 + 合并)"
```

---

## Task 7: 收尾

**Files:**
- Modify: `计划书.md`(新增 §M8)、`README.md`、`docs/AI-使用记录.md`
- Create: `docs/学习笔记/M8-串联.md`(**作者亲手写**)
- Verify: 全仓库

- [ ] **Step 1: 零警告 + `ld` 不新增**

```bash
ls build/          # 先确认没有要留的取证文件
make clean && make 2>&1 | grep -i warning
```

Expected: 只有 M5 的两条 `ld` 警告;`gcc`/`nasm` 零警告。

- [ ] **Step 2: 全量回归**

| # | 验什么 | 命令/动作 | 期望 |
|---|---|---|---|
| 1 | 物理内存 | 正常启动 | `PMM` 全部 PASS |
| 2 | 页表 | 正常启动 | `VM map/unmap/huge-page reject` PASS |
| 3 | 地址空间 | 正常启动 | A/B 读到不同值 |
| 4 | 按需分页 | 正常启动 + `-d int` | `v=0e` 出现;`c1=c0+1`,`c2=c1` |
| 5 | 堆 | 正常启动 | `HEAP` 全部 PASS |
| 6 | 键盘 | `sendkey h` + 解码 | 屏幕出现 `h` |
| 7 | `#DE` | `M6_CASE 1` + 解码 | 仍 panic |
| 8 | 越界 `#PF` | `M6_CASE 4` + 解码 | 仍 panic,`cr2=0x300000` |
| 9 | 不同内存 | `-m 256` / `-m 512` | 帧数与可用内存相应变化 |

- [ ] **Step 3: 里程碑收尾「埋错找 bug」**

AI 只改一行、只给症状,作者定位。素材池:

| 埋法 | 症状 |
|---|---|
| `vm_map` 里删掉 `invlpg` | 映射/拆映射"看起来没生效" |
| `pmm_init` 不保留内核镜像 | 随机时刻崩;自检的地址检查先炸 |
| `vm_create_space` 不复制 `entry[0]` | 切 CR3 立刻黑屏 |
| `#PF` 处理器删掉 `in_fault` 守卫 | 处理器自己缺页 → triple fault |
| `vm_handle_page_fault` 不检查错误码 bit0 | 保护错误被当成缺页补页,症状诡异 |
| `kfree` 不做向后合并 | 大块分配失败 |
| `kmalloc` 的对齐改成 `size` 原值 | 后续块错位,图案测试失败 |

> 诚实标注:删 `invlpg` 在**全新 VA** 上可能观察不到(没有旧 TLB 项)。要暴露它,得用"先 map、再 unmap、再 map 同一 VA"的序列;测不到就记"讲得通但本环境未复现",不要假装测到。

- [ ] **Step 4: 回填文档**

- `计划书.md`:新增 §M8(把本计划 §0 的范围、布局、关键点、教训写进去);状态 ✅;
- `README.md`:进度表加 M8 行;
- `docs/AI-使用记录.md`:M8 每处 🔴 由谁写、哪里走了效率出口、AI 错在哪、尚未销的账;
- `docs/学习笔记/M8-串联.md`:**作者亲手写**。建议结构:
  1. 一次 `kmalloc` 写入的完整路径(VA → 页表 → #PF → 重试 → TLB);
  2. 物理帧分配与虚拟映射的分工;
  3. 你自己的三个"原来如此"和一个"还没懂"。

- [ ] **Step 5: Commit**

```bash
git add 计划书.md README.md docs/AI-使用记录.md docs/学习笔记/M8-串联.md
git commit -m "docs: 里程碑更新到 M8 (内存管理)"
```

---

## 分工表(默认)

| 文件/内容 | 档 | 谁写什么 |
|---|---|---|
| `boot.asm` 的 EBX 保存与 RDI 传递 | 🔴 | 作者写;AI 只给寄存器事实 |
| `mb2.c` 的 tag/mmap 遍历 | 🔴 | 作者写;AI 给格式与边界 |
| `pmm.c` 的位图、alloc/free、恒等映射扩展 | 🔴 | 作者写 |
| `vm.c` 的遍历器、map/unmap、地址空间、`#PF` | 🔴 | 作者写 |
| `heap.c` 的 first-fit/切分/合并 | 🔴 | 作者写 |
| `idt.c` 的 `#PF` 分支 | 🔴 | 作者写(一行,但概念重要) |
| `kernel.c` 的初始化序列与自检调用 | 🟡 | AI 骨架 + 注释,作者确认顺序 |
| `pmm_self_test` / `vm_self_test` / `heap_self_test` | 🟡 | AI 给检查项与输出格式,作者填 |
| 各头文件的常量与原型 | ⚪ | AI 写 |
| `Makefile` | ⚪ | AI 写 |
| `docs/学习笔记/M8-串联.md` | 🔴 | 作者亲手写 |
| 计划书/README/AI 记录 | 🟡 | AI 代笔,作者核对 |

**效率出口**:任何 🔴 都可以说"你直接写",但要报一句原档位,并记入 `docs/AI-使用记录.md` 第九节。

---

## Self-Review

**1. Spec coverage** —— 逐条对 §0:

| Spec 要求 | 落在哪个任务 |
|---|---|
| 保存 multiboot2 指针并传给 C | Task 1 Step 1 |
| 解析 memory map,取 type 1 区间 | Task 1 Step 3 |
| 信息结构在 2MB 外也能解析 | Task 1 Step 2、Review Focus #1 |
| 位图帧分配器(4KB 帧) | Task 2 Step 1/3 |
| 保留低 1MB/内核/信息结构 | Task 2 Step 1、Review Focus #2 |
| 恒等映射扩到可用 RAM(封顶 1GB) | Task 2 Step 2 |
| 帧分配/释放/自检 | Task 2 Step 4 |
| 四级页表遍历器(缺表分配) | Task 3 Step 2 |
| `vm_map`/`vm_unmap` + `invlpg` | Task 3 Step 3 |
| 拒绝在 2MB 大页区做 4KB 映射 | Task 3 Step 3/4 |
| 地址空间 + CR3 切换 + 内核映射共享 | Task 4 |
| `#PF` 恢复 + 按需分页 | Task 5 |
| 堆区外 `#PF` 仍 panic | Task 5 Step 5 |
| `kmalloc`/`kfree` + first-fit/切分/合并/对齐 | Task 6 |
| 跨页分配触发按需缺页 | Task 6 Step 5 |
| 零警告、M7/M6 回归、文档、串联笔记 | Task 0、Task 7 |

**2. Step scan** —— 无 "TBD"/"TODO";每个代码步骤给的是契约、精确常量/端口/寄存器事实、可执行验收命令与期望输出。`pmm_self_test` 等自检给了检查项与输出格式,不是完整函数体(项目约定:内核代码作者写)。

**3. Type consistency** —— `mb2_range_t`、`mb2_init`、`pmm_init(uint64_t)`、`pmm_alloc_frame -> uint64_t`、`page_table_t`、`vm_map(page_table_t*, uint64_t, uint64_t, uint64_t)`、`vm_unmap -> uint64_t`、`vm_handle_page_fault(regs_t*)`、`kmalloc(size_t) -> void*`、`kfree(void*)` 在各任务中一致。`regs_t` 沿用 M6 定义,未改字段。

**4. Review Focus** —— 五条都在所属任务里有对应验收:信息结构映射(T1 Step 2/4)、保留区(T2 Step 6 负对照)、CR3 复制(T4 Step 5 负对照)、`#PF` 重入/过滤(T5 Step 1/5)、kmalloc 合并(T6 Step 7 负对照)。

**5. Proportion** —— 本计划不提供 🔴 的完整函数体,只给接口、常量、算法契约与验收;代码量留给执行者。这是与 writing-plans 默认"每步附完整代码"的**刻意偏差**,理由见项目约定(内核代码由作者亲手写)。

**6. 已知缺口(诚实登记)**

- 没有 `vm_destroy_space` / 页表页回收:测试空间与页表帧会泄漏;M8 不处理。
- OOM 不置换:帧分配失败 → panic;页面置换不在 M8。
- 恒等映射区不支持 4KB 映射(不拆 2MB 大页)。
- 新地址空间创建后,内核新增映射不会自动同步;M8 用"先建映射、后建空间"的顺序规避。
- `NX` 未开;`CR0.WP` 未讨论;`SMAP/SMEP` 未开。
- `vm_map` 的 `VM_PTE_USER` 常量已定义但 M8 不使用(用户态不在范围)。
- `-m` 超过 1GB 时,1GB 以上物理内存被忽略;这是封顶策略,不是 bug。

---

## 执行方式

按项目约定,**执行者是作者本人**;AI 在每个任务后做验收、找 bug、写文档,不代写 🔴 代码。若某块卡住,可以:

1. 要求"逐行带写"(AI 拆成行级步骤,作者键入);
2. 说"你直接写"(AI 代写,但要在 `docs/AI-使用记录.md` 报原档位)。

每个任务完成后先跑该任务的验收,再进下一个;任何一条硬验收不过,停下来复盘,不要靠"后面会好"硬推。
