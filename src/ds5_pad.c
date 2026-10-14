/*
 * DS5 映射引擎实现。
 *
 * 输入状态 → DualSense 0x31 输入报告。按键位布局遵循内核
 * hid-playstation.c 的 USB 输入报告（与 DS5Dongle 的抓包模板一致）。
 *
 * 输入源与合并（过渡占位，后期由 Lua 接管）：
 *   - 键鼠：WASD → 左摇杆、鼠标速度 → 右摇杆、按键查表映射（原有行为）
 *   - 手柄（XInput / Switch Pro / DS5，经 hidkit 归一化）：已连接时
 *     摇杆/扳机/十字键直通覆盖，按键与键鼠映射取并集
 */

#include <string.h>
#include <math.h>

#include "pico/time.h"

#include "hidkit.h"       // hidkit BTN_*（手柄按键统一词表）；须先于 config.h ——
                          // 后者的 KEY_* 有 #ifndef 守卫，让位给 hidkit 同名宏
#include "config.h"
#include "ds5_pad.h"
#include "input_queue.h"  // INQ_PAD_PAYLOAD（手柄帧布局）
#include "debug.h"

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
static float    s_vel_x, s_vel_y;   // 鼠标速度 EMA（计数/ms）→ 右摇杆偏移
static float    s_pending_x, s_pending_y; // 距上次 tick 的未处理位移
static uint64_t s_last_tick_us;     // 上次 update_right_stick 的时间
static uint8_t  s_seq;              // 输入报告序号

// 手柄输入状态（hidkit 归一化：轴 -32767..32767，扳机 0..255）
static bool     s_pad_connected;
static int16_t  s_pad_lsx, s_pad_lsy, s_pad_rsx, s_pad_rsy;
static uint8_t  s_pad_lt, s_pad_rt;
static uint32_t s_pad_btnmask;      // bit n = hidkit BTN_* n 按住

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
    s_last_tick_us = time_us_64();
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

    // 位移累计到待处理量，速度在 tick 里按时间归一（计数/ms）
    s_pending_x += (float)x;
    s_pending_y += (float)y;

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
        s_last_move_us = time_us_64();
    }
#endif
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
        // 断连标记：清状态，回落键鼠映射
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

// 方向键 → hat（byte7 低 4 位）
static void update_hat(void)
{
    uint8_t hat = hat_from_dirs(key_held(KEY_UP), key_held(KEY_DOWN),
                                key_held(KEY_LEFT), key_held(KEY_RIGHT));
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

// 鼠标按键 → 扳机 / 中键（手柄已连接时扳机由手柄驱动，跳过）
static void update_mouse_buttons(void)
{
    if (s_pad_connected) {
        btn_set(MAP_MOUSE_M_TO, (s_ms_buttons & 0x04u) != 0);
        return;
    }

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

// 右摇杆：摇杆偏移 = 鼠标当前速度（视角转动速率）。
// 鼠标位移按 tick 间隔归一成速度，经 EMA 平滑后乘灵敏度；
// 鼠标停止后速度在 ~3τ 内归零，摇杆回中、视角停止——与鼠标手感一致
static void update_right_stick(void)
{
    uint64_t now = time_us_64();

    float dt = (float)(now - s_last_tick_us) / 1000.0f;
    if (dt <= 0.0f) dt = 0.001f;

    // 采样窗口内的平均速度（计数/ms）
    float inst_x = s_pending_x / dt;
    float inst_y = s_pending_y / dt;
    s_pending_x = 0.0f;
    s_pending_y = 0.0f;
    s_last_tick_us = now;

    // EMA 平滑：等效于取最近 AIM_EMA_TAU_MS 的平均速度
    float w = 1.0f - expf(-dt / AIM_EMA_TAU_MS);
    s_vel_x += (inst_x - s_vel_x) * w;
    s_vel_y += (inst_y - s_vel_y) * w;
    if (fabsf(s_vel_x) < 0.005f) s_vel_x = 0.0f;
    if (fabsf(s_vel_y) < 0.005f) s_vel_y = 0.0f;

    s_report[OFF_RX] = aim_axis(s_vel_x * MOUSE_SENS, 128.0f);
    s_report[OFF_RY] = aim_axis(s_vel_y * MOUSE_SENS, 128.0f);
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

// ---- 手柄（已连接时在键鼠路径之后调用，覆盖摇杆/扳机/十字键，按键取并集）----

// hidkit 归一化轴 -32767..32767 → DS5 摇杆 0..255（中心 128）
static uint8_t stick_to_u8(int16_t v)
{
    int32_t out = 128 + ((int32_t)v * 127) / 32767;
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (uint8_t)out;
}

// 摇杆/扳机直通覆盖（L2/R2 数字位在 update_pad_buttons 补）
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

// 手柄按键与键鼠映射取并集（在 update_key_buttons / update_mouse_buttons 清位之后）
static void update_pad_buttons(void)
{
    for (uint8_t b = BTN_A; b <= BTN_EXTRA_8; b++) {
        if (s_pad_btnmask & (1u << b)) {
            uint8_t ds = pad_btn_to_ds5(b);
            if (ds != 0xFF) btn_set(ds, true);
        }
    }
    // 扳机数字位（模拟值已在 update_pad_sticks 写入）
    if (s_pad_lt) s_report[OFF_BTN1] |= 0x04u;
    if (s_pad_rt) s_report[OFF_BTN1] |= 0x08u;
}

void ds5_pad_tick(void)
{
    update_left_stick();
    update_hat();
    update_key_buttons();
    update_mouse_buttons();
    update_right_stick();
    if (s_pad_connected) {
        update_pad_sticks();
        update_pad_buttons();
    }
    update_touchpad();
}

void ds5_pad_build_report(uint8_t *out)
{
    s_report[OFF_SEQ] = s_seq++;
    memcpy(out, s_report, DS_REPORT_LEN);
}
