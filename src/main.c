/*
 * pico-ds5-coventer：USB 输入 → DualSense (DS5) 转换器。
 *
 * 输入（core1，PIO-USB Host, GPIO12/13）：TinyUSB HID host 回调交给
 *   input_app —— hidkit 解析键鼠 / HID 手柄，XInput 走独立类驱动，
 *   Switch Pro（057e:2009/2006/2007）由握手库先行消费应答；事件经跨核队列上送。
 * 输出（core0，原生 USB Device）：模拟 DS5（VID 0x054C / PID 0x0CE6），
 *   队列出队映射成 0x01 输入报告，按主机 1kHz 轮询节奏发送。
 * TinyUSB 中 roothub port0 是原生 USB 控制器，roothub port1 是 pico-pio-usb。
 */

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"

#if ENABLE_DS5_BT
#include "ds5_bt.h"
#else
#include "pio_usb.h"
#include "switch_pro.h"
#endif
#include "tusb.h"

#include "debug.h"
#include "boot_button.h"
#include "input_queue.h"
#include "input_app.h"
#include "ds5_pad.h"
#include "usb_km.h"       // NKRO 键盘 / 16bit 鼠标输出接口（Lua 请求热重载）
#include "usb_io.h"       // custom IO 通讯接口 echo（dev_enable 第三参动态开关）
#include "uart_proto.h"   // UART 传输适配器（DMA RX + SLIP 协议帧）
#include "lua_binder.h"
#include "lua_script.h"

// Two-core startup handshake. Keep IRQs enabled while waiting so KV Flash
// operations can still lock out the other core. No FIFO traffic or new locks:
// pico_multicore owns the FIFO for Flash lockout after core1 is launched.
enum { STARTUP_WAIT_SCRIPT, STARTUP_SCRIPT_READY, STARTUP_DEVICE_READY };
static uint32_t s_startup_phase;

// ARM 工具链 15.x 兼容: newlib 的 remove()/rename()（Lua os 库拉入）通过
// _unlink_r/_link_r 调用 _unlink/_link, 但 Pico SDK 的 newlib_interface.c
// 未提供这两个 syscall 桩。裸机无文件系统环境直接返回错误。
int _unlink(const char *name) {
  (void)name;
  return -1;
}
int _link(const char *old, const char *new) {
  (void)old;
  (void)new;
  return -1;
}

// Lua 引擎调试回调（lua_core 模块要求外部实现）：转发调试串口。
// lua_core 的 debugf 消息不带换行，这里补上
void lua_core_debug_cb(const char *msg)
{
  DEBUG("[DS5C]%s\n", msg);
}

// core1: 处理 USB Host 事件
void core1_main(void) {
  flash_safe_execute_core_init();
  sleep_ms(10);


#if !ENABLE_DS5_BT
  // 通过 tuh_configure() 将 PIO 配置传递给 Host 栈
  // 注意: tuh_configure() 必须在 tuh_init() 之前调用
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = 12;   // 开发板 host 端口 D+ = GPIO12（DM = D+1 = GPIO13）
  tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

  // Report 协议而非 Boot 协议：hidkit 按报告描述符解析，
  // 才能拿到完整按键数/NKRO/手柄布局等扩展字段
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

#endif

  // hidkit 槽位表清空（须在 tusb_init 之前）
  input_app_init();

  // 初始化 Lua VM + 加载 KV 脚本，缺失或加载失败时回退预制脚本（全部在
  // core1 同线程 —— 与 mapper 的 engine 线程模型一致）。先完成全部顶层
  // dev_enable 请求，再允许 core0 以最终能力进行第一次 USB 枚举。
  lua_script_boot();
  __atomic_store_n(&s_startup_phase, STARTUP_SCRIPT_READY, __ATOMIC_RELEASE);
  while (__atomic_load_n(&s_startup_phase, __ATOMIC_ACQUIRE) != STARTUP_DEVICE_READY) {
    tight_loop_contents();
  }

  // Device 就绪后才启动 Host / Lua tick，避免输入回调在首枚举配置提交前
  // 改写启动脚本的目标。之后的 dev_enable 仍按运行期热重载处理。

#if ENABLE_DS5_BT
  ds5_bt_init(); // core1 polling; Device is already available even if radio fails
#else
  tusb_rhport_init_t host_init = {
    .role  = TUSB_ROLE_HOST,
    .speed = TUSB_SPEED_FULL
  };
  tusb_init(1, &host_init);

#endif

  DEBUG("[DS5C] core1 input ready (bluetooth=%d)\n", ENABLE_DS5_BT);

  while (true) {
#if ENABLE_DS5_BT
    ds5_bt_task();
#else
    tuh_task();
#endif
    input_app_task();   // Switch Pro 握手状态机（须与 tuh 回调同核同循环）
    lua_script_process();
    boot_button_process();  // 板载 BOOTSEL 事件（core0 消抖后经跨核队列送来）
    lua_binder_process();   // Lua tick(dt_us)：主循环每轮调用（不节流，dt 兜时差）
  }
}

