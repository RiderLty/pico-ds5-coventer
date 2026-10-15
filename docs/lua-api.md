# Pico DS5 Lua API

Lua 5.4.6 runs on the device. Callbacks are notifications and do not return a value to intercept an event. Keep top-level work short; a watchdog hook runs during script execution.

## Listening

The firmware listens to **all** events by default — no `enable_listen_*` declarations are needed. Every input event from the connected HID device is forwarded to the corresponding Lua callback if the function is defined.

Callbacks are `on_key(keycode, down)`, `on_mouse_btn(button, down)`, `on_mouse_move(dx, dy)`, `on_mouse_wheel(wheel)`, `on_gamepad_btn(code, pressed)`, `on_gamepad_abs(lsx, lsy, rsx, rsy, lt, rt)`, `on_motion(gx, gy, gz, ax, ay, az)`, `on_touchpad(meta, dims, x0, y0, ...)`, `on_dropped(slot, vid, pid)`, and `tick(dt_us)`. The first `tick` receives zero. Touch arguments contain up to 10 touch points reported by the HID device (meta bit0..9 = point present, dims = width | (height << 16)).

## Output

### Keyboard / Mouse (host interfaces)

`ability_enable(keyboard, mouse, io)` changes output interfaces asynchronously and may briefly re-enumerate USB. `dkb_key(keycode, down)`, `dkb_release_all()`, `dkb_is_down(keycode)`, `dkb_is_on()`, `dkb_led()`, and `dkb_state()` control/query the direct keyboard. `dmo_move(dx, dy[, wheel])`, `dmo_btn(button, down)`, `dmo_release_all()`, `dmo_is_down(button)`, `dmo_is_on()`, and `dmo_state()` control/query the direct mouse. Keyboard and mouse output is no-op until `ability_enable(true, ...)` has been called.

### Gamepad / Touchpad / Motion (DS5 report)

The DS5 DualSense report is driven entirely by Lua via `pad_*` functions. These are always available — they do not depend on `ability_enable`. Data flows through a SPSC queue from core1 (Lua VM) to core0 (DS5 report builder).

| Function | Parameters | Description |
|---|---|---|
| `pad_btn(code, pressed)` | code 0–31, pressed bool | Set/clear a gamepad button in the DS5 button mask. The mask is sent with the next `pad_abs` frame. |
| `pad_abs(lsx, lsy, rsx, rsy, lt, rt)` | each 0–32767 (clamped), lt/rt 0–32767 (scaled to 0–255) | Update stick axes and trigger values in the DS5 report. Also flushes the current button mask. |
| `pad_touch(x0, y0, x1, y1, down1, down2)` | x/y 0–65535, down bool | Set up to 2 touch points on the DS5 touchpad (12-bit packed). Points with `down=false` are marked as not touching. |
| `pad_motion(gx, gy, gz, ax, ay, az)` | each int16 (signed) | Set gyroscope (X/Y/Z) and accelerometer (X/Y/Z) values in the DS5 report. |
| `pad_disconnect()` | — | Clear all pad output (buttons released, axes zeroed, touch/motion reset) and signal device disconnect to the DS5 report. |

`print(...)` and `warn(...)` appear in the device log.

### Constants

`hidkit` exposes constants including `CODE_KEYBOARD`, `CODE_MOUSE`, `CODE_GAMEPAD`, protocol constants, button constants, and sensor scale constants.

## Persistence

The editor keeps a browser draft. **Load to memory** compiles and runs a script without writing flash. **Write to storage** explicitly saves the currently running image to the 512 KiB KV tail partition. A script is limited to 60 KiB UTF-8 bytes. When KV is absent or a saved script fails to load, firmware runs the built-in default script.

Lua has the standard libraries opened by `luaL_openlibs`, but the firmware does not provide a useful filesystem or network API.
