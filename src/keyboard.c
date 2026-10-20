/*
 * 来源声明 —— 详见 docs/AI-使用记录.md
 *   AI 生成: 2026-10-20, M7 Task 5。原档位 🔴(scancode 状态机 + 环形缓冲两端)
 *            / ⚪(两张 scancode 查表)。队员走出效率出口「直接做」,
 *            故主体由 AI 写; 逐段注释保留设计理由。
 *   队员验证: Task 5 端到端验收 —— hi / shift-h → H / 退格 / tab / 回车;
 *             以及 M6 的 #DE 回归。
 *
 * 中断上下文只做四件事:
 *   1. 读 0x60(必须读, 否则 8042 不会再抬高 IRQ1);
 *   2. 处理状态机(release 位 / 0xE0 前缀 / Shift);
 *   3. 把字符放进环形缓冲(满了就丢新字符);
 *   4. 返回 —— EOI 由 irq_dispatch 在 handler 返回后统一发。
 *
 * **这里绝不调用 vga_***: VGA 慢, 而且主循环也在用它, 中断重入会打架。
 * 屏幕输出全部留给主循环(下半部)。
 */

#include "keyboard.h"
#include "irq.h"
#include "io.h"

_Static_assert(KB_BUF_SIZE >= 2, "环形缓冲至少要有 2 格");
_Static_assert((KB_BUF_SIZE & (KB_BUF_SIZE - 1)) == 0,
               "KB_BUF_SIZE 必须是 2 的幂: 回绕用 & (SIZE-1) 而不是 %");

/*
 * SPSC 环形缓冲:
 *   - kb_head 只在中断里写(生产者);
 *   - kb_tail 只在主循环里写(消费者);
 *   - kb_buf 先写数据、再推进 head; 先读数据、再推进 tail。
 *
 * **volatile 省不掉**: 编译器看不到"中断会改 kb_head", 在 -O2 下完全可能
 * 把主循环里的 kb_head == kb_tail 判断提到循环外, 症状是"中断明明在跑,
 * 主循环像没看见"。加 volatile 后每次访问都真的去内存读。
 *
 * 单核 SPSC 下不需要 cli 关中断: 对一个人对齐的 unsigned 读写是原子的,
 * 而且生产者和消费者各写各的索引, 不会同时写同一个变量。
 * 两个生产者、或索引与数据不是"先数据后索引"的关系时, 才需要临界区。
 */
static volatile char     kb_buf[KB_BUF_SIZE];
static volatile unsigned kb_head;      /* 写位置: 中断写 */
static volatile unsigned kb_tail;      /* 读位置: 主循环写 */

/* Shift 的按下状态。左右各记一个, 避免"按住两个 Shift 再松一个"被误判成全松。 */
static volatile int shift_left;
static volatile int shift_right;

/* 0xE0 前缀: 上一字节是扩展键引导码, 本字节要整体吞掉。 */
static int e0_prefix;

/*
 * scancode set 1 的两张查表(纯数据, ⚪)。
 * 没列出的下标默认是 0 —— 也就是"这个键不产生字符"(功能键、Ctrl/Alt、
 * CapsLock、数字小键盘部分键等)。**必须显式映射为 0, 不能读到表外。**
 * 数组长度固定 128, code 已用 & 0x7F 限过范围。
 */
static const char kb_normal[128] = {
    [0x01] = 27,                                    /* Esc */
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x37] = '*', [0x39] = ' ',
    /* 数字小键盘(由 0xE0 前缀区分的那部分不在这里) */
    [0x47] = '7', [0x48] = '8', [0x49] = '9', [0x4A] = '-',
    [0x4B] = '4', [0x4C] = '5', [0x4D] = '6', [0x4E] = '+',
    [0x4F] = '1', [0x50] = '2', [0x51] = '3', [0x52] = '0', [0x53] = '.',
};

static const char kb_shift[128] = {
    [0x01] = 27,
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?', [0x37] = '*', [0x39] = ' ',
};

void keyboard_init(void) {
    /* 只做注册。真正的芯片初始化(重映射/解屏蔽)已经在 pic.c 里做完了。 */
    irq_register(1, keyboard_handler);
}

void keyboard_handler(regs_t *r) {
    (void)r;

    /*
     * 第一步永远是读 0x60 —— 不读, 8042 的输出缓冲一直是满的,
     * 它就不会再产生新的 IRQ1。症状是"只收到第一个键"。
     */
    uint8_t sc = inb(0x60);

    /*
     * 0xE0 前缀: 方向键、右 Ctrl/Alt、小键盘的一部分等扩展键,
     * 先发一个 0xE0, 再发真正的键码。现在的策略是整对吞掉 ——
     * 不支持扩展键, 但绝不能把第二字节当成独立 make code 去查表。
     */
    if (e0_prefix) {
        e0_prefix = 0;
        return;
    }
    if (sc == 0xE0) {
        e0_prefix = 1;
        return;
    }

    /* bit7 = 1 表示松键(release): 更新状态, 但不产生字符。 */
    int     release = (sc & 0x80) != 0;
    uint8_t code    = (uint8_t)(sc & 0x7F);

    /* 左右 Shift 分别记状态; 松键也要更新, 否则 Shift 会"卡住"。 */
    if (code == 0x2A) { shift_left  = !release; return; }
    if (code == 0x36) { shift_right = !release; return; }

    if (release) {
        return;                 /* 其他键松开: 不产生字符 */
    }

    /* code 已 <= 0x7F, 下标安全; 查到的 0 表示"这个键没有字符"。 */
    char c = (shift_left || shift_right) ? kb_shift[code] : kb_normal[code];
    if (c == '\0') {
        return;
    }

    /* 满时丢**新**字符: 先算 next, 只有 next != tail 才写。不覆盖旧数据。 */
    unsigned next = (kb_head + 1) & (KB_BUF_SIZE - 1);
    if (next == kb_tail) {
        return;
    }
    kb_buf[kb_head] = c;
    kb_head = next;             /* 数据先写, 索引后推 */
}

int kb_get(void) {
    if (kb_head == kb_tail) {
        return -1;              /* 空。不能用 0 表示空: '\0' 是合法字符 */
    }

    char c = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) & (KB_BUF_SIZE - 1);
    return (unsigned char)c;    /* 避免 char 为有符号时把高位字符变成负数 */
}
