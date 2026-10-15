/*
 * usb_km 实现 — NKRO 键盘 / 16bit 鼠标 / custom IO 通讯接口的运行期开关。
 *
 * 输出服务循环（usb_km_service）照抄 pico-hid-mapper 的 mouse_hid_service /
 * kbd_hid_service：
 *   鼠标 —— 相对增量报文，端点忙（主机没取走上一帧）时**塞回队尾**，同按键
 *           状态的位移会被 mouse_queue 合并，绝不丢位移；bInterval=1 由主机
 *           1ms 轮询天然限速，每轮最多发一条。
 *   键盘 —— 整份 NKRO 状态是绝对值：先 peek 再发、成功才 commit（端点忙时
 *           保持队首不动下一轮重试，顺序不会乱；队列满丢最旧，最新一条即
 *           最终真相）。
 *   IO   —— echo 由 usb_io_task 独立处理，不经本文件的队列。
 */

#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "debug.h"
#include "usb_km.h"
#include "usb_io.h"
#include "kbd_hid.h"
#include "kbd_out.h"
#include "mouse_hid.h"
#include "mouse_queue.h"

// 全部状态 BSS 静态初始化（全关 = 全零）。usb_km_apply 在 usb_km_task 的
// 状态切换（重枚举窗口）内惰性执行：boot 时未应用 → 配置描述符回调返回
// flash 基线（与无 km 模块时逐字节一致）；ability_enable 请求 →
// tud_disconnect → apply → tud_connect。队列临界区在双核启动前由
// usb_km_init 初始化。

// Lua 请求（core1 写，单次原子 store 三位一起生效）
static volatile uint32_t s_req_mask;

// 已生效状态（core0 私有；apply 与重枚举都在 core0）。
// 状态默认全关，队列锁在双核启动前初始化。
static uint32_t s_applied_mask;

void usb_km_init(void)
{
    kbd_out_init();
    mouse_queue_init();
    usb_io_init();
    s_applied_mask = 0u;
    __atomic_store_n(&s_req_mask, 0u, __ATOMIC_RELAXED);
    debug_printf("[DS5C][KM] queues initialized\n");
}

void usb_km_request(bool kbd, bool mouse, bool io)
{
    uint32_t mask = (kbd  ? USB_KM_BIT_KBD   : 0u)
                  | (mouse ? USB_KM_BIT_MOUSE : 0u)
                  | (io   ? USB_KM_BIT_IO    : 0u);
    __atomic_store_n(&s_req_mask, mask, __ATOMIC_RELEASE);
}

bool usb_km_kbd_enabled(void)
{
    return (s_applied_mask & USB_KM_BIT_KBD) != 0u;
}

bool usb_km_mouse_enabled(void)
{
    return (s_applied_mask & USB_KM_BIT_MOUSE) != 0u;
}

bool usb_km_io_enabled(void)
{
    return (s_applied_mask & USB_KM_BIT_IO) != 0u;
}

// core0 主循环：请求与生效状态不一致 → 重枚举切换
void usb_km_task(void)
{
    uint32_t req = __atomic_load_n(&s_req_mask, __ATOMIC_ACQUIRE);
    if (req == s_applied_mask) return;

    bool kbd  = (req & USB_KM_BIT_KBD) != 0u;
    bool mouse = (req & USB_KM_BIT_MOUSE) != 0u;
    bool io   = (req & USB_KM_BIT_IO) != 0u;

    bool was_inited = tud_inited();
    debug_printf("[DS5C][KM] reload: old(kbd=%d mouse=%d io=%d) -> "
                 "new(kbd=%d mouse=%d io=%d), inited=%d mounted=%d\n",
                 (int)usb_km_kbd_enabled(), (int)usb_km_mouse_enabled(),
                 (int)usb_km_io_enabled(),
                 kbd, mouse, io, was_inited, tud_mounted());
    if (was_inited) {
        tud_disconnect();
        sleep_ms(500);   // 让主机完成"设备拔出"，重枚举期间 DS5 上报暂停
    }

    usb_km_apply(mouse, kbd, io);
    uint8_t kbd_instance = kbd ? (uint8_t)(1u + (mouse ? 1u : 0u)) : KBD_HID_INST_NONE;
    kbd_hid_set_instance(kbd_instance);

    // 关闭侧清队列/状态，防粘键与陈旧位移；IO 实例在重枚举窗口内切换
    kbd_out_set_enabled(kbd);
    if (!mouse) mouse_queue_clear();
    usb_io_set_instance(io ? (uint8_t)(1u + (mouse ? 1u : 0u) + (kbd ? 1u : 0u))
                           : USB_IO_INST_NONE);

    s_applied_mask = req;

    if (was_inited) {
        tud_connect();
        debug_printf("[DS5C][KM] reconnect requested; waiting for host enumeration\n");
    }
}

// NKRO 键盘：状态队列出队发送（peek → 发送成功才 commit）
static void kbd_hid_service(void)
{
    if (!kbd_hid_enabled()) return;

    kbd_out_state_t st;
    if (!kbd_out_peek(&st)) return;

    if (kbd_hid_send_nkro(st.modifier, st.bitmap)) {
        kbd_out_commit();
    }
}

// 16bit 鼠标：报文队列出队发送；端点忙时塞回队尾（同按键状态自动合并）
static void mouse_hid_service(void)
{
    if (!usb_km_mouse_enabled()) return;

    mouse_report_t rep;
    if (!mouse_queue_pop(&rep)) return;

    if (!mouse_hid_send(&rep)) {
        mouse_queue_push(rep.dx, rep.dy, rep.wheel, rep.buttons);
    }
}

void usb_km_service(void)
{
    kbd_hid_service();
    mouse_hid_service();
}
