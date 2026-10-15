#ifndef USB_IO_H
#define USB_IO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * usb_io — custom HID 通讯协议 v1（传输层 + 最小命令集），用于主机↔设备验证。
 *
 * 单接口双中断端点、vendor usage page、无 IAD（usbccgp 保持 legacy 分组，
 * DS5 音频不受影响）。报文：Report ID 2 = 主机→设备，ID 1 = 设备→主机，
 * 线上 1B Report ID + ≤63B 块。
 *
 * 分块格式（63B 块内）：
 *   [seq:1][flags:1][data:0..61]
 *   flags: START 0x01 / END 0x02 / RESET 0x04（START|END = 单块完整消息）
 *   START 的 data 头：[cmd:1][msg_id:2 LE][varint total_len][首段 data]
 * USB HID 链路可靠有序（无丢包/乱序），故无逐包 CRC、无重传；seq 仅用于
 * 对账，流控靠端点 NAK 背压。
 *
 * 命令（msg_id 关联请求↔响应；设备主动消息 msg_id=0）：
 *   0x01 INFO  主机→设备查询；设备回同 cmd + 同 msg_id 的文本
 *   0x02 PRINT 设备→主机日志（任意长度，短消息单块、长消息分块流式，
 *              主机重组后无感）；Lua print 经 4KB 跨核队列接入
 *   0x03 RESET 主机→设备：清会话状态（连接/重连后先发）
 *
 * 设备端缓冲：4KB 跨核消息环（core1 Lua print 生产 + core0 应答生产，
 * core0 usb_io_task 消费），生产者非阻塞，满则整条丢弃并计数。
 */

// ---- 命令与块 flags ----
#define PROTO_CMD_INFO    0x01
#define PROTO_CMD_PRINT   0x02
#define PROTO_CMD_RESET   0x03

#define PROTO_F_START     0x01
#define PROTO_F_END       0x02
#define PROTO_F_RESET     0x04

#define IO_REPORT_ID_IN   0x01
#define IO_REPORT_ID_OUT  0x02
#define IO_PAYLOAD_SIZE   63u    // 1B Report ID + ≤63B 块 = 64B EP size

#define USB_IO_INST_NONE  0xFFu

// 初始化（必须在双核启动前调用：初始化跨核环的临界区）
void usb_io_init(void);

// 设置 IO 接口的 TinyUSB HID 实例号；USB_IO_INST_NONE = 未启用。
// 仅 usb_km_task 在重枚举窗口内调用，同一时刻也切换配置描述符，无并发。
void usb_io_set_instance(uint8_t instance);

uint8_t usb_io_instance(void);
bool usb_io_enabled(void);

// Lua print 出口（core1，非阻塞）：整条文本作为一个 PRINT 消息入环。
// 环满或单条超出环容量时整条丢弃并计数，绝不阻塞调用方。
void usb_log_write(const uint8_t *data, uint16_t len);

// 主机→设备收到一块（OUT 端点 payload，不含 Report ID；usb_descriptors.c 调用）
void usb_io_rx(const uint8_t *payload, uint16_t len);

// core0 主循环调用：从环出队消息 → 分块发送（未启用时直接返回）
void usb_io_task(void);

#endif // USB_IO_H
