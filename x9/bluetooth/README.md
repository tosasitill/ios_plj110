# MT6653 native Bluetooth（进行中）

目标是未修改的 iOS **16.7.16 / 20H392** `bluetoothd`，不是 AppleBT kernel user-client 的仿制品。
本目录的 H4/vendor 适配仅修改经过 UUID 与指令校验的进程内映射，不改 Apple 原件文件。

## 当前状态与证据（2026-11-26）

- **passive 默认**：`x9/build/boot-device.json` 只有 `x9_bt=1`，注册 metadata service 与
  `/dev/cu.bluetooth`，但不启动硬件下载。完整初始化候选必须显式追加
  `x9_bt_init=1 x9_bt_fc=1`；`x9_bt_fc=1` 使用 MTK SW 流控。
- **历史设备观察**：`~/425.txt` 对应 `x9-bt-vendor-events-20261125`（提交 `414cbe3f`）。
  FE01 被识别为 connac3 动态固件 section 请求并完成 section 2 重载；完整 capabilities 已返回；
  `0xE0..0xFF` 厂商事件在 Apple H4 parser 前消费后，先前的 reason-607 崩溃消失，bluetoothd
  存活超过 139 s。这不是当前主线或当前设备状态的重新验收。
- **仍未验收**：Settings 灰色、`BluetoothManager.available`、实际扫描/配对/连接/BLE、
  A2DP/HFP/SCO 双向声音、睡眠与活动连接 suspend/resume。H4 只提供 HCI，不是 PCM codec。
- **Settings 候选**：`.worktrees/bt-default-on` 的
  `x9-bt-settings-diag-20261125-r2` 已有完整 host 构建，但未刷入、未安装 root、未运行
  Preferences 诊断 hook。`BluetoothManager` 只有 `[self+0x18]==2` 才 available；五个原生
  setup 调用全部成功才写 state 2，不能直接改 state 或伪造 native 返回。
- 根 payload 的 `candidate_ready`、`stageable`、`root_installed` 与
  `runtime_verified/audio_verified` 是不同状态；当前没有搜索或音频成功证据。

## 启动入口

- `x9_bt=1`：注册 metadata service 与 `/dev/cu.bluetooth`；不启动硬件下载。
- 旧 `x9_bt_probe` / `x9_bt_raw` ROM 探针与用户侧 H4 注入路径已移除；selectors 2/3 返回 unsupported。
- `x9_bt_init=1`：完整 session 的 worker 入口。它不是绕过 ownership/SSPM handoff 的开关，
  失败仍保留确切 errno；完整初始化镜像必须同时提供 `x9_bt_fc`。
- `x9_bt_fc`：Android `UART_FC` enum，0=NONE、1=MTK SW、2=Linux、3=HW。
  缺失 `ENOENT`、范围外 `EINVAL`、尚未实现的 Linux/HW 模式 `ENOTSUP`，全部在供电前返回。
  enum 1 配置 AP 的 MTK SW backend（WMT byte `0x80`），不能选 0 绕过。
- `kern.x9_bt_boot` 默认 `ENXIO`，仅完整 session 到 DONE 且 driver_owned 才可为 0。
  明确睡眠后为 `EHOSTDOWN`，转换中为 `EBUSY`，不能把保留固件当作 awake。
  `kern.x9_bt_stage` 是最后返回的 session stage，**不是 ready 指标**。
- `kern.x9_bt_power` 是同步的明确 ownership 请求：0=awake/resume、1=asleep、2=初始化/转换/不可用。
  只有 root/mobile 可写，sole UART worker 执行；重复目标幂等、并发转换拒绝，不重启或重下载。
  sleep 只允许关闭且 RX/TX 空、无 wire claim 的 transport，活动 daemon 返回 `EBUSY`。

## 内核实现

- `x9_conn_power.c`：Wi-Fi 与 BT 共用的 connv3 lease / power-done / release；30ms → PMIC_EN →20ms →
  faultb 与 shared combo pre-on；第二 radio 不重启 PMIC、不重放 shared pin setup，最后 lease 才关电。
  未登记但已经为高的 PMIC 返回 `EBUSY`，不猜它属于 Wi-Fi。MT6993 vsel 为真实 no-op，antenna/32K callback 缺失。
  Wi-Fi 的真实 radio loop 保留 lease；返回的失败/诊断路径只释放 Wi-Fi，不再直接关闭 BT 的 PMIC。
