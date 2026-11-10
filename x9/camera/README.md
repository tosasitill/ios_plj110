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
| SENINF/CAMSV 计划 | 零 MMIO 查询实际 CSI 端口、主摄 VC1／前摄 VC0 RAW10、候选 outmux/CAMSV、尺寸/64B stride/WDMA 打包及 SID；**不是资源申请或采集** |
| 接收电源接口 | TF-A MM-SMMU `PM_GET/PM_PUT`、CSI_BS_RX MM HWCCF 单 bit 申请/恢复；带供电前置检查的 SMMUv3 只读快照。**代码已构建，未上机；不是完整 MM power/clock provider 或 DMA mapping** |
| MM 寄存器时钟事务 | 显式 AO/infra0/VMM BACKUP 引用、CAM_VCORE/MAIN/MRAW 域、CAM autolink FENC、CAMTM/SENINF voter、CAM/CAMSV 寄存器 CG 申请/逆序恢复。**未上机；不改共享 AVS/CVFS/PLL，不申请 SMI larb，不证明采集速率** |
| 接收寄存器预检 | 持有完整 MM 寄存器时钟链时，读取目标 MAC、async/AFIFO、outmux0 outer/inner、CAMSV0 central/DMA/CQ 控制，双采样验证稳定及使能位关闭。**只读，不提交配置，不证明 ownership 或 DMA 排空** |
| 持久 sensor/receiver 生命周期 | kernel-private 冷会话可在 verified standby 后持有同一 MM 时钟与 CSI/secure 电源 journal；后续请求及撤销前检查完整已消费依赖与原始接收控制，先隔离 sensor reset/MCLK，再退休 MM，最后还原 sensor 供电。**无 native capture caller，不 stream-on；不是 DMA lease/drain** |
| MAC/async 包配置 | verified standby/receiver 后，真实配置前 DPHY／后 CPHY 包格式、正常 RAW10 DT、unsplit async/CAM_MAIN AFIFO，并仅追加选定 async CG；逐控制 journal/guard/逆序恢复，保留其它端口及原 IRQ policy。**已局部编译，未运行；不启用 PHY mux/MAC lane，不是完整 PHY 或采集后端** |
| OUTMUX 配置引擎 | 同一已持有会话可显式配置 inactive OUTMUX0 的固定 image tag0，真实 masked shadow 写入；CFG_DONE 需 fresh masked STATUSX 事件和完整实际 inner target，再单 bit W1C 退休。WCLR policy 独立 journal/恢复，不启 AP/CCU IRQ 或 DL/DMA。**已局部编译，未运行；无 GIC/PHY/rate/SMI/SID80 grant/native capture caller** |
| CAMSV GIC/worker | 本次 LK FDT 的四 cell GIC 拓扑→SWEET Group1 分发→单次 SOF/hybrid/debug 采样→有界队列→既有逐页 cache/RAW10 finish；未启采集的配置路径只准备 dormant worker，close 先退休 GIC/worker。**已局部编译，未运行；begin 仍必须真实 normal domain/route/AP-IRQ/IMG0A lease，没有 native capture caller；已接后文 receive/TG consumer arm，仍不启 PHY/source 或置 READY** |
| normal SID80 映射引擎 | 自有 IAS34/16K RAW/CD/PTE；新增已启用normal NS控制器上的STE80 BBM/CFGI/CD/TLBI/SYNC安装、原STE80恢复与live check，保留实际STRTable/CMDQ/其它STEs。**接口需实际host跨firmware独占权限及power/cache/backing/drain证明；无注册者/运行结果，不能用private模板替换共享控制器或据此发DMA** |
| 采集后端 | **缺失**：完整 DVFS/rate/SMI ownership、PHY 校准/速率、SENINF 路由提交、MM-SMMUv3 映射、RAW DMA 完成/排空队列 |

原始 X9 DT 与 Android 启动日志确定：主摄为 i2c4/7-bit `0x1a`，ID 寄存器 `0x16/0x17` 返回 `0x9186`；前摄 i2c2/`0x10`，返回 `0x0615`。
不把厂商宏中加了 offset 的软件 sensor ID 当成芯片读数。
当前模式来自厂商 preview：主摄 4096×3072 / C-PHY 3 trio，前摄 3264×2448 / D-PHY 4 lane，均标称 30 fps。

目前只向 D22 用户态提供 Photo-compatible 格式（4032×3024 / 3088×2320），不声明 60/120 fps、视频录制、HDR、OIS、马达 AF 或拍摄闪光能力。
光学字段沿用 D22 兼容描述，不是 X9 的标定参数；图像朝向、Bayer 顺序、曝光时序、实际帧率和色彩均待物理帧验证。

### iOS 仅交付完整、本帧绑定的输出

`camera_device.c:deliver` 只有在实际输出池成功、CoreVideo lock 成功、锁定后的 allocation/plane/stride/span 不越界且不相交、全部分片渲染成功、unlock 成功及本帧 port/PTS metadata 绑定成功后，才调用 `PixelBufferHandler` 并返回成功。NV12 要求正确的两 plane 和偶数尺寸；BGRA 要求非 planar。池背压、非法 layout、分片分配失败或 attachment 失败都不消费 `StillImageCaptureNow`，不增加照片已交付计数。上限沿现有预览 renderer 的 8192 像素边长，涵盖已声明 D22 Photo 尺寸。

所有 renderer 返回 `bool`，失败意味着整张 buffer 不可发布。still 的 scratch 分组全部分配成功才替换旧指针/容量，避免早项 malloc 失败但后项成功时误认为可用；完整 NV12 和 BGRA still 共用逐像素 demosaic/缩放路径，BGRA 照片不再使用 2×2-cell 预览算法。分片按行对划分，短或奇数高度 BGRA 不留缝、不重复写。

`AddAttachmentsToSampleBuffer` 只接受自身 pixelbuffer 上已绑定且 port/PTS 有效的 metadata；没有“最近帧”fallback。samplebuffer 已有 metadata 的 port/原始 PTS 与自身图像冲突时返回错误，其它 graph-added keys 保留。RAW header 同时核对 NO_FOCUS 与 reserved 全零。以上为源码与真实 SDK 编译结果，**没有实际照片、前后切换或相机硬件验证；native capture 全链仍继续实现**。

### Stop 与传感器切换保留真实退休错误

worker 的 fatal read、短记录、非法/重复帧、malloc、曝光命令失败记录首个 errno；仅 EAGAIN/EINTR 继续等待。不再从 worker 以“另一传感器帧”为由重新发 SENSOR。所有实际采集 fd 在 close 前先执行一次显式 kernel STOP；分别保留 first/stop/close errno，实际 close 也只调用一次：descriptor 可能已被消费，不能重试。Stop/Invalidate/Relinquish 及切传感器必须先 join 原 worker，并传播真实错误；`running=false` 不是退休证明。STOP、回执验证或 close 失败均在该进程中锁存 shared-retire error，后续不能重新 open/发新 SENSOR，不能掩盖内核 full-drain 失败。

全局 native-device transition mutex 用 trylock 串行 Start/Stop；busy 不阻塞等待。Stop 先关闭 callback admission；若已有 handler 在执行，立即返回 unavailable，worker 在该回调返回后 close 一次，active 引用仍保留到后续实际 join。worker 对自身流的 Stop 重入也只关闭 admission/请求停止，不 join 自己或伪造退休成功。Start 的 worker 重入仍拒绝。worker 本身不取 transition mutex，join 不持它需要的 output lock。唯一 active stream 持有强引用，join 成功后才在 transition mutex 外释放；join 失败保留上下文并禁止盲目重试。Device invalidate 尝试停止自身两流并返回首错，finalize 不把失败当成功；未能 join 的 active worker 保持 pin。

仅完整成功的显式 Stop 取消未交付的旧 still 请求，取消不增加已交付照片数；停止期间拒绝新 shutter ticket。图像 zoom/AF 取本帧快照，计数与可并发读写的标量使用 atomic；符号初始化用 pthread_once。此处的 iOS 错误传播/serialization **不授予硬件 lease，不证明 source/AXI/SMMU drain 已实现或成功**。XNU 源码 `fg_drop→fo_close→vn_closefile→vn_close→spec_close→cam_close` 保留错误返回，但只有最后 fileglob/vnode 引用才执行 driver close；不能把普通 `close()==0` 单独当作完整退休凭证。

显式 `X9_CAMERA_STOP` 使用独立 **64B** 回执，旧 88B status 不变。请求只能携带 version=1、实际 sensor/generation，其他字节必须零；mode2/FWRITE 且原 open 身份授权，控制锁内先检查同 sensor/generation，过期请求不执行停止。接受后封闭本次 open，重复 STOP 返回 EALREADY，不重发硬件操作，SENSOR/EXPOSURE 返回 ESHUTDOWN；真正新 open 才重置封闭标记。回执记录前后 generation、attempted power/capture/stream、CPU readers 与状态：IDLE 只说明没有 attempted scope；RETIRED 必须实际 stop 成功且 after scope/readers/streaming/retained/latched 都清空；失败返回 RETAINED/LATCHED 与原 errno，不能把 ioctl 返回0当成功。generation 耗尽锁存 EOVERFLOW、不回绕；已有 open/保留状态拒绝 platform 注册，避免注册探测擦掉旧故障。

用户态共享 `camera_stop.h` 先取实际 status，再发唯一 STOP，严格核对版本、sensor、前后 generation、状态/attempted bits、reader 数、reserved 与 disposition；不对 ENOTTY/EINTR/ESTALE/EALREADY 或坏回执自动重发，也不 fallback 到 close0。已显式 STOP 的 driver close 只验证成功回执仍匹配当前已退休 scope，或返回原停机错误，不再次调用 stop/递增 generation；CPU staging 只有实际无 scope/readers/retained/latched 才释放，S1 export backing 不动。`cameractl back/front` 的所有退出路径都消费显式 STOP 并传播清理错误；只读/mode3诊断不发 STOP，close 错误仍传播。`GET_STOP`/status 打印的是历史缓存，初始 version0 表示没有回执，不发 MMIO、不赋予当前权限。**此为真实控制调用与源码编译结果，仍没有 native capture 注册/SID80 attach/物理帧；full AXI/SMI/SMMU/EMI 退休和完整采集后端继续实现。**

### STOP 不发布旧帧，但可退休真实 owned DONE

core STOP 在下层回调前递增 generation、清除 STREAMING，阻止旧 RAW 交给 iOS。原 worker/公开 finish 只按该 generation 拒绝，会把已经排队、实际 SOF/DONE/FBC 完成的自有请求留在 DEVICE/RETAINED，导致后续局部停机所需的 CPU RETIRED 资格无法成立。现在仅 native IRQ worker 有私有 current-or-canceled finish；公开 `x9_camera_camsv_finish` 仍在 owner/cache 回调前拒绝过期 generation。

私有取消分支要求实际 core STOP journal、同一 sensor、原 attempted power/capture/stream 仍保持、没有 latched/FAULT、core generation 恰好为原 generation+1。worker 在 finish 的 camera_lock 内重新判断，覆盖 STOP 发生在首次 core 检查前、检查后/FBC 验收期间及 finish 准入后的不同窗口；`ECANCELED` 只表示可继续验证，不是退休成功。若 finish 准入后才发生 STOP，原 delivery 的 generation 检查仍拒绝交付，再按原完成路径退休。

取消帧还必须有原 native lease/request、真实 SOF/DONE/tag/group/sequence、已验收两次的双 bank FBC 原单 credit WCNT+1/CNT0、实际 GIC mapping/power/route/priority 和无 IRQ/FBC/source 错误。spin 内新采样仅重核普通 FBC control/count，不重读或清除 IRQ 历史。资格成立后依次执行原 owner check→真实逐页 CPU IVAC→owner check，**不调用 `x9_camera_deliver`**，再 owner check→S1 RAW_IDLE→slot RETIRED。只有实际 slot RETIRED 才允许 worker 将该丢弃的 ESTALE 归为局部完成并 mask 自己的 GIC；其他 stale、缺 SOF/DONE、非法计数、启动 FAULT 或回调失败仍锁存并保留依赖。

`X9 CAMERA STOP DONE` 日志记录 generation/capture_id/sequence/core_cancel/slot state/errno。这是源码修复与真实 kernel 单位编译结果，**无 native caller 或运行证明；不等于 full AXI/SMI/SMMU/EMI drain，不取消 credit、不 unmap、不撤供电，也不放行 rearm**。后续原 consumer-stop/spent 检查必须继续逐项通过，完整采集后端仍未完成。

### 锁外回调绑定实际配置与下一完成帧

worker 不再持 output mutex 渲染或调用 `PixelBufferHandler`。每次输出取得不可变 config、`Block_copy` 的真实 handler、配置 revision、run epoch 和对应 shutter ticket。锁外从原图像池分配及渲染，最后短锁 admission 只接受仍匹配的运行/配置/handler/request；渲染期间配置被改掉则丢弃旧输出，不调用旧 handler。handler 输入必须是实际 libclosure concrete block class，stack block 在注册时复制，不以普通 CF 值充当 callable。config/handler/enable 多输出更新先完整准备，再一次受锁提交；分配或参数错误不留下部分更新。

已 admission 的 callback 期间，任何图像输出配置/handler/enable 替换返回 unavailable、不修改 graph context；getter、zoom、AF、shutter 等同步重入不再撞上持锁 callback。CF/Block/CV 析构和图像池调用在 output lock 外。主输出的默认启用仅用于没有显式 enable 属性时，明确 false 不再被 preview fallback 覆盖。old config/pool 与 handler 持有到实际回调完成或被丢弃，preview 的 HELD 防重用引用保留。

