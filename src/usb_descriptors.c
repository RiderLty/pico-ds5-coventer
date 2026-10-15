/*
 * DS5（DualSense）设备描述符：UAC1 音频接口 0-2 + HID 手柄接口 3。
 * 基线取自 awalol/DS5Dongle，DS5 端点 IN 0x84 / OUT 0x03。
 * 运行期可追加鼠标/键盘 HID 接口，保持 DS5 身份与既有接口编号不变。
 */

#include "tusb.h"

#include "ds5_pad.h"
#include "usb_km.h"       // usb_km_apply（动态组装配置描述符）
#include "kbd_hid.h"      // KBD_HID_REPORT_DESC_LEN（NKRO 键盘接口，可选追加）
#include "mouse_hid.h"    // MOUSE_HID_DESC_LEN（16bit 鼠标接口，可选追加）
#include "usb_io.h"       // IO 实例查询/接收（custom IO 通讯接口，可选追加）
#include "debug.h"
#include <string.h>

//--------------------------------------------------------------------
// Device Descriptor
//--------------------------------------------------------------------

// Windows 按 VID/PID/bcdDevice 缓存 OS 描述符。每种接口布局用独立的
// BCD 修订号，避免首次基线枚举与后续 IO 枚举、不同 IO 接口号共用缓存。
#define DEVICE_REV_BASE 0x0120u

static tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x054C,   // Sony
    .idProduct          = 0x0CE6,   // DualSense Wireless Controller
    .bcdDevice          = DEVICE_REV_BASE,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x00,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------
// Configuration Descriptor（227 字节，照搬 DS5Dongle 的 DS 模式配置：
// 音频控制/扬声器流/麦克风流 接口 0-2 + HID 手柄接口 3。
// 仅端点号沿用 DS5Dongle：音频 OUT EP1 / 音频 IN EP2 / HID IN 0x84 / HID OUT 0x03）
//--------------------------------------------------------------------

#define CONFIG_TOTAL_LEN  227u   // 0x00E3
#define HID_REPORT_DESC_LEN 321u

