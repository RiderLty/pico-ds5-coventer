# Windows WinUSB 自动绑定

## 已确认的故障

Windows 11 Pro 26200 的现场输出：DS5、音频、鼠标、键盘正常；只有
`USB\VID_054C&PID_0CE6&MI_06`（Pico WinUSB）为代码 28，Service 与
DriverInfPath 为空。注册表
`HKLM\SYSTEM\CurrentControlSet\Control\usbflags\054C0CE60101`
中的 `osvc=0000`，证明 Windows 缓存了该修订号不支持 Microsoft OS 描述符。

旧固件上电先枚举 DS5 + UAC1，5 秒后才追加 IO，却在 IO 未启用时拒绝
`GET_DESCRIPTOR(String, 0xEE)`。Windows 首次查询失败后不再重复查询；
热重载沿用同一 VID/PID/bcdDevice，因此无法通过 Compatible ID 自动匹配
系统自带的 `winusb.inf`。单纯增加版本号仍会在新版本首次枚举时重现。

另外两个描述符问题：Extended Compat ID 功能段偏移 1（整个描述符偏移 17）
必须为 `0x01`，旧实现为零；Extended Properties 未实现，不能通过 OS 描述符
注册应用发现接口所需的 GUID。它们是独立缺陷，不能仅凭现场输出断言各自
对本次代码 28 的影响。

## 固件修复

- 首次基线枚举也返回 18 字节 `MSFT100` 签名；未启用 IO 时返回合法的
  16 字节空 Compatible ID 表，启用时返回 40 字节单功能表。
- 功能段只关联当前 IO 接口，保留字段设为 `0x01`；DS5 与音频不关联 WinUSB。
- 支持 IO 接口的 Extended Properties 查询，返回 `REG_MULTI_SZ` 类型的
  `DeviceInterfaceGUIDs`，GUID 为 `{56D3BAC2-F5C4-43BE-A690-71B61199C15E}`。
  支持头部与完整查询，接口号随键鼠布局变化。
- 每种布局使用独立的 BCD 修订号：`0x0120 | kbd | (mouse << 1) | (io << 2)`。
  基线 `0x0120`，仅开 IO 为 `0x0124`，全开为 `0x0127`。在首次 USB 连接前或运行期断开窗口内更新修订号与配置。
  后续改变 OS 描述符内容时，需升级整组修订号，避免旧缓存。

保留 Sony VID/PID、原有接口顺序和无 IAD 布局。默认脚本中的 5 秒自动启用逻辑现已
整段注释保留，运行中按下再松开 BOOT 可仅启用 WinUSB；脚本也可拦截该动作并自行选择接口，
见 [BOOT 用法](lua-scripts.md#boot-按键与接口启用)。
这些更改已通过编译与主机侧描述符测试；用户已确认 Windows 真机恢复正常识别。

## 网页自动重连与持久授权

工作台启动、检测到 USB 插入或页面恢复可见时，使用 WebUSB `getDevices()` 查询本站已授权
设备并自动重连。仅接受 `054c:0ce6`、厂商类 `0xFF` 且具备 bulk IN/OUT 的接口，
因此不会尝试打开真正 DS5 的 HID 接口。重连仍执行协议 RESET，再读取脚本和 AI 配置。
手动断开或选择 UART 会暂停自动连接；点击“连接 USB”恢复。多个设备时优先使用上次
记录的序列号，无法确定唯一候选则要求手动选择。

Chromium 只对带非空 USB 序列号的设备保存跨拔插权限；无序列号时，拔出或热重载会撤销
临时授权，前端保存 VID/PID 不能代替浏览器授权。因此固件启用 IO 时设置 `iSerialNumber=6`，
返回 Pico SDK 缓存的 64 位板卡唯一 ID（16 个十六进制字符）。所有带 IO 的键鼠布局使用
相同序列号；关闭 IO 时恢复无序列号基线。回调只读取启动前缓存，不执行 Flash 操作或加锁。
首次刷入此版本后需重新手动授权一次；后续同源页面刷新、拔插和带 IO 的布局切换可以自动重连。
关闭 IO 后无法通讯，直到脚本或 BOOT 再次启用 IO。清除网站权限后仍须手动授权。

序列号和自动重连已有主机/浏览器模拟回归；Windows 实机上的持久授权与驱动就绪时序仍需验证。

## 刷入与验证

按住 Pico 2 BOOTSEL 接入，复制 `build/src/pico-ds5-coventer.uf2`。默认脚本运行后，
按下再松开 BOOT，等待 USB 重枚举完成。默认仅启用 IO，接口号为 `MI_04`；若运行的是
KV 中的自定义脚本，需确认它没有拦截 BOOT 默认行为，或由该脚本自行启用 IO。
新修订号避开旧版缓存，首次验证不需要删除注册表或手动装驱动。

在 Windows PowerShell 执行以下只读查询。逐项展开数组，防止 PowerShell 截断兼容 ID：

```powershell
$device = Get-PnpDevice -PresentOnly |
    Where-Object { $_.InstanceId -like 'USB\VID_054C&PID_0CE6&MI_04\*' }
$device | Format-List Status, Class, FriendlyName, InstanceId
foreach ($item in $device) {
    foreach ($key in @(
        'DEVPKEY_Device_HardwareIds'
        'DEVPKEY_Device_CompatibleIds'
        'DEVPKEY_Device_Service'
        'DEVPKEY_Device_DriverInfPath'
        'DEVPKEY_Device_ProblemCode'
    )) {
        $key
        (Get-PnpDeviceProperty -InstanceId $item.InstanceId -KeyName $key).Data
    }
}
reg.exe query 'HKLM\SYSTEM\CurrentControlSet\Control\usbflags\054C0CE60124'
```

默认仅开 IO 布局的预期结果：硬件 ID 含 `REV_0124&MI_04`，兼容 ID 含
`USB\MS_COMP_WINUSB`，Service 为 `WinUSB`，DriverInfPath 为 `winusb.inf`，
ProblemCode 为 `0`，新修订号的 `osvc` 通常为 `0121`（支持 OS 描述符，vendor code 0x21）。
之后用 Chrome/Edge 打开 `tools/webhid-test.html`，点击“连接 WinUSB”，
验证 INFO 查询和数据传输。

若脚本调用 `dev_enable(1, 1, 1)` 全开接口，IO 位于 `MI_06`，修订号为 `REV_0127`；
将查询中的 `MI_04` 改为 `MI_06`、注册表路径末尾 `0124` 改为 `0127`。

若仍失败，保留以上输出，再提取设备安装日志：

```powershell
Select-String -Path "$env:windir\INF\setupapi.dev.log" `
    -Pattern 'VID_054C&PID_0CE6&MI_04' -SimpleMatch -Context 10,70 |
    Select-Object -Last 3
```

## 本地检查

```bash
cmake --build build -j8
python3 tools/test_usb_descriptors.py
```

主机测试直接编译实际 `src/usb_descriptors.c`，使用项目固定的 TinyUSB 头文件，
替代硬件依赖与控制传输出口；检查初始枚举、8 种配置和反复切换、修订号隔离、
配置边界、OS 签名、Compatible ID 格式、UTF-16 GUID 与终止符、短请求和无效请求。
它不验证 Windows 驱动安装或实际 USB 总线时序。

参考：[Microsoft OS 描述符与缓存](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors)、
[WinUSB 自动安装](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/automatic-installation-of-winusb)、
[OS 1.0 描述符规范](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-os-1-0-descriptors-specification)。