每个 still 请求有 FIFO ticket、真实 `mach_absolute_time()` 接受时间和目标 run epoch。只选择**完成时间不早于该请求**、同一 run 的 RAW 帧；预览回调中新发出的 shutter 不会消费早已完成的同一 RAW。callback 真正调用并返回后才退休原队头/增加照片数，失败或背压保留原队头；Stop 取消与 object finalize 丢弃都不计照片完成。这里约束的是 header 的实际 DONE time，**不是已证明 SOF/exposure 在 shutter 之后、零快门延迟、拍照 settings ID 回传、相册写入或前后摄实测**。

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

`x9_camera_camsv_arm/finish` 为真实 provider 提供 kernel-only 的 owned-slot 完成路径，不注册后端、不改变 READY，也不读取 MMIO 或新增 ioctl。**kernel-private CAMSV version 已为2**：删除可被空实现冒充的 `sync_cpu` 回调，强制提供 `x9_camera_s1_export` 返回的 `backing`，由具体 S1 代码同步缓存。旧版1不能混用；用户态 RAW10/ioctl ABI 不变。arm 保存已绑定的 metadata、64-bit capture_id、预期硬件 inner sequence、tag/group 和 IOVA；合法 domain/route/slot 的 `check` 回调仍必需。16KiB 对齐、16GiB dma-range、backing 指针或 cache state 都不能证明授权映射；PM_GET、CPU PA 和空成功 check 均不是 lease。

arm 的实际路径为 **lease check → 完整 S1/PTE/wired-page 核对 → 每一16KiB RAW物理页对应VA的 `DC CIVAC` → 再核对S1和lease → ARMED**。backing 的软件状态在首笔 cache op 前绑定唯一 slot/context、lease 与完整 request；DEVICE/CPU/RETAINED 都不得重发/rebind。已退休 backing 的 generation/capture_id/inner sequence 也独立核对，不依赖 slot 被外部清零后丢失的历史。同 generation 不得换 lease；新 generation 可在同 slot/context 上取得新的真实 lease，但不能换 backing/回调/图像sensor/tag/group。

唯一 IRQ owner 必须提供一次**一致的原始快照**：outer DONE `+0x348`、inner ERR `+0x350`、CQ STATUS `+0x1c`、inner FIRST/LAST_TAG 与该 tag 的 FH_SPARE，group_tags 来自该帧 SOF 的有效 group 绑定。读清状态只消费一次，worker 不补读；快照的 generation/capture_id 来自实际 slot journal，不能由硬件 counter 猜出。

finish 优先拒绝错误；CQ setting-done 或普通 GP_DONE 单独返回 `EAGAIN`，不是帧。只接受目标 `SW_GP_PASS1_DONE`（bit16..19）、单 tag/group 及精确 inner sequence，outer 即使超前也不当作完成凭据。通过后实际执行 **lease check → S1精确request匹配 → CPU pin/逐页 `DC IVAC` → 再验证S1和lease → `x9_camera_deliver` 同步复制 → 再验证lease/S1 → CPU pin退休/slot RETIRED**，此前不读取 RAW。完成后只 invalidate，绝不 clean+invalidate 将旧CPU dirty bytes写回覆盖DMA图像。成功、CPU 背压或同步期间 stop 都退休 source；重复 finish 不重放。tag/group/序号歧义或 lease/cache 失败使slot及其已绑定backing终态 RETAINED，保存首错，不 rearm、free、unmap 或撤依赖。其他slot误用正在使用的backing仅拒绝/保留其错误尝试，不替合法worker取得或更改缓存所有权。重用退休 slot 时 capture_id 必须前进，32-bit inner sequence 只接受小于半周期的非零向前差值，允许 wrap 到零但不自动扩展成 capture_id。

本地冻结 SWEET `caches.c` 的 coherent-IO 分支将通用 `clean_dcache/flush_dcache` 实现为仅屏障，`*64` assembly入口跳转到它们；不能据API名称保证NC相机硬件能看到数据。相机局部 `x9_camera_cache.inc` 直接使用 `CTR_EL0.DminLine`、`DSB SY` 和PoC指令，不依赖该分支，也不改变其它子系统的cache政策。所有range限定在一个已核对的wired物理页；RAW包括末页padding都拥有整行，只做全页invalidate，无相邻分配partial-line丢弃。S1/CD/STE/CMDQ/EVTQ首次发布与每笔CMDQ command也已接实际CVAC/CIVAC，只有成功同步并再验证storage后才返回export或提交producer；不能用第一物理页+整个非连续虚拟长度同步。RAW ownership首错单独保存在 `raw_errno`，不伪装为PTE/storage损坏来阻断未来合法owner的abort/TLBI/drain；RAW仍终态不可重用，现有代码不新增map退休/授权。

返回码不能单独决定 slot 归属：`EAGAIN + ARMED` 才是继续等完成，`EBUSY + RETIRED` 才是 CPU 背压丢帧；lease/cache 回调也可能返回同样 errno，但此时是 `RETAINED`，不能重试或回收。终态保留最初失败 errno。

slot、raw、映射和回调 context 必须由 provider pin 到同步调用返回。**native controller 必须禁止补充 FBC/RCNT 或任何自动重用在 finish 返回 RETIRED 且实际 producer/AXI 退休前重新写此 RAW slot**；软件CPU pin不能自行停硬件，也不是整个DMA/AXI drain证明。同步复制完成后才可重新发布DEVICE cache状态；用户 `uiomove` 只读取独立CPU staging，旧的 `frame_readers` 背压不是RAW source ownership。capture(off) 必须先停硬件并排空这些 worker；export后仍永久保留，不以CPU退休当作map/unmap授权。**finish 本层实现快照资格检查、具体逐页cache/CPU pin与交付，不独立证明 IRQ 来源、控制生效、合法映射或实际 drain。** 下述 GIC worker 已有实际采样/调用代码，但尚无真实 native capture provider 调用 begin，也没有运行 IRQ/RAW10 证据；SID80/domain 委派、rate/SMI/PHY/route/AP-IRQ owner与采集启动仍缺，不能拍摄。UniXNU的4KiB kit不替代此16KiB/v3路径，HakiOS `PLACE_READING`/ring初始化说明reader pin边界，未移植其QCOM地址或完成后CIVAC策略。

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

标称 pclk、sensor PLL 读回或 `VERIFIED` **不是实测 lane/trio 速率、PHY 校准或 rate lease**。主 preview 的图像为 **VC1/DT2b/4096×3072**，另有 **VC3/DT30/4096×768 PDAF**；前摄图像为 **VC0/DT2b/3264×2448**。这来自两枚 sensor 的 `mode_struct[0].frame_desc_prev`，不是其它 custom mode 的推断；写表不代表接收器已过滤出图像通道。前摄 fab 分支已接入，但 vendor open 中的主摄 AF 激活/QSC 与 QSC/LRC 标定仍未执行，因此只称 **未标定 nominal standby**，不能称厂商完整 open 或成像质量验证。blockers `1=factory/calibration`、`2=receiver`、`4=DMA mapping`、`8=drain` 不因读回成功清除。

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
- tag0 的 outmux VCDT `+0x100=(0x2b<<16)|(vc<<8)|1`，主摄 `vc=1`、前摄 `vc=0`；首次 `_config_outmux → SET_TAG` 预期尺寸 `+0x104=((height-1)<<16)|(width/4*5-1)`。
  HSIZE 是 packed RAW10 **字节数减1**，不是像素数；后续 apply-v2 路径的 HSIZE 语义不同，不混用。
  候选 CAMSV0 base `0x3a600000`，DMA bank `+0x1000`：地址 low/high `+0x200/+0x204`，basic `+0x210=(stride<<16)|0x10`；不是旧 unit-test 的 TG 寄存器布局。
- OUTMUX 的 tag block 步长为 **`0x1c`**，不是 CAMSV 的 `0x40`；格式在 tag base `+0x14`（tag0 `+0x114`），`+0x108/+0x10c` 是 crop，`+0x110` 是 debug。主 preview 的 `SOURCE_CONFIG0` reference/last VC 为 **1/3**，前摄为 **0/0**；tag0 的图像 VC 与整个 sensor 的最后帧边界 VC 不混用。
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

### 接通正常相机 DMA 要实现 stage1，不是调用 protected-memory MAP

厂商正常路径是 `arm_smmu_attach_dev → arm_smmu_domain_finalise → ARM_64_LPAE_S1`，分配 ASID、建立 CD/STE 并同步命令队列；之后 `mtk_smmu_map_pages` 调用该 domain 的 `io_pgtable_ops.map_pages` 建立 IOVA→PA。仅有电源引用、可用 IOVA 范围或空闲控制寄存器，都没有提供这套主机映射服务。

`pkvm_smmu_host.c::smmu_host_hvc` 注册的 secure MAP/UNMAP 是另一条链：

- `mtk_tmem_interop.c::pkvm_smmu_mapping` 用固定 SMC `0xBB00FFCB/0xBB00FFCC` 查询返回在 **a1** 的动态 HVC handle，再通过 `pkvm_el2_mod_call` 调用。固定 SMC 返回不等于已经 map，也不能把源码中的注册顺序当作 HVC 编号。
- `mtk_smmu_secure_v2` 接收 PFN 列表、属性与条目数，调用 `host_donate_hyp`，把页从 Linux VM0 隔离并让 protected VM1 访问；没有相机 IOVA/SID/ASID 输入。**它不是正常 SID80 的 stage1 映射入口，不能拿普通 RAW 缓冲区尝试这条 donation 路径。**
- 当该 HYP 模块确实运行时，`check_ste_update_content` 将 guest S1 字段与 host S2 字段合成；`write_guest_strtab_base_reg` 对 guest STRTAB base 作 write-once 保护，并把硬件 base 指向 host 表。此时寄存器所见 base 不一定是可由 XNU直接更新的 guest 表。当前 XNU 启动日志没有证明该模块运行，不能反过来把 Android 编译配置当作当前状态。

captured DT 区分 normal camera **SID80**、protected camera **SID89**、display **SID90**（其 `mtk,smmu-dma-mode="bypass"` 是配置描述，不是本轮状态），三者都引用 `mm_common`。不能用 SID89 授权替代 SID80，或只建一张 SID80 表而破坏显示与其它共享 stream。LK 的 `platform_mtksmmu_mpool` reservation 也不是相机 buffer/IOVA 分配权。

下一具体工作是落实 **MM-SMMUv3 主机初始化/委派契约及正常 SID80 attach**：明确当前 NS/HYP 归属、合法的 STRTAB/CMDQ owner、CD/ASID/IOVA 分配与显示保留方式，再实现 map/unmap、TLBI/cache 和真实 CAMSV producer。当前仓库只有受供电保护的 MM-SMMUv3 快照，没有这套 mapper，也没有 `x9_camera_register_platform` 的调用者。该缺口不能由新的 completion adapter 或 diagnostic 镜像补齐。

### 自有转换 backing 与 CMDQ 超时收尾

`x9_camera_s1` 与 `x9_camera_smmu` 已提供真实wired RAW/16KiB页表/CD/STE/CMDQ/EVTQ分配、PoC发布及固定SID80/SSID0/ASID1的CFGI/TLBI/SYNC命令实体，**尚不是正常主机bootstrap/attach或授权mapping服务**。模板保留display90 bypass、其他STE abort，不能未经prior-preservation和native NS授权安装。export之后永久保留所有分配；本节不新增controller注册、PM票据或READY。

每个批次保存最多三个完整16B command record（最后一笔固定CS_NONE SYNC），写入自有wired队列并实际CVAC；提交PROD前及观察CONS时核对原record、批次起点/目标、own storage/bases/config、actual PROD/CONS和GERROR/EVTQ。private CPU record相同也不是firmware grant，controller仍必须排除其他producer和HYP重定向。

命令10ms deadline在每次观察前后检查；超过deadline的CONS即使已经到目标也不能返回正常成功。只有明确的`ETIMEDOUT + pending`在**同一owner-pinned调用与锁内**进入一次额外、有界的10ms close观察，真实批次/自有storage完整保持且两个actual CONS样本均等于已发PROD目标才退休这笔pending。第一次实际到目标后，原forward-window已收缩为零，任何回退或外部producer/config/batch变化都拒绝。其它queue/global/EVTQ错误不沿timeout例外收尾。

close不写任何MMIO，不重发PROD、不写CONS/GERRORN/EVTQ、不cancel/reset，也不新增命令。即使实际迟到消费已确认，**原调用仍返回首个ETIMEDOUT，namespace保持终态，不接受后续命令或重试**；`X9 CAMERA SMMU CLOSE`缓存日志区分original、qualify、pending、retired、samples与actual/target。两次CONS到目标只资格化这一批含SYNC命令，不是RAW/AXI/IRQ/worker drain，更不是释放backing/shared power、恢复继承policy或unmap的许可。仍需完整native正常控制器与真实采集producer。

## 硬件后端接入契约

### 持有冷传感器与接收时钟

