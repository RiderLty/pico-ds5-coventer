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
 * （由 pico-hid-mapper 的 src/lua_binder.c 按本工程 hidkit 回调精简移植）。
 *
 * 事件过滤：Lua 脚本通过 enable_listen_* 声明关注的事件，C 侧 bitmap/标志
 * O(1) 过滤，不关心的事件零开销跳过 Lua VM。
 *
 * 拦截语义（与 mapper 一致）：on_key / on_mouse_btn / on_gamepad_btn /
 * on_mouse_move / on_mouse_wheel / on_gamepad_abs 返回 true = 拦截，
 * C 侧默认处理（打包进跨核队列）被跳过；on_motion / on_touchpad /
 * on_dropped 无拦截语义。暂无 init()/tick() 回调（后期需要再加）。
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

//--- C → Lua 事件转发 ------------------------------------------------

// 键盘按键（keycode = HID Usage ID）。返回 true = Lua 拦截
bool lua_binder_on_key(uint8_t keycode, bool down);

// 鼠标按键（button = 0..7）。返回 true = Lua 拦截
bool lua_binder_on_mouse_btn(uint8_t button, bool down);

// 鼠标移动。Lua 返回 true 则把 *dx/*dy 清零（跳过 C 默认处理）并返回 true
bool lua_binder_on_mouse_move(int32_t *dx, int32_t *dy);

// 鼠标滚轮。Lua 返回 true 则把 *wheel 清零（跳过 C 默认处理）并返回 true
bool lua_binder_on_mouse_wheel(int32_t *wheel);

// 手柄按键（code = hidkit BTN_*）。返回 true = Lua 拦截
bool lua_binder_on_gamepad_btn(uint8_t code, bool pressed);

// 手柄绝对状态（每份解析成功的报文；轴 -32767..32767，扳机 0..32767）。
// 返回 true = Lua 拦截（跳过 C 默认打包）
bool lua_binder_on_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
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
