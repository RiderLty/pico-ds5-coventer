-- 板载 Lua 脚本 —— 全量直通管线（KV 缺失或加载失败时使用的预制脚本）
--
-- 架构：输入与输出完全解耦，中间只有 Lua。hidkit 事件全量直投回调
-- （无需 enable_listen_* 声明），由脚本调用输出函数组装管线：
--     键盘边沿  → dkb_key()          （NKRO 位图，全键无冲）
--     鼠标按键  → dmo_btn()
--     鼠标位移  → dmo_move()
--     滚轮      → dmo_move(0,0,wheel)
--     手柄按键  → pad_btn()
--     手柄轴    → pad_abs()
--     触摸板    → pad_touch()
--     六轴      → pad_motion()
--
-- 时序：上电 5s 后 ability_enable(true, true, true) 开启宿主键鼠接口与
--       custom IO 通讯接口（重枚举 ~0.5s）；
--       重枚举前 dkb_*/dmo_* 为 no-op（接口未启用），期间键鼠输入被吞。
--       pad_* 始终可用（手柄输出不经过 ability_enable）。
--
-- 事件回调 / pad_* / hidkit 表的完整 API 见工程 README。

print("lua ready; hidkit.info():")
hidkit.info()

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
