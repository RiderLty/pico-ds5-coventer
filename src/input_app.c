/*
 * hidkit 输入接线层实现（参考 pico-hid-mapper 的 src/hidkit_app.c 精简）。
 *
 * 与 mapper 版的差异（本工程只保留"同样的输入能力"）：
 *   - 出口分两路：先转发 Lua（返回 true = 拦截，跳过 C 默认路径，映射逻辑
 *     交由脚本接管），未拦截才进 C 默认状态机并打包进跨核队列：
 *       键盘边沿   → 虚拟 8B 键盘报文（mod + keys[6]），INQ_KIND_KB
 *       鼠标按键   → 按键位字节，随下一帧 mouse_abs 带出，INQ_KIND_MS
 *       手柄按键   → BTN_* 位掩码，随 PAD 帧带出，INQ_KIND_PAD
 *       手柄轴     → i16 四轴 + u8 双扳机，INQ_KIND_PAD
 *   - 六轴/触摸板：C 侧无消费，仅转发 Lua（脚本未监听时零开销）
 *   - Switch Pro 握手库移植层在此收口（时间/日志/校准交付）
 *   - usbh_app_driver_get_cb 全工程唯一定义（XInput 类驱动注册）
 *   - lua_binder_rebind 在此实现：print/warn 覆盖 + hidkit 信息表
 *
 * include 顺序：先 tusb.h 再 hidkit —— hidkit 的 MOUSE_BUTTON_* 宏与
 * TinyUSB 的 HID 枚举名重叠（见 hidkit_switch_pro_glue.h 的说明）。
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <lua.h>       // _lcb_print/_lcb_warn 直接操作 lua_State
#include <lauxlib.h>

#include "pico/time.h"
#include "tusb.h"

#include "hidkit.h"
#include "hidkit_xinput_glue.h"
#include "switch_pro.h"
#include "hidkit_switch_pro_glue.h"

#include "debug.h"
#include "input_app.h"
#include "input_queue.h"
#include "lua_binder.h"
#include "lua_core.h"

// instance → hidkit 槽位；-1 = hidkit 未接管该接口
static int8_t s_slot[CFG_TUH_HID];

// 设备登记表（hidkit.info() 的数据源；instance 为 TinyUSB 全局接口下标）
typedef struct {
    uint16_t vid, pid;
    uint8_t  dev_addr, itf, proto;
    int8_t   slot;
} dev_reg_t;
static dev_reg_t s_dev[CFG_TUH_HID];

// 虚拟键盘报文状态（边沿事件累积；多键盘合并，与旧路径"最新报文生效"等价）
static uint8_t s_kb_mod;
static uint8_t s_kb_keys[6];

// 鼠标按键位（bit0 左 / bit1 右 / bit2 中…，随下一帧 mouse_abs 带出）
static uint8_t s_ms_buttons;
static uint8_t s_ms_buttons_sent;   // 上一次已入队帧携带的按键位（全零移动帧去重用）

// 手柄状态（多手柄合并到最后一份状态上；转换器场景可接受）
static int16_t s_pad_lsx, s_pad_lsy, s_pad_rsx, s_pad_rsy;
static uint8_t s_pad_lt, s_pad_rt;
static uint32_t s_pad_btnmask;

/*--------------------------------------------------------------------+
 * 队列打包
 *--------------------------------------------------------------------*/

// 虚拟键盘报文（mod + reserved + keys[6]）入队
static void send_kb(void)
{
    uint8_t payload[8];
    payload[0] = s_kb_mod;
    payload[1] = 0;
    memcpy(&payload[2], s_kb_keys, sizeof(s_kb_keys));
    input_queue_send(INQ_KIND_KB, payload, sizeof(payload));
}

