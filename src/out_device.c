/*
 * DS5 模拟实现：把统一事件打上 kind 标签写入跨核队列。
 *
 * 键盘转发原始 8 字节报文（mod + reserved + keys[6]）；鼠标转发归一化
 * 6 字节帧（buttons u8 / wheel i8 / x i16 / y i16）。core0 的
 * input_queue_flush() 出队后交给 ds5_pad 映射成 DualSense 0x31 输入报告
 * （tud_hid_* 只能在 core0 调用）。
 *
 * 挂载/拔出/错误/其他 HID 设备在 DS5 模拟侧无对应输出，静默忽略。
 */

#include <string.h>

#include "hid_output.h"
#include "out_device.h"
#include "input_queue.h"

// 模拟键盘为标准 8 字节布局（mod + reserved + keys[6]），超长截断
#define DEV_KB_REPORT_LEN 8u

static void dev_mount(uint8_t instance, const hid_dev_info_t *info, uint8_t proto)
{
    (void)instance; (void)info; (void)proto;
}

static void dev_error(const char *msg)
{
    (void)msg;
}

static void dev_generic(uint8_t instance, const hid_dev_info_t *info,
                        const uint8_t *report, uint16_t len)
{
    (void)instance; (void)info; (void)report; (void)len;
    // 未识别设备无法映射为 DS5 手柄，丢弃
}

// 键盘报文 → 队列
static void dev_keyboard(uint8_t instance, const hid_dev_info_t *info,
                         const uint8_t *report, uint8_t len)
{
    (void)instance; (void)info;
    if (len > DEV_KB_REPORT_LEN) len = (uint8_t)DEV_KB_REPORT_LEN;
    input_queue_send(INQ_KIND_KB, report, len);
}

// 归一化鼠标帧 → 队列（buttons u8, wheel i8, x i16, y i16，小端）
static void dev_mouse(uint8_t instance, const hid_dev_info_t *info,
                      const hid_mouse_frame_t *frame)
{
    (void)instance; (void)info;
    uint8_t payload[6];
    payload[0] = frame->buttons;
    payload[1] = (uint8_t)frame->wheel;
    payload[2] = (uint8_t)(frame->x & 0xFF);   // x 小端
    payload[3] = (uint8_t)((frame->x >> 8) & 0xFF);
    payload[4] = (uint8_t)(frame->y & 0xFF);   // y 小端
    payload[5] = (uint8_t)((frame->y >> 8) & 0xFF);
    input_queue_send(INQ_KIND_MS, payload, sizeof(payload));
}

// 设备拔出：补发全零事件，避免手柄上残留按键/摇杆偏移
// （hub 下多键盘同插时，一个拔出会连带清掉其他键盘的按住状态——
// 罕见场景，换取实现简单）
static void dev_umount(uint8_t instance, const hid_dev_info_t *info)
{
    (void)instance; (void)info;
    static const uint8_t zero_kb[DEV_KB_REPORT_LEN] = { 0 };
    static const uint8_t zero_ms[6] = { 0 };
    input_queue_send(INQ_KIND_KB, zero_kb, sizeof(zero_kb));
    input_queue_send(INQ_KIND_MS, zero_ms, sizeof(zero_ms));
}

const hid_output_ops_t hid_output_device_ops = {
    .mount    = dev_mount,
    .umount   = dev_umount,
    .error    = dev_error,
    .keyboard = dev_keyboard,
    .mouse    = dev_mouse,
    .generic  = dev_generic,
};
