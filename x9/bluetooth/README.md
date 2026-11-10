# X9 Bluetooth bootstrap

首阶段的 **MT6653 UART/H4 transport + 自有 iOS IOKit 诊断接口**。
已编译不代表实机握手成功，更不代表蓝牙扫描、配对或音频可用。

## 参考与边界

- OPPO `modules/vendor/mediatek/kernel_modules/connectivity/bt/linux_v2/`:
  `uart/btmtktty.c` 的 UART/WMT 查询，`btmtk_main.{c,h}` 的 BT 容器布局。
- `kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts`:
  UART3 `0x16030000`，UARTHUB，GPIO241/242，BT_RST GPIO249，`dx5_2g2a`。
- 已保存的 Android 启动记录确认 `btmtk_uart_unify`、MT6653、115200→24 Mbaud、
  `BT_RAM_CODE_MT6653_dx5_2g2a_1_hdr.bin`。蓝牙不是 Wi-Fi 的 PCIe 通道。
- 本地 UniXNU `ae68700`、hackios `c84b463` 提供 IOResources 注册模式，
  没有可直接移植的本板蓝牙后端。
- 原始 20H392 `bluetoothd` 包含 AppleBluetoothModule/AppleBT 管理与管道接口，
  也包含 `/dev/cu.bluetooth`、`/dev/uart.bluetooth` UART 路径。
  **只有字符串不足以确定原生 ABI/传输选择条件**。本阶段不发布这些串口别名、
  不冒充 AppleBluetoothModule、不修改原始 `bluetoothd`。

## 开关

| Boot arg | 行为 |
| --- | --- |
| `x9_bt=1` | 注册 `X9BluetoothTransport`，读取 AO GPIO/时钟门；默认关闭 |
| `x9_bt_probe=1` | 另行启用有界 ROM 查询；须同时有 `x9_bt=1` |
| `x9_bt_raw=1` | ROM 握手完成后允许管理员客户端发送 H4；默认关闭 |
| `x9_bt_discovery=1` | 将服务名改为 `bluetooth`，仅用于原生 name-match 观察 |

主动模式只接管**已上电、已开时钟、IRQ/DMA/FIFO 空闲**的 AP/CMM UART，
拒绝忙碌的 MD/ADSP UARTHUB。不会开关共享 PMIC、POR、WF_RST、时钟或共享 reset。
门未开时不访问 off-domain MMIO；失败恢复所改的 UART 配置、GPIO 位与 bypass 位，
不发固件 assert、不循环重试。FIFO 内已发送/接收的字节不可能撤销。
这不是完整的 UARTHUB/SSPM 上电驱动；前置条件缺失会明确失败。

## 构建与客户端

常规内核构建自动运行 `x9/tools/bluetooth_firmware.py`。
可用 `local.json` 的 `bluetooth_firmware` 指定固件目录，否则使用 Wi-Fi 固件目录。
它校验 BT 的**小端**节表、范围和重叠，只生成元数据，不嵌入/下载固件字节；
没有本地固件时生成显式 stub。生成头不入 git。

```sh
python3 -I x9/bluetooth/build.py --out .local/x9/bluetooth/client
```

上式只编译、adhoc 签名 iOS 16 arm64 `btctl`，不会安装或操作设备。
工具经正常开发交付途径可用后，在 iOS 上：

```sh
btctl --status
btctl --sections
# 以下需要管理员、x9_bt_raw=1 和真实 ROM_READY；每次只发一个只读查询。
btctl --query-uart
btctl --chip-id
```

`x9_bluetooth.h` 定义 ABI v1：selector 0 状态、1 固件节、2 H4 入队、3 H4 出队。
只支持同步 inband 参数；拒绝未知 selector、异步与 descriptor，管理员独占 H4。
发送成功表示入队，不表示硬件命令完成；必须核对实际匹配的 WMT 回复。
H4 支持分片/连续帧、ACL/SCO/event/ISO 长度解析、超长包按声明边界丢弃、
半帧超时、队列背压与真实 TX/RX/错误统计。ROM 查询的回复不交给外部客户端抢读。

## 后续验收

1. 先确认真实 LK 时钟/SSPM/UARTHUB 状态，补齐不能接管时的初始化链路。
2. ROM `query_uart(2)` 与 `chip_id(9)` 必须收到正确的完整事件，不能套用厂商的 chip-id 失败默认值。
3. 实现 ROM patch→ILM→connfem→校准→RAM code、低功耗 ownership，验证 HCI Reset/真实本机能力。
4. 还原 20H392 的传输选择及原生 UART/AppleBT ABI，接入原始 bluetoothd，再验收扫描和配对。

在上述阶段完成前，`firmware_ready` 始终为 0；当前不能验收真实蓝牙功能。
