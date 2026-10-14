# pico-ds5-coventer

USB 输入 → Sony DualSense (DS5) 转换器，运行于 Raspberry Pi Pico 2 (RP2350)。
输入侧由 [hidkit](https://github.com/RiderLty/hidkit) 解析键盘、鼠标、HID 手柄，
另接 XInput 类驱动与 Switch Pro USB 握手库；设备端采用
[awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) 的 DS5/UAC1 描述符。
键鼠输出与描述符运行期开关参考
[pico-hid-mapper](https://github.com/RiderLty/pico-hid-mapper)。

```text
键鼠 / Xbox 手柄 / Switch Pro / HID 手柄（可经 USB Hub）
                      │
       PIO-USB Host，core1，GPIO12/13
          ├─ 键鼠事件 → Lua → dkb_* / dmo_* → 跨核队列
          └─ 手柄状态 → 跨核队列
                      │
             原生 USB Device，core0
          ├─ DS5：Report ID 0x01，63B 数据 + 1B ID，EP 0x84
          ├─ 可选 16bit 鼠标：EP 0x86
          └─ 可选 NKRO 键盘：EP 0x87
                      │
                   PC / 主机
```

## 功能与默认行为

- 手柄状态经 C 管线直通 DS5：双摇杆、模拟扳机、十字键和数字按键；拔出后回到空闲态。
  XInput 支持 Xbox 360/One/Series 有线输入，Switch Pro 经 USB 握手与 SPI 校准读取。
- 键鼠输入经 Lua 转发到宿主键鼠接口。C 侧固定 FPS 键鼠→手柄映射已移除，
  Lua 回调是事件通知，返回值不拦截或改变 C 手柄直通路径。
- 上电时键鼠输出接口均关闭，配置为 DS5 + UAC1 音频（227 字节、4 个接口）。
  默认 `scripts/default.lua` 累计 tick 时间约 5 秒后调用 `enable_km(true, true)`，
  经 USB 重枚举追加鼠标和键盘（277 字节、6 个接口）。断开等待为 500ms，
  主机重新枚举还需要额外时间，期间 DS5 上报暂停。
- 若主机连接晚于脚本的 5 秒延时，首次看到的配置可能已经包含键鼠。
  当前主线没有“开机预装键鼠”或“首次挂载后等待 15 秒”的验证开关。
- DS5 身份始终为 `054c:0ce6`，运行期切换不会改 VID/PID。
- UAC1 扬声器/麦克风接口供主机识别和开关流，无实际音频输出。
  Feature 报告按尺寸返回零数据，震动/灯条等输出报告接收后忽略。
- 板载 Lua 5.4.6 使用预制脚本，无运行期脚本加载；`print` 走 UART，
  带约 10ms 步数看门狗。修改 `scripts/default.lua` 后重新构建，CMake 自动更新嵌入头文件。

手柄按键映射：A/B/X/Y → ✕/○/□/△，LB/RB → L1/R1，LT/RT → L2/R2，
Select/Create → Create，Start → Options，LS/RS → L3/R3，Home → PS，
Misc → 触摸板按键，十字键直通。

## 键鼠热重载

Lua `enable_km(kbd, mouse)` 异步提交请求，core0 在设备断开窗口内组装配置描述符、
更新 HID 实例和队列状态，再连接等待主机枚举。相同状态的请求不会再次断开。

| 接口 | USB 接口号 | TinyUSB HID 实例 | 端点 |
|---|---|---|---|
| UAC1 控制 / 扬声器 / 麦克风 | 0 / 1 / 2 | — | OUT `0x01` / IN `0x82` |
| DS5 | 3 | 0 | IN `0x84` / OUT `0x03` |
| 16bit 鼠标（可选） | 4 | 1 | IN `0x86`，最大包 8B |
| NKRO 键盘（可选） | 有鼠标时 5，否则 4 | 有鼠标时 2，否则 1 | IN `0x87`，最大包 16B |

鼠标为无 Report ID 的 6B 相对报告；键盘描述符含 ID 1 的 6KRO 报告和 ID 2 的
112 键位图报告，实际发送 NKRO。两者接口协议均为 None，不提供 USB HID Boot 协议。
`CFG_TUD_HID=3` 预留 DS5、鼠标、键盘的全部实例。

RP2350 的队列临界区必须通过 SDK 初始化，`usb_km_init()` 在 core1 启动前完成。
根因、切换顺序、日志和硬件验证记录见 [USB 描述符热重载说明](docs/usb-hot-reload.md)。

## Lua 接线

脚本通过 `enable_listen_*` 声明监听，未监听的事件跳过 VM。
默认脚本将键盘、鼠标按键、位移和滚轮原样转发。

| 监听函数 | 事件回调 |
|---|---|
| `enable_listen_keys(keycode, ...)` | `on_key(keycode, down)` |
| `enable_listen_mouse_btn(button, ...)` | `on_mouse_btn(button, down)` |
| `enable_listen_mouse_move()` | `on_mouse_move(dx, dy)` |
| `enable_listen_mouse_wheel()` | `on_mouse_wheel(wheel)` |
| `enable_listen_gamepad_btn(code, ...)` | `on_gamepad_btn(code, pressed)` |
| `enable_listen_gamepad()` | `on_gamepad_abs(lsx, lsy, rsx, rsy, lt, rt)` |
| `enable_listen_motion()` | `on_motion(gx, gy, gz, ax, ay, az)` |

同名 `disable_listen_*` 关闭监听；keys/mouse_btn/gamepad_btn 无参数时清空全部。
另有 `tick(dt_us)`、`on_touchpad(meta, dims, x0, y0, x1, y1)`、
`on_dropped(slot, vid, pid)`。`hidkit.info()` 输出登记表，`hidkit.BTN_*` 等提供语义常量。

| 输出函数 | 用途 |
|---|---|
| `enable_km(kbd, mouse)` | 开关接口；参数顺序是键盘、鼠标 |
| `dkb_key(keycode, down)` | 键盘 Usage `0x04..0x73`，修饰键 `0xe0..0xe7` |
| `dkb_release_all()` / `dkb_is_down(keycode)` | 抬起全部键 / 查询按键 |
| `dkb_is_on()` / `dkb_led()` / `dkb_state()` | 生效开关 / LED 位 / 状态（4 个 u32） |
| `dmo_move(dx, dy[, wheel])` | 16bit 相对位移；正 dx=右、正 dy=下、正 wheel=上滚 |
| `dmo_btn(button, down)` | 0=左、1=右、2=中、3=后退、4=前进 |
| `dmo_release_all()` / `dmo_is_down(button)` | 抬起全部鼠标键 / 查询按键 |
| `dmo_is_on()` / `dmo_state()` | 生效开关 / 按键位掩码 |

接口关闭时输出调用不转发。`dkb_is_on()` / `dmo_is_on()` 表示设备侧生效开关，
不代表主机已完成枚举。键盘队列保存绝对状态、满时丢旧保新；鼠标队列合并增量，
端点忙时重试，位移超过报告范围时会限幅。

## 构建

需要 CMake、Python 3、Arm GNU 工具链和 Git。依赖由子模块和 vendored 源码管理：

| 依赖 | 版本 | 说明 |
|---|---|---|
| `lib/pico-sdk` | 2.3.0 | 内嵌 TinyUSB 切到 `2d56dc53` 以支持 UAC1 |
| `lib/pico_pio_usb` | `675543bc` | TinyUSB 官方配对版本（0.7.2） |
| `lib/hidkit` | v0.3.1 | HID/XInput 语义解析 |
| `lib/hidkit-tusb-xinput` | `4fffdf3` | XInput 类驱动 |
| `lib/hidkit-tusb-switch-pro` | `db5dc8a` | Switch Pro USB 握手库 |
| `lib/lua-5.4.6` | vendored | Lua 引擎 |

```bash
./scripts/setup-sdk.sh
cmake -S . -B build -DTUSB_DEBUG_LEVEL=0
cmake --build build -j8
# 产物：build/src/pico-ds5-coventer.uf2
```

SDK 路径已在顶层 CMake 指定，无需设置环境变量。setup 脚本会切换 SDK 内嵌的
TinyUSB commit，因此 SDK 子模块显示 TinyUSB 指针差异是已有构建安排；无需修改依赖源码。
Host 引脚在 `src/main.c` 的 `pio_cfg.pin_dp` 设置。

### 串口与 TinyUSB 调试

UART0：TX=GPIO2、RX=GPIO3、默认 **4000000 8N1**，串口适配器与固件须使用相同速率。
默认保留启动、设备生命周期与 KM 切换日志，TinyUSB debug 关闭。需要协议日志时：

```bash
cmake -S . -B build-debug -DTUSB_DEBUG_LEVEL=2
cmake --build build-debug -j8
# 适配器不支持 4Mbaud 时，配置时可加 -DDEBUG_BAUDRATE=115200
```

`TUSB_DEBUG_LEVEL` 范围 0..3。级别 2 会打印枚举、控制请求和高频端点传输，
日志带 `[TUSB]` 前缀；阻塞 UART 输出会影响时序，定位完成后重新配置为 0。
CMake 会缓存调试级别，复用已有构建目录时请显式传入所需级别。

### PIO-USB 版本说明

本仓库使用配对的 PIO-USB 0.7.2；设备端为原生 USB，不采用 mapper 旧版 PIO-USB
的 device SE0 补丁。当前不写运行期 flash；将来若加入 KV 存储，需要另行评估
写 flash 导致 Host 静默的问题。TinyUSB hub 驱动韧性修复已包含于固定 commit。

## 烧录与验证

1. 按住 Pico 2 BOOTSEL 插入电脑，拷贝 `build/src/pico-ds5-coventer.uf2`。
2. 电脑识别 `DualSense Wireless Controller`，默认脚本约 5 秒后重枚举并出现键鼠接口。
3. 实体键鼠/手柄接 GPIO12(D+)/GPIO13(D-) 对应的 Host 端口，可经 USB Hub。
4. 用 gamepad-tester.com 或 Steam 手柄设置观察手柄输入，检查键盘、鼠标和滚轮转发。
   UART 日志中，热重载后 `MOUNTED ... hid(kbd=1 mouse=1)` 表示新配置完成挂载。

## 已知限制

- 震动、灯条、自适应扳机输出被忽略；音频接口无实际声音。
- 六轴与触摸数据使用模板，不转发真实运动/触摸；键鼠不映射为 DS5 输入。
- 多手柄最终汇入同一份 DS5 状态，主要面向单手柄场景。
- 当前键盘转发范围为 112 个常规键与 8 个修饰键，不包括消费者控制等其他 Usage Page。
- 热重载的硬件验证范围见专门文档，尚未覆盖所有系统与反复切换组合。
