#ifndef DS5_DEBUG_H
#define DS5_DEBUG_H

#include <stdint.h>

/*
 * DEBUG — 设备日志唯一出口（printf 用法），走协议通道。
 *
 * 旧的"系统级 print"（裸文本直写 UART）已废弃：日志统一定期经
 * debug_log() → 协议 PRINT 子命令广播到所有激活传输（custom HID +
 * 调试串口），网页控制台/上位机统一查看。UART 线上只剩协议帧
 * （SLIP+CRC16，见 uart_proto.c），不再有人类可读文本。
 *
 * proto 未初始化前（boot 极早期）的 DEBUG 调用直接丢弃。
 * 仅持锁做环形入队，非阻塞，任何上下文（含 Lua 所在 core1）可调用。
 */

#define DEBUG(fmt, ...) debug_log(fmt, ##__VA_ARGS__)

// UART 实例与引脚（协议帧 TX/RX；RX 由 uart_proto.c 的 DMA 接收）
#define DEBUG_UART       uart0
#define DEBUG_TX_PIN     2u
#define DEBUG_RX_PIN     3u
// 4Mbaud @ 240MHz clk_sys 分频恰为整数（BRD=3.75 → IBRD=3, FBRD=48，零误差）；
// 依赖 main.c 里把 clk_peri 挂回 clk_sys（默认会被 set_sys_clock_khz 切到 48MHz
// USB PLL，4M 会超 48M/16=3M 上限被静默回退）。适配器不支持时可编译期覆盖
// （-DDEBUG_BAUDRATE=115200 等，见 src/CMakeLists.txt）
#ifndef DEBUG_BAUDRATE
#define DEBUG_BAUDRATE   4000000u
#endif

// 初始化 UART（协议帧 TX 用：引脚/波特率/锁；必须在双核启动前调用）
void debug_init(void);

// 原始字节写（线程安全，持锁）：协议帧发送专用（uart_proto.c），防帧被
// 并发写撕裂。boot 后线上不应再出现任何非帧字节。
void debug_write_raw(const uint8_t *buf, uint32_t len);

// DEBUG 的实现（一般不直接调用，用宏）
void debug_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// TinyUSB CFG_TUSB_DEBUG_PRINTF 适配：路由到 DEBUG（仅在
// -DTUSB_DEBUG_LEVEL>0 时被 TinyUSB 调用，注意高频日志会挤占协议环）
int tu_printf(const char *fmt, ...);

#endif // DS5_DEBUG_H
