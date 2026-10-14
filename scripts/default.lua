-- 板载 Lua 脚本 —— hidkit 事件全量打印 + 5s 后开启键鼠输出（预制，无动态加载）
--
-- 事件回调（全部可选定义，未定义零开销）：
--   on_key(keycode, down)            键盘按键（HID Usage ID；返回 true = 拦截 C 默认路径）
--   on_mouse_btn(button, down)       鼠标按键 0..7（0 左 1 右 2 中；返回 true = 拦截）
--   on_mouse_move(dx, dy)            鼠标位移（返回 true = 清零跳过）
--   on_mouse_wheel(wheel)            滚轮（返回 true = 清零跳过）
--   on_gamepad_btn(code, pressed)    手柄按键 code = hidkit.BTN_*（返回 true = 拦截）
--   on_gamepad_abs(lsx,lsy,rsx,rsy,lt,rt)  手柄轴，±32767 / 扳机 0..32767（返回 true = 拦截）
--   on_motion(gx,gy,gz,ax,ay,az)     六轴，±32767 = ±2000dps / ±4g（无拦截语义）
--   on_touchpad(meta, dims, x0,y0,...,x9,y9)  触摸板快照（无拦截语义）
--   on_dropped(slot, vid, pid)       槽位耗尽丢弃通知（无拦截语义）
--   tick(dt_us)                      周期回调（主循环每轮调用，dt = 距上次的微秒差）
--
-- 监听声明（可运行时调用，未声明的键零开销跳过）：
--   enable_listen_keys(k...)         / disable_listen_keys([k...])
--   enable_listen_mouse_btn(b...)    / disable_listen_mouse_btn([b...])
--   enable_listen_mouse_move()       / disable_listen_mouse_move()
--   enable_listen_mouse_wheel()      / disable_listen_mouse_wheel()
--   enable_listen_gamepad_btn(k...)  / disable_listen_gamepad_btn([k...])
--   enable_listen_gamepad()          / disable_listen_gamepad()
--   enable_listen_motion()           / disable_listen_motion()
--
-- 键鼠输出接口（默认关，重枚举约 0.5s 生效）：
--   enable_km(kbd, mouse)            NKRO 键盘 / 16bit 鼠标输出接口开关
--   dkb_key(code, down)  dkb_release_all()  dkb_led()  dkb_is_on()  dkb_state()  dkb_is_down(code)
--   dmo_move(dx,dy[,wheel])  dmo_btn(b,down)  dmo_release_all()  dmo_is_on()  ...
--
-- hidkit 表：info() 打印当前设备登记表；CODE_*/PROTO_*/BTN_*/DPAD_* 常量；
--   GYRO_FULL_SCALE_DPS=2000 / ACCEL_FULL_SCALE_MG=4000（±32767 满量程锚点）
-- print(...) 走调试串口（GPIO2 @4Mbaud）。
--
-- ⚠ 事件回调为全量监听 + 逐事件打印（不节流），演示/排查用；
--   正式映射请按需收窄 enable_listen_* 范围。

print("lua ready; hidkit.info():")
hidkit.info()

-- 全量监听：256 键位（含修饰键 0xE0..0xE7）/ 8 鼠标键 / 位移+滚轮 /
-- 全部手柄键位（0x00..0x1F）/ 手柄轴 / 六轴
for k = 0, 255 do enable_listen_keys(k) end
for b = 0, 7 do enable_listen_mouse_btn(b) end
enable_listen_mouse_move()
enable_listen_mouse_wheel()
for k = 0, 31 do enable_listen_gamepad_btn(k) end
enable_listen_gamepad()
enable_listen_motion()

function on_key(keycode, down)
    print("key", string.format("0x%02X", keycode), down and "down" or "up")
    return false   -- 不拦截，C 默认映射照常
end

function on_mouse_btn(button, down)
    print("ms btn", button, down and "down" or "up")
    return false
end

function on_mouse_move(dx, dy)
    print("ms move", dx, dy)
    return false
end

function on_mouse_wheel(wheel)
    print("ms wheel", wheel)
    return false
end

function on_gamepad_btn(code, pressed)
    print("pad btn", code, pressed and "down" or "up")
    return false
end

function on_gamepad_abs(lsx, lsy, rsx, rsy, lt, rt)
    print("pad abs", string.format("ls(%d,%d) rs(%d,%d) lt=%d rt=%d", lsx, lsy, rsx, rsy, lt, rt))
    return false
end

function on_motion(gx, gy, gz, ax, ay, az)
    print("motion", string.format("g(%d,%d,%d) a(%d,%d,%d)", gx, gy, gz, ax, ay, az))
end

function on_touchpad(meta, dims, x0, y0, x1, y1)
    local contacts = meta & 0x3FF
    print("touchpad", string.format("contacts=0x%03X dims=%dx%d p0=(%d,%d) p1=(%d,%d)",
          contacts, dims & 0xFFFF, dims >> 16, x0, y0, x1, y1))
end

function on_dropped(slot, vid, pid)
    print("dropped", slot, string.format("%04x:%04x", vid, pid))
end

-- tick：主循环每轮调用（不节流），dt = 距上次的微秒差。
-- 示例：上电 5 秒后开启 NKRO 键盘 + 16bit 鼠标输出接口
local boot_us = 0
local km_started = false
local KM_DELAY_US = 5 * 1000 * 1000

function tick(dt_us)
    if km_started then return end
    boot_us = boot_us + dt_us
    if boot_us >= KM_DELAY_US then
        km_started = true
        print("enabling keyboard+mouse output...")
        enable_km(true, true)
        print("enable_km requested (re-enumerating, ~0.5s)")
    end
end
