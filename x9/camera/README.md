# X9 前后相机初步移植

**已实现控制层、RAW10 帧接口、iOS 双流桥、冷启动 sensor-probe/standby 配置、共享接收电源与 MM 寄存器时钟事务、SENINF/CAMSV 接收计划及带供电前置的寄存器预检；采集后端仍缺失，不能拍摄。**
没有假帧、测试图案、Android 代理或 Camera.app 成功状态伪造。默认关闭。

## 实现边界

| 层 | 当前实现 |
|---|---|
| 传感器 | Android 日志确认的 `hhmain_mipi_raw` / `hhfront_mipi_raw`；ID 检查、init/preview 表、stream-on/off、曝光/帧长、Sony 倒数增益与线性 Q10 转换 |
| XNU | `/dev/x9-camera` 独占控制；只读状态；16 MiB 上限 RAW10；1 s 有界等待；generation 拒绝旧 DMA 完成；CPU staging 在用户复制期间 pin，关闭时停止、排空、恢复 |
| 软件 ISP | HakiOS MIT 软件去马赛克、灰世界 AWB、AE、sRGB、NV12/BGRA；不使用高通 Chromatix；CCM 为单位矩阵，无 LSC |
| iOS | HakiOS `CMDerivedObjectCreate` 设备类 v3/流类 v2，前后 `FigCaptureStream`；输出池、PixelBufferHandler、帧 metadata；先确认真实 start 再返回成功 |
| sensor-probe | MT6993 CAMTG voter/parent 申请与释放、AP I2C 父时钟及单控制器门控/FIFO/WRRD；WL28681C 指定通道电压/启停；reset/MCLK pinmux；真实 ID 检查与恢复日志。闲置 AP timing 初始化、禁用共享 rail 的申请/还原、GPIO203 局部唤醒。**I2C 父时钟可有条件从 off 开启；HWCCF autolink/所选 PLL 仍需已就绪，未上机验证** |
| sensor-standby | 同一冷启动 journal 内，ID 匹配后经真实 AP I2C 写 init/preview 与镜像；前摄先做固定 fab 读端口识别，仅 B4/01 分支追加两项 init patch；读回稳定控制，然后 reset/恢复。mode3 显式命令；与未来 mode2 共用表验证/加载/读回。**不 stream-on、不做 QSC/LRC 标定；未上机** |
| SENINF/CAMSV 计划 | 零 MMIO 查询实际 CSI 端口、VC0/RAW10、候选 outmux/CAMSV、尺寸/64B stride/WDMA 打包及 SID；**不是资源申请或采集** |
| 接收电源接口 | TF-A MM-SMMU `PM_GET/PM_PUT`、CSI_BS_RX MM HWCCF 单 bit 申请/恢复；带供电前置检查的 SMMUv3 只读快照。**代码已构建，未上机；不是完整 MM power/clock provider 或 DMA mapping** |
| MM 寄存器时钟事务 | 显式 AO/infra0/VMM BACKUP 引用、CAM_VCORE/MAIN/MRAW 域、CAM autolink FENC、CAMTM/SENINF voter、CAM/CAMSV 寄存器 CG 申请/逆序恢复。**未上机；不改共享 AVS/CVFS/PLL，不申请 SMI larb，不证明采集速率** |
| 接收寄存器预检 | 持有完整 MM 寄存器时钟链时，读取目标 MAC、async/AFIFO、outmux0 outer/inner、CAMSV0 central/DMA/CQ 控制，双采样验证稳定及使能位关闭。**只读，不提交配置，不证明 ownership 或 DMA 排空** |
| 采集后端 | **缺失**：完整 DVFS/rate/SMI ownership、PHY 校准/速率、SENINF 路由提交、MM-SMMUv3 映射、RAW DMA 完成/排空队列 |

原始 X9 DT 与 Android 启动日志确定：主摄为 i2c4/7-bit `0x1a`，ID 寄存器 `0x16/0x17` 返回 `0x9186`；前摄 i2c2/`0x10`，返回 `0x0615`。
不把厂商宏中加了 offset 的软件 sensor ID 当成芯片读数。
当前模式来自厂商 preview：主摄 4096×3072 / C-PHY 3 trio，前摄 3264×2448 / D-PHY 4 lane，均标称 30 fps。

目前只向 D22 用户态提供 Photo-compatible 格式（4032×3024 / 3088×2320），不声明 60/120 fps、视频录制、HDR、OIS、马达 AF 或拍摄闪光能力。
光学字段沿用 D22 兼容描述，不是 X9 的标定参数；图像朝向、Bayer 顺序、曝光时序、实际帧率和色彩均待物理帧验证。

## 构建

常规 `x9/build/build.py kernel|all` 自动构建、ad-hoc 签名 iOS 16 arm64 的 `libx9-camera.dylib` 和 `cameractl`，内嵌自有/MIT 桥。
生成头被 gitignore，Metal 插件/mailbox 不变。

如要编入公开厂商 init/preview 寄存器数据，在私有 `x9/build/local.json` 添加绝对路径键 `camera_vendor_root`，指向：

```text
<OPPO modules>/vendor/oplus/kernel/camera/imgsensor/common
```

数据来源为 `oppo-source/android_kernel_modules_and_devicetree_oppo_mt6993`，main 表为 GPL-2.0-only/OPLUS，front 表为 GPL-2.0/MediaTek；生成文件保留其原始声明，不改许可证，也不提交到仓库。
缺少此键仍可构建，但每次生成明确不可用的空表，不复用旧生成内容。
表的源路径、SHA256 和寄存器对数记录在该次输出的 `camera/sensor-tables.json`。

单独构建用户态工具（不操作设备）：

```bash
python3 -I x9/camera/build.py \
  --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer \
  --out .local/x9/camera-tools
```

## 启动模式与 iOS 对接

在原有完整 boot config 上增加参数，不丢失 Wi-Fi/GPU/根盘/蜂窝参数：

| 参数 | 行为 |
|---|---|
| 不传 / `x9_camera=0` | 不创建设备节点，不安装/注入桥 |
| `x9_camera=1` | 创建设备节点，只能查询状态；不访问 MMIO/I2C、电源，不安装桥 |
| `x9_camera=2` | 允许硬件后端注册和控制；在 iOS 根盘创建 hash-addressed 自有 dylib，并为 mediaserverd 注入；**当前仍没有硬件后端** |
| `x9_camera=3` | 诊断模式，节点 root:root/0600；启动与状态查询不访问 MMIO、不上电、不安装桥；只有 root 的显式 sensor/standby/resource/MM-clocks/receiver ioctl 才访问硬件 |

模式 2 的首次设备运行涉及根盘写入，需要先确认；本模块没有自动 staging/flash/boot。
安装只创建 `/private/var/x9/camera/libx9-camera-<hash>.dylib`，校验内容与属性，不覆盖 Apple 文件、symlink、不同内容文件或 launchd plist。
现有 `DYLD_INSERT_LIBRARIES` 链保留，**不会合并/覆盖**；启用了 `x9_torch=3` 等已有注入时，相机桥会跳过并记录日志。
初步相机桥应单独启用，不能把多桥同时开启视为已支持。

桥只接受 20H392 CMCapture UUID `45d37488-2cc7-3028-8a4d-936bce7a220b` 和已核对的 vendor IMP。
`sharedCaptureDeviceVendor` 原返回非空时保持原值；物理后端没有通过两枚芯片 ID 验证时，不创建相机 vendor。
缺少设备、启动失败或无帧均返回不可用，不把字符节点存在等同于相机存在。
mediaserverd 的原始 launchd plist 使用 `mobile`（501:501）；writer 只接受 root 或该身份的 mediaserverd。

工具不自动部署。部署后由 root 手动使用：

```bash
cameractl status       # 无硬件时 supported=0、无 READY 是预期状态
cameractl back         # 要求真实后端；缺失返回错误，不产生假图
cameractl front
```

## 手动探测芯片，不启动采集

仅在带 `x9_camera=3` 的系统中，由 root 手动执行（本项目不自动部署或上机）：

```bash
cameractl status
cameractl probe back
# 主摄恢复成功后，可以单独探测前摄：
cameractl probe front
```

