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
audioctl dsp-status
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
这仍不是 OPEN→HWPARAM→PREPARE→START 播放状态机；DSP shared-DMA 描述符、
AFE 和功放需要下一阶段实现。外层 mailbox 传输现在由
`x9_adsp_transport.{c,h}` 实现，**没有板级注册者，因此默认不执行 MMIO**。

### 接入有界 ADSP mailbox

板级驱动确认固件、时钟／睡眠保护／信号量和独占 mailbox 所有权之后，调用
`x9_adsp_register_transport()` 提供真实 power/reset/start/wake 回调及 Device-memory
映射。不能把普通 cacheable 指针或另一消费者仍使用的 pin 交给 transport。
板级回调必须有界；失败的 `power_on`／`wake_lock` 须完整回滚。

| 内核调用 | 行为与失败边界 |
|---|---|
| `x9_adsp_boot(core)` | 真实 halt/reset→清旧 RX→检查 TX 已空→start→等待新 READY，最多 2 秒 |
| `x9_adsp_request(core, request, reply, timeout_ms)` | 单请求，先排除旧 ACK；TX 消费最多 1 ms，NEED_ACK 最多 250 ms |
| `x9_adsp_poll(core)` | 单包接收；无消息返回 EAGAIN，不更新播放 cursor |
| `x9_adsp_next_event(core, event)` | 取 logger/timesync/audio 异步包；最多 16 包，溢出 fail-closed |
| `x9_adsp_shutdown(core)` | 先 halt/reset 再撤 power；reset 失败不释放仍可能使用的资源 |
| `audioctl dsp-status` | iOS 只读查看两个 core 的状态、generation、TX/RX/ACK、超时和错误计数 |

外层头是四个 LE u32（16 B），发送 IPI ID=9、options=`0xffff0000`、
reserved=`0xdeadbeef`；其后才是 inner audio_ipi，不复制 host data 指针。
mbox0/2 是 AP→DSP，mbox1/3 是 DSP→AP；**每 slot 是 4 B，`is64d` 不改变它**。
RX 主 pin 60 slots=240 B，末尾 VP pin 的 16 B 不碰。先验证 RX 长度，复制到
私有包后才 CLR；不把整个 direct-copy 缓冲区长度当成 inner payload 长度。
发送前持有 wake lease，直到消费和 ACK 结束。RX options/reserved 不强制回显，
因为源码接收方没有这种要求，也没有真实固件抓包支持它。

READY 必须来自 reset 后的 IPI13、4 B payload、正确 core ID。运行中的新 READY、
WDT、错配／迟到 ACK、畸形包或事件队列溢出均隔离会话；**超时后不重发相同 ID**，
只有另一次真实 reset/start/fresh READY 可恢复。ACK param1/param2 原样交给调用者，
不能把未知 DSP 回复格式当成 errno 或直接宣布 HWPARAM 成功。

DSP `online` 与扬声器 `Ready` 完全分离。隔离只阻止后续 mailbox send，不代替
PA mute／AFE DMA stop；最终 backend 必须在任何 transport 错误后执行物理停流。
目前没有启动命令暴露给用户态，也没有 ADSP firmware loader／板级回调提供者。

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
- 本机捕获 DT 的 `audio_extend` 指定 `speaker-vendor="nxp"`、
  `tfa98xx.6-0034/.6-0035` 和对应两个 DAI，因此当前板级配置指向双 NXP PA。
  仍需 live driver binding、revision 和实际 container/profile，不能由配置证明物理出声。
  DT 同时包含 RT5512 节点／两套 KO 加载，不再把它们当作与 NXP 等权的路线证据。
- `speaker-i2s-id=9/8` 是 `audio_extend_drv.c::extend_i2s_prop` 中
  I2SIN4/I2SOUT4 的名字索引；`spk-i2s-index=0x19/0x18` 是
  `mtk-sp-spk-amp.h::mtk_spk_i2s_type` 的 OUT4/IN4（25/24），**不是 I2SOUT5**。
  仍须取得 mixer 当前路由；不能将这两套编号直接当 AFE DAI ID。
  AFE 为 `0x1a110000`；实际 ADSP `shared_memory` 为 `0x160000000/0xda0000`，
  `adsp-rsv-audio=0x852000`，不能直接使用通用 DTS 的 5.75 MiB 分配。
  原始 DT、日志、厂商固件和校准数据不入 git。

## 取得 Android 板级证据

Android 连接后执行一次只读采集（目录必须新建且在 `.local` 内）：

```sh
python3 -I x9/audio/collect-speaker.py \
  --out .local/x9/audio-capture/android-speaker-01 --card 0
```

脚本先检查设备和 root 读取权限，失败不重试。只读 ALSA/cards/PCM、四个候选 PA
的 sysfs binding、NXP 模块参数、version/dsp-state/fw-state、当前 `tinymix`、
音频相关 dmesg，以及 vendor/odm 中两个默认 `.cnt` 候选。不播放、设 mixer、
校准、访问 debug RPC、改寄存器选择器、mount、flash 或写设备文件。
可由用户先在 Android 手动播放媒体，再采集活跃路由。结果／固件／设备身份只进
私有目录，不入 git；脚本当前未在线执行，文件缺失和硬件 revision 仍可能无法取得。
`capture.json` 的 complete 仅表示必需采集命令完成；候选文件是否存在、是否被加载
及首尾 boot ID 是否一致必须按各原始结果核对，不是 backend 验收标志。

当前明确未完成：板级 DSP power/reset/firmware/SMC 注册、真实 IPI 硬件验收、
AFE/PA 后端与型号／profile 验证、设备出声、
HAL 安装／原生 iOS 选择、CoreAudio host-clock 调度差异，以及多 context 混音。
