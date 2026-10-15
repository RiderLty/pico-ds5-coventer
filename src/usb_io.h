#ifndef USB_IO_H
#define USB_IO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * usb_io — WinUSB vendor 接口传输适配器（协议 v1）。
 *
 * 通道核心与协议常量见 proto.h（INFO/PRINT/RESET/TEST，[flags][data] 块）。
 * 本文件是 WinUSB 侧适配：bulk 端点对 + SLIP 转义 + CRC16 帧（与 UART
 * 适配器同构——bulk 是可靠字节流，CRC 仅作应用级兜底），MS OS 1.0 描述符
 * 的描述符/控制请求部分在 usb_descriptors.c。向 proto_log_write 广播注册。
 */

#define USB_IO_INST_NONE   0xFFu

// 初始化（双核启动前调用）：proto 核心 + WinUSB 通道 + UART 通道一并就绪
void usb_io_init(void);

// 设置 IO 接口的配置描述符追加状态；USB_IO_INST_NONE = 未启用。
// 仅 usb_km_task 在重枚举窗口内调用，同一时刻也切换配置描述符，无并发。
void usb_io_set_instance(uint8_t instance);

uint8_t usb_io_instance(void);
bool usb_io_enabled(void);

// DEBUG 日志的 PRINT 入口（debug_log 调用）：广播到所有激活传输
void usb_log_write(const uint8_t *data, uint16_t len);

// core0 主循环调用：vendor FIFO 收字节解码 + 通道 TX 任务
void usb_io_task(void);

#endif // USB_IO_H
