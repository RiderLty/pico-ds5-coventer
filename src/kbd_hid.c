/*
 * kbd_hid 实现 — NKRO 键盘报告描述符与发送
 *
 * 报告描述符照抄 usb-ability-hid 分支（当年 BOOTSEL 自测在真机上验证过枚举与报告），
 * 长度 89 字节由 _Static_assert 绑定。只做 USB 侧，不含"何时发"的策略。
 */
#include "tusb.h"
#include "kbd_hid.h"
#include <string.h>

uint8_t const desc_hid_report_keyboard[] =
{
  HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP          ),
  HID_USAGE      ( HID_USAGE_DESKTOP_KEYBOARD      ),
  HID_COLLECTION ( HID_COLLECTION_APPLICATION      ),

    // ---- Report ID 1：6KRO 引导报告（同时承载 LED 输出）----
    // 注意：TinyUSB 0.18 的 HID_REPORT_ID 宏展开自带尾逗号，此处不能再加
    HID_REPORT_ID  ( 1                             )
    HID_USAGE_PAGE ( HID_USAGE_PAGE_KEYBOARD       ),
    // 修饰键（LeftCtrl..RightGUI，8 bits）
    HID_USAGE_MIN  ( 0xE0                          ),
    HID_USAGE_MAX  ( 0xE7                          ),
    HID_LOGICAL_MIN( 0                             ),
    HID_LOGICAL_MAX( 1                             ),
    HID_REPORT_SIZE( 1                             ),
    HID_REPORT_COUNT( 8                            ),
    HID_INPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),
    // 保留字节
    HID_REPORT_COUNT( 1                            ),
    HID_REPORT_SIZE( 8                             ),
    HID_INPUT      ( HID_CONSTANT | HID_VARIABLE | HID_ABSOLUTE ),
    // 6 键码数组
    HID_LOGICAL_MIN( 0                             ),
    HID_LOGICAL_MAX( 101                           ),
    HID_USAGE_MIN  ( 0x00                          ),
    HID_USAGE_MAX  ( 0x65                          ),
    HID_REPORT_SIZE( 8                             ),
    HID_REPORT_COUNT( 6                            ),
    HID_INPUT      ( HID_DATA | HID_ARRAY | HID_ABSOLUTE ),
    // LED 输出（NumLock/CapsLock/ScrollLock/Compose/Kana，5 bits + 3 bits 填充）
    HID_REPORT_SIZE( 1                             ),
    HID_REPORT_COUNT( 5                            ),
    HID_USAGE_PAGE ( HID_USAGE_PAGE_LED            ),
    HID_USAGE_MIN  ( 0x01                          ),
    HID_USAGE_MAX  ( 0x05                          ),
    HID_OUTPUT     ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),
    HID_REPORT_COUNT( 3                            ),
    HID_OUTPUT     ( HID_CONSTANT | HID_VARIABLE | HID_ABSOLUTE ),

    // ---- Report ID 2：NKRO 位图（直通与 Lua 都用这条）----
    HID_REPORT_ID  ( 2                             )
    HID_USAGE_PAGE ( HID_USAGE_PAGE_KEYBOARD       ),
    // 修饰键（8 bits）
    HID_USAGE_MIN  ( 0xE0                          ),
    HID_USAGE_MAX  ( 0xE7                          ),
    HID_LOGICAL_MIN( 0                             ),
    HID_LOGICAL_MAX( 1                             ),
    HID_REPORT_SIZE( 1                             ),
    HID_REPORT_COUNT( 8                            ),
    HID_INPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),
    // 键位图（用法 0x04-0x73，112 键 → 112 bits）
    HID_USAGE_MIN  ( 0x04                          ),
    HID_USAGE_MAX  ( 0x73                          ),
    HID_REPORT_SIZE( 1                             ),
    HID_REPORT_COUNT( 112                          ),
    HID_INPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),

  HID_COLLECTION_END // Application
};

// Report1: 修饰1+保留1+键6=8 字节数据；Report2: 修饰1+位图14=15 字节数据
_Static_assert(sizeof(desc_hid_report_keyboard) == KBD_HID_REPORT_DESC_LEN,
               "键盘报告描述符长度与 KBD_HID_REPORT_DESC_LEN(89) 不一致");

// 本键盘在 device 端的 HID 实例号（main() 在 tud_init() 前设定；NONE = 未枚举）
static uint8_t s_instance = KBD_HID_INST_NONE;

// 主机写回的 LED 状态
static uint8_t s_led_state;

uint8_t const *kbd_hid_report_desc(void)
{
  return desc_hid_report_keyboard;
}

void kbd_hid_set_instance(uint8_t instance)
{
  s_instance = instance;
}

uint8_t kbd_hid_instance(void)
{
  return s_instance;
}

bool kbd_hid_enabled(void)
{
  return s_instance != KBD_HID_INST_NONE;
}

bool kbd_hid_ready(void)
{
  if (!kbd_hid_enabled()) return false;
  return tud_hid_n_ready(s_instance);
}

bool kbd_hid_send_nkro(uint8_t modifier, const uint8_t bitmap[KBD_HID_NKRO_BYTES])
{
  if (!kbd_hid_enabled()) return false;

  // 修饰键(1) + 112 键位图(14)
  uint8_t rep[1 + KBD_HID_NKRO_BYTES] = { modifier };
  memcpy(&rep[1], bitmap, KBD_HID_NKRO_BYTES);
  return tud_hid_n_report(s_instance, 2, rep, sizeof(rep));
}

uint8_t kbd_hid_led_state(void)
{
  return s_led_state;
}

void kbd_hid_set_led(uint8_t leds)
{
  s_led_state = leds;
}
