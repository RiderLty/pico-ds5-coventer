#ifndef UART_PROTO_H
#define UART_PROTO_H

#include <stdint.h>

/*
 * uart_proto — 调试串口（UART0，4Mbaud）传输适配器（协议 v1，通道核心见
 * proto.c）。与 WinUSB 平级承载同一套 INFO/PRINT/RESET/TEST 协议。
 *
 * RX：DMA 环形接收（移植 pico-hid-mapper/src/uart.c 成熟方案，零 RX 中断），
 * 逐字节喂 SLIP 增量解码器；帧 = 0xC0 + SLIP([seq][flags][data] +
 * CRC16-CCITT) + 0xC0。线上只有协议帧，日志经 PRINT 消息传送。
 *
 * TX：块 + CRC16 → SLIP 编码 → debug_write_raw（持 debug 锁，防日志交错
 * 撕裂帧）。
 */

// 初始化（双核启动前调用）：DMA RX 通道延迟启动（task 守护负责首启）
void uart_proto_init(void);

// core0 主循环调用：RX 解析 + 通道 TX
void uart_proto_task(void);

// UART 通道消息环丢弃计数（诊断用）
uint32_t uart_proto_drops(void);

typedef struct {
    uint32_t ring_discard_bytes;
    uint32_t uart_errors;
    uint32_t crc_errors;
    uint32_t frame_errors;
    uint32_t seq_gaps;
    uint32_t dma_restarts;
} uart_proto_stats_t;

// core0 调用；INFO 查询包含这些计数，用于区分链路错误与协议/网页误判。
uart_proto_stats_t uart_proto_stats(void);

#endif // UART_PROTO_H
