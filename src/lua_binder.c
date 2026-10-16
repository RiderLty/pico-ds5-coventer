/*
 * lua_binder 实现（由 pico-hid-mapper 的 src/lua_binder.c 按本工程 hidkit
 * 回调精简移植）。
 *
 * 只做 C → Lua 回调。除板载 BOOTSEL 按键外，回调返回值都会被忽略；
 * on_boot_button 返回真值时会拦截释放时的默认 WinUSB 开启行为。
 *
 * **事件全量监听**：hidkit 事件一律直投 Lua 回调，C 侧无开关、无位图过滤
 * （开销可接受：手柄报文 125~1000Hz，Lua pcall 调用本身 ~1µs 量级；
 * 未定义的回调只需一次整数比较即返回）。
 *
 * 转发口：键盘 / 鼠标按键 / 鼠标位移 / 滚轮 / 手柄按键 / 手柄绝对状态 / 六轴 /
 * 触摸板快照 / 设备丢弃 / 板载 BOOTSEL 按键；另有 tick 周期回调。
 */

#include "lua_binder.h"
#include "lua_core.h"
#include "pico/time.h"     // time_us_64（tick 的 dt 计时）
#include <lua.h>
#include <lauxlib.h>
#include <string.h>

//--------------------------------------------------------------------+
// 缓存的 Lua 函数 id
//--------------------------------------------------------------------+

static int g_lid_on_key         = -1;
static int g_lid_on_mouse_btn   = -1;
static int g_lid_on_mouse_move  = -1;
static int g_lid_on_mouse_wheel = -1;
static int g_lid_on_gamepad_btn = -1;
static int g_lid_on_gamepad_abs = -1;
static int g_lid_on_motion      = -1;
static int g_lid_on_touchpad    = -1;
static int g_lid_on_dropped     = -1;
static int g_lid_on_boot_button = -1;
static int g_lid_tick           = -1;

// tick 的限频及 dt 计时：距上次实际回调的微秒差（首个 tick 记 0）
static uint64_t g_last_tick_us = 0;
static bool     g_tick_primed  = false;

// 触摸板节流基线：上次已分发到 Lua 的快照（逐字节比较，纯去重非边沿差分）。
// VM 重建时清零 → 新 VM 一定会收到当前状态的第一帧。
static uint32_t s_tp_last_meta;
static uint32_t s_tp_last_dims;
enum { LUA_TOUCHPAD_SLOTS = 2 }; // DS5 的两个触点
static uint16_t s_tp_last_x[LUA_TOUCHPAD_SLOTS];
static uint16_t s_tp_last_y[LUA_TOUCHPAD_SLOTS];

//--------------------------------------------------------------------+
// 外部 rebind 回调（默认空实现，应用层覆盖 —— 本工程实现在 input_app.c）
//--------------------------------------------------------------------+

__attribute__((weak))
void lua_binder_rebind(void) {}

//--------------------------------------------------------------------+
// 生命周期
//--------------------------------------------------------------------+

bool lua_binder_init(const char *script, size_t len) {
    lua_binder_deinit(); // clear cached registry IDs even if the new script fails

    if (!lua_core_init(NULL, 0)) return false;

    // 应用层绑定 C 函数（print/warn 覆盖、hidkit 信息等）
    lua_binder_rebind();

    // 加载外部脚本
    if (script && len > 0) {
        if (!lua_core_dobuffer(script, len)) return false;
    }

    // 缓存脚本中定义的入口函数 id（脚本未定义的为 -1，转发时跳过）
    g_lid_on_key         = lua_core_find("on_key");
    g_lid_on_mouse_btn   = lua_core_find("on_mouse_btn");
    g_lid_on_mouse_move  = lua_core_find("on_mouse_move");
    g_lid_on_mouse_wheel = lua_core_find("on_mouse_wheel");
    g_lid_on_gamepad_btn = lua_core_find("on_gamepad_btn");
    g_lid_on_gamepad_abs = lua_core_find("on_gamepad_abs");
    g_lid_on_motion      = lua_core_find("on_motion");
    g_lid_on_touchpad    = lua_core_find("on_touchpad");
    g_lid_on_dropped     = lua_core_find("on_dropped");
    g_lid_on_boot_button = lua_core_find("on_boot_button");
    g_lid_tick           = lua_core_find("tick");

    // 触摸板节流基线清零：新 VM 从零起
    s_tp_last_meta = 0;
    s_tp_last_dims = 0;
    memset(s_tp_last_x, 0, sizeof(s_tp_last_x));
    memset(s_tp_last_y, 0, sizeof(s_tp_last_y));

    // 新 VM：重新等待一个 tick 间隔，首个 tick 的 dt 从 0 起
    g_last_tick_us = time_us_64();
    g_tick_primed = false;

    return true;
}

