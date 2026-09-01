#ifndef HID_HOST_APP_H
#define HID_HOST_APP_H

/*
 * HID 报文处理模块。
 *
 * 实现 TinyUSB Host 的 HID 回调（tuh_hid_*_cb），完成：
 *   - 挂载时鼠标报告描述符解析
 *   - 报文接收与分发（键盘原始报文 / 鼠标归一化帧 / 其他设备原始报文）
 *   - 产出统一事件交给 hid_output 中间层
 *
 * 全部回调运行在 core1 的 tuh_task() 上下文，单线程无并发问题。
 */

#include <stdint.h>

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len);
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance);
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len);

#endif // HID_HOST_APP_H
