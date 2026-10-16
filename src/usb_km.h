#ifndef USB_KM_H
#define USB_KM_H

#include <stdbool.h>
#include <stdint.h>

/*
 * usb_km — NKRO 键盘 / 16bit 鼠标 / custom IO 通讯接口的运行期开关（参考
 * pico-hid-mapper 的 usb_desc_config_apply + runtime reconfigure 路径）。
 *
 * 默认**全关**：boot 时配置描述符回调返回 flash 基线（DS5 + 音频），与无
 * km 模块时逐字节一致。Lua dev_enable(kbd, mouse, io) 请求变更（三态参数，
 * 见 usb_km_request）
 * （core1）→ usb_km_task()（core0 主循环）在重枚举窗口内惰性 apply +
 * USB 重枚举（tud_disconnect → apply → tud_connect，期间设备从主机消失
 * 约 0.5s，DS5 上报同步暂停）。
 *
 * 状态默认全关；usb_km_init 在双核启动前通过 SDK striped 锁池初始化
 * 队列临界区，兼容 RP2350 软件锁，并保留独占锁池给 TinyUSB。
 *
 * 接口在配置描述符里的追加顺序：DS5(itf3) → [鼠标 itf4/EP0x86]
 * → [键盘 itf4或5/EP0x87] → [IO itf紧随键盘，EP0x88 IN/0x08 OUT]
 * → TinyUSB HID 实例号：DS5=0，鼠标（启用时）=1，键盘 = 1 + 鼠标有无，
 * IO 紧随键盘。所有追加接口均为单接口 HID、无 IAD，usbccgp 保持 legacy
 * 分组（带 IAD 的类与 DS5 音频基线在 Windows 上互斥）。
 */

// 目标状态位（core1 Lua dev_enable 写，core0 usb_km_task 读）
enum {
    USB_KM_BIT_KBD = 1u << 0,
    USB_KM_BIT_MOUSE = 1u << 1,
    USB_KM_BIT_IO = 1u << 2,
};

// 三态合并：cur 当前掩码，val <0 保持该位、0 清位、>0 置位。
// 抽成纯函数是为了能被主机侧单测直接验证（不依赖 USB 栈）。
static inline uint32_t usb_km_merge_tri(uint32_t cur, int val, uint32_t bit)
{
    if (val < 0) return cur;
    return val > 0 ? (cur | bit) : (cur & ~bit);
}

// 请求变更开关（core1 / Lua dev_enable 调用；异步生效，由 usb_km_task 完成）。
// 每个参数三态：-1 保持当前目标不变、0 关闭、1 开启。
void usb_km_request(int kbd, int mouse, int io);

// 队列和状态初始化（必须在双核启动前调用）。
void usb_km_init(void);

// core0 主循环调用：有未应用的请求则重枚举切换（无请求时仅一次原子读）
void usb_km_task(void);

// core0 主循环调用：键盘/鼠标输出队列出队发送（接口未启用时内部直接返回）
void usb_km_service(void);

// 当前生效状态（Lua kbd_is_on / mouse_is_on / io_is_on 查询用）
bool usb_km_kbd_enabled(void);
bool usb_km_mouse_enabled(void);
bool usb_km_io_enabled(void);

// 动态组装配置描述符（定义在 usb_descriptors.c，与基线数组同文件便于自检）。
// mouse/kbd/io = 是否追加对应接口；追加顺序固定 鼠标 → 键盘 → IO。
// 必须在 tud_init 之前、或 tud_disconnect 之后的重枚举窗口内调用。
void usb_km_apply(bool mouse, bool kbd, bool io);

#endif // USB_KM_H
