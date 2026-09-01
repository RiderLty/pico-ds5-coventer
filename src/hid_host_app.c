/*
 * HID 报文处理模块实现。
 *
 * 数据流：tuh_hid_report_received_cb 收到报文 →
 *   键盘接口        → 原始报文（mod + keys）
 *   鼠标（已解析）  → hid_mouse_parse_frame 归一化为按键/滚轮/X/Y
 *   其他/未识别     → 原始报文兜底
 * → hid_output 中间层按当前模式格式化输出。
 */

#include <string.h>

#include "tusb.h"

#include "hid_host_app.h"
#include "hid_parser.h"
#include "hid_output.h"

// 鼠标设备句柄：挂载时解析好的描述符（按 instance 分槽）
static hid_mouse_dev_t g_mouse_devs[CFG_TUH_HID];

// HID 设备挂载时的回调
// 注意: 如果报告描述符长度 > CFG_TUH_ENUMERATION_BUFSIZE，desc_report 为 NULL，
// 鼠标将无法解析、退化为原始报文兜底输出
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const* desc_report, uint16_t desc_len)
{
  // 接口协议类型 (hid_interface_protocol_enum_t)
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  const hid_dev_info_t info = { .vid = vid, .pid = pid, .dev_addr = dev_addr };

  // 鼠标设备：挂载时解析报告描述符，供 hid_mouse_parse_frame 使用。
  // 接口协议为 None 的接口也尝试解析：部分鼠标（尤其游戏鼠）的
  // bInterfaceProtocol 不规范填 0，hid_mouse_parse 会按"相对轴+按键"
  // 判定是否真是鼠标（手柄摇杆/触摸板为绝对轴，不会误判）。
  // （instance 是 TinyUSB 全局 _hidh_itf[] 下标，恒 < CFG_TUH_HID；此处仍显式守界）
  if (instance < CFG_TUH_HID &&
      itf_protocol != HID_ITF_PROTOCOL_KEYBOARD && desc_report && desc_len > 0) {
    memset(&g_mouse_devs[instance], 0, sizeof(hid_mouse_dev_t));
    g_mouse_devs[instance].parsed = hid_mouse_parse(&g_mouse_devs[instance].desc,
                                                    desc_report, desc_len);
    // 解析失败不致命：该设备后续报告走原始报文兜底，不会静默丢数据
  }

  hid_output_mount(instance, &info, itf_protocol);

  // 订阅该接口的报告，收到后进入 tuh_hid_report_received_cb()
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    hid_output_error("cannot request report");
  }
}

// HID 设备拔出时的回调
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  const hid_dev_info_t info = { .vid = vid, .pid = pid, .dev_addr = dev_addr };

  // 中间层先补发仍按着的键（文本模式的边沿状态在其内部维护），
  // 再清理本模块的鼠标描述符槽位
  hid_output_umount(instance, &info);
  if (instance < CFG_TUH_HID) {
    memset(&g_mouse_devs[instance], 0, sizeof(hid_mouse_dev_t));
  }
}

// 收到 HID 设备中断端点报告时的回调：解析并产出统一事件
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const* report, uint16_t len)
{
  if (len != 0 && instance < CFG_TUH_HID) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    const hid_dev_info_t info = { .vid = vid, .pid = pid, .dev_addr = dev_addr };

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
      // 标准布局键盘：原始报文（mod + keys）直接上送
      hid_output_keyboard(instance, &info, report, (uint8_t)len);
    } else if (g_mouse_devs[instance].parsed) {
      // 鼠标：归一化提取；Report ID 不匹配（如复合设备的消费者键报文）则跳过
      hid_mouse_frame_t frame;
      if (hid_mouse_parse_frame(&g_mouse_devs[instance].desc, report, len, &frame)) {
        hid_output_mouse(instance, &info, &frame);
      }
    } else {
      // 手柄/厂商自定义设备：原始报文兜底
      hid_output_generic(instance, &info, report, len);
    }
  }

  // 继续请求接收下一份报告
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    hid_output_error("cannot request report");
  }
}