`kernel/src/bsd/dev/x9_camera_sensor_session.h` 的 kernel-private 调用顺序为：
`open → configure → prepare_receiver → read8 → close`。`prepare_receiver` 可省略，原 probe/standby 不因此增加 MM 操作。准备 receiver 后可显式 `prepare_smi_clocks → configure_mac → prepare_analog → configure_timing → configure_ctle → configure_outmux`（均为 `x9_camera_sensor_session_*`）；SMI clocks 需要 configured standby 且必须在 MAC/PHY/OUTMUX 之前，analog 需要先成功配置 MAC，timing/CTLE 需要已校准 analog，各阶段均可省略，原诊断不会自动写这些配置。MAC/analog/timing/CTLE 必须先于 OUTMUX；已持有 OUTMUX 时再配置返回 `EBUSY`，不能改变已 posted 命令的输入环境。timing 不能在 CTLE 已尝试后追加；后续接入的 generic PHY 配置也须在最终 CTLE override 之前完成。

`prepare_smi_clocks(epoch, &smi_clock_status)` 先补 **CAMSV0 的真实 SMI 时钟前置条件**，再配置单图像 A0 端口的默认参数，仍不等于 `mtk_smi_larb_enable` 的全部 port/common 配置。设备树 `camsv1@3a600000` 的实际 `camsv-id=0`，larb 顺序为 **19/14/29**：19→cam-subcomm3→disp-common0，14→cam-subcomm1→subcomm1_1→disp-common0，29→cam-subcomm2→mdp-common1。common0/1 均为 skip-rpm-cb 且没有 clocks，不触其 VCP/display 状态；subcomm1 与 1_1 共用一个物理 gate，不能重复申请或提前释放。

实际写入为 supplier **CAM_VCORE_R1A `3c80d000` / CG35 bits13/14/15**，随后 consumer **CAM_MRAW `3a680000` / CG34 LARB14/19/29 bits1/2/5**；`3c800000` 是邻接 CCU，不能替代 R1A。只追加缺失的 AP raw HWV bit，等待真实 AP readback/global enable/STA0，按 vendor normal path 必要时 masked CG_CLR，并确认实际 CG0。完整 MM/CSI/secure power 已持有，MMINFRA AL parent 的 CFG_0+10 整 byte、CKSTA/FENC 必须稳定；不写其 AP/parent/PDN/PLL，不把 FENC 当成速率/吞吐 lease。私有 status 和实体 `x9_camera_smi_clocks.inc/.h` 不改变既有公开 ioctl ABI。

每组独立保存 before/expected/owned/borrowed 与 posted SET/CG_CLR/CLR pending；拒绝 voted-but-gated 修复和 unvoted direct-clock 接管。原 borrowed AP/global/effect 必须保持，失败不能重发原请求或以 prior 控制值证明消费。pending 只有确认 target/effect 与 voter STA0 才允许唯一 close 继续；仍未知时保留所有依赖。receiver/PHY/AP 的既有 guard 现在也检查该 optional lifetime；原 MM close 在 SMI held 时拒绝卸依赖。关闭顺序 OUTMUX→PHY→MAC/async→SMI consumer→supplier→MM/sensor rails，只撤新增 AP bit，physical gate 由最后 aggregate vote 恢复，不强制 CG_SET。新 owner 保持原 gated 位时拒绝继续撤低层依赖。

准入完成及撤 clock 前双采样三路 LARB 的普通 RO `STAT+0` / internal `STAT+500` 必须0；失败部分仍 gated 的 larb 不读。它仅是当前未启 producer 的必要 clock-unvote 条件，**不是完整 AXI/IRQ/cache/worker drain**。没有 IRQ/R-C 读取、SID80/path/MMU 写入、native capture caller 或实机证据；因此 **NEED_SMI/NEED_RATE/NEED_DMA_MAP/NEED_DRAIN 均不清除**，`SMI_CLOCKS_CONFIGURED` 只表示时钟控制读回，不造 READY。

#### 共享时钟打开前保持其他 CAMSV inactive

Captured DT 中，A 的 L19 与 B 共用，L14 与 C 共用，L29 与 D/E/F 共用。只采样 A 的 disabled controls 和 LARB STAT0，不能保证共享 clock 打开后这些 producer 不会恢复运行。`x9_camera_sv_siblings.inc` 已接实际 MM take/guard/restore：既有 receive PM/CSI reference 申请后、**MM journal 第一笔 backup/domain/clock vote 之前**双采样 CG34 bits15..19（B～F）的全部十个 XPU EN、global 和 STA，必须全0；不等 foreign vote 消失、不追加 sibling vote。这没有前移或替换 receive provider 的 secure PM/CSI 申请。

CG34 bit12/13/14 实际是 CAMTG/TOP/A，不是三个 CAMSV engine。MRAW domain8 真正消费后，在仍 held 的 `seen` 和 fresh domain AP/global/PM_ACK/STA 资格下读取 CG34，B～F 的 physical gate 必须全部为1。partial take 的 MRAW SET 即使尚未 `completed`，也必须证明实际完整 target 才能检查 CG 和继续唯一 close；入口 off/prior 不能退休已发请求。未消费/未发 MRAW 引用或已退休 domain 时只读受 secure/infra authority 保护的 HWV，不读断电 CG；不为了检查而启用 B～F，也不读它们的 producer、CQ 或 IRQ banks。

上述 guard 覆盖每笔 MM SET/CG_CLR/CLR、receiver/MAC/PHY/OUTMUX/SMI 配置及整个 MM 逆序恢复。准入后出现 foreign vote、direct ungate 或电源资格变化会 terminal latch，即使之后回到0也不恢复操作；保留尚未退休的依赖。初次准入被拒且尚未发 MM vote，只恢复自己的 receive power 引用。关闭后 `X9 CAMERA SV SIBLING JOURNAL/XPU` 只输出保存的实际采样，不回读已撤电硬件；失败路径也不为了补日志再读失去资格的 effect。

SMI 共享快照同时冻结实际消费的 **CG35 bits13/14/15 和 CG34 bits1/2/5** 的全部 foreign EN，不再只检查 COMM33/L19 单位。新增 L14/L29、COMM1/2 的外部 owner 即使被本 AP aggregate 隐藏也会拒绝；COMM33/L19 冷 misc 的资格仍只按各自 bit15/bit2 scope 计算，不因此取得或写入其它 common 的配置。这里是 inactive 时钟准入和可见冲突保护，**不是硬件 lease、ABA 防护、SMMUv3 ownership、完整 DMA drain 或拍摄就绪**；已有工作的 sibling 会被拒绝，未来需要真正共享 owner 才能支持并行使用。

#### 初始化冷启动的共享 SMI misc

`x9_camera_smi_shared.inc` 在同一个准备入口中，先于 SMI clock votes 捕获共享资格，clock 消费后才初始化 misc。**这不是以新增 AP clock 冒充全局独占：已有共享 owner 的不匹配配置仍返回 `EOPNOTSUPP`，不接管、不 reset。** Captured COMM33 不只供应 L19，还供应 **L27/CAM_MAIN 和 L44/RAW_C**；L44 的两路 SMI clock 实际为 RAW_C larbx bit0、cam bit1，不能只检查 L19。

资格与守护覆盖以下真实 RO 状态：

| 路径 | 准备前条件 / 后续检查 |
|---|---|
| COMM33 | CG35 bit15，准备前 AP/global0、physical gated；全部十个 XPU 同位0 |
| L19 | CG34 bit2，准备前 AP/global0、physical gated；全部十个 XPU 同位0 |
| L27 | CG22 bit0 global/XPU0且 CAM_MAIN 实际 gate0=1，不启用它来检查 |
| L44/RAW_C | CG30 bits0/1 global/XPU0，RAW_C domain11 AP/global/PM_ACK和十个 XPU0；不读断电 RAW_C 的物理 CG |

十个 XPU 的 v1 EN 地址来自 `hwccf.h::CCF_XPU`：0..4 为 `20000+n*10000`，5..9 为 `70000+(n-5)*1000`，CG EN 为 `base+cg*0xc+8`。原 AP-only MM_HWV mapping 不足以读非 AP bank，因此单独映射实际 DT resource 内 `0x31c30000/0x45000`；不访问 history/IRQ，也不写非 AP bank。投票快照跨阶段保持，只有本 journal 消费的两个 AP mask允许变为自身 expected；新增外部投票即使被现有 aggregate 隐藏也会拒绝后续写入。

COMM33 的 scoped cold 条件还要求 L19/L27/L44 全部上述条件成立；L19-wide misc 的 cold 条件只针对准备前 L19 gated/unvoted。通过后双采样真实 misc，仅写下表五个配置 CSR：

| bank | 顺序 / 配置 |
|---|---|
| COMM33 `3c808000` | `L1LEN+100=a → PREULTRA_MASK1+244=4146 → DUMMY+444=1` |
| L19 `3a01b000` | `CMD_THRT_CON+24=370456 → SW_FLAG+40=1` |

顺序取自 MT6993 misc 与 supplier-first 路径；先 journal before/expected/owned，再 posted write、完整 target 读回与依赖验证。已经匹配的字段不写也不恢复，旧共享 owner 仅可匹配使用。保留全部 L1ARB、BUS_SEL、其它端口、其它 common、路由/MMU/SID 和 reserved padding；没有 `offset0/value0` 的盲循环。`shared.before/after/target` 分别是初始采样、实际控制读数、未必消费的目标，pending 不伪装成 observed。

关闭前先资格化 SMI/共享/A0 pending 的真实 target，再执行 OUTMUX→PHY→MAC→A0。之后以唯一逆序 journal 恢复 L19 misc→COMM33 misc，再撤 consumer/supplier clocks和 MM/supplies。任何控制/投票变化、未消费 target 或 busy 都保持尚未退休依赖；不以 prior 值重发、cancel/reset 或 late retry。此处的 scoped cold 初始化没有建立持续 bandwidth/rate、IRQ/worker 或 DMA authority，**不是完整 SMI drain 或 READY**。

#### 准备 CAMSV A0 的默认 SMI 端口

共享引擎消费冷写入或确认继承配置后，`x9_camera_smi_a0.inc` 在同一会话里再次检查 COMM33 `0x3c808000` 的 `L1LEN=a / PREULTRA_MASK1=4146 / DUMMY=1`，以及 L19 的 `CMD_THRT_CON=370456 / SW_FLAG=1`。A0 本身不修复共享字段。所有八个 L1ARB、BUS_SEL 及五个合法 L19 端口的 NONSEC/OSTD 都按最初消费快照持续检查；不把 MT6993 mutable common BW 表初始全0当成覆盖其它 masters 的授权。

只有 **L19/P0=`CAMSV_TOP_A0`，AXID `0x260`** 的三个字段可写：

| 字段 | 写入 | 上游依据 |
|---|---|---|
| `OSTDL +200` | 本端口完整配置值 `1` | captured `is-default-ostdl` 和 MT6993 default_bwl |
| `DISABLE_ULTRA +70` | 仅 bit0=`1`，保留其余端口 | probe 把可切 HRT 的端口先初始化为 SRT |
| `NONSEC +380` | 仅 bit3=`1`，保留 MMU/bank/path 等其它位 | `mtk_smi_larb_bw_thr(..., true)` |

DT 的 `larb-port-real-time-type=0` 是 `ALWAYS_HRT`，但 **不等于启动时已授予 HRT**。`mtk_smi_larb_probe` 对它仍置 `real_time_type`，实际 HRT bandwidth request 后才撤 SRT 位；这里不虚构该 request。L19/P2=`CQI_A/0x262`、B1/STG、L14/A1、L29/A2 均不写，也不写不存在于三路 captured DT 的 internal OSTD 配置。两个 force-ultra 位必须关闭，SLP_CON EN/RDY 和 A0 主/内部 outstanding monitor `+280/+780` 双采样必须0；这里只资格化当前未启 producer 的端口，不证明完整 DMA drain。

每次写前检查 receiver/MM/SMI/共享快照，逐字段保存 before/expected/owned mask，并在 posted write 前记 pending。读回必须达到完整 target；失败后的唯一 close 只接受两次真实 target，不以 prior 值、重发或 reset 退休。关闭先在 supplier/consumer clocks 仍持有时按 `NONSEC → DISABLE_ULTRA → OSTD` 恢复本次改变的字段，再撤 consumer/supplier；原本已为目标值的字段不写也不恢复。任何共享字段变化、仍有 outstanding 或未消费写入都保留低层依赖。

private `smi_clock_status.a0/shared` 返回快照、owned/pending、配置与恢复结果，不改变公开 ioctl ABI。`A0_CONFIGURED` 仅表示 inactive A0 default 配置，**不是全部 common/port 初始化、MMQoS/rate lease、SID80 mapping 或 READY**。其它实际使用的 common/port、持续 bandwidth/rate、native SMMUv3、IRQ、DMA 与 iOS 采集仍须接完，原诊断不自动调用此 preparation。

- standalone 入口取得 `camera_control_lock`；同 TU 的 native 回调已经持锁，必须使用内部 `_locked` helper，不重入公开入口。
- `open` 返回的非零 epoch（包括错误返回）代表已准入 journal，必须安排一次 close；旧 epoch、重复 prepare/close 不做 MMIO。成功 prepare 不撤销 MM votes，不设置 READY，不返回 IOVA。
- MM 与 receiver probe 实际复用同一持久引擎，但立即 close；诊断的 once-per-boot success 与持久会话正常退休后的再次开关独立。GET 仍只读缓存，不实时访问硬件。
- 每次后续 sensor 请求检查 held receiver/MM；将新采样与**最初消费的配置及本 journal 确认的写入**比较，而非只验证两次当前读数相等。初始 snapshot 保留。共享 async/AFIFO 只比较选定端口字段，不接管其它 AOV/SCP 端口；不读取 read-clear IRQ/DONE。
- 一旦依赖丢失、接收控制被修改/使能、或 close 失败，保持 epoch 和尚未退休依赖，整 boot 锁存。接收检测失败不会继续撤 sensor rails；sensor 隔离失败也不会撤 MM。没有单独 receiver-drop 或外部 `drained=true` 参数。