上述 `probe` 命令不会写 `0x0100`，不加载 init/preview 表、不启动 ISP/DMA、不安装或注入 iOS 桥。`standby` 是下面的独立命令，不能混为只读 ID 探测。
成功时 `probe_mask` 对应位与 `PROBED` 标志生效；**`supported_mask=0`、READY 不置位**。两枚 ID 都通过也不意味着能拍摄。
每枚 sensor 每次启动最多成功探测一次；任何失败锁存，后续请求不会重复硬件访问。工具返回非零退出码并打印决定阶段、blockers、芯片 ID 和独立恢复错误；`status` 可读取保留的最后证据。

当前实现刻意不接管未知的共享资源，前置条件包括：

- UNIVPLL 的已有 PCW/postdiv 配置在 `/104` 后为标称 24MHz（容差 ±1kHz）。不重编 PLL、不初始化 HWCCF 的全局/autolink 表；DONE 不就绪时有界退出。没有其他 XPU 投票且 CAMTG FENC 为 off 时，才允许把目标 mux 改为 parent7。已有投票则只接受正确 parent 和 FENC-on，不改 live parent。
- i2c4/i2c2 和 **i2c12** 的共享 I2C_EAST/I2C_PERI parent 必须 CKSTA-idle，选用 PLL 时该 PLL 已 FENC-on；不修改共同 source。已 FENC-on 且 PDN-off 的 parent 只借用。若 FENC-off、PDN-on、该分组**全部**物理子控制器 gate-off（IMPE mask `0x3f`、IMPC mask `0x0d`），两次入口快照及源 PCW 稳定，才记录 journal 并单 bit CLR 父 PDN，等待 FENC-on。随后只 ungate 目标 IMPE/IMPC bit，退出恢复自己增加的门控。所有暴露 bank 必须无事务/IRQ/FIFO 残留，所有 DMA bank 均未运行；主/前各 8 个 I2C、3 个 DMA bank，WL 为 4/1。TIMING 非零时仅借用经 divider/high/low/EXT 资格检查的 20–100kHz 标准模式、open-drain/非 HS 配置；TIMING 为零且 MULTI 为零时，可按已验证 parent PCW 推导标准模式 ≤98kHz 的 high/low sample、step、divider 和 5µs EXT，保存/恢复 14 个 CSR。不会 reset 整个控制器、改 MCU IRQ 或共享 shadow mode。实际 DT 没有 `ch-offset-i2c`，使用 channel0，不能猜成 `+0x10000`。
- AP I2C 引脚允许已为 mode1，或 mode0/input 后仅切目标引脚的 mode1，退出恢复；mode2/SCP、GPIO output 等未知 owner 一律拒绝。GPIO203 已 output-high 时只借用；output-low 或 mode0/input 且 DIN-low 时可记录 DIR/DOUT，先低→output→高，等 1ms 后读 WL ID。退出前必须读整个 WL enable 字节为零才恢复自己拉高的203；GPIO187/202 和 UVP 控制始终不写。
- SID3 LN60_7 DOVDD、主摄 SID6 LDC0_3 AFVDD：已启用时只借精确电压/normal/DA-enabled。禁用时还必须 DA-off/HW_OP_EN0=0，AF 另要求 EINT_EN=0，才能记录 selector，设置电压并单 bit EN_SET。DOVDD 的实际编码是 **0xd0=1.8V**（coarse 高 nibble、fine 低 nibble），AF 0x0a=3V；此前 0x0d 检查不正确。每次 snapshot→ownership→写入→readback 同持共享 X9PMIF 锁；模糊写入保留 journal。主摄仅申请 SID3 0x178a 的 VS mask4，最后撤销自己的 vote，不清其他 bit。
- 目标 sensor 的 reset 为 GPIO output-low，或 mode0/input 且 DIN-low（保存 DIR/DOUT 后可转 output-low）；MCLK pinmux 为 off、它的 WL 通道均 disabled。主摄使用 WL4/7/2，前摄使用 WL3/1；其他通道保持。

`blockers` 位：`1=clock`、`2=shared rail`、`4=AP route`、`8=I2C state`、`16=inherited active/reset state`。
阶段编号：`1=preflight`、`2=PMIC`、`3=bus`、`4=LDO`、`5=power`、`6=ID`、`7=restore`、`8=done`，追加 `9=clock`、`10=config`（不改旧编号）。
一次 AP FIFO 请求的完成等待使用绝对时间 5ms deadline，另有1000次轮询上限；配置、clock guard 和收尾不包含在这个5ms预算内。单次 PMIF 状态等待最多约 1ms，DA 另有最多20次/100µs有界采样；单次 HWCCF DONE 等待最多 10ms，AP EN 最多 200µs，CKSTA/FENC 最多 1ms。未知继承状态不清 IRQ、不做总线 reset/retry。
失败后先把 sensor reset 拉低、断开 MCLK，再按真实反向顺序撤 DOVDD、WL DVDD、AF/AVDD（前摄 DOVDD 在 DVDD 之后），恢复仅自己改过的电压/引脚/14个控制寄存器/单控制器门控，撤销 CAMTG vote 并在无人使用时还原 parent，最后撤自己的 VS vote。共享 rail 的 EN_CLR 确认/DA-off 后才恢复原 selector；已有 live rail 不动。继承投票和其他通道保持；恢复失败保留 journal、锁存并明确报告，不声称资源已恢复。

**恢复采用首错即停的依赖屏障**：reset-low 和 MCLK-off 均确认后，先检查 sensor/WL AP 请求 journal、配置和空闲状态，再允许撤任何供电；未退休 FIFO/START、wedged 或可见配置/parent 冲突立即止于 bus 阶段，保留 supply/pins/bus/clock/VS。任一 rail/WL 通道失败不再撤更早启用的 supply，也不恢复 pins/撤 bus、clock、VS。sensor bus 或 gate 恢复失败，不继续撤 WL bus、AP routes 或 CAMTG。PMIF pending/fault 先尝试本地 reset/MCLK 隔离，再保留依赖退出，不冒领后来响应。此前“累计错误但继续恢复”会破坏保留依赖，这一行为已修正。

GET_STATUS 仍为 **88B**，原 reserved 前两字现为 `cold_retained/cold_restore_step`；旧工具和 ioctl 尺寸兼容。它们是最后冷事务的**软件 journal 缓存，不是实际硬件状态或 lease**。保留 mask：`1=pins`、`2=MCLK mux/vote`、`4=sensor bus/gate`、`8=WL bus/gate`、`16=WL rails`、`32=DOVDD`、`64=AFVDD`、`128=VS`、`256=WL wake`、`512=AP routes`、`1024=PMIF fault/pending`、`2048=sensor I2C parent`、`4096=WL I2C parent`。restore_step：1=quiesce、2=DOVDD、3=WL（日志另给 channel）、4=AFVDD、5=wake、6=pins、7=sensor bus、8=WL bus、9=routes、10=clock、11=VS、13=PMIF、14=WL I2C parent、15=sensor I2C parent；成功时缓存 step=0。失败看 `X9 CAMERA COLD RESTORE`，不在同一 boot 重试。
**这补齐了冷启动供电/AP timing 的局部 ownership 和有条件 I2C 父时钟开启，但 HWCCF autolink/所选 PLL 仍要求已就绪；未知 HW/EINT 用户或 SCP owner 会拒绝。没有本轮实机 ID，不能宣称冷 LK 已全部打通，更不是可拍摄镜像。**

### 冷引脚按字段确认，不用入口状态覆盖后来的使用者

`x9_camera_pins.inc` 已接入真实 `probe/standby` 的 reset/MCLK、四枚 AP route、GPIO203 wake 与目标 MCLK drive。MODE/DIR/DOUT 分别记录 expected/prior/dirty/uncertain；每次写入前记 journal，读回确认后立即取消该字段的 prior 例外。只有尚未确认写的精确 prior 可供 cleanup 使用，第三值锁存冲突；确认后又变回入口值不再视为合法。借用 mode1 route、output-high wake 也检查三个字段，但没有 dirty 的字段不写恢复。

MT6993 厂商 `FLAG_RACE_FREE_ACCESS` 路径使用 MODE `+0xc` MWR（每 pin 的 bit4 为修改使能），不再整字 RMW；DIR/DOUT 与 IOCFG drive 使用 masked `+4/+8` SET/CLR。drive 仅处理目标 pin 的低三位，SET 单笔确认后才从新读数计算 CLR，模糊 SET 后不继续另一笔写，也不在 MCLK mode1 时调 drive。邻接字段保持，仍不是硬件原子 lease 或 ABA 保护。

