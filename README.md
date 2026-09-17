# 8GodOS

一个从零实现的 x86_64 操作系统内核。目标:引导 → 进入 64 位长模式 → 在 VGA 屏幕上打印 `Hello, kernel!`。

## 当前进度

| 里程碑 | 内容 | 状态 |
|--------|------|------|
| M0 | 环境就绪 | ✅ 完成 |
| M1 | 能引导(Grub + Multiboot2 加载内核) | ✅ 完成 |
| M2 | 进入 64 位长模式 | ✅ 完成 |
| M3 | VGA 打印 `Hello, kernel!` | ✅ 完成 |
| M4 | 一键构建脚本 | ✅ 完成 |
| M5 | VGA 终端(滚动 + 硬件光标) | ✅ 完成 |
| M6 | IDT 与 CPU 异常处理 | ✅ 完成 |

详细技术方案与路线图见 [`计划书.md`](计划书.md)。

## AI 使用留痕

本项目从第一天起按**全国大学生计算机系统能力大赛操作系统设计赛**的 AI 工具披露要求留痕（8GodOS 本身是练手项目，目标是为 2027 赛季做准备）。

| 文档 | 内容 |
|------|------|
| [`docs/AI-使用记录.md`](docs/AI-使用记录.md) | 主记录：工具与模型 / 逐文件来源标注 / 逐次记录 / AI 生成内容的错误与发现方式 / 可核验性 / 更正记录 |
| [`docs/AI交流记录/`](docs/AI交流记录/) | 各次会话的可读片段（原始 `.jsonl` 另存于 `原始记录/`，不入库） |
| [`docs/AI留痕方法.md`](docs/AI留痕方法.md) | 这套留痕**方法的模板**，供新项目复用 |

> **注意**：本项目所有 git 提交的作者都是队员本人（`laoba-01`），AI 生成的代码同样以队员名义提交 —— **来源不能从 git 元数据判断**，以记录文档与各源码文件顶部的来源声明为准。

## 技术栈

| 决策项 | 选择 |
|--------|------|
| 目标架构 | x86_64 长模式 |
| 引导方式 | GRUB + Multiboot2 |
| 开发语言 | C + 少量汇编 |
| 运行环境 | QEMU |
| 开发环境 | WSL2 Ubuntu |

## 环境要求

WSL2 Ubuntu 24.04,安装以下工具:

```bash
sudo apt update
sudo apt install -y nasm qemu-system-x86 grub-pc-bin xorriso mtools
```

## 构建与运行

```bash
make          # 编译内核 + 生成 ISO(build/os.iso)
make run      # 编译 + 启动 QEMU
make clean    # 清理构建产物
```

手动方式(等价于 `make` 的各步):

```bash
# 编译(C 的参数与 Makefile 里的 CFLAGS 一致)
CFLAGS="-ffreestanding -fno-stack-protector -fno-pie -no-pie -m64 \
        -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=large \
        -Wall -Wextra -O2"
nasm -f elf64 src/boot.asm -o build/boot.o
nasm -f elf64 src/isr.asm  -o build/isr.o
gcc $CFLAGS -c src/kernel.c -o build/kernel.o
gcc $CFLAGS -c src/vga.c    -o build/vga.o
gcc $CFLAGS -c src/idt.c    -o build/idt.o

# 链接
ld -nostdlib -z max-page-size=0x1000 -T src/linker.ld \
   -o build/kernel.elf build/boot.o build/kernel.o build/vga.o \
      build/isr.o build/idt.o

# 组 ISO
mkdir -p build/iso/boot/grub
cp build/kernel.elf build/iso/boot/kernel.elf
cp grub/grub.cfg    build/iso/boot/grub/grub.cfg
grub-mkrescue -o build/os.iso build/iso

# 运行
qemu-system-x86_64 -cdrom build/os.iso
```

> 上面只是对照理解用,实际构建以 `Makefile` 为准(它是唯一事实来源,两边有出入时以 Makefile 为准)。

## 目录结构

```
8GodOS/
├── src/
│   ├── boot.asm      # multiboot2 头 + 长模式切换(32→64) + 64 位入口
│   ├── linker.ld     # 链接脚本(内核固定在 1MB)
│   ├── kernel.c      # kmain(启动逻辑) + M6 异常用例开关
│   ├── vga.c         # VGA 文本终端 + vga_printf(M5/M6)
│   ├── vga.h         # VGA 接口与调色板常量(M5 加入)
│   ├── io.h          # 端口读写 outb / inb(M5 加入)
│   ├── isr.asm       # 32 个异常 stub(宏生成) + common stub(M6 加入)
│   ├── idt.c         # IDT 表 + 门构造 + 异常名表 + panic(M6 加入)
│   └── idt.h         # 门描述符与 regs_t 栈帧结构(M6 加入)
├── tools/
│   └── decode_vga.py # 显存 dump 解码器(取证用, 故意放在 build/ 之外)
├── grub/
│   └── grub.cfg      # ISO 的 GRUB 菜单
├── build/            # 构建产物(git 忽略)
├── Makefile          # 一键 build / run / clean
├── 计划书.md          # 详细技术方案与路线图
└── README.md
```

## 参考资料

- [OSDev Wiki — Bare Bones](https://wiki.osdev.org/Bare_Bones)
- [OSDev Wiki — Multiboot2](https://wiki.osdev.org/Multiboot2)
- [OSDev Wiki — Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)
- [GRUB Multiboot2 规范](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
