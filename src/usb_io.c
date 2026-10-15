#include "tusb.h"

#include "usb_io.h"
#include "proto.h"
#include "uart_proto.h"
#include "debug.h"

/*
 * usb_io — custom HID 传输适配器（协议 v1，通道核心见 proto.c）。
 *
 * 块 → 满长 HID 报文：[seq][flags][data...] 补零到 63B 载荷（短 IN 包会被
 * Windows→Chrome 链路丢弃，故恒满长）；RX 方向 usb_descriptors.c 把 OUT
 * 端点 payload（已剥 Report ID）喂给 usb_io_rx，剥掉 seq 后交给通道核心。
 * 通道核心对传输方式无感（UART 适配器见 uart_proto.c）。
 */

static uint8_t s_instance = USB_IO_INST_NONE;
static uint8_t s_seq;
static proto_chan_t s_chan;

// ---- 传输操作表 ----

static bool hid_ready(proto_chan_t *ch)
{
    (void)ch;
    return tud_hid_n_ready(s_instance);
}

static bool hid_send_chunk(proto_chan_t *ch, const uint8_t *chunk, uint16_t len)
{
    (void)ch;
    uint8_t report[IO_PAYLOAD_SIZE] = {0};
    report[0] = s_seq++;
    memcpy(report + 1, chunk, len);          // len ≤ 1 + PROTO_CHUNK_DATA_MAX = 62
    return tud_hid_n_report(s_instance, IO_REPORT_ID_IN, report, IO_PAYLOAD_SIZE);
}

static const proto_transport_t s_hid_tr = {
    .name       = "hid",
    .ready      = hid_ready,
    .send_chunk = hid_send_chunk,
};

// ---- 对外 API ----

void usb_io_init(void)
{
    proto_init();
    proto_chan_init(&s_chan, &s_hid_tr);
    proto_register_channel(&s_chan);
    uart_proto_init();          // UART 传输平级注册（见 uart_proto.c）
    debug_printf("[DS5C][IO] hid transport ready\n");
}

void usb_io_set_instance(uint8_t instance)
{
    s_instance = instance;
    s_chan.active = (instance != USB_IO_INST_NONE);
    debug_printf("[DS5C][IO] hid instance=%d active=%d\n", instance, (int)s_chan.active);
}

uint8_t usb_io_instance(void)
{
    return s_instance;
}

bool usb_io_enabled(void)
{
    return s_instance != USB_IO_INST_NONE;
}

void usb_log_write(const uint8_t *data, uint16_t len)
{
    proto_log_write(data, len);     // 广播：HID（启用时）+ UART
}

void usb_io_rx(const uint8_t *payload, uint16_t len)
{
    // OUT 端点 payload = [seq][flags][data...]（Report ID 已由 usb_descriptors.c 剥除）
    if (len < 1u) return;
    proto_chan_rx(&s_chan, payload + 1, (uint16_t)(len - 1u));
}

void usb_io_task(void)
{
    proto_chan_task(&s_chan);
}