POWER、每次 AP control/FIFO/START/完成收尾、WL guard 及 PMIC rail 变更前检查已消费 pin/drive。隔离分别尝试已知的 reset-low 和 MCLK mode0：一枚冲突不会阻止隔离另一枚已知 pin，未知字段不会被覆盖。reset 必须 MODE0/DIR-output/DOUT0 都可确认才允许撤任何供电；DIR 未确认为 output 不重新申请或补写来制造“隔离成功”。隔离失败停在 quiesce；其他 pin/drive 冲突在任何撤电前停于原 pins step6，保留依赖。

恢复只处理 dirty 字段；reset/wake 先保持低，再恢复输入 DIR，最后恢复可能为高的入口 DOUT latch，避免在输出状态恢复高 latch 形成脉冲。自己拉高的 wake 仍要求整个 WL ENABLE 为零后才恢复；借用高 wake 只核对并退休，不脉冲，也不要求无关通道关闭。旧 `cold_retained` 的 pins/wake/routes 位涵盖 borrowed references、dirty/uncertain/conflict，88B ABI 不扩展。`X9 CAMERA PIN CONFLICT/DRIVE CONFLICT` 记录决定差异，结束后的 `X9 CAMERA COLD PIN/DRIVE` 只输出软件 journal，不补读已撤电硬件。

源码反例（未运行测试，也非实机故障证据）：已确认 reset DIR1 后被改为 DIR0，旧 restore 直接写 DOUT0 并读回成功，不能证明输入 pin 实际拉低；mode1 route 确认后被改回入口 mode0，旧 restore 仍可当作入口例外。现在拒绝上述变化并保留供电/clock/bus，不盲写恢复。**没有本轮实机 ID/standby 或合法 SID80 mapping/RAW10，不能据此宣称冷启动或拍摄完成。**

### I2C 父时钟遵循厂商 PDN/FENC 路径，不冒领 CAMTG voter

`x9_camera_i2c_clock.inc` 接在现有 `probe/standby → cam_board_prepare → cam_bus_take`，不是等待注册的后端。厂商 A0/B0 两表均把这两个 mux 定义为 `MUX_GATE_FENC_CLR_SET_UPD_CHK`，实际 enable/disable 只写 CKSYS `+0x118/+0x114` 的 PDN 位：EAST bit23、PERI bit7。**不发 UPDATE、不 reparent、不伪造 HWV vote、不写 PLL。** FENC `+0x278` 的 bit29/31 是 on，CKSTA `+0x338` 的 bit27/29 是 busy。

自己的父时钟持有期间，每次控制 CSR 校验（含 timing 写、FIFO 前、START 前及完成后）检查完整 mux byte、源 PCW、FENC 和所有 sibling gate；变化立即停止后续写入。恢复须先完成两条 bus/timing/子 gate 及 AP pins，再按 WL parent→sensor parent→CAMTG→VS 逆序撤依赖。借用的父时钟不 SET；自己的 CLR 模糊写仅允许精确入口/预期 byte，任意第三种值或子 gate 被其他用户开启便保留，不关未知使用者的时钟。SET/FENC 恢复失败同样保留，boot 内不重试。

这只能检测可见的配置冲突，不能消除检查与写入之间的 AP/SCP 竞态，也不是长期硬件 lease。`X9 CAMERA AP CLOCK` 为申请结果，`X9 CAMERA COLD CLOCK` 为事务结束后的 journal；后者不重新读取已撤供电的硬件。mode/用户 ioctl/READY 不变，仍需真实 ID/standby 及 SID80 mapping/CSI RAW10 实测。

### WL28681C enable 只更新一个通道，冲突时不撤共享供电

厂商 regulator 的 enable_mask 是 `BIT(channel-1)`；芯片 `0x03` 的实际写入仍是一个共享字节。此前 X9 按整个 main/front mask 允许差分，可能把两次读取之间同组其他通道的变化覆盖。`x9_camera_wl.inc` 现接入 cold `probe/standby` 的实际启停：独立保留已预期 enable 字节、最后写前字节、唯一未确认通道 bit；普通 selector helper 不再接受 enable 寄存器写入。

源码反例（不是实机测试）：main mask `0x4a`，准备把 channel7 从 `enable=0x08` 设为 `0x48`；随后读取到 `0x0a`（channel2 新启用），旧 mask 检查仍允许写 `0x48`，会清掉 channel2。新路径在受控 enable 与 journal 不符处返回 `EBUSY`，不发该写入。

每次 enable 更新从最后一次读取的字节仅改目标 bit，保留非目标通道；相机控制组的任何其他 bit 必须仍符合 journal。起始目标通道全部 off 的既有约束不放宽，不清借用通道。未知其他组通道的变化不回写入口值；若写后整字节读回不一致则锁存冲突，而不是宣称启停成功。模糊 SET 仅在 cleanup 时接受该 bit 的精确 prior/expected；如果确认仍 off，退休此次 SET journal，不再发冗余 disable。

在启用目标通道前、启停时、interleaved AFVDD/DOVDD 申请前、MCLK/reset 放行前及恢复共享 rail 前，核对实际已申请且仍 enabled 通道的 selector 是自己的目标电压。发现 enable 或 live selector 变化便保留，**先 reset-low/MCLK-off 隔离，再停在 WL 恢复屏障；不先把 DOVDD/AFVDD 降下去，也不先清 enable 才发现 selector 已被改**。`X9 CAMERA WL CONFLICT` 给首次决定字段，`X9 CAMERA COLD WL` 仅输出最后软件 journal；ioctl/结构尺寸/READY 不变。

这修正的是可见变化下的单通道 RMW 和恢复依赖顺序，仍不能消除最后一次 read 与整字节 write 之间的竞态、识别同值 ABA 或证明 SCP 硬件 lease。未上机、不 stream-on，不能据此声称共享供电实测通过或已经拍摄。

### DOVDD/AFVDD 冲突须在撤任何相关供电之前发现

`x9_camera_rails.inc` 替换原 cold `probe/standby` 的 PMIC rail 申请/恢复实现，不注册采集后端。selector 和 enable 分别记录当前预期值、精确写前值及未确认位；成功写入不再把入口值当作任意时候都可接受的恢复分支。借用的已启用 rail 也记录 `taken` 引用，供电期间检查目标电压、EN/DA 正常模式与入口 HW_OP_EN/EINT，退出仅退休引用，不发 CLR 或恢复电压。

确定的源码反例（未执行测试、不代表实机发生）：DOVDD 入口 selector=0x80、EN/DA=0；相机已成功设 d0 并启用，随后 live selector 被改回0x80。旧恢复因为同时允许 target/entry，仍能清 enable；前摄还会先撤 WL DVDD，再检查 DOVDD。新 guard 在 live selector 不再等于 d0 时锁存 `EBUSY`，先 reset-low/MCLK-off 隔离，然后保留供电及 bus/pins/parent/CAMTG/VS 依赖，不发该 CLR、不撤更早 rail。

入口 snapshot、每次申请和最终检查、恢复共享屏障均持同一个 X9PMIF 锁；WL guard 在最终 fresh ENABLE 读取之前也检查所有已使用/未恢复的 PMIC rails，覆盖 interleaved 申请、WL 启停及 MCLK/reset 放行。恢复前检查整个已消费 PMIC 集合，不等到 reverse-order 后段才检查前摄 DOVDD/主摄 AFVDD。EN_CLR 确认、DA-off 和 selector/EN/HW/EINT 再检查通过后，才退休 enable journal；selector 仅在 EN/DA-off 时回写。

未确认写入仅在 cleanup 接受该字段的精确 prior/expected，第三种值或已确认字段后来变化一律保留；任何 enabled rail 必须仍为 target，不能借“未确认”允许 live prior 电压。未确认 SET 如果已证实保持原 off，不补发冗余 CLR；仍按原有有界 DA 检查退出，不延长请求预算、不做硬件重试。EN-on/DA 尚 off 仅在 cleanup 的未确认 enable 写入中作为可能 settling 状态接受；LP、EN-off/DA-on 拒绝。

`X9 CAMERA RAIL CONFLICT` 给首次决定字段；`X9 CAMERA COLD RAIL` 只输出结束后的软件 journal，不补读撤电后的 PMIC。原88B状态不变，retained 的 DOVDD/AFVDD 位也包括尚未退休的 borrowed reference，**不是独占硬件 lease 或当前电压证明**。这是可见状态冲突屏障，不能消除 AP/SCP 最后读→写竞态、同值 ABA、其他用户的同值请求或保证完整长期供电所有权。未上机、READY/supported0，不能据此称共享供电或拍摄已验收。

