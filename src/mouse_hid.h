/*
 * DMO（Direct Mouse Output）鼠标 HID 接口
 *
 * 独立 HID 接口（ITF 3）+ 独立 IN 端点（0x86），bInterval = 1，**无 Report ID**。
 *
 * 报文 6 字节（小端）：
 *   [0]    buttons   bit0=左 bit1=右 bit2=中 bit3=后退 bit4=前进（bit5..7 恒 0）
 *   [1..2] dx int16  相对位移，有符号（正 = 向右）
 *   [3..4] dy int16  相对位移，有符号（正 = 向下，与引擎像素坐标同向）
 *   [5]    wheel i8   滚轮格数（正 = 上滚，HID 惯例"远离用户"）
 *
 * 是否把这个接口放进配置描述符由开机读 KV 决定（见 usb_descriptors.c 的
 * mouse 相关接口），因此本模块始终编译、运行时只做"发得出去就发"。
 */
#ifndef MOUSE_HID_H
#define MOUSE_HID_H

#include <stdint.h>
#include <stdbool.h>

// 报告描述符字节数（usb_descriptors.c 的 wDescriptorLength 用）。
// mouse_hid.c 有 _Static_assert 守着它与实际数组长度一致。
#define MOUSE_HID_DESC_LEN    59
#define MOUSE_HID_REPORT_LEN  6

// 报文里的按键位（序号与 defines.h 的 MOUSE_BUTTON_* 一致，1:1 直搬）
#define MOUSE_HID_BIT_LEFT     0x01
#define MOUSE_HID_BIT_RIGHT    0x02
#define MOUSE_HID_BIT_MIDDLE   0x04
#define MOUSE_HID_BIT_BACK     0x08
#define MOUSE_HID_BIT_FORWARD  0x10
#define MOUSE_HID_BIT_MASK     0x1F

// 报文结构（与队列元素共用，避免两处各写一遍打包逻辑）
typedef struct __attribute__((packed)) {
  uint8_t buttons;
  int16_t dx;
  int16_t dy;
  int8_t  wheel;
} mouse_report_t;

_Static_assert(sizeof(mouse_report_t) == MOUSE_HID_REPORT_LEN,
               "mouse_report_t 必须是 6 字节，且无隐式填充");

// 该 HID 实例号（接口在描述符里排在触屏 HID 之后 → TinyUSB 分配 instance 1）
#define MOUSE_HID_INSTANCE  1

uint8_t const *mouse_hid_report_desc(void);

// 端点是否可发（未枚举/主机未取走上一帧时为 false）
bool mouse_hid_ready(void);

// 发送一条鼠标报文；返回 true 表示已提交
bool mouse_hid_send(const mouse_report_t *rep);

#endif // MOUSE_HID_H
