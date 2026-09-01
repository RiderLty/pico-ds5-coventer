#ifndef DS5_CONFIG_H
#define DS5_CONFIG_H

/*
 * 键鼠 → DS5 映射的全部可调参数。
 * 键码为标准 HID Keyboard/Keypad usage ID（模拟键盘报文中的 keycode）。
 */

//--------------------------------------------------------------------
// 左摇杆（WASD）
//--------------------------------------------------------------------

// 满按键时摇杆偏离中心的量（0~127；越大游戏内移动越快）
#define MAP_STICK_RANGE       120

//--------------------------------------------------------------------
// 右摇杆（鼠标）
//
// FPS 中摇杆是"速度"输入：偏移量 = 视角转动速率。
// 因此摇杆值正比于鼠标的瞬时速度（计数/毫秒），鼠标停止 →
// 摇杆立即回中 → 视角立即停止，与真实鼠标手感一致。
//--------------------------------------------------------------------

// 鼠标速度(计数/ms) → 摇杆偏移量的放大倍数。
// 800dpi 下匀速跟踪约 0.5~1.5 计数/ms，×40 得到 20~60 的摇杆偏移；
// 甩枪（~15 计数/ms）会打满摇杆，达到游戏最大转身速度
#define MOUSE_SENS            40.0f

// 速度平滑窗口（毫秒）：把瞬时速度理解为最近 τ 毫秒的平均速度。
// 越大越平滑/跟手延迟越高；鼠标停止后约 3τ 内速度归零
#define AIM_EMA_TAU_MS        25.0f

// anti-deadzone：游戏摇杆有小死区，把有效输入抬到这个值之上，
// 否则轻微移动鼠标在游戏里没有反应（0 = 关闭）
#define AIM_DEADZONE_MIN      15.0f

// 小于此偏移视为"未动"（防止抖动触发 anti-deadzone 抬底）
#define AIM_EPSILON           0.3f

//--------------------------------------------------------------------
// 触摸板镜像（鼠标移动 → 触摸板 contact）
//--------------------------------------------------------------------

#define TOUCHPAD_MIRROR       1      // 0 = 关闭触摸板镜像

// 鼠标停止多久后松开触摸板（毫秒）
#define TOUCH_RELEASE_MS      60u

//--------------------------------------------------------------------
// 键位映射表（keycode → DS5 按钮）
//--------------------------------------------------------------------

// DS5 按钮标识（引擎内部按内核 hid-playstation 的位布局写入报告）
enum {
    BTN_SQUARE = 0,   // byte7 bit4
    BTN_CROSS,        // byte7 bit5
    BTN_CIRCLE,       // byte7 bit6
    BTN_TRIANGLE,     // byte7 bit7
    BTN_L1,           // byte8 bit0
    BTN_R1,           // byte8 bit1
    BTN_CREATE,       // byte8 bit4（Share）
    BTN_OPTIONS,      // byte8 bit5
    BTN_L3,           // byte8 bit6
    BTN_R3,           // byte8 bit7
    BTN_PS,           // byte9 bit0
    BTN_TOUCH_CLICK,  // byte9 bit1（触摸板按键）
    BTN_COUNT
};

typedef struct {
    uint8_t key;   // HID keycode（修饰键除外——修饰键在报文 bit0-7，单独映射）
    uint8_t btn;   // BTN_* 枚举值
} keymap_entry_t;

// 默认 FPS 映射表，按需增删
#define KEYMAP_TABLE {                                                    \
    { 0x2C, BTN_CROSS         },  /* Space       → ✕  (跳跃/确认)      */ \
    { 0x08, BTN_SQUARE        },  /* E           → □  (互动/换弹)      */ \
    { 0x09, BTN_CIRCLE        },  /* F           → ○                   */ \
    { 0x12, BTN_TRIANGLE      },  /* R           → △  (换武器)         */ \
    { 0x14, BTN_L1            },  /* Q           → L1                  */ \
    { 0x29, BTN_OPTIONS       },  /* Esc         → Options             */ \
    { 0x2B, BTN_TOUCH_CLICK   },  /* Tab         → 触摸板按键 (地图)    */ \
    { 0x65, BTN_OPTIONS       },  /* Application → Options             */ \
}

// 修饰键映射（模拟键盘报文 byte0 的位）
#define MOD_BIT_LCTRL   0x01
#define MOD_BIT_LSHIFT  0x02
#define MOD_BIT_LALT    0x04
#define MOD_BIT_LGUI    0x08
#define MOD_BIT_RCTRL   0x10
#define MOD_BIT_RSHIFT  0x20
#define MOD_BIT_RALT    0x40
#define MOD_BIT_RGUI    0x80

// 左Shift → L3（疾跑）、左Ctrl → R3（蹲下）；0xFF = 不映射
#define MAP_MOD_LSHIFT_TO   BTN_L3
#define MAP_MOD_LCTRL_TO    BTN_R3
#define MAP_MOD_RSHIFT_TO   0xFF
#define MAP_MOD_RCTRL_TO    0xFF

// WASD 移动键 keycodes
#define KEY_W 0x1A
#define KEY_A 0x04
#define KEY_S 0x16
#define KEY_D 0x07

// 方向键 → 十字键 keycodes
#define KEY_UP    0x52
#define KEY_DOWN  0x51
#define KEY_LEFT  0x50
#define KEY_RIGHT 0x4F

// 鼠标按键（hid_mouse_frame_t.buttons 的位）→ DS5
// bit0 = 左键, bit1 = 右键, bit2 = 中键
#define MAP_MOUSE_L_TO   0xFF  /* 保留：左键固定为 R2 模拟扳机（引擎内实现） */
#define MAP_MOUSE_R_TO   0xFF  /* 保留：右键固定为 L2 模拟扳机 */
#define MAP_MOUSE_M_TO   BTN_R3

#endif // DS5_CONFIG_H
