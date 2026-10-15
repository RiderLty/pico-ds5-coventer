-- 板载 Lua 脚本 —— 键鼠→键鼠 纯 Lua 管线（KV 缺失或加载失败时使用的预制脚本）
--
-- 架构：输入与输出完全解耦，中间只有 Lua。C 侧键鼠→手柄的固定映射管线
-- 已移除，hidkit 事件单向通知到脚本，由脚本调用输出函数组装管线：
--     键盘边沿  → dkb_key()          （NKRO 位图，全键无冲）
--     鼠标按键  → dmo_btn()
--     鼠标位移  → dmo_move()
--     滚轮      → dmo_move(0,0,wheel)
--   手柄输入仍走 C 直通 → DS5 输出（pad.* 接口落地后移交 Lua）。
--
-- 时序：上电 5s 后 ability_enable(true, true, true) 开启宿主键鼠接口与
--       custom IO 通讯接口（重枚举 ~0.5s）；
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

-- ── 键盘 → 宿主 NKRO 键盘 ────────────────────────────────────────────
function on_key(keycode, down)
    dkb_key(keycode, down)
end

-- ── 鼠标 → 宿主 16bit 鼠标 ───────────────────────────────────────────
function on_mouse_btn(button, down)
    dmo_btn(button, down)
end

function on_mouse_move(dx, dy)
    dmo_move(dx, dy)
end

function on_mouse_wheel(wheel)
    dmo_move(0, 0, wheel)   -- wheel 走 dmo_move 第三参（正 = 上滚）
end

-- ── 手柄 → DS5（C 直通照常；pad.* 落地后由脚本接管）─────────────────
function on_gamepad_btn(code, pressed)
end

function on_gamepad_abs(lsx, lsy, rsx, rsy, lt, rt)
end

function on_motion(gx, gy, gz, ax, ay, az)
end

function on_touchpad(meta, dims, x0, y0, x1, y1)
end

function on_dropped(slot, vid, pid)
    print("dropped", slot, string.format("%04x:%04x", vid, pid))
end

-- ── tick：上电 5s 后开启宿主键鼠接口与 IO 通讯接口 ───────────────────
local boot_us = 0
local km_started = false
local KM_DELAY_US = 5 * 1000 * 1000

function tick(dt_us)
    if km_started then return end
    boot_us = boot_us + dt_us
    if boot_us >= KM_DELAY_US then
        km_started = true
        print("enabling keyboard+mouse+io output...")
        ability_enable(true, true, true)
        print("ability_enable requested (re-enumerating, ~0.5s)")
    end
end
