# pico-ds5-coventer

USB / 蓝牙输入 → Sony DualSense (DS5) 转换器，运行于 Raspberry Pi Pico 2 / Pico 2 W / Waveshare RP2350B-Plus-W (RP2350)。

[网页工作台](https://riderlty.github.io/pico-ds5-coventer/) · [下载 USB / 蓝牙固件](https://github.com/RiderLty/pico-ds5-coventer/releases/latest) · [蓝牙配对与构建说明](docs/bluetooth.md)
输入侧由 [hidkit](https://github.com/RiderLty/hidkit) 解析键盘、鼠标、HID 手柄，
另接 XInput 类驱动与 Switch Pro USB 握手库；设备端采用
[awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) 的 DS5/UAC1 描述符。
键鼠输出与描述符运行期开关参考
[pico-hid-mapper](https://github.com/RiderLty/pico-hid-mapper)。

```text
键鼠 / Xbox 手柄 / Switch Pro / HID 手柄（可经 USB Hub）
                      │
       PIO-USB Host，core1，GPIO12/13
          ├─ 键鼠事件 → Lua → kbd_* / mouse_* → 跨核队列
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

- 可选蓝牙变体：`./build.sh --pico2w`（官方 Pico 2 W）或 `./build.sh --waveshare`（微雪 RP2350B-Plus-W），使用板载经典蓝牙连接 DualSense / Edge，
  替换 PIO-USB Host。PS + Create 首次配对，PS 重连；密钥与 Lua/AI KV 分区隔离。
  详见 [蓝牙说明与验证范围](docs/bluetooth.md)。

- 手柄状态经 hidkit → Lua → DS5：双摇杆、模拟扳机、十字键和数字按键；拔出后回到空闲态。
  XInput 支持 Xbox 360/One/Series 有线输入，Switch Pro 经 USB 握手与 SPI 校准读取。
- 键鼠输入经 Lua 转发到宿主键鼠接口。C 侧固定 FPS 键鼠→手柄映射已移除，
  Lua 回调负责映射，输出需调用 `pad_*`；除 `on_boot_button` 外回调返回值均忽略。
- 使用默认脚本上电时，额外接口均关闭，配置为 DS5 + UAC1 音频（227 字节、4 个接口）。
  默认 `scripts/default.lua` 已将 5 秒后自动启用接口的逻辑整段注释保留，不会自动追加键鼠或 WinUSB。
  该示例原本累计脚本 tick 时间约 5 秒后调用 `dev_enable(1, 1, 1)`，同时启用三个接口；
  如需延时启用，可取消该代码块的注释。计时从脚本 tick 开始，不从主机首次挂载开始。
  若希望插上主机即包含指定接口，在脚本顶层写 `dev_enable(1, 1, 1)` 并保存到设备；
  下次上电先执行脚本，再以最终配置首次枚举，无需先断开重连。
- 运行中按下再松开板载 BOOT（BOOTSEL）即可开启 WinUSB，键盘、鼠标设置保持不变。
  Lua 可通过 `on_boot_button(pressed)` 自行设置接口并返回 `true` 拦截默认行为，
  见 [自定义 BOOT 行为](docs/lua-scripts.md#boot-按键与接口启用)。
  接口布局变化会触发 USB 重枚举，断开等待为 500ms，主机重新枚举还需要额外时间，期间 DS5 上报暂停。
- DS5 身份始终为 `054c:0ce6`，运行期切换不会改 VID/PID。
- UAC1 扬声器/麦克风接口供主机识别和开关流，无实际音频输出。
  Feature 报告按尺寸返回零数据，震动/灯条等输出报告接收后忽略。
- 板载 Lua 5.4.6 支持运行期加载，源码上限 60 KiB；编辑、上传运行只改 RAM，明确保存才写
  Flash 尾部 512 KiB KV。启动优先加载 KV，缺失或加载失败恢复预制 `scripts/default.lua`。
  `print` 经 DEBUG → PRINT 通道输出，带约 10ms 执行看门狗。预制脚本修改后由 CMake 自动嵌入。
  最简编辑器已集成到验证页；API、协议与测试见 [Lua 脚本存储说明](docs/lua-scripts.md)。

手柄按键映射：A/B/X/Y → ✕/○/□/△，LB/RB → L1/R1，LT/RT → L2/R2，
Select/Create → Create，Start → Options，LS/RS → L3/R3，Home → PS，
Misc → 触摸板按键，十字键直通。

## 键鼠热重载

Lua `dev_enable(kbd, mouse, io)` 异步提交请求。上电时先执行 KV 脚本（失败则回退预制脚本），
待顶层代码全部完成后，core0 应用最终配置并启动 USB，首次枚举就包含指定接口。
多次顶层调用合并生效；启动脚本报错时清除其部分请求，再运行预制脚本。
顶层执行期间 `*_is_on()` 仍反映已应用状态，不应循环等待它变化。

USB 启动后，`tick`、按键回调或网页“上传运行”等产生的配置变更仍通过断开窗口更新
描述符、HID 实例和队列，再连接等待主机枚举。相同状态的请求不会再次断开。
首次枚举配置以**保存到设备、下次重新上电时执行的脚本**为准，上传运行本身不写 KV。

网页首次手动授权 WinUSB 后，会在页面打开、拔插和热重载时自动找回已授权的同一设备；
手动断开或选择 UART 会暂停自动连接。匹配会检查厂商类 bulk 接口，排除真正的 HID DS5。
启用 WinUSB 的固件配置提供稳定板卡序列号，供浏览器保存跨拔插权限；旧固件无序列号时
拔出会失去临时授权。详见 [自动重连与持久授权](docs/winusb-windows.md#网页自动重连与持久授权)。

| 接口 | USB 接口号 | TinyUSB HID 实例 | 端点 |
|---|---|---|---|
| UAC1 控制 / 扬声器 / 麦克风 | 0 / 1 / 2 | — | OUT `0x01` / IN `0x82` |
| DS5 | 3 | 0 | IN `0x84` / OUT `0x03` |
| 16bit 鼠标（可选） | 4 | 1 | IN `0x86`，最大包 8B |
| NKRO 键盘（可选） | 有鼠标时 5，否则 4 | 有鼠标时 2，否则 1 | IN `0x87`，最大包 16B |
| WinUSB IO 通讯（可选） | 4 + 鼠标有无 + 键盘有无 | —（vendor 类） | IN `0x88` / OUT `0x08`，最大包 64B |

鼠标为无 Report ID 的 6B 相对报告；键盘描述符含 ID 1 的 6KRO 报告和 ID 2 的
112 键位图报告，实际发送 NKRO。两者接口协议均为 None，不提供 USB HID Boot 协议。
WinUSB 通讯接口为 class 0xFF 单接口 + bulk 端点对（MS OS 1.0 描述符绑定
winusb.sys，无 IAD），跑自定义分块协议 v1（INFO 查询 / PRINT 日志流 /
RESET / TEST 压测 / SCRIPT 脚本管理，varint 长度 + 分块重组）；Lua `print` 与全部设备日志
统一走 `DEBUG()` → 协议 PRINT 通道（4KB 跨核环）。协议命令层与传输解耦：
WinUSB 与调试串口（UART0 4Mbaud，SLIP+CRC16 帧，DMA 环形接收）两个平级
传输，命令层无感。验证页见 `tools/webhid-test.html`；网页端可复用 API 见 [PicoDevice 文档](docs/pico-device.md)
（WebUSB / Web Serial 双入口）。键鼠为单接口 HID，IO 为单接口 vendor，均**无 IAD**——带 IAD 的
USB 类（CDC/NCM 等）会令 Windows usbccgp 放弃 legacy 音频分组、拆散 DS5 音频
接口（详见 docs/usb-hot-reload.md 与 exp/cdc-serial 分支的探索记录）。
`CFG_TUD_HID=3` 预留 DS5、鼠标、键盘实例，IO 通讯经 `CFG_TUD_VENDOR=1` 挂载。

Windows OS 描述符从首次枚举起始终可读；IO 关闭时 Compatible ID 表为空，
启用时返回 `WINUSB` 与 `DeviceInterfaceGUIDs` 属性。8 种布局分别使用
`bcdDevice=0x0120..0x0127`（仅开 IO 为 `0x0124`，全开为 `0x0127`），避免跨布局复用 OS 描述符缓存。
代码 28 的根因、刷入后验证与主机侧测试见 [Windows WinUSB 排查](docs/winusb-windows.md)。
串口压测失败的代码原因、诊断计数和复测步骤见 [串口压测排查](docs/uart-stress.md)。

RP2350 的队列临界区必须通过 SDK 初始化，`usb_km_init()` 在 core1 启动前完成。
根因、切换顺序、日志和硬件验证记录见 [USB 描述符热重载说明](docs/usb-hot-reload.md)。

## Lua 接线

hidkit 事件全量直投 Lua 回调：脚本里定义哪个回调就收哪个事件，无需任何开关或声明。
默认脚本将键盘、鼠标、手柄、触摸和六轴原样转发；键鼠输出需先用 `dev_enable` 启用对应接口。

| 事件回调 | 说明 |
|---|---|
| `on_key(keycode, down)` | 键盘按键（HID Usage ID） |
| `on_mouse_btn(button, down)` | 鼠标按键（0..7） |
| `on_mouse_move(dx, dy)` | 鼠标位移 |
| `on_mouse_wheel(wheel)` | 鼠标滚轮 |
| `on_gamepad_btn(code, pressed)` | 手柄按键（hidkit BTN_* 0..31） |
| `on_gamepad_abs(lsx, lsy, rsx, rsy, lt, rt)` | 手柄轴（±32767）/扳机（0..32767） |
| `on_motion(gx, gy, gz, ax, ay, az)` | 六轴（陀螺 ±2000dps / 加速度 ±4g，归一化 ±32767） |
| `on_touchpad(meta, dims, x0, y0, x1, y1)` | DS5 触摸板快照（2 个触点，meta bit0/bit1 触点在位） |
| `on_boot_button(pressed)` | 板载 BOOTSEL 按键（按下 true / 抬起 false，设备侧消抖）；返回 true 拦截默认行为，否则释放时开启 WinUSB，键盘和鼠标保持不变 |
| `on_dropped(slot, vid, pid)` | 设备被丢弃通知 |
| `tick(dt_us)` | 周期回调，默认至少间隔 1000us（最高 1000Hz）；dt 为实际回调间隔，首个为 0 |

`hidkit.info()` 输出登记表，`hidkit.BTN_*` 等提供语义常量。

输出函数一律以**目标**为前缀，与输入侧的 `on_*` 回调形成对照：
`kbd_` 宿主键盘 / `mouse_` 宿主鼠标 / `io_` custom IO 通讯接口 /
`pad_` DS5 报告（手柄·触摸板·六轴）/ `dev_` 设备级开关。

| 输出函数 | 用途 |
|---|---|
| `dev_enable(kbd, mouse, io)` | 开关接口；顺序为键盘、鼠标、IO 通讯，各参数三态：`-1` 保持 / `0` 关 / `1` 开（省略按 `-1`） |
| `kbd_key(keycode, down)` | 键盘 Usage `0x04..0x73`，修饰键 `0xe0..0xe7` |
| `kbd_release_all()` / `kbd_is_down(keycode)` | 抬起全部键 / 查询按键 |
| `kbd_is_on()` / `kbd_led()` / `kbd_state()` | 生效开关 / LED 位 / 状态（4 个 u32） |
| `mouse_move(dx, dy[, wheel])` | 16bit 相对位移；正 dx=右、正 dy=下、正 wheel=上滚 |
| `mouse_btn(button, down)` | 0=左、1=右、2=中、3=后退、4=前进 |
| `mouse_release_all()` / `mouse_is_down(button)` | 抬起全部鼠标键 / 查询按键 |
| `mouse_is_on()` / `mouse_state()` | 生效开关 / 按键位掩码 |
| `io_is_on()` | custom IO 通讯接口是否启用 |
| `pad_btn(code, pressed)` | 手柄按键状态（hidkit BTN_* 0..31） |
| `pad_abs(lsx, lsy, rsx, rsy, lt, rt)` | 手柄轴/扳机（±32767 / 0..32767） |
| `pad_touch(x0, y0, x1, y1, down1, down2)` | 触摸板 2 触点（原生坐标，不缩放，0..4095 钳位） |
| `pad_motion(gx, gy, gz, ax, ay, az)` | 六轴（±32767） |
| `pad_disconnect()` | 手柄断连帧（清零状态） |

接口关闭时输出调用不转发。`kbd_is_on()` / `mouse_is_on()` 表示设备侧生效开关，
不代表主机已完成枚举。键盘队列保存绝对状态、满时丢旧保新；鼠标队列合并增量，
端点忙时重试，位移超过报告范围时会限幅。

## 构建

需要 CMake、Python 3、Arm GNU 工具链和 Git。依赖由子模块和 vendored 源码管理：

| 依赖 | 版本 | 说明 |
|---|---|---|
| `lib/pico-sdk` | 2.3.0 | 内嵌 TinyUSB 切到 `2d56dc53` 以支持 UAC1 |
| `lib/pico_pio_usb` | `9510f795` + 本地补丁 | 保留 Hub 热插拔可靠性的 host 基线 |
| `lib/hidkit` | v0.3.1 | HID/XInput 语义解析 |
| `lib/hidkit-tusb-xinput` | `4fffdf3` | XInput 类驱动 |
| `lib/hidkit-tusb-switch-pro` | `db5dc8a` | Switch Pro USB 握手库 |
| `lib/lua-5.4.6` | vendored | Lua 引擎 |
| `lib/kvstore` | vendored，自 mapper 移植 | logkvs + Flash blockdevice，尾部 512 KiB |

```bash
./build.sh
# 产物：build/src/pico-ds5-coventer.uf2

# 显式 USB 版，与不加参数相同
./build.sh --usb

# 官方 Pico 2 W 蓝牙输入版
./build.sh --pico2w
# 产物：build-bluetooth-pico2w/src/pico-ds5-coventer.uf2

# Waveshare RP2350B-Plus-W 蓝牙输入版
./build.sh --waveshare
# 产物：build-bluetooth-waveshare/src/pico-ds5-coventer.uf2

# 已准备好依赖时跳过 setup（支持离线增量编译）
./build.sh --skip-setup
```

`build.sh` 默认编译 Pico 2 **Release** 固件，自动初始化固定版本的子模块、切换
TinyUSB 并应用 PIO-USB 补丁。通过 `BUILD_TYPE`、`JOBS`、`TUSB_DEBUG_LEVEL`、
`DEBUG_BAUDRATE` 环境变量调整构建；默认日志级别 0、UART 4000000。
使用 `--build-dir` 指定独立目录，额外 CMake 参数放在 `--` 后；`./build.sh --help`
查看用法。脚本每次显式传入上述配置，避免复用上次调试构建的缓存值。

SDK 路径已在顶层 CMake 指定，无需设置环境变量。setup 脚本会切换 SDK 内嵌的
TinyUSB commit，因此 SDK 子模块显示 TinyUSB 指针差异是已有构建安排；无需修改依赖源码。
Host 引脚在 `src/main.c` 的 `pio_cfg.pin_dp` 设置。

### 自动发布

推送 `v*` tag 到 GitHub 后，`.github/workflows/release.yml` 调用同一个 `build.sh`
分别编译 USB / Pico 2、蓝牙 / Pico 2 W、蓝牙 / Waveshare RP2350B-Plus-W 固件；三个任务都成功后创建对应 tag 的
GitHub Release 并自动生成发布说明。附件包含带版本号与 `usb-pico2` / `bluetooth-pico2w`
/ `bluetooth-waveshare-rp2350b-plus-w` 后缀的三套 `.uf2`，不再发布其他格式的附件。

```bash
git tag v1.0.0
git push origin v1.0.0
```

如果标签推送未触发构建，或需要重新发布已有版本，可在 Actions → Build and Release →
Run workflow 中选择 `main`，填写已有 tag；也可以使用 GitHub CLI：

```bash
gh workflow run release.yml --ref main -f tag=v1.0.0
```

手动发布会检出指定 tag 的源码，构建与发布均使用该版本。

发布使用 GitHub Actions 自带的 `GITHUB_TOKEN`，无需额外配置发布密钥。

### 串口与 TinyUSB 调试

UART0：TX=GPIO2、RX=GPIO3、默认 **4000000 8N1**，串口适配器与固件须使用相同速率。
UART 线上只有协议帧（SLIP+CRC16），设备日志经 `DEBUG()` → PRINT 通道在网页
控制台查看，普通串口终端看到的是二进制帧字节。需要 TinyUSB 协议日志时：

```bash
cmake -S . -B build-debug -DTUSB_DEBUG_LEVEL=2
cmake --build build-debug -j8
# 适配器不支持 4Mbaud 时，配置时可加 -DDEBUG_BAUDRATE=115200
```

`TUSB_DEBUG_LEVEL` 范围 0..3。级别 2 会打印枚举、控制请求和高频端点传输，
日志带 `[TUSB]` 前缀；阻塞 UART 输出会影响时序，定位完成后重新配置为 0。
CMake 会缓存调试级别，复用已有构建目录时请显式传入所需级别。

### PIO-USB 版本说明

PIO-USB 固定在 `pico-hid-mapper` 验证过 Hub 热插拔的旧版基线 `9510f795`，并通过
`patches/pio_usb/` 应用 Pico SDK 2 兼容、device SE0 超时、TinyUSB endpoint API
兼容和 Host 总线静默自愈补丁。运行 `./scripts/setup-sdk.sh` 会自动应用；可用
`./scripts/apply-patches.sh --status` 检查基线及补丁状态。Host 在总线静默超过
`PIO_USB_STALL_RECOVER_US`（默认 2500 微秒）后执行软拔插恢复，设为 0 可关闭。
KV 保存、删除和初始化 / GC 经 SDK `flash_safe_execute` 暂停双核与 IRQ，Host 可能短暂静默并触发
自愈。运行脚本不写 Flash；保存时保持外设连接的行为仍需上板验证，见 [脚本存储说明](docs/lua-scripts.md)。
固定版 TinyUSB hub 驱动也会在传输失败后重试状态轮询。
这一旧基线不支持经 Hub 连接低速（LS）设备；部分键盘、鼠标会受影响。

## 烧录与验证

1. 按住 Pico 2 BOOTSEL 插入电脑，拷贝 `build/src/pico-ds5-coventer.uf2`。
2. 电脑识别 `DualSense Wireless Controller`；默认脚本不会定时启用额外接口。
   运行中按下再松开 BOOT，待重枚举完成后可通过 WinUSB 连接网页，键鼠仍保持关闭。
3. 实体键鼠/手柄接 GPIO12(D+)/GPIO13(D-) 对应的 Host 端口，可经 USB Hub。
4. 用 gamepad-tester.com 或 Steam 手柄设置观察手柄输入。要验证键盘、鼠标和滚轮转发，
   可在 Lua 中调用 `dev_enable(1, 1, 1)`，或取消默认脚本文末 5 秒延时示例的注释并加载运行。
   UART 日志中，热重载后 `MOUNTED ... hid(kbd=1 mouse=1)` 表示新配置完成挂载。

## 已知限制

- 震动、灯条、自适应扳机输出被忽略；音频接口无实际声音。
- 六轴与触摸板由默认脚本原样转发（`on_motion` → `pad_motion`、`on_touchpad` → `pad_touch`，
  触摸只取前 2 个触点，DS5 报告本就只有 2 个触点槽；
  [触摸报文布局与验证](docs/ds5-touch.md)）；键鼠不映射为 DS5 输入。
- 板载 BOOTSEL 按键以 10ms 轮询 + 三次一致消抖检测，短于约 30ms 的点按可能漏掉；
  读该脚会短暂脱离 XIP，因此每次轮询要停住另一核几微秒（不适合当高频输入）。
- 多手柄最终汇入同一份 DS5 状态，主要面向单手柄场景。
- 当前键盘转发范围为 112 个常规键与 8 个修饰键，不包括消费者控制等其他 Usage Page。
- 热重载的硬件验证范围见专门文档，尚未覆盖所有系统与反复切换组合。

### 网页编辑器与 AI 配置

网页首次未连接设备时编辑器为空且只读，AI 及设备操作禁用；连接后先读取当前运行脚本并替换
编辑器。断线保留编辑内容并只读，重连重新读取设备状态；有本地修改时保留并标注差异，
无本地修改时更新为设备脚本。可点击“读取设备”主动替换。
AI 参数（含 API Key）与 mapper 一样保存在设备 KV 的 `ai_cfg` 键，连接成功后读取，设置保存
等待设备确认；已有设备配置优先，旧浏览器配置仅在设备无配置时迁移。详见
[前端说明](frontend/README.md#ai-配置与设备连接)。