uint8_t const desc_configuration[] = {
    // --- CONFIGURATION DESCRIPTOR ---
    0x09,        // bLength
    0x02,        // bDescriptorType (CONFIGURATION)
    U16_TO_U8S_LE(CONFIG_TOTAL_LEN), // wTotalLength
    0x04,        // bNumInterfaces
    0x01,        // bConfigurationValue: 1
    0x00,        // iConfiguration: 0
    0xC0,        // bmAttributes: SELF-POWERED, NO REMOTE-WAKEUP
    0xFA,        // bMaxPower: 500mA (250 * 2mA)

    // --- INTERFACE DESCRIPTOR (0.0): Audio Control ---
    0x09,        // bLength
    0x04,        // bDescriptorType (INTERFACE)
    0x00,        // bInterfaceNumber: 0
    0x00,        // bAlternateSetting: 0
    0x00,        // bNumEndpoints: 0
    0x01,        // bInterfaceClass: Audio (0x01)
    0x01,        // bInterfaceSubClass: Audio Control (0x01)
    0x00,        // bInterfaceProtocol: 0x00
    0x00,        // iInterface: 0

    // Class-specific AC Interface Header Descriptor
    0x0A, 0x24, 0x01,       // bLength, CS_INTERFACE, Header
    0x00, 0x01,             // bcdADC: 1.00
    0x49, 0x00,             // wTotalLength: 73 (0x0049)
    0x02,                   // bInCollection: 2 streaming interfaces
    0x01,                   // baInterfaceNr(1): Interface 1
    0x02,                   // baInterfaceNr(2): Interface 2

    // Input Terminal Descriptor (Terminal ID 1: USB Streaming → Speaker)
    0x0C, 0x24, 0x02,       // bLength, CS_INTERFACE, Input Terminal
    0x01,                   // bTerminalID: 1
    0x01, 0x01,             // wTerminalType: USB Streaming (0x0101)
    0x06,                   // bAssocTerminal: 6
    0x04,                   // bNrChannels: 4
    0x33, 0x00,             // wChannelConfig: L/R Front + L/R Surround
    0x00,                   // iChannelNames: 0
    0x00,                   // iTerminal: 0

    // Feature Unit Descriptor (Unit ID 2 ← from Terminal 1, 扬声器音量/静音)
    0x0C, 0x24, 0x06,       // bLength, CS_INTERFACE, Feature Unit
    0x02,                   // bUnitID: 2
    0x01,                   // bSourceID: 1
    0x01,                   // bControlSize: 1 byte per control
    0x03,                   // bmaControls[0]: Master – Mute, Volume
    0x00, 0x00, 0x00, 0x00, 0x00, // bmaControls[1..4]: 无 per-channel 控制
    // Output Terminal (Terminal ID 3: Speaker)
    0x09, 0x24, 0x03,       // bLength, CS_INTERFACE, Output Terminal
    0x03,                   // bTerminalID: 3
    0x01, 0x03,             // wTerminalType: Speaker (0x0301)
    0x04,                   // bAssocTerminal: 4
    0x02,                   // bSourceID: 2
    0x00,                   // iTerminal: 0

    // Input Terminal (Terminal ID 4: Headset Mic)
    0x0C, 0x24, 0x02,       // bLength, CS_INTERFACE, Input Terminal
    0x04,                   // bTerminalID: 4
    0x02, 0x04,             // wTerminalType: Headset (0x0402)
    0x03,                   // bAssocTerminal: 3
    0x02,                   // bNrChannels: 2 (mono mic duplicated)
    0x03, 0x00,             // wChannelConfig: Front Left + Front Right
    0x00, 0x00,             // iChannelNames, iTerminal

    // Feature Unit (Unit ID 5 ← from Terminal 4, 麦克风音量/静音)
    0x09, 0x24, 0x06,       // bLength, CS_INTERFACE, Feature Unit
    0x05,                   // bUnitID: 5
    0x04,                   // bSourceID: 4
    0x01,                   // bControlSize: 1
    0x03,                   // bmaControls[0]: Master – Mute, Volume
    0x00,                   // bmaControls[1]: Ch1 – 无
    0x00,                   // iFeature: 0
    // Output Terminal (Terminal ID 6: USB Streaming)
    0x09, 0x24, 0x03,       // bLength, CS_INTERFACE, Output Terminal
    0x06,                   // bTerminalID: 6
    0x01, 0x01,             // wTerminalType: USB Streaming
    0x01,                   // bAssocTerminal: 1
    0x05,                   // bSourceID: 5
    0x00,                   // iTerminal: 0

    // --- INTERFACE (1.0): Audio Streaming OUT (Alternate 0, 零带宽) ---
    0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    // --- INTERFACE (1.1): Audio Streaming OUT (Alternate 1, 有 EP) ---
    0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    // AS General
    0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00, // bTerminalLink=1, bDelay=1, PCM
    // Format: 4ch, 2 bytes/sample, 16bit, 48000 Hz
    0x0B, 0x24, 0x02, 0x01, 0x04, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00,
    // EP OUT 0x01: Isochronous, Adaptive, 392 字节
    0x09, 0x05, 0x01, 0x09, 0x88, 0x01, 0x01, 0x00, 0x00,
    // CS Endpoint
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    // --- INTERFACE (2.0): Audio Streaming IN (Alternate 0, 零带宽) ---
    0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    // --- INTERFACE (2.1): Audio Streaming IN (Alternate 1, 有 EP) ---
    0x09, 0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    // AS General
    0x07, 0x24, 0x01, 0x06, 0x01, 0x01, 0x00, // bTerminalLink=6, bDelay=1, PCM
    // Format: 2ch, 2 bytes/sample, 16bit, 48000 Hz
    0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00,
    // EP IN 0x82: Isochronous, Asynchronous, 196 字节
    0x09, 0x05, 0x82, 0x05, 0xC4, 0x00, 0x01, 0x00, 0x00,
    // CS Endpoint
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    // --- INTERFACE DESCRIPTOR (3.0): HID (DualSense 5 Gamepad + Touchpad) ---
    0x09,        // bLength
    0x04,        // bDescriptorType (INTERFACE)
    0x03,        // bInterfaceNumber: 3
    0x00,        // bAlternateSetting: 0
    0x02,        // bNumEndpoints: 2 (IN + OUT)
    0x03,        // bInterfaceClass: HID
    0x00,        // bInterfaceSubClass: None
    0x00,        // bInterfaceProtocol: None
    0x00,        // iInterface: 0

    // HID 类描述符
    0x09, 0x21,             // bLength, bDescriptorType (HID)
    0x11, 0x01,             // bcdHID: 1.11
    0x00,                   // bCountryCode: Not localized
    0x01,                   // bNumDescriptors: 1 report descriptor
    0x22,                   // bDescriptorType: Report
    U16_TO_U8S_LE(HID_REPORT_DESC_LEN), // wDescriptorLength: 321 (0x0141)

    // EP IN 0x84: Interrupt, 64 字节, 1ms（1000Hz 轮询）
    0x07, 0x05, 0x84, 0x03, 0x40, 0x00, 0x01,
    // EP OUT 0x03: Interrupt, 64 字节, 1ms（主机下发声光/震动设置）
    0x07, 0x05, 0x03, 0x03, 0x40, 0x00, 0x01,
};

