/*
 * DS5 映射引擎实现 —— 手柄直通版。
 *
 * 手柄状态 → DualSense 0x01 输入报告。按键位布局遵循内核
 * hid-playstation.c 的 USB 输入报告（与 DS5Dongle 的抓包模板一致）。
 *
 * 输入源：手柄帧（hidkit 归一化，见 input_queue.h 的 PAD 布局）——
 *   摇杆/扳机/十字键/按键直通覆盖；未接入时报告维持空闲态
 *   （摇杆回中、按键全零）。键鼠→手柄的固定映射已移除（键鼠输出
 *   经 Lua 转发到宿主键鼠接口）；pad.* Lua 接口落地后，手柄直通
 *   亦将移交 Lua。
 */

#include <string.h>

#include "pico/time.h"

#include "config.h"       // DS5 按钮 enum（BTN_SQUARE..BTN_TOUCH_CLICK）
#include "ds5_pad.h"
#include "input_queue.h"  // INQ_PAD_PAYLOAD（手柄帧布局）
#include "hidkit.h"       // hidkit BTN_*（手柄按键统一词表）

// 报文偏移（0x01 输入报告，Report ID 之后）
#define OFF_LX      0u
#define OFF_LY      1u
#define OFF_RX      2u
#define OFF_RY      3u
#define OFF_L2      4u
#define OFF_R2      5u
#define OFF_SEQ     6u
#define OFF_BTN0    7u   // 低4位 hat + bit4□ bit5✕ bit6○ bit7△
#define OFF_BTN1    8u   // bit0 L1 bit1 R1 bit2 L2 bit3 R2 bit4 Create bit5 Options bit6 L3 bit7 R3
#define OFF_BTN2    9u   // bit0 PS bit1 触摸板按键 bit2 静音
#define OFF_TOUCH   32u  // touch points[2]，各 4 字节
#define OFF_STATUS0 52u

// hat 值（byte7 低 4 位）
#define HAT_N 0u
#define HAT_NE 1u
#define HAT_E 2u
#define HAT_SE 3u
#define HAT_S 4u
#define HAT_SW 5u
#define HAT_W 6u
#define HAT_NW 7u
#define HAT_CENTER 8u

// 各 BTN_* 枚举值在报告中的位置（byte 偏移, 位掩码）
static const struct { uint8_t off; uint8_t mask; } btn_pos[BTN_COUNT] = {
    [BTN_SQUARE]      = { OFF_BTN0, 0x10 },
    [BTN_CROSS]       = { OFF_BTN0, 0x20 },
    [BTN_CIRCLE]      = { OFF_BTN0, 0x40 },
    [BTN_TRIANGLE]    = { OFF_BTN0, 0x80 },
    [BTN_L1]          = { OFF_BTN1, 0x01 },
    [BTN_R1]          = { OFF_BTN1, 0x02 },
    [BTN_CREATE]      = { OFF_BTN1, 0x10 },
    [BTN_OPTIONS]     = { OFF_BTN1, 0x20 },
    [BTN_L3]          = { OFF_BTN1, 0x40 },
    [BTN_R3]          = { OFF_BTN1, 0x80 },
    [BTN_PS]          = { OFF_BTN2, 0x01 },
    [BTN_TOUCH_CLICK] = { OFF_BTN2, 0x02 },
};

// ---- 输入状态 ----
static uint8_t  s_seq;              // 输入报告序号

// 手柄输入状态（hidkit 归一化：轴 -32767..32767，扳机 0..255）
static bool     s_pad_connected;
static int16_t  s_pad_lsx, s_pad_lsy, s_pad_rsx, s_pad_rsy;
static uint8_t  s_pad_lt, s_pad_rt;
static uint32_t s_pad_btnmask;      // bit n = hidkit BTN_* n 按住

// 报告模板：来自 DS5Dongle 对真实 DS5 的抓包（保留 reserved/status 区域）
static const uint8_t s_report_template[DS_REPORT_LEN] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7,
    0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
    0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03,
    0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
    0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b
};

static uint8_t s_report[DS_REPORT_LEN];

void ds5_pad_init(void)
{
    memcpy(s_report, s_report_template, sizeof(s_report));
}

static void btn_set(uint8_t btn, bool pressed)
{
    if (btn >= BTN_COUNT) return;
    if (pressed) {
        s_report[btn_pos[btn].off] |= btn_pos[btn].mask;
    } else {
        s_report[btn_pos[btn].off] &= (uint8_t)~btn_pos[btn].mask;
    }
}

// hidkit BTN_* → DS5 按钮；0xFF = 不映射
static uint8_t pad_btn_to_ds5(uint8_t btn)
{
    switch (btn) {
    case BTN_A:      return BTN_CROSS;
    case BTN_B:      return BTN_CIRCLE;
    case BTN_X:      return BTN_SQUARE;
    case BTN_Y:      return BTN_TRIANGLE;
    case BTN_LB:     return BTN_L1;
    case BTN_RB:     return BTN_R1;
    case BTN_SELECT: return BTN_CREATE;
    case BTN_START:  return BTN_OPTIONS;
    case BTN_LS:     return BTN_L3;
    case BTN_RS:     return BTN_R3;
    case BTN_HOME:   return BTN_PS;
    case BTN_MISC:   return BTN_TOUCH_CLICK;
    default:         return 0xFF;
    }
}