- `x9_bt_hardware.c`：PCLK/HCLK 检查先于 hub MMIO；SRAM default 位于 **0x00113c00+0x1e4**，写0x10001；
  multi-host RX/TX request → 最多50ms真实ready → PLL/SPM resources → idle-host 检查 → AP-only FIFO隔离/bypass/pins。
  忙 MD/ADSP、残留 XOFF、未停止的 AP IRQ/DMA 均拒绝，不强行复位运行中的 radio。
  AP UART 的115200分数计算、24M sample7/point3/fracdb,1来自8250；CMM不复制这些参数。
  MTK SW配置AP的EFR=0x1a、XON1/2=0x11、XOFF1/2/INB_WORD=0x13、ESCAPE_DAT=0xdb、ESCAPE_EN=1；
  START/STOP字符已从当前Android tty只读取得，launcher原件亦确认enum1设置c_iflag的MTK专用位。
  8250参考原件的SW分支与源码一致；escape与XON/XOFF由UART硬件处理，不二次软件转义H4或下载原始流。
  只启用PIO的RDI/RLSI，屏蔽无handler的XOFF/RTS/CTS IRQ；baud/bank/IER修改与真实RX IRQ共用锁。
  失败回滚还恢复XON/XOFF/INB_WORD bank，不只恢复DLL/DLH/EFR。**实现已编译不代表24M PIO已上机验证。**
- **SSPM/CMM 移交**：Android generic CMM 24M config 是 TODO，不能据此给 CMM 写 AP 参数。
  当前离线材料包含活跃 slot 的 2 MiB SSPM 原件、ID 0/1/99 与 BL2 loader 结构，以及 Android
  启动日志中的 hub 208 MHz/mux 2 与 dev0/CMM 配置。post_download 只在当前 request-ready、
  resources、208 MHz mux、CRC 与两端配置全部匹配后恢复 multi-host；非生产 SW 流控、inband
  或任何配置不符仍 `ENOTSUP`。log 425 的 capabilities/FE01 证据不替代 CMM 移交、sleep/resume
  或活动连接 suspend 的独立验收。
- `x9_bluetooth.c`：将 session/下载器接入 worker；XO必须读取已有7个boot args且record长度25，不使用默认校准。
  UART3真实SPI0x148+32的IRQ收包、32KiB原始ring、worker WMT完整帧与opcode选择、保留无关包、实际partial TX。
  不宣称这是Linux APDMA，也不启用没有resume callback的wake-enhance。失败先关自己的IRQ、复位BT、按lease清理。
  UART IRQ hook同步存于overlay与 `x9/xnu/0006-x9-bt-uart-irq.patch`；没有修改共用XNU源树。