// 鼠标帧（buttons u8, wheel i8, x i16, y i16）入队
static void send_ms(int32_t wheel, int32_t dx, int32_t dy)
{
    uint8_t payload[6];
    payload[0] = s_ms_buttons;
    payload[1] = (uint8_t)wheel;
    payload[2] = (uint8_t)(dx & 0xFF);
    payload[3] = (uint8_t)((dx >> 8) & 0xFF);
    payload[4] = (uint8_t)(dy & 0xFF);
    payload[5] = (uint8_t)((dy >> 8) & 0xFF);
    input_queue_send(INQ_KIND_MS, payload, sizeof(payload));
}

// 手柄帧（见 input_queue.h 布局）入队
static void send_pad(void)
{
    uint8_t payload[INQ_PAD_PAYLOAD];
    payload[0] = 0x01u;                                 // flags: connected
    payload[1] = (uint8_t)(s_pad_lsx & 0xFF);
    payload[2] = (uint8_t)(((uint16_t)s_pad_lsx >> 8) & 0xFF);
    payload[3] = (uint8_t)(s_pad_lsy & 0xFF);
    payload[4] = (uint8_t)(((uint16_t)s_pad_lsy >> 8) & 0xFF);
    payload[5] = (uint8_t)(s_pad_rsx & 0xFF);
    payload[6] = (uint8_t)(((uint16_t)s_pad_rsx >> 8) & 0xFF);
    payload[7] = (uint8_t)(s_pad_rsy & 0xFF);
    payload[8] = (uint8_t)(((uint16_t)s_pad_rsy >> 8) & 0xFF);
    // 扳机已在 hidkit_input_gamepad_abs 折算 0..32767 → 0..255
    payload[9]  = s_pad_lt;
    payload[10] = s_pad_rt;
    payload[11] = (uint8_t)(s_pad_btnmask & 0xFF);
    payload[12] = (uint8_t)((s_pad_btnmask >> 8) & 0xFF);
    payload[13] = (uint8_t)((s_pad_btnmask >> 16) & 0xFF);
    payload[14] = (uint8_t)((s_pad_btnmask >> 24) & 0xFF);
    input_queue_send(INQ_KIND_PAD, payload, sizeof(payload));
}

// 手柄断连标记（core0 据此清手柄状态、回落键鼠映射）
static void send_pad_disconnect(void)
{
    static const uint8_t payload[INQ_PAD_PAYLOAD] = { 0 };
    input_queue_send(INQ_KIND_PAD, payload, sizeof(payload));
}

/*--------------------------------------------------------------------+
 * hidkit 出口（弱符号覆盖；热路径，全部 not_in_flash）
 *--------------------------------------------------------------------*/

// 键盘 + 鼠标按键 + 手柄按键统一出口，code 段前缀区分类型（hidkit_codes.h）。
// 各段先转发 Lua（返回 true = 拦截，跳过 C 默认路径），未拦截才进 C 状态机。
void __no_inline_not_in_flash_func(hidkit_input_key)(int8_t slot, uint16_t code, bool pressed)
{
    (void)slot;

    switch (HIDKIT_CODE_SEG(code)) {
    case HIDKIT_CODE_KEYBOARD: {
        uint8_t usage = (uint8_t)(code & 0xFFu);
        if (lua_binder_on_key(usage, pressed)) return;   // Lua 拦截

        if (usage >= 0xE0u && usage <= 0xE7u) {
            // 修饰键：HID 报文 byte0 的位序 = usage - 0xE0
            uint8_t bit = (uint8_t)(1u << (usage - 0xE0u));
            if (pressed) s_kb_mod |= bit;
            else         s_kb_mod &= (uint8_t)~bit;
        } else {
            // 普通键：6 槽位表（满则丢弃，标准 6-key rollover 行为）
            uint8_t i;
            if (pressed) {
                for (i = 0; i < sizeof(s_kb_keys); i++) {
                    if (s_kb_keys[i] == usage) return;   // 重复事件
                    if (s_kb_keys[i] == 0) { s_kb_keys[i] = usage; break; }
                }
                if (i == sizeof(s_kb_keys)) return;      // 槽位满
            } else {
                bool found = false;
                for (i = 0; i < sizeof(s_kb_keys); i++) {
                    if (s_kb_keys[i] == usage) { s_kb_keys[i] = 0; found = true; }
                }
                if (!found) return;
            }
        }
        send_kb();
        break;
    }

    case HIDKIT_CODE_MOUSE: {
        uint8_t num = (uint8_t)(code & 0xFFu);
        if (num >= 8) return;
        if (lua_binder_on_mouse_btn(num, pressed)) return;   // Lua 拦截

        uint8_t bit = (uint8_t)(1u << num);
        if (pressed) s_ms_buttons |= bit;
        else         s_ms_buttons &= (uint8_t)~bit;
        // 按键变化随下一帧 mouse_abs 带出；位移为零的按下/抬起也发一帧，
        // 保证 core0 的扳机立即响应
        send_ms(0, 0, 0);
        break;
    }

    case HIDKIT_CODE_GAMEPAD: {
        uint8_t btn = (uint8_t)(code & 0xFFu);
        if (btn > 31u) return;
        if (lua_binder_on_gamepad_btn(btn, pressed)) return;   // Lua 拦截

        if (pressed) s_pad_btnmask |= (1u << btn);
        else         s_pad_btnmask &= ~(1u << btn);
        // 不在此发帧：hidkit 的按键边沿后总跟着同一份报文的 gamepad_abs
        // （含 umount 补发的全零轴），随那帧带出位掩码即可
        break;
    }

    default:
        break;
    }
}

