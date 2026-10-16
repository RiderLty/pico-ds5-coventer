#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUSB_OS               OPT_OS_PICO

// TinyUSB 日志走项目 UART（src/debug.c 的 tu_printf），生产默认关闭。
// -DTUSB_DEBUG_LEVEL=2 可查看枚举、控制传输和端点活动；会产生高频日志。
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG             0
#endif
#include "debug.h"
#define CFG_TUSB_DEBUG_PRINTF      tu_printf

// Enable device stack
#define CFG_TUD_ENABLED     1

// Enable host stack with pio-usb if Pico-PIO-USB library is available
#if ENABLE_DS5_BT
#define CFG_TUH_ENABLED     0
#define CFG_TUH_RPI_PIO_USB 0
#else
#define CFG_TUH_ENABLED     1
#define CFG_TUH_RPI_PIO_USB 1
#endif

/* USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
 * Tinyusb use follows macros to declare transferring memory so that they can be put
 * into those specific section.
 */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
// HID（instance 0 = DS5 手柄；可选追加 NKRO 键盘 / 16bit 鼠标，
// 见 usb_km.c 的 dev_enable 路径）+ UAC1 音频（扬声器/麦克风端点，
// 数据全部丢弃）
// 热重载后的配置最多包含 3 个 HID 接口：DS5、鼠标、键盘。TinyUSB 会按
// 这个值分配每个接口的端点/控制传输状态；只配 1 无法打开全部 HID 接口。
#define CFG_TUD_HID              3
// WinUSB vendor 接口（IO 通讯，dev_enable 第三开关注册，见 usb_io.c）：
// class 0xFF 单接口 + bulk IN/OUT，MS OS 1.0 描述符绑定 winusb.sys
#define CFG_TUD_VENDOR           1
#define CFG_TUD_VENDOR_RX_BUFSIZE 256
#define CFG_TUD_VENDOR_TX_BUFSIZE 256
#define CFG_TUD_AUDIO            1

// HID Endpoint transfer buffer size
#define CFG_TUD_HID_EP_BUFSIZE   64

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION（照搬 DS5Dongle：扬声器 4ch 16bit 48kHz
// OUT / 麦克风 2ch 16bit 48kHz IN，本 SDK 的 EP 尺寸宏签名不同，直接硬编码）
//--------------------------------------------------------------------

// Full-Speed ISO EP：48kHz 满帧 48 样本 + 1 冗余样本
// OUT(扬声器): (48+1)*2*4 = 392 字节；IN(麦克风): (48+1)*2*2 = 196 字节
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX   392
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX    196
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (8u * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ  (16u * CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX)

#define CFG_TUD_AUDIO_ENABLE_EP_OUT          1
#define CFG_TUD_AUDIO_ENABLE_EP_IN           1

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------

// Size of buffer to hold descriptors and other data used for enumeration.
// 复杂鼠标描述符常超 256 字节，超限时 desc_report=NULL、无法解析
#define CFG_TUH_ENUMERATION_BUFSIZE 512

#define CFG_TUH_HUB                 1
// max device support (excluding hub device)
#define CFG_TUH_DEVICE_MAX          (CFG_TUH_HUB ? 4 : 1) // hub typically has 4 ports

#define CFG_TUH_HID                  16
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

// XInput 类驱动（Xbox 手柄没有 HID 接口，走独立类驱动；驱动注册见 input_app.c）
#define CFG_TUH_XINPUT 1

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