### 非零 AP TIMING 也必须通过标准模式资格检查

`probe/standby → cam_bus_take` 原来只检查非 HS、open-drain；非零 TIMING 不能证明本次 FIFO 事务的速度、high/low 或 START/STOP 设置可用。`x9_camera_i2c_timing.inc` 现在同时用于已有配置准入和零 TIMING 初始化，不增加诊断 ioctl、不修改用户态 ABI。

确定的源码反例（未执行测试、不代表实机状态）：26MHz source、TIMING=1、LTIMING=1、DIV=0、HS=0、IO=3，原借用分支会接受；high/low 各仅2计数，显然不是本次标准模式 profile。新分支在 bus 阶段拒绝，尚未发 WL ID 或 sensor 请求。

按厂商 `mtk_i2c_set_speed_v2` 解码 separate high/low sample×step 和复制的 5-bit divider。只接受标准模式 profile：TIMING/LTIMING/DIV/EXT 无未知位、两份 divider 相同、HS/SDA/MIS 为零、IO 为 open-drain、LS EXT 启用。根据稳定入口 source/PCW 求向上取整的配置频率上界，完整周期扣一计数（与厂商 LS solver 的额外一计数余量对应）、每个 phase 再保守扣一计数，要求 rate 上界 ≤100kHz、保守下界 ≥20kHz、high ≥4µs、low ≥4.7µs、EXT ≥5µs。20kHz 下限仅限定现有约5ms请求预算，不保证 clock stretching 一定在预算内。

零 TIMING 求解仍优选 45% duty、上界 ≤98kHz；EXT 改为完整表达式向上取整，避免二次截断使其短于5µs。编码在第一笔 timing 写之前通过同一 profile 检查，全部写后沿原 journal 再检查；已有非零配置不通过时直接拒绝，不重新定时、不清 FIFO/IRQ、不发 START，不修改已有 timing。原 dirty/uncertain 和恢复屏障保持。

`X9 CAMERA AP TIMING` 输出借用/初始化、raw CSR、source/rate **配置上界**和 errno；原 `COLD timing_hz` 现在也为上界（并补齐借用时此前为零的字段），不是测得的总线频率。资格只证明这个窄配置模型，不证明波形、rise time、stretch、AP/SCP 独占或真实传感器应答。继承400kHz/HS/其他profile会明确拒绝，不能借此称冷启动或拍摄已实测通过。

### AP timing/control 冲突时保留，而不是覆盖

14 个可恢复 CSR 现在分别记录入口值、当前预期值、最后一次写前值、dirty/uncertain bit。第一次 timing/control 写之前、每次 FIFO 请求、START 之前及完成后都核对配置；每次 CSR 写再核对该项写前值。共享 MULTI 必须保持零。检测到配置变化时记录 `X9 CAMERA AP CONFLICT`，不再写配置、清 IRQ/FIFO 或发 START；已填 FIFO 或已发请求的冲突另置 wedged，保留依赖。

恢复只回写实际尝试改过的 dirty CSR，不再无条件回写整个快照；没有变化的 CSR 不写。模糊写入仅在当前值等于本次预期值或精确写前值时允许单次恢复，第三种值立即止于冷恢复屏障。dirty 仅在恢复读回确认后清除。

这是一道配置冲突检测/恢复屏障，**不是跨 AP/SCP 的硬件 lease，也不能消除核对与 MMIO 写之间的全部竞态**。共同 provider/未知 owner 前置要求不变；没有冷启动实测时，不宣称 bus 独占或 AP timing 已验收。旧 ioctl、结构尺寸及 per-sensor 一次事务限制不变。

### AP FIFO 请求完成与撤电屏障

`probe/standby → cam_bus_transfer` 实际调用 `x9_camera_i2c_xfer.inc`。FIFO mutation 和 START **写前**分别标记 `fifo_owned/request_owned`，它们只是已尝试操作的 journal，不是硬件所有权。填完后要求 FIFO count 等于 tx 字节数；该寄存器定义来自厂商 common FIFO，厂商显式填充检查位于 SCP FIFO 分支，**本板 AP channel0 的行为尚待实测**。

任意非零状态都会结束轮询；只接受停止且配置稳定、其他暴露 bank/APDMA 空闲的 COMP/ACKERR/HS_NACK（bits0–2）。deadline 后看到的完成不算成功。ARB_LOST、CONFERR、RS_TRANSFER、未知位、仍运行的 START 或模糊 FIFO/配置直接锁存，**不清 W1C/FIFO/START、不发 resume/reset、不重试**。合格 NACK 以 `ENXIO` 返回，不冒充应答成功。

成功 read 必须恰有1个 RX 字节，write 必须无残留；RX 先放局部变量，只有状态、FIFO、START 收尾及最终完整空闲检查全部通过后才发布输出。收尾的 W1C 与 FIFO clear 后再次检查配置/其他 bank/START/新状态，失败立即停，不继续剩余清理。hot journal 只有确认完成收尾后才能退休；START 后来清零不能自行解除它。

冷恢复先隔离 reset/MCLK，再在 PMIC/WL/DOVDD/AFVDD 撤销前执行 AP 屏障；bus restore 与 parent restore 也拒绝 hot journal。`cold_retained` 原有 sensor/WL bus 位包含这些未退休状态，restore_step 仍用7/8，无 ABI 扩展。`X9 CAMERA AP REQUEST` 记录捕获状态和停止原因，`X9 CAMERA COLD AP` 仅输出最后 journal，撤时钟后不补读硬件。

多次 CSR 检查仍不是原子 lease，不能消除 AP/SCP check→write 竞态或同值 ABA。这一修复不建立 SID80/domain 映射，也不启动 SENINF/CAMSV DMA；**未上机、READY/supported0、无真实 RAW10，不能拍摄**。

### RAW10 复制与关闭不占住 DMA 完成锁

沿用 HakiOS `PLACE_READING/ios_readers` 的生命周期原则，但保留 X9 单份 CPU staging、原始 RAW10 ABI 和内存上限：完整 read 在锁内固定 header/raw/sequence 并 pin，随后释放相机锁执行可能 page-fault 的 `uiomove`。header peek 只复制局部快照。复制期间新 delivery 返回 `EBUSY`、丢弃该帧，不覆盖 staging，也不让 completion worker 等待用户页；提供者不得重放或占住硬件 slot 等待。

stop 先失效 generation 并按原顺序 drain 硬件，再最多等 1 s 排空 CPU reader；只有排空后才能复用/释放 staging。CPU drain 超时记录 `X9 CAMERA COPY DRAIN`、锁存 FAULT/RETAINED，close/另一 sensor 不重试。复制完成只确认其快照 sequence，不误消费更新的 `st.frames`；跨 generation 的完成报告 `ESTALE`。这不保证已经复制到用户空间的字节能被撤销。

**上述 pin 是 CPU 内存生命周期，不是 MM-SMMUv3 映射、CAMSV PASS1_DONE 或硬件 drain 的证明。** `x9_camera_deliver` 仍要求真实 provider 在 thread context 提交已完成、cache-visible 且调用返回前不变的 RAW10；SENINF/RAW DMA provider 仍缺失，READY/supported 仍为零，不能拍摄。

### 完成帧的曝光 metadata 不再从后来的寄存器读数拼接

内核采集接口升级为 **platform version2**，提交一个 `x9_camera_completed_frame`：sensor、generation、非零 capture_id、完成 time、raw/stride/bytes，以及该帧实际应用的 exposure_lines、Sony gain_code、frame_lines。`x9_camera_deliver` 不再执行 I2C 读；旧的“DMA 完成后读当前曝光/增益/帧长”不能证明这些值属于已完成的帧，因此不保留该降级路径。没有已注册的 MT6993 provider，升级不会移除现有可工作的硬件后端。

provider 必须先确认真实 PASS1_DONE/group/tag/inner sequence 与本次 owned slot/request 对应、检查错误并完成 CPU cache 同步，按 SOF/控制生效延迟绑定该帧 metadata。无法确认绑定时不交付；不能把最新 AP 命令、晚读寄存器或未资格化 IRQ 当作完成证明。capture_id 是 generation 内严格递增的 64-bit owned-request 标识，不是直接放宽/截断硬件 32-bit counter。