_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
               "config descriptor length mismatch");

//--------------------------------------------------------------------
// 动态组装（参考 pico-hid-mapper 的 usb_desc_config_apply）：
// flash 基线 = DS5 + 音频（上文数组）；按需在**末尾**追加 16bit 鼠标接口
// （itf 4 / EP 0x86）与 NKRO 键盘接口（itf 5 / EP 0x87），回填 wTotalLength /
// bNumInterfaces / 键盘的 bInterfaceNumber（接口号必须连续）。追加在末尾是
// 有意的：不改动既有接口编号（AC 描述符的 baInterfaceNr 引用 itf 1/2 等），
// 保持原有 DS5/音频布局。
//--------------------------------------------------------------------

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_MOUSE,     // 3: 16bit 鼠标接口（仅启用时的配置描述符引用）
    STRID_KBD,       // 4: NKRO 键盘接口（同上）
    STRID_IO,        // 5: custom IO 通讯接口（同上）
};

// 追加块 = TUD_HID_DESCRIPTOR 展开：接口 9 + HID 9 + 端点 7 = 25 字节
#define KM_ITF_DESC_LEN   25u
// IO 追加块 = TUD_VENDOR_DESCRIPTOR 展开：接口 9 + EP OUT 7 + EP IN 7 =
// 23 字节（class 0xFF 单接口双 bulk，无 IAD —— usbccgp 保持 legacy 分组，
// 音频不受影响；CDC/NCM 等带 IAD 的类与 DS5 音频基线在 Windows 上互斥）。
// Windows 侧经 MS OS 1.0 描述符（CompatibleID="WINUSB"）自动绑定 winusb.sys
#define IO_ITF_DESC_LEN   23u
#define KM_DESC_MAX       (CONFIG_TOTAL_LEN + 2u * KM_ITF_DESC_LEN + IO_ITF_DESC_LEN)

// 键盘/鼠标端点（与 DS5 的 0x84 IN/0x03 OUT、音频的 0x82 IN/0x01 OUT 无冲突）
#define EPNUM_MOUSE_IN    0x86
#define EPNUM_MOUSE_SIZE  8u    // 报文仅 6 字节，8 是鼠标端点的常规值
#define EPNUM_KBD_IN      0x87
#define EPNUM_KBD_SIZE    16u   // NKRO：1B Report ID + 1B 修饰键 + 14B 位图

// IO 端点：bulk 端点对（WinUSB，全速 64B）
#define EPNUM_IO_IN       0x88
#define EPNUM_IO_OUT      0x08
#define EPNUM_IO_SIZE     64u

// 接口描述符里 bInterfaceNumber 的位置（追加块内偏移 2）
#define KM_ITF_BNUM_OFFSET      2u
_Static_assert(KM_ITF_DESC_LEN == 25u, "TUD_HID_DESCRIPTOR 展开长度变化，偏移需重算");
_Static_assert(IO_ITF_DESC_LEN == 23u, "TUD_VENDOR_DESCRIPTOR 展开长度变化，偏移需重算");

