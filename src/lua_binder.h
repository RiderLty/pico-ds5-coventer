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
 * 架构：输入与输出完全解耦，中间只有 Lua —— 本模块负责 C → Lua 事件回调。
 * 除板载 BOOTSEL 按键外，回调返回值都会被忽略；on_boot_button 返回真值可
 * 拦截释放时的默认 WinUSB 开启行为。输出由脚本直接调用 C 注册
 * 的 kbd_* 、mouse_* 、pad_* 输出函数。
 *
 * 事件全量监听：hidkit 事件一律直投 Lua 回调，无需任何声明 —— 脚本里
 * 定义哪个回调就收哪个事件。未定义的回调只需一次整数比较即返回
 * （g_lid_* < 0 检查）。
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

//--- C → Lua 事件转发（BOOTSEL 回调返回拦截结果，其余回调无返回值）------

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

// 触摸板快照 on_touchpad(meta, dims, x0, y0, x1, y1)，仅前两点坐标参与去重
void lua_binder_on_touchpad(uint32_t meta, uint32_t dims,
                            const uint16_t *x, const uint16_t *y);

// 设备被丢弃通知 on_dropped(slot, vid, pid)。无拦截语义
void lua_binder_on_dropped(int8_t slot, uint16_t vid, uint16_t pid);

// 板载 BOOTSEL 按键 on_boot_button(pressed)。返回 true 表示拦截默认行为；
// false、nil、无返回值、未绑定或调用出错均返回 false。
bool lua_binder_on_boot_button(bool pressed);

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
