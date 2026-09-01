/*
 * DS5（DualSense）HID-only 设备描述符。
 *
 * 设备/报告描述符的格式与字节值取自 awalol/DS5Dongle（src/usb_descriptors.cpp），
 * 在其基础上去掉音频（UAC）接口：HID 接口重排为接口 0，
 * 端点重排为 IN 0x81 / OUT 0x02。报告描述符 321 字节逐字节照搬。
 */

#include "tusb.h"

#include "ds5_pad.h"

//--------------------------------------------------------------------
// Device Descriptor
//--------------------------------------------------------------------

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x054C,   // Sony
    .idProduct          = 0x0CE6,   // DualSense Wireless Controller
    .bcdDevice          = 0x0100,
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

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;   // 只有一份配置
    return desc_configuration;
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
    (void)itf;
    return desc_hid_report_ds;
}

//--------------------------------------------------------------------
// String Descriptors
//--------------------------------------------------------------------

static char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },     // 0: English (0x0409)
    "Sony Interactive Entertainment", // 1: Manufacturer
    "DualSense Wireless Controller",  // 2: Product
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
};

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
    (void)itf;

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
    (void)itf;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
