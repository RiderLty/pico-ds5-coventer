#ifndef LUA_BINDER_H
#define LUA_BINDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * lua_binder — lua_core 的唯一调用者，对外隐藏 Lua 实现细节
 * （由 pico-hid-mapper 的 src/lua_binder.c 按本工程架构精简移植）。
 *
 * 架构：输入与输出完全解耦，中间只有 Lua —— 本模块只做**单向事件通知**
 * （C → Lua 回调，无返回值、无拦截语义；mapper 版的"返回 true 拦截 C 默认
 * 处理"语义已删除，本工程 C 侧没有默认映射）。输出由脚本直接调用 C 注册
 * 的 dkb_* 、dmo_* 、pad_* 输出函数。
 *
 * 事件全量监听：hidkit 事件一律直投 Lua 回调，无需 enable_listen_* 声明。
 * 未定义的回调只需一次整数比较即返回（g_lid_* < 0 检查）。
 */

// 热重载回调：lua_binder_init 在创建新 VM 后、加载脚本前调用。
// 应用层在此回调中通过 lua_binder_bind 注册 C 函数（本工程实现在 input_app.c）。
extern void lua_binder_rebind(void);

// 初始化：创建 VM + 注册内建 listen 回调 + rebind + 加载脚本。
// 重复调用会销毁旧 VM 并重建。
bool lua_binder_init(const char *script, size_t len);
void lua_binder_deinit(void);
bool lua_binder_is_ready(void);

// 注册 C 函数到 Lua 全局表（func: lua_CFunction）
bool lua_binder_bind(const char *name, void *func);

//--- C → Lua 事件转发（单向通知，无返回值）----------------------------

// 键盘按键（keycode = HID Usage ID）
void lua_binder_on_key(uint8_t keycode, bool down);

// 鼠标按键（button = 0..7）
void lua_binder_on_mouse_btn(uint8_t button, bool down);

// 鼠标位移
void lua_binder_on_mouse_move(int32_t dx, int32_t dy);

// 鼠标滚轮
void lua_binder_on_mouse_wheel(int32_t wheel);

// 手柄按键（code = hidkit BTN_*）
void lua_binder_on_gamepad_btn(uint8_t code, bool pressed);

// 手柄绝对状态（每份解析成功的报文；轴 -32767..32767，扳机 0..32767）
void lua_binder_on_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                               int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt);

// 手柄六轴（陀螺 ±32767=±2000dps，加速度 ±32767=±4g）。无拦截语义
void lua_binder_on_motion(int8_t slot, int16_t gx, int16_t gy, int16_t gz,
                          int16_t ax, int16_t ay, int16_t az);

// 触摸板快照 on_touchpad(meta, dims, x0,y0, ..., x9,y9)，内置逐字节去重
void lua_binder_on_touchpad(uint32_t meta, uint32_t dims,
                            const uint16_t *x, const uint16_t *y);

// 设备被丢弃通知 on_dropped(slot, vid, pid)。无拦截语义
void lua_binder_on_dropped(int8_t slot, uint16_t vid, uint16_t pid);

//--- 周期处理 ----------------------------------------------------------

// 调用 Lua 的 tick(dt_us)，dt = 距上次调用的微秒差（内部计时）。
// core1 主循环每轮调用（不节流）；脚本未定义 tick 时只有一次整数比较。
bool lua_binder_process(void);

//--- 诊断 ------------------------------------------------------------

// 最近一次脚本加载错误串；无错返回 ""
const char *lua_binder_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* LUA_BINDER_H */
