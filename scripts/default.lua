-- 板载 Lua 脚本 —— 键鼠→键鼠 纯 Lua 管线（预制，无动态加载）
--
-- ⚠ 本版断开 C 固定管线：键盘/鼠标事件被 Lua 拦截（回调返回 true），
--   不再进入 C 默认映射（键鼠→DS5 手柄）；由脚本直接转发到宿主键鼠接口：
--     键盘边沿  → dkb_key()          （NKRO 位图，全键无冲）
--     鼠标按键  → dmo_btn()
--     鼠标位移  → dmo_move()
--     滚轮      → dmo_move(0,0,wheel)
--   手柄输入不在拦截范围（on_gamepad_* 返回 false），仍走 C 直通 → DS5 输出。
--
-- 时序：上电 5s 后 enable_km(true, true) 开启宿主键鼠接口（重枚举 ~0.5s）；
--       重枚举前 dkb_*/dmo_* 为 no-op（接口未启用），期间键鼠输入被吞。
--
-- 事件回调 / 监听声明 / hidkit 表的完整 API 见工程 README。

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

-- ── 键盘 → 宿主 NKRO 键盘（拦截 C 默认管线）────────────────────────────
function on_key(keycode, down)
    dkb_key(keycode, down)
    return true
end

-- ── 鼠标 → 宿主 16bit 鼠标（拦截 C 默认管线）──────────────────────────
function on_mouse_btn(button, down)
    dmo_btn(button, down)
    return true
end

function on_mouse_move(dx, dy)
    dmo_move(dx, dy)
    return true
end

function on_mouse_wheel(wheel)
    dmo_move(0, 0, wheel)   -- wheel 走 dmo_move 第三参（正 = 上滚）
    return true
end

-- ── 手柄 → DS5（不拦截，C 直通照常）──────────────────────────────────
function on_gamepad_btn(code, pressed)
    return false
end

function on_gamepad_abs(lsx, lsy, rsx, rsy, lt, rt)
    return false
end

function on_motion(gx, gy, gz, ax, ay, az)
end

function on_touchpad(meta, dims, x0, y0, x1, y1)
end

function on_dropped(slot, vid, pid)
    print("dropped", slot, string.format("%04x:%04x", vid, pid))
end

-- ── tick：上电 5s 后开启宿主键鼠接口 ─────────────────────────────────
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
