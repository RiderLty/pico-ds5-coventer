/*
 * lua_binder 实现（由 pico-hid-mapper 的 src/lua_binder.c 按本工程 hidkit
 * 回调精简移植）。
 *
 * 与 mapper 版的差异：
 *   - 无 init()/tick()/on_custom_event/on_boot_button（本工程暂不需要；
 *     mapper 的 process/refresh 及对应 id 缓存一并裁掉）
 *   - 新增手柄绝对状态（on_gamepad_abs）与六轴（on_motion）转发，各配
 *     enable_listen_gamepad / enable_listen_motion 门（手柄报文 125~1000Hz，
 *     未声明监听时零 VM 开销）
 *   - 计时不需要：无 tick，touchpad 节流是纯逐字节比较
 */

#include "lua_binder.h"
#include "lua_core.h"
#include "hidkit.h"        // HIDKIT_TOUCHPAD_SLOTS（触摸板快照槽位数）
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
static int g_lid_tick           = -1;

// tick 的 dt 计时：距上次 lua_binder_process 的微秒差（首个 tick 记 0）
static uint64_t g_last_tick_us = 0;
static bool     g_tick_primed  = false;

// 触摸板节流基线：上次已分发到 Lua 的快照（逐字节比较，纯去重非边沿差分）。
// VM 重建时清零 → 新 VM 一定会收到当前状态的第一帧。
static uint32_t s_tp_last_meta;
static uint32_t s_tp_last_dims;
static uint16_t s_tp_last_x[HIDKIT_TOUCHPAD_SLOTS];
static uint16_t s_tp_last_y[HIDKIT_TOUCHPAD_SLOTS];

//--------------------------------------------------------------------+
// 事件监听 bitmap（Lua 脚本通过 enable_listen_* / disable_listen_* 增删）
//--------------------------------------------------------------------+

static uint64_t g_key_listen[4];       // 256-bit, keycode 0x00–0xFF
static uint8_t  g_mouse_listen;        //   8-bit, mouse button 0–7
static bool     g_mouse_move_listen;   // 鼠标移动（dx/dy）监听开关
static bool     g_mouse_wheel_listen;  // 鼠标滚轮（wheel）监听开关
static uint32_t g_gamepad_listen;      //  32-bit, gamepad btn code 0x00–0x1F
static bool     g_gamepad_abs_listen;  // 手柄绝对状态监听开关
static bool     g_motion_listen;       // 六轴监听开关

//--------------------------------------------------------------------+
// Lua → C: enable_listen_* / disable_listen_*（可在脚本顶层或运行时调用）
//--------------------------------------------------------------------+

static int _cb_enable_listen_keys(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        int k = (int)luaL_checkinteger(L, i);
        g_key_listen[(k & 0xFF) >> 6] |= (1ULL << (k & 0x3F));
    }
    return 0;
}

// disable_listen_keys(k...)：清除指定键；无参数 → 清空全部
static int _cb_disable_listen_keys(lua_State *L) {
    int n = lua_gettop(L);
    if (n == 0) { memset(g_key_listen, 0, sizeof(g_key_listen)); return 0; }
    for (int i = 1; i <= n; i++) {
        int k = (int)luaL_checkinteger(L, i);
        g_key_listen[(k & 0xFF) >> 6] &= ~(1ULL << (k & 0x3F));
    }
    return 0;
}

static int _cb_enable_listen_mouse_btn(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        int b = (int)luaL_checkinteger(L, i);
        g_mouse_listen |= (1U << (b & 7));
    }
    return 0;
}

// disable_listen_mouse_btn(b...)：清除指定按键；无参数 → 清空全部
static int _cb_disable_listen_mouse_btn(lua_State *L) {
    int n = lua_gettop(L);
    if (n == 0) { g_mouse_listen = 0; return 0; }
    for (int i = 1; i <= n; i++) {
        int b = (int)luaL_checkinteger(L, i);
        g_mouse_listen &= ~(1U << (b & 7));
    }
    return 0;
}

static int _cb_enable_listen_mouse_move(lua_State *L)  { (void)L; g_mouse_move_listen  = true;  return 0; }
static int _cb_disable_listen_mouse_move(lua_State *L) { (void)L; g_mouse_move_listen  = false; return 0; }
static int _cb_enable_listen_mouse_wheel(lua_State *L) { (void)L; g_mouse_wheel_listen = true;  return 0; }
static int _cb_disable_listen_mouse_wheel(lua_State *L){ (void)L; g_mouse_wheel_listen = false; return 0; }

static int _cb_enable_listen_gamepad_btn(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        int k = (int)luaL_checkinteger(L, i);
        g_gamepad_listen |= (1U << (k & 0x1F));
    }
    return 0;
}

// disable_listen_gamepad_btn(k...)：清除指定按键；无参数 → 清空全部
static int _cb_disable_listen_gamepad_btn(lua_State *L) {
    int n = lua_gettop(L);
    if (n == 0) { g_gamepad_listen = 0; return 0; }
    for (int i = 1; i <= n; i++) {
        int k = (int)luaL_checkinteger(L, i);
        g_gamepad_listen &= ~(1U << (k & 0x1F));
    }
    return 0;
}