控制层只验证记录版本/保留字、generation、preview 精确尺寸与 64B stride、字节范围、完成时间、曝光/帧长步长和增益范围。重复或乱序 capture_id/time 返回 `ESTALE`；有效但被 CPU reader 背压丢弃的记录也退休，再提交同一记录仍为 `ESTALE`，不冒充新帧。用户 RAW10 `sequence` 保持接受帧计数，独立于 provider capture_id。iOS 在 allocation/ISP/metadata/PixelBufferHandler 前验证固定 preview header，并拒绝重复、倒序或未来时间帧。

**这实现了完成记录→RAW10 header→iOS 的绑定和拒绝路径，不是 CAMSV 硬件 completion、控制延迟、SMMUv3 mapping 或真实帧的实测。** 旧用户态 RAW10/ioctl ABI 未改，也没有新增诊断 ioctl；仍需真实采集 provider，不能拍摄。

### CAMSV 单 tag 的完成资格层

`x9_camera_camsv_arm/finish` 为真实 provider 提供 kernel-only 的 owned-slot 完成路径，不注册后端、不改变 READY，也不读取 MMIO 或新增 ioctl。arm 保存已绑定的 metadata、64-bit capture_id、预期硬件 inner sequence、tag/group 和 IOVA；必须有合法 domain/route/slot 的 lease 检查及 CPU cache 同步回调。16KiB 对齐及 16GiB dma-range 只是边界检查，不能证明映射成立；PM_GET、CPU PA 和空成功回调均不是 lease。

唯一 IRQ owner 必须提供一次**一致的原始快照**：outer DONE `+0x348`、inner ERR `+0x350`、CQ STATUS `+0x1c`、inner FIRST/LAST_TAG 与该 tag 的 FH_SPARE，group_tags 来自该帧 SOF 的有效 group 绑定。读清状态只消费一次，worker 不补读；快照的 generation/capture_id 来自实际 slot journal，不能由硬件 counter 猜出。

finish 优先拒绝错误；CQ setting-done 或普通 GP_DONE 单独返回 `EAGAIN`，不是帧。只接受目标 `SW_GP_PASS1_DONE`（bit16..19）、单 tag/group 及精确 inner sequence，outer 即使超前也不当作完成凭据。通过后重新验证 lease → sync_cpu → 再验证 lease → `x9_camera_deliver`，此前不读取 RAW。成功、CPU 背压或同步期间 stop 都退休 source；重复 finish 不重放。tag/group/序号歧义或 lease/cache 失败置终态 RETAINED，不 rearm、free、unmap 或撤依赖。重用退休 slot 时 capture_id 必须前进，32-bit inner sequence 只接受小于半周期的非零向前差值，允许 wrap 到零但不自动扩展成 capture_id。

返回码不能单独决定 slot 归属：`EAGAIN + ARMED` 才是继续等完成，`EBUSY + RETIRED` 才是 CPU 背压丢帧；lease/cache 回调也可能返回同样 errno，但此时是 `RETAINED`，不能重试或回收。终态保留最初失败 errno。

slot、raw、映射和回调 context 必须由 provider pin 到同步调用返回，capture(off) 必须先停硬件并排空这些 worker。**本层只实现快照资格检查、cache 回调顺序和交付路径，不证明 IRQ 快照来自硬件、SOF metadata 绑定、合法映射或实际 drain。** 目前没有调用它的真实 provider；SID80/domain 委派、rate/SMI/PHY/route、IRQ owner/采集启动仍缺，不能拍摄。

### CAMTG 时钟申请和诊断

按 OPPO `clk-mt6993.c` → `clk-mux.c` → `hwccf_api.c` 的 v1 AP 路径实现，不能沿用旧版的两个假设：

- `CKSTA +0x330` 对应位是 **busy**，必须等它清零；实际 on/off 查 `FENC +0x270`，不把 CKSTA=1 或 CLK_CFG 的 PDN 当作时钟已运行。
- clock 表的 HWV SET/CLR 是相对偏移。v1 `v1_raw_hwccf_voter_ctrl` 加 **XPU0 `+0x20000`**；SET 后检查 AP EN、DONE，再检查 CAMTG/UNIVPLL FENC。只写目标 bit，不直接写 PDN、PLL enable 或全局 HWCCF 初始化。

| 资源 | VLP parent 字段 / UPDATE bit | AP HWV SET / CLR / EN / DONE | vote bit / CKSTA、FENC bit |
|---|---|---|---|
| 主 CAMTG7 | `+0x70[27:24]` / `+0x4 bit27` | `+0x20048 / +0x2004c / +0x20050 / +0x12618` | `31 / 4` |
| 前 CAMTG1 | `+0x60[11:8]` / `+0x4 bit21` | `+0x2003c / +0x20040 / +0x20044 / +0x12614` | `15 / 10` |

VLP base 为 `0x1c200000`，AP HWV base 为 `0x1c040000`。共享 UNIVPLL base `0x1c213100`，CON1 `+0x0c`、FENC `+0x34 bit4`；只读。
probe v1 仍为 80B，把末尾四个 reserved 字用于 `hwv_vote`（入口 AP EN）、`hwv_done`、`pll_con1`、`pll_fence`；请求仍须全零。旧内核返回零，旧工具仍可读取前面字段。

### CAMTG 已确认配置不得回退为“模糊中间值”

`x9_camera_mclk.inc` 接入实际 cold `probe/standby`，不是新 provider。parent nibble、保留 bits4–6 和目标 AP vote 各有当前预期；CLR/SET 及 AP vote 写前分别记录精确 prior/uncertain，读回确认后立即撤销 prior 例外。UPDATE 单独记录 pending，以 CKSTA-idle 和预期配置核对收尾。**bit7/PDN 由硬件 voter 控制，仅观察，不写、不还原。**

旧恢复路径不区分 SET 是否已确认，始终接受 entry、parent0 和 parent7。源码反例（未运行测试/非实机状态）：parent7 已确认后，其他用户改成0，旧代码仍可能回写 entry；新路径在该变化处锁存 `EBUSY`，不撤 AP vote 或覆盖 parent。只有真正未确认的那笔 CLR/SET，cleanup 才允许其精确 prior；未确认 SET 已在 entry/AP-off 时不补冗余写。借用 AP vote 永不 CLR，aggregate vote、PLL 编程及其他 mux 字段不写。

每次 AP 控制 CSR 检查（包括 FIFO/START 和完成收尾）、POWER 入口、MCLK/reset 放行前，核对 CAMTG DONE/CKSTA、预期 parent/保留位、目标 AP vote 和同一 PLL CON1；活跃使用另要求 CAMTG/PLL FENC-on。冷恢复先做本地 reset/MCLK 隔离，再在任何 supply teardown 前执行该检查；发生冲突止于原 clock step10，保留依赖。最终 unvote/parent 恢复前同样核对，不等到撤供电后才发现源已变化。

`X9 CAMERA MCLK CONFLICT` 输出决定差异；`X9 CAMERA COLD MCLK` 仅为末尾 journal，`active` 是尚未退休的已资格化使用，不是 live FENC/独占 lease。原 retained 的 MCLK 位含未确认写/UPDATE/冲突，88B/ioctl/READY 不变。AP REQUEST 的“no further cleanup”表示故障后不再继续剩余清理，不否认故障前可能已执行的合格 W1C。

多次寄存器读取不能排除 AP/SCP check→write 竞态、同值 ABA 或同一 AP bit 的未知同值用户；CON1/FENC 也不是测频。本轮不建立 SID80/domain/IOVA 映射、不启动 SENINF/CAMSV DMA；**未上机、没有真实 RAW10，仍不能拍摄**。
`flags & 16` 表示 voter/CAMTG/PLL FENC 检查通过，不表示实测频率、更不表示 capture READY；`cameractl` 输出 raw 证据并明确标注标称配置。

## 冷启动配置 sensor standby，再复位恢复

仅在 `x9_camera=3` 的设备 **root shell** 中手动执行。工具来自该次构建的 `camera/cameractl`，不会自动部署：

```bash
cameractl status
cameractl standby back
cameractl status
# 主摄 last_errno=0、restore_errno=0 后，可以在同一 boot 单独配置前摄：
cameractl standby front
```

**每枚 sensor 在一个 boot 里选择 `probe` 或 `standby` 之一，不要先 probe 再 standby。** 两者共用每 sensor 一次成功/首次硬件错误锁存；任何硬件或恢复错误后不再访问硬件、不自动重试。表缺失/关键值不匹配在上电前拒绝，不消耗硬件事务；GET_CONFIG/status 保留缓存，重复请求不覆盖此前成功或失败证据。

