# X9 Bluetooth：MT6653 下载执行器与 iOS16 H4 桥（进行中）

已有 **ROM bootstrap、自有 IOKit 诊断接口、Android connac3 固件下载执行器、原生 iOS16 H4 串口适配代码**。
已编译不代表实机握手成功，更不代表蓝牙扫描、配对或音频可用。

## 参考与边界

- OPPO `modules/vendor/mediatek/kernel_modules/connectivity/bt/linux_v2/`:
  `uart/btmtktty.c` 的 UART/WMT 查询，`btmtk_main.{c,h}` 的 BT 容器布局。
- `kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts`:
  UART3 `0x16030000`，UARTHUB，GPIO241/242，BT_RST GPIO249，`dx5_2g2a`。
- 已保存的 Android 启动记录确认 `btmtk_uart_unify`、MT6653、115200→24 Mbaud、
  `BT_RAM_CODE_MT6653_dx5_2g2a_1_hdr.bin`。蓝牙不是 Wi-Fi 的 PCIe 通道。
- 本地 UniXNU `e893717` / QcaBluetooth `986d6319` 提供传输前端与 native routing 参考，
  不移植高通初始化或 iPadOS27 的 ABI/偏移。
- 原始 20H392 `bluetoothd` 包含 AppleBluetoothModule/AppleBT 管理与管道接口，
  也包含 `/dev/cu.bluetooth`、`/dev/uart.bluetooth` UART 路径。
  **已追清：encoding1选择H4，但名为 POSIX 的底层实际打开 UART.SK/Skywalk**。
  `X9H4Transport.c` 只替换该表的open slot，保留原始H4 parser、RxLoop、TX、close及stack锁。
  UUID/解析后函数指针不匹配则不修改；不修改 Apple 原件文件、不冒充 AppleBluetoothModule。

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
它校验 BT 的**小端**节表、范围和重叠，生成元数据与私有 payload：
`x9/kernel/generated/x9_bt_{firmware,payload}.inc`（均 gitignored）。
`x9_bt_download_builtin` 使用真实固件字节，但**生成/编译不等于实际下载**；
没有本地固件则生成 stub，调用返回 `ENOENT`。

```sh
python3 -I x9/bluetooth/build.py --out x9/out/bluetooth-native/client --kernel-objects
```

上式编译、adhoc 签名 iOS16 arm64 `btctl` / `X9H4Transport.dylib`，并单独编译四个 BSD overlay 对象。
不链接完整内核、不打包 boot、不运行测试、不安装或操作设备。
省略 `--kernel-objects` 则只编用户态。`build.json` 明确 `stageable=false`，尚不改 launch plist。
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

## 已落地的协议和前端

- `bluetooth_board.py`：从实机 production `sku/hw-0` 编码171 B BT connfem WMT ACL，
  原顺序3 B pinmap、七个必需flags、FEM/ctrl-pin/truth/layout顺序与Android一致；私有生成头不入git。
- `x9_bt_session.c`：115200/query/CTSRTS/chip → 24M/raw wake → 实机25 B XO/平台word →
  builtin下载器/connfem → 实际FW配置查询 → post-download hub配置 → ownership验证。
  所有步骤严格校验WMT完整长度/opcode/status；硬件board ops仍待实现，不能用空回调解锁串口。
- `x9_bt_download.c`：完整52B section spec 的announce、status0/1/2判定、其他下载者最多20次100ms等待、
  type `{0,2,3,0x80,0x90,0x10000}` 首个匹配、type3后强制board connfem回调、最后bin index3。
  UART DMA为不带H4的1988B原始流，正确处理partial/zero write、有界超时、drain后1ms间隔与phase3事件；
  WMT fallback为1978B payload的ACL phase1/2/3。每次UART写前验证全容器，不在模糊TX失败后重发。
- `x9_bt_serial.c`：`/dev/cu.bluetooth`（mobile 0600）、独占open、阻塞read/write/select、
  64-bit termios ioctls、16KiB RX/TX FIFO、TX peek/commit背压和generation隔离、关闭/失败唤醒。
  `kern.x9_bt_boot` 默认 `ENXIO`；只有board完整初始化结果可置0，ROM_READY不能解锁原生栈。
- `X9H4Transport.c`：锁定20H392 UUID `AA9424E8-0648-34E2-B3CF-E6C793B9D471`。
  源原件 SHA256 `8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f`。
  未来安装配置用 `BT_HCI_TRANSPORT=H4:POSIX` / dylib注入；当前**不提供可stage配置**。
  open先检查完整board结果，失败不启动RxLoop；原始H4接收buffer先初始化再创建原始RX线程。
- `X9Vendor16.c`：显式 `X9_BT_VENDOR=MT6653` 且完整board结果为0才安装type0路由。
  核对本版原始指令后，COW修改进程内constructor/registration/vendor-init/callback路径；
  Apple原件文件不改。VSC ops保持null，返回原生NOT_IMPLEMENTED而非假成功；保留通用HCI回调、
  supported-commands真实错误状态与非type0回退路径。**仍未做native model能力getter适配，未实跑**。

## 仍须完成（不能以编译成功代替）

1. 先确认真实 LK 时钟/SSPM/UARTHUB 状态，补齐不能接管时的初始化链路。
2. ROM `query_uart(2)` 与 `chip_id(9)` 必须收到正确的完整事件，不能套用厂商的 chip-id 失败默认值。
3. 将下载执行器接入完整115200→24M→XO→connfem→post-fwdl配置→低功耗ownership的board worker，
   将真实HCI流接到serial RX/TX，验证HCI Reset/真实本机能力。当前执行器**未调用**，TTY仍fail-closed。
4. 完成20H392 chipset/vendor ops与实际能力回调适配（type0目前被原件拒绝，不能冒充BCM），
   再接入原始bluetoothd，验收扫描/配对/BLE，蓝牙音频单独验收。

在上述阶段完成前，`firmware_ready` 始终为 0；当前不能验收真实蓝牙功能。