MAC 服务使用独立 MAC-top／MAC-A banks（前 `0x13884000/0x13885000`，后 `0x1389c000/0x1389d000`），不是把 TOP 当作 CSI2 parser。它检查 155 项命名控制字段，包括两个 MAC lane-enable、包转换/DT/VC words、两套 checker 控制及 IRQ enable；不读 packet/IRQ status 或 checker 接收结果。要求目标未使能、没有 PRBS/BIST/reset、checker/IRQ off、没有借用 async CG，拒绝继承 descramble/RAW10 DPCM/forced-DT/bypass/LRTE 配置。

仅改九字段：TOP 普通非 shared-16p、PHY mux unsplit mode（enable 仍 0）、CSI2 ECC/B2P/image/spec2/multiframe/VS1T/early-VSYNC、header、resync/dummy、RAW10 DT identity，以及选定 async/AFIFO split。主 CPHY3 的 `HEADER_MODE/LEN=2/4`，不是把 trio 数 3 当 LEN；前 DPHY 为 `0/0`。共享 split mask 均为 `3 << (2*port)`，CAM_MAIN AFIFO 的 **`+0x7dc`** 不是其 vendor 已偏移子映射的相对地址。只追加 TOP async CG 的 `1 << port`，不搬运厂商 all-port CG/overrun IRQ；原 async debug policy 保留，不启用 test model。

每个写入在 MMIO 前记录 dirty/pending，确认后才更新 receiver expected；失败只接受 exact old/new 观察资格化，再还原 owned 字段，不重做失败写。sensor close 隔离 reset/MCLK 后先恢复 OUTMUX 并确认其 commit，再停止/恢复 analog（若请求仍 pending 则在 OUTMUX 恢复之前拒绝退休），最后恢复 MAC/async 并撤本次 async CG，然后退休 MM/rails，避免 OUTMUX 恢复时先丢 async 时钟。任一冲突/恢复失败保留尚未退休依赖，不 reset/retry。MAC 的 `CONFIGURED/RESTORED` 不是 PHY 校准、packet arrival、IRQ/event/AXI drain 或独占证明。

analog 服务（`x9_camera_phy.h/.inc`）使用前摄 A/B `13880000/13881000`、后摄 A/B `13898000/13899000`；两个 unsplit profile 都初始化并校准两个 banks。要求 analog CORE/LPF/OS_RSTB、calibration/force/scan、digital lanes/BIST/IRQ off，并对命名控制双采样、持续比较本 journal expected。写入仅影响 LP/ALP/VREF、终端电阻、EQ/CDR、16-bit AFIFO、普通 split/dummy 与 reference/source 字段；不重放 AFIFO CLEAR/IRQ CLR 或任意 RO 结果。

校准数据不是 `10160000` 的直接 MMIO。上游 `mtk-devinfo.c` 从 `/chosen`（或唯一 `/chosen@0`）的 **little-endian** `atag,devinfo` 取 `data[reg/4]`。shim 本次启动资格化 CSI0/3 的 nvmem consumer/phandle/cell（offset `1d0/1dc`），只转交两个 word 至 32B `x9-camera-calibration`；全表/设备标识不转交、不打印或写入 git。缺失/长度错误/重复/未知 cell mapping 不给数据；明确存在的 all-zero word 才使用 vendor 默认 termination16，不能把缺失当 zero。

标称 front2160Mbps、main3300.004166Msps 仅用于 preview band 配置，不是测频/SMI 票或吞吐保证。CPHY 的上游 `setting0=0x322` 会覆盖早先 source setter 的 REF_CK_SEL 为0；本实现保留该已确认调用顺序，不能擅自改成32a并声称 oscillator 已持有。DPHY A 使用 upstream oscillator1/gating2。下面的单次 timing 服务已写 settle/trail/prepare/CPHY POST 与 inactive lane-map/deskew/protocol 配置；**PHY lane、AFIFO flush、IRQ/DMA 启动仍未接通**，无 native capture caller 或本机运行证据。

`configure_timing(epoch, &phy_status)` 使用真实 **CKSYS_MM `10040000` / CKMTR_MM `10041000`（DT范围0x20/32B）**：前/后 CSI0/3 的 CKGEN_CK2 ID **18/21**、FENC bit **14/11**，不搬 VLP audio meter ID/address、不触 PLL test clock。要求当前 parent/PDN/CKSTA/FENC 与 MM journal 一致、meter HW-mode/trigger/BUSY off且 DBG ID 空闲，两次采样稳定后才开始；不接管继承 HW 模式或清前任 trigger。软件mode0、load1023、divider1（/2）、source1/ref0；参考26MHz是 vendor hardware contract，**不是独立测得晶振 ppm**。每次发出 trigger 前记录 pending，要求真实 BUSY rise→fall、deadline500us/400次上限、非零非饱和重复稳定 count，再恢复本次控制。BUSY 窗口中只检查 AO meter、CSI parent/CKSTA/FENC；发出前和完成后检查全部 MM/PHY/sensor 依赖，避免慢 secure/HWV guard 在约39us窗口前吞掉起沿。

两次 count 差≤2 tick且独立换算后的寄存器数完全一致才写 timing。`csi_clock_hz=count*26000000*2/1024`：主CPHY data/clock settle=`ceil(57ns*clk)`、prepare/trail enable0，POST valid1；前DPHY data settle=`ceil(100ns*clk)-6`、clock settle0，prepare0/en1，trail=`ceil((floor(224000/2160)-64)ns*clk)`，仅非零才TRAIL_EN。校验8-bit settle和10-bit trail范围，无nominal312MHz或外部频率fallback。例如**仅公式示例**，312MHz会得到主settle18、前settle26/trail13，不是本机测量结果。写4个data HS、2个clock HS及主POST enable后才应用以下固定数字配置；保留非参数字段、拒绝继承LP00-start，PHY lanes/IRQ仍off。模拟校准 pending 只允许 A/B 的 ANA8 `+0x20` 置位；DPHY_TOP 同偏移的 HS timing 不能误发模拟校准。

`x9_camera_phy_digital.inc` 是上述既有 `configure_timing` 的内部实现，不新增用户 ioctl 或独立 reset 接口。前摄 unsplit4 最终 lane-map **LC0=2/LC1=0/LD0=1/LD1=3/LD2=0/LD3=4**，DPHY_TOP `+4` masked value **`0x80403102`**；按上游最后一次 setter 保持 `CK_DATA_MUX_EN=1`，没有任何 LC/LD/HSRX lane enable。两路 DPHY_TOP `SPARE0 +f0=0x31`，不是模拟 A/B 同偏移的 SETTING0。主摄不猜 CPHY trio-swap；上游只在 start 中启用实际 trio0..2，当前仍off。

前摄 >1500Mbps 的 generic deskew 分支即使 sensor 的 init/periodic-deskew support0 也执行：专用 inactive `DESKEW_SW_RST` 已按 1→2us→0→2us 写入并核对，随后 generic DESKEW_EN/LP11_IDLE_EN=1、CODE_UNIT_SEL=2、DELAY_APPLY_OPT=1、DATA_SEL=1、APPLY_SEL/ACC_MODE/INITIAL_SETUP=0；setup1、hold **0x66**（vendor `0x66666666` 被字段 mask 截断）、initial-detect2、periodic-detect0、compare-length3、APPLY_ONLY_1ST_PAT1、patternff/mask0。四路 DELAY_EN/PERIODIC/APPLY 始终0；不搬 vendor 的全开 IRQ，不读/ack RO delay/calibration/IRQ result。`deskew_reset_released` 仅记录观察到控制释放，**不是校准完成、旧 IRQ 历史清除、接收或 READY**；中途不确定 reset 保留 pending，不 late reset/cancel/retry。

`common_get_csi_param` 只是复制 scenario 参数：两个 preview 的 **ULPS/ALP/LRTE support 均0**，编译宏 enable 不改变 sensor support。按对应上游分支资格化并保持这些功能关闭：模拟 ULPS enable/forced CG/escape-sample off，DPHY normal sync-method1/ALP EN/HSRX_SEL0、LRTE idle clocks/init off，CPHY ALP/TINIT 与 LRTE POST encode/replace off。support0 的 ULPS setter 直接返回，不能盲写其 IRQ/CG/escape 参数；这里不是把应启用的模式省略掉。

meter DBG/CON0/CON1 load/CON2 各有before/expected/owned/单笔pending；CON1 count `[15:0]` 写时置零，不重放结果。成功测量立即逆序还原本次meter控制，释放该短meter journal，不占用它到整个预览结束；返回count仅是当时频率观察，**不是持续DVFS/rate lease或SMI吞吐资格**。未消费meter trigger或不确定deskew reset，在OUTMUX恢复之前就拒绝全部依赖撤销；消费后坏count或普通配置读回失败，只允许exact old/new资格化及一次close恢复，不重复测量/重发失败写。close 在OUTMUX之后先恢复meter、停本次owned generic DESKEW_EN/LP11配置（不以此断言收包/worker drain），再逆序恢复owned digital controls，最后还原CTLE force/analog/MAC。失败状态包含 `clock_pending/clock_busy_seen/clock_dirty_mask/clock_pending_field` 与 `deskew_reset_pending/released`；只有timing和数字配置全部成功才发布 `TIMING_CONFIGURED|DIGITAL_CONFIGURED`，不是PHY接收、RAW10、SID80或capture READY。

`configure_ctle(epoch, &phy_status)` 是单次显式最终 override，仍无 native capture caller。前摄的 `hhfront_static_ctle_param` 和后摄 preview 的 `hhmain_cdr_0x9` 都要求 EQ IS2/BW3、DG0/DG1/LATCH/SR0/SR1=0；只改 ANA5 `0xff7f`，保留非参数 IS_RDC bit7。后摄额外将两 bank 的 ANA6/7 CK_DELAY 和 ANA13 两个 SEL_CODE 设9，保留其它 CDR width。遵循上游 CPHY 共用 setter，两 bank 的三个 DPHY delay-force enable word 写 `0x301`、value word 只设 DELAY_EN；**不改 DPHY delay code、不发 DELAY_APPLY 脉冲**。force value 的 RO_DA `[25:16]` 和 EN6 的 RO_AD `[31:16]` 在写入时置零，不重放采样结果；保留 reserved bits。入口要求 force/VAL EN/APPLY 均off，任何外部 APPLY 变化由 expected guard 拒绝，通用 writer 不接受 APPLY mask。

CTLE 复用 analog 的逐字段 owned/pending journal；成功只发布 `CTLE_CONFIGURED`（并给出固定 cdr_delay/eq_control），不是测频、校准新结果、收包或 READY。重复配置 `EALREADY`，未校准 `ENXIO`，不匹配 epoch `ESTALE`，已 OUTMUX `EBUSY`；已开始操作的错误锁存并要求唯一一次 sensor close，不可重试。close 在 OUTMUX 恢复之后、CAL_EN/RSTB/LPF/CORE 撤销之前，先逆 bank/lane 还原本次 owned force enables 和 value EN，要求原值明确为off且不触 APPLY；随后 analog 逆序恢复其它 EQ/CDR 控制。任何读回冲突/未确认写保留剩余依赖，`RESTORED` 仍只表示配置与电源恢复，不是 RO result/IRQ 历史或 DMA drain。

每个 bank 执行 idle200us → CORE1/30us → LPF1/5us → OS_RSTB1/1us → CAL_EN → 25us → 有界状态消费。只有触发前 CPLT0、10ms/10000次内六个完成位全部1且再次保持1，才标记该 bank CALIBRATED；上游 delay 后写 `ANA9.CPLT=1` 的做法不移植。结果/code 不用作普通控制 guard；完成后 CAL_EN 仍 on 时持续检查完成位。入场已有 CPLT 历史不猜 W1C，也不清零。任何已 posted 但未确认的校准阻止全部接收/MM/rail 退休，不 reset/cancel/retry；已完成才能单次逆序关 CAL_EN/RSTB/LPF/CORE、等待200us并还原 owned 配置。RESTORED 不表示 RO 校准结果/IRQ history 已回到入口或全链 drain。

OUTMUX 引擎只接受选定 bank 的 TOP CG 原为 off、当前 IRQ policy 为 AP/VM0 且其它 VM 未选中、DL/ready/reset/IRQ 与所有 tag filters 关闭、无 crop、所有已知 shadow/inner 配置一致且双采样稳定；两 bank 的 CFG_DONE command/status 和 IRQ_TRIG 必须为0。拒绝继承的 staged difference，不替其它 owner 发布配置，也不修改 VM routing。它保留所有 8 个 tag 的完整已知控制，只改普通 pixel mode、unsplit source、reference/last VC、tag0 VC/RAW10 format/word-count size/filter 的固定字段，并只追加 TOP outmux0 CG bit。

