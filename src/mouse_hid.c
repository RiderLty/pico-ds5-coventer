/*
 * DMO 鼠标 HID：报告描述符与发送接口
 *
 * 见 mouse_hid.h 的报文格式说明。这里只做两件事：
 *   1. 提供报告描述符（供 usb_descriptors.c 填 wDescriptorLength 之外的主机读取）
 *   2. 把 6 字节报文交给 TinyUSB 的 HID 实例 1
 */

#include "tusb.h"
#include "mouse_hid.h"

//--------------------------------------------------------------------+
// HID 报告描述符（Generic Desktop / Mouse，相对轴，无 Report ID）
//--------------------------------------------------------------------+
static uint8_t const desc_hid_report_mouse[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x02,        // Usage (Mouse)
  0xA1, 0x01,        // Collection (Application)
  0x05, 0x09,        //   Usage Page (Button)
  0x19, 0x01,        //   Usage Minimum (1)
  0x29, 0x05,        //   Usage Maximum (5)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x05,        //   Report Count (5)
  0x81, 0x02,        //   Input (Data,Var,Abs)   ← 5 个按键位
  0x75, 0x03,        //   Report Size (3)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x03,        //   Input (Const,Var,Abs)  ← 补齐到一个整字节
  0x05, 0x01,        //   Usage Page (Generic Desktop)
  0x09, 0x30,        //   Usage (X)
  0x09, 0x31,        //   Usage (Y)
  0x16, 0x01, 0x80,  //   Logical Minimum (-32767)  ← 不用 -32768，避开部分主机的边界处理
  0x26, 0xFF, 0x7F,  //   Logical Maximum (32767)
  0x75, 0x10,        //   Report Size (16)
  0x95, 0x02,        //   Report Count (2)
  0x81, 0x06,        //   Input (Data,Var,Rel)   ← dx, dy（各 int16 小端）
  0x09, 0x38,        //   Usage (Wheel)
  0x15, 0x81,        //   Logical Minimum (-127)
  0x25, 0x7F,        //   Logical Maximum (127)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x06,        //   Input (Data,Var,Rel)   ← wheel（int8）
  0xC0               // End Collection
};

// 编译期保护：长度必须与 mouse_hid.h 的 MOUSE_HID_DESC_LEN 一致（后者喂给
// usb_descriptors.c 的 wDescriptorLength，不符会让主机按错误长度解析）。
_Static_assert(sizeof(desc_hid_report_mouse) == MOUSE_HID_DESC_LEN,
               "鼠标报告描述符长度与 MOUSE_HID_DESC_LEN 不一致");

uint8_t const *mouse_hid_report_desc(void)
{
  return desc_hid_report_mouse;
}

bool mouse_hid_ready(void)
{
  return tud_hid_n_ready(MOUSE_HID_INSTANCE);
}

bool mouse_hid_send(const mouse_report_t *rep)
{
  // report_id = 0 → 不带 ID 前缀（专用接口只有一个集合）
  return tud_hid_n_report(MOUSE_HID_INSTANCE, 0, rep, sizeof(*rep));
}