static int _cb_enable_listen_gamepad(lua_State *L)  { (void)L; g_gamepad_abs_listen = true;  return 0; }
static int _cb_disable_listen_gamepad(lua_State *L) { (void)L; g_gamepad_abs_listen = false; return 0; }
static int _cb_enable_listen_motion(lua_State *L)   { (void)L; g_motion_listen     = true;  return 0; }
static int _cb_disable_listen_motion(lua_State *L)  { (void)L; g_motion_listen     = false; return 0; }

//--------------------------------------------------------------------+
// 外部 rebind 回调（默认空实现，应用层覆盖 —— 本工程实现在 input_app.c）
//--------------------------------------------------------------------+

__attribute__((weak))
void lua_binder_rebind(void) {}

//--------------------------------------------------------------------+
// 生命周期
//--------------------------------------------------------------------+

bool lua_binder_init(const char *script, size_t len) {
    // 清零 bitmap；创建 VM + 注册内建回调
    memset(g_key_listen, 0, sizeof(g_key_listen));
    g_mouse_listen       = 0;
    g_mouse_move_listen  = false;
    g_mouse_wheel_listen = false;
    g_gamepad_listen     = 0;
    g_gamepad_abs_listen = false;
    g_motion_listen      = false;

    if (!lua_core_init(NULL, 0)) return false;

    // 在脚本加载前注册本模块的自有回调
    lua_core_register("enable_listen_keys",        _cb_enable_listen_keys);
    lua_core_register("disable_listen_keys",       _cb_disable_listen_keys);
    lua_core_register("enable_listen_mouse_btn",   _cb_enable_listen_mouse_btn);
    lua_core_register("disable_listen_mouse_btn",  _cb_disable_listen_mouse_btn);
    lua_core_register("enable_listen_mouse_move",  _cb_enable_listen_mouse_move);
    lua_core_register("disable_listen_mouse_move", _cb_disable_listen_mouse_move);
    lua_core_register("enable_listen_mouse_wheel", _cb_enable_listen_mouse_wheel);
    lua_core_register("disable_listen_mouse_wheel",_cb_disable_listen_mouse_wheel);
    lua_core_register("enable_listen_gamepad_btn", _cb_enable_listen_gamepad_btn);
    lua_core_register("disable_listen_gamepad_btn",_cb_disable_listen_gamepad_btn);
    lua_core_register("enable_listen_gamepad",     _cb_enable_listen_gamepad);
    lua_core_register("disable_listen_gamepad",    _cb_disable_listen_gamepad);
    lua_core_register("enable_listen_motion",      _cb_enable_listen_motion);
    lua_core_register("disable_listen_motion",     _cb_disable_listen_motion);

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
    g_lid_tick           = lua_core_find("tick");

    // 触摸板节流基线清零：新 VM 从零起
    s_tp_last_meta = 0;
    s_tp_last_dims = 0;
    memset(s_tp_last_x, 0, sizeof(s_tp_last_x));
    memset(s_tp_last_y, 0, sizeof(s_tp_last_y));

    // 新 VM：首个 tick 的 dt 从 0 起
    g_tick_primed = false;

    return true;
}