- `bluetooth_board.py`：171 B production `sku/hw-0` BT connfem命令，3 B原顺序pinmap，七个必需flags；
  私有 `x9_bt_board.inc` 不入git。`x9_bt_session.c`严格执行 ROM→24M→XO→下载/connfem→真实FW flags→routing→ownership→WMT function-on→有效controller配置。
  function-on只接受完整opcode6 ACK；仅明确busy=2才50ms退避，超时/解析/TX失败不重发，busy耗尽不假报成功。
  UART额外calibration callback在Android中实际为no-op，不能将ownership替代function-on。
  **已取得当前加载的HAL/vendor库及有效校准对象的NVRAM成员**，确定MT6993选择MT6653的24项preload表；
  原件优先读取 `/vendor/odm/etc/bt/BT_FW.cfg`，才fallback到 `/vendor/firmware/BT_FW.cfg`。
  ADB恢复后已补得优先配置、实际properties及完整6448 B btinit。优先配置末行**实际启用FC37的A2DP dynamic min power**，
  不是注释或空配置。countrycode/country-NV/SSPdebug/PIP四项实测为空，RAT为N/Lf/Lt/W/G。
  原件构造器离线分析已遍历24项，17条构造命令、2个property skip、5个内部无命令路径；这不是实机回包或生产输入。
  `bluetooth_vendor.py`现按原ELF指令正式移植构造器，当前输入生成17条命令+优先cfg末行FC37，接入session。
  构建只读固定 `.local/x9/bluetooth/android-mt6653/` 的原ELF、完整btinit、properties与优先cfg，
  不依赖Unicorn/Capstone/离线emulation JSON。ELF版本、24项表顺序、平台/chip/type、配置字段与已解析TX limits均严格校验。
  BDADDR反序、radio的signed-byte长/短分支、功率表96+6+96+6交错、sleep literal尾部覆盖均按原load/store实现。
  尚未移植的可选表/属性分支在构建时报错，不会静默跳过；缺输入的passive stub在runtime供电前ENOENT。
  session供电前校验**全部**packet，再在function-on后顺序等待每条真实Command Complete/status0，记录失败step/opcode/status，
  无模糊TX重试，最后确切FC37 append不可缺失。controller_config不再是固定ENOENT；log 425已观察完整capabilities，
  但搜索、配对、连接、睡眠与音频仍须各自验收。
  不将注释中的VENDOR_CMD或源码示例 `bt.cfg` 当作生产配置。
  `uart_launcher` 的 `HCIUARTSETBAUD` 保存用户所选配置；node==1的LOADPATCH是no-op，下载在pre_open。
  `bt.cfg` loader只编入node==0路径，不能把node==1的生产HAL/vendor流程错误归成“等待bt.cfg”。
- 普通HCI vendor命令现在有独立16-bit opcode的Command Complete/Status匹配路径，与WMT E4的一字节opcode分开，
  只由完整初始化worker独占；不开放btctl注入。`x9_bt_session_vendor_command`严格检查完整H4长度/opcode和实际status；
  Command Status不能冒充完成，失败/partial TX/timeout不重发，不复制Android原common回调忽略Status错误的缺陷。
- `x9_bt_download.c`：完整52 B section spec、首个匹配type `{0,2,3,0x80,0x90,0x10000}`、type3后connfem、最后bin index3；
  raw firmware1988 B单元或WMT ACL phase1/2/3，所有写入与drain有界，无模糊TX失败后重发。
- `x9_bt_serial.c`：mobile501/0600、独占open、阻塞read/write/select、64-bit termios、16KiB RX/TX FIFO与generation。
  native HCI流只在完整初始化之后接入，不与bootstrap IOUserClient共用命令队列。
  native TX现在claim完整H4帧（command/ACL/SCO/ISO），不发送只有header的半帧；保留队列计数直到真实UART drain完成，
  finish在同一锁内发布错误和唤醒，避免TIOCDRAIN把失败误报成功。TX在wire上时close拒绝立即reopen；
  并发write调用有独立锁，uiomove不会使两个payload的512 B块交错。
  RX整帧原子入FIFO，暂时满则worker保留整帧；真实IRQ overflow仍明确失败，不切断H4尾部。
  worker独立8192 B上限覆盖实测4096 B ACL payload+5 B H4头（旧bootstrap4096上限不够），不修改metadata ABI/控制器MTU；
  incoming/TX缓冲放静态sole-worker存储，不在kernel stack分配8KiB frame。保留pending帧依序交给native通道。
  无新字节的半帧也会超时；仅帧边界按Android node==1 raw_filter处理0xff/0x00，不在payload中过滤、不套node==0的0xf0规则。
  私有 `_IO('X',1)` 只取消阻塞 READ，供原生 close 在 BSD fileproc_drain 之前唤醒 reader；
  不取消 TX、不发送 HCI、不改硬件，reopen 清除 cancel。select 同样报告取消，drain 在队列为空时仍检查真实 worker 错误。