// 鼠标位移与滚轮（仅非零时被调用）。Lua 可分别拦截位移/滚轮（清零跳过）
void __no_inline_not_in_flash_func(hidkit_input_mouse_abs)(int8_t slot, int32_t dx, int32_t dy, int32_t wheel)
{
    (void)slot;
    lua_binder_on_mouse_move(&dx, &dy);
    lua_binder_on_mouse_wheel(&wheel);
    if (dx == 0 && dy == 0 && wheel == 0 && s_ms_buttons == s_ms_buttons_sent) return;
    s_ms_buttons_sent = s_ms_buttons;
    send_ms(wheel, dx, dy);
}

// 手柄绝对状态：每份解析成功的报文都回调（125~1000Hz）
void __no_inline_not_in_flash_func(hidkit_input_gamepad_abs)(int8_t slot, int32_t ls_x, int32_t ls_y,
                                                             int32_t rs_x, int32_t rs_y,
                                                             int32_t lt, int32_t rt, uint32_t buttons)
{
    (void)buttons;   // 按键另经 key 出口的 GAMEPAD 段进入位掩码
    if (ls_x < -32767) ls_x = -32767;   // 闭区间 -32767..32767（hidkit 约定）
    if (ls_y < -32767) ls_y = -32767;
    if (rs_x < -32767) rs_x = -32767;
    if (rs_y < -32767) rs_y = -32767;
    if (lt < 0) lt = 0; else if (lt > 32767) lt = 32767;
    if (rt < 0) rt = 0; else if (rt > 32767) rt = 32767;

    if (lua_binder_on_gamepad_abs(slot, ls_x, ls_y, rs_x, rs_y, lt, rt)) return;   // Lua 拦截

    s_pad_lsx = (int16_t)ls_x;
    s_pad_lsy = (int16_t)ls_y;
    s_pad_rsx = (int16_t)rs_x;
    s_pad_rsy = (int16_t)rs_y;
    s_pad_lt  = (uint8_t)(lt >> 7);
    s_pad_rt  = (uint8_t)(rt >> 7);

    send_pad();
}

// 六轴：C 侧无消费，仅转发 Lua（enable_listen_motion 门控）
void hidkit_input_gamepad_motion(int8_t slot, int16_t gx, int16_t gy, int16_t gz,
                                 int16_t ax, int16_t ay, int16_t az)
{
    lua_binder_on_motion(slot, gx, gy, gz, ax, ay, az);
}

// 触摸板快照：C 侧无消费，仅转发 Lua（binder 内置逐字节去重）
void hidkit_input_touchpad(int8_t slot, const hidkit_touchpad_t *tp)
{
    (void)slot;
    if (!tp) return;
    lua_binder_on_touchpad(tp->meta, tp->dims, tp->x, tp->y);
}

