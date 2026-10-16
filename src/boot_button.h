#ifndef BOOT_BUTTON_H
#define BOOT_BUTTON_H

#include <stdbool.h>

/*
 * BOOTSEL 按键（板上唯一实体按键）检测 → Lua on_boot_button(pressed) 事件。
 * 回调返回 true 可拦截默认行为；否则释放事件会请求开启 WinUSB，并保持键盘、
 * 鼠标目标状态不变。
 *
 * 为什么必须跨核：BOOTSEL 接在 QSPI flash 的 CS 引脚上，读它要把 CS 临时置
 * Hi-Z 并脱离 XIP —— 窗口期内**另一核的任何 flash 访问都会崩**。所以：
 *   · core0 负责轮询（10ms 限频 + 连续 3 次读数一致才确认变化），读取前用
 *     multicore_lockout 把 core1 停进 RAM 中的 victim 循环；
 *   · 事件经本模块的小型 SPSC 环投给 core1，由 core1 在 Lua VM 所在线程调用
 *     lua_binder_on_boot_button()（VM 禁止跨核直调）。
 *
 * 实现移植自 pico-hid-mapper 的 get_bootsel_button() / bootsel_button_poll()；
 * mapper 经 IPC job 投递，本工程没有 IPC 框架，改用跨核环形队列。
 *
 * 接线：
 *   boot_button_init()    core0，multicore_launch_core1() 之前
 *   boot_button_poll()    core0 主循环（内部限频，可直接每轮调用）
 *   boot_button_process() core1 主循环（须与 Lua VM 同核）
 */

void boot_button_init(void);      // core0：初始化跨核队列，须早于 core1 启动
void boot_button_poll(void);      // core0：读引脚 + 消抖 + 投递事件
void boot_button_process(void);   // core1：排空事件并转发 Lua

#endif // BOOT_BUTTON_H