新 GET_CONFIG/CONFIG_STANDBY（`0x9a/0x9b`）为独立 **720B** v1 ABI，请求只能有 version1/sensor1或2，其余全零；旧 ioctl 和结构尺寸不变。执行顺序：

1. 本地表长度/字节值/禁止寄存器检查，并检查最终 RAW10、尺寸、binning、PLL、行/帧长、曝光、增益、VC 关键值；不访问 MMIO。
2. 复用原冷启动/AP timing/共享 rail journal，读取真实 `0x16/0x17` ID。匹配后仍持有同一供电/总线，不先恢复再另行上电。
3. 读取 `0x0100` 与 `0x0104`，均为0才开始配置。前摄先执行下面的固定 fab 分支，再逐个 reg16/data8 FIFO 写 init/preview 和镜像（主3/前0）。**从不写 `0x0100/0x0103/0x0104`、OTP programming、EEPROM、AF/OIS**。生成器和内核仍拒绝 OTP 表项；仅已匹配前摄的固定 read-port 命令有独立例外，镜像也只能独立写固定值。
4. 按稳定控制白名单读回，主 **41** 个、前普通分支 **38** 个、B4 分支 **40** 个字节（额外两项 patch，包括再次检查 stream/hold）。不读回全部厂商 microcode/write-only 命令。每个 I2C 请求完成等待预算5ms（不含配置/guard/收尾），配置段总 deadline5s，调用前后检查预算；超时不重试、不继续 init。fab read-port exit 可独立尝试一次清理写、使用同样的完成等待预算，不延长初始化预算；AP hot/fault 时拒绝实际发出该写。
5. 无论中途写入/读回是否失败，均调用 `cam_restore`：先 reset-low/MCLK-off，再逆序恢复自己的依赖；任一恢复失败立即停在该屏障，保留 journal、FAULT/RETAINED，不在 close 或另一 sensor 上重试。

结果包括原80B probe、独立操作/恢复错误、成功 ACK/实际尝试写入数、决定阶段/寄存器/index、expected/observed 与逐字节读数；`observed=ffffffff` 表示没有被接受的读数，不能当硬件值。预算耗尽而未发出的写不计 attempted；成功 ACK 即使随后发现总预算耗尽也计 written，但仍返回超时。index 在 init/preview 阶段为表 pair offset，在 readback 阶段为 sample offset。阶段1..9不变：1=table-check、2=cold、3=idle、4=init、5=preview、6=mirror、7=readback、8=restore、9=done；追加10=fab、11=fab-exit、12=fab-patch。

### 前摄 fab 分支

依据公开 `hhfrontmipiraw_Sensor.c::Imx615_fab_read`，在芯片 ID 已匹配且确认 standby 后，仅写 `0x0a02=0x7f`、`0x0a00=1`，等1ms，单次读取 `0x0a01/0x0a1f/0x0a20`。ready=1 且 fab=`b4/01` 时再等2ms；尝试开启读端口后，**任何分支及错误都单次尝试 `0x0a00=0` 退出**，退出成功才加载 init，不把 I2C 错误冒充 fab 字节。厂商 ready!=1 分支未显式关闭读端口，本实现也会关闭，不复制这一遗漏。若总预算已过，不再进入 init；硬件 reset/供电恢复仍独立执行。

B4/01 分支在 init 后、preview 前仅写 `0x574b=1`、`0x5765=0x33`，并纳入最后稳定读回。静态 preflight 拒绝会无条件写入/覆盖这两项的前摄表。普通分支不写 patch；读取成功但 ready!=1 时按厂商 fallback 使用普通表，**不宣称识别到 B4**。此处是 OTP **read-port**，不读 EEPROM/QSC/LRC，不进行 OTP programming、page 扫描或重试。

新增 flags `0x80=FAB_READ`（三字节已读且退出写获 ACK）、`0x100=FAB_B4`（识别到 B4 分支，是否 patch 完成还须 WRITTEN/VERIFIED）。退出寄存器没有强行读回，ACK 不冒充额外硬件状态观察；事务最终还必须通过 reset/供电恢复。`X9 CAMERA FAB` 日志保留成功读字节数、raw ready/fab、原错误与独立 port_exit 错误；未读字节的零不能当结果。预期成功写入数：主930；前普通 **336**（333+三个读端口命令），前 B4 **338**（再加两项 patch）。稳定 read_count 不包括先前三个 fab 观察字节。完整成功 flags 主3f、前普通bf、前B4为1bf；这些是验收条件，不是已上机结果。

厂商模式的主要核对项：

| 项 | 主摄 | 前摄 |
|---|---|---|
| 输出 | 4096×3072 / RAW10 | 3264×2448 / RAW10 |
| `0342/0340` 行长/帧长 | 9696 / 7546 | 3768 / 7642 |
| `0202/0204` 初始曝光/增益寄存器 | 1000 / 0 | 7594 / 112 |
| sensor PLL `0306/030e` | `016e / 0226` | `010e / 0438` |
| vendor nominal pclk | 2196000000 | 864000000 |

标称 pclk、sensor PLL 读回或 `VERIFIED` **不是实测 lane/trio 速率、PHY 校准或 rate lease**。主 preview 包含 PDAF/额外 VC，写表不代表接收器已过滤为纯 VC0。前摄 fab 分支已接入，但 vendor open 中的主摄 AF 激活/QSC 与 QSC/LRC 标定仍未执行，因此只称 **未标定 nominal standby**，不能称厂商完整 open 或成像质量验证。blockers `1=factory/calibration`、`2=receiver`、`4=DMA mapping`、`8=drain` 不因读回成功清除。

成功后器件已 reset/恢复原供电，**配置不持久、READY/supported仍0、不能拍摄**。未来 mode2 的 start 共用同一 preflight/表加载/稳定读回，再交由真实 capture provider 建立 CSI/DMA，最后才 stream-on；目前无该 provider。

读用户本次日志与缓存（无需自动操作设备）：

```bash
rg -n 'X9 CAMERA (STANDBY|FAB|PROBE|COLD|CLOCK)' ~/N.txt
```

先按该次日志证明 ID→表写→读回→恢复的完整路径，再接 rate/SMI/SMMUv3 owner 与 SENINF/CAMSV，不把构建成功当作硬件验收。

## 查询 SENINF → CAMSV RAW DMA 接收计划

```bash
cameractl pipeline back
cameractl pipeline front
```

这两个命令以 `O_RDONLY` 打开节点，**不访问 MMIO、不上电、不写 sensor、不分配或提交 DMA**，mode1 也可用。
新 `_IOWR('C',0x93)` 使用独立128B ABI，输入只有 version1/sensor1或2，其他字段必须全零；旧 status/probe ABI 不变。
结果来自 captured DT/当前 ISP8s 寄存器描述，标为 source-only。候选 outmux0/CAMSV0 未被申请；缺 provider 不能 stream-on。

| sensor | CSI/async | PHY | RAW10 / stride / bytes |
|---|---|---|---|
| 主摄 | port3 / async3，base `0x13898000` | C-PHY 3 trio | 4096×3072 / 5120 / 15728640 |
| 前摄 | port0 / async0，base `0x13880000` | D-PHY 4 lane | 3264×2448 / 4096 / 10027008 |

- 当前板 outmux0..5 是 CAMSV、6..8 才是 RAW、9 是 UISP；不按 sensor index 猜 CSI port。
- tag0 的 outmux VCDT `+0x100=(0x2b<<16)|1`、首次 `_config_outmux → SET_TAG` 预期尺寸 `+0x104=((height-1)<<16)|(width/4*5-1)`。
  HSIZE 是 packed RAW10 **字节数减1**，不是像素数；后续 apply-v2 路径的 HSIZE 语义不同，不混用。
  候选 CAMSV0 base `0x3a600000`，DMA bank `+0x1000`：地址 low/high `+0x200/+0x204`，basic `+0x210=(stride<<16)|0x10`；不是旧 unit-test 的 TG 寄存器布局。