void lua_binder_deinit(void) {
    g_lid_on_key = g_lid_on_mouse_btn = g_lid_on_mouse_move = g_lid_on_mouse_wheel = -1;
    g_lid_on_gamepad_btn = g_lid_on_gamepad_abs = g_lid_on_motion = -1;
    g_lid_on_touchpad = g_lid_on_dropped = g_lid_tick = -1;
    g_lid_on_boot_button = -1;
    lua_core_deinit();
}

bool lua_binder_is_ready(void) {
    return lua_core_is_ready();
}

//--------------------------------------------------------------------+
// Core → Lua: 注册 C 函数
//--------------------------------------------------------------------+

bool lua_binder_bind(const char *name, void *func) {
    return lua_core_register(name, func);
}

//--------------------------------------------------------------------+
// C → Lua: 事件转发（全量直投，仅检查回调是否存在）
//--------------------------------------------------------------------+

void lua_binder_on_key(uint8_t keycode, bool down) {
    if (g_lid_on_key < 0) return;

    lua_core_push_int(keycode);
    lua_core_push_bool(down);
    (void)lua_core_pcall_id(g_lid_on_key, 2, 0);
}

void lua_binder_on_mouse_btn(uint8_t button, bool down) {
    if (g_lid_on_mouse_btn < 0) return;

    lua_core_push_int(button);
    lua_core_push_bool(down);
    (void)lua_core_pcall_id(g_lid_on_mouse_btn, 2, 0);
}

void lua_binder_on_mouse_move(int32_t dx, int32_t dy) {
    if (g_lid_on_mouse_move < 0) return;

    lua_core_push_int(dx);
    lua_core_push_int(dy);
    (void)lua_core_pcall_id(g_lid_on_mouse_move, 2, 0);
}

void lua_binder_on_mouse_wheel(int32_t wheel) {
    if (g_lid_on_mouse_wheel < 0) return;

    lua_core_push_int(wheel);
    (void)lua_core_pcall_id(g_lid_on_mouse_wheel, 1, 0);
}

void lua_binder_on_gamepad_btn(uint8_t code, bool pressed) {
    if (g_lid_on_gamepad_btn < 0) return;

    lua_core_push_int((int)code);
    lua_core_push_bool(pressed);
    (void)lua_core_pcall_id(g_lid_on_gamepad_btn, 2, 0);
}

void lua_binder_on_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                               int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt) {
    (void)slot;
    if (g_lid_on_gamepad_abs < 0) return;

    lua_core_push_int(ls_x);
    lua_core_push_int(ls_y);
    lua_core_push_int(rs_x);
    lua_core_push_int(rs_y);
    lua_core_push_int(lt);
    lua_core_push_int(rt);
    (void)lua_core_pcall_id(g_lid_on_gamepad_abs, 6, 0);
}

void lua_binder_on_motion(int8_t slot, int16_t gx, int16_t gy, int16_t gz,
                          int16_t ax, int16_t ay, int16_t az) {
    (void)slot;
    if (g_lid_on_motion < 0) return;

    lua_core_push_int(gx);
    lua_core_push_int(gy);
    lua_core_push_int(gz);
    lua_core_push_int(ax);
    lua_core_push_int(ay);
    lua_core_push_int(az);
    (void)lua_core_pcall_id(g_lid_on_motion, 6, 0);
}