// core0: 处理 USB Device 事件 + DS5 映射
int main(void) {
  // 系统时钟必须是 12MHz 的整数倍（PIO-USB 时序依赖）。240MHz（与
  // pico-hid-mapper / pico-hid-debugger 同款）：PIO-USB 时序裕量更大，
  // 调试串口 4Mbaud 分频仍为整数（IBRD=3, FBRD=48）
  set_sys_clock_khz(240000, true);

  // set_sys_clock_khz() 重配 PLL 后默认把 clk_peri 切到 48MHz USB PLL，而
  // UART 的时钟源正是 clk_peri —— 显式挂回 clk_sys，恢复"分频按系统时钟算"
  // （921600 误差 0.03%，pico-hid-debugger 同款处理；不影响 PIO-USB，
  //  其时序走 clk_usb 的 48MHz USB PLL）
  clock_configure_undivided(clk_peri,
                            0,
                            CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                            clock_get_hz(clk_sys));

  debug_init();

  // 初始化跨核队列与临界区。必须先于 core1 启动：
  // 生产者随时可能入队，自旋锁必须先就绪
  input_queue_init();
  // BOOTSEL 事件队列（core0 → core1）同理，须在 core1 启动前就绪
  boot_button_init();
  // 键鼠队列在启动 core1 前通过 SDK 初始化临界区，兼容 RP2350 软件锁。
  usb_km_init();
  lua_script_init();

  multicore_reset_core1();
  flash_safe_execute_core_init();
  // 默认 core1 栈只有 2KB，Lua 解析器（luaL_loadbuffer → luaY_parser）栈深
  // 不可控，扩到 16KB（uint32_t 数组 4 字节对齐 + SDK 内部取 8 字节对齐起点，
  // 直接传数组即满足 API 的 uint32_t* 要求）
  static uint32_t s_core1_stack[4096] __attribute__((aligned(8)));   // 16KB
  multicore_launch_core1_with_stack(core1_main, s_core1_stack, sizeof(s_core1_stack));

  while (__atomic_load_n(&s_startup_phase, __ATOMIC_ACQUIRE) != STARTUP_SCRIPT_READY) {
    tight_loop_contents();
  }
  // Device 栈还未初始化，不会连接主机；此处直接应用最终目标，无断开/500ms 等待。
  usb_km_task();
  ds5_pad_init();
  // 在原生 USB 上初始化 Device 栈 (roothub port0)
  tusb_rhport_init_t dev_init = {
    .role  = TUSB_ROLE_DEVICE,
    .speed = TUSB_SPEED_FULL
  };

  tusb_init(0, &dev_init);
  __atomic_store_n(&s_startup_phase, STARTUP_DEVICE_READY, __ATOMIC_RELEASE);

  DEBUG("[DS5C] core0 device ready (bluetooth=%d, tusb_debug=%d)\n",
               ENABLE_DS5_BT, CFG_TUSB_DEBUG);

  uint8_t report[DS_REPORT_LEN];
  while (true) {
    tud_task();
    input_queue_flush();
    ds5_pad_tick();
    boot_button_poll(); // BOOTSEL：10ms 限频读引脚 + 消抖，事件经跨核队列给 core1
    usb_km_task();     // dev_enable 请求轮询，apply 在重枚举窗口内执行
    usb_km_service();  // 键鼠输出队列出队（未启用直接返回）
    usb_io_task();     // IO 通讯接口 echo（未启用直接返回）
    uart_proto_task(); // UART 传输：DMA RX 解析 + 通道 TX
    lua_script_task();

    // DS5 以 1kHz 被主机轮询（bInterval=1）；端点就绪即发送，
    // 未就绪时由主机 NAK 主导节奏，与真实手柄/DS5Dongle 行为一致
    if (tud_hid_ready()) {
      ds5_pad_build_report(report);
      tud_hid_report(0x01, report, DS_REPORT_LEN);
    }
  }

  return 0;
}