唯一 thread-only STATUSX 采样者在 `camera_control_lock` 内工作，**不注册 GIC、不启 AP/CCU IRQ**。按厂商 `INT_WCLR_EN`（CSR_CFG_CTRL `+0x20 bit0`）设置/资格化 WCLR 后才读 **outer `+0x54 IRQ_STATUSX`**；`+0x44` 是 AP status，`+0x24/+0x28` 是 debug，不作为 raw alias。已 WCLR 的 policy 只借用，未 WCLR 的两 bank 仅 journal/改变此 bit，不改其它 ready/DL controls。inner writable aperture 在上游有实际调用，但 **inner WCLR 的立即/镜像行为没有本机运行证据**：每笔只能接受完整 prior/target 控制值，显式写的 bank 必须双采样到 target，peer effect 只从实际读数记录；不能假定镜像或直接写 inner route/tag 来伪造消费。读回不能证明时保留依赖，不发 CFG_DONE 来修复 policy。

入口 raw 两次为0才建立 clear baseline；若稳定且**只有 CFG_DONE bit0**，它仅是 stale history，在完整 quiet/matched 控制资格下只 W1C bit0 一次，再确认两次0。任何其它/混合事件（包括 frame/tag/error）无 owned producer，记录原值并 terminal retain，**不 ack**。`CFG_DONE` 写出前以 sensor epoch 和不可复用的 command sequence 记录 pending。10ms/10000次内须同时看到 clear baseline 后的新 CFG_DONE，以及**两次全部实际 outer/inner target 匹配、command/status idle**；才允许一次单 bit W1C。W1C pending 在 posted write 前登记，只有实际两次0及仍保持 target 才报告 CONSUMED/completed sequence。先到事件可等待 target；无 W1C 却自行消失、确认 target 后回退或 ack clear 后重现都是 terminal conflict，不被后来的读数掩盖。

唯一 sensor close 可资格化 consistent timeout 的晚到真实 target/event 或 W1C 清零，**不重发 SET、trigger 或 W1C，不把 prior 当退休**；初始 TOP CG SET 若未确认，也须两次实际 target=on 才可开始退休，prior/off 不能被当作已撤销 SET 而卸依赖。未知事件或已观测 control/authority 变化 terminal latch，即使恢复原值也不继续。未发 CFG_DONE 的 shadow failure 只恢复 owned shadow；已消费 route 的恢复同样需要新的 command/event 和完整 target，再退休历史、恢复 owned WCLR policy，最后确认本次 CG 实际关闭。初始 snapshot 保留，receiver expected 只随真实确认更新。WCLR 恢复后不再读可能 read-clear 的历史，也不追加 CFG_DONE。

`RESTORED` 表示配置/policy/CG 恢复；嵌套 kernel-private IRQ status 的 `HISTORY_RETIRED` 才表示**本 scope 实际采样到的 masked CFG_DONE 历史**已退休。mode setup 若在首次 raw read/trigger 前失败，可只恢复已改变 policy，不能凭空设置 HISTORY_RETIRED。flags 是里程碑记录，command/completed、mode/ack pending 和 raw before/after/sample count 均为缓存，没有新增 ioctl 或 capture READY。这不是 GIC/IRQ worker/DMA/PASS1_DONE drain；未来 native capture 仍须取得实际 GIC 来源和全链 producer owner，不能把配置事件当图像完成。

这套生命周期可显式开启 SMI supplier/larb clocks、inactive analog 校准、数字HS/lane-map/deskew/protocol和最终 CTLE 配置，但不启用 digital PHY lanes、SENINF DL、IRQ 或 DMA。CG/VM0/稳定 disabled 条件只是配置准入，不证明独占 AP/SCP、采集速率、包接收、全写链 drain 或 IOVA 授权；共享 CG 的 masked RMW 与检查也不是跨固件的原子 lease/ABA 检测。完整LARB/SMI port/common数据通路配置、持续rate authority、PHY/AFIFO有界start-stop、正常 SID80 attach、唯一IRQ owner与真正采集仍缺失，SMI clocks/analog/deskew配置也尚无本机运行证据。

### CAMSV0 inactive RAW10 硬件配置

`x9_camera_sv_hw.inc` 实际接在 `configure_outmux` 的 route 消费之后，不是另一个未调用的 capture adapter。固定 CAMSV A/id0 的三个合法 bank 与 inner views（`3a600000/3a601000/3a602000`，inner `+8000`），只写十个字段：tag0 `GRAB_PXL/GRAB_LIN/FORMAT`、A0 `BASIC` 的 stride/MAX_BURST、四组 tag mask 和 FIRST/LAST。前后摄均采用原 pipeline 的 packed RAW10/start0；FORMAT 的 UNPACK/MQE/UFE/QBN 等功能清零，保留 reserved 位；BASIC 的 reserved 位保留。group0/first/last=tag0，group1～3=0。**不写任何 DMA base/IOVA、FBC enable/RCNT、CMOS/VF、IRQ enable 或 CQ**。

准入双采样 central/DMA/SCQ 的命名普通配置，必须稳定且 outer/inner 完全相同，防止 DB toggle 消费继承的其它 staged tag/address/CQ 配置。所有 DMA/IRQ/CMOS/VF/CQ-threshold/reset/trigger 必须关闭，八个 FBC 的 `RCNT_INC bit7 / RESET bit8 / LOCK bit12 / SUB_EN bit14 / EN bit15` 必须0。receiver 的 FBC 准入也补上了原来漏掉的 bit14。全局 DMA SLC_CTRL 是 **`+102c`**，tag0 的才是 `+123c`；不会按错误全局 offset 读写。只读普通 controls，不读 DONE/ERR/SOF/COMMON/CHANNEL/FBC1/VF_ST/FIFO_STAT/CQ status；PDP/STG 和其它 CAMSV instances 不配置，不以这三个 bank 的 inactive 状态宣称全局 owner。

每笔 outer 写之前登记完整 next、pending/dirty，actual target 双采样后才确认；inner effect 无论来自 DB_EN=0 的 live-through 或后续 DB transfer，均只从实际读数接纳与发布，观测 target 后回退即 terminal。只按上游实际 `mtk_cam_sv_toggle_db` 的 **DB_EN down→up** 路径；初始 DB_EN=0 则跳过无效 down，不发送 posted no-op，不用 FORCE_LOAD/SWITCH_FORCE/inner 数据写制造消费。每个 DB phase 明确区分尚未 issued、已 posted pending 和实际消费，**两 bank 的 DOWN target 双采样后才允许 UP**；shadow/DB timeout 的唯一 close 只观察迟到 target，已发 phase 不重放。必须完整两 bank 目标双采样才记录 command/completed，这只证明 inactive 配置，不是 CFG_DONE 或图像完成。

关闭先在完整 MM/SMI/MAC/PHY/OUTMUX 时钟资格下退休 CAMSV pending。若在首个 DB command 前 shadow-only 失败，须先独立消费完整 actual partial target，不能以旧 inner/prior 假定早先写入已取消。随后逆序还原本次改变的十字段，再以独立 DB down/up 消费原配置，最后恢复原 DB policy，整个 ordinary snapshot 必须 actual 回原。借用字段不写、不恢复。失败/unknown control/authority/producer 变化保留尚未退休依赖，不 reset/cancel/retry。receiver expected 只发布 journal 接受的 actual bank 值，包括尚未结束但已实际观测的 posted BASIC target；不能先以旧 expected 拒绝自己这笔写，再假装恢复成功。原快照保持证据。kernel-private OUTMUX status 嵌套 `sv` 保存 before/target/actual/sequence/pending/DB policy，并绑定同一 sensor epoch，无新 ioctl/READY，close 日志只输出缓存。

这是实际 inactive 接收配置，**不是可拍摄链路**。正常 SID80/HYP delegation、合法 IOVA、持续 MMQoS/rate、PHY/AFIFO 启停、唯一 GIC owner、CAMSV DMA/PASS1_DONE/cache/worker drain 与 native→iOS preview/photo 尚须完成；没有本轮硬件消费或 RAW10 运行证据。

### 接入 CAMSV 的 GIC 与完成线程

**尚无 native capture caller，不能拍摄；此路径不新增 ioctl、READY 或供测镜像。** `configure_outmux` 在实际 CAMSV 配置消费后调用 `cam_irq_prepare_locked`，只核对本次启动的拓扑、物理 MMIO alias 与当前 GIC 配置，准备 dormant worker；不读取 IRQ 历史、不修改 GIC prior、不启任何 IRQ/DMA source。

shim 的 `camera_irq_handoff.c` 从本次 LK FDT 一次完整 capture 验证 root→soc→CAMSV 的父节点继承/覆盖、唯一 GIC phandle、`#interrupt-cells=4`、十个命名 bank 和三笔中断 specifier。`/soc` 可以合法继承根节点，不能要求它必须重复 interrupt-parent。第四 cell 必须0，SPI cell 必须加32得到 INTID；拒绝 alternate parent、interrupt-map/extended、错误 bank 或 specifier。只向 `/chosen/x9-camera-irq-layout` 的唯一64B placeholder 复制拓扑，不传设备身份/转储或 MM-SMMU owner 标志。无效 FDT 保持全零；缺失或旧 ADT 不偷偷回退到 captured 常量。

| role | INTID | 该 handler 的单次 read-clear 采样 |
|---|---:|---|
| hybrid | 653 | inner ERR `+350`、outer DONE `+348`、outer SCQ `+201c` |
| SOF | 659 | outer SOF `+360`、CHANNEL `+370`；同时锁存 inner FIRST/LAST、四 GROUP_TAG 与对应 FH_SPARE |
| debug | 665 | outer COMMON `+368`、TOP `+378`、DMA FIFO_STAT `+104c`；**不重复读 CHANNEL，不写 debug selector/CCU** |

正常 controller 才能调用 `cam_irq_begin_locked`（同一翻译单元、已持控制锁）或独立包装 `x9_camera_camsv_irq_begin`。该调用先经真实 `.check`/export backing 与逐页 CIVAC 绑定一个 tag0/group0 request，再发布并确认其 inactive 地址/FH metadata，以及无信用 FBC_EN 初始化（见下两节），才对这三路已 disabled/pending0/active0 的 Group1NS、level IRQ 保存/配置 route 与 priority。`.check` **还必须证明 AP 而非 CCU/他方的 IRQ/tag0 FBC policy、prior source/request 历史已实际退休**；GIC masked、拓扑正确、PM_GET、FBC_CNT0 或 all-zero 不提供此授权。每笔 prior mutation 前复核整个已消费 route/priority 集合及真实 owner，允许 unresolved posted prior/target，但 accepted target 不能回退。GIC 只写该三路 route/priority/enable/mask，不改 CTLR/IGROUP/ICFGR/redistributor/timer/IPI；每笔 begin unmask 前再次核对完整 request/地址/metadata/FBC/owner/power/GIC。三路实际 GIC publication 完成后只提交一次 RCNT 命令并确认完整计数/control 接受（见末节）；然后按末节实际确认限定的 source IRQ/IMG0A receive arm 与正常 CMOS/FLUSH/VF；仍不设置 CQ、PHY/MAC lanes、OUTMUX DL 或 sensor stream-on。

SWEET `sleh.c` 保留 timer/IPI、ADSP filter、后备 `interrupt_handler` 和原来的唯一 EOI。camera filter 仅在已绑定的 actual IAR 下先 mask 对应 line，再从永久映射单次采样；不用 mutex、owner callback、缓存/PTE/RAW 访问。SOF 保持 masked 直到下一 request，hybrid 可以独立到达。DONE 只使用该 request **先前 SOF 锁存**的 group/last/inner sequence，缺 SOF 或迟到/歧义绑定直接保留，不能在 DONE 时晚读 group 猜 frame。四槽有界队列不能覆盖旧事件；溢出、错误/unknown/debug 或丢失 power/config 立即 mask 并保留。

worker 在线程上下文调用已有 `x9_camera_camsv_finish`；CQ-only/input 事件不进入 frame copy。首个真实 DONE 若抢在 sensor stream-on ACK 后的 core flag 发布之前，最多等待500ms的实际发布事件，不伪置 STREAMING；stop 先失效 generation/唤醒该事件再 drain，所以不与控制锁形成循环等待。前后各有固定独立 slot/backing 历史，IRQ lane 仍单 request 串行。完成或明确背压/并发 stop 的 RETIRED 只 mask，不增 FBC/RCNT、不自动重写 RAW；前32笔事件及错误输出缓存日志，避免每帧持续打印。

receiver close **先**停止 filter 接受新采样，再排空 queued/running worker pin（500ms界限）和实际 GIC RWP/PENDING/ACTIVE（1ms观察、两个 idle 样本），之后才恢复 CAMSV/OUTMUX/PHY/SMI/MM/sensor。每笔 route/priority 恢复仍复核 owner 和完整 prior/consumed set；不写 ICPENDR/ICACTIVER，不重发 EOI、不吞未退休 source 历史。未完成或 RETAINED RAW slot、未消失 GIC event、authority/config 变化均阻止撤电。成功只是 **GIC/worker retirement**，不是 RAW/AXI/TLBI/unmap 或全链 drain；永久 S1 export retention 不变。

### 在 IRQ 发布前绑定真实 DMA 地址和帧序号

**这是地址/metadata 发布实现，不是已启动的 DMA producer；没有 native capture caller 或真实帧。** 本地 UniXNU 的设备入口未提供本板 CAMSV 实现；HakiOS redwood 的地址在 SOF 切换、晚写可能覆盖同一 buffer 的规则不能当作 MT6993 的 FBC 语义。这里只沿 ISP8s 命名 WDMA 地址寄存器和正常 SOF/hybrid 的 inner `FH_SPARE` reader 接入本板 journal，不移植 QCOM 常量、stress buffer 或伪帧。

