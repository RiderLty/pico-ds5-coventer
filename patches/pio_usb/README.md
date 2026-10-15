# PIO-USB 基线与补丁

子模块固定在 `9510f795853b468d6a1fdf12dd582b39eddee436`，与
`../pico-hid-mapper` 相同。该版本位于 0.6.0 RX PIO 重写之前，包含拔出设备时
结束所有在途端点传输的修复。选择它是为恢复 Hub 热插拔可靠性；代价是无法支持
经 Hub 连接的低速（LS）设备。

| 补丁 | 来源与作用 |
| --- | --- |
| `0001-sdk2-compat.patch` | 来自 mapper；兼容 Pico SDK 2 的 PIO API 和生成头文件 |
| `0002-device-se0-timeout.patch` | 来自 mapper；限制 device SE0 等待时间。本项目 device 使用原生 USB，此路径不参与正常运行 |
| `0003-tinyusb-endpoint-api.patch` | 适配本项目较新的 TinyUSB；避免重复打开端点，并补充单端点关闭 API |
| `0004-host-stall-recover.patch` | 移植 mapper 的 SOF 静默检测、复核窗口和软拔插恢复；另补齐在途传输清理、连接状态更新及拔插事件隔离 |

`./scripts/setup-sdk.sh` 自动调用 `./scripts/apply-patches.sh`。重复应用是幂等的，
`./scripts/apply-patches.sh --status` 可检查基线和全部补丁。切换基线时若存在本地
修改，脚本会停止，避免覆盖。

自愈默认在 SOF 间隔达到 2500 微秒时触发检测，等待 20ms 复核窗口后进行软拔插，
两次自动恢复至少相隔 2 秒。配置 CMake 参数 `-DPIO_USB_STALL_RECOVER_US=0`
可关闭自动静默检测。库保留 pending/veto、手动重连与统计 API。

TinyUSB Hub 失败重试兼容逻辑在顶层 CMake 中，必要时编译构建目录中的修复副本，
不修改 SDK 源码。本项目固定的 TinyUSB `2d56dc53` 已包含该重试处理。

恢复依赖设备能通过重新枚举的总线复位唤醒；不能保证所有设备都能恢复。
mapper 曾观察到 Hub 后总线供电的 Xbox 手柄无法恢复。本项目的热插拔已通过用户
实机验证；异常总线静默后的自动恢复尚未单独验证。