// 鼠标接口块：HID 类、协议 None（**不能用 boot 协议**——boot 鼠标只有 3 字节
// 报文格式，与本项目的 6 字节 16bit 轴不符）
static uint8_t const desc_itf_mouse[] = {
    TUD_HID_DESCRIPTOR(4, STRID_MOUSE, HID_ITF_PROTOCOL_NONE, MOUSE_HID_DESC_LEN,
                       EPNUM_MOUSE_IN, EPNUM_MOUSE_SIZE, 1),
};
// NKRO 键盘接口块：协议 None（boot 键盘只认 6KRO 固定格式，本键盘用 NKRO 位图）
static uint8_t const desc_itf_kbd[] = {
    TUD_HID_DESCRIPTOR(5, STRID_KBD, HID_ITF_PROTOCOL_NONE, KBD_HID_REPORT_DESC_LEN,
                       EPNUM_KBD_IN, EPNUM_KBD_SIZE, 1),
};

// WinUSB 通讯接口块：vendor 类（0xFF）、协议 None（非 boot 集合）。SLIP 帧
// 流，见 usb_io.c
static uint8_t const desc_itf_io[] = {
    TUD_VENDOR_DESCRIPTOR(6, STRID_IO, EPNUM_IO_OUT, EPNUM_IO_IN, EPNUM_IO_SIZE),
};

_Static_assert(sizeof(desc_itf_mouse) == KM_ITF_DESC_LEN &&
               sizeof(desc_itf_kbd) == KM_ITF_DESC_LEN &&
               sizeof(desc_itf_io) == IO_ITF_DESC_LEN,
               "追加接口块长度与 *_ITF_DESC_LEN 不一致");

static uint8_t  s_cfg_desc_ram[KM_DESC_MAX] __attribute__((aligned(4)));
static uint16_t s_cfg_desc_len = 0;   // 0 = 尚未组装（回调返回 flash 基线）

void usb_km_apply(bool mouse, bool kbd, bool io)
{
    memcpy(s_cfg_desc_ram, desc_configuration, sizeof(desc_configuration));
    uint16_t len = (uint16_t)sizeof(desc_configuration);
    uint8_t  itf_count = 4u;   // 基线：音频 0-2 + DS5 3

    uint16_t kbd_off = 0;

    if (mouse) {
        memcpy(s_cfg_desc_ram + len, desc_itf_mouse, sizeof(desc_itf_mouse));
        len = (uint16_t)(len + sizeof(desc_itf_mouse));
        itf_count++;
    }

    if (kbd) {
        kbd_off = len;
        memcpy(s_cfg_desc_ram + len, desc_itf_kbd, sizeof(desc_itf_kbd));
        // 接口号必须连续：音频 0-2 / DS5 3 / [鼠标 4] / 键盘紧随其后。
        // 描述符数组里的占位编号（5）只在"无鼠标"时需要回填成 4。
        s_cfg_desc_ram[kbd_off + KM_ITF_BNUM_OFFSET] = itf_count;
        len = (uint16_t)(len + sizeof(desc_itf_kbd));
        itf_count++;
    }

    if (io) {
        memcpy(s_cfg_desc_ram + len, desc_itf_io, sizeof(desc_itf_io));
        // IO 追加在最后，占位接口号（6）按实际追加位置回填
        s_cfg_desc_ram[len + KM_ITF_BNUM_OFFSET] = itf_count;
        len = (uint16_t)(len + sizeof(desc_itf_io));
        itf_count++;
    }

    // 回填 wTotalLength(offset 2, LE) 与 bNumInterfaces(offset 4)
    s_cfg_desc_ram[2] = (uint8_t)(len & 0xFF);
    s_cfg_desc_ram[3] = (uint8_t)(len >> 8);
    s_cfg_desc_ram[4] = itf_count;

    s_cfg_desc_len = len;
    desc_device.bcdDevice = (uint16_t)(DEVICE_REV_BASE |
        (kbd ? USB_KM_BIT_KBD : 0u) |
        (mouse ? USB_KM_BIT_MOUSE : 0u) |
        (io ? USB_KM_BIT_IO : 0u));

    // 自检：三项都不追加时，RAM 副本必须与 flash 基线逐字节一致，
    // 否则说明基线构造/回填有偏差，会静默改变主机看到的接口布局
    if (!mouse && !kbd && !io &&
        memcmp(s_cfg_desc_ram, desc_configuration, sizeof(desc_configuration)) != 0) {
        DEBUG("[DS5C][KM] BUG: config desc RAM copy != baseline\n");
    }

    DEBUG("[DS5C][KM] config desc: %u B, %u itf, mouse=%d kbd=%d io=%d\n",
                 (unsigned)len, (unsigned)itf_count, mouse, kbd, io);
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;   // 只有一份配置
    return s_cfg_desc_len ? s_cfg_desc_ram : desc_configuration;
}

