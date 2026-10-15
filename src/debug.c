/*
 * DEBUG 日志实现：printf 式格式化 → 协议 PRINT 消息（proto_log_write 广播
 * 到 HID/UART 通道）。UART 上不再有裸文本，只剩协议帧（debug_write_raw）。
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/sync.h"

#include "debug.h"
#include "usb_io.h"

// 由 SDK 分配的独占锁：协议帧 TX 串行化（uart_write_blocking 不并发）
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
    // 后续 critical_section_init（proto 通道环）领到的锁撞号 —— 撞上后
    // 临界区自锁死整个系统。debug_init 先于一切 critical_section 调用，
    // 这里必领到锁。
    uint lock_num = spin_lock_claim_unused(true);
    s_lock = spin_lock_init(lock_num);
}

void debug_write_raw(const uint8_t *buf, uint32_t len)
{
    if (!s_lock || !buf) return;
    uint32_t save = spin_lock_blocking(s_lock);
    uart_write_blocking(DEBUG_UART, buf, len);
    spin_unlock(s_lock, save);
}

void debug_log(const char *fmt, ...)
{
    if (!s_lock) return;                 // boot 极早期（DEBUG_UART 未初始化）

    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(msg) - 1) n = (int)sizeof(msg) - 1;

    usb_log_write((const uint8_t *)msg, (uint16_t)n);   // PRINT 广播（非阻塞）
}

int tu_printf(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) return n;
    if (n >= (int)sizeof(msg)) n = (int)sizeof(msg) - 1;

    // TinyUSB 自带日志已含 CRLF；只加前缀不补换行，避免日志空行
    char line[272];
    int prefix = snprintf(line, sizeof(line), "[TUSB] ");
    if (prefix < 0) return 0;
    if (prefix + n >= (int)sizeof(line)) n = (int)sizeof(line) - prefix - 1;
    memcpy(line + prefix, msg, (size_t)n);
    usb_log_write((const uint8_t *)line, (uint16_t)(prefix + n));
    return n;
}
