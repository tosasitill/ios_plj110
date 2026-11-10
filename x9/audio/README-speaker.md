# X9 扬声器 / ADSP 音频接口

**当前是可编译的输出链路基础实现，不是已经出声的驱动。**
没有 MT6993 物理 backend 注册者；`Ready=0`，`Start` 返回
`kIOReturnNotReady`。不访问 off-domain MMIO、不自动起 DSP、不写功放／root，
没有自动安装 HAL。麦克风是另一套接口，不在此输出设备中虚报输入流。
PCM ABI 仍为 v1；**board-status 和 stream-status 为 v2**，需配套本轮 `audioctl`，旧工具会拒绝
新版结构。私有 kernel backend 回调也升级为 v2（start 增加预填输入）；不能继续注册
旧版 start 回调。私有 AFE platform 为 v2（新增 cached `held` 回调，保留部分申请失败的资源）。
AFE/stream/resource 状态 selector 只返回缓存，不申请硬件资源。
新增 `clock-status` 同样只读缓存；`clock-probe` 是 root／mode2 的一次
**有电源、时钟和禁用 OUT4 寄存器写入**的显式事务，不是只读 probe，不能自动执行。
新增 `pll-status` 只读缓存，`pll-probe` 在同一基础事务内额外启停预配置 APLL2、
切换 VLP mux 并写硬件测频器。新增 `path-status` 只读缓存，`path-probe` 进一步配置
禁用的 DL24→OUT4 路由；**clock-probe／pll-probe／path-probe 共用每 boot 一次机会**，
不能连续运行这三个事务。

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
audioctl resource-status
audioctl stream-status
audioctl afe-status
audioctl dsp-status
audioctl play /var/tmp/stereo-48000-s16le.pcm
```

工具不会自动生成或播放测试音。`play` 映射 PCM 环，按实际硬件 cursor
等待完成；文件尾提交两个静音 period，等待最后一个真实内容帧播放后 stop，
不会等 DMA 队列耗尽才停。最后不足一个 period 的内容按 stereo frame 计数，
残缺 stereo frame 拒绝。5 秒无进度会停流；内核独立的 10 ms 游标／低水位
保护可能更早停流。未实现的物理后端会立即拒绝播放。
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
| selector 7/8 | cached AFE status v2 / stream lease + physical cursor status v2；只读，不做 MMIO |
| selector 9/10 | cached PA status v1 / root type2 显式双 PA reg03 probe；不写 PA 值 |
| selector 11 | cached AP resource lease status v1／64B；没有 claim/release 用户接口 |
| selector 12/13 | cached clock status v1／216B / root type2 显式 cold register-clock＋disabled OUT4 配置／恢复 |
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

### 避免不同音频驱动撤销彼此的电源链

OPPO `mt6993-afe-clk.c` 的 AFE clock 路径依赖 ADSP INFRA／AUDIO，当前板级
麦克风会借用已开启的 AO／INFRA AP vote，而 ADSP stop 会撤自己的 INFRA vote。
以前两者只有独立的 `afe_mic_lock`／`board_lock`，无法互相串行化这个借票→撤票窗口。
`x9_audio_resources.{c,h}` 现在提供同一把**短叶锁＋跨回调 ticket lease**，接在
ADSP power_on、麦克风 cold probe／VUL1 start 和 speaker AFE acquire 之前。

| 结果 | 行为 |
|---|---|
| 已有 owner | 新 claim 立即 EBUSY，不等待、不递归、不进入新的 MMIO／SMC 事务 |
| claim 成功 | ticket 单调且不复用；保持到物理 stop／restore／release 完成 |
| 物理回滚失败 | 保留 owner/ticket/首个 held_error，别的 AP 音频 owner 不能拆该电源链 |
| 旧 ticket／其他 owner 释放 | EPERM，只计拒绝，不清当前所有权 |
| `resource-status` | 返回 owner、ticket、held errno、历史 errno、claim/release/refusal 计数；不读寄存器 |

叶锁中没有硬件操作、等待或外部 callback；顺序为 consumer lock→resource leaf lock，
状态查询不反向拿 consumer lock。麦克风资源冲突发生在一次诊断的 attempt 锁存之前；
真正开始后的失败／回滚仍沿用每 boot 一次及 retained journal 规则。
ADSP 正常 stop 后有意保留的 AO erratum vote 不当作活动播放 lease，下一次硬件
admission 仍须重新核对。当前独立 DSP／mic／speaker owner 保守互斥；未来同一
DSP＋AFE 播放后端必须统一一次资源 lease，不能递归申请两个独立 owner。

AFE platform v2 的 `held(context)` 必须是缓存所有权谓词，不做 MMIO；部分 acquire
失败仍 held 时执行 mute/release，只有物理恢复成功才撤 ticket。没有物理 lease 且
无 AFE journal 时不调用 off-domain mute/restore；journal 尚未恢复却丢 lease、
或 release 返回0但仍 held，均隔离并保留 ticket。所有 callback 仍须有界并处理
部分资源，`held` 本身不替代可访问性／quiesce 证明。

这仅互斥已接入的本地 AP 音频 owner，**不是 clock provider，也不是物理资源证明**。
foreign XPU、HWV/PMACK、APLL2、pad、PA、EMI/cache 和 DMA quiesce 检查没有移除。
`owner=none` 不代表所有电源 vote 已撤或功放已静音；禁止据此设置 speaker Ready。

### 显式接通寄存器电源／时钟与禁用的 OUT4 配置

`x9_speaker_mt6993.c` 现在提供独立的板级窄事务，不注册 speaker AFE 或 playback
backend。用户完成配套 kernel/tool 部署并以 `x9_audio=2` 启动后，在 **iOS root shell**
可显式执行一次：

```sh
audioctl clock-status
audioctl clock-probe
audioctl clock-status
audioctl resource-status
```

不要把它放进启动脚本。它与 DSP boot、mic、speaker prepare 共用 AP ticket；
已有 owner 时 EBUSY，且不消耗本 boot 的 attempt。进入事务后每 boot 仅一次，
失败不 retry/reset，也不通过关闭控制连接撤销保留的硬件资源。

| 阶段 | 实际操作与边界 |
|---|---|
| cold admission | AO 快照确认 AUDIO／ADSP TOP 全局 off、无 foreign XPU1..9 AUDIO/TOP/INFRA vote；两个 VLP mux 必须 PDN 且 FENC off |
| AUDIO resources | AUDIO SMC INIT0 → AO／INFRA／AUDIO AP votes，只添加缺少的票；borrowed INFRA／AO 票不撤 |
| register clocks | audiointbus／audio_h 的 2-bit parent 在 PDN 下切到 index0 的 26 MHz，再启 direct FENC，检查 UPDATE CHK／FENC／readback |
| idle admission | 第一次 AFE 读前确认 domain／mux 可访问；拒绝 engine/monitor、32 memif、7 IN＋7 OUT enable、SRC/test/sine/pad TX 活动；不清 IRQ、reset 或取消 DMA |
| configure | 仅开 HOPPING/F26M 的 bank1 bits0/1 → SMC SIDEBANDS7 → 四个禁用 OUT4 字段；EN 始终0，不写 pads、routes、DL24 地址、DMA、PA 或 APLL |
| restore | OUT4→base gate 逐字段 masked restore/readback → audio_h／intbus PDN与原 parent → AUDIO AP unvote；确认 PMACK/global/STA/busy off 后才撤自己的 INFRA票 |

OUT4 复用 `x9_speaker_out4.h` 中与实际 engine 相同的 S16／32-bit slot、FS5／relatch10
profile，避免诊断配置和播放配置漂移。**只是禁用状态下的寄存器格式验证，不是 48 kHz
时钟或 I²S 波形证明。**这里只接通 26 MHz 寄存器时钟，没有开启 AUDIO engine／SRAM，
不做 SRAM request，也不建立 safe PA mute。将来 full provider 仍须补足这些条件。

写入前记录 dirty／uncertain；恢复只接受本字段原值或自己的预期值，第三种值立即拒绝。
final readback 也核对未写字段，不能把外部变化收编成新 baseline。SMC 返回未知失败、
HWV／FENC timeout、domain accessibility 丢失或 inherited engine 活动均保留 journal／ticket，
不掉电冒充恢复。`error` 与 `restore_error` 分开，失败阶段保留。`Restored` 仅表示本轮
可恢复字段与活动票的 cleanup 完成，不表示安全调用从未产生持久效果：成功 SMC INIT／
SIDEBANDS 没有伪造 undo，若本轮添加 AO vote，按既有 ADSP 的未知 sw_ver erratum 策略
保留为 **parked_vote=8**，不当作活动播放 lease；borrowed 票也保持不变。

`clock-status` 保留 before/configured/after、raw SMC result、dirty/uncertain、held ticket。
APLL2 CON0/CON1/CON3/FENC/TUNER/PCW0/PCW1 仅在 cold admission 后读取 AO 快照，
**clock-probe 路径不写 PLL**。OPPO `clk-pll.c` 的 `mtk_pll_fenc_prepare()` 仅维护 software count，
`is_prepared()` 只读 FENC；FENC不是已证实的 lock/rate/所有权原语。不能依据参数
`0x78fd5264/65`、FENC或本事务返回0设置 Ready。仍须接 VLP APLL2／全资源 ownership、
可信双PA mute/config、pads与DMA/cache/quiesce，实体出声与 iOS HAL 路由尚未验收。

### 申请预配置 APLL2 并取得硬件测频样本

`pll-probe` 不是状态查询，不自动运行，不是播放 backend。它替代该 boot 的
`clock-probe`（或选择下节 `path-probe`），共用同一 AP ticket、cold AUDIO／26M／SMC INIT0＋SIDEBANDS7、
禁用 OUT4 与恢复流程。必须由用户在配套 kernel／工具已部署且 `x9_audio=2` 的
**iOS root shell** 中显式选择；本轮只有编译结果，以下硬件流程尚未上机执行：

```sh
audioctl pll-status
audioctl clock-status
audioctl resource-status
# 若本 boot 尚未进入 clock-probe/pll-probe，且用户决定执行有写入的事务：
audioctl pll-probe
# 之后只读缓存；失败也不重试、不 reset、不取消未知硬件操作。
audioctl pll-status
audioctl clock-status
audioctl resource-status
```

| 阶段 | 准入／操作 |
|---|---|
| APLL2 cold admission | `0x1c203100` 的 EN／SDM／TUNER/FENC off，test-clock bits15/17 off；PCW0/1 已为 `0x78fd5264/65`，CON1 bit31已1、postdiv0；不改PCW，不重放PCW_UPDATE |
| non-owned consumers | TOP AUD2、MM DVO_FAVT_DP 与 VLP SW_ENGEN2 必须 gated、FENC off、UPDATE CHK无pending；TOP AUD2 parent0、MM DVO parent0也保守要求。不写TOP／MM消费者，检查只是当前快照，不是远端所有权证书 |
| FQMTR admission | VLP selector0、独立 `0x1c201000` CON3 HW mode0、CON6 BUSY0、CON0 enable/cal-mode/trigger bits全0；不关闭已有自动测频 |
| reference samples | 保持 intbus／audio_h 为26M，先分别测 VLP id33与34；拒绝旧counter／超时结果 |
| PLL／mux cycle | 只开 APLL2 EN→SDM→TUNER bit0并检查FENC；ENGEN2在PDN下切parent3=APLL2/4，audio_h在PDN下切parent3=APLL2；intbus继续26M，AFE engine／OUT4 EN仍0 |
| active samples | 三对 id30=49.152MHz／id34=196.608MHz；每次重新申请测频，非自有消费者、可访问性与AFE idle重查 |
| restore | 每样本先清自己的已完成trigger、逆序恢复CON2/load/CON0/selector；总事务audio_h回26M、ENGEN2 gate与原parent、TUNER→SDM→EN逆序恢复，任何PLL位曾写入也须等FENC off，再走AUDIO-8基础恢复与AP ticket释放 |

FQMTR采用 OPPO `clk-fmeter-mt6993.c::fqmtr_cal` 的 normal mode、load1023、
divider1（divide2）、reference/source0与CON0 hi16 timeout0x410。必须观察该次
**BUSY上升（≤20µs）→下降（≤320µs）**，不以旧count或厂商超时后算出的数值为成功。
每次控制字段检查／恢复均只接受原值或自有预期值；CON0 bits15/12按普通RW严格readback，
若实机显示self-clear等不同语义则保守失败，不假设已触发。未观察到完整BUSY周期的
trigger不取消、不清除，保留所有依赖／journal／ticket。已观察完整周期后count0／
不稳定也可仅清自己的trigger并恢复控制；不会写回保存的历史低16-bit count。
未被接纳的baseline没有cleanup写入；late mux/FENC／未知SMC／第三值同样阻止掉电。

频率计算为 `count * 26000000 * 2 / 1024`。**26MHz参考没有实测或校准**：
±1%只做nominal路径sanity，每路三样本spread≤2只做短窗口一致性；单count约50.781kHz，
不是audio ppm精度、PLL锁定、I²S输出／pad波形或声学时钟证明。
本地board mutex串行该VLP测频用户，并不授权覆盖其他AP核／远端固件的测频操作。

| selector／缓存 | 内容与边界 |
|---|---|
| 14 `pll-status` v1／264B | flags、主/restore errno、stage、last register/value、dirty/uncertain、mux-owned、ticket；PLL before/measured/after、消费者／测频器原始快照、8个count与nominal Hz，只读软件缓存 |
| 15 `pll-probe` | root＋exclusive＋admin＋open type2，audioLock下拒绝已注册backend、backendHeld或Running；进入硬件后每boot一次，与selector13共享，不自动重试 |
| flags | Attempted1、Baseline2、Captured4、Powered8、Measured16、Restored32、Held64、Poisoned128、MeterCaptured256、Reference512；Powered只表示启用位/FENC通过，Measured只表示8样本sanity通过 |
| stage | 0none、1cold、2meter-admission、3reference、4PLL-power、5mux、6measure、7restore、8done；失败后保留主错stage/register，不让cleanup覆盖。共用基础事务的错误还须查看clock-status |

`Restored`仍只表示可恢复配置与活动AP票cleanup，不撤borrowed票、不撤按ADSP
erratum保留的parked AO vote、不伪造SMC undo。**此事务成功也不设置 verified／Ready，
不注册AFE/platform/backend、不启AUDIO engine／DMA、不写SRAM／pads／routes／PA。**
双PA右rail、实际revision/profile/校准、可信mute/config与完整AFE缓存／EMI／quiesce
以及iOS原生HAL路由仍未接齐；DSP固件RELOAD/READY也没有新增实机验收。

### 配置禁用的 DL24→OUT4 格式和路由

`path-probe` 复用完整 `pll-probe` 的cold AUDIO／SMC／APLL2／测频事务，在其
APLL2仍on且OUT4 EN0时接入四字段配置，随后先恢复路由，再恢复PLL与基础电源。
没有物理AFE/platform/backend注册，不启动播放。**以下设备步骤尚未执行**：

```sh
# 配套kernel/tool部署及x9_audio=2启动后，在iOS shell读取软件缓存：
audioctl path-status
audioctl pll-status
audioctl clock-status
audioctl resource-status
# 仅用户选择有写入的事务、且本boot三个probe均未进入时，在root shell一次执行：
audioctl path-probe
# 失败也只读缓存，不重试、不取消未知request：
audioctl path-status
audioctl pll-status
audioctl clock-status
audioctl resource-status
```

| 字段／阶段 | 行为与边界 |
|---|---|
| board admission | 基础clock／PLL准入保持；AFE engine、32 memif、7IN＋7OUT EN全部off；DL24 ON/one-heart/domain与两个SW_CLEAR位必须0；CONN116/117各7个合法route词全0 |
| shared descriptors | `x9_speaker_dl24.h`同时供板级事务与AFE引擎使用：inactive cowork、DL24 S16 stereo／FS10、PBUF/MIN/MAX编码0、CONN116_2 bit16与117_2 bit17；不是Android当前mixer的运行证明 |
| configure | 只修改0x1690 cowork bits8..11、0x46d0普通disabled格式、0x2f08／0x2f28；OUT4／DL24 EN始终0，不写保留route word3 |
| inherited fields | DL24 BASE_MSB/BASE/END_MSB/END、MEM_UP_MSB/MEM_UP与PAD_TOP只读快照，每次字段变更和恢复前核对未变；其它route词始终空，不清foreign source |
| restore | 两route→DL24格式→cowork逆序masked恢复；核对四字段（包括不需要写的字段）与全部非自有route/address/control baseline，再恢复PLL、OUT4与基础票；失败保留同一AP ticket |

PLJ110捕获DT的`etdm-out-ch[4]=2`、`etdm-out-sync[4]=0`、`etdm-vlp[4]=1`，
且所有`etdm-in-mclk/etdm-out-mclk`为0；**本事务不申请MCK，不重编TOP AUD2 mux/divider**。
同样不写DMA地址／CUR／MEM_UP、SRAM／DRAM request、pad／GPIO、PA或engine enable。
继承字段未变和memif EN0不是权威DMA drain／无outstanding请求证明，不能据此注册provider。

OPPO`mt6993-reg.h`明确DL24_CON0 bit18/7为SW_CLEAR_MEM_RAN_OUT／SW_CLEAR_BUF_EMPTY
动作位；板级及AFE引擎均拒绝继承的高位，RMW写入显式带0，绝不重放清除动作。
AFE引擎也新增prepare final readback与restore final baseline，包含未写字段；
non-owned route词／one-heart／domain／action变化会阻止物理release，不因dirty mask清零
就当作恢复。私有platform v2／PCM ABI1及原verified注册门不变，当前没有真实调用者。

| 接口 | 内容 |
|---|---|
| selector16 `path-status` v1／272B | 缓存flags／主与restore errno／stage／last register,value／dirty,uncertain／ticket，四字段before/configured/after、14route词before/after、未写地址与control before/after |
| selector17 `path-probe` | root＋exclusive＋admin＋type2＋audio2，audioLock下拒绝haveBackend/backendHeld/Running，AP资源冲突不消耗attempt；真正进入与selector13/15共用每boot一次 |
| flags／stage | Attempted1、Captured2（完整准入后的journal）、Configured4、Restored8、Held16、Poisoned32；stage0none/1admission/2configure/3verify/4restore/5done。早期失败须联看PLL与clock缓存 |

`Configured`仅指四个禁用字段readback通过，`Restored`仅指共用事务可恢复配置／活动票
cleanup（borrowed票与parked AO保持、无SMC undo）；**不等于真实48k I²S、DMA进度、
PA静音或实体出声，Ready仍0**。仍须取得真实共享资源／EMI/cache/quiesce、双PA
右rail/revision/profile/校准与可信mute/config，以及DSP固件READY和iOS HAL路由证据。

### 对齐启动预填和 PCM 时间线

backend v2 注册要求 `lead_frames` 为 480 的整数倍、范围 **960..3840**，八项
physical prerequisites 仍须全部确认。Start 先清零内核私有预填（不是用户可写环），
调用 `start(context, prefill, lead_frames, &physical_anchor)`；backend 必须完整
copy/cache-sync 后再使能 DMA，返回物理预填 frame0 的 host anchor，不保留输入指针。
成功后仍验证 anchor 非零／不在未来、激活未超过安全预算；失败也执行真实 stop，
stop 失败不丢 lease。

| 量 | 定义 |
|---|---|
| physical played / queued | 包含启动静音的真实 backend 时间线，queued 只在完整复制成功后增加 |
| PCM `written` / `out_queued` | 仍从0开始，只计用户 PCM，不把启动静音写入可写 producer |
| PCM `out_frames` | `max(physical_played - lead_frames, 0)`，不按墙钟生成进度 |
| PCM `out_anchor_host` | physical anchor + lead_frames 的 nominal Q32 frame ticks，HAL sample0 对齐用户 PCM |
| stream-status v2 | 缓存预填长度／是否提交、物理 played/queued/anchor、position EAGAIN 累计次数和 stop lease |

position 必须给出包含预填的单调游标，且不越 physical queued；underrun 每 Start
归零，新增 underrun 立即 fault/stop。瞬时 EAGAIN 不推进 PCM、不提交新 period；
读取延迟和**真实游标无进度**分别受 `min(10ms, (known pending - 480)/48k)` 预算约束。
成功返回旧游标不更新进度期限；过期／低水位执行真实 stop，不循环续命。
预算先按帧上限截断再换算，避免累计游标乘法溢出；HAL host-anchor 与提交时间线
也检查溢出。空闲 worker 睡眠，Start 在锁内唤醒。

HAL latency 继续发布 configured lead，safety offset 为 lead + 两个 period；这些是
排队策略元数据，**不是测得的 PA／声学延迟**，也不保证原生 iOS 路由接受未来的
logical sample0 anchor。时间戳来自物理游标和 nominal timebase，未验证硬件时钟
漂移。预填、静音尾和软件调度都不能代替 DMA／PA 物理验收。

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
start v2（完整私有预填、物理预填 sample-0 host anchor）、stop（必须 mute/quiesce）、submit（完整
period 已复制才返回成功）、position（包含预填的真实 DMA/DSP played cursor）。回调串行执行；
submit EAGAIN 不消费 PCM；position EAGAIN 有独立期限。只有八项 physical prerequisites 全部确认才能注册，
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
| `x9_speaker_afe.{c,h}` | DL24/OUT4 逐字段写入/不确定/恢复 journal、32-bit I2S slots/S16、48k、CONN116/117 DL24 route；AP transition ticket、platform v2 部分失败 ownership；prefill 后 DMA start、period copy/cache、稳定 CUR、高低地址/半环歧义检查；独立低水位 watcher、mute/disable/quiesce/restore | AUDIO/VLP APLL2、pad/物理共享资源 lease、DMA bus/EMI/cache、双 PA mute、权威 DMA quiesce |
| `x9_tfa_plj110.{c,h}` | root 显式只读 reg03；PMIF-P SID3 reset/SID5 左供电快照，已开启 I²C6/共享 DMA clock 与 pads 校验、2 CSR bank/1 DMA idle、FIFO WRRD 1+2 BE、逐字段总线恢复 | 不具备双侧供电/reset/write lease；右 rail、真实 binding/profile/校准仍待确认 |
| `x9_tfa98xx.{c,h}` | PM container total-size/CRC、双 I2C 类型（container bus=0）的地址、明确名字的两侧 profile、descriptor 边界；真实 reg03 revision 比对、AMPE=0 且读回；双侧预检白名单 register/bitfield prefix | 实际完整 revision、immutable container、per-revision 白名单、I2C6/reset/rails 互斥；Probus DSP file/patch/校准/boost 未执行 |

AFE start 必须预填至少960帧后才使能 DMA；PA 始终由外部 board 维持 muted。
组件**没有调用 `x9_audio_register_backend`**：service/HAL 已接私有预填／逻辑与物理 timeline
边界，但仍缺真正的 board adapter，将 prepare/start/submit/position/stop 连接至
已确认的资源／PA；underflow 前物理静音和 host anchor 精度仍须验收，
不可把启动前空环的 cursor 冒充已排队播放。
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
没有在线采集、PA值写操作或完整板级 I2C/rail/reset write provider；新增的诊断路径见下节。

TFA 只有 reg03 **成功读取且完整revision匹配**才设置 `revision_verified`；
失败回调即便填写了相同值，也不能授权cleanup写该芯片。prefix记录write attempts与
applied次数，失败写入/读回置poison，后续双侧mute成功不抹掉部分配置的不确定性。

### 读取已供电功放的身份

**仅在配套内核和工具已部署、`x9_audio=2` 时使用**；本轮未安装／上机，
没有自动开机探测。`amp-status` 是缓存读取，未探测时 flags=0，raw 字段0不代表实际硬件值。

```sh
audioctl amp-status
# 在设备root shell中，显式一次；只读PA reg03，不播放、不冷上电、不复位
audioctl amp-probe
audioctl amp-status
```

`amp-probe` 要求 SID3 GPIO10/11 已为数字 SW 控制、mode0/output/DO及DI低；
不把低电平说成冷 PA。左 LNC0_8 已为SW 1.8V/on/non-LP/no-HW-vote；保留完整 DA
原值，不将 `regulator.c` 的 `da_en_mask=3/da_lp_mask=7` 简化成完整 ready。
右节点无VDD supply属性，**右供电仍未知**；即使双 ACK，也不发布两侧 rail ready。
完整 PMIF mutex 跨两次快照、FIFO访问和恢复，但不是远端硬件所有权保证。

I²C6 的 gate 必须已开、GPIO114/115 已为function1且物理高；不改 gate/pad。
共享 APDMA gate 是 **PERAO1 `0x16640014` bit26**；不为探测开DMA钟，off时不读其CSR。
同时核查 WEST I²C 与 PERI_AXI 的 mux/CKSTA/FENC/所选PLL状态；
未证实ULPOSC parent1时明确拒绝，绝不重配共享parent/PLL。
仅在两组 `0x10000` CSR bank 的 START/IRQ/FIFO/shadow/DMA控制及一个 DMA EN
均空闲时，journal原值后写入保守≤98kHz nominal时序，再读0x34/0x35的reg03。
频率是PCW推导，不是频率计实测。

每boot最多一次（包括缺资源/NACK），不重试、不扫描0x5c/0x5d或其他同总线器件。
只有ACK且FIFO恰有2字节才缓存完整BE revision、设置对应 `revision_read`；
未知低字节family拒绝，不写chip。未知/arbitration status或timeout不清IRQ/reset/cancel，
保留dirty journal并poison；可判定完成的自身ACK/NACK仅清该次status。
恢复只处理dirty字段并读回，已经恢复的字段不重复写；第三方变化拒绝覆盖。
主错、主失败stage和restore_error分开缓存，`ProbeComplete`只表示本次读ID并恢复成功。

I²C6还承载display/charger节点；当前XNU无其他I²C6访问路径，root入口由audio mutex串行。
以后增加同总线consumer必须先引入共享controller arbiter，不能靠“刚好idle”并发接管。
本探测**没有注册 `x9_tfa_bus`、AFE provider 或speaker backend**，不授予verified=7、
fresh-reset或trusted prefix能力，不读取／执行container，也不做AMPE mute/unmute。
board type2不会替换PCM页；无backend lease时关闭诊断会话不改PCM fault/timeline。

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