//--------------------------------------------------------------------
// HID Report Descriptor（321 字节，照搬 DS5Dongle desc_hid_report_ds）
//--------------------------------------------------------------------

uint8_t const desc_hid_report_ds[] = {
    0x05, 0x01, // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05, // Usage (Game Pad)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x01, //   Report ID (1)
    0x09, 0x30, //   Usage (X)
    0x09, 0x31, //   Usage (Y)
    0x09, 0x32, //   Usage (Z)
    0x09, 0x35, //   Usage (Rz)
    0x09, 0x33, //   Usage (Rx)
    0x09, 0x34, //   Usage (Ry)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x06, //   Report Count (6)
    0x81, 0x02, //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x20, //   Usage (0x20)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x02, //   Input (Data,Var,Abs)
    0x05, 0x01, //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x39, //   Usage (Hat switch)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x07, //   Logical Maximum (7)
    0x35, 0x00, //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14, //   Unit (English Rotation, Centimeter)
    0x75, 0x04, //   Report Size (4)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x42, //   Input (Data,Var,Abs,Null State)
    0x65, 0x00, //   Unit (None)
    0x05, 0x09, //   Usage Page (Button)
    0x19, 0x01, //   Usage Minimum (0x01)
    0x29, 0x0F, //   Usage Maximum (0x0F)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x0F, //   Report Count (15)
    0x81, 0x02, //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x21, //   Usage (0x21)
    0x95, 0x0D, //   Report Count (13)
    0x81, 0x02, //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x22, //   Usage (0x22)
    0x15, 0x00, //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x34, //   Report Count (52)
    0x81, 0x02, //   Input (Data,Var,Abs)
    0x85, 0x02, //   Report ID (2)
    0x09, 0x23, //   Usage (0x23)
    0x95, 0x2F, //   Report Count (47)
    0x91, 0x02, //   Output (Data,Var,Abs,Non-volatile)
    0x85, 0x05, //   Report ID (5)
    0x09, 0x33, //   Usage (0x33)
    0x95, 0x28, //   Report Count (40)
    0xB1, 0x02, //   Feature (Data,Var,Abs,Non-volatile)
    0x85, 0x08, //   Report ID (8)
    0x09, 0x34, //   Usage (0x34)
    0x95, 0x2F, //   Report Count (47)
    0xB1, 0x02, //   Feature
    0x85, 0x09, //   Report ID (9)
    0x09, 0x24, //   Usage (0x24)
    0x95, 0x13, //   Report Count (19)
    0xB1, 0x02, //   Feature
    0x85, 0x0A, //   Report ID (10)
    0x09, 0x25, //   Usage (0x25)
    0x95, 0x1A, //   Report Count (26)
    0xB1, 0x02, //   Feature
    0x85, 0x0B, //   Report ID (11)
    0x09, 0x41, //   Usage (0x41)
    0x95, 0x29, //   Report Count (41)
    0xB1, 0x02, //   Feature
    0x85, 0x0C, //   Report ID (12)
    0x09, 0x42, //   Usage (0x42)
    0x95, 0x29, //   Report Count (41)
    0xB1, 0x02, //   Feature
    0x85, 0x20, //   Report ID (32)
    0x09, 0x26, //   Usage (0x26)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0x21, //   Report ID (33)
    0x09, 0x27, //   Usage (0x27)
    0x95, 0x04, //   Report Count (4)
    0xB1, 0x02, //   Feature
    0x85, 0x22, //   Report ID (34)
    0x09, 0x40, //   Usage (0x40)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0x80, //   Report ID (-128)
    0x09, 0x28, //   Usage (0x28)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0x81, //   Report ID (-127)
    0x09, 0x29, //   Usage (0x29)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0x82, //   Report ID (-126)
    0x09, 0x2A, //   Usage (0x2A)
    0x95, 0x09, //   Report Count (9)
    0xB1, 0x02, //   Feature
    0x85, 0x83, //   Report ID (-125)
    0x09, 0x2B, //   Usage (0x2B)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0x84, //   Report ID (-124)
    0x09, 0x2C, //   Usage (0x2C)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0x85, //   Report ID (-123)
    0x09, 0x2D, //   Usage (0x2D)
    0x95, 0x02, //   Report Count (2)
    0xB1, 0x02, //   Feature
    0x85, 0xA0, //   Report ID (-96)
    0x09, 0x2E, //   Usage (0x2E)
    0x95, 0x01, //   Report Count (1)
    0xB1, 0x02, //   Feature
    0x85, 0xE0, //   Report ID (-32)
    0x09, 0x2F, //   Usage (0x2F)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0xF0, //   Report ID (-16)
    0x09, 0x30, //   Usage (0x30)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0xF1, //   Report ID (-15)
    0x09, 0x31, //   Usage (0x31)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0xF2, //   Report ID (-14)
    0x09, 0x32, //   Usage (0x32)
    0x95, 0x0F, //   Report Count (15)
    0xB1, 0x02, //   Feature
    0x85, 0xF4, //   Report ID (-12)
    0x09, 0x35, //   Usage (0x35)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0xF5, //   Report ID (-11)
    0x09, 0x36, //   Usage (0x36)
    0x95, 0x03, //   Report Count (3)
    0xB1, 0x02, //   Feature
    0x85, 0xF6, //   Report ID (-10)
    0x09, 0x37, //   Usage (Vendor 0x37)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0xF7, //   Report ID (-9)
    0x09, 0x38, //   Usage (Vendor 0x38)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0xF8, //   Report ID (-8)
    0x09, 0x39, //   Usage (Vendor 0x39)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0x85, 0xF9, //   Report ID (-7)
    0x09, 0x3A, //   Usage (Vendor 0x3A)
    0x95, 0x3F, //   Report Count (63)
    0xB1, 0x02, //   Feature
    0xC0,       // End Collection
};

