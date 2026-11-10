# X9 前后相机初步移植

**已实现传感器控制层、RAW10 帧接口和 iOS 双流桥；MT6993 硬件后端尚未实现，不能拍摄。**
没有假帧、测试图案、Android 代理或 Camera.app 成功状态伪造。默认关闭。

## 实现边界

| 层 | 当前实现 |
|---|---|
| 传感器 | Android 日志确认的 `hhmain_mipi_raw` / `hhfront_mipi_raw`；ID 检查、init/preview 表、stream-on/off、曝光/帧长、Sony 倒数增益与线性 Q10 转换 |
| XNU | `/dev/x9-camera` 独占控制；只读状态；16 MiB 上限 RAW10；1 s 有界等待；generation 拒绝旧 DMA 完成；关闭时停止、排空、恢复 |
| 软件 ISP | HakiOS MIT 软件去马赛克、灰世界 AWB、AE、sRGB、NV12/BGRA；不使用高通 Chromatix；CCM 为单位矩阵，无 LSC |
| iOS | HakiOS `CMDerivedObjectCreate` 设备类 v3/流类 v2，前后 `FigCaptureStream`；输出池、PixelBufferHandler、帧 metadata；先确认真实 start 再返回成功 |
| 硬件后端 | **缺失**：MT6993 时钟/电源/共享 regulator 所有权、AP I2C、SENINF C/D-PHY、VC/RAW10 接收、RAW DMA/SMMU、完成队列 |

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

## 硬件后端接入契约

`kernel/src/bsd/dev/x9_camera.h` 定义 `x9_camera_platform`：

1. `power` 按 sensor 配置的顺序执行，并记录/恢复共享资源的原状态；失败半途也必须可恢复。
2. `read8/write8` 是 16-bit register / 8-bit data I2C；只在上电及总线归 AP 后调用。
3. `capture(on)` 在 stream-on 前完成 SENINF、VC0/DT0x2b、RAW10 DMA、SMMU 配置；`capture(off)` 在返回前停止并排空，即使初始化只做了一半。
4. 注册会分别上电读 ID 并恢复，只有两枚真实 ID 相符才发布 READY。调用发生在线程上下文；所有回调都不可重入相机 API。
5. DMA worker 完成缓存同步后调用 `x9_camera_deliver(sensor,generation,raw,stride,bytes,time)`；不得在硬中断调用。generation 必须来自本次 `capture(on)`，time 为帧完成的 `mach_absolute_time`。

下一阶段不能绕过 regulator/SMMU 所有权或从 LK framebuffer carve-out 偷分配相机 DMA。
先证明一枚芯片的上电→I2C ID→还原，再证明 SENINF 包→RAW DMA 完整帧，最后验收前后切换、预览和落盘照片。