`cam_irq_begin_locked` 在本次实际 `x9_camera_camsv_arm`/S1 逐页 CIVAC 成功后，调用 `cam_sv_request_prepare_locked`，只写 outer 的三个整字字段：

| kernel-private field | CAMSV view offset | 来自已绑定 request 的值 |
|---|---:|---|
| 10: A0 BASE_ADDR | `+1200` | `(uint32_t)request.iova` |
| 11: A0 BASE_ADDR_MSB | `+1204` | `(uint32_t)(request.iova >> 32)` |
| 12: tag0 FH_SPARE | `+57c` | `request.inner_sequence`，不截断 capture_id、不从晚读 I2C 拼接 |

前后摄均保留固定独立 slot/backing，要求 tag0/group0、同一 sensor epoch、16KiB 对齐且位于实际 DMA range 的非零 IOVA；此前 arm 还必须证明它正是 exported S1 的全部 RAW bytes。发布器保存 immutable request/lease/backing，再在每笔写前复核 owner 与完整 MM/SMI/receiver/MAC/PHY/OUTMUX/CAMSV 集合；沿原 pending/dirty/DB down→up 状态机确认两 bank 的实际目标，之后才能设置 `X9_CAM_SV_HW_REQUEST`。重复相同 target 只观察，不因已有 geometry dirty history 再发一次 DB pulse。这个 flag 只表示 inactive 地址/FH 消费，不授予 source/FBC/IRQ/DMA 资格。

错 epoch、未配置、busy/pending 或不合法 slot 在地址写入前拒绝。实际 arm 后的发布、DB 或 authority 失败保留本次 slot 和全部依赖，不换 sensor、不重试、不把 late prior 当取消。没有 issued credit 时，下一合法 rearm 在改变 S1 request **之前**检查旧 authority，改变之后只核对新的 immutable binding，不能以旧 request callback 拒绝自己的新 frame。尚未 issued credit 的 GIC/worker 退休后，CAMSV close 仍持有同一 request lease，先恢复无信用 FBC，再逆序恢复本次改变的 FH/high/low、geometry 和 DB policy；全部原 snapshot 实际回归才解除本 journal 的绑定，export storage 仍永久保留。kernel-private `sv` result 现为14字段（增加下节 FBC0）并保留 request generation/capture_id/sequence/errno；用户态 RAW10/frame/ioctl ABI 不变。

FBC 的信用限制与全链停写仍须完成：厂商 `mtk_cam_sv_check_fbc_cnt` 内的软件 `fbc_cnt++` 不是新的硬件观察，不能用它证明 RCNT 已消费或单帧写入预算；`VF_CON.SINGLE_MODE` 的字段名也不是 one-shot 停写证明。下面已接入正常 composer 的无信用 FBC_EN 初始化及独立一次 RCNT 接受状态机；不 reset/清全 tag、不用 FBC_CNT0 或 GIC idle 宣称 RAW/AXI/SMMU drain，仍不生成中间供测镜像。

### 正常 composer 的无信用 FBC 初始化

**未启 DMA、未生成真实帧；只有代码与局部编译结果。** 本机 vendor `lib64/mt6993/libccd.so` 的 `camsys_sv_compose` 是 CCD 正常 CQ writer，不是 slave stress 代码。其 `0x50640..0x506c4` 计算 `0x3a600040 + (dev_id-3)*0x10000`，`0x50a00` 加 `tag_id*0x40`，`0x50ad0..0x50af4` 向再加 `0x500` 的目标提交一个 `0x8000` 值。故 CAMSV A/tag0 的实际 FBC0 为 **`0x3a600540`**，不是 `+500`。`0x5325c..0x535bc` 的另一笔提交是 **`0x8080`（FBC_EN + RCNT_INC）**；功能位0x48.bit4可重复提交该 descriptor。`cq_append_with_values` 生成 CQ 描述符/数据，不直接执行 MMIO；本实现不运行或内嵌 vendor ELF，不移植 CQ/global policy，也不假定每笔 RCNT 已被硬件消费。

本次实际 arm 与地址/FH 两 bank 消费成功后，`cam_sv_fbc_prepare_locked` 在同一 owner/cold epoch 下双采样 tag0 `FBC1 +544`，要求稳定、两 bank 相同且 CNT[19:16]=0。这里只选择无信用配置基线，**不是独占、旧请求退休或 AXI drain 证明**；这些仍由真实 `.check` 证明。之后仅 journal outer `FBC0 +540` 的命名字段：EN15=1，RCNT7/RESET8/LOCK12/SUB_EN14/VALID_NUM/SUB_RATIO=0，保留 reserved 位。只用原 DB down→up、完整 actual outer/inner 确认，不写 inner FBC 控制；没有信用、source IRQ/DMA/CMOS/VF/CQ 启动。

在尚未 issued credit 的 FBC 初始化、GIC 每笔发布、rearm 与关闭恢复中，FBC1 都要求 **完整原值不变**（包括 RCNT/WCNT/CNT/DROP），不以计数局部++、读回bit7或新的稳定值重新建立基线。counter/config/authority 变化或未知副作用锁存并保留 slot/供电/映射；初次双采样失败不写 FBC。cold admission 仍拒绝所有八 tag 的 FBC enable；live receiver/OUTMUX 仅在同一实际 journal 检查后豁免本次 tag0 EN15；独立 RCNT pending 期间另有下面的精确命令比较，其它 FBC 命令/他方 tag/所有 source 和 DMA 仍严格拒绝。`X9_CAM_SV_HW_FBC_EMPTY=64` 只表示已确认的无信用 FBC 初始化；缓存保存 fbc_before/after、sampled 和 first errno，不是 READY/frame/drain。

没有 issued credit 时，close 先恢复本次 FBC0，再 FH/high/low 和 geometry，全部实际回原并核原 counter evidence 后才恢复 DB policy/解绑；不为清理发 RCNT/RESET。合法 rearm 保留原 counter evidence，不能暗中加信用或覆写 RAW；未来实际 producer 必须有明确的信用/完成/停写状态转换，不能直接放宽这里的“counter 不变”门。正常 SID80 authority/attach、持续 rate/MMQoS、PHY/AFIFO/source 启停、全 RAW/AXI/map 退休与 native→iOS 预览/照片仍需完成，最终链接通后才出唯一镜像。

### 接通采集回调

`kernel/src/bsd/dev/x9_camera.h` 定义 `x9_camera_platform`：

1. `power` 按 sensor 配置的顺序执行，并记录/恢复共享资源的原状态；失败半途也必须可恢复。
2. `read8/write8` 是 16-bit register / 8-bit data I2C；只在上电及总线归 AP 后调用。
3. `capture(on)` 在 stream-on 前完成 SENINF、主摄 VC1／前摄 VC0 的 DT0x2b 图像过滤、RAW10 DMA、SMMU 配置；`capture(off)` 在返回前停止并排空，即使初始化只做了一半。
4. 注册会分别上电读 ID 并恢复，只有两枚真实 ID 相符才发布 READY。调用发生在线程上下文；所有回调都不可重入相机 API。
5. provider.version 必须为 `X9_CAMERA_PLATFORM_VERSION`（2）；version1 不接受。DMA worker 在真实 completion/slot/metadata 绑定及缓存同步后调用 `x9_camera_deliver(&completed)`，其中 completed 为 version2 的 `x9_camera_completed_frame`；不得在硬中断调用。generation 来自本次 `capture(on)`，time 为帧完成的 `mach_absolute_time`，capture_id 为该代已资格化 owned-request 标识。record/raw 直到返回都保持不变；EBUSY 丢帧并释放硬件 slot，不重放。记录验证不能代替 provider 的硬件、映射或控制生效证明。

控制层在调用 power/capture/stream 前记录 attempted 阶段及原始 generation；半途错误只清理实际尝试过的阶段。stop 先失效 generation/唤醒读者，再退出帧锁等待 capture(off)，因此完成 worker 不会被 start/stop 持帧锁堵住。**sensor 0100-off 或 capture(off) 非零时不调用 power(false)**；0100-off 不确定时也不调用后续 capture(off)，保持映射/时钟/供电依赖并置 FAULT/RETAINED；power(false) 失败也保留 journal。后续 close 不重试硬件清理、不释放保留的 staging buffer、不切换另一 sensor。注册时的 ID 供电恢复失败同样锁存。非零返回不是已经恢复或可以重新开始，只有明确 drain 成功后才能撤依赖。

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

### 每个 immutable request 的一次实际 RCNT 接受

**只有命令接受代码，尚无真实 DMA producer 或帧；issued credit 目前禁止 rearm/配置恢复。** `cam_irq_begin_locked` 在真实 S1 arm、地址/FH/FBC_EN 两 bank 消费及全部三路 GIC publication 之后实际调用 `cam_sv_credit_issue_locked`。每个 immutable request 只执行一次 outer `FBC0 +540 = original EN control | RCNT7` store；先记录 issued，再写。不会发 DB、移植 vendor 条件重复 descriptor 或 fill-to-two 循环，也不会重试命令。

接受必须观察两次完整 outer/inner 目标：原 RCNT+1 modulo256、CNT=1，WCNT/DROP/reserved 全部不变，FBC0 回原 EN control 且 RCNT 已清。10ms 有界等待期间每次复核同一 request/lease、完整已消费配置/供电以及实际 GIC route/priority/enable，要求没有 sampled/pending/active/SOF 历史。每 bank 见目标后不许回原；已见 cleared control 后不再允许命令重现。pending 期间仅为 exact 本次 EN|RCNT 做比较 canonicalization，缓存/public 原样保留实际读值，cold admission 不受该例外影响。

private credit 状态记录 EMPTY/PENDING/ACCEPTED、issued、seen/ack、complete samples、原 counter、exact target/actual control 和首错。`FBC_CREDIT=128` 仅表示实际命令接受，不代表一帧写入、完成、RAW/AXI/map drain 或 READY。timeout、未知 delta、回退或 owner/config 冲突 terminal retain 同一 slot/backing/映射及供电；不能用 EN-off、reset、DB 或 caller `consumed=true` 撤销信用。credit helper 本身不启 source IRQ/DMA；begin 随后调用下面的 inactive receive arm。实际消费/完成/stop/full drain 接通前，任何 issued credit 均禁止地址 rearm 和 CAMSV restore；CQ/PHY/MAC lanes/OUTMUX DL/sensor source 仍关闭；CMOS/VF 由下一节之后的 TG consumer arm 单独接通。

### 真实 source IRQ 与 IMG0A 的 inactive receive arm

**已写控制路径，尚无 native capture caller 或真实 RAW10；不是完整 producer。** 正常 composer 的 `0x54530..0x5454c` 建立 `central+0x40`，`0x54c48..0x54d0c` 随后提交 image DMA 与 `+304/+30c/+31c` 的 DONE/ERR/SOF enables；ISP8s normal backup/restore 和 common enable/disable 使用相同命名寄存器。`cam_irq_begin_locked` 在一笔实际 credit 完整接受后直接调用 `cam_sv_receive_transition_locked`，只限制为 immutable tag0/group0：

| outer 寄存器 | 实际 target | 含义 |
|---|---|---|
| `+344 DONE_STATUS_EN` | `0x10000` | group0 SW_PASS1_DONE |
| `+34c ERR_STATUS_EN` | `0x701` | DMA SRAM full、tag0 grab/overrun/DB-load fault |
| `+35c SOF_STATUS_EN` | `4` | tag0 SOF |
| `+040 DMA_EN_IMG` | `1` | IMG0A |

每项先留 issued，再唯一 outer store，逐项要求两次完整 actual outer/inner target；前面三个 source IRQ 接受后才写 DMA_EN。10ms 总截止时间内持续核同一 request/lease、完整已消费 power/config、实际 GIC 与无 sampled/pending/active/SOF 历史。SOURCE enable 与相邻 read-clear STATUS 分开，不读/清历史来制造 idle；CHANNEL/COMMON/CQ/其它 tag/LEN 仍0；该 helper 不写 CMOS/VF，正常 begin 随后由 TG gate journal 写入，PHY/MAC lanes/OUTMUX DL/sensor source 仍关闭。FBC1 必须仍为同一 accepted credit target；任何自然/未知计数变化目前都 retain，不把它假定为帧消费。

原 cold busy admission 不变，live 仅允许本次已 issued 的精确 source/IMG0A target。普通控制和每次实际 receiver DMA 采样都记录 per-bank target 观察，见 target 后不能回 prior，即使后来又读回 target。pending 比较可容纳合法先 prior 后 target 的历史样本，但**比较本身不授权/推进状态，不抹去实际回退**；完整 raw 缓存不变。private receive phase/issued/on-seen/off-issued/off-seen/samples/首错及四项实际值单独保存，不混入原 geometry dirty/DB/RCNT 引擎。

close 在 GIC mask 前，先完成下一节限定的实际 TG stop，再最多一次 quiesce 这个输入仍未启动的 receive arm：按 normal disable 的限定子集顺序 DONE→ERR→SOF→DMA，每项唯一0 store及两次完整两bank消费。partial/未知/活动 arm 不通过盲写 disable 修复，失败终止并保留依赖。`RECEIVE_ARMED=256` 只描述四项使能消费，`RECEIVE_QUIESCENT=512` 只描述四项实际关闭；**两者均不是 READY、帧、RAW/AXI/map drain**。quiesce 不修改 accepted credit/CNT、S1/cache/slot，不发 reset/DB/RCNT，不完成或撤销未完 RAW；目前 issued credit 仍禁止 rearm/CAMSV restore/释放映射。