_Static_assert(sizeof(desc_hid_report_ds) == HID_REPORT_DESC_LEN,
               "HID report descriptor length mismatch");

uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf)
{
    // 按 TinyUSB HID 实例号分发（实例按配置描述符里接口出现顺序分配：
    // DS5=0，鼠标（启用时）=1，键盘 = 1 + 鼠标有无，见 usb_km.c。
    // IO 通讯接口是 vendor 类（WinUSB），不走 HID 报告描述符）
    if (usb_km_mouse_enabled() && itf == MOUSE_HID_INSTANCE) {
        return mouse_hid_report_desc();
    }
    if (kbd_hid_enabled() && itf == kbd_hid_instance()) {
        return kbd_hid_report_desc();
    }
    return desc_hid_report_ds;
}

//--------------------------------------------------------------------
// String Descriptors
//--------------------------------------------------------------------

static char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },     // 0: English (0x0409)
    "Sony Interactive Entertainment", // 1: Manufacturer
    "DualSense Wireless Controller",  // 2: Product
    "Mouse",                          // 3: 16bit 鼠标接口（仅启用时引用）
    "Keyboard",                       // 4: NKRO 键盘接口（仅启用时引用）
    "Pico WinUSB",                    // 5: WinUSB 通讯接口（仅启用时引用）
};

