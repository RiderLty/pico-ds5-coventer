#ifndef USB_IO_H
#define USB_IO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * usb_io — custom HID 通讯接口的运行期开关与 echo 数据通路。
 *
 * 单接口双中断端点、vendor usage page、无 IAD —— usbccgp 保持 legacy
 * 分组，DS5 音频不受影响（CDC/NCM 等带 IAD 的类在 Windows 上与 DS5
 * 音频基线互斥，见 exp/cdc-serial 分支的教训）。
 *
 * 全部在 core0：OUT 端点数据由 TinyUSB 经 tud_hid_set_report_cb 送达
 * （usb_descriptors.c 调 usb_io_receive），echo 由主循环的 usb_io_task
 * 发回，无跨核并发。接口未启用时不触碰任何端点。
 *
 * 报文（见 usb_descriptors.c 的 desc_hid_report_io）：
 *   Report ID 1 = 设备→主机（63B payload）
 *   Report ID 2 = 主机→设备（63B payload）
 *   echo 回包 = "pico" 前缀 + 原 payload 前 59B
 */

#define IO_REPORT_ID_IN    0x01
#define IO_REPORT_ID_OUT   0x02
#define IO_PAYLOAD_SIZE    63u   // 1B Report ID + 63B = 64B（EP size）

#define USB_IO_INST_NONE   0xFFu

// 设置 IO 接口的 TinyUSB HID 实例号；USB_IO_INST_NONE = 未启用。
// 仅 usb_km_task 在重枚举窗口内调用，同一时刻也切换配置描述符，无并发。
void usb_io_set_instance(uint8_t instance);

uint8_t usb_io_instance(void);
bool usb_io_enabled(void);

// OUT 端点收到主机报文（payload，不含 Report ID）；未启用时丢弃
void usb_io_receive(const uint8_t *payload, uint16_t len);

// core0 主循环调用：echo —— "pico" 前缀 + 原 payload，Report ID 1 发回
void usb_io_task(void);

#endif // USB_IO_H
