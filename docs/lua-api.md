# Pico DS5 Lua API

Lua 5.4.6 runs on the device. Input callbacks are notifications; `on_boot_button` is the exception and can return a truthy value to intercept its default release behavior. Keep top-level work short; a watchdog hook runs during script execution.

## Listening

The firmware listens to **all** events by default: every input event from the connected HID device is forwarded to the corresponding Lua callback if that function is defined. There is no registration step — defining the callback is all it takes.

Callbacks are `on_key(keycode, down)`, `on_mouse_btn(button, down)`, `on_mouse_move(dx, dy)`, `on_mouse_wheel(wheel)`, `on_gamepad_btn(code, pressed)`, `on_gamepad_abs(lsx, lsy, rsx, rsy, lt, rt)`, `on_motion(gx, gy, gz, ax, ay, az)`, `on_touchpad(meta, dims, x0, y0, ...)`, `on_dropped(slot, vid, pid)`, `on_boot_button(pressed)`, and `tick(dt_us)`. The first `tick` receives zero. Touch arguments contain up to 10 touch points reported by the HID device (meta bit0..9 = point present, dims = width | (height << 16)).

`on_boot_button(pressed)` fires for the board's physical BOOTSEL button (`pressed` is `true` on press, `false` on release). It is debounced on the device (10 ms polling, three consistent samples) so presses shorter than ~30 ms may be missed. Because reading the pin briefly detaches the QSPI flash, the other core is paused for a few microseconds per poll — this is harmless but means the button is not suitable as a high-rate input.

The callback runs before the default action. Return `true` to handle the event yourself. If it returns `false`, `nil`, no value, or is not defined, releasing the button enables the WinUSB interface while leaving the keyboard and mouse settings unchanged.

## Output

Output functions are plain globals prefixed by their target, mirroring the `on_*` input callbacks:

| Prefix | Target |
|---|---|
| `kbd_` | host keyboard interface |
| `mouse_` | host mouse interface |
| `io_` | custom IO (WinUSB) interface |
| `pad_` | DS5 report: gamepad buttons/axes, touchpad, motion |
| `dev_` | device-wide switches |

### Host keyboard / mouse / IO

`dev_enable(keyboard, mouse, io)` changes which host-facing interfaces are exposed. Each argument is tri-state: `-1` leaves that interface unchanged, `0` turns it off, `1` turns it on; omitted arguments default to `-1`. It is asynchronous and may briefly re-enumerate USB (the DS5 disappears from the host for ~0.5 s).

| Function | Description |
|---|---|
| `kbd_key(keycode, down)` | Host keyboard key. Usage `0x04..0x73`, modifiers `0xe0..0xe7`; NKRO bitmap, no rollover limit. |
| `kbd_release_all()` | Release every held key (anti-stuck safety valve). |
| `kbd_is_down(keycode)` | Whether a key is currently held. |
| `kbd_is_on()` | Whether the host keyboard interface is live. |
| `kbd_led()` | LED state written back by the host (bit 0–4). |
| `kbd_state()` | Four u32 words: the 16-byte NKRO bitmap, little-endian. |
| `mouse_move(dx, dy[, wheel])` | Relative movement, 16-bit signed. Positive `dx` = right, positive `dy` = down, positive `wheel` = scroll up. |
| `mouse_btn(button, down)` | `0`=left, `1`=right, `2`=middle, `3`=back, `4`=forward. |
| `mouse_release_all()` | Release every held mouse button. |
| `mouse_is_down(button)` | Whether a mouse button is held. |
| `mouse_is_on()` | Whether the host mouse interface is live. |
| `mouse_state()` | Button bitmask. |
| `io_is_on()` | Whether the custom IO interface is live. |

Keyboard and mouse output is a no-op until `dev_enable(1, ...)` has been called.

### Gamepad / Touchpad / Motion (DS5 report)

The DS5 DualSense report is driven entirely by Lua via `pad_*` functions. These are always available — they do not depend on `dev_enable`. Data flows through a SPSC queue from core1 (Lua VM) to core0 (DS5 report builder).

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