- 明确 sleep/resume 已接入 sole worker 和 serial admission。sleep 的完整 WMT ACK 拒绝为 `EBUSY`，保留 driver-own；
  成功后实际 drain、mask 自有 UART IER/GIC，再按 Android 释放 host-awake/TX/RX requests，不释放固件供电 lease。
  resume 先 TX request/SSPM resources/host-awake，保留 SPI route、pending 与 FIFO，再仅在 DEV0_BT_AWAKE=0 时发送一次 raw FF，
  有界轮询 CMM/DEV0 awake，核对 own-clear ACK 后恢复 RX request；任何不确定阶段均不发布 READY。
  MT6653 raw FF 与 own-clear 的 ACK 完全相同：在提交 own-clear **之前**独立消费 FF 完整 ACK，
  缺 ACK/重复/畸形均失败，不用固定延迟或 BT_AWAKE 代替回复归属。不启用自动 idle sleep、wake-enhance 或活动连接系统 suspend。

## iOS桥

- `X9H4Transport.c`锁定20H392 arm64 UUID `AA9424E8-0648-34E2-B3CF-E6C793B9D471`。
  原件SHA256 `8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f`。
  原POSIX open实际是UART.SK/Skywalk；仅替换open为cu.bluetooth，复用原RxLoop/TX/close。
  H4 buffer先初始化再创建RX线程，完整board结果为0前拒绝open。
  close slot 在原件已确认的 RX stop 标志后调用 READ-only cancel ioctl，再执行未修改的原生 close，
  避免 socket→character vnode 转换后 `fo_no_drain` 导致 blocked read/close 死锁；四组 stop/exit 指令先校验。
- `X9Vendor16.c`仅在显式 `X9_BT_VENDOR=MT6653`、正确UUID/指令与完整board结果为0时安装type0路由；
  null vendor ops返回原生NOT_IMPLEMENTED，不伪造BCM私有能力或成功；保留generic callback及supported-commands真实status。
  LE features回调的日志分支也保留入口status（x20保存，不把日志返回值当作成功），原生8 B mask存储不改。
- `X9Capabilities16.c`绑定真实supported commands/local/LE features，发布扩展扫描/广播、periodic、PAST、coded/2M PHY及广播数量。
  广播数量通过真实203b查询（软件容量上限5），回包前不发布部分能力；原provider once初始化后才同步缓存和标准HCI selector。
  日志分开报告controller实际数量和native分配slots，拒绝203b成功回复中超出0..0xf0的数量，避免MIN把畸形回复变成5。
  当前Android原始HCI捕获返回16；这只用于理解适配边界，iOS仍必须自己读取，不能把16硬编码进桥。
  HCI Reset保留真实失败状态，并清理各层能力；旧epoch的未完成203b不能推进新初始化。
  broadcaster启动时刷新构造阶段的legacy选择（本版本实例+0x88），不套iOS27布局。
  标准extended scan门为 `19a7e8→85a646`，取真实ext_scan；独立Apple VSC scan门
  `19a83c→85a645` 必须为0。原provider后续初始化与此一致，否则会进入空vendor ops返回0xe11。
  2041临时serializer的头不会原样发出：native queue按真实cursor提取payload，H4发送端重建正确
  `3+5*PHY_count` plen；无需修改该serializer。标准200d/2043及LE01/0a/0d事件保留原生路径。
- LE-support/LE-buffer/SupportedStates/FilterAcceptListSize/PAListSize初始化回调保留真实错误。
  后两种size是scalar，0不是NULL；SupportedStates才是8 B指针。成功继续原件cache setter，不伪造payload。
- CIS host-feature、subrating host-feature、default-subrating三个完成回调不再无条件返回成功；
  保留原件按真实LE feature/descriptor在**提交前**跳过不支持项的路径，已提交命令的真实失败则清能力并传给init sequencer。
  HostFlowControl完成回调也保留入口status，不让0x2cd日志分支吞错或泄漏logger返回值。
  四个回调原件均无payload/cache setter，桥不虚构参数或缓存；全部text改动先核原指令及容量。
  HostBufferSize 0c33声明的是不含4B ACL header的payload容量。原H4 RX固定包预算2032，
  ACL payload实际2028；现去掉原builder再加4的指令，不把serial_open的4096 TX staging误当RX容量。
  默认原HCI config的host flow为0；现于原HCI Init注册前clone完整12B配置，仅开启BR/LE ACL信用归还，
  SCO保持关闭。0c33声明2028B payload/1 ACL packet；先按iOS实际SupportedCommands逐项检查0c31/33/35，
  只有0c31真实status0且原模块保有该配置时才启用桥侧active标志，不拿配置意图当运行成功。
  信用归还复用原ID5f特殊marshaller和command-credit bypass，保留原blocked/count门禁，入队成功才清count；
  活连接零长ACL补同一归还路径。0c35入队、异步transport及标准0e/0f真实错误均进入原native fatal，
  不重发可能已上线的credit，不伪造Complete、pending或timer。Reset/vendorinit清active；六entry hook及
  config/queue/RX/error原指令contract在任何text修改前核对。协商与信用路径已接通源码，仍须iOS实机验证。