- shared `mm_smmu_cam` 是 MM-SMMUv3 `0x30800000` 的 **SID0x80**、35-bit DMA mask，但 DT camera dma-range 只有16GiB。
  `mtk_collect_smmu_dma_regions → invert_dma_regions` 将 `mm_common` 的两段**允许范围**取补集作为保留区：`[0,0xfc910000)` 和 `[0xfffff000,0x400000000)` 可用于分配，间隙 `[0xfc910000,0xfffff000)` 不可用于相机 IOVA。第二段还要按 domain granule 对齐（16KiB 时首个完整页在 `0x100000000`），并避开零页和现有分配。**这些范围不是现成空闲池或映射租约**；不能把 CPU PA 当相机 IOVA，也不能套 GPU 专用 SMMU bypass。
- 当前 `blockers`：1=该 sensor ID 未匹配且还原，2=MM power/clock ownership，4=PHY，8=路由申请/提交，16=DMA映射，32=完成/排空。
  只有1会在真实 probe 成功后清掉；其余 provider 尚未实现，不因计划生成成功变成 READY。
- 当前驱动 `mtk_irq_camsv_hybrid()` 用 central `+0x348` 的 **SW_GP_PASS1_DONE [19:16]**，active group/tag 和 inner frame sequence 判断帧完成；SCQ `+0x2000` 的 setting-done 不是帧。
  还须错误状态检查、cache同步、timeout隔离、stop/drain 后才释放 IOVA/buffer，再交给 `x9_camera_deliver()`。
- 尚需 port 对应 eFuse/PHY settle/速率、CKSYS_MM/CAM/SENINF/CSI_BS 的共同 provider；不从 source-only 配置推出实机已收包。

## 在 MM 时钟持有期间预检接收寄存器

仅 `x9_camera=3`、root、显式读写打开节点；**每 boot 选择 receiver 或 clocks 的一个事务，不先运行 clocks 再运行 receiver**：

```bash
cameractl receiver back
# 下一次用户手动 clean boot 可选择：
cameractl receiver front
cameractl status                 # 只读缓存，不重新触碰硬件
```

`PROBE_RECEIVER` ioctl `0x99` / cache-only `GET_RECEIVER` `0x98` 是独立 **1192B v1** ABI，旧接口不变。输入仅 version1/sensor1或2，其余全零。
执行 `cam_mm_take → 两次接收控制采样 → cam_mm_restore`，take 任一阶段失败便不访问接收寄存器；每次采样前再检查 PM_ACK、FENC、CG 与 parent。退出时仍逆序恢复并返回独立 MM restore_errno/retained。失败按已有共享资源规则锁存，本 boot 不重试；GET/status 不改变硬件。

地址由 captured DT 的 `reg-names` 与 `mediatek,camsv-id=0` 决定：

| bank | 地址 / 读取范围 |
|---|---|
| 目标 CSI MAC-top | 前 `0x13884000`，主 `0x1389c000`；CTRL、VERSION、PHY_CTRL、PRBS_GEN |
| async / CAM_MAIN AFIFO | `0x3a401000 +0/+0xc`，`0x3a0107dc`；只针对目标 port 比較 test/debug/split 掩码 |
| outmux0 | outer `0x3a200000`，inner `0x3a208000`；source、DL、ready/reset/config-done、tag0 尺寸/格式 |
| CAMSV0 | outer `0x3a600000`，inner `+0x8000`；DMA_EN、CMOS/VFDATA、8 个 FBC 控制、WDMA address/basic、CQ 控制 |

不读取中断/status-clear、帧完成、队列 producer/consumer 或 debug selector 寄存器；不写 PHY、async CG/IRQ、outmux、CAMSV、DMA/CQ/SMMU。上游 async CG/IRQ 的“always on”用于避免 AOV/SCP race，不照搬为整 bank 初始化。

结果 `flags`：1=attempted、2=两次快照完整、4=目标控制稳定、8=观察到被检查的使能/触发位关闭、16=本次 MM 引用已恢复、32=失败锁存。`busy`：1=PHY/PRBS使能、2=目标test-model、4=outmux DL/ready/config-done、8=CAMSV DMA/CMOS/VF/FBC、16=CQ reset/thread enable、32=MAC/outmux reset；`changed`：1=MAC、2=目标async/AFIFO、4=outmux0、8=CAMSV0。
**flags8不是 PHY 全部断电、独占、队列为空或 DMA 排空证明**。即使 errno0/RESTORED/retained0，owner/rate/PHY/route/SMI/map/drain 七项 blockers 仍全部保留，只有 NEED_SNAPSHOT 会清；READY/supported 不变。相机真实初始化不能在此基础上直接写配置。

上游 `iommu_attach_device_ssid → arm_smmu_s1_set_dev_pasid` 需要已存在的 default domain、STE/CD、命令队列及 IOVA allocator；`mm_smmu_cam` 节点没有单独的 SSID range，因此不能猜一个私有 SSID。secure `PM_GET` 只提供电源引用，`SEC_INIT`、`AID_SID_MAP`、HYP debug SMC 不提供相机 domain 租约。本地 UniXNU ArmSmmu4K 是 SMMUv2/4KiB，不能代替 MT6993 共享 v3/16KiB 后端。

## 硬件后端接入契约

`kernel/src/bsd/dev/x9_camera.h` 定义 `x9_camera_platform`：

1. `power` 按 sensor 配置的顺序执行，并记录/恢复共享资源的原状态；失败半途也必须可恢复。
2. `read8/write8` 是 16-bit register / 8-bit data I2C；只在上电及总线归 AP 后调用。
3. `capture(on)` 在 stream-on 前完成 SENINF、VC0/DT0x2b、RAW10 DMA、SMMU 配置；`capture(off)` 在返回前停止并排空，即使初始化只做了一半。
4. 注册会分别上电读 ID 并恢复，只有两枚真实 ID 相符才发布 READY。调用发生在线程上下文；所有回调都不可重入相机 API。
5. provider.version 必须为 `X9_CAMERA_PLATFORM_VERSION`（2）；version1 不接受。DMA worker 在真实 completion/slot/metadata 绑定及缓存同步后调用 `x9_camera_deliver(&completed)`，其中 completed 为 version2 的 `x9_camera_completed_frame`；不得在硬中断调用。generation 来自本次 `capture(on)`，time 为帧完成的 `mach_absolute_time`，capture_id 为该代已资格化 owned-request 标识。record/raw 直到返回都保持不变；EBUSY 丢帧并释放硬件 slot，不重放。记录验证不能代替 provider 的硬件、映射或控制生效证明。

控制层在调用 power/capture/stream 前记录 attempted 阶段及原始 generation；半途错误只清理实际尝试过的阶段。stop 先失效 generation/唤醒读者，再退出帧锁等待 capture(off)，因此完成 worker 不会被 start/stop 持帧锁堵住。**capture(off) 非零时不调用 power(false)**，保持映射/时钟/供电依赖并置 FAULT/RETAINED；power(false) 失败也保留 journal。后续 close 不重试硬件清理、不释放保留的 staging buffer、不切换另一 sensor。注册时的 ID 供电恢复失败同样锁存。非零返回不是已经恢复或可以重新开始，只有明确 drain 成功后才能撤依赖。

下一阶段不能绕过 regulator/SMMU 所有权或从 LK framebuffer carve-out 偷分配相机 DMA。
先证明一枚芯片的上电→I2C ID→还原，再证明 SENINF 包→RAW DMA 完整帧，最后验收前后切换、预览和落盘照片。

## 申请并还原共享接收电源（不启动 PHY/DMA）

仅在 `x9_camera=3` 下，由 root 显式执行一次：

```bash
cameractl resources
cameractl status       # 读取缓存的资源快照，不再访问 MMIO
```

这不是 `pipeline back|front` 的零 MMIO 计划：资源请求实际调用电源服务、申请 CSI 电源并恢复。
两枚传感器共用这一生命周期；不重复按前后相机申请同一份资源，不改 sensor-probe 的电源顺序。
请求 ABI 为独立 224B `X9_CAMERA_PROBE_RESOURCES`（version=1，其余全零）；`GET_RESOURCES` 只返回缓存。
旧 status/probe/pipeline ABI 不变，工具读取旧内核的缓存 ioctl 遇到 `ENOTTY` 时仍可显示旧状态。

具体顺序和成功边界：

