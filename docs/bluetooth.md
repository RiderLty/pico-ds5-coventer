# 蓝牙固件（Pico 2 W）

[下载最新固件](https://github.com/RiderLty/pico-ds5-coventer/releases/latest) · [网页工作台](https://riderlty.github.io/pico-ds5-coventer/)

同一版本提供两套固件，请按输入方式与板卡选择：

| Release 附件后缀 | 板卡 | 输入 |
|---|---|---|
| `usb-pico2.uf2` | Raspberry Pi Pico 2 | GPIO12/13 的 PIO-USB，键鼠 / HID / Xbox XInput / Switch Pro |
| `bluetooth-pico2w.uf2` | Raspberry Pi Pico 2 W | 板载 CYW43 经典蓝牙，DualSense / DualSense Edge，单手柄 |

两版都经板卡原生 USB 向 PC 输出 DS5 + UAC1，保留 Lua、键鼠输出、WinUSB、UART 和设备 KV。
蓝牙版替换 USB Host，不能同时接 GPIO12/13 的 USB 输入；不支持 USB 蓝牙适配器。
目前支持范围为 DualSense / DualSense Edge 的 BR/EDR HID 输入，不含 Xbox BLE、Switch 或通用蓝牙键鼠。
PC 无需蓝牙；PC 连接的是 Pico 的 USB，手柄连接的是 Pico 2 W 板载蓝牙。

## 配对与重连

1. 按住 Pico 2 W 的 BOOTSEL 插入 PC，复制 `bluetooth-pico2w.uf2`。
2. 手柄先关机，按住 **PS + Create**，直到灯条快速闪烁进入配对模式。
3. Pico 2 W 自动扫描并连接。板载绿色 LED 扫描时闪烁，HID 通道就绪时常亮。
4. 使用默认 Lua 脚本，按键、摇杆、扳机、触摸、六轴经过 Lua 转发成 USB DS5 输入。
5. 关机或断线后，按手柄 **PS** 重连。配对密钥保存在 Pico 的独立 Flash 分区，板卡断电后仍保留。

配对给其他主机后，再次按 **PS + Create** 与 Pico 配对。认证失败会清除对应的过期密钥，允许重新配对。
没有配对成功或断开后会重复扫描，也可等待已配对手柄主动重连；连接后停止扫描，避免影响输入回报。
同一时刻只接一只手柄；连接期间不接受第二只。

两版的 BOOT 行为相同：**运行中按下再松开 BOOT**，默认开启 WinUSB，其他接口保持不变。
在网页工作台手动授权后，可按设备已有的 WinUSB 自动重连规则找回设备。
Lua 的 `on_boot_button(pressed)` 返回 `true` 可拦截默认行为，自行决定启用哪些接口。

网页管理的是 Pico 的 USB/UART 配置链路，**浏览器不直接配对蓝牙手柄**。
无需在浏览器选择手柄，也不依赖 Web Bluetooth。

## 构建与发布

```bash
./build.sh                         # USB / Pico 2，build/src/pico-ds5-coventer.uf2
./build.sh --bluetooth             # 蓝牙 / Pico 2 W，build-bluetooth/src/pico-ds5-coventer.uf2
./build.sh --bluetooth --skip-setup # 已准备依赖时增量构建
```

两版默认使用不同目录，避免 CMake 的板卡缓存交叉污染。用 `--build-dir DIR` 可指定独立目录。
直接使用 CMake 时需同时指定 `-DENABLE_DS5_BT=ON -DPICO_BOARD=pico2_w`。
推送 `v*` tag 后，CI 两个矩阵任务分别构建，全部成功才发布同一版本的 Release，
包含两套 UF2 / ELF / BIN 和统一 `SHA256SUMS`，任务产物也可在 Actions 页面下载。

## 实现与存储

- core0 独占 TinyUSB Device 和描述符；core1 轮询 CYW43/BTstack、hidkit 和 Lua。
  启动先执行脚本顶层并确定首次 USB 枚举能力，再启动蓝牙，无需等待手柄配对。
- GAP/L2CAP 管理 ACL、认证和加密；注册 HID Control / Interrupt 两个服务，支持 PS 主动重连。
  BTstack 自动回复持久化密钥，应用不重复发送原始 HCI 回复。
- 沿用 mapper 验证过的 Feature 0x05 请求与 0x32/0x10 输出初始化，启用 0x31 完整输入流。
  两个通道的初始化都等待 `CAN_SEND_NOW`，缓冲暂满保留请求重试。
- 校验 79 字节 L2CAP HID 输入的 CRC32，排除截断、短报文和麦克风数据。
  0x31 公共数据体还原为 USB 0x01，经现有 hidkit DS5/Edge 解析器进入 Lua，坐标不缩放。
  为接受 Edge 附加键，蓝牙路径使用 Edge 兼容解析档案（`hidkit.info()` 中的 054c:0df2 表示该档案）。
  静音键为 `hidkit.BTN_EXTRA_1`，Edge FN1/FN2/左右背键为 `BTN_EXTRA_2..5`，可由 Lua 映射；
  宿主仍枚举标准 DS5，默认脚本不会输出 Edge 专用身份。
- 任一 HID 通道关闭立即释放输入并断开 ACL；连接超时或 10 秒无有效完整输入也清理链路。
  断线清理在 Lua 释放事件之后追加 DS5 断连帧，避免残留触点、六轴和按键。
- 官方 Pico 2 W 为 4 MiB Flash：蓝牙双 bank 位于 `0x37e000..0x37ffff`（8 KiB），
  原有 Lua/AI KV 保持 `0x380000..0x3fffff`（512 KiB），二者互不覆盖。
  两版均在链接期保留该 520 KiB 区域，检查固件不侵入持久化区，所有 Flash 写入沿用 SDK 双核安全锁出。

参考 [pico-hid-mapper](https://github.com/RiderLty/pico-hid-mapper) 与
[DS5Dongle](https://github.com/awalol/DS5Dongle)，协议初始化参考的 MIT 许可见
[DS5Dongle-LICENSE](third-party/DS5Dongle-LICENSE)。

## 验证范围

已提供双版本固件构建与主机回归测试（`python3 tools/test_ds5_bt.py`），覆盖 CRC/触摸布局、
异步通道打开、配对扫描、已绑定 PS 重连、发送重试、认证失败、断线释放和超时。
测试通过真实 BTstack 事件格式驱动应用状态机，射频与 Flash 后端使用替身。
**本次未连接实体 Pico 2 W / 手柄进行上板验证**，无线稳定性、不同手柄固件和高频输入
仍需刷入后实测。震动、音频、灯条回传与自适应扳机不在本次支持范围。
