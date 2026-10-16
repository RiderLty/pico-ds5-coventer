-- ═══════════════════════════════════════════════════════════════════════════
-- Pico DS5 转换器 · 板载预制脚本
--   KV 里没有已存脚本、或已存脚本加载失败时，固件运行这一份。
--   这一份是**全量直通管线**：把输入原样转给输出，可作为自己写脚本的起点。
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
--   on_touchpad(meta, dims, x0, y0, ...)  触摸板快照，最多 10 触点：
--                                         meta 的 bit0..9 = 该触点在位，
--                                         dims = 宽 | (高 << 16)，其后是 10 组 x,y
--   on_boot_button(pressed)               板载 BOOTSEL 按键（设备侧 10ms 消抖）
--   on_dropped(slot, vid, pid)            槽位耗尽丢弃设备时的一次性通知
--   tick(dt_us)                           周期回调（dt = 距上次的微秒差，首个为 0）
--
-- ── 输出 · 宿主键盘 / 鼠标 / IO 通讯接口 ─────────────────────────────────
--   这一组受 dev_enable 控制；接口没开时调用是 no-op。
--   dev_enable(kbd, mouse, io)            开关对外接口。三个参数各为三态：
--                                         -1 = 不修改，0 = 关闭，1 = 开启
--                                         （省略的参数按 -1）。
--                                         异步生效，需 USB 重枚举（~0.5s，
--                                         期间 DS5 从主机短暂消失）。
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
--                                         触摸板 2 触点（12-bit 坐标，down = 在位）
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
-- meta bit0..9 = 触点在位；dims = 宽 | (高 << 16)
-- 取前 2 个在位触点的坐标；无触点时发全零
function on_touchpad(meta, dims, x0, y0, x1, y1, x2, y2, x3, y3, x4, y4,
                     x5, y5, x6, y6, x7, y7, x8, y8, x9, y9)
    local t0 = (meta & 0x01) ~= 0
    local t1 = (meta & 0x02) ~= 0
    pad_touch(x0, y0, x1, y1, t0, t1)
end

function on_dropped(slot, vid, pid)
    print("dropped", slot, string.format("%04x:%04x", vid, pid))
end

-- ── 板载 BOOTSEL 按键 ────────────────────────────────────────────────
-- 板上唯一的实体按键；core0 消抖后投递，按下 true / 抬起 false。
-- 返回 true 可拦截释放时默认开启 WinUSB；默认无返回值，所以释放时开启 WinUSB。
function on_boot_button(pressed)
    print("boot button", pressed and "pressed" or "released")
end

-- ── tick：上电 5s 后开启宿主键鼠接口与 IO 通讯接口 ───────────────────
-- 延后 5s 是为了避开开机枚举窗口，重枚举才不会和主机抢时序。
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
