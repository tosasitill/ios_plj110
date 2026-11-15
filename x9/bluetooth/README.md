# MT6653 native Bluetooth（进行中）

目标是未修改的 iOS **16.7.16 / 20H392** `bluetoothd`，不是 AppleBT kernel user-client 的仿制品。
本目录的 H4/vendor 适配仅修改经过 UUID 与指令校验的进程内映射，不改 Apple 原件文件。
**当前不可 stage：硬件 session 尚在 SSPM post-download handoff 处 fail-closed，不能验收蓝牙。**

## 当前入口

- `x9_bt=1` 注册 metadata service 与 `/dev/cu.bluetooth`。当前 boot-device 已有此项，但不启动硬件下载。
- 旧 `x9_bt_probe` / `x9_bt_raw` ROM 探针与用户侧 H4 注入路径已移除；selectors 2/3 返回 unsupported。
- `x9_bt_init=1` 是完整 session 的 worker 入口，**当前不加入 boot-device，也不交付部分初始化镜像**。
  它不是绕过前置的开关，无法跳过 SSPM handoff / ownership；失败仍保留确切 errno。
- `kern.x9_bt_boot` 默认 `ENXIO`，仅完整 session 到 DONE 且 driver_owned 才可为0。
  `kern.x9_bt_stage` 是最后返回的 session stage，**不是 ready 指标**。

## 内核实现

- `x9_conn_power.c`：Wi-Fi 与 BT 共用的 connv3 lease / power-done / release；30ms → PMIC_EN →20ms →
  faultb 与 shared combo pre-on；第二 radio 不重启 PMIC、不重放 shared pin setup，最后 lease 才关电。
  未登记但已经为高的 PMIC 返回 `EBUSY`，不猜它属于 Wi-Fi。MT6993 vsel 为真实 no-op，antenna/32K callback 缺失。
  Wi-Fi 的真实 radio loop 保留 lease；返回的失败/诊断路径只释放 Wi-Fi，不再直接关闭 BT 的 PMIC。
- `x9_bt_hardware.c`：PCLK/HCLK 检查先于 hub MMIO；SRAM default 位于 **0x00113c00+0x1e4**，写0x10001；
  multi-host RX/TX request → 最多50ms真实ready → PLL/SPM resources → idle-host 检查 → AP-only FIFO隔离/bypass/pins。
  忙 MD/ADSP、残留 XOFF、未停止的 AP IRQ/DMA 均拒绝，不强行复位运行中的 radio。
  AP UART 的115200分数计算、24M sample7/point3/fracdb,1来自8250；CMM不复制这些参数。
- **保留的硬件缺口**：Android generic CMM24M config 是 TODO，SSPM所用配置和当前 LK 移交还没有闭环证据。
  `post_download` 明确 `ENOTSUP`，不把 default SRAM / AP24M 当作 CMM24M 成功。
  host-awake state bit11、SET bit2、CLR bit4已区分；真实反馈验证代码尚未可达。
- `x9_bluetooth.c`：将 session/下载器接入 worker；XO必须读取已有7个boot args且record长度25，不使用默认校准。
  UART3真实SPI0x148+32的IRQ收包、32KiB原始ring、worker WMT完整帧与opcode选择、保留无关包、实际partial TX。
  不宣称这是Linux APDMA，也不启用没有resume callback的wake-enhance。失败先关自己的IRQ、复位BT、按lease清理。
  UART IRQ hook同步存于overlay与 `x9/xnu/0006-x9-bt-uart-irq.patch`；没有修改共用XNU源树。
- `bluetooth_board.py`：171 B production `sku/hw-0` BT connfem命令，3 B原顺序pinmap，七个必需flags；
  私有 `x9_bt_board.inc` 不入git。`x9_bt_session.c`严格执行 ROM→24M→XO→下载/connfem→真实FW flags→routing→ownership。
- `x9_bt_download.c`：完整52 B section spec、首个匹配type `{0,2,3,0x80,0x90,0x10000}`、type3后connfem、最后bin index3；
  raw firmware1988 B单元或WMT ACL phase1/2/3，所有写入与drain有界，无模糊TX失败后重发。
- `x9_bt_serial.c`：mobile501/0600、独占open、阻塞read/write/select、64-bit termios、16KiB RX/TX FIFO与generation。
  native HCI流只在完整初始化之后接入，不与bootstrap IOUserClient共用命令队列。

## iOS桥

- `X9H4Transport.c`锁定20H392 arm64 UUID `AA9424E8-0648-34E2-B3CF-E6C793B9D471`。
  原件SHA256 `8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f`。
  原POSIX open实际是UART.SK/Skywalk；仅替换open为cu.bluetooth，复用原RxLoop/TX/close。
  H4 buffer先初始化再创建RX线程，完整board结果为0前拒绝open。
- `X9Vendor16.c`仅在显式 `X9_BT_VENDOR=MT6653`、正确UUID/指令与完整board结果为0时安装type0路由；
  null vendor ops返回原生NOT_IMPLEMENTED，不伪造BCM私有能力或成功；保留generic callback及supported-commands真实status。
  LE features回调的日志分支也保留入口status（x20保存，不把日志返回值当作成功），原生8 B mask存储不改。
- `X9Capabilities16.c`绑定真实supported commands/local/LE features，发布扩展扫描/广播、periodic、PAST、coded/2M PHY及广播数量。
  广播数量通过真实203b查询（软件容量上限5），回包前不发布部分能力；原provider once初始化后才同步缓存和标准HCI selector。
  HCI Reset保留真实失败状态，并清理各层能力；旧epoch的未完成203b不能推进新初始化。
  broadcaster启动时刷新构造阶段的legacy选择（本版本实例+0x88），不套iOS27布局。
- **未完成**：其它初始化status路径核对、launch/BlueTool编排、实机COW与能力发布/缓存路由验证。
  未来环境 `BT_HCI_TRANSPORT=H4:POSIX` / `X9_BT_VENDOR=MT6653`，当前不改launch plist。

## 编译与只读核对

```sh
python3 -I x9/bluetooth/build.py --kernel-objects --out x9/out/bluetooth-native/<唯一名>
```

编译/adhoc签名 `btctl`、`X9H4Transport.dylib`；单独编译6个BSD对象、UART IRQ hook及2个IOKit owner对象。
使用常规BT/Wi-Fi私有生成器；不链接完整kernel、不打包boot、不测试、不安装、不操作设备。
输出 `build.json` 保持 `stageable=false`。

未来工具正常交付后仅有只读metadata入口：

```sh
btctl --status
btctl --sections
sysctl kern.x9_bt_boot kern.x9_bt_stage
```

必须后续验证完整HCI Reset/真实capabilities、扫描/配对/BLE，A2DP/HFP/SCO单独验收；当前没有这些成功证据。