PHY/MAC/AFIFO/OUTMUX 的 input-inactive 正常启停现接在后面的 receive-path 阶段；sensor source 的实际发布、owned SOF/DONE 与 RCNT/WCNT/CNT 因果消费及完整 RAW/AXI/IRQ/worker/map 退休仍须继续。正常 SID80 attach、native owner/capture caller、持续 rate/MMQoS 和 iOS 实帧链也未完成。不得把 inactive arm 的局部编译或“四 enable 关闭”替代这些工作，全部采集链接通后才交唯一镜像。


### 正常 CAMSV CMOS/VF consumer gate 与 input-inactive stop

**代码与相机对象编译已接通；上游输入仍关闭，没有真实帧或 native capture caller。** begin 在真实 S1/cache/address/FBC/GIC/receive arm 完整接受之后调用 `cam_sv_gate_transition_locked`。采用 ISP8s `mtk_cam_sv_central_common_enable/disable` 与 `mtk_cam_sv_toggle_tg_db` 的正常命名字段；不移植 global DMA reset、all-tag inner 清零或 stress 单帧假定。

| 阶段 | outer CSR | 单次实际控制动作 |
|---|---|---|
| arm | `+140 SEN_MODE` | CMOS_EN=1，再 FLUSH_ALL_SRC_DIS=1 |
| arm | `+148 VF_CON` | VFDATA_EN=1 |
| stop | `+140 SEN_MODE` | TG_MODE_OFF=1 |
| stop | `+148 VF_CON` | VFDATA_EN=0 |
| stop | `+14c PATH_CFG` | SYNC_VF_EN_DB_LOAD_DIS=1，再=0 |
| stop | `+140 SEN_MODE` | CMOS_EN=0 |

每步骤保存 immutable prior/target，先记录 issued 再唯一 ordinary RW store；原 FLUSH 已1等同值仅实际观察，不再写。10ms 总 deadline 内，每步骤需两次完整实际 outer/inner target；TG DB 的1和0各自必须完整接受，不以未知1被0“取消”来收尾。每次实际采样先留 per-bank seen，再做历史比较；只当前已 issued 的精确 prior/target 允许 pending 跨度，实际见 target 后不许回 prior。重复 SEN_MODE 与 PATH_CFG 步骤独立留账，不能用旧步骤的 seen 当新步骤消费。

S1 arm 之前先拒绝不兼容的 inherited SAT/MSTREAM/sub/downsample/RGBW、VF single/fr-con/delay/mux、JPG/TG DB-disable/escape 模式；不修复 inherited profile。其它 pixel/clock/timing/polarity/reserved 位原样保留且完整核对，兼容性筛查不是 native authority。持续核同一 immutable request/lease、power/config、实际 GIC publication 与无 sampled/pending/active/SOF 输入；accepted FBC1 完整值不变。未知、回退、owner/config/输入事件或超时 terminal retain，不 reset/retry/rebase。此阶段不启 PHY/MAC lanes/OUTMUX DL/sensor source。

close 在 GIC mask 前先关闭后述 input-inactive receive path，再最多一次 TG stop 与 receive quiesce；仅同一完整 ARMED 且输入未开始的 consumer。失败的部分 arm/DB 不能盲写 off 修复。`GATE_ARMED=1024` 只说明这些实际 controls 接受，`GATE_STOPPED=2048` 只说明 gate 实际关闭；stop 保留 TG_MODE_OFF/FLUSH，不恢复 cold profile。**均不是 READY、帧、RAW/AXI/map drain、信用取消或映射/供电释放许可**。issued credit 与 unfinished RAW slot 继续保留，真正 producer 与完整自然计数消费/全写链退休仍必须接通后才能复用或恢复。


### 同一真实 SOF/DONE 的 FBC 计数与 CPU 读取门

**新增的是实际 IRQ worker 的必要计数条件，不是完整 producer、写链退休或可拍摄结果。** begin 在全部 consumer gate 接受、输入仍关闭时调用 `cam_irq_fbc_bind_locked`，额外两次实际读取 outer/inner tag0 `FBC0 +540` 和 `FBC1 +544`，必须精确匹配同一 immutable request 的 accepted control/credit。绑定只进行一次，记录原 RCNT/WCNT/CNT/DROP/reserved，不重立新的稳定基线，不授 native authority。

正常 ISP8s FBC1 字段是 RCNT[7:0]、WCNT[15:8]、CNT[19:16]、reserved[24:20]、DROP[31:25]；正常 SOF handler 会调用 `mtk_cam_sv_is_zero_fbc_cnt`，所以 **CNT 可在 SOF 已减为0，不能要求 CNT 与 WCNT 一起、或都只在 DONE 时变化**。本路径的验收目标是一笔原 credit 的 WCNT+1 modulo256、CNT0，RCNT/DROP/reserved 与 FBC0 完全不变；这是必须观察的条件，尚非本机实测事实。WCNT 与 CNT 各自只接受 prior→target 一次，两个合法字段可分别先变化；未知值、超过一笔、任何回退或 DROP/RCNT 改变都终态拒绝。

真正 SOF/hybrid IAR 的既有唯一 STATUS 采样之后，`cam_irq_fbc_event_locked` 读取上述 ordinary counters/controls，每 bank 立即留账再读下一个 bank；不重读/清 IRQ STATUS、不开其它 tag，不调用 owner/cache/PTE/RAW。队列保留 raw control/counter 和 sampled 两bit（未采样的0不算实际读值）。没有实际匹配的 owned SOF 前，只接受原 counter；DONE 必须使用原 SOF group/last/sequence/time 与同一 immutable request，拒绝重复/foreign/error completion。单请求不更新 outer FH，所以本层同时要求 outer/inner cookie 精确匹配，不采用 next-CQ 解释掩盖变化。

实际 IRQ worker 在 `cam_irq_core_ready` 的真实 STREAMING 发布后、`x9_camera_camsv_finish` 的 CIVAC/CPU copy **之前**，调用 `cam_irq_fbc_done_locked`。仅一次10ms、10000poll上限的 readonly 两bank观察；每次 native `.check` 在 spin 外，真实 power/GIC/config 与 counters 在安全资格内检查，要求两次完整 final targets，最后 owner check 后再实际核 final controls/counters 与 deadline。超时/unknown/回退/冲突保留同一slot/backing/映射/依赖，不重试命令、补 RCNT、reset、取消信用或使用 caller consumed boolean。原 IRQ 日志输出 raw FBC/control/sampled，`X9 CAMERA DONE FBC` 输出 actual/seen/samples/count_accepted/errno。

锁序仍 control→IRQ→slot；新的 record 由 IRQ spin 保护，worker **不取得 control mutex、不修改 SV journal**，避免 stop 持control锁 drain worker 的死锁。**count_accepted 仅表示该 DONE 的计数观察，不是 RAW/AXI/map drain，不能复用或撤 backing。** control-locked SV 的 cold/no-source strict guard 保持；实际 source issue 后按下一节在同一 IRQ spin 内读取并记录 immutable live credit。自然消费后 stop/full bus/map retirement 与合法 repeat 仍须接通。下述 PHY/MAC/OUTMUX input-inactive 通路不补 normal NS SID80 attach、持续 rate/MMQoS、native capture caller 或 iOS 实帧链，因此不出中间镜像、不声称已拍摄。

### 实际 PHY/MAC/OUTMUX 接收通路与 input-inactive 关闭

**代码已接到真实 begin/close，不是 sensor stream-on、完整采集或可拍摄结果。** `x9_camera_source.inc` 的 admission 在 S1 arm 前检查同一 epoch 的 MAC、PHY calibration/timing/digital/CTLE、OUTMUX route 消费及 SMI clocks/A0/shared 全部已配置，无 pending/error；经现有 AP engine 实读两次 sensor `0100=0/0104=0`。这些配置 flags 与 standby 只作前提，真实 DMA `.check` 仍须独立证明完整 normal domain/route/rate/PHY/AP IRQ 与 retained range，不能用空闲或 nominal pclk 授权。

begin 在 accepted FBC bind 后，直接执行一次以下固定正常字段序列。依据是 ISP8s PHY3.0 `csirx_mac_top_setting/mac_csi_setting/dphy_setting/cphy_setting`，不是旧 Qualcomm PHY 或 stress/reset 路线。

| 顺序 | 前摄 DPHY4/4D1C | 后摄 CPHY3 |
|---|---|---|
| 1 | MAC-top `+40` mux mask3 → DPHY1 | 同字段 → CPHY2 |
| 2 | MAC-A `+0` lanes maskf → f | 同字段 → 7 |
| 3 | DPHY_TOP `+0` LC/LD maskf03 → f01 | CPHY_TOP `+0` LP trio mask7 → 7 |
| 4 | PHY-A `+100` FLUSH_EN mask101 → 101 | CPHY_TOP `+168` valid-pipe mask7 → 7 |
| 5 | PHY-B `+100` FLUSH_EN mask101 → 101 | OUTMUX outer `+20` DL bit12 → 1 |
| 6 | OUTMUX outer `+20` DL bit12 → 1 | — |

前摄要求已接受的 lane map 精确为 `80403102`。后摄**不复制前摄 AFIFO enable**。AFIFO `+100` 的 FILL_0 是控制，不是 RO；FLUSH_FLAG_CLEAR bit7/15/23 永不重放，真实 RO FLUSH_FLAG 在 `+104`，本阶段不读它或冒充 AXI drain。

独立 source ledger 为每项保存 immutable prior/target、issued 与每 bank seen；先留 issued，再唯一 masked ordinary RW store，保留其它 controls。每次 receiver/MAC/PHY/OUTMUX 实际采样在下一读之前记目标，目标不能退回 prior；仅当前已发字段允许 exact prior/target 的历史比较跨度，第三值与任何真实回退锁存。非 OUTMUX 字段需两次完整实际 target，DL 先确认 outer 两次，然后**复用现有 `cam_out_commit`**：独立唯一 `+1c`，fresh WCLR-qualified CFG_DONE 与全 inner target，再单 bit W1C 退休，最后两次完整 both-bank target。不写 inner DL/tag/route、不扫历史或重触发。嵌套 commit/ack 的每次 full guard 也检查同一 native request 与 actual input-inactive GIC，callback 不在 spin。10ms 总 deadline 包含嵌套 commit 和最终 guard。

close 在 TG/receive/GIC 之前，限定逆序关闭同一**完整** ARMED path：DL0及其 fresh commit → PHY/AFIFO → MAC lanes → mux，再原 TG stop/receive quiesce。失败的部分 arm、未知 commit、sampled input、自然计数变化或 owner 冲突均保留原 slot/backing/映射/供电，不补 enable/off/reset/RCNT。core 的 `session.stream` 在 `0100=1` **发出前**已记录，任何该 attempt 都禁止直接套用这个 inactive close，即使尚无 SOF 服务；下面新增的 streamed-stop 分支还要求独立实际 OFF/owned DONE/CPU/worker/GIC receipt，不是 caller drained boolean。source ledger 不修改 PHY cold owned bits，也不授予 credit 取消、RAW/AXI/map 退休或合法 repeat。日志 `X9 CAMERA RX PATH` 记录 epoch/request/sensor/off/phase/issued/pending/errno。

真实 sensor source publication 与 live immutable FBC 观察已按下一节接线，仍必须完成 actual source-on 后 stop、full RAW/AXI/IRQ/worker/map 退休及 repeat、normal NS/prior-preserving SID80、持续 rate/MMQoS、native owner/capture caller 和 iOS 前后实帧预览/照片。现有对象编译不能替代这些，完整链就绪后才构建唯一最终镜像。


### 通过同一 AP/request 发出 sensor stream 命令

**代码接线和局部对象编译，不是 native capture provider 已注册或相机可用。** core `write8(0100)` 现在调用 `cam_stream_set_locked`；只有同一 configured sensor epoch、完整 source path、原 S1/request/generation/FBC credit 和 actual power/GIC/config 资格才能进入。生成表的 generic writer 仍禁止 0100/hold/reset，不借任意 backend 的 write8 返回值发命令。实际使用既有 `cam_bus_transfer` 发 `{01,00,01}` 或 `{01,00,00}`，ON/OFF issued 各只留一次且在 AP START 前留账。

命令前实际读取两次 0100 prior、0104 hold0；ON 另读 0005=FF。正常 ISP8s `adaptor-subdrv-ctrl.c:4295,4360..4415` 的 stop sentinel 为 0005=FF，hhmain/hhfront 也明确 stream register0100、frame count0005。命令后一个500ms总deadline内读取 actual0100 prior→target，只在两次目标、hold0、最后 owner/control 检查后发布 ACK；OFF 另要求两次及最后 fresh0005=FF。见目标后回 prior、异常 control、未知 AP completion或超时都保留原请求，不用反向命令修复。日志 `X9 CAMERA SENSOR STREAM` 分开 prior/command control、frame_count、sampled、issued/ack/seen、phase 和首错；未采样零值不是实际观察。**OFF/FF 是 sensor 协议观察，不是 RAW/AXI/IRQ/worker/map drain。**

source ON 真正 issued 后，原单 credit 的 CNT0/WCNT+1 可先于 SOF 服务被观察；没有 source issue 时仍严格原 accepted 值。control guard 和实际 IAR 均在同一 IRQ spin 内读取并立即记录 FBC0/FBC1，避免旧 counter sample 排到新 IAR sample 后造成假回退。所有 native owner/cache/PTE callback 在 spin 外；不更改原 accepted baseline 或 SV expected control。DONE/CPU 必须同时有 source ON ACK 与真实 owned SOF/DONE；计数不能制造 IRQ。CPU slot RETIRED 后继续观察同一 live credit，以便停流资格检查，但不能取消 credit 或授权释放。

