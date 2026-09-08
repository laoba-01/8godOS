# 8GodOS

一个从零实现的 x86_64 操作系统内核。目标:引导 → 进入 64 位长模式 → 在 VGA 屏幕上打印 `Hello, kernel!`。

## 当前进度

| 里程碑 | 内容 | 状态 |
|--------|------|------|
| M0 | 环境就绪 | ✅ 完成 |
| M1 | 能引导(Grub + Multiboot2 加载内核) | ✅ 完成 |
| M2 | 进入 64 位长模式 | 🚧 进行中 |
| M3 | VGA 打印 `Hello, kernel!` | ⏳ |
| M4 | 一键构建脚本 | ⏳ |

详细技术方案与路线图见 [`计划书.md`](计划书.md)。

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
nasm -f elf64 src/boot.asm -o build/boot.o
gcc -ffreestanding -c src/kernel.c -o build/kernel.o
ld -T src/linker.ld -o build/kernel.elf build/boot.o build/kernel.o
grub-mkrescue -o build/os.iso build/iso
qemu-system-x86_64 -cdrom build/os.iso
```

## 目录结构

```
8GodOS/
├── src/
│   ├── boot.asm      # multiboot2 头 + 入口(长模式切换进行中)
│   ├── linker.ld     # 链接脚本(内核固定在 1MB)
│   └── kernel.c      # kmain + VGA 打印(M2 加入)
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