// 槽位耗尽且策略为 DROP_NEW 时的一次性通知（默认 EVICT_IDLE，走不到这儿）
void hidkit_input_dropped(int8_t slot, uint16_t vid, uint16_t pid)
{
    lua_binder_on_dropped(slot, vid, pid);
    debug_printf("[DS5C][HID] dropped slot=%d %04x:%04x\n", (int)slot, vid, pid);
}

// hidkit 库内诊断（冷路径：挂载/认领判定/挤出/首次未消费）
void hidkit_debug_printf(const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(msg) - 1) n = (int)sizeof(msg) - 1;

    // 库内消息自带换行，剥掉后统一前缀
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    msg[n] = '\0';
    debug_printf("[DS5C][HK] %s\n", msg);
}

/*--------------------------------------------------------------------+
 * HID 侧转发
 *--------------------------------------------------------------------*/

void input_app_init(void)
{
    for (uint8_t i = 0; i < CFG_TUH_HID; i++) s_slot[i] = -1;
    hidkit_init();
}

void input_app_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num,
                     uint8_t itf_proto, uint8_t const *desc_report, uint16_t desc_len)
{
    if (instance >= CFG_TUH_HID) return;
    s_slot[instance] = -1;

    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    static const char *proto_str[] = { "None", "Keyboard", "Mouse", "Gamepad" };

    hidkit_dev_info_t info;
    info.vid = vid;
    info.pid = pid;
    info.dev_addr = dev_addr;
    info.itf = itf_num;
    // TinyUSB 的 hid_interface_protocol_enum_t 与 HIDKIT_PROTO_* 数值一致，
    // 但仍显式映射：hidkit 不依赖任何栈的头文件
    info.proto = (itf_proto == HID_ITF_PROTOCOL_KEYBOARD) ? HIDKIT_PROTO_KEYBOARD
               : (itf_proto == HID_ITF_PROTOCOL_MOUSE)    ? HIDKIT_PROTO_MOUSE
                                                          : HIDKIT_PROTO_NONE;
    info.report_desc = desc_report;
    info.report_desc_len = desc_len;

    int8_t slot = hidkit_mount(&info);
    s_slot[instance] = slot;   // <0 = 库不认，后续报文不再打扰库

    // 登记设备信息（Lua hidkit.info() 的数据源；未接管也登记 —— 排查"为什么不认"用）
    s_dev[instance].vid = vid;
    s_dev[instance].pid = pid;
    s_dev[instance].dev_addr = dev_addr;
    s_dev[instance].itf = itf_num;
    s_dev[instance].proto = info.proto;
    s_dev[instance].slot = slot;

    debug_printf("[DS5C][HID] mount %04x:%04x addr=%u itf=%u proto=%s slot=%d\n",
                 vid, pid, dev_addr, itf_num,
                 proto_str[info.proto <= HIDKIT_PROTO_GAMEPAD ? info.proto : 0],
                 (int)slot);
}

void input_app_umount(uint8_t instance)
{
    if (instance >= CFG_TUH_HID) return;
    memset(&s_dev[instance], 0, sizeof(s_dev[instance]));

    int8_t const slot = s_slot[instance];
    if (slot >= 0) {
        if (hidkit_is_gamepad(slot)) {
            send_pad_disconnect();   // core0 回落键鼠映射
        }
        // 库内先补发该槽位仍按着的键/鼠标键"松开"、手柄轴归零
        hidkit_umount(slot);
        s_slot[instance] = -1;
    }
}

bool input_app_report(uint8_t instance, uint8_t const *report, uint16_t len)
{
    if (instance >= CFG_TUH_HID) return false;
    int8_t const slot = s_slot[instance];
    if (slot < 0) return false;

    // false = 这份报文本库不认（如已认领的手柄收到了非手柄报文）
    return hidkit_report(slot, report, len);
}

void input_app_task(void)
{
    // Switch Pro 握手状态机：非阻塞时间步进；发送/超时重试都在这里推进
    switch_pro_task();
}

