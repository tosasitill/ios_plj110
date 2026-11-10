# X9 前后相机初步移植

**已实现控制层、RAW10 帧接口、iOS 双流桥及有条件的 MT6993 sensor-probe；采集后端仍缺失，不能拍摄。**
没有假帧、测试图案、Android 代理或 Camera.app 成功状态伪造。默认关闭。

## 实现边界

| 层 | 当前实现 |
|---|---|
| 传感器 | Android 日志确认的 `hhmain_mipi_raw` / `hhfront_mipi_raw`；ID 检查、init/preview 表、stream-on/off、曝光/帧长、Sony 倒数增益与线性 Q10 转换 |
| XNU | `/dev/x9-camera` 独占控制；只读状态；16 MiB 上限 RAW10；1 s 有界等待；generation 拒绝旧 DMA 完成；关闭时停止、排空、恢复 |
| 软件 ISP | HakiOS MIT 软件去马赛克、灰世界 AWB、AE、sRGB、NV12/BGRA；不使用高通 Chromatix；CCM 为单位矩阵，无 LSC |
| iOS | HakiOS `CMDerivedObjectCreate` 设备类 v3/流类 v2，前后 `FigCaptureStream`；输出池、PixelBufferHandler、帧 metadata；先确认真实 start 再返回成功 |
| sensor-probe | MT6993 CAMTG voter/parent 申请与释放、AP I2C 单控制器门控/FIFO/WRRD；WL28681C 指定通道电压/启停；reset/MCLK pinmux；真实 ID 检查与恢复日志。**仍要求已配置的 HWCCF/PLL、AP timing/引脚和共享供电，未上机验证** |
| 采集后端 | **缺失**：完整冷启动 HWCCF/共享 regulator/AP timing ownership、SENINF C/D-PHY、VC/RAW10 接收、RAW DMA/SMMU、完成队列 |

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
| `x9_camera=3` | probe-only，节点 root:root/0600；启动与状态查询不访问 MMIO、不上电、不安装桥；只有 root 的显式 probe ioctl 才访问硬件 |

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
- i2c4/i2c2 和 **i2c12** 的共享 I2C_EAST/I2C_PERI parent 已 FENC-on/CKSTA-idle，选用 PLL 时该 PLL 已 FENC-on；不修改共同 parent。允许只 ungate 目标 IMPE/IMPC bit，退出恢复自己增加的门控。仍要求非零 timing、open-drain、非 HS，四个暴露的 channel bank 均无事务/IRQ/FIFO 残留，DMA 未运行。实际 DT 没有 `ch-offset-i2c`，使用 channel0，不能猜成 `+0x10000`。
- AP I2C 引脚已为 mode1，尤其前摄不能仍归 SCP；WL28681C 共享 reset GPIO203 已拉高。不切换 SCP ownership，不重置 LDO 芯片或 GPIO187/202。
- SID3 LN60_7 DOVDD 已处于精确 1.8V/normal/DA-enabled；主摄另需 SID6 LDC0_3 AFVDD 精确 3V。只读 PMIF-P/M 验证，不改共享 rail 或 VS voter。
- 目标 sensor 的 reset 为 GPIO output-low、MCLK pinmux 为 off、它的 WL 通道均 disabled。主摄使用 WL4/7/2，前摄使用 WL3/1；其他通道保持。

`blockers` 位：`1=clock`、`2=shared rail`、`4=AP route`、`8=I2C state`、`16=inherited active/reset state`。
阶段编号：`1=preflight`、`2=PMIC`、`3=bus`、`4=LDO`、`5=power`、`6=ID`、`7=restore`、`8=done`，追加 `9=clock`（不改旧编号）。
一次 AP FIFO 请求最多约 5ms，PMIF 状态等待最多约 1ms；单次 HWCCF DONE 等待最多 10ms，AP EN 最多 200µs，CKSTA/FENC 最多 1ms。未知继承状态不清 IRQ、不做总线 reset/retry。
失败后先把 sensor reset 拉低、断开 MCLK，再关闭自身 WL 通道并恢复电压/引脚/控制寄存器/单控制器门控，最后撤销自己增加的 CAMTG vote 并在无人使用时还原 parent。继承投票和其他通道保持；恢复失败保留 journal、锁存并明确报告，不声称资源已恢复。
**冷 LK 基线仍可能被共享 rail/WL reset/AP timing 等前置条件挡住；这不是完整的相机冷启动或采集驱动，也不是可拍摄镜像。**

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

## 硬件后端接入契约

`kernel/src/bsd/dev/x9_camera.h` 定义 `x9_camera_platform`：

1. `power` 按 sensor 配置的顺序执行，并记录/恢复共享资源的原状态；失败半途也必须可恢复。
2. `read8/write8` 是 16-bit register / 8-bit data I2C；只在上电及总线归 AP 后调用。
3. `capture(on)` 在 stream-on 前完成 SENINF、VC0/DT0x2b、RAW10 DMA、SMMU 配置；`capture(off)` 在返回前停止并排空，即使初始化只做了一半。
4. 注册会分别上电读 ID 并恢复，只有两枚真实 ID 相符才发布 READY。调用发生在线程上下文；所有回调都不可重入相机 API。
5. DMA worker 完成缓存同步后调用 `x9_camera_deliver(sensor,generation,raw,stride,bytes,time)`；不得在硬中断调用。generation 必须来自本次 `capture(on)`，time 为帧完成的 `mach_absolute_time`。

下一阶段不能绕过 regulator/SMMU 所有权或从 LK framebuffer carve-out 偷分配相机 DMA。
先证明一枚芯片的上电→I2C ID→还原，再证明 SENINF 包→RAW DMA 完整帧，最后验收前后切换、预览和落盘照片。
