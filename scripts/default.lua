-- ═══════════════════════════════════════════════════════════════════════════
-- Pico DS5 转换器 · 板载预制脚本
--   KV 里没有已存脚本、或已存脚本加载失败时，固件运行这一份。
--   这一份是**全量直通管线**：把输入原样转给输出，可作为自己写脚本的起点。
--   默认不再自动启用键鼠 / WinUSB；运行中按下再松开 BOOT 即可启用 WinUSB。
--   键鼠接口需自行用 dev_enable 开启；文末保留了已注释的 5s 延时启用示例。
-- ═══════════════════════════════════════════════════════════════════════════
--
-- 架构：输入与输出完全解耦，中间只有 Lua。
--   · 输入 —— hidkit 解析出的键鼠 / 手柄 / 触摸 / 六轴事件**全量**直投同名回调，
--     **定义哪个回调就收哪个事件**，没有任何开关或声明；没定义的回调只有一次
--     整数比较的开销。除 on_boot_button 外，回调返回值都会被忽略。
--   · 输出 —— 脚本直接调用输出函数。手柄/触摸/六轴经跨核队列驱动 DS5 报告；
--     键鼠经跨核队列驱动宿主 HID 接口。
--   · 脚本上限 60 KiB（UTF-8 字节）。「加载到内存」只热重载、不写 Flash；
--     要断电保留必须手动点「写入存储」。
--
-- ── 输入回调（全部可选，按需定义）────────────────────────────────────────
--   on_key(keycode, down)                 键盘按键（HID Usage ID；修饰键 0xE0..0xE7）
--   on_mouse_btn(button, down)            鼠标按键 0=左 1=右 2=中 3=后退 4=前进
--                                         （设备可能报 0..7，5..7 没有对应输出）
--   on_mouse_move(dx, dy)                 鼠标相对位移（正 dx=右，正 dy=下）
--   on_mouse_wheel(wheel)                 滚轮（正 = 上滚）
--   on_gamepad_btn(code, pressed)         手柄按键（code = hidkit.BTN_*，0..31）
--   on_gamepad_abs(lsx, lsy, rsx, rsy, lt, rt)
--                                         手柄轴 / 扳机（轴 ±32767，扳机 0..32767）
--   on_motion(gx, gy, gz, ax, ay, az)     六轴（±32767 = 陀螺 ±2000dps / 加速度 ±4g）
--   on_touchpad(meta, dims, x0, y0, x1, y1)  DS5 触摸板快照，共 2 个触点：
--                                         meta 的 bit0/bit1 = 对应触点在位，
--                                         dims = 宽 | (高 << 16)
--   on_boot_button(pressed)               板载 BOOTSEL 按键（设备侧 10ms 消抖）
--   on_dropped(slot, vid, pid)            槽位耗尽丢弃设备时的一次性通知
--   tick(dt_us)                           默认至少间隔 1000us（最高 1000Hz）
--                                         dt = 实际回调间隔（微秒），首个为 0
--
-- ── 输出 · 宿主键盘 / 鼠标 / IO 通讯接口 ─────────────────────────────────
--   这一组受 dev_enable 控制；接口没开时调用是 no-op。
--   dev_enable(kbd, mouse, io)            开关对外接口。三个参数各为三态：
--                                         -1 = 不修改，0 = 关闭，1 = 开启
--                                         （省略的参数按 -1）。
--                                         启动脚本顶层设置用于首次枚举，无需重枚举。
--                                         运行中异步生效；配置改变会重枚举（~0.5s，
--                                         期间 DS5 从主机短暂消失）；相同配置不重载。
--   若想插上 PC 即启用接口，在脚本顶层添加 dev_enable(1, 1, 1)，保存后重新上电。
--   顶层代码全部执行完才应用配置；不要在顶层循环等待 *_is_on() 变为 true。
--   kbd_key(keycode, down)                宿主键盘按键（Usage 0x04..0x73，NKRO 全键无冲）
--   kbd_release_all()                     抬起全部键（防粘键安全阀）
--   kbd_is_down(keycode)                  查询某键是否按下
--   kbd_is_on()                           宿主键盘接口是否已生效
--   kbd_led()                             主机写回的 LED 位（bit0..4）
--   kbd_state()                           NKRO 位图，4 个 u32（16 字节小端视图）
--   mouse_move(dx, dy[, wheel])           宿主鼠标相对位移（16bit 有符号，正 wheel = 上滚）
--   mouse_btn(button, down)               鼠标按键 0=左 1=右 2=中 3=后退 4=前进
--   mouse_release_all()                   抬起全部鼠标键
--   mouse_is_down(button)                 查询某鼠标键是否按下
--   mouse_is_on()                         宿主鼠标接口是否已生效
--   mouse_state()                         鼠标按键位掩码
--   io_is_on()                            custom IO 通讯接口是否已生效
--
-- ── 输出 · DS5 报告（手柄 / 触摸板 / 六轴）──────────────────────────────
--   这一组**始终可用**，不受 dev_enable 影响。
--   pad_btn(code, pressed)                手柄按键（code = hidkit.BTN_*，0..31；
--                                         随下一个 pad_abs 一并发出）
--   pad_abs(lsx, lsy, rsx, rsy, lt, rt)   手柄轴 / 扳机（±32767 / 0..32767）；
--                                         同时冲刷当前按键掩码
--   pad_touch(x0, y0, x1, y1, down1, down2)
--                                         触摸板 2 触点（坐标 0..4095，DS5 原生 1920×943，不缩放；down = 在位）
--   pad_motion(gx, gy, gz, ax, ay, az)    六轴（±32767）
--   pad_disconnect()                      手柄断连帧（清零全部 pad 状态）
--
-- ── 日志与 hidkit 表 ────────────────────────────────────────────────────
--   print(...) / warn(...)                写入设备日志（网页控制台可见）
--   hidkit.info()                         打印已挂载设备的登记表
--   hidkit.CODE_KEYBOARD / CODE_MOUSE / CODE_GAMEPAD / CODE_SEG_MASK
--                                         事件 code 的段前缀（配 CODE_SEG_MASK 拆包）
--   hidkit.PROTO_NONE / PROTO_KEYBOARD / PROTO_MOUSE / PROTO_GAMEPAD
--                                         设备协议类型
--   hidkit.BTN_A / BTN_B / BTN_X / BTN_Y / BTN_LB / BTN_RB / BTN_LT / BTN_RT /
--     BTN_SELECT / BTN_START / BTN_LS / BTN_RS / BTN_HOME / BTN_MISC
--                                         手柄按键常量（传给 pad_btn / 用于 on_gamepad_btn）
--   hidkit.DPAD_UP / DPAD_DOWN / DPAD_LEFT / DPAD_RIGHT
--                                         十字键（同为 BTN_* 值）
--   hidkit.GYRO_FULL_SCALE_DPS / ACCEL_FULL_SCALE_MG
--                                         六轴量程锚点（把归一化值折算成物理量）
-- ═══════════════════════════════════════════════════════════════════════════