void ds5_pad_feed_pad(const uint8_t *data, uint8_t len)
{
    if (len < INQ_PAD_PAYLOAD) return;

    if (!(data[0] & 0x01u)) {
        // 断连标记：清状态，报告回落空闲态
        s_pad_connected = false;
        s_pad_btnmask = 0;
        s_pad_lt = s_pad_rt = 0;
        s_pad_lsx = s_pad_lsy = s_pad_rsx = s_pad_rsy = 0;
        return;
    }

    s_pad_connected = true;
    s_pad_lsx = (int16_t)(data[1] | ((uint16_t)data[2] << 8));
    s_pad_lsy = (int16_t)(data[3] | ((uint16_t)data[4] << 8));
    s_pad_rsx = (int16_t)(data[5] | ((uint16_t)data[6] << 8));
    s_pad_rsy = (int16_t)(data[7] | ((uint16_t)data[8] << 8));
    s_pad_lt = data[9];
    s_pad_rt = data[10];
    s_pad_btnmask = (uint32_t)data[11]
                  | ((uint32_t)data[12] << 8)
                  | ((uint32_t)data[13] << 16)
                  | ((uint32_t)data[14] << 24);
}

// 四方向 → hat 值
static uint8_t hat_from_dirs(bool up, bool down, bool left, bool right)
{
    if (up && right)       return HAT_NE;
    if (down && right)     return HAT_SE;
    if (down && left)      return HAT_SW;
    if (up && left)        return HAT_NW;
    if (up)                return HAT_N;
    if (right)             return HAT_E;
    if (down)              return HAT_S;
    if (left)              return HAT_W;
    return HAT_CENTER;
}

// hidkit 归一化轴 -32767..32767 → DS5 摇杆 0..255（中心 128）
static uint8_t stick_to_u8(int16_t v)
{
    int32_t out = 128 + ((int32_t)v * 127) / 32767;
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (uint8_t)out;
}

// 摇杆/扳机模拟值 + hat 直通覆盖
static void update_pad_sticks(void)
{
    s_report[OFF_LX] = stick_to_u8(s_pad_lsx);
    s_report[OFF_LY] = stick_to_u8(s_pad_lsy);
    s_report[OFF_RX] = stick_to_u8(s_pad_rsx);
    s_report[OFF_RY] = stick_to_u8(s_pad_rsy);
    s_report[OFF_L2] = s_pad_lt;
    s_report[OFF_R2] = s_pad_rt;

    uint8_t hat = hat_from_dirs(s_pad_btnmask & (1u << BTN_DPAD_UP),
                                s_pad_btnmask & (1u << BTN_DPAD_DOWN),
                                s_pad_btnmask & (1u << BTN_DPAD_LEFT),
                                s_pad_btnmask & (1u << BTN_DPAD_RIGHT));
    s_report[OFF_BTN0] = (uint8_t)((s_report[OFF_BTN0] & 0xF0u) | hat);
}

// 手柄按键直通（含扳机数字位）
static void update_pad_buttons(void)
{
    for (uint8_t b = BTN_A; b <= BTN_EXTRA_8; b++) {
        bool pressed = (s_pad_btnmask & (1u << b)) != 0;
        uint8_t ds = pad_btn_to_ds5(b);
        if (ds != 0xFF) btn_set(ds, pressed);
    }
    if (s_pad_lt) s_report[OFF_BTN1] |= 0x04u;
    else          s_report[OFF_BTN1] &= (uint8_t)~0x04u;
    if (s_pad_rt) s_report[OFF_BTN1] |= 0x08u;
    else          s_report[OFF_BTN1] &= (uint8_t)~0x08u;
}

// 空闲态：摇杆回中、扳机归零、hat 居中、按键全清
static void update_idle(void)
{
    s_report[OFF_LX] = 128;
    s_report[OFF_LY] = 128;
    s_report[OFF_RX] = 128;
    s_report[OFF_RY] = 128;
    s_report[OFF_L2] = 0;
    s_report[OFF_R2] = 0;
    s_report[OFF_BTN0] = HAT_CENTER;
    s_report[OFF_BTN1] = 0;
    s_report[OFF_BTN2] = 0;
}

void ds5_pad_tick(void)
{
    if (s_pad_connected) {
        update_pad_sticks();
        update_pad_buttons();
    } else {
        update_idle();
    }
}

void ds5_pad_build_report(uint8_t *out)
{
    s_report[OFF_SEQ] = s_seq++;
    memcpy(out, s_report, DS_REPORT_LEN);
}
