# pico-ds5-coventer

键盘鼠标 → Sony DualSense (DS5) 转换器，运行于 Raspberry Pi Pico 2 (RP2350)。

由 [pico-hid-forward](https://github.com/) 改造：保留 PIO-USB Host 读取实体键鼠的
全部代码，原生 USB 设备端由"模拟键鼠"替换为"模拟 DS5 手柄"（参考
[awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) 的 tinyusb 通讯格式），
并移除了串口/CDC 输出路径。

```
实体键鼠 ──USB──> [PIO-USB Host, core1, GPIO12/13] ──SPSC队列──> [core0]
                键盘 8B 报文 / 鼠标归一化帧                        │ 映射引擎 (ds5_pad)
                                                                  ▼
PC / 主机 <──USB──> [原生 USB Device：DualSense VID 0x054C PID 0x0CE6]
                                  0x31 输入报告 63B @ 1kHz (EP 0x81)
                                  输出报告 0x02 接收忽略   (EP 0x02)
```

## 功能

- DS5 模拟：双摇杆、双模拟扳机、十字键、全部数字按键、触摸板（含按键）
- 陀螺仪/加速度填静止值，电池/状态字节用真实 DS5 抓包模板
- 主机 GET_REPORT（校准/配对 feature report）按描述符尺寸返回零数据；
  输出报告（震动/灯条）接收但忽略
- 键位映射、灵敏度、回中参数集中在 `src/config.h`

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

## 构建

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -B build -S .
make -C build -j8
# 产物: build/src/pico-ds5-coventer.uf2
```

## 烧录与使用

1. 按住 Pico 2 的 BOOTSEL 插入电脑，拷贝 `pico-ds5-coventer.uf2`
2. 电脑端会出现 "DualSense Wireless Controller"（有线手柄）
3. 实体键盘/鼠标通过 OTG 转接头接 GPIO12(D+)/GPIO13(D-)（可经 USB Hub）

## 验证

- Chrome 打开 gamepad-tester.com 或 Steam 手柄设置，观察实时输入
- 或用 hidapi 按 VID/PID 0x054C/0x0CE6 打开设备读输入报告

## 已知限制

- 带 Report ID 前缀的键盘报文按原始 8 字节透传（继承自 pico-hid-forward 的限制）
- 滚轮未映射；震动/灯条/自适应扳机输出被忽略
- 触摸板镜像在鼠标停止 60ms 后松开（`TOUCH_RELEASE_MS`）