void lua_binder_deinit(void) {
    g_lid_on_key = g_lid_on_mouse_btn = g_lid_on_mouse_move = g_lid_on_mouse_wheel = -1;
    g_lid_on_gamepad_btn = g_lid_on_gamepad_abs = g_lid_on_motion = -1;
    g_lid_on_touchpad = g_lid_on_dropped = g_lid_tick = -1;
    memset(g_key_listen, 0, sizeof(g_key_listen));
    g_mouse_listen       = 0;
    g_mouse_move_listen  = false;
    g_mouse_wheel_listen = false;
    g_gamepad_listen     = 0;
    g_gamepad_abs_listen = false;
    g_motion_listen      = false;
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
// C → Lua: 事件转发（入口 bitmap/标志 O(1) 过滤）
//--------------------------------------------------------------------+

bool lua_binder_on_key(uint8_t keycode, bool down) {
    if (!(g_key_listen[keycode >> 6] & (1ULL << (keycode & 0x3F)))) return false;
    if (g_lid_on_key < 0) return false;

    lua_core_push_int(keycode);
    lua_core_push_bool(down);
    if (!lua_core_pcall_id(g_lid_on_key, 2, 1)) return false;
    bool intercepted = false;
    lua_core_pop_bool(&intercepted);
    return intercepted;
}

bool lua_binder_on_mouse_btn(uint8_t button, bool down) {
    if (!(g_mouse_listen & (1U << (button & 7)))) return false;
    if (g_lid_on_mouse_btn < 0) return false;

    lua_core_push_int(button);
    lua_core_push_bool(down);
    if (!lua_core_pcall_id(g_lid_on_mouse_btn, 2, 1)) return false;
    bool intercepted = false;
    lua_core_pop_bool(&intercepted);
    return intercepted;
}

// 鼠标移动：dx/dy 以指针传入，脚本返回 true 则清零跳过后续处理
bool lua_binder_on_mouse_move(int32_t *dx, int32_t *dy) {
    if (!g_mouse_move_listen) return false;
    if (g_lid_on_mouse_move < 0) return false;

    lua_core_push_int(*dx);
    lua_core_push_int(*dy);
    if (!lua_core_pcall_id(g_lid_on_mouse_move, 2, 1)) return false;
    bool intercepted = false;
    lua_core_pop_bool(&intercepted);
    if (intercepted) { *dx = 0; *dy = 0; }
    return intercepted;
}

// 鼠标滚轮：wheel 以指针传入，脚本返回 true 则清零跳过后续处理
bool lua_binder_on_mouse_wheel(int32_t *wheel) {
    if (!g_mouse_wheel_listen) return false;
    if (g_lid_on_mouse_wheel < 0) return false;

    lua_core_push_int(*wheel);
    if (!lua_core_pcall_id(g_lid_on_mouse_wheel, 1, 1)) return false;
    bool intercepted = false;
    lua_core_pop_bool(&intercepted);
    if (intercepted) { *wheel = 0; }
    return intercepted;
}

bool lua_binder_on_gamepad_btn(uint8_t code, bool pressed) {
    if (!(g_gamepad_listen & (1U << (code & 0x1F)))) return false;
    if (g_lid_on_gamepad_btn < 0) return false;

    lua_core_push_int((int)code);
    lua_core_push_bool(pressed);
    if (!lua_core_pcall_id(g_lid_on_gamepad_btn, 2, 1)) return false;
    bool intercepted = false;
    lua_core_pop_bool(&intercepted);
    return intercepted;
}

bool lua_binder_on_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                               int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt) {
    (void)slot;
    if (!g_gamepad_abs_listen) return false;
    if (g_lid_on_gamepad_abs < 0) return false;

    lua_core_push_int(ls_x);
    lua_core_push_int(ls_y);
    lua_core_push_int(rs_x);
    lua_core_push_int(rs_y);
    lua_core_push_int(lt);
    lua_core_push_int(rt);
    if (!lua_core_pcall_id(g_lid_on_gamepad_abs, 6, 1)) return false;
    bool intercepted = false;
    lua_core_pop_bool(&intercepted);
    return intercepted;
}

void lua_binder_on_motion(int8_t slot, int16_t gx, int16_t gy, int16_t gz,
                          int16_t ax, int16_t ay, int16_t az) {
    (void)slot;
    if (!g_motion_listen) return;
    if (g_lid_on_motion < 0) return;

    lua_core_push_int(gx);
    lua_core_push_int(gy);
    lua_core_push_int(gz);
    lua_core_push_int(ax);
    lua_core_push_int(ay);
    lua_core_push_int(az);
    (void)lua_core_pcall_id(g_lid_on_motion, 6, 0);
}

// 触摸板快照：瞬时量直投 on_touchpad(meta, dims, x0,y0, ..., x9,y9)。
// 内置节流：与上次已分发的快照逐字节相同时不调用——静止手指的定频报文
// 洪峰在此挡掉。脚本未定义回调时只有一次整数比较。
void lua_binder_on_touchpad(uint32_t meta, uint32_t dims,
                            const uint16_t *x, const uint16_t *y) {
    if (g_lid_on_touchpad < 0) return;
    if (meta == s_tp_last_meta && dims == s_tp_last_dims &&
        memcmp(x, s_tp_last_x, sizeof(s_tp_last_x)) == 0 &&
        memcmp(y, s_tp_last_y, sizeof(s_tp_last_y)) == 0) return;
    s_tp_last_meta = meta;
    s_tp_last_dims = dims;
    memcpy(s_tp_last_x, x, sizeof(s_tp_last_x));
    memcpy(s_tp_last_y, y, sizeof(s_tp_last_y));

    lua_core_push_int((int)meta);
    lua_core_push_int((int)dims);
    for (int i = 0; i < HIDKIT_TOUCHPAD_SLOTS; i++) {
        lua_core_push_int(x[i]);
        lua_core_push_int(y[i]);
    }
    (void)lua_core_pcall_id(g_lid_on_touchpad, 2 + 2 * HIDKIT_TOUCHPAD_SLOTS, 0);
}

void lua_binder_on_dropped(int8_t slot, uint16_t vid, uint16_t pid) {
    if (g_lid_on_dropped < 0) return;

    lua_core_push_int(slot);
    lua_core_push_int(vid);
    lua_core_push_int(pid);
    (void)lua_core_pcall_id(g_lid_on_dropped, 3, 0);
}

//--------------------------------------------------------------------+
// C → Lua: 周期处理
//--------------------------------------------------------------------+

bool lua_binder_process(void) {
    if (g_lid_tick < 0) return false;
    uint64_t now = time_us_64();
    // dt = 距上次 tick 的微秒差（首个 tick 为 0）。now 单调(uint64)，dt 恒非负
    uint64_t dt = g_tick_primed ? (now - g_last_tick_us) : 0;
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
