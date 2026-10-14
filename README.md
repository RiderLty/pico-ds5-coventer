# pico-ds5-coventer

USB 输入 → Sony DualSense (DS5) 转换器，运行于 Raspberry Pi Pico 2 (RP2350)。

由 [pico-hid-forward](https://github.com/) 改造：保留 PIO-USB Host 读取实体设备的
全部代码，原生 USB 设备端由"模拟键鼠"替换为"模拟 DS5 手柄"（参考
[awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) 的 tinyusb 通讯格式），
并移除了串口/CDC 输出路径。

输入侧使用 [hidkit](https://github.com/RiderLty/hidkit) 解析（由
[pico-hid-mapper](https://github.com/RiderLty/pico-hid-mapper) 精简移植），
支持键盘鼠标、XInput 手柄（Xbox 360/One/Series 有线）与 Switch Pro
（057e:2009 USB）；输出保持 DS5 模拟不变。

```
键鼠 / Xbox 手柄 / Switch Pro
        │ USB（可经 Hub）
        ▼
[PIO-USB Host, core1, GPIO12/13] ──SPSC队列──> [core0]
  hidkit 解析（键盘/鼠标/HID 手柄）               │ 映射引擎 (ds5_pad)
  XInput 类驱动 / Switch Pro 握手库                ▼
                          PC / 主机 <──USB──> [原生 USB Device：DualSense VID 0x054C PID 0x0CE6]
                                              0x31 输入报告 63B @ 1kHz (EP 0x81)
                                              输出报告 0x02 接收忽略   (EP 0x02)
```

## 功能

- DS5 模拟：双摇杆、双模拟扳机、十字键、全部数字按键、触摸板（含按键）
- 输入：键鼠（hidkit 描述符解析，支持 NKRO/游戏鼠）+ XInput 手柄 +
  Switch Pro（USB 握手 + SPI 校准读回）；DS5 作为输入设备同样被 hidkit
  布局表识别
- 输入合并：手柄已连接时摇杆/扳机/十字键由手柄直通，按键取手柄与键鼠映射
  的并集；手柄拔出自动回落键鼠映射（过渡占位逻辑，后期由 Lua 接管输入输出）
- 音频端点（UAC1，照搬 DS5Dongle 描述符）：扬声器 OUT EP / 麦克风 IN EP，
  主机能识别并开关音频流，数据全部丢弃（扬声器数据不读、麦克风不发），无实际声音
- 陀螺仪/加速度填静止值，电池/状态字节用真实 DS5 抓包模板
- 主机 GET_REPORT（校准/配对 feature report）按描述符尺寸返回零数据；
  输出报告（震动/灯条）接收但忽略
- 键位映射、灵敏度、回中参数集中在 `src/config.h`
- **板载 Lua 引擎**（5.4.6，预制脚本无动态加载）：hidkit 全部回调
  （on_key / on_mouse_btn / on_mouse_move / on_mouse_wheel / on_gamepad_btn /
  on_gamepad_abs / on_motion / on_touchpad / on_dropped）+ tick(dt_us)（主循环
  每轮调用）经 `lua_binder` 投递到脚本，返回 true 可拦截 C 默认路径（映射逻辑
  可交由脚本接管）；`hidkit.info()` 打印设备登记表，`hidkit.BTN_*` 等常量供脚本
  使用；脚本经 `enable_listen_*` 声明监听，未声明的事件零开销跳过 VM；print 走
  调试串口；10ms 步数看门狗防死循环。改脚本：编辑 `scripts/default.lua` 后运行
  `python3 tools/embed_lua.py scripts/default.lua src/lua_default_script.h`
- **键鼠输出接口（默认关）**：Lua `enable_km(kbd, mouse)` 运行期开启
  NKRO 键盘（QMK 风格双报告：6KRO boot + 112 键位图，itf5/EP0x87）与
  16bit 相对鼠标（6 字节报文无 Report ID，itf4/EP0x86）——动态组装配置描述符 +
  USB 重枚举（约 0.5s，期间 DS5 暂停上报）。数据通路：Lua（core1）→ dkb_*/dmo_*
  → 跨核队列（键盘绝对状态队列丢旧保新 / 鼠标增量队列合并防丢位移）→
  core0 出队发送。绑定名与 pico-hid-mapper 一致（dkb_key/dmo_move/…）
- 调试串口：uart0 TX=GPIO2 / RX=GPIO3，**4000000** 8N1（240MHz 下分频零误差，
  依赖 main.c 把 clk_peri 挂回 clk_sys；挂载/拔出/握手日志，仅冷路径输出，
  不随报文刷屏。适配器不支持时可 -DDEBUG_BAUDRATE 覆盖）

## 默认映射（FPS）

| 键鼠 | DS5 | | 键鼠 | DS5 |
|---|---|---|---|---|
| W/A/S/D | 左摇杆 | | 鼠标移动 | 右摇杆 + 触摸板 |
| 鼠标左键 | R2（模拟扳机） | | 鼠标右键 | L2（模拟扳机） |
| 鼠标中键 | R3 | | Space | ✕ |
| 左Shift | L3 | | 左Ctrl | R3 |
| E | □ | | F | ○ |
| R | △ | | Q | L1 |
| Esc | Options | | Tab | 触摸板按键 |
| 方向键 | 十字键 | | 滚轮 | （忽略） |

手柄输入直通：A→✕、B→○、X→□、Y→△、LB/RB→L1/R1、LT/RT→L2/R2、
Select/Create→Create、Start→Options、LS/RS→L3/R3、Home→PS、
十字键→十字键。

## 构建

依赖以 git submodule 管理（与 DS5Dongle CI 环境一致），CMakeLists 中直接指定
SDK 路径，无需设置环境变量：

| 依赖 | 版本 | 说明 |
|---|---|---|
| `lib/pico-sdk` | submodule @ 2.3.0 | 内嵌 tinyusb 需切到 2d56dc53（UAC1 支持） |
| `lib/pico_pio_usb` | submodule @ 675543bc | tinyusb 官方配对版本（0.7.2 血脉） |
| `lib/hidkit` | submodule @ v0.3.1 | HID/XInput 语义解析 core |
| `lib/hidkit-tusb-xinput` | submodule @ 4fffdf3 | XInput 类驱动（含 0.19+ open() 兼容） |
| `lib/hidkit-tusb-switch-pro` | submodule @ db5dc8a | Switch Pro USB 握手库 |
| `lib/lua-5.4.6` | vendored 拷贝 | Lua 引擎（与 pico-hid-mapper 同源） |

```bash
./scripts/setup-sdk.sh      # 初始化 submodule + 切换 tinyusb commit
cmake -B build -S .
make -C build -j8
# 产物: build/src/pico-ds5-coventer.uf2
```

host 端口引脚（GPIO12/13）在 `src/main.c` 的 `pio_cfg.pin_dp` 设置，
不依赖对库源码的修改。

### PIO-USB 版本说明

本仓库固定 tinyusb 官方配对的 pio-usb **0.7.2**（新血脉），未套用
pico-hid-mapper 的旧血脉（9510f79）+ 补丁方案——逐项分析见其
`patches/pio_usb/README.md`：0001（SDK2 兼容）与 0002（device SE0 超时）
对本仓库不适用（0.7.2 原生支持 SDK2；本仓库设备端走原生 USB）；
0004（总线静默自愈）解决的是"运行期写 flash 冻结主机 → 设备 suspend"
的场景，本仓库不写 flash，若将来引入 KV 存储，需对 0.7.2 重新推导。
TinyUSB hub 驱动韧性修复（0.19+）已内置于本仓库的 tinyusb commit。

## 烧录与使用

1. 按住 Pico 2 的 BOOTSEL 插入电脑，拷贝 `pico-ds5-coventer.uf2`
2. 电脑端会出现 "DualSense Wireless Controller"（有线手柄）
3. 实体键盘/鼠标/手柄通过 OTG 转接头接 GPIO12(D+)/GPIO13(D-)（可经 USB Hub）

## 验证

- Chrome 打开 gamepad-tester.com 或 Steam 手柄设置，观察实时输入
- 或用 hidapi 按 VID/PID 0x054C/0x0CE6 打开设备读输入报告
- 调试串口（GPIO2 @921600）观察设备挂载/拔出与 Switch Pro 握手进度

## 已知限制

- 滚轮未映射；震动/灯条/自适应扳机输出被忽略
- 音频端点仅存在（主机可开关流），无实际音频数据
- 触摸板镜像在鼠标停止 60ms 后松开（`TOUCH_RELEASE_MS`）
- 多手柄输入合并到最后一份手柄状态上（转换器场景单手柄为主）
