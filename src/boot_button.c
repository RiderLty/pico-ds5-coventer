/*
 * BOOTSEL 按键检测与 Lua 事件投递（移植自 pico-hid-mapper，投递路径改为跨核队列）。
 *
 * 数据通路：core0 主循环 boot_button_poll()
 *             → 10ms 限频 + 连续 3 次一致消抖
 *             → multicore_lockout 停住 core1（CS 劫持期间它的 flash 访问会崩）
 *             → read_bootsel_raw() 读 QSPI CS 电平
 *             → 事件压入 SPSC 环
 *           → core1 主循环 boot_button_process() 出队 → lua_binder_on_boot_button()
 */

#include "boot_button.h"

#include "hardware/gpio.h"           // GPIO_OVERRIDE_LOW / GPIO_OVERRIDE_NORMAL
#include "hardware/structs/ioqspi.h" // ioqspi_hw（QSPI CS 控制）
#include "hardware/structs/sio.h"    // sio_hw->gpio_hi_in（可观测 QSPI 引脚）
#include "hardware/sync.h"           // save_and_disable_interrupts
#include "pico/multicore.h"          // multicore_lockout_*
#include "pico/sync.h"               // critical_section
#include "pico/time.h"               // time_us_64

#include "debug.h"
#include "lua_binder.h"
#include "usb_km.h"

/*--------------------------------------------------------------------+
 * core0 → core1：按键事件环（单生产者单消费者，临界区保护）
 *--------------------------------------------------------------------*/

#define BOOT_EVENT_DEPTH 8u

static critical_section_t s_cs;
static uint8_t s_head, s_tail, s_count;
static bool    s_events[BOOT_EVENT_DEPTH];

void boot_button_init(void)
{
    critical_section_init(&s_cs);
    s_head = s_tail = s_count = 0;
}

static void push_event(bool pressed)
{
    critical_section_enter_blocking(&s_cs);
    if (s_count < BOOT_EVENT_DEPTH) {
        s_events[s_tail] = pressed;
        s_tail = (uint8_t)((s_tail + 1u) % BOOT_EVENT_DEPTH);
        s_count++;
    }
    critical_section_exit(&s_cs);
}

void boot_button_process(void)
{
    for (;;) {
        critical_section_enter_blocking(&s_cs);
        if (s_count == 0) {
            critical_section_exit(&s_cs);
            return;
        }
        bool pressed = s_events[s_head];
        s_head = (uint8_t)((s_head + 1u) % BOOT_EVENT_DEPTH);
        s_count--;
        critical_section_exit(&s_cs);

        bool handled = lua_binder_on_boot_button(pressed);
        // BOOTSEL 释放默认开启 WinUSB，键盘和鼠标目标保持不变。
        // Lua 可以在按下或释放回调中返回 true，拦截这次默认行为。
        if (!pressed && !handled) {
            usb_km_request(-1, -1, 1);
        }
    }
}

/*--------------------------------------------------------------------+
 * core0：读引脚 + 消抖
 *--------------------------------------------------------------------*/

// 读 BOOTSEL 原始电平（按下返回 true）。
// BOOTSEL 挂在 QSPI flash 的 CS 上，bootrom 靠它进 USB bootcode；这里把 CS
// 临时置为 Hi-Z，再从 SIO 的 HI GPIO 寄存器观测该脚是否被按键拉低。
// 运行期间关中断且脱离 XIP —— 不得与另一核的 flash 访问并发（由调用方的
// multicore_lockout 保证）。实现同 TinyUSB hw/bsp/rp2040/family.c。
static bool __no_inline_not_in_flash_func(read_bootsel_raw)(void)
{
    const uint CS_PIN_INDEX = 1;

    // 中断处理程序可能驻留 flash，而我们即将短暂脱离 flash 访问
    uint32_t flags = save_and_disable_interrupts();

    // flash CS 置 Hi-Z
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // 此期间不能调用任何可能驻留 flash 的延时函数
    for (volatile int i = 0; i < 1000; ++i) { }

    // 引脚高 = 释放、低 = 按下（取反使按下为 true）
    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);

    // 恢复 CS，否则返回 flash 代码后会出问题
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

// 消抖后的按键状态（true = 按下）
static bool s_confirmed;

void boot_button_poll(void)
{
    static uint64_t s_next_poll_us;
    static uint8_t  s_run;          // 连续偏离确认态的读数计数

    uint64_t now = time_us_64();
    if (now < s_next_poll_us) return;
    s_next_poll_us = now + 10000u;  // 10ms：把 flash 断开窗口压到 100 次/秒

    // CS 劫持会踩踏 XIP 控制器对 CS 的驱动，窗口期内 core1 的 flash 访问会被
    // 破坏导致死机 —— 先把它停进 RAM 中的 victim 循环。core1 暂时关中断不应答
    // 则本轮跳过（victim 由 core1_main 的 flash_safe_execute_core_init 注册）。
    if (!multicore_lockout_start_timeout_us(100)) return;
    bool raw = read_bootsel_raw();
    multicore_lockout_end_blocking();

    if (raw == s_confirmed) {
        s_run = 0;
        return;
    }
    if (++s_run < 3) return;        // 连续 3 次一致才确认变化

    s_confirmed = raw;
    s_run = 0;
    push_event(raw);
    DEBUG("[DS5C][BOOT] bootsel %s\n", raw ? "pressed" : "released");
}