// MS OS 1.0：0xEE 字符串（规范 18 字节）= bLength/bType + "MSFT100" UTF-16
// （14B）+ bMS_VendorCode（1B）+ bPadding（1B）。Windows 据此发起 Compat ID
// 拉取（tud_vendor_control_xfer_cb 应答）。
#define MS_OS_VENDOR_CODE 0x21u

static uint8_t const desc_str_msos[18] __attribute__((aligned(2))) = {
    18, 0x03,
    'M', 0x00, 'S', 0x00, 'F', 0x00, 'T', 0x00,
    '1', 0x00, '0', 0x00, '0', 0x00,
    MS_OS_VENDOR_CODE, 0x00,
};

static uint8_t const *desc_string_ms_os_10(void)
{
    return desc_str_msos;
}

// MS OS 1.0 Extended Compat ID：未启用 IO 时返回合法的 16B 空表；
// 启用时追加 24B 功能段，接口号为 4+鼠标+键盘。
static uint8_t desc_ms_compat_id[40];

static uint8_t winusb_interface_number(void)
{
    return (uint8_t)(4u + (usb_km_mouse_enabled() ? 1u : 0u) +
                         (usb_km_kbd_enabled() ? 1u : 0u));
}

static uint8_t const *desc_ms_compat_id_get(void)
{
    bool io = usb_km_io_enabled();
    memset(desc_ms_compat_id, 0, sizeof desc_ms_compat_id);
    uint32_t len = io ? sizeof desc_ms_compat_id : 16u;
    memcpy(desc_ms_compat_id + 0, &len, 4);            // dwLength = 16 或 40
    desc_ms_compat_id[4] = 0x00; desc_ms_compat_id[5] = 0x01;   // bcdVersion 1.0
    desc_ms_compat_id[6] = 0x04; desc_ms_compat_id[7] = 0x00;   // wIndex = 4
    desc_ms_compat_id[8] = io ? 1u : 0u;               // bCount
    // [9..15] 保留 0
    if (io) {
        desc_ms_compat_id[16] = winusb_interface_number();
        desc_ms_compat_id[17] = 0x01;  // 功能段首个 RESERVED 字段必须为 1
        memcpy(desc_ms_compat_id + 18, "WINUSB", 6);
    }
    // [26..39] subCompatibleID[8] + reserved[6]，保留 0
    return desc_ms_compat_id;
}

// Extended Properties：让 WinUSB/浏览器通过 GUID 发现通讯接口。
// REG_MULTI_SZ 数据包含字符串终止符和列表终止符（两个 UTF-16 NUL）。
static struct TU_ATTR_PACKED {
    uint32_t length;
    uint16_t version;
    uint16_t index;
    uint16_t count;
    uint32_t property_size;
    uint32_t property_type;
    uint16_t name_length;
    uint16_t name[21];
    uint32_t data_length;
    uint16_t data[40];
} const desc_ms_properties = {
    .length = 146,
    .version = 0x0100,
    .index = 0x0005,
    .count = 1,
    .property_size = 136,
    .property_type = 7,  // REG_MULTI_SZ
    .name_length = 42,
    .name = u"DeviceInterfaceGUIDs",
    .data_length = 80,
    .data = u"{56D3BAC2-F5C4-43BE-A690-71B61199C15E}\0",
};

_Static_assert(sizeof(desc_ms_properties) == 146u,
               "MS OS Extended Properties descriptor length mismatch");