0100-off 不确定时 core 不再调用 capture-off/power-off；原45 inactive close 不因 STOPPED/FF 获得豁免；下面的独立实际 streamed-stop receipt 接通完整已完成请求的控制停流。全写链退休、normal NS SID80、持续 rate/MMQoS、native provider 注册及前后 iOS 实帧预览/照片继续实现；链完整后才出唯一最终镜像。


### 停止已经 stream-on 且真实完成的同一请求

**这是实际 source/TG/receive 控制停流，不是 AXI/map drain，仍无 native provider/实机结果。** `cam_irq_close_locked` 对 recorded stream attempt 先调用新 `cam_stream_close_begin_locked`，不得直接使用45 inactive资格。只在原 ON/OFF issued+ACK+target observations、sensor STOPPED、真实 owned SOF/DONE、两bank原单credit WCNT+1/CNT0、原slot CPU RETIRED、worker pin已排空及实际GIC enable/pending/active全0、同一native `.check`/request/mapping/power/config都成立后允许进入。两个fresh AP样本再要求0100=0、0104=0、0005=FF；等待worker和AP共享一个500ms总deadline，等待时释放IRQ mutex，不能拿着worker所需的锁等待它。无SOF/DONE、早停/被取消的未完成slot或外来pending都保留，不清历史制造成功。

实际关闭顺序仍是唯一逆序 source **DL0+fresh CFG_DONE→PHY/AFIFO→MAC lanes→mux**，然后正常 **TG_MODE_OFF→VF0→TG DB down/up→CMOS0**，再原 restricted **DONE/ERR/SOF→IMG0A DMA0**。每一store、嵌套OUTMUX commit/W1C以及目标poll仍核同一独立receipt和实际counter，native callback不在spin，不写inner/RESET/新RCNT、不改变immutable accepted baseline。new receipt的checking不是publication，只有两次fresh观察完成才admitted；partial或首次错误不可重试。cold/on分支仍原credit/ARMED/no-input门，不按off布尔或CPU RETIRED独自放宽。

controls完成后才标 `X9 CAMERA STREAM CONTROLS` 的stopped并进入下一节普通DMA停止观察，再原GIC/worker关闭；**原issued credit、S1 backing、namespace及所有supplier保留**。本control receipt单独仍不能恢复地址、unmap、撤电或repeat；普通CAMSV配置恢复另需后文真实自然spent receipt。原close后续worker timeout不覆盖更早的source/control首错。GIC route/priority逆序恢复期间，live FBC只在controls完整receipt后改用原dirty/uncertain精确journal比较，不能因本次已合法恢复第一项prior就用旧all-target门拒绝自己；普通close标志或masked line没有该豁免。完整下游AXI/SMMU退休、合法复用和native/iOS实帧闭环仍须实现；不要把此receipt当全链STOP成功，不制作中间镜像。

### 正常 CAMSV0 DMA-top 停止与必要下游观察

`x9_camera_dma_stop.inc` 已接到实际 `cam_irq_close_locked`：完整streamed-controls receipt后、GIC恢复前执行。依据普通 ISP8s `common_disable` 的 DMA DCM inhibit（outer `+7c=1`）及 `sv_reset` 的 DMA-top `+1020=1`/poll ACK bit1，不搬它后续STG/CQ/central/inner/debug reset。当前仍无实际native `.check` 注册者；接口要求独立证明exclusive CAMSV0 DMA-top stop scope，并排除其他image/LEN/CQ users，不能凭空闲CSR自授权限。

首先实际核两bank原stop CSR为0、DCM prior仅0/1。outer DCM prior0时先记issued，再唯一store1并两次实际readback；prior1只borrow、不重写/释放。随后先记DMA issued，再唯一outer request1；100ms单总deadline覆盖DCM、ACK、SMI观察及最后guard。vendor只要求ACK bit1，未证明request bit0保持，所以ACK完整值2或3均合法；见ACK不得回0/1，见ACK2后bit0不得再出现。reserved/其他reset bits拒绝。inner可保持原值，不猜inner ACK或写DB/inner。actual在下一sample/比较前记录，不修改原SV expected、accepted credit或S1 range。

ACK后要求实际held的同epoch L19/P0 mapping、SMI/A0、原native request/power/config；读取普通`mtk-smi.c` external `+280`、internal `+780` monitor pair，两次0及最后fresh pair0，保存raw/sample masks。每store/poll仍核同一actual source/TG/receive/PHY/MAC/OUTMUX/FBC/S1。首错/回退/timeout保留请求和依赖；失败请求不重复、不release/clear/reset或补RCNT。`X9 CAMERA DMA STOP`输出issued/DCM/ACK/sample/OSTD/errno。

成功的原stop现在继续实际恢复本次局部 DMA 控制，仍在 GIC priority/route unwind 前、同一 control/IRQ mutex 和 request/lease 下。先重新验证完整原 OFF/DONE/CPU/worker/GIC/两bank实际 WCNT+1/CNT0，并取得两次新 ACK/P0 monitor0；单个100ms deadline包含全部restore操作。按上游 `sv_reset` 的局部终序：**只对自己曾改变的 DCM inhibit 写一次原值并确认两bank目标两次，再对自己已 ACK 的 DMA-top stop 写一次原值0并确认两bank目标两次**。借用DCM1不写、不释放；不搬central SW_CTL/STG/CQ/inner reset。所有 issued 先于唯一store，pending只允许原已消费状态到精确prior，观察到prior后再出现请求/ACK或DCM目标回退即失败，不能靠重发修复。初次restore失败锁存、任何晚到目标不重置错误。`X9 CAMERA DMA RESTORE`保存phase/issued/seen/sample/实际DCM-stop/OSTD/errno；RESTORED只描述这两项局部控制，不是credit/map/namespace退休凭证。

**DMA ACK、port zeros及局部控制恢复都不是full downstream AXI/SMMU/EMI/map退休。** 不取消原issued credit，不释放request/S1/supplier，不允许repeat；CAMSV close仍拒绝没有真实spent receipt的issued credit。仍必须实现完整退休、normal NS controller/prior-preserving SID80 attach、持续rate/MMQoS、actual native provider和iOS前后实帧链。只有全链完成后才构建唯一最终镜像。

### 在 sensor 隔离前停止 consumer，确认自然 credit 消费

**这是可编译的真实局部停机实现，不是已注册 native capture 或完整 DMA drain。** 新的内核入口 `x9_camera_camsv_irq_stop(epoch)` 获取 control mutex；同TU的native callback用 `cam_irq_stop_locked`，不能重入公开入口。必须在原owned sensor OFF已完成、sensor仍configured/AP可访问、`session.power/capture/stream`仍保持attempted时调用，先于sensor-close的reset/MCLK隔离。它实际执行原stream receipt→source/TG/receive逆序停机→DMA stop/DCM恢复→GIC priority/route恢复→自然spent确认。若只等power-close再执行，capture已被清除且sensor已隔离，原fresh AP资格必然失败；不能通过放宽这些门来修复顺序。**目前没有实际native caller**，后续provider须调用此入口，并继续独立full bus/map drain成功后才能让capture(off)返回0。

`x9_camera_credit_retire.inc` 不接收“consumed/drained”布尔，不写RCNT、FBC1、RESET或重新建立accepted baseline。只有原epoch/request/lease、真实owned SOF/DONE、count_accepted、两bank原单credit WCNT+1/CNT0、CPU slot RETIRED与实际S1 `RAW_IDLE`、worker busy/count0、GIC enable/pending/active0及全部route/priority回原、source STOPPED、TG/receive停机、DMA stop/DCM已恢复并fresh P0 external/internal monitor0时，才进行两个新AP OFF0/hold0/frame-countFF样本。100ms总deadline覆盖owner、PTE/S1、GIC/SMI与AP操作，末次仍重采两bankFBC；首次attempt先留账，错误终态锁存SV/IRQ/slot并保留所有依赖。owner callback在spin及slot/S1锁外，锁序为control→IRQ→slot→S1。

成功只保存不可变spent counters与 `CREDIT_SPENT`：**`credit_issued=true`、原 `credit_phase=ACCEPTED` 和原native lease/S1仍保留，begin仍拒绝新credit/rearm**。失败保持IRQ prepared/close-attempted，不能被下一次prepared=false快路径吞掉；成功后的power-close也重新核原spent receipt，不重发物理停机。每次spent后的SV environment（包括DB/field目标poll）仍重查原native authority、GIC原配置、slot/S1与actual counters/stop/DCM/P0；不是只在第一store前查一次。

普通CAMSV close仅在该receipt后允许恢复自己的原FBC control；双bank消费后，唯一outer SEN_MODE store只还原自己改过的TG_MODE_OFF/FLUSH字段，CMOS/VF始终0，PATH/TGDB必须已回原。TG restore使用独立RESTORING/RESTORED、issued-before-store、双bank两次目标/seen及10ms总deadline；见原值后再回stopped值、未知位、timeout均不可重放。随后只还原原FH/address/geometry和原DB policy（除正常DB_EN down/up外不得改变其它policy）；不写inner/新TG DB/reset/credit return。历史receiver comparison可跨本次精确FBC/SEN变化，但live spin sampler仍拒绝任何counter变化或target回退。

私有status和日志 `X9 CAMERA CREDIT SPENT` / `X9 CAMERA TG RESTORE`记录实际phase、attempt、issued、seen、samples、count与errno；旧用户态status/STOP/frame ioctl ABI未改。**局部配置restore不授予namespace/unpower或合法repeat**。normal host/current STRTable+CMDQ映射及跨firmware排他、full AXI/SMI/SMMU/EMI退休、native power/I2C/config/capture注册、持续rate/MMQoS和iOS真实前后RAW preview/photo仍继续；不出中间镜像。

### 保留共享 normal NS 控制器的 SID80 安装/退休

`x9_camera_smmu_attach.inc` 新增 `x9_camera_smmu_attach`、`x9_camera_smmu_attached_check` 和 `x9_camera_smmu_detach`。不是把已有private whole-table模板写进共享控制器；**当前没有实际host/capture注册者，没有SID80实际attach或拍摄结果**。本路径使用当前已启用的NS SMMU，只初步支持linear LOG2SIZE8 STRTAB、LOG2SIZE2..8 CMDQ；能力不符返回错误，不reset/bootstrap、换表/队列或回退到Android转储。

host的enter/guard/leave必须真实取得跨所有CPU/firmware producers的STE/queue序列化，并独立证明正常NS权限、SID80/SSID0/ASID1独占、actual power/cache及私有RAW/CD/PTE backing。传入的是常驻、当前coherent的真实表/队列CPU映射；逐16K页用pmap核实际PA对应，拒绝alias私有RAW/PTE/CD/queue和EVTQ。INSTALL要求原endpoint/DMA/history已退休；CHECK允许本stream运行；RETIRE另须完整source/AXI/IRQ/worker退休，不能以47/48 controls、ACK/port zeros或CPU IDLE代替。callback不能回调camera API，leave只退出serialization，不释放依赖。`export.map`描述真实自有RAW/CD；export里private STRTAB/CMDQ/EVTQ地址在此路径从不安装。

先保存全部256个原STE及实际CR0/ACK、CR1/2、GBPA、IRQ/error/event和三个base/config，核原CMDQ producer=consumer、无未消费event/global error。STE80只接受全零invalid或canonical ABORT（word0=1、其余0），不接管已有bypass/live translation。display90/protected89/其它STE完整raw保持原状；每guard重核所有非目标STE、原控制器配置、actual queue单向消费及原backing，不改其它stream的CD/PTE。

实际安装：atomic64清V→PoC clean→targeted CFGI80/SYNC消费；只写STE80的私有CD描述→valid-last→CFGI80/CD80/SYNC→TLBI NH/ASID1/SYNC，最后fresh guard。全部命令直接写当前真实CMDQ的已消费空slot并clean，issued/pending先于唯一actual PROD store；原producer/wrap不归零，两个actual目标CONS及最后fresh record/config/lease/deadline核对才消费该batch。不写CONS/GERRORN/EVTQ/IRQ、global TLBI/CR0/GBPA/STRTable/queue bases，不whole-table copy、cancel/retry。一个100ms总deadline包含所有BBM/store/cache/batch/最后观察，unknown/第三值/foreign producer/error/timeout终结并保留暴露资源。

实际退休：只有host RETIRE证明和原RAW cache PRIVATE/IDLE等必要条件成立后，invalid-first→targeted CFGI/CD/TLBI/SYNC→只还原原STE80→最后CFGI/SYNC；不释放已export的RAW/CD/PTE或共享supplier。`x9_camera_smmu_check`在shared attempt后走保留host的真实live check，原private own-base CFGI/TLBI/SYNC入口拒绝，防止往未安装的private队列写入。日志`X9 CAMERA SID80`记录operation/phase/batches/pending/actual PROD/CONS/samples/errno；这些观察不是READY或完整capture-off成功。

下一仍要提供实际normal host权限/当前映射来源、完整DMA→SMI/SMMU退休与credit/repeat、持续rate/MMQoS及native power/I2C/config/capture注册，最后接iOS前后实帧预览/照片。此安装算法不能冒充provider，不构建中间供测镜像。
