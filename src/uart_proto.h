#ifndef UART_PROTO_H
#define UART_PROTO_H

#include <stdint.h>

/*
 * uart_proto — 调试串口（UART0，4Mbaud）传输适配器（协议 v1，通道核心见
 * proto.c）。与 custom HID 平级承载同一套 INFO/PRINT/RESET 协议。
 *
 * RX：DMA 环形接收（移植 pico-hid-mapper/src/uart.c 成熟方案，零 RX 中断），
 * 逐字节喂 COBS 增量解码器；帧 = COBS([seq][flags][data] + CRC16-CCITT) +
 * 0x00 分隔。调试日志文本与协议帧混流：日志为 ASCII（无 0x00），解码器以
 * 0x00 重同步、垃圾帧被 CRC 淘汰——串口终端可读性保留，协议不受干扰。
 *
 * TX：块 + CRC16 → COBS 编码 → debug_write_raw（持 debug 锁，防日志交错
 * 撕裂帧）。
 */

// 初始化（双核启动前调用）：DMA RX 通道延迟启动（task 守护负责首启）
void uart_proto_init(void);

// core0 主循环调用：RX 解析 + 通道 TX
void uart_proto_task(void);

// UART 通道消息环丢弃计数（诊断用）
uint32_t uart_proto_drops(void);

#endif // UART_PROTO_H