// 触摸板快照：只向 Lua 传 DS5 的两个触点，共六个参数。
// on_touchpad(meta, dims, x0, y0, x1, y1)
// 内置节流：与上次已分发的快照逐字节相同时不调用——静止手指的定频报文
// 洪峰在此挡掉。脚本未定义回调时只有一次整数比较。
void lua_binder_on_touchpad(uint32_t meta, uint32_t dims,
                            const uint16_t *x, const uint16_t *y) {
    if (g_lid_on_touchpad < 0) return;
    if (meta == s_tp_last_meta && dims == s_tp_last_dims &&
        memcmp(x, s_tp_last_x, sizeof(s_tp_last_x)) == 0 &&
        memcmp(y, s_tp_last_y, sizeof(s_tp_last_y)) == 0) return;
    // Reserve six arguments plus the registry function before any pushes, so
    // allocation failure leaves the stack untouched and permits a later retry.
    if (!lua_core_checkstack(3 + 2 * LUA_TOUCHPAD_SLOTS)) return;
    s_tp_last_meta = meta;
    s_tp_last_dims = dims;
    memcpy(s_tp_last_x, x, sizeof(s_tp_last_x));
    memcpy(s_tp_last_y, y, sizeof(s_tp_last_y));

    lua_core_push_int((int)meta);
    lua_core_push_int((int)dims);
    for (int i = 0; i < LUA_TOUCHPAD_SLOTS; i++) {
        lua_core_push_int(x[i]);
        lua_core_push_int(y[i]);
    }
    (void)lua_core_pcall_id(g_lid_on_touchpad, 2 + 2 * LUA_TOUCHPAD_SLOTS, 0);
}

void lua_binder_on_dropped(int8_t slot, uint16_t vid, uint16_t pid) {
    if (g_lid_on_dropped < 0) return;

    lua_core_push_int(slot);
    lua_core_push_int(vid);
    lua_core_push_int(pid);
    (void)lua_core_pcall_id(g_lid_on_dropped, 3, 0);
}

// 板载 BOOTSEL 按键：on_boot_button(pressed)。返回 true 表示拦截默认行为；
// false、nil、无返回值、未绑定或 Lua 调用出错均不拦截。
bool lua_binder_on_boot_button(bool pressed) {
    if (g_lid_on_boot_button < 0) return false;

    lua_core_push_bool(pressed);
    if (!lua_core_pcall_id(g_lid_on_boot_button, 1, 1)) return false;

    bool handled = false;
    (void)lua_core_pop_bool(&handled);
    return handled;
}

//--------------------------------------------------------------------+
// C → Lua: 周期处理
//--------------------------------------------------------------------+

bool lua_binder_process(void) {
    if (g_lid_tick < 0) return false;
    uint64_t now = time_us_64();
    uint64_t elapsed = now - g_last_tick_us;
    if (elapsed < LUA_TICK_INTERVAL_US) return false;
    // dt = 距上次 tick 的微秒差（首个 tick 为 0）。now 单调(uint64)，dt 恒非负
    uint64_t dt = g_tick_primed ? elapsed : 0;
    g_last_tick_us = now;
    g_tick_primed  = true;
    // dt 可能超 INT32_MAX（约 35.8 分钟），钳制避免截断为负
    int32_t dt32 = (dt > (uint64_t)INT32_MAX) ? INT32_MAX : (int32_t)dt;
    lua_core_push_int(dt32);              // 参数: delta time（微秒）
    return lua_core_pcall_id(g_lid_tick, 1, 0);
}

//--------------------------------------------------------------------+
// 诊断
//--------------------------------------------------------------------+

const char *lua_binder_last_error(void) {
    return lua_core_last_error();
}
