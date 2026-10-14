/*
 * kbd_hid — USB HID NKRO 键盘设备（可选的第二个「输出到宿主」的接口）
 *
 * 报告描述符（QMK 风格双报告，89 字节）：
 *   Report ID 1：6KRO 报告（接口协议 None，不提供 Boot 协议）（修饰键 + 保留 + 6 键码）+ LED 输出
 *   Report ID 2：NKRO 位图（修饰键 8 bit + 键盘用法 0x04-0x73 共 112 键位图）
 *
 * 本模块只管 USB 侧（描述符 / 发报告 / LED 状态）；「什么时候发、发什么状态」由
 * kbd_out.c 负责（Lua dkb_* 接口）。
 *
 * 注意：HID 实例号不是常量——配置描述符里接口的追加顺序是
 *   DS5(恒在) → [16bit 鼠标] → [本键盘]
 * 所以实例号 = 1 + 鼠标接口是否存在，由 usb_km_task() 在重枚举窗口内设定。
 */
#ifndef KBD_HID_H
#define KBD_HID_H

#include <stdint.h>
#include <stdbool.h>

// 0x04-0x73 共 112 键 → 112/8 = 14 字节位图
#define KBD_HID_NKRO_BYTES     14
// 键盘键位图的取值范围（HID Keyboard page 用法号）
#define KBD_HID_USAGE_MIN      0x04
#define KBD_HID_USAGE_MAX      0x73
// 修饰键（左 Ctrl..右 GUI）
#define KBD_HID_MOD_MIN        0xE0
#define KBD_HID_MOD_MAX        0xE7
// 实例号无效值
#define KBD_HID_INST_NONE      0xFF

// 报告描述符字节数（usb_descriptors.c 的 wDescriptorLength 用，模块内有 _Static_assert）
#define KBD_HID_REPORT_DESC_LEN  89

// 报告描述符（给 tud_hid_descriptor_report_cb 分发用）
uint8_t const *kbd_hid_report_desc(void);

// 设置/查询本键盘在 device 端的 HID 实例号（usb_km_task 在重枚举窗口内更新）
void kbd_hid_set_instance(uint8_t instance);
uint8_t kbd_hid_instance(void);

// 键盘接口是否已启用（实例号已设定；不代表主机已完成枚举）
bool kbd_hid_enabled(void);

// 接口是否就绪（未启用恒 false）
bool kbd_hid_ready(void);

// 发送 NKRO 位图报告（Report ID 2）：modifier 8 bit + 14 字节位图
bool kbd_hid_send_nkro(uint8_t modifier, const uint8_t bitmap[KBD_HID_NKRO_BYTES]);

// 最近一次主机写回的 LED 状态（Num/Caps/Scroll/Compose/Kana，bit0-4）
uint8_t kbd_hid_led_state(void);

// 由 tud_hid_set_report_cb 调用：记录主机写回的 LED 状态
void kbd_hid_set_led(uint8_t leds);

#endif // KBD_HID_H
