/*
 * usb_km 实现 — NKRO 键盘 / 16bit 鼠标输出接口的运行期开关。
 *
 * 输出服务循环（usb_km_service）照抄 pico-hid-mapper 的 mouse_hid_service /
 * kbd_hid_service：
 *   鼠标 —— 相对增量报文，端点忙（主机没取走上一帧）时**塞回队尾**，同按键
 *           状态的位移会被 mouse_queue 合并，绝不丢位移；bInterval=1 由主机
 *           1ms 轮询天然限速，每轮最多发一条。
 *   键盘 —— 整份 NKRO 状态是绝对值：先 peek 再发、成功才 commit（端点忙时
 *           保持队首不动下一轮重试，顺序不会乱；队列满丢最旧，最新一条即
 *           最终真相）。
 */

#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "debug.h"
#include "usb_km.h"
#include "kbd_hid.h"
#include "kbd_out.h"
#include "mouse_hid.h"
#include "mouse_queue.h"

// 逐层排查开关：1 = usb_km_init 调用 usb_km_apply（RAM 描述符路径）+ 主循环
// 挂 task/service；0（默认，L1a）= 全部旁路，配置描述符走 flash 原路径。
// CMake -DKM_L1_APPLY=1 切换；排查结束移除。
#ifndef KM_L1_APPLY
#define KM_L1_APPLY 0
#endif

// Lua 请求（core1 写，原子）
static volatile bool s_req_kbd;
static volatile bool s_req_mouse;

// 已生效状态（core0 私有；apply 与重枚举都在 core0）
static bool s_kbd, s_mouse;

void usb_km_init(void)
{
    // 队列先就绪：core1（Lua）随时可能入队
    kbd_out_init(false);
    mouse_queue_init();
    s_kbd = s_mouse = false;
#if KM_L1_APPLY   // L1b 起置 1：L1a 阶段不组装，配置描述符走 flash 原路径
    usb_km_apply(false, false);   // 组装基线描述符（全关 = DS5+音频，与历史一致）
#endif
}

void usb_km_request(bool kbd, bool mouse)
{
    __atomic_store_n(&s_req_kbd, kbd, __ATOMIC_RELEASE);
    __atomic_store_n(&s_req_mouse, mouse, __ATOMIC_RELEASE);
}

bool usb_km_kbd_enabled(void)
{
    return s_kbd;
}

bool usb_km_mouse_enabled(void)
{
    return s_mouse;
}

// core0 主循环：请求与生效状态不一致 → 重枚举切换
void usb_km_task(void)
{
    bool kbd = __atomic_load_n(&s_req_kbd, __ATOMIC_ACQUIRE);
    bool mouse = __atomic_load_n(&s_req_mouse, __ATOMIC_ACQUIRE);
    if (kbd == s_kbd && mouse == s_mouse) return;

    bool was_inited = tud_inited();
    if (was_inited) {
        tud_disconnect();
        sleep_ms(500);   // 让主机完成"设备拔出"，重枚举期间 DS5 上报暂停
    }

    usb_km_apply(mouse, kbd);
    kbd_hid_set_instance(kbd ? (uint8_t)(1u + (mouse ? 1u : 0u)) : KBD_HID_INST_NONE);

    // 关闭侧清队列/状态，防粘键与陈旧位移
    kbd_out_set_enabled(kbd);
    if (!mouse) mouse_queue_clear();

    s_kbd = kbd;
    s_mouse = mouse;

    if (was_inited) {
        tud_connect();
        debug_printf("[DS5C][KM] re-enumerated: kbd=%d mouse=%d\n", kbd, mouse);
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
    if (!s_mouse) return;

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
