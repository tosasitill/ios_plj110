# X9 扬声器 / ADSP 音频接口

**当前是可编译的输出链路基础实现，不是已经出声的驱动。**
没有 MT6993 物理 backend 注册者；`Ready=0`，`Start` 返回
`kIOReturnNotReady`。不访问 off-domain MMIO、不自动起 DSP、不写功放／root，
没有自动安装 HAL。麦克风是另一套接口，不在此输出设备中虚报输入流。
PCM ABI 仍为 v1；**board-status 已升级为 v2**，需配套本轮 `audioctl`，旧工具会拒绝
新版结构。新增 AFE/stream 状态 selector 只返回缓存，不申请硬件资源。

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
启动参数 `x9_audio=1` 仅发布音频服务；`=2` 还注册板级 ADSP core0 transport，
但不在开机时上电／SMC。默认关闭。
打包 boot 时必须沿用最新完整参数，不能拿缺 Wi-Fi／蜂窝校准的默认配置替换。

## 部署后使用

以下是**设备 iOS 内执行的命令**，不是 macOS 播放程序：

```sh
audioctl status
audioctl stream-status
audioctl afe-status
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
| open type 2 | mode2 的 root 控制会话；不能映射 PCM 或 start 播放，可完成失败后保留的 stop |
| selector 0/1/2 | status / start / stop；拒绝 async、descriptor 和额外参数 |
| selector 7/8 | cached AFE status v2 / stream lease status v1；只读，不做 MMIO |
| memory 0 | 只读 kernel control，带 seqlock、generation、真实 played/queued |
| memory 1 | 可写 producer end-frame，release 发布，kernel 检查窗口 |
| memory 2 | 65536-frame S16LE stereo PCM 环，256 KiB |
| HAL 外观 | 48 kHz interleaved Float32 输出、软件音量、无麦克风 |

HAL 不改已经提交的 PCM；间隙清零、越界／重叠 IO context 拒绝，避免与
DMA 并发修改。多 IO context 的混音尚未实现。时基取实际 XNU timebase，
时间戳 sample cursor 来自 backend，而不是把墙钟前进冒充硬件播放。
关闭／进程退出触发 stop；新 writer 获得新可写页，旧 mapping 不能污染下一会话。
物理回调错误保留 fault，不能在不重新确认硬件状态时返回 ready。
物理 lease 与 `Running` 独立：start 失败也必须执行 stop；stop 失败继续保留 lease、
首错和独立 stop errno，不会因为 `Running=0` 把后续 stop 变为假成功。
旧 writer 退出后，普通新 writer 不能接管该 lease；root 可在 mode2 下显式执行一次
`audioctl stream-stop` 完成已有资源的 mute/quiesce/restore，不 reset DSP、不播放或解静音。
Stop 返回0只说明这次资源清理成功，历史 Fault 不自动清除，不能据此重新宣布 Ready。
HAL 的 StopIO 保留失败状态，后续 StopIO 仍执行必要清理；活动流已经 fault 时，新的
StartIO client 不会仅因 IO user 计数非零而获得虚假的启动成功。

## MTK DSP ABI 与物理后端

`x9_audio_dsp.{c,h}` 已提供独立的 MTK `audio_ipi` 字节编码、解码和 ACK
匹配：magic `0x8888`、16 B header、最多 96 B payload、明确 source/target
4-bit 布局，支持 MSG_ONLY/PAYLOAD；DMA 描述符尚未支持，不接受用户物理地址。
OPEN/CLOSE/PREPARE/HWPARAM/START/STOP 等 ID 来自 OPPO 协议定义。
这仍不是 OPEN→HWPARAM→PREPARE→START 播放状态机；DSP shared-DMA 描述符、
AFE 和功放需要下一阶段实现。外层 mailbox 传输现在由
`x9_adsp_transport.{c,h}` 实现，**mode2 已有 ADSP 板级注册者，但默认不执行 MMIO**。

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
现有板级提供者 `x9_adsp_mt6993.c` 接通以下显式启动入口；没有 host firmware uploader。

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

## 显式 ADSP 板级操作（不等于播放）

仅当候选以 `x9_audio=2` 启动、工具已经按 extent 流程部署后，才可在 **iOS root shell** 使用：

```sh
audioctl board-status
audioctl dsp-boot
audioctl dsp-status
audioctl dsp-stop
```

`board-status` 是缓存快照，无 MMIO 副作用。boot/stop 使用独占管理员 type2
连接和 selector5/6；有已注册 speaker backend 或活动流时拒绝 reset。
`dsp-boot` 单次执行，不轮询设备连接、不失败重试、不自动 unmute。
关闭连接不会隐式重启／停止 DSP，需显式 `dsp-stop`。

板级流程：确认 TOP/INFRA **全局 off** → AO/INFRA/TOP AP vote → CORE_STOP →
pending-count=0 → SYS_CLEAR → secure RELOAD → CORE_START0 → transport 新 READY。
继承 TOP/INFRA vote 返回 EBUSY，不借 QUERY_STATE 的布尔电源查询来猜 core 是否空闲。
不启动 core1，mode2 禁止 core1 transport 注册，避免全局 SYS_CLEAR 破坏另一个 owner。

AP HWCCF 在 `0x1c040000`，SET/CLR/EN 为 `0x20700/704/708`，DONE 为
`0x1132c`，global EN/transition 为 `0x13700/704`。每次只写单域 bit，
有界 EN/DONE/global 检查；错误保留 ownership、尝试恢复可访问电源链。
transport v2 的 `power_held` 确保回滚失败的资源仍可显式停机／恢复。
**sw_ver 未确证，自己取得的 AO vote 保留到本次 boot 结束**，不猜 A0 erratum
已不适用；继承 AO 不撤销。SMC 为 `0xc200052b`，magic 为 `0xad5bad5b`，
a0..a3 原值和首错／撤票错／恢复电源链错分别保存在 cached board status v2；只有零返回被接受。
`registers-valid=0` 时域内 pending/DPSW/ACK 数据标记为 `UINT32_MAX`，不是旧的正常读数；
wake 必须有本次有效域内快照，不能以未刷新字段授权访问。
SMC 本身依赖 secure monitor 返回，AP 不能为一个不返回的 monitor 实现抢占超时。

v3 没有旧 prelock/IPIC 回调。当前不发送 runtime suspend IPI，域持续 pinned；
wake lease 验证全电源链稳定且 DPSW req/ack bit0 均为0，不把 WFI 等同于 suspend，
不将 firmware RSV 状态猜成 Linux enum。新 suspend/DPSW 状态会拒绝邮箱访问，
而不是空回调虚报 wake。固件是否存在／能重载仍须 SMC 结果与真实 fresh READY 证明。

## AFE／功放配置组件与当前缺口

| 文件／入口 | 已实现 | 仍需实际板级提供者 |
|---|---|---|
| `x9_speaker_afe.{c,h}` | DL24/OUT4 逐字段写入/不确定/恢复 journal、32-bit I2S slots/S16、48k、CONN116/117 DL24 route；prefill 后 DMA start、period copy/cache、稳定 CUR、高低地址/半环歧义检查；独立低水位 watcher、mute/disable/quiesce/restore | AUDIO/VLP APLL2、pad/共享资源 lease、DMA bus/EMI/cache、双 PA mute、权威 DMA quiesce |
| `x9_tfa98xx.{c,h}` | PM container total-size/CRC、双 I2C 类型（container bus=0）的地址、明确名字的两侧 profile、descriptor 边界；真实 reg03 revision 比对、AMPE=0 且读回；双侧预检白名单 register/bitfield prefix | 实际完整 revision、immutable container、per-revision 白名单、I2C6/reset/rails 互斥；Probus DSP file/patch/校准/boost 未执行 |

AFE start 必须预填至少960帧后才使能 DMA；PA 始终由外部 board 维持 muted。
组件**没有调用 `x9_audio_register_backend`**：最终 service/HAL 还须接预填 timeline、
underflow 前静音和真实 host anchor，不可把启动前空环的 cursor 冒充已排队播放。
ON 清零不是全部 outstanding AXI DMA 已结束的证据，必须有真实 `quiesce` 后
才恢复地址／释放 lease；任何失败都保留资源。没有共享 lease 提供者时不会触碰 AFE，
也不与麦克风 VUL1/ADDA 争寄存器。captured DT 的 OUT4 是2ch/sync0/mclk0、
`etdm-vlp[4]=1`，不能把普通非VLP APLL2 回调充当已接通的板级资源。

AFE register 时才建立 boot-lifetime watcher；空闲时睡眠，运行时每2ms检查真实 CUR。
剩余已提交PCM低于或等于480帧（10ms），或 AP 观察/硬件进度超过剩余安全预算，
即停流，保留主错与 mute/disable/quiesce/restore 各阶段错误。**这是软件保护，不是
硬件不重放保证**：AP 调度超时可能晚于deadline，仍须真实硬件资源与物理静音验收。
watcher不会 unmute PA，也不会注册 speaker backend。
未注册、重复 prepare 等 admission 错误不再污染现有状态；实际部分写入只恢复 touched
字段，已恢复字段不重写。跨环 copy/cache sync 失败保留 `copy_pending`，不能报告
EAGAIN“没有消费”；失败 stop 的 lease/poison 保留，只有完整 quiesce/restore/release
后才能进行新的 prepare。

**container 的 `bus` 不是 Linux I2C adapter号**：厂商 `tfaRunStartup` 对
`dev->bus != 0` 跳过I2C初始化，所以 parser要求bus=0；I2C6由board lease选择，
不能把DT的adapter6硬塞到container字段。

功放 reset 的 DT phandle `0x25` 已解析为 **MT6661-3 PMIC pinctrl** 的 GPIO10/11，
不是 AP GPIO10/11。不能直接写 AP GPIO来“解除 reset”。进一步确认的源码/DT资源为：
MT6661-3 SID3 reset，MT6661-5 SID5 **LNC0_8** 为左 PA 的 `tfa98xx_vdd-supply`，
二者均由 `spmi-dev-mask=0x55a8` 路由到 **PMIF-P 0x1c018000**。厂商设置VDD为1.8V，
reset动作是高5ms→低5ms，不能套用麦克风 GPIO9 的低→高使能流程。
AP I2C6 的 imps gate 是 `0x129d0e00` bit2（CLR +4 / SET +8），SCL/SDA为AP GPIO114/115；
这些是源码资源定位，不是已获取共享所有权、实测电压、复位极性或芯片 revision。
本轮没有在线采集、PA写操作或完整板级 I2C/rail/reset provider。

TFA 只有 reg03 **成功读取且完整revision匹配**才设置 `revision_verified`；
失败回调即便填写了相同值，也不能授权cleanup写该芯片。prefix记录write attempts与
applied次数，失败写入/读回置poison，后续双侧mute成功不抹掉部分配置的不确定性。

`audioctl` 还可仅离线检查本地 container（不需要 X9Audio 服务、不加载到功放）：

```sh
audioctl amp-inspect /var/tmp/tfa98xx.cnt '<exact-profile-name>'
```

parser 报告两个 PA 的 profile/prefix/opaque 数目。CRC通过和 prefix写完均不表示
完整初始化／校准成功。不会执行未知 opaque payload、猜默认profile、写MTP/OTP，
没有 unmute API；真实 profile／芯片证据未齐时，不允许进入 speaker Ready。

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

当前明确未完成：真实 DSP power/secure RELOAD/READY 硬件验收、
AFE/PA 后端与型号／profile 验证、设备出声、
HAL 安装／原生 iOS 选择、CoreAudio host-clock 调度差异，以及多 context 混音。
