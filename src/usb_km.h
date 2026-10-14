#ifndef USB_KM_H
#define USB_KM_H

#include <stdbool.h>

/*
 * usb_km — NKRO 键盘 / 16bit 鼠标输出接口的运行期开关（参考 pico-hid-mapper
 * 的 usb_desc_config_apply + runtime reconfigure 路径）。
 *
 * 默认**全关**：boot 时配置描述符回调返回 flash 基线（DS5 + 音频），与无
 * km 模块时逐字节一致。Lua enable_km(kbd, mouse) 请求变更（core1）→
 * usb_km_task()（core0 主循环）在重枚举窗口内惰性 apply + USB 重枚举
 * （tud_disconnect → apply → tud_connect，期间设备从主机消失约 0.5s，
 * DS5 上报同步暂停）。
 *
 * 状态默认全关；usb_km_init 在双核启动前通过 SDK striped 锁池初始化
 * 队列临界区，兼容 RP2350 软件锁，并保留独占锁池给 TinyUSB。
 *
 * 接口在配置描述符里的追加顺序：DS5(itf3) → [鼠标 itf4/EP0x86] → [键盘 itf4或5/EP0x87]
 * → TinyUSB HID 实例号：DS5=0，鼠标（启用时）=1，键盘 = 1 + 鼠标有无。
 */

// 请求变更开关（core1 / Lua enable_km 调用；异步生效，由 usb_km_task 完成）
void usb_km_request(bool kbd, bool mouse);

// 队列和状态初始化（必须在双核启动前调用）。
void usb_km_init(void);

// core0 主循环调用：有未应用的请求则重枚举切换（无请求时仅几次原子读）
void usb_km_task(void);

// core0 主循环调用：键盘/鼠标输出队列出队发送（接口未启用时内部直接返回）
void usb_km_service(void);

// 当前生效状态（Lua dkb_is_on / dmo_is_on 查询用）
bool usb_km_kbd_enabled(void);
bool usb_km_mouse_enabled(void);

// 动态组装配置描述符（定义在 usb_descriptors.c，与基线数组同文件便于自检）。
// mouse/kbd = 是否追加对应接口；追加顺序固定 鼠标 → 键盘。
// 必须在 tud_init 之前、或 tud_disconnect 之后的重枚举窗口内调用。
void usb_km_apply(bool mouse, bool kbd);

#endif // USB_KM_H