/*--------------------------------------------------------------------+
 * Lua 应用层绑定（lua_binder_rebind；VM 每次创建/重建时被调用）
 *
 * - print / warn 覆盖 → 调试串口（须在 openlibs 之后注册才能覆盖内建同名全局）
 * - hidkit 表：info() 打印设备登记表 + BTN_*、段前缀、量程常量（脚本可读地写映射）
 *--------------------------------------------------------------------*/

// print(...)：参数以 tab 分隔拼接，走调试串口
static int _lcb_print(lua_State *L) {
    int n = lua_gettop(L);
    char buf[200];
    size_t off = 0;
    for (int i = 1; i <= n && off < sizeof(buf) - 1; i++) {
        if (i > 1) buf[off++] = '\t';
        if (off >= sizeof(buf) - 1) break;
        size_t l;
        const char *s = luaL_tolstring(L, i, &l);   // 尊重 __tostring；结果压栈
        size_t space = sizeof(buf) - 1 - off;
        if (l > space) l = space;
        memcpy(buf + off, s, l);
        off += l;
        lua_pop(L, 1);                              // 弹出 tostring 结果，保持栈平衡
    }
    buf[off] = '\0';
    debug_printf("[lua] %s\n", buf);
    return 0;
}

static int _lcb_warn(lua_State *L) {
    int n = lua_gettop(L);
    char buf[200];
    size_t off = 0;
    for (int i = 1; i <= n && off < sizeof(buf) - 1; i++) {
        size_t l;
        const char *s = luaL_tolstring(L, i, &l);
        if (i == 1 && l > 0 && s[0] == '@') { lua_pop(L, 1); return 0; }  // 控制消息，忽略
        size_t space = sizeof(buf) - 1 - off;
        if (l > space) l = space;
        memcpy(buf + off, s, l);
        off += l;
        lua_pop(L, 1);
    }
    buf[off] = '\0';
    debug_printf("[lua warn] %s\n", buf);
    return 0;
}

// hidkit.info()：打印设备登记表（挂载/拔出时更新）
static int _lcb_hidkit_info(lua_State *L) {
    (void)L;
    static const char *proto_str[] = { "None", "Keyboard", "Mouse", "Gamepad" };
    uint8_t count = 0;
    for (uint8_t i = 0; i < CFG_TUH_HID; i++) {
        if (s_dev[i].dev_addr == 0) continue;
        debug_printf("[lua] [%02d] %04x:%04x addr=%u itf=%u proto=%s slot=%d\n",
                     i, s_dev[i].vid, s_dev[i].pid, s_dev[i].dev_addr,
                     s_dev[i].itf, proto_str[s_dev[i].proto <= HIDKIT_PROTO_GAMEPAD
                                             ? s_dev[i].proto : 0],
                     (int)s_dev[i].slot);
        count++;
    }
    if (count == 0) debug_printf("[lua] no host devices\n");
    debug_printf("[lua] device_type=%u (0 none / 1 kb / 2 mouse)\n", hidkit_device_type());
    return 0;
}

