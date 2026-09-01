/*
 * pico-ds5-coventer：USB HID Host 键鼠 → DualSense (DS5) 转换器。
 *
 * 由 pico-hid-forward 改造：去掉串口/CDC 与模拟键鼠设备，
 * 原生 USB 设备端改为模拟 Sony DualSense（VID 0x054C / PID 0x0CE6）。
 *
 * core1：PIO-USB Host（GPIO12/13）读实体键盘/鼠标，事件经跨核队列上送
 * core0：原生 USB Device 模拟 DS5，队列出队映射成 0x31 输入报告，
 *        按主机 1kHz 轮询节奏发送。
 * TinyUSB 中 roothub port0 是原生 USB 控制器，roothub port1 是 pico-pio-usb。
 */

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "pio_usb.h"
#include "tusb.h"

#include "input_queue.h"
#include "ds5_pad.h"

// core1: 处理 USB Host 事件
void core1_main(void) {
  sleep_ms(10);

  // 通过 tuh_configure() 将 PIO 配置传递给 Host 栈
  // 注意: tuh_configure() 必须在 tuh_init() 之前调用
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = 12;   // 开发板 host 端口 D+ = GPIO12（DM = D+1 = GPIO13）
  tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

  // Report 协议而非 Boot 协议：配合描述符级鼠标解析，
  // 才能拿到完整按键数/12bit 滚轮等扩展字段
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  // 在 core1 上初始化 PIO-USB Host 栈 (roothub port1)
  tusb_rhport_init_t host_init = {
    .role  = TUSB_ROLE_HOST,
    .speed = TUSB_SPEED_FULL
  };
  tusb_init(1, &host_init);

  while (true) {
    tuh_task();
  }
}

// core0: 处理 USB Device 事件 + DS5 映射
int main(void) {
  // 默认 125MHz 不合适，系统时钟必须是 12MHz 的整数倍（PIO-USB 时序依赖）
  set_sys_clock_khz(120000, true);

  // 初始化跨核队列与临界区。必须先于 core1 启动：
  // 生产者随时可能入队，自旋锁必须先就绪
  input_queue_init();

  multicore_reset_core1();
  multicore_launch_core1(core1_main);

  sleep_ms(100);
  // 在原生 USB 上初始化 Device 栈 (roothub port0)
  tusb_rhport_init_t dev_init = {
    .role  = TUSB_ROLE_DEVICE,
    .speed = TUSB_SPEED_FULL
  };
  tusb_init(0, &dev_init);
  ds5_pad_init();

  uint8_t report[DS_REPORT_LEN];
  while (true) {
    tud_task();
    input_queue_flush();
    ds5_pad_tick();

    // DS5 以 1kHz 被主机轮询（bInterval=1）；端点就绪即发送，
    // 未就绪时由主机 NAK 主导节奏，与真实手柄/DS5Dongle 行为一致
    if (tud_hid_ready()) {
      ds5_pad_build_report(report);
      tud_hid_report(0x01, report, DS_REPORT_LEN);
    }
  }

  return 0;
}
