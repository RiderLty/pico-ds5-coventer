/*
 * DS5 映射引擎实现。
 *
 * 键鼠状态 → DualSense 0x31 输入报告。按键位布局遵循内核
 * hid-playstation.c 的 USB 输入报告（与 DS5Dongle 的抓包模板一致）。
 */

#include <string.h>
#include <math.h>

#include "pico/time.h"

#include "config.h"
#include "ds5_pad.h"
#include "hid_parser.h"   // hid_mouse_frame_t 的打包格式约定

// 报文偏移（0x31 输入报告，Report ID 之后）
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

#define TOUCH_MAX_X 1919u
#define TOUCH_MAX_Y 1079u

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

static const keymap_entry_t s_keymap[] = KEYMAP_TABLE;

// ---- 输入状态 ----
static uint8_t  s_kb[8];            // 最新键盘报文（mod + reserved + keys[6]）
static uint8_t  s_ms_buttons;       // 最新鼠标按键位
static float    s_acc_x, s_acc_y;   // 鼠标位移累积器（右摇杆偏移）
static uint64_t s_last_ms_us;       // 最近一次鼠标事件时间
static uint64_t s_last_decay_us;    // 最近一次右摇杆衰减时间
static uint8_t  s_seq;              // 输入报告序号

// 触摸板镜像
#if TOUCHPAD_MIRROR
static bool     s_touch_active;
static uint16_t s_touch_x, s_touch_y;   // 虚拟光标（触摸板分辨率 1920x1080）
static uint64_t s_last_move_us;
static uint8_t  s_touch_counter;        // contact 计数（bit7 = 释放）
#endif

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