- `X9Bootstrap16`同时核对只读ABI1状态（READY/error0/FW-ready1/chip6653/baud24M）和 `kern.x9_bt_boot=0`。
  只在service尚未注册或真实INITIALIZING时等待；PASSIVE/FAILED立即保留失败。内核在serial boot结果后才发布READY。
- `X9BluetoothLaunch`保留mobile501身份，等完整board就绪后设置 `BT_HCI_TRANSPORT=H4:POSIX`、
  `X9_BT_VENDOR=MT6653`、`DYLD_INSERT_LIBRARIES=/usr/lib/X9H4Transport.dylib`，exec原件bluetoothd。
  不能由exec成功推断dyld注入/COW安装成功，必须实机验证。
- `X9BlueTool -R`实现原件 `com.apple.BlueTool` XPC契约，`RunBuiltinScript boot/init`明确调用 resume，deepsleep 明确调用 sleep；
  成功前验证实际 power/boot 与 resume 后完整 IO READY。失败result1、原始BSD errno另存x9_errno；不吞 EPERM、不合成manufacturer/firmware，未知命令拒绝。
  launcher 与无旧 FD/RX 线程的 H4 reopen 同样调用 resume，不用 boot-status 冒充睡眠。
- **未完成**：完整 vendor/session 的当前主线复验、CMM/SSPM 移交与 MTK SW PIO 的独立证据、
  Linux/HW backend、sleep/firmware-own/resume 实机验证、实机 COW 与能力/缓存路由、Settings 可用性、
  扫描/配对/BLE 与 A2DP/HFP/SCO 音频验收。当前不安装或修改 root 上的 launch plist。

## 编译与只读核对

```sh
python3 -I x9/bluetooth/build.py --kernel-objects --out x9/out/bluetooth-native/<唯一名>
```

编译/adhoc签名 `btctl`、`X9H4Transport.dylib`、`X9BluetoothLaunch`、`X9BlueTool`；单独编译6个BSD对象、UART IRQ hook及2个IOKit owner对象。
使用常规BT/Wi-Fi私有生成器；不链接完整kernel、不打包boot、不测试、不安装、不操作设备。
输出 `build.json` 保持 `stageable=false`。
完整显式 init 构建拒绝重复 selector，要求 `x9_bt=1 x9_bt_init=1 x9_bt_fc=1` 和全部原始 firmware/connfem/vendor 输入。
并行构建可用 `x9/xnu/build-xnu.sh macos --isolated x9/out/<唯一名>/dxb`，不修改共享 `.local/dxb*/xnu`。
`x9/bluetooth/package.py --runtime <完整userland输出> --out x9/out/<新名>` 只生成离线 root payload，
保留原始 jobs 与只改 Program 的候选，**不安装，不写设备，不等于原生运行验收**。

正式完整初始化/启动整合候选（不是功能验收或 root 安装）：

```sh
python3 -I x9/bluetooth/candidate.py --name <全新简单名字> \
  --config x9/out/bluetooth-native/xnu-lifecycle-20261115/local.json \
  --boot-config x9/build/boot-device.json \
  --boot-arg x9_bt_init=1 --boot-arg x9_bt_fc=1 \
  --launchd-source x9/out/bluetooth-native/launchd20h392-input \
  --launchd-cache-source x9/out/bluetooth-native/launchd20h392-input/launchd.plist --jobs 8
```

