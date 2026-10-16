#include <string.h>

#include "pico/time.h"
#include "tusb.h"

#include "usb_io.h"
#include "proto.h"
#include "uart_proto.h"
#include "debug.h"

/*
 * usb_io — WinUSB vendor 接口传输适配器（通道核心见 proto.c）。
 *
 * vendor 接口（class 0xFF + bulk IN/OUT，描述符与 MS OS 1.0 绑定见
 * usb_descriptors.c）是可靠字节流，帧封装与 UART 适配器（uart_proto.c）
 * 同构：SLIP 转义 + CRC16-CCITT。数据经 TinyUSB vendor FIFO
 * （CFG_TUD_VENDOR_RX/TX_BUFSIZE）缓冲，usb_io_task 收发。
 *
 * USB bulk 可在每帧内排多个 64B 事务，吞吐远高于 HID 中断端点的
 * 1ms 一包限制（~1MB/s vs 63KB/s）。
 */

// SLIP 控制字节
#define SLIP_END     0xC0u
#define SLIP_ESC     0xDBu
#define SLIP_ESC_END 0xDCu
#define SLIP_ESC_ESC 0xDDu

// 原始帧 = [seq][chunk ≤62] + CRC16(2)；解码缓冲取 2 倍转义裕量
#define RAW_FRAME_MAX 66u
#define DECODED_MAX   (2u * RAW_FRAME_MAX)

// CRC16-CCITT-FALSE（0x1021 / 0xFFFF），逐位实现（帧 ≤140B，开销可忽略）
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

// ---- SLIP 解码状态（core0 usb_io_task 上下文独占）----
static struct {
    bool     in_frame;
    bool     escape;
    uint16_t len;
    uint8_t  buf[DECODED_MAX];
} s_slip;

static proto_chan_t s_chan;
static uint8_t s_seq;

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
    if (crc_calc != crc_recv) return;    // 应用级异常帧，静默丢弃

    // buf = [seq][flags][data...] → 剥 seq 喂通道核心
    proto_chan_rx(&s_chan, s_slip.buf + 1, (uint16_t)(len - 3u));
}

static void slip_process_byte(uint8_t b)
{
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
        slip_reset();                    // 超长：异常，重同步
        return;
    }
    s_slip.buf[s_slip.len++] = b;
}

// ---- 通道 + 传输操作表 ----

static bool winusb_ready(proto_chan_t *ch)
{
    (void)ch;
    return tud_vendor_write_available() > 0u;   // TX FIFO 有空位
}

static bool winusb_send_chunk(proto_chan_t *ch, const uint8_t *chunk, uint16_t len)
{
    (void)ch;
    if (len > PROTO_CHUNK_DATA_MAX + 1u) return false;

    // raw = [seq][chunk] + CRC16 LE → SLIP 转义
    uint8_t raw[RAW_FRAME_MAX];
    uint16_t rlen = 0;
    raw[rlen++] = s_seq;
    memcpy(raw + rlen, chunk, len);
    rlen = (uint16_t)(rlen + len);
    uint16_t crc = crc16_update(CRC16_INIT, raw, rlen);
    raw[rlen++] = (uint8_t)(crc & 0xFFu);
    raw[rlen++] = (uint8_t)(crc >> 8);

    uint8_t frame[2u * RAW_FRAME_MAX + 2u];
    uint16_t flen = 0;
    frame[flen++] = SLIP_END;
    for (uint16_t i = 0; i < rlen; i++) {
        uint8_t b = raw[i];
        if (b == SLIP_END) {
            frame[flen++] = SLIP_ESC;
            frame[flen++] = SLIP_ESC_END;
        } else if (b == SLIP_ESC) {
            frame[flen++] = SLIP_ESC;
            frame[flen++] = SLIP_ESC_ESC;
        } else {
            frame[flen++] = b;
        }
    }
    frame[flen++] = SLIP_END;

    // FIFO 空间不足会部分写入撕裂帧：空间够才发
    if (tud_vendor_write_available() < flen) return false;
    if (tud_vendor_write(frame, flen) != flen) return false;
    ++s_seq;
    // TinyUSB only auto-flushes once the FIFO reaches the endpoint packet size.
    // A short SCRIPT ACK (or final frame tail) must not wait for later logs to
    // fill 64 bytes. If the endpoint is busy, its completion flushes the FIFO.
    tud_vendor_write_flush();
    return true;
}

static const proto_transport_t s_winusb_tr = {
    .name       = "winusb",
    .ready      = winusb_ready,
    .send_chunk = winusb_send_chunk,
};

// ---- 对外 API ----

void usb_io_init(void)
{
    proto_init();
    proto_chan_init(&s_chan, &s_winusb_tr);
    proto_register_channel(&s_chan);
    uart_proto_init();          // UART 传输平级注册（见 uart_proto.c）
    DEBUG("[DS5C][IO] winusb transport ready\n");
}

void usb_io_set_instance(uint8_t instance)
{
    usb_io_reset_session();
    s_chan.active = (instance != USB_IO_INST_NONE);
    DEBUG("[DS5C][IO] winusb io=%d active=%d\n", instance, (int)s_chan.active);
}

void usb_io_reset_session(void)
{
    proto_chan_reset(&s_chan);
    slip_reset();
    s_seq = 0;
}

uint8_t usb_io_instance(void)
{
    return s_chan.active ? 0u : USB_IO_INST_NONE;   // 仅作启用标志（无 HID 实例号语义）
}

bool usb_io_enabled(void)
{
    return s_chan.active;
}

void usb_log_write(const uint8_t *data, uint16_t len)
{
    proto_log_write(data, len);     // 广播：WinUSB（启用时）+ UART
}

void usb_io_task(void)
{
    // RX：vendor FIFO → SLIP 逐字节解码
    uint8_t buf[64];
    uint32_t n;
    while ((n = tud_vendor_read(buf, sizeof buf)) > 0u) {
        for (uint32_t i = 0; i < n; i++) slip_process_byte(buf[i]);
    }

    // TX：通道任务（环消息 + 流式压测）
    proto_chan_task(&s_chan);
}