// 键盘报文中某 keycode 是否按住
static bool key_held(uint8_t keycode)
{
    for (uint8_t i = 2; i < sizeof(s_kb); i++) {
        if (s_kb[i] == keycode) return true;
    }
    return false;
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

// 修饰键位（模拟键盘报文 byte0）→ BTN_*
static bool mod_bit_pressed(uint8_t bit)
{
    return (s_kb[0] & bit) != 0;
}

static uint8_t mod_to_btn(uint8_t bit, uint8_t map_to)
{
    if (map_to != 0xFF && mod_bit_pressed(bit)) return map_to;
    return 0xFF;
}

// 右摇杆：累积器 → 摇杆值（含 anti-deadzone 抬底）
static uint8_t aim_axis(float acc, float center)
{
    float a = acc;
    if (a > AIM_EPSILON) {
        if (a < AIM_DEADZONE_MIN) a = AIM_DEADZONE_MIN;
    } else if (a < -AIM_EPSILON) {
        if (a > -AIM_DEADZONE_MIN) a = -AIM_DEADZONE_MIN;
    } else {
        a = 0.0f;
    }
    int v = (int)(center + a);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

void ds5_pad_feed_kb(const uint8_t *report, uint8_t len)
{
    if (len > sizeof(s_kb)) len = (uint8_t)sizeof(s_kb);
    memcpy(s_kb, report, len);
}

void ds5_pad_feed_ms(const uint8_t *data, uint8_t len)
{
    if (len < 6) return;

    // 打包帧：buttons u8, wheel i8, x i16, y i16（小端）
    s_ms_buttons = data[0];
    int16_t x = (int16_t)(data[2] | (data[3] << 8));
    int16_t y = (int16_t)(data[4] | (data[5] << 8));

    s_acc_x += (float)x * MOUSE_SENS;
    s_acc_y += (float)y * MOUSE_SENS;
    s_last_ms_us = time_us_64();

#if TOUCHPAD_MIRROR
    if (x != 0 || y != 0) {
        if (!s_touch_active) {
            s_touch_active = true;
            s_touch_counter = (uint8_t)((s_touch_counter + 1u) & 0x7Fu);
            s_touch_x = TOUCH_MAX_X / 2u;
            s_touch_y = TOUCH_MAX_Y / 2u;
        }
        int nx = (int)s_touch_x + x;
        int ny = (int)s_touch_y + y;
        s_touch_x = (uint16_t)(nx < 0 ? 0 : (nx > (int)TOUCH_MAX_X ? (int)TOUCH_MAX_X : nx));
        s_touch_y = (uint16_t)(ny < 0 ? 0 : (ny > (int)TOUCH_MAX_Y ? (int)TOUCH_MAX_Y : ny));
        s_last_move_us = s_last_ms_us;
    }
#endif
}

// WASD → 左摇杆
static void update_left_stick(void)
{
    int dx = (key_held(KEY_D) ? 1 : 0) - (key_held(KEY_A) ? 1 : 0);
    int dy = (key_held(KEY_S) ? 1 : 0) - (key_held(KEY_W) ? 1 : 0);

    float range = (float)MAP_STICK_RANGE;
    if (dx != 0 && dy != 0) {
        range *= 0.7071f;   // 对角线归一化
    }
    int lx = 128 + dx * (int)range;
    int ly = 128 + dy * (int)range;
    if (lx < 0) lx = 0;
    if (lx > 255) lx = 255;
    if (ly < 0) ly = 0;
    if (ly > 255) ly = 255;
    s_report[OFF_LX] = (uint8_t)lx;
    s_report[OFF_LY] = (uint8_t)ly;
}

// 方向键 → hat（byte7 低 4 位）
static void update_hat(void)
{
    bool up    = key_held(KEY_UP);
    bool down  = key_held(KEY_DOWN);
    bool left  = key_held(KEY_LEFT);
    bool right = key_held(KEY_RIGHT);

    uint8_t hat;
    if (up && right)       hat = HAT_NE;
    else if (down && right) hat = HAT_SE;
    else if (down && left)  hat = HAT_SW;
    else if (up && left)    hat = HAT_NW;
    else if (up)            hat = HAT_N;
    else if (right)         hat = HAT_E;
    else if (down)          hat = HAT_S;
    else if (left)          hat = HAT_W;
    else                    hat = HAT_CENTER;

    s_report[OFF_BTN0] = (uint8_t)((s_report[OFF_BTN0] & 0xF0u) | hat);
}

// 键位映射表 + 修饰键 → 按键位
static void update_key_buttons(void)
{
    // 先清掉所有映射目标位（避免拔键后残留），触摸板按键同属 byte9
    for (uint8_t b = 0; b < BTN_COUNT; b++) {
        if (b == BTN_PS) continue;   // PS 键无默认映射，保留
        btn_set(b, false);
    }

    for (size_t i = 0; i < sizeof(s_keymap) / sizeof(s_keymap[0]); i++) {
        if (key_held(s_keymap[i].key)) {
            btn_set(s_keymap[i].btn, true);
        }
    }

    uint8_t b;
    if ((b = mod_to_btn(MOD_BIT_LSHIFT, MAP_MOD_LSHIFT_TO)) != 0xFF) btn_set(b, true);
    if ((b = mod_to_btn(MOD_BIT_LCTRL,  MAP_MOD_LCTRL_TO))  != 0xFF) btn_set(b, true);
    if ((b = mod_to_btn(MOD_BIT_RSHIFT, MAP_MOD_RSHIFT_TO)) != 0xFF) btn_set(b, true);
    if ((b = mod_to_btn(MOD_BIT_RCTRL,  MAP_MOD_RCTRL_TO))  != 0xFF) btn_set(b, true);
}

// 鼠标按键 → 扳机 / 中键
static void update_mouse_buttons(void)
{
    bool l = (s_ms_buttons & 0x01u) != 0;   // 左键 → R2 模拟扳机
    bool r = (s_ms_buttons & 0x02u) != 0;   // 右键 → L2 模拟扳机
    bool m = (s_ms_buttons & 0x04u) != 0;

    s_report[OFF_R2] = l ? 0xFF : 0x00;
    s_report[OFF_L2] = r ? 0xFF : 0x00;
    // 扳机按下同时置数字位，方便只读数字按键的游戏
    if (l) s_report[OFF_BTN1] |= 0x08u; else s_report[OFF_BTN1] &= (uint8_t)~0x08u;
    if (r) s_report[OFF_BTN1] |= 0x04u; else s_report[OFF_BTN1] &= (uint8_t)~0x04u;

    btn_set(MAP_MOUSE_M_TO, m);
}

// 右摇杆：累积器 + 空闲回中
static void update_right_stick(void)
{
    uint64_t now = time_us_64();

    // 鼠标空闲超过延迟后，每 1ms 衰减一步（AIM_RETURN_RATE^steps）
    if (now - s_last_ms_us > AIM_RETURN_DELAY_MS * 1000u) {
        uint64_t since_decay_us = now - s_last_decay_us;
        uint32_t steps = (uint32_t)(since_decay_us / 1000u);
        if (steps > 50) steps = 50;
        if (steps) {
            float rate = powf(AIM_RETURN_RATE, (float)steps);
            s_acc_x *= rate;
            s_acc_y *= rate;
            if (fabsf(s_acc_x) < 0.01f) s_acc_x = 0.0f;
            if (fabsf(s_acc_y) < 0.01f) s_acc_y = 0.0f;
            s_last_decay_us = s_last_decay_us + (uint64_t)steps * 1000u;
        }
    } else {
        s_last_decay_us = now;
    }

    s_report[OFF_RX] = aim_axis(s_acc_x, 128.0f);
    s_report[OFF_RY] = aim_axis(s_acc_y, 128.0f);
}

// 鼠标移动 → 触摸板 contact
static void update_touchpad(void)
{
#if TOUCHPAD_MIRROR
    if (s_touch_active &&
        (time_us_64() - s_last_move_us) > TOUCH_RELEASE_MS * 1000u) {
        s_touch_active = false;
    }

    uint8_t *p0 = &s_report[OFF_TOUCH];
    uint8_t *p1 = &s_report[OFF_TOUCH + 4];

    if (s_touch_active) {
        // contact 字节：bit7=释放；x 12bit + y 12bit 打包
        p0[0] = s_touch_counter;
        p0[1] = (uint8_t)(s_touch_x & 0xFF);
        p0[2] = (uint8_t)(((s_touch_y & 0x0Fu) << 4) | ((s_touch_x >> 8) & 0x0Fu));
        p0[3] = (uint8_t)(s_touch_y >> 4);
    } else {
        p0[0] = (uint8_t)(0x80u | s_touch_counter);   // 释放
        p0[1] = p0[2] = p0[3] = 0;
    }
    p1[0] = 0x80;   // touch1 恒释放
    p1[1] = p1[2] = p1[3] = 0;
#endif
}

void ds5_pad_tick(void)
{
    update_left_stick();
    update_hat();
    update_key_buttons();
    update_mouse_buttons();
    update_right_stick();
    update_touchpad();
}

void ds5_pad_build_report(uint8_t *out)
{
    s_report[OFF_SEQ] = s_seq++;
    memcpy(out, s_report, DS_REPORT_LEN);
}
