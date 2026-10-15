#include <string.h>

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/platform.h"
#include "pico/time.h"

#include "uart_proto.h"
#include "proto.h"
#include "debug.h"       // DEBUG_UART / debug_write_raw

/*
 * UART 传输适配器。RX 架构移植自 pico-hid-mapper/src/uart.c（DMA 环形接收，
 * 零 RX 中断）；帧封装为 SLIP 风格转义 + CRC16：
 *
 *   线上帧 = 0xC0 <转义([seq][flags][data] + CRC16 LE)> 0xC0
 *   转义：0xC0 → DB DC，0xDB → DB DD（RFC 1055 SLIP）
 *
 * 选 SLIP 而非 COBS：实现无边界歧义、逐字节状态机简单，重同步特性相同
 * （分隔字节即帧界）。调试日志为 ASCII（不含 0xC0/0xDB），与协议帧混流时
 * 日志文本落在帧外被解码器自然忽略；线上垃圾帧由 CRC16 淘汰。
 */

#define UART_RX_RING_SIZE  4096u
#if (UART_RX_RING_SIZE & (UART_RX_RING_SIZE - 1u)) != 0
#error "UART_RX_RING_SIZE must be a power of two (DMA ring wrap)"
#endif
#define UART_RX_RING_BITS  (__builtin_ctz(UART_RX_RING_SIZE))

#define UART_RX_FRAME_TIMEOUT_US 50000u   // 半帧超时：主机断发后尽快重同步

// SLIP 控制字节
#define SLIP_END     0xC0u
#define SLIP_ESC     0xDBu
#define SLIP_ESC_END 0xDCu
#define SLIP_ESC_ESC 0xDDu

// TX 原始帧最大长 = seq + 块(1+61) + CRC16(2)；RX 解码缓冲再留裕量
#define RAW_FRAME_MAX     (2u + PROTO_CHUNK_DATA_MAX + 1u + 2u)
#define DECODED_MAX       RAW_FRAME_MAX

static uint8_t  uart_rx_ring[UART_RX_RING_SIZE] __attribute__((aligned(UART_RX_RING_SIZE)));
static uint32_t uart_rx_tail;
static volatile uint32_t rx_overflow_count;
static volatile uint32_t rx_error_count;
#define UART_RX_DMA 15

// ---- SLIP 解码状态（core0 uart_proto_task 上下文独占）----
static struct {
    bool     in_frame;
    bool     escape;
    uint16_t len;
    uint8_t  buf[DECODED_MAX];
    uint64_t last_byte_us;
} s_slip;

static proto_chan_t g_uart_chan;
static uint8_t s_tx_seq;

// ---- CRC16-CCITT-FALSE（0x1021 / 0xFFFF），逐位实现（帧 ≤70B，开销可忽略）
#define CRC16_INIT 0xFFFFu

static uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// ---- SLIP 编解码 ----

// raw → out：0xC0 + 转义载荷 + 0xC0；返回总长
static uint16_t slip_encode(const uint8_t *raw, uint16_t len, uint8_t *out)
{
    uint16_t n = 0;
    out[n++] = SLIP_END;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = raw[i];
        if (b == SLIP_END) {
            out[n++] = SLIP_ESC;
            out[n++] = SLIP_ESC_END;
        } else if (b == SLIP_ESC) {
            out[n++] = SLIP_ESC;
            out[n++] = SLIP_ESC_ESC;
        } else {
            out[n++] = b;
        }
    }
    out[n++] = SLIP_END;
    return n;
}

static void slip_reset(void)
{
    s_slip.in_frame = false;
    s_slip.escape = false;
    s_slip.len = 0;
}

// 出帧：CRC16 校验 → 剥 seq → 通道核心
static void slip_finish_frame(void)
{
    s_slip.in_frame = false;

    uint16_t len = s_slip.len;
    if (len < 4u) return;                // seq + flags + CRC16 最少 4B

    uint16_t crc_calc = crc16_update(CRC16_INIT, s_slip.buf, (uint16_t)(len - 2u));
    uint16_t crc_recv = (uint16_t)(s_slip.buf[len - 2u] | (s_slip.buf[len - 1u] << 8));
    if (crc_calc != crc_recv) return;    // 噪声帧 / 日志垃圾，静默丢弃

    // buf = [seq][flags][data...] → 剥 seq 喂通道核心
    proto_chan_rx(&g_uart_chan, s_slip.buf + 1, (uint16_t)(len - 3u));
}

// 逐字节喂解码器
static void slip_process_byte(uint8_t b)
{
    s_slip.last_byte_us = time_us_64();

    if (b == SLIP_END) {
        if (s_slip.in_frame && s_slip.len > 0u) slip_finish_frame();
        slip_reset();
        return;
    }
    if (!s_slip.in_frame) {
        slip_reset();
        s_slip.in_frame = true;
    }
    if (s_slip.escape) {
        s_slip.escape = false;
        if (b == SLIP_ESC_END) b = SLIP_END;
        else if (b == SLIP_ESC_ESC) b = SLIP_ESC;
        else { slip_reset(); return; }   // 非法转义，重同步
    } else if (b == SLIP_ESC) {
        s_slip.escape = true;
        return;
    }
    if (s_slip.len >= DECODED_MAX) {
        slip_reset();                    // 超长：噪声/垃圾，重同步
        return;
    }
    s_slip.buf[s_slip.len++] = b;
}