void lua_binder_rebind(void) {
    lua_binder_bind("print", _lcb_print);
    lua_binder_bind("warn",  _lcb_warn);


    // hidkit 表：信息查询 + 常量（段前缀 / BTN_* / 六轴量程锚点）
    lua_core_register_in("hidkit", "info", _lcb_hidkit_info);
    lua_core_table_int("hidkit", "CODE_KEYBOARD", HIDKIT_CODE_KEYBOARD);
    lua_core_table_int("hidkit", "CODE_MOUSE",    HIDKIT_CODE_MOUSE);
    lua_core_table_int("hidkit", "CODE_GAMEPAD",  HIDKIT_CODE_GAMEPAD);
    lua_core_table_int("hidkit", "CODE_SEG_MASK", 0x0F00u);
    lua_core_table_int("hidkit", "PROTO_NONE",     HIDKIT_PROTO_NONE);
    lua_core_table_int("hidkit", "PROTO_KEYBOARD", HIDKIT_PROTO_KEYBOARD);
    lua_core_table_int("hidkit", "PROTO_MOUSE",    HIDKIT_PROTO_MOUSE);
    lua_core_table_int("hidkit", "PROTO_GAMEPAD",  HIDKIT_PROTO_GAMEPAD);
    lua_core_table_int("hidkit", "BTN_A",      BTN_A);
    lua_core_table_int("hidkit", "BTN_B",      BTN_B);
    lua_core_table_int("hidkit", "BTN_X",      BTN_X);
    lua_core_table_int("hidkit", "BTN_Y",      BTN_Y);
    lua_core_table_int("hidkit", "BTN_LB",     BTN_LB);
    lua_core_table_int("hidkit", "BTN_RB",     BTN_RB);
    lua_core_table_int("hidkit", "BTN_LT",     BTN_LT);
    lua_core_table_int("hidkit", "BTN_RT",     BTN_RT);
    lua_core_table_int("hidkit", "BTN_SELECT", BTN_SELECT);
    lua_core_table_int("hidkit", "BTN_START",  BTN_START);
    lua_core_table_int("hidkit", "BTN_LS",     BTN_LS);
    lua_core_table_int("hidkit", "BTN_RS",     BTN_RS);
    lua_core_table_int("hidkit", "BTN_HOME",   BTN_HOME);
    lua_core_table_int("hidkit", "BTN_MISC",   BTN_MISC);
    lua_core_table_int("hidkit", "DPAD_UP",    BTN_DPAD_UP);
    lua_core_table_int("hidkit", "DPAD_DOWN",  BTN_DPAD_DOWN);
    lua_core_table_int("hidkit", "DPAD_LEFT",  BTN_DPAD_LEFT);
    lua_core_table_int("hidkit", "DPAD_RIGHT", BTN_DPAD_RIGHT);
    lua_core_table_int("hidkit", "GYRO_FULL_SCALE_DPS", HIDKIT_MOTION_GYRO_FULL_SCALE_DPS);
    lua_core_table_int("hidkit", "ACCEL_FULL_SCALE_MG", HIDKIT_MOTION_ACCEL_FULL_SCALE_MG);
}

/*--------------------------------------------------------------------+
 * Switch Pro 握手库移植层 + 校准交付（全部回调在 core1）
 *--------------------------------------------------------------------*/

uint32_t switch_pro_time_us(void)
{
    return time_us_32();
}

// error=true = 该设备初始化失败（停机等待重插）；false = 进度/校准信息
void switch_pro_log(bool error, const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(msg) - 1) n = (int)sizeof(msg) - 1;

    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    msg[n] = '\0';
    debug_printf("[DS5C][PRO]%s %s\n", error ? "!" : "", msg);
}

// 握手模块经 SPI 读回手柄校准后回调（读超时回落出厂/缺省校准，valid=0 也会
// 送达 —— 注入 false 即 hidkit 用缺省映射，0x30 流不受影响）
void switch_pro_calibration(uint8_t dev_addr, uint8_t instance,
                            const switch_pro_calib_t *cal)
{
    (void)dev_addr;
    if (instance >= CFG_TUH_HID) return;
    int8_t const slot = s_slot[instance];
    if (slot < 0) return;   // hidkit 尚未接管（罕见：校准先于挂载完成）

    hidkit_switchpro_calib_t converted = hidkit_switch_pro_calibration(cal);
    (void)hidkit_switchpro_set_calib(slot, &converted);
    debug_printf("[DS5C][PRO] calibration applied (valid=0x%04x)\n", cal->valid);
}

/*--------------------------------------------------------------------+
 * XInput：注册适配器的类驱动
 *
 * usbh_app_driver_get_cb() 是 Host 栈里唯一的应用类驱动注册钩子，全工程
 * 只能有一个定义。以后再加别的类驱动在这里一起返回、driver_count 加一。
 *--------------------------------------------------------------------*/

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return hidkit_tusb_xinput_driver();
}
