#include "tusb.h"

#include "usb_io.h"

static uint8_t s_instance = USB_IO_INST_NONE;   // BSS = 未启用；仅重枚举窗口内切换

// OUT 端点报文 stash（set_report_cb 与 task 都在 core0，无需并发保护）
static uint8_t s_payload[IO_PAYLOAD_SIZE];
static uint16_t s_payload_len;
static bool s_pending;

void usb_io_set_instance(uint8_t instance)
{
    s_instance = instance;
}

uint8_t usb_io_instance(void)
{
    return s_instance;
}

bool usb_io_enabled(void)
{
    return s_instance != USB_IO_INST_NONE;
}

void usb_io_receive(const uint8_t *payload, uint16_t len)
{
    if (s_instance == USB_IO_INST_NONE || len == 0) return;
    if (len > IO_PAYLOAD_SIZE) len = IO_PAYLOAD_SIZE;

    memcpy(s_payload, payload, len);
    s_payload_len = len;
    s_pending = true;
}

// echo：回包 = "pico"(4B) + 原 payload 前 59B，共 63B，Report ID 1。
// 主机 1ms 轮询 OUT，端点忙时保留 pending 下一轮重发（覆盖式 stash，
// 通讯口场景新报文即最新真相，与 NKRO 键盘队列同语义）。
void usb_io_task(void)
{
    if (!s_pending || s_instance == USB_IO_INST_NONE) return;
    if (!tud_hid_n_ready(s_instance)) return;

    uint8_t reply[IO_PAYLOAD_SIZE] = "pico";
    uint16_t body = (uint16_t)(IO_PAYLOAD_SIZE - 4u);
    if (s_payload_len < body) body = s_payload_len;
    memcpy(reply + 4u, s_payload, body);

    if (tud_hid_n_report(s_instance, IO_REPORT_ID_IN, reply, IO_PAYLOAD_SIZE)) {
        s_pending = false;
    }
}
