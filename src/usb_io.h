#ifndef USB_IO_H
#define USB_IO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * usb_io — custom HID 传输适配器（协议 v1）。
 *
 * 通道核心与协议常量见 proto.h（INFO/PRINT/RESET，[flags][data] 块）。
 * 本文件是 HID 侧适配：块 ↔ 满 63B HID 报文（含 seq、补零），并向
 * proto_log_write 广播注册。UART 传输平级适配器见 uart_proto.c。
 */

#define IO_REPORT_ID_IN    0x01
#define IO_REPORT_ID_OUT   0x02
#define IO_PAYLOAD_SIZE    63u    // 1B Report ID + ≤63B = 64B EP size

#define USB_IO_INST_NONE   0xFFu

// 初始化（双核启动前调用）：proto 核心 + HID 通道 + UART 通道一并就绪
void usb_io_init(void);

// 设置 IO 接口的 TinyUSB HID 实例号；USB_IO_INST_NONE = 未启用。
// 仅 usb_km_task 在重枚举窗口内调用，同一时刻也切换配置描述符，无并发。
void usb_io_set_instance(uint8_t instance);

uint8_t usb_io_instance(void);
bool usb_io_enabled(void);

// DEBUG 日志的 PRINT 入口（debug_log 调用）：广播到所有激活传输
void usb_log_write(const uint8_t *data, uint16_t len);

// OUT 端点收到主机报文（payload，已剥 Report ID）；未启用时丢弃
void usb_io_rx(const uint8_t *payload, uint16_t len);

// core0 主循环调用：通道 TX 任务
void usb_io_task(void);

#endif // USB_IO_H