同一次编排完成全 overlay kernel、用户态/jobs及权威launchd cache、离线 root payload、一次 boot.img；全部 exit0 后才发布
`manifest.json` 的 `built_complete/candidate_ready=true`。`runtime_verified/stageable/root_installed` 仍为 false。
失败保留日志、无完成 manifest、如已生成被拒绝镜像则改名 `.failed`，不能交付为完整候选。
**boot.img 本身不安装 iOS 根盘中的桥或 jobs**；root 修改先确认、核对原件及 live root preflight，用户手动 boot 后才验原生功能。

### 首次启动由内核安装（推荐，替代 Android 侧定点写）

`--boot-arg x9_bt_root=1 --apple-originals x9/out/bluetooth-native/apple-originals20h392-stage`
让 candidate 先建用户态/root payload，再由 `root_header.py` 把全部文件（约1.04 MB）嵌入内核
（gitignored `x9/kernel/generated/x9_bt_root.inc`），然后编译内核和 boot.img，并额外生成
`boot-rollback/boot.img`（同一内核，`x9_bt_root=2`）。PID1 在原 launchd 读 cache 之前
（`x9_bluetooth_root.inc`，与 torch/camera/HAL 安装器同一位置）：

1. 核对 `/usr/sbin/bluetoothd`、`/usr/sbin/BlueTool` 与构建原件一致，嵌入数据 SHA 正确；
2. 先把全部目标分类（已安装 / 待装 / 未知），任何未知内容即整体不写；
3. 四个新文件：temp→fsync→校验→`link` 发布（不覆盖已存在目标）；
4. 两 jobs 与 launchd cache：当前必须是 Apple 原件，先硬链接保留 `<path>.x9-orig`，
   新文件校验后 `rename` 原子替换；cache 最后替换，中断后 launchd 总拿到完整新或旧 cache；
5. 日志 `X9 BT ROOT:` 是唯一 receipt。已装好的再次启动只校验不写。

升级：`--previous-payload <已装到设备的旧 candidate>/root-payload`（可重复）让内核把内容等于旧自有版本的文件
原子 rename 升级（被替换的 Apple 文件要求 `.x9-orig` 已核对存在）；其它未知内容仍整体拒绝。

`boot-rollback/boot.img` 反序恢复：cache 先回 `.x9-orig`，再删四个自有文件；不需要 Android。
不读整个根盘、不 macFUSE/COW、不需要 Android 侧 owner 修正（内核以 0:0 创建）。

`stage_targeted.py` 是当前许可下的定点 root 安装器。用户禁止全盘读/备份：不运行旧
`stage.py --apply` 的 whole-root snapshot/clone/fullhash。私有 macFUSE RPC 只读取真实必要块，
原生 HFS 的所有修改先入本机 COW；detach 后定点修七文件 owner、冻结并重验全部读依赖，
再按现有 pinfile OLD-SHA compare-write。只保留必要块缓存和变化块回滚前像。
最终验证真实七路径/bytes/mode/owner、Apple 原件与 clean journal；未知/torn block 拒绝覆盖。
具体命令、预算、资源保留与恢复约束见 [STAGING.md](STAGING.md)。安装许可不提升 audio/runtime 验收状态。

已确认的音频边界：20H392 type0 VSC support 表全0，`"shci"` source2 的 PCM 路由操作会经
`BD_VSC_WRITE_PCM_PARAM` admission 返回 unsupported，不更新原路由状态；上层继续，不证明它阻断初始启动。
Android 已有 FC72 voice-interface 初始化，不是标准 `WriteVoiceSetting`，其常量不证明 HCI/I2S 路由语义。
不能把 type0 unsupported 当作音频成功，也不能直接发 BCM 私有命令或伪造 route state；SCO/WBS 等价桥仍待真实证据。

当前活动 `btmtk_uart_unify.ko` 已证实：所有 SCO（不只 VTS）在 UART 上使用 MTK 头，
`03 AA BB LL payload` ↔ `03 AA (LL+2) (LL+1) BB payload`。
worker 仅在 UART 边界转换；native serial、pending 与背压队列始终标准 H4。
RX 先核两长度关系与整帧再转换；TX 在额外 wire byte 已真实 drain 后才退原 native claim。
payload 上限251按原模块声明 maxlen255/四字节wire头的安全预算设置，**不是实测固件拒收界限**；
真实控制器已报告 SCO244。长度回绕或畸形帧 fail closed，硬件SW转义不再软件二次编码。
这项修复是传输适配，不证明 HCI voice-route、WBS 配置或 HFP/SCO 音频验收成功。

