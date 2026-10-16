#ifndef INPUT_APP_H
#define INPUT_APP_H

#include <stdint.h>
#include <stdbool.h>

/*
 * hidkit 输入接线层（core1 侧，参考 pico-hid-mapper 的 src/hidkit_app.c 精简）。
 *
 * TinyUSB HID host 回调 → hidkit 解析（键鼠 / HID 手柄 / XInput 类驱动 /
 * Switch Pro 握手库）→ 维护小状态并打包成跨核队列记录。
 *
 * 所有函数（含 TinyUSB 回调）只在 core1 调用。
 */

// Start before either USB host or Bluetooth: clear slots + hidkit_init().
void input_app_init(void);
#if ENABLE_DS5_BT
void input_app_bt_mount(void);
#endif

// tuh_hid_mount_cb 转发：desc_report 可为 NULL（描述符超长时）
void input_app_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num,
                     uint8_t itf_proto, uint8_t const *desc_report, uint16_t desc_len);

// tuh_hid_umount_cb 转发
void input_app_umount(uint8_t instance);

// tuh_hid_report_received_cb 转发；返回是否被 hidkit 消费（未消费交宿主兜底）
bool input_app_report(uint8_t instance, uint8_t const *report, uint16_t len);

// core1 主循环每轮调用：推进 Switch Pro 握手状态机（须与 tuh 回调同核同循环）
void input_app_task(void);
// core1, before replacing a Lua VM: release held keyboard/mouse outputs.
void input_app_release_outputs(void);

#endif // INPUT_APP_H
