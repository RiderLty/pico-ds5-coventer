/*
 * kbd_out — 「输出到宿主的 NKRO 键盘」策略与跨核状态
 *
 * 职责分工：
 *   kbd_hid.c  —— 纯 USB 侧（报告描述符、发报告、LED 状态）
 *   本模块     —— 什么时候发、发什么状态：整份 NKRO 状态（修饰键 + 112 位位图）
 *                 由 core1 维护并入队，core0 主循环取出发送。
 *
 * 两个使用场景：
 *   ① 映射关闭时的键盘直通：core.c 的 core_input_keyboard() 里，凡不是框架内部键
 *      （Lua 已消费 / 槽位热键 Alt+F1..F9 / 映射开关切换键）且 !map_on 的事件，
 *      都会经 kbd_out_key() 打到宿主键盘；
 *   ② Lua 的 dkb_key / dkb_release_all：绕过 map_on，直接打给宿主。
 *
 * 为什么是"队列"而不是"最新状态覆盖"：kbd_tap 会在极短时间里产生 down→up 两次状态，
 * 若只留最新状态，core0 还没轮询就被 up 覆盖 → 这一次敲击就丢了。队列保证 down/up
 * 各自成为一条报文；队列满时丢**最旧**（状态是绝对值，最新一条即最终真相）。
 *
 * keycode 取值范围（与报告描述符一致），超出范围的键静默忽略：
 *   0x04..0x73 → 112 位位图（字母/数字/F 键/方向键/编辑键…）
 *   0xE0..0xE7 → 修饰键字节
 *   （媒体键等 0xE8+ 位图装不下，本次不转发，见 CLAUDE.md）
 */
#ifndef KBD_OUT_H
#define KBD_OUT_H

#include <stdint.h>
#include <stdbool.h>
#include "kbd_hid.h"

// 送往宿主键盘的整份状态：修饰键 1 字节 + 112 位（14 字节）位图
typedef struct __attribute__((packed)) {
  uint8_t modifier;
  uint8_t bitmap[KBD_HID_NKRO_BYTES];
} kbd_out_state_t;

_Static_assert(sizeof(kbd_out_state_t) == 1 + KBD_HID_NKRO_BYTES,
               "kbd_out_state_t 必须是 15 字节（修饰 1 + 位图 14），与 NKRO 报告一致");

// 开关来源：参数配置的 bitmap 位 PARAM_BIT_KBD_OUT（0=关，**默认**；需重启生效）。
// main() 用 param_cfg_preload_bool() 读出后，经 kbd_out_init() 交给本模块。

// 初始化队列与使能位（main() 在 core1 启动前调用一次）
void kbd_out_init(bool enabled);

void kbd_out_set_enabled(bool on);
bool kbd_out_enabled(void);

// ── core1（映射引擎 / Lua）────────────────────────────────────────────
// 以下接口在未启用时静默 no-op；keycode 超出可转发范围同样忽略。
void kbd_out_key(uint8_t keycode, bool down);
void kbd_out_release_all(void);           // 清空所有键与修饰（防粘键安全阀）

// ── 查询（Lua 的 dkb_state / dkb_is_down）─────────────────────────────
// 取"宿主当前应该看到"的整份状态（最新那条；不是队列里还没发出去的那条）。
// 未启用时恒为全 0。
void kbd_out_state(kbd_out_state_t *out);

// 单键查询；keycode 不在可转发范围（0x04..0x73 / 0xE0..0xE7）时返回 false
bool kbd_out_is_down(uint8_t keycode);

// 整份状态的 16 字节小端视图：字节 1 = 修饰键位图，字节 2..15 = 112 位键位图，
// 字节 16 = 补齐（恒 0）。于是 keycode (0x04 + n) 对应第 (8 + n) 位。
void kbd_out_state_bytes(uint8_t out[16]);

// 同上的 4 个 u32（小端）视图，供 Lua 的 dkb_state() 直接 push
void kbd_out_state_words(uint32_t out[4]);

// ── core0（USB 主循环）────────────────────────────────────────────────
// 看队首但不弹出；无待发返回 false。
// 必须"先 peek 再发、成功后 commit"——端点忙时若先弹出再放回队尾，
// 会让旧状态排到新状态之后，宿主会先看到过期状态。
bool kbd_out_peek(kbd_out_state_t *out);

// 发送成功后提交（弹出队首）
void kbd_out_commit(void);

// 队列积压条数（诊断用）
uint16_t kbd_out_count(void);

#endif // KBD_OUT_H