### HCI 与 PCM 是两条独立链

- `X9H4Transport` 只复用 Apple 的 H4 HCI open/RxLoop/TX/close；kernel serial FIFO 也不是 codec。
  UART SCO 长度转换只保证 H4 wire 形状，不产生或消费 PCM。
- 当前 X9 `AudioServerPlugIn` 只有 Speaker/Hawking，没有 X9 编写的 Bluetooth PCM consumer。
  AP↔DSP/AFE ring、`3D00` 或 FC72 都不是 Bluetooth PCM 路由证据。
- 20H392 原生 `bluetoothd` 仍有 XPC audio publication、A2DP start/stop/data、HFP SCO completion、
  HFPAudioDevice 与 HCI send-thread 等候选消费者；静态符号存在不证明它们在 X9 上成功实例化。
- Android traced HFP 使用标准 HCI `0x043D`，input/output data path 位于 payload offset 50/51。
  FC72 是 MTK voice-interface 初始化，不是 per-codec route，也不能替代标准 HFP 状态机。
- A2DP 最小验收链：真实 controller/ACL → Apple A2DP 初始化与编码器 → start/stop/suspend →
  media payload → 对端实际声音；HFP 最小验收链：SCO/voice-setting → `0x043D` 与连接状态 →
  双向 PCM callback → 对端与 X9 实际声音。
- **PCM consumer ABI 尚未建立（2026-11-26）**：现有静态证据已闭合标准 HCI 的
  SCO send/receive 与 Android `0x043D` data-path 链，但没有建立 stock
  `HFPAudioDevice` 的 XPC selector、请求/回复序列化或 audio-device publication
  descriptor；符号存在不能证明 X9 上实例化。host 测试直接编译真实
  `X9AudioPlugIn.c`，确认 `DeviceList` 仅发布 Speaker/Hawking，Bluetooth/
  HFPAudioDevice UID 翻译为 unknown，未知对象的 StartIO/StopIO/DoIOOperation
  全部 fail closed。测试不初始化 IOKit、不模拟 callback 或 route。
- 实现真实 consumer 前仍缺的合同是：稳定的 Bluetooth device UID/transport/
  input/output stream 与 ASBD/timing publication；Start/Stop/zero-timestamp 与
  ReadInput/WriteMix 的 buffer 生命周期；把双向 AP PCM 实际绑定到 SCO TX/RX 的
  codec/ring supplier；以及 stock XPC peer 的真实实例化证据。kernel serial FIFO
  和 UART 长度转换均不能充当该 supplier。
- 下一离线步骤是从既有原件/捕获追上述 XPC selector、序列化和 publication 字段，
  先证明首个消费者合同，再决定是否需要 X9 PCM endpoint；不得伪造 route state、
  PCM callback 或 audio_verified。

`python3 -I x9/tools/bluetooth_vendor.py --required` 是纯标准库的原件移植构建入口，生成gitignored
`x9/kernel/generated/x9_bt_vendor.inc`。`build.py all`也接此生成器；显式`x9_bt_init=1`时要求完整原件，
不能从零输入生成“成功的空vendor初始化”。私有生成物包含地址/校准，不入git、不打印参数。

