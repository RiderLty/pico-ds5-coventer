#ifndef DS5_DEBUG_H
#define DS5_DEBUG_H

#include <stdint.h>

/*
 * 串口调试输出：uart0，TX=GPIO2 / RX=GPIO3，4000000 8N1。
 *
 * 双核共用一个 UART：debug_printf() 内部用 SDK 自旋锁序列化，
 * 跨核并发调用不会撕裂。日志只打冷路径（挂载/拔出/握手/错误），
 * 默认不随每报文热路径刷屏；开启 TinyUSB debug 后会增加传输日志。
 */

// UART 实例与引脚（RX 当前未接，仅保留配置）
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

// 初始化（core0 main() 最先调用，先于 core1 启动）
void debug_init(void);

// 格式化输出（线程安全）。fmt 同 printf；日志自带换行由调用方控制
void debug_printf(const char *fmt, ...);

// TinyUSB CFG_TUSB_DEBUG_PRINTF 适配函数。TinyUSB 要求 printf 兼容的 int 返回值，
// 不能直接使用返回 void 的 debug_printf()。
int tu_printf(const char *fmt, ...);

// 原始字符串输出（线程安全）
void debug_puts(const char *s);

#endif // DS5_DEBUG_H
