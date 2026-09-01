#ifndef HID_OUTPUT_H
#define HID_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "hid_parser.h"

/*
 * 输出中间层（由 pico-hid-forward 改造）：
 * hid_host_app 产出的统一事件固定转发到 out_device（DS5 模拟）。
 * 串口文本/二进制输出与模式切换已移除。
 */

// 报文来源设备信息
typedef struct {
    uint16_t vid, pid;
    uint8_t  dev_addr;   // 端口（USB 设备地址）
} hid_dev_info_t;

// 输出实现的统一接口
typedef struct {
    void (*mount)(uint8_t instance, const hid_dev_info_t *info, uint8_t proto);
    void (*umount)(uint8_t instance, const hid_dev_info_t *info);
    void (*error)(const char *msg);
    void (*keyboard)(uint8_t instance, const hid_dev_info_t *info,
                     const uint8_t *report, uint8_t len);
    void (*mouse)(uint8_t instance, const hid_dev_info_t *info,
                  const hid_mouse_frame_t *frame);
    void (*generic)(uint8_t instance, const hid_dev_info_t *info,
                    const uint8_t *report, uint16_t len);
} hid_output_ops_t;

extern const hid_output_ops_t hid_output_device_ops;   // out_device.c（DS5 模拟）

// 统一事件入口（hid_host_app 在 core1 上调用；单线程，无并发问题）
void hid_output_mount(uint8_t instance, const hid_dev_info_t *info, uint8_t proto);
void hid_output_umount(uint8_t instance, const hid_dev_info_t *info);
void hid_output_error(const char *msg);
void hid_output_keyboard(uint8_t instance, const hid_dev_info_t *info,
                         const uint8_t *report, uint8_t len);
void hid_output_mouse(uint8_t instance, const hid_dev_info_t *info,
                      const hid_mouse_frame_t *frame);
void hid_output_generic(uint8_t instance, const hid_dev_info_t *info,
                        const uint8_t *report, uint16_t len);

#endif // HID_OUTPUT_H
