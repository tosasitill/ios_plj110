# X9 前后相机初步移植

**已实现控制层、RAW10 帧接口、iOS 双流桥、冷启动 sensor-probe、共享接收电源与 MM 寄存器时钟事务、SENINF/CAMSV 接收计划；采集后端仍缺失，不能拍摄。**
没有假帧、测试图案、Android 代理或 Camera.app 成功状态伪造。默认关闭。

## 实现边界

| 层 | 当前实现 |
|---|---|
| 传感器 | Android 日志确认的 `hhmain_mipi_raw` / `hhfront_mipi_raw`；ID 检查、init/preview 表、stream-on/off、曝光/帧长、Sony 倒数增益与线性 Q10 转换 |
| XNU | `/dev/x9-camera` 独占控制；只读状态；16 MiB 上限 RAW10；1 s 有界等待；generation 拒绝旧 DMA 完成；关闭时停止、排空、恢复 |
| 软件 ISP | HakiOS MIT 软件去马赛克、灰世界 AWB、AE、sRGB、NV12/BGRA；不使用高通 Chromatix；CCM 为单位矩阵，无 LSC |
| iOS | HakiOS `CMDerivedObjectCreate` 设备类 v3/流类 v2，前后 `FigCaptureStream`；输出池、PixelBufferHandler、帧 metadata；先确认真实 start 再返回成功 |
| sensor-probe | MT6993 CAMTG voter/parent 申请与释放、AP I2C 单控制器门控/FIFO/WRRD；WL28681C 指定通道电压/启停；reset/MCLK pinmux；真实 ID 检查与恢复日志。闲置 AP timing 初始化、禁用共享 rail 的申请/还原、GPIO203 局部唤醒。**仍要求共享时钟/HWCCF provider 已就绪，未上机验证** |
| SENINF/CAMSV 计划 | 零 MMIO 查询实际 CSI 端口、VC0/RAW10、候选 outmux/CAMSV、尺寸/64B stride/WDMA 打包及 SID；**不是资源申请或采集** |
| 接收电源接口 | TF-A MM-SMMU `PM_GET/PM_PUT`、CSI_BS_RX MM HWCCF 单 bit 申请/恢复；带供电前置检查的 SMMUv3 只读快照。**代码已构建，未上机；不是完整 MM power/clock provider 或 DMA mapping** |
| MM 寄存器时钟事务 | 显式 AO/infra0/VMM BACKUP 引用、CAM_VCORE/MAIN/MRAW 域、CAM autolink FENC、CAMTM/SENINF voter、CAM/CAMSV 寄存器 CG 申请/逆序恢复。**未上机；不改共享 AVS/CVFS/PLL，不申请 SMI larb，不证明采集速率** |
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
| `x9_camera=3` | probe-only，节点 root:root/0600；启动与状态查询不访问 MMIO、不上电、不安装桥；只有 root 的显式 sensor/resource/MM-clocks probe ioctl 才访问硬件 |

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

此模式不会写 `0x0100`，不加载 init/preview 表、不启动 ISP/DMA、不安装或注入 iOS 桥。
成功时 `probe_mask` 对应位与 `PROBED` 标志生效；**`supported_mask=0`、READY 不置位**。两枚 ID 都通过也不意味着能拍摄。
每枚 sensor 每次启动最多成功探测一次；任何失败锁存，后续请求不会重复硬件访问。工具返回非零退出码并打印决定阶段、blockers、芯片 ID 和独立恢复错误；`status` 可读取保留的最后证据。

当前实现刻意不接管未知的共享资源，前置条件包括：

- UNIVPLL 的已有 PCW/postdiv 配置在 `/104` 后为标称 24MHz（容差 ±1kHz）。不重编 PLL、不初始化 HWCCF 的全局/autolink 表；DONE 不就绪时有界退出。没有其他 XPU 投票且 CAMTG FENC 为 off 时，才允许把目标 mux 改为 parent7。已有投票则只接受正确 parent 和 FENC-on，不改 live parent。
- i2c4/i2c2 和 **i2c12** 的共享 I2C_EAST/I2C_PERI parent 已 FENC-on/CKSTA-idle，选用 PLL 时该 PLL 已 FENC-on；不修改共同 parent。允许只 ungate 目标 IMPE/IMPC bit，退出恢复自己增加的门控。所有暴露 bank 必须无事务/IRQ/FIFO 残留，所有 DMA bank 均未运行；主/前各 8 个 I2C、3 个 DMA bank，WL 为 4/1。TIMING 非零时只借用 open-drain/非 HS 状态；TIMING 为零且 MULTI 为零时，可按已验证 parent PCW 推导标准模式 ≤98kHz 的 high/low sample、step、divider 和 5µs EXT，保存/恢复 14 个 CSR。不会 reset 整个控制器、改 MCU IRQ 或共享 shadow mode。实际 DT 没有 `ch-offset-i2c`，使用 channel0，不能猜成 `+0x10000`。
- AP I2C 引脚允许已为 mode1，或 mode0/input 后仅切目标引脚的 mode1，退出恢复；mode2/SCP、GPIO output 等未知 owner 一律拒绝。GPIO203 已 output-high 时只借用；output-low 或 mode0/input 且 DIN-low 时可记录 DIR/DOUT，先低→output→高，等 1ms 后读 WL ID。退出前必须读整个 WL enable 字节为零才恢复自己拉高的203；GPIO187/202 和 UVP 控制始终不写。
- SID3 LN60_7 DOVDD、主摄 SID6 LDC0_3 AFVDD：已启用时只借精确电压/normal/DA-enabled。禁用时还必须 DA-off/HW_OP_EN0=0，AF 另要求 EINT_EN=0，才能记录 selector，设置电压并单 bit EN_SET。DOVDD 的实际编码是 **0xd0=1.8V**（coarse 高 nibble、fine 低 nibble），AF 0x0a=3V；此前 0x0d 检查不正确。每次 snapshot→ownership→写入→readback 同持共享 X9PMIF 锁；模糊写入保留 journal。主摄仅申请 SID3 0x178a 的 VS mask4，最后撤销自己的 vote，不清其他 bit。
- 目标 sensor 的 reset 为 GPIO output-low，或 mode0/input 且 DIN-low（保存 DIR/DOUT 后可转 output-low）；MCLK pinmux 为 off、它的 WL 通道均 disabled。主摄使用 WL4/7/2，前摄使用 WL3/1；其他通道保持。

