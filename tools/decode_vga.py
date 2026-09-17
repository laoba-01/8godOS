#!/usr/bin/env python3
"""解码 VGA 文本模式显存 dump。

来源声明 —— 详见 docs/AI-使用记录.md
  AI 生成: 原版由 AI 于 2026-09-15 写入 build/(已随 make clean 丢失);
           2026-09-17 由 AI 重建并迁至 tools/。
  队员验证: 队员在 M5/M6 各里程碑以其输出做验收判读。

用法:
    python3 tools/decode_vga.py <dump 文件> [--attr]

输入是 QEMU monitor 取出来的裸显存:
    memsave 0xb8000 4000 build/m6.bin
即 80x25 格, 每格 2 字节、交错排列: [字符][属性] 逐格轮流(不是先 2000 字符再 2000 属性)。

输出 25 行, 每行 "<两位十六进制行号>  <该行文本>"。文本裁掉行尾空白,
不可打印字符打成 '.'(这样未初始化/被写坏的位置会露出来, 不会伪装成空白)。
加 --attr 时每行末尾附上该行出现过的属性字节。

stdout 只输出解码结果, 不混标题/进度行, 便于管道取行比对:
    python3 tools/decode_vga.py build/m6.bin | head -1
"""

import sys

COLS = 80
ROWS = 25
ROW_BYTES = COLS * 2   # 每行 80 格 x 2 字节


def decode_rows(data):
    """按行切出 (文本, 属性列表)。文本裁掉行尾空白。"""
    rows = []
    for r in range(ROWS):
        base = r * ROW_BYTES
        chars = []
        attrs = []
        for c in range(COLS):
            ch = data[base + c * 2]
            attrs.append(data[base + c * 2 + 1])
            chars.append(chr(ch) if 32 <= ch < 127 else '.')
        rows.append(("".join(chars).rstrip(), attrs))
    return rows


def main():
    args = sys.argv[1:]
    show_attr = "--attr" in args
    args = [a for a in args if a != "--attr"]

    if len(args) != 1:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    path = args[0]
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"错误: 读不了 {path}: {e}\n"
              f"      dump 文件要先用 QEMU monitor 的 memsave 生成, "
              f"QEMU 那步失败的话这里就没有文件。", file=sys.stderr)
        return 1

    need = ROWS * ROW_BYTES
    if len(data) < need:
        print(f"错误: {path} 只有 {len(data)} 字节, "
              f"解码 {ROWS}x{COLS} 文本模式需要 {need} 字节", file=sys.stderr)
        return 1

    for r, (text, attrs) in enumerate(decode_rows(data)):
        line = f"{r:02X}  {text}"
        if show_attr:
            line += "   [attr: " + " ".join(f"{a:02X}" for a in sorted(set(attrs))) + "]"
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