// ---- 通道 + 传输操作表 ----

static bool uart_ready(proto_chan_t *ch)
{
    (void)ch;
    return true;                         // TX 为持锁阻塞写，恒可发
}

static bool uart_send_chunk(proto_chan_t *ch, const uint8_t *chunk, uint16_t len)
{
    (void)ch;
    if (len > PROTO_CHUNK_DATA_MAX + 1u) return false;

    // raw = [seq][chunk] + CRC16 LE → SLIP 转义
    uint8_t raw[RAW_FRAME_MAX];
    uint16_t rlen = 0;
    raw[rlen++] = s_tx_seq++;
    memcpy(raw + rlen, chunk, len);
    rlen = (uint16_t)(rlen + len);
    uint16_t crc = crc16_update(CRC16_INIT, raw, rlen);
    raw[rlen++] = (uint8_t)(crc & 0xFFu);
    raw[rlen++] = (uint8_t)(crc >> 8);

    uint8_t frame[2u * RAW_FRAME_MAX + 2u];
    uint16_t flen = slip_encode(raw, rlen, frame);
    debug_write_raw(frame, flen);        // 持 debug 锁，防日志交错撕裂帧
    return true;
}

static const proto_transport_t s_uart_tr = {
    .name       = "uart",
    .ready      = uart_ready,
    .send_chunk = uart_send_chunk,
};

// ---- 任务（RX 追指针 + 通道 TX）----

void uart_proto_task(void)
{
    uart_hw_t *hw = uart_get_hw(DEBUG_UART);

    // 通道守护兼首启：self-trigger 模式下正常永不停机；异常停机则完整重装
    if (!dma_channel_is_busy(UART_RX_DMA)) {
        dma_channel_set_read_addr(UART_RX_DMA, &uart_get_hw(DEBUG_UART)->dr, false);
        dma_channel_set_write_addr(UART_RX_DMA, uart_rx_ring, false);
        dma_channel_set_transfer_count(
            UART_RX_DMA, dma_encode_transfer_count_with_self_trigger(UART_RX_RING_SIZE), false);
        dma_channel_start(UART_RX_DMA);
    }

    // 接收错误（FE/PE/BE）：计数并清除；出错字节仍进环按普通字节消费
    uint32_t rsr = hw->rsr & UART_UARTRSR_BITS;
    if (rsr != 0u) {
        rx_error_count++;
        hw->rsr = rsr;
    }

    // 读指针追 DMA 写指针；gap 超环半 = 消费端停摆被写穿 → 跳写指针复位解析
    uint32_t wpos = (uint32_t)(dma_hw->ch[UART_RX_DMA].write_addr - (uintptr_t)uart_rx_ring);
    uint32_t gap = (wpos - uart_rx_tail) & (UART_RX_RING_SIZE - 1u);
    if (gap >= UART_RX_RING_SIZE / 2u) {
        rx_overflow_count += gap;
        uart_rx_tail = wpos;
        slip_reset();
        return;
    }

    while (uart_rx_tail != wpos) {
        slip_process_byte(uart_rx_ring[uart_rx_tail]);
        uart_rx_tail = (uart_rx_tail + 1u) & (UART_RX_RING_SIZE - 1u);
    }

    // 半帧超时：主机断发留下半帧时尽快放弃重新同步
    if (s_slip.in_frame && s_slip.last_byte_us != 0u &&
        time_us_64() - s_slip.last_byte_us >= UART_RX_FRAME_TIMEOUT_US) {
        slip_reset();
    }

    proto_chan_task(&g_uart_chan);
}

void uart_proto_init(void)
{
    uart_rx_tail = 0;
    rx_overflow_count = 0;
    rx_error_count = 0;
    slip_reset();
    s_tx_seq = 0;

    proto_chan_init(&g_uart_chan, &s_uart_tr);
    g_uart_chan.active = true;           // UART 恒可用（RX 噪声由 CRC 兜底）
    proto_register_channel(&g_uart_chan);

    // RX DMA：读 = UART DR（定址），写 = 环形缓冲（地址回绕），DREQ 在 FIFO
    // 非空期间持续有效 → 字节一到即被搬走。延迟启动：boot 期 flash 操作期间
    // 通道空闲，由 uart_proto_task 的守护在就绪后触发。
    dma_channel_config c = dma_channel_get_default_config(UART_RX_DMA);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, uart_get_dreq(DEBUG_UART, false));
    channel_config_set_ring(&c, true /* 写地址回绕 */, UART_RX_RING_BITS);
    channel_config_set_irq_quiet(&c, true);
    dma_channel_configure(UART_RX_DMA, &c, uart_rx_ring,
                          &uart_get_hw(DEBUG_UART)->dr,
                          dma_encode_transfer_count_with_self_trigger(UART_RX_RING_SIZE),
                          false);        // 不立即启动（见上）

    // RX 中断全部关闭（DMA 搬运不需要）；TX 本就无中断（阻塞写）
    uart_set_irqs_enabled(DEBUG_UART, false, false);
    irq_set_enabled(UART_IRQ_NUM(DEBUG_UART), false);

    debug_printf("[DS5C][IO] uart transport ready (dma rx)\n");
}

uint32_t uart_proto_drops(void)
{
    return g_uart_chan.drops;
}