static uint16_t _desc_str[60 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count;

    switch (index) {
        case STRID_LANGID:
            memcpy(&_desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;

        case 0xEE:   // 首次基线枚举也必须应答，避免 osvc=0000 负缓存
            return (uint16_t const *)desc_string_ms_os_10();

        default:
            if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
                return NULL;
            }
            const char *str = string_desc_arr[index];
            chr_count = (uint8_t)strlen(str);
            if (chr_count > 60) chr_count = 60;
            for (uint8_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = str[i];
            }
            break;
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

//--------------------------------------------------------------------
// HID 报告回调（GET_REPORT / SET_REPORT）
//--------------------------------------------------------------------

// Feature 报告尺寸表（数据长度，不含 Report ID 字节），与报告描述符一致。
// 主机（Steam/Windows）可能读校准/配对信息，返回零数据即可。
static uint8_t feature_report_len(uint8_t report_id)
{
    switch (report_id) {
        case 0x05: return 40;   // 触摸板校准
        case 0x08: return 47;
        case 0x09: return 19;
        case 0x0A: return 26;
        case 0x0B: case 0x0C: return 41;
        case 0x20: return 63;   // 配对信息
        case 0x21: return 4;
        case 0x22: return 63;
        case 0x80: return 63;
        case 0x81: return 63;
        case 0x82: return 9;
        case 0x83: return 63;
        case 0x84: return 63;
        case 0x85: return 2;
        case 0xA0: return 1;
        case 0xE0: return 63;
        case 0xF0: case 0xF1: case 0xF4: case 0xF6:
        case 0xF7: case 0xF8: case 0xF9: return 63;
        case 0xF2: return 15;
        case 0xF5: return 3;
        default:   return 0;
    }
}

// GET_REPORT：Input(1) 返回当前输入报告（含 Report ID 字节共 64 字节）；
// Feature 返回零数据；未知 ID 返回 0 → 栈会 STALL。
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    // 仅 DS5 实例（instance 0）处理；键盘/鼠标实例的 GET_REPORT 无需应答
    if (itf != 0) return 0;

    if (report_type == HID_REPORT_TYPE_INPUT && report_id == 0x01) {
        if (reqlen < 1u + DS_REPORT_LEN) return 0;
        buffer[0] = 0x01;
        ds5_pad_build_report(buffer + 1);
        return 1u + DS_REPORT_LEN;
    }

    if (report_type == HID_REPORT_TYPE_FEATURE) {
        uint8_t len = feature_report_len(report_id);
        if (len == 0) return 0;
        if (reqlen < (uint16_t)len + 1u) return 0;
        memset(buffer, 0, len + 1u);
        buffer[0] = report_id;
        return (uint16_t)len + 1u;
    }

    return 0;
}

// SET_REPORT / OUT 端点：主机下发的输出报告（Report ID 0x02 = 声光/震动
// 设置，47 字节 SetStateData）。无外设可驱动，忽略即可，不要 STALL。
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    // NKRO 键盘实例：主机写回 LED 状态（Report ID 1 的 output 报告，
    // bit0-4 = Num/Caps/Scroll/Compose/Kana）
    if (kbd_hid_enabled() && itf == kbd_hid_instance()) {
        if (report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
            kbd_hid_set_led(buffer[0]);
        }
        return;
    }

    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

//--------------------------------------------------------------------
// MS OS 1.0：Windows 读取 0xEE 字符串获得 vendor code 后，以
// (vendor, device) 控制请求拉取描述符（usbd 把所有 vendor 请求路由到
// 本回调）。wIndex=4 → Extended Compat ID（"WINUSB"）；
// wIndex=5 → IO 接口的 Extended Properties（DeviceInterfaceGUIDs）。
//--------------------------------------------------------------------
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        request->bmRequestType_bit.direction != TUSB_DIR_IN) {
        return false;   // 非本用途的 vendor 请求：STALL
    }

    if (request->bRequest != MS_OS_VENDOR_CODE) return false;

    if (stage == CONTROL_STAGE_SETUP) {
        if (request->wIndex == 0x0004 &&
            request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE &&
            request->wValue == 0) {
            uint8_t const *desc = desc_ms_compat_id_get();
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc,
                                    desc[0]);  // 16B 空表或 40B 单功能表
        }
        if (request->wIndex == 0x0005 && usb_km_io_enabled()) {
            uint8_t itf = winusb_interface_number();
            // Windows 复合设备：C1、wValue 低字节为接口号。
            // 同时兼容旧 OS 1.0 文档的 C0、wValue 高字节接口号形式。
            bool interface_request =
                request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE &&
                request->wValue == itf;
            bool legacy_request =
                request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE &&
                request->wValue == (uint16_t)(itf << 8);
            if (!interface_request && !legacy_request) return false;
            return tud_control_xfer(rhport, request,
                                    (void *)(uintptr_t)&desc_ms_properties,
                                    sizeof desc_ms_properties);
        }
        return false;
    }
    return true;
}
