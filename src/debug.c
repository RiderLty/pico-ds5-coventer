/*
 * 串口调试输出实现。
 *
 * printf 路径：debug_printf → 自旋锁 → vsnprintf → uart_write_blocking。
 * 默认只在冷路径调用（挂载/拔出/握手状态机/错误）；TinyUSB debug 可增加高频日志。
 * 正常日志调用频率低，
 * 阻塞式写出在这里可接受，不值得引入环形缓冲/IRQ 复杂度。
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/sync.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#include "debug.h"

// 由 SDK 分配的独占锁，兼容软件/硬件自旋锁实现
static spin_lock_t *s_lock;

void debug_init(void)
{
    uart_init(DEBUG_UART, DEBUG_BAUDRATE);
    // 引脚复用必须用 UART_FUNCSEL_NUM：RP2350 上 GPIO2/3 的 UART0 功能在
    // FUNCSEL 11（GPIO_FUNC_UART_AUX），GPIO_FUNC_UART（FUNCSEL 2，RP2040
    // 时代的写法）在 RP2350 上根本不是 UART —— 硬编码导致 TX 完全无输出
    gpio_set_function(DEBUG_TX_PIN, UART_FUNCSEL_NUM(DEBUG_UART, DEBUG_TX_PIN));
    gpio_set_function(DEBUG_RX_PIN, UART_FUNCSEL_NUM(DEBUG_UART, DEBUG_RX_PIN));
    uart_set_hw_flow(DEBUG_UART, false, false);
    uart_set_format(DEBUG_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(DEBUG_UART, true);
    // RP2350 pad 复位无上下拉：RX 悬空会收随机字节（uart_init 已置 RXE），
    // 上拉保持空闲线为高（pico-hid-debugger 的教训）
    gpio_pull_up(DEBUG_RX_PIN);

    // 从 free 池正式领取一把锁：spin_lock_init(固定 ID) 不入池登记，会与
    // 后续 critical_section_init（input_queue）领到的锁撞号 —— 撞上后
    // input_queue_flush 在临界区内打丢弃日志就自锁死整个系统。
    // debug_init 先于一切 critical_section 调用，这里必领到锁。
    uint lock_num = spin_lock_claim_unused(true);
    s_lock = spin_lock_init(lock_num);

    debug_puts("\n[DS5C] boot\n");
}

static void write_all(const uint8_t *s, uint32_t len)
{
    uint32_t save = spin_lock_blocking(s_lock);
    uart_write_blocking(DEBUG_UART, (const uint8_t *)s, len);
    spin_unlock(s_lock, save);
}

void debug_puts(const char *s)
{
    if (!s_lock || !s) return;
    write_all((const uint8_t *)s, (uint32_t)strlen(s));
}

void debug_write_raw(const uint8_t *buf, uint32_t len)
{
    if (!s_lock || !buf) return;
    write_all(buf, len);
}

void debug_printf(const char *fmt, ...)
{
    if (!s_lock) return;

    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(msg) - 1) n = (int)sizeof(msg) - 1;

    write_all((const uint8_t *)msg, (uint32_t)n);
}

int tu_printf(const char *fmt, ...)
{
    if (!s_lock || !fmt) return 0;

    char msg[256];
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) return n;
    if (n >= (int)sizeof(msg)) n = (int)sizeof(msg) - 1;

    // TinyUSB 自带日志已经包含 CRLF；只加前缀，不额外补换行，避免日志空行。
    char line[272];
    int prefix = snprintf(line, sizeof(line), "[TUSB] ");
    if (prefix < 0) return 0;
    if (prefix + n >= (int)sizeof(line)) n = (int)sizeof(line) - prefix - 1;
    memcpy(line + prefix, msg, (size_t)n);
    write_all((const uint8_t *)line, (uint32_t)(prefix + n));
    return n;
}