可选 `--launchd-source <仓库内原始20H392 plist目录>` 仅生成启动候选配置：
须同时提供 `--launchd-cache-source <仓库内原始20H392 cache>`。
先校验两个原job的Label/mobile身份/ProgramArguments/MachServices，再仅override `Program` 为
`/usr/libexec/X9BluetoothLaunch` 和 `/usr/libexec/X9BlueTool`，保留原arguments、MachServices、KeepAlive、events及其它字段。
输入原件副本在输出 `launchd-originals/`，派生件在 `launchd-candidates/`；两者不入git、均未安装，
不是自动可stage或已验收包；完整候选只可在明确 root 许可后由上述安装器安装。
这版launchd正常启动会优先使用 `/System/Library/xpc/launchd.plist` 的cached job，独立plist不能覆盖它。
`launch_cache.py` 仅append六个对象（两Program值、两job及两父dict），保留所有其它records、原707339B
object region、22269个IDs/offsets及2B ref width，不全量重排。原cache796447B，派生cache798777B；
内核Siri仅接受两种精确size/trailer并继续核全部32个原string guards。package/stage同时备份、比较并安装
第七文件cache，拒绝旧六文件payload；原cache必须与live root逐字节相等，不删cache或.sig。

未来工具正常交付后仅有只读metadata入口：

```sh
btctl --status
btctl --sections
sysctl kern.x9_bt_boot kern.x9_bt_stage
```

log 425 已提供 HCI Reset 后 capabilities 与厂商事件过滤的历史证据；当前主线仍须复验完整初始化，
并单独验收扫描/配对/BLE、A2DP/HFP/SCO。不得把这些历史观察外推为当前设备搜索或音频成功。

## Android在线原件取证与离线解码

设备由用户打开蓝牙并接好ADB后，只读采集到**全新**gitignored目录（默认要求只有一台online设备，不打印序列号）：

```sh
python3 -I x9/tools/bluetooth_capture_android.py --out x9/out/bluetooth-native/<新采集名>
# 可追加 --hci /data/misc/bluetooth/logs/<已确认的原始HCI文件路径>
python3 -I x9/tools/bluetooth_hci_facts.py <原始.cfa>
python3 -I x9/tools/bluetooth_sspm.py <原始sspm分区.bin> --extract x9/out/bluetooth-native/<新提取名>
```

采集器解析实际加载的ELF导出符号大小与当前maps，只读取btinit这一已知对象（当前6448 B，含6136 B NVRAM及cfg尾部），
不转储整个进程。失败/连接断开不会覆盖先前原件，exec-out的exit0但内容为cat错误也不冒充有效ELF/firmware。
Toybox ps可能只显示basename，采集器另核对当前 `/proc/PID/exe`；完整对象跨入匿名`.bss`，按PT_LOAD+完整maps约束读取。
远端dd统计可能混入exec-out的stdout，因此在设备端仅抑制dd stderr并严格要求符号size，含统计的先前输入保留为.failed。
它不关闭蓝牙、不发HCI、不写debug寄存器、不启动其它程序、不重启、不写分区。
HCI解码器只发布同reset epoch内、匹配请求且成功的控制器能力/缓冲区回复，不输出地址、名字、连接内容或配对密钥；
SSPM索引/提取仅验证原容器，不把静态分段匹配当成当前LK移交成功。

### 433 的 H4 teardown 空跳转（2026-11-26，离线观察）

`~/433.txt` 的 194.615 s `bluetoothd` SIGSEGV，PC/FAR 为 0；LR
去 slide 后为 `0x1000f3b50`，前一条 BL 至 `0x1000e6aac`。
20H392 静态切片中，该函数读取 `0x100844850` 指向的 transport 表：
表类型非零时直接跳转 `+0x60`，不检查空值。`BT_HCI_TRANSPORT=H4:POSIX`
选择 H4 表 `0x100844228`，该表的 `+0x60` 原始为空。此表与
`X9Vendor16.c::vendor_init` 管理的 vendor 表不同。日志和静态切片
一致支持空回调跳转，但未捕获寄存器证实实际表身份。

对 stock `__text` 的直接 BL 检索有两个调用者：`0x1000f3b4c`
（433 栈）及 `0x1003ca25c`，均未直接消费返回值。尚未穷尽间接
引用及回调的完整语义，因此不能将全局填槽视为安全修复；也未确定
为何发生 teardown，或其是否与更早的灰色开关有关。下一步优先限定
首个调用点、调用时 transport 与生命周期，谨慎设计诊断或局部 guard。
单独 kernel 更新不能更换已安装的 H4 dylib；root 安装须另行授权及
确认确切 predecessor 与 preflight。未声称扫描、配对或设备修复。