1. 只映射虚拟 MMIO 窗口，尚不读未供电的 MM-SMMU/CSI 寄存器。
2. 调用 `0xc2000514`，`a1=0x803`（MM_SMMU/secure/PM_GET）。只有返回 0 才记录自己的引用；失败不调用 PM_PUT。
3. 验证 HWCCF 的 MM_INFRA_AO/infra0 `PM_ACK`（bank0 bits31/29）且对应 `STA=0`，之后才读 MMUP 的 GPR11 dummy register。
   前置不足返回 `ENXIO` 并平衡 secure 引用，不擅自触发 VMM backup IRQ 或初始化其固件处理链。
4. CSI_BS_RX 使用 **MM HWCCF bank1 bit11**：XPU0 SET/CLR/EN 为 `+0x2070c/+0x20710/+0x20714`。
   按 `STA=0 → FSM idle → SET → AP EN → global EN → STA=0 → PM_ACK=1` 申请新增 vote。
   `DONE` 只作证据，**不能套 AP CAMTG 的 DONE 判据**。已继承 AP vote 仅借用；未知未投票但上电状态拒绝接管。
5. 在 secure 引用内读取 MM-SMMUv3 的 IDR0/1/3/5、CR0/ACK、CR1/2、GBPA、STRTAB、CMDQ、EVTQ 和 GERROR/N。
   EVTQ prod/cons 位于架构 page1 `+0x100a8/+0x100ac`；不读保留的 IDR2/4，不解引用继承的表地址。
   配置需两次快照一致；支持 stage1、SID80、SSID、16KiB granule 仅是未来映射的必要条件。
6. 先撤自己的 CSI vote（CLR → AP EN=0 → 100µs → STA=0；最后全局 vote 消失时再等 PM_ACK=0），继承 vote 不撤。
   最后 `a1=0x804` PM_PUT 平衡 secure 引用。撤票失败保留 secure 依赖；PUT 失败保留未释放引用，均锁存、不重试。

`RESTORED` 只表示本次新增资源已释放/借用关系已验证，不表示其他 XPU 的 CSI/整个 MM 电源必须为 off。
`CSI_POWER/SMMU_POWER/SMMU_SNAPSHOT` 是这次操作的历史观察，不是请求返回后仍然持有的资源。
任何失败锁存本 boot 的后续 sensor/resource probe；成功的资源请求也不重复执行，第二次返回 `EALREADY`，不会再 MMIO/SMC。
失败阶段、原始 SMC 返回、独立 restore errno、前/中/后 PM_ACK 和保留 journal 会在工具与 `X9 CAMERA RESOURCES:` 日志中显示。

MM-SMMU 的 `QUIESCENT` 只说明稳定 NS 配置为空且 GBPA.ABORT 生效，**不证明独占**；`INHERITED` 表示存在/无法排除继承配置。
MT6993 上游启用 SEC/HYP，显示与相机共享实例，因此两种状态都保留 `NEED_SMMU_OWNER/NEED_DMA_MAP`。
本接口不 SEC_INIT/reset，不改 CR0/GBPA、wrapper、stream/CD/page table 或 CMDQ，不将 CPU PA 用作 IOVA，也不套用 GPU bypass。
上述 224B 接口不申请 CAM_MAIN/CAM_MRAW/VMM。下一节是独立的 MM 寄存器时钟事务；完整 PHY/eFuse/速率、async/outmux、真实 RAW DMA 完成与排空仍需实现，`pipeline` 的 MM power blocker 不会因历史探测成功被清除。
**资源 probe 返回 0 也不注册 platform、不置 READY/supported、不注入 iOS 桥、不产生帧。**

## 申请并还原 MM 域与寄存器时钟

仅 root、mode3、显式调用。**每次启动最多执行一条前或后相机事务；不是前后各一次。**

```bash
cameractl clocks back  # 主摄 SENINF3；另一次干净启动可选 clocks front（SENINF0）
cameractl status       # GET_MM_CLOCKS 只读缓存，没有 MMIO/SMC
```

独立 576B `PROBE_MM_CLOCKS(0x97)` 请求只含 version1/sensor1或2；`GET_MM_CLOCKS(0x96)` 返回最后结果。旧 ABI/命令不改。
先调用共享 receive provider，取得 secure MM-SMMU/CSI 引用并证明 AO/infra0 ready；**PM_GET 或共同 infra 尚未就绪时仍会失败，不绕过这个冷启动依赖**。
随后按下表顺序执行实际 HWV 事务，成功后逆序释放，最后才 CSI unvote/secure PM_PUT。

| slot | 资源 | HWV AP SET / aggregate EN / STA | effect 的判据 |
|---|---|---|---|
| 0/1 | AO / infra0，BACKUP_GRP0 bits0/1 | `20724 / 11358 / 1135c` | PM_ACK bank0 bits31/29=1 |
| 2 | VMM BUCK_SWITCH/AP bits10/11 | 同上 | backup STA bits10/11=0；**不是电压/OPP测量** |
| 3/4/5 | CAM_VCORE/MAIN/MRAW，bank0 bits16/15/8 | `20700 / 11318 / 1131c` | 对应 PM_ACK=1 |
| 6 | CAM autolink，mux bit2 | **不写 AP SET/CLR** | CKSYS_MM FENC bit18=1，CKSTA bit18=0 |
| 7 | CAMTM，mux bit3 | `20600 / 12330 / 12030` | FENC bit17=1，CKSTA bit17=0 |
| 8 | SENINF3（主）/0（前），mux bit20/17 | 同上 | FENC/CKSTA bit11/14 |
| 9 | CAM CG22 bits1/6/7 | `20108 / 12258 / 11858` | `3a010000` CG_STA 对应位=0 |
| 10 | CAMSV CG34 bits12/13/14 | `20198 / 12288 / 11888` | `3a680000` CG_STA 对应位=0 |

HWV base `31c00000`，CKSYS_MM base `10040000`，FENC `+270`、CKSTA `+330`。
CAM 的上游 `mtk_mux_generic_hwv_al_ops` 没有 enable/disable callback，因此不得把它当普通 mux 投 AP vote；域/CG 的真实 autolink 必须产生 FENC，否则有界失败。
CAMTM/对应 SENINF 的 selector 只检查有效范围和前后一致，**不改 parent/PLL/PDN/全局 autolink 表，不宣称 Hz 或 DVFS 已正确**。

MM raw voter 采用 `STA0 → FSM idle → SET → AP EN → aggregate EN → STA0`，BACKUP IRQ voter按上游不套 raw FSM 前置，均先在已持 secure 依赖时 dummy-read MMUP。
DONE 不是完成判据。CG 在 raw vote/FSM 检查后只对原 gated 目标做 `CG_CLR`，再读实际 CG_STA；不修复已投票但 gated 的继承状态，不接管未投票却 ungated 的直接 owner。

恢复只撤新增 AP mask，部分继承 mask 原样保留。CG 恢复只依赖 HWV last-unvote 的真实 gate readback，**不强写 CG_SET**；新 owner 阻止恢复时保留低层依赖并锁存。
最后 VMM bits10..13 unvote前按上游等 bank0 PM_ACK mask`10004`、bank1 mask`2`关闭；10ms 超时不降压、不撤 infra/secure。
AO/infra BACKUP 撤票后不要求物理 PM_ACK=0，因为 secure PM_GET 仍独立持有共同 infra。
每个 slot 保存完整 AP/aggregate/effect 的 before/active/after 和最后 STA；`retained`低11位标依赖 journal，bit30/31标 CSI/secure未释放。
`POWER/CLOCKS` 是本次持有期间的历史观察，`RESTORED` 表示自己的事务已还原，**不是返回后仍有采集资源**。

任何失败将 sensor/resources/clocks 全部锁存；重复 clocks 返回 `EALREADY`（失败则 EIO），不再访问硬件。
`X9 CAMERA MM:` / `X9 CAMERA MM SLOT:` / `X9 CAMERA MM RESTORE:` 保留决定 stage/slot 与恢复错误。
工具应确认 errno0、restore0、RESTORED、retained0，但 supported/READY 仍须0。

后续采集仍要求 VMM/DVFS与实际 rate 所有权、SMI larb/公共保留区、共享 SMMUv3 SID80/SSID/map/unmap、PHY/eFuse/settle、async/outmux、SW_PASS1_DONE与cache/timeout/drain。
当前不会开启 larb19/14/29、sensor stream-on 或 CAMSV DMA；不会因 NS SMMU 空配置抢占显示的实例，也不使用 UniXNU 的 SMMUv2/4KiB kit 替代 MT6993 的 SMMUv3/16KiB。
没有这次真实运行电源/ID/CSI包/DMA帧证据，**仍不能拍摄**。