print("lua ready; hidkit.info():")
hidkit.info()

-- ── 键盘 → 宿主 NKRO 键盘 ────────────────────────────────────────────
function on_key(keycode, down)
    kbd_key(keycode, down)
end

-- ── 鼠标 → 宿主 16bit 鼠标 ───────────────────────────────────────────
function on_mouse_btn(button, down)
    mouse_btn(button, down)
end

function on_mouse_move(dx, dy)
    mouse_move(dx, dy)
end

function on_mouse_wheel(wheel)
    mouse_move(0, 0, wheel)   -- wheel 走 mouse_move 第三参（正 = 上滚）
end

-- ── 手柄 → DS5（由脚本 pad_* 驱动输出）──────────────────────────────
function on_gamepad_btn(code, pressed)
    pad_btn(code, pressed)
end

function on_gamepad_abs(lsx, lsy, rsx, rsy, lt, rt)
    pad_abs(lsx, lsy, rsx, rsy, lt, rt)
end

function on_motion(gx, gy, gz, ax, ay, az)
    pad_motion(gx, gy, gz, ax, ay, az)
end

-- ── 触摸板 → DS5（2 触点直通）──────────────────────────────────────
-- meta bit0/bit1 = 对应触点在位；dims = 宽 | (高 << 16)
function on_touchpad(meta, dims, x0, y0, x1, y1)
    local t0 = (meta & 0x01) ~= 0
    local t1 = (meta & 0x02) ~= 0
    pad_touch(x0, y0, x1, y1, t0, t1)
end

function on_dropped(slot, vid, pid)
    print("dropped", slot, string.format("%04x:%04x", vid, pid))
end

-- ── 板载 BOOTSEL 按键 ────────────────────────────────────────────────
-- 板上唯一的实体按键；core0 消抖后投递，按下 true / 抬起 false。
-- 默认无返回值，松开时固件会 dev_enable(-1, -1, 1)：开启 WinUSB，键鼠保持不变。
-- 返回 false、nil 或未定义此回调，也使用相同默认行为。
-- 可在回调中自行 dev_enable，再返回 true 拦截默认行为；按下、松开分别判断返回值。
-- 例如将本回调替换为以下代码，松开时仅开启键盘、保持鼠标不变，并关闭 WinUSB：
-- function on_boot_button(pressed)
--     if not pressed then
--         dev_enable(1, -1, 0)
--     end
--     return true  -- 松开事件也必须返回 true，才能阻止固件默认开启 WinUSB
-- end
function on_boot_button(pressed)
    print("boot button", pressed and "pressed" or "released")
end

-- ── 可选示例（已注释）：脚本启动 5s 后启用键鼠与 WinUSB ───────────────
-- 以下逻辑累计 tick 时间约 5s 后调用 dev_enable(1, 1, 1)，同时开启键盘、鼠标、
-- WinUSB 通讯接口，触发 USB 重枚举；断开等待约 0.5s，期间 DS5 上报暂停。
-- 这是可选的运行期延时示例，从脚本 tick 开始计时，不从主机首次挂载计时。
-- 若希望首次枚举就包含这些接口，直接在脚本顶层调用 dev_enable，无需使用此延时。
-- 如需恢复自动启用，取消下面整个代码块的注释；当前默认由 BOOT 松开时仅启用 WinUSB。
--[[
local boot_us = 0
local km_started = false
local KM_DELAY_US = 5 * 1000 * 1000

function tick(dt_us)
    if km_started then return end
    boot_us = boot_us + dt_us
    if boot_us >= KM_DELAY_US then
        km_started = true
        print("enabling keyboard+mouse+io output...")
        dev_enable(1, 1, 1)
        print("dev_enable requested (re-enumerating, ~0.5s)")
    end
end
--]]