`blockers` 位：`1=clock`、`2=shared rail`、`4=AP route`、`8=I2C state`、`16=inherited active/reset state`。
阶段编号：`1=preflight`、`2=PMIC`、`3=bus`、`4=LDO`、`5=power`、`6=ID`、`7=restore`、`8=done`，追加 `9=clock`（不改旧编号）。
一次 AP FIFO 请求最多约 5ms，单次 PMIF 状态等待最多约 1ms，DA 另有最多20次/100µs有界采样；单次 HWCCF DONE 等待最多 10ms，AP EN 最多 200µs，CKSTA/FENC 最多 1ms。未知继承状态不清 IRQ、不做总线 reset/retry。
失败后先把 sensor reset 拉低、断开 MCLK，再按真实反向顺序撤 DOVDD、WL DVDD、AF/AVDD（前摄 DOVDD 在 DVDD 之后），恢复仅自己改过的电压/引脚/14个控制寄存器/单控制器门控，撤销 CAMTG vote 并在无人使用时还原 parent，最后撤自己的 VS vote。共享 rail 的 EN_CLR 确认/DA-off 后才恢复原 selector；已有 live rail 不动。继承投票和其他通道保持；恢复失败保留 journal、锁存并明确报告，不声称资源已恢复。
**这补齐了冷启动供电/AP timing 的局部 ownership，但共同 I2C clock FENC/HWCCF autolink 仍要求已就绪；未知 HW/EINT 用户或 SCP owner 会拒绝。没有本轮实机 ID，不能宣称冷 LK 已全部打通，更不是可拍摄镜像。**

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
`flags & 16` 表示 voter/CAMTG/PLL FENC 检查通过，不表示实测频率、更不表示 capture READY；`cameractl` 输出 raw 证据并明确标注标称配置。

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
  分配还须 obey shared `mm_common` reservations；不能把 CPU PA 当相机 IOVA，也不能套 GPU 专用 SMMU bypass。
- 当前 `blockers`：1=该 sensor ID 未匹配且还原，2=MM power/clock ownership，4=PHY，8=路由申请/提交，16=DMA映射，32=完成/排空。
  只有1会在真实 probe 成功后清掉；其余 provider 尚未实现，不因计划生成成功变成 READY。
- 当前驱动 `mtk_irq_camsv_hybrid()` 用 central `+0x348` 的 **SW_GP_PASS1_DONE [19:16]**，active group/tag 和 inner frame sequence 判断帧完成；SCQ `+0x2000` 的 setting-done 不是帧。
  还须错误状态检查、cache同步、timeout隔离、stop/drain 后才释放 IOVA/buffer，再交给 `x9_camera_deliver()`。
- 尚需 port 对应 eFuse/PHY settle/速率、CKSYS_MM/CAM/SENINF/CSI_BS 的共同 provider；不从 source-only 配置推出实机已收包。

## 硬件后端接入契约

`kernel/src/bsd/dev/x9_camera.h` 定义 `x9_camera_platform`：

1. `power` 按 sensor 配置的顺序执行，并记录/恢复共享资源的原状态；失败半途也必须可恢复。
2. `read8/write8` 是 16-bit register / 8-bit data I2C；只在上电及总线归 AP 后调用。
3. `capture(on)` 在 stream-on 前完成 SENINF、VC0/DT0x2b、RAW10 DMA、SMMU 配置；`capture(off)` 在返回前停止并排空，即使初始化只做了一半。
4. 注册会分别上电读 ID 并恢复，只有两枚真实 ID 相符才发布 READY。调用发生在线程上下文；所有回调都不可重入相机 API。
5. DMA worker 完成缓存同步后调用 `x9_camera_deliver(sensor,generation,raw,stride,bytes,time)`；不得在硬中断调用。generation 必须来自本次 `capture(on)`，time 为帧完成的 `mach_absolute_time`。

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
