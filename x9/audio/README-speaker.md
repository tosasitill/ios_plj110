# X9 扬声器 / ADSP 音频接口

**当前是可编译的输出链路基础实现，不是已经出声的驱动。**
没有 MT6993 物理 backend 注册者；`Ready=0`，`Start` 返回
`kIOReturnNotReady`。不访问 off-domain MMIO、不起 DSP、不写功放／root，
没有自动安装 HAL。麦克风是另一套接口，不在此输出设备中虚报输入流。

## 构建

仓库根目录，输出目录必须是新的：

```sh
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I \
  x9/audio/build.py --out x9/out/speaker-local/audio

/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I \
  x9/build/build.py kernel --name speaker-local
```

输出 `audio/X9Audio.driver`、`audio/audioctl`（iOS 16 arm64、adhoc 签名）
和 `kernel.development.sweet`。这不会重建 Metal 插件或修改设备。
启动参数 `x9_audio=1` 仅发布音频服务；默认关闭。
打包 boot 时必须沿用最新完整参数，不能拿缺 Wi-Fi／蜂窝校准的默认配置替换。

## 部署后使用

以下是**设备 iOS 内执行的命令**，不是 macOS 播放程序：

```sh
audioctl status
audioctl play /var/tmp/stereo-48000-s16le.pcm
```

工具不会自动生成或播放测试音。`play` 映射 PCM 环，按实际硬件 cursor
等待完成；5 秒无进度会停流。未实现的物理后端会立即拒绝播放。
HAL 最终位置是 `/Library/Audio/Plug-Ins/HAL/X9Audio.driver`，但首次根盘
staging 必须单独确认、走项目 extent 写入流程；本构建器不安装、不写 root、
不覆盖 Apple `VirtualAudio.plugin`。尚未验证 20H392 原生路由层是否选中本 HAL，
也没有移植 Sheng 的 Thunderbolt 伪装、VirtualAudio hooks 或 shared-cache 时钟写补丁。

## 内核／HAL 契约

`kernel/src/bsd/dev/x9_audio.h` 是双方唯一 ABI 定义：

| 项目 | 行为 |
|---|---|
| 服务 | `X9Audio`，IOResources 上注册 |
| open type 0 | 只读 status；不能 start、stop 或映射 PCM |
| open type 1 | root／指定音频 daemon 的独占 writer |
| selector 0/1/2 | status / start / stop；拒绝 async、descriptor 和额外参数 |
| memory 0 | 只读 kernel control，带 seqlock、generation、真实 played/queued |
| memory 1 | 可写 producer end-frame，release 发布，kernel 检查窗口 |
| memory 2 | 65536-frame S16LE stereo PCM 环，256 KiB |
| HAL 外观 | 48 kHz interleaved Float32 输出、软件音量、无麦克风 |

HAL 不改已经提交的 PCM；间隙清零、越界／重叠 IO context 拒绝，避免与
DMA 并发修改。多 IO context 的混音尚未实现。时基取实际 XNU timebase，
时间戳 sample cursor 来自 backend，而不是把墙钟前进冒充硬件播放。
关闭／进程退出触发 stop；新 writer 获得新可写页，旧 mapping 不能污染下一会话。
物理回调错误保留 fault，不能在不重新确认硬件状态时返回 ready。

## MTK DSP ABI 与物理后端

`x9_audio_dsp.{c,h}` 已提供独立的 MTK `audio_ipi` 字节编码、解码和 ACK
匹配：magic `0x8888`、16 B header、最多 96 B payload、明确 source/target
4-bit 布局，支持 MSG_ONLY/PAYLOAD；DMA 描述符尚未支持，不接受用户物理地址。
OPEN/CLOSE/PREPARE/HWPARAM/START/STOP 等 ID 来自 OPPO 协议定义。
**这不是 mailbox 传输或 OPEN→HWPARAM→PREPARE→START 状态机**；超时后相同
task/message 的迟到 ACK 隔离、ADSP 外层 framing 仍需要物理 transport 实现。

物理驱动通过 `x9_audio_register_backend()` 注册 boot-lifetime callbacks：
start（真实 sample-0 host anchor）、stop（必须 mute/quiesce）、submit（完整
period 已复制才返回成功）、position（真实 DMA/DSP played cursor）。回调串行执行；
EAGAIN 不消费 PCM。只有八项 physical prerequisites 全部确认才能注册，
没有 null backend、模拟 DMA 或由用户设 ready 的接口。

优先完成以下一个窄闭环，再启用 HAL 出声：

1. 确认实际绑定的 PA、I2C ID、Android 路由和使用中的校准／profile。
2. ADSP v3 上电／SMC／READY、pin 与外层 framing，或经过确认的 CPU AFE 路径。
3. 固定实际 carve-out、EMI 权限、缓存策略和 ring 描述符；完成有界 ACK 生命周期。
4. AFE／I2S／PA 安全 mute→配置→低增益 unmute→真实 period→mute。
5. `audioctl` 的 played cursor 与实体出声都确认后，再验收 HAL／iOS 路由。

## 参考与证据边界

- HAL property/interface scaffold：本地 UniXNU-sheng
  `08cde20edc3254ed6f7df8d0f5384fb887786e8f`，保留 `LICENSE.UniXNU`
  （BSD-2-Clause-Patent）。Qualcomm AudioReach／GPR／GLINK 没有移植。
- MTK 协议参考：OPPO `kernel_device_modules-6.12` 的
  `drivers/misc/mediatek/audio_ipi/include/{audio_messenger_ipi,audio_playback_msg_id,audio_task}.h`
  和 `sound/soc/mediatek/audio_dsp/mtk-dsp-platform-driver.c`。
- 本机捕获 DT 同时包含 I2C6 `RT5512@5c/@5d`、`TFA98xx@34/@35`；既有
  Android 日志中两套 KO 都已加载。**节点 enabled／KO 加载不能证明哪套 PA 实际绑定。**
  AFE 为 `0x1a110000`；实际 ADSP `shared_memory` 为 `0x160000000/0xda0000`，
  `adsp-rsv-audio=0x852000`，不能直接使用通用 DTS 的 5.75 MiB 分配。
  原始 DT、日志、厂商固件和校准数据不入 git。

当前明确未完成：真实 DSP boot/IPI、AFE/PA 后端与功放选型、设备出声、
HAL 安装／原生 iOS 选择、CoreAudio host-clock 调度差异，以及多 context 混音。