//--------------------------------------------------------------------
// USB Device lifecycle callbacks (core0)
//--------------------------------------------------------------------

void tud_mount_cb(void)
{
  DEBUG("[DS5C][USB] MOUNTED: cfg=%d speed=%d hid(kbd=%d mouse=%d)\n",
               tud_mounted(), (int)tud_speed_get(),
               usb_km_kbd_enabled(), usb_km_mouse_enabled());
}

void tud_umount_cb(void)
{
  usb_io_reset_session();
  DEBUG("[DS5C][USB] UMOUNTED: inited=%d mounted=%d\n",
               tud_inited(), tud_mounted());
}

void tud_suspend_cb(bool remote_wakeup_en)
{
  DEBUG("[DS5C][USB] SUSPEND: remote_wakeup=%d\n", remote_wakeup_en);
}

void tud_resume_cb(void)
{
  DEBUG("[DS5C][USB] RESUME\n");
}

//--------------------------------------------------------------------
// USB Host HID 回调（core1）
//--------------------------------------------------------------------

// HID 设备挂载：解析与接管交给 input_app（hidkit 按报告描述符自动判定），
// Switch Pro 手柄需主机握手才会开始上报（模块内部自判 vid/pid）
// 注意: 如果报告描述符长度 > CFG_TUH_ENUMERATION_BUFSIZE，desc_report 为 NULL，
// hidkit 无法解析该设备（保持未接管，报文走未消费路径丢弃）
#if !ENABLE_DS5_BT
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const* desc_report, uint16_t desc_len)
{
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  // 接口号：hidkit 用它做槽位信息记录
  uint8_t itf_num = 0;
  tuh_itf_info_t itf_info;
  if (tuh_hid_itf_get_info(dev_addr, instance, &itf_info)) {
    itf_num = itf_info.desc.bInterfaceNumber;
  }

  input_app_mount(dev_addr, instance, itf_num, itf_protocol,
                  desc_report, desc_len);

  // Switch Pro：非目标设备直接返回；上报订阅照常启动，握手应答报文走
  // tuh_hid_report_received_cb → switch_pro_report 消费
  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  switch_pro_maybe_mount(dev_addr, instance, vid, pid);

  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    DEBUG("[DS5C][HID] cannot request report\n");
  }
}

// HID 设备拔出：清握手状态机槽位，再释放 hidkit 槽位（库内补发全部抬起）
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  switch_pro_umount(dev_addr);
  input_app_umount(instance);
}

// 收到 HID 设备中断端点报告：Switch Pro 握手应答（0x21/0x81）先喂握手模块
// （按 dev_addr 核对消费，hidkit 见到只会标记已消费并跳过解析），再进 hidkit
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const* report, uint16_t len)
{
  if (len != 0) {
    switch_pro_report(dev_addr, instance, report, len);

    // false = hidkit 未消费（instance 未接管 / 报文不认）→ 静默丢弃
    input_app_report(instance, report, len);
  }

  // 继续请求接收下一份报告
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    DEBUG("[DS5C][HID] cannot request report\n");
  }
}

#endif // !ENABLE_DS5_BT
