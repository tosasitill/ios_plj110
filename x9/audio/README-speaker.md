# X9 扬声器 / ADSP 音频接口

**2026-11-10 启动回归回退：MIC DMA 与 ADSP（含 AUDIO IRQ）的 FDT/DRAM
交接已从 shim 的调用链及编译列表移除，不再使用 `X9_ADSP_LOADER` 开关。**
保留实现源码供后续分析，但不会链接或执行；三个 ADT ABI placeholder 保持生成时
的全零值，内核拒绝授予 microphone DMA / DSP shared memory / IRQ topology。
XNU 入口计算、内核复制、RAM map、GPU/SCP/GPS/Wi-Fi 交接不变；内核及 HAL 音频
代码未删除。恢复这条路线需要独立修复及启动验证，不能用启动参数重新打开。
下文的 live handoff/provider 描述为该回退前的实现边界，不代表当前镜像已授予资源。

<a id="audio-loader-fdt"></a>

## 待处理：音频 loader 的 FDT 扫描优化

**2026-11-11：用户要求先记录，稍后处理。以下仅为设计方案，未实现，
不恢复 MIC／ADSP loader，也不决定重新采用 pre-XNU 音频 metadata 路线。**

### 已知问题与证据边界

- 130 console 停在 GPS FDT 返回后，冻结源码 `0cbd413` 下一步是 ADSP handoff；
  131 复用同一内核，只跳过 ADSP 调用后，`~/189.txt:13` 已进入 Darwin。
  这收窄了故障范围，但没有具体 fault PC。
- 旧 ADSP 成功路径调用60次完整 FDT lookup。130 的 `SCTLR_EL1` 表明 MMU／
  D-cache 关闭；parser 使用字节读取，且连续 handoff 路径没有 watchdog 服务点。
  **长时间扫描耗尽 watchdog 预算是待验证假设，不是已经证实的复位原因。**
  该函数不启动 DSP、不等待 READY，也不访问 shared pool 或音频 MMIO。
- 重复扫描来自接口：`fdt_property_lookup()` 将全树校验与单字段查询绑定；
  每次查属性都重新检查直到合法 END。`named_reg()` 反复获取同一节点的
  `reg`／`reg-names`，schema 与 phandle 唯一性检查也各自扫树。
  必须完整验证一次，不等于每取一个字段都必须重新完整验证。
- `56e7b9a` 的 capture 只优化基础 ADSP 布局；I2S4／PA 等后续查询仍重复扫描。
  不能把它当作整个音频 handoff 已完成 single-pass 优化。

### 推荐设计：固定一至两遍，随后只查本次启动的小表

1. **分离校验／收集与查询。** 先列出 ADSP、I2S4、PA、MIC 的完整节点／属性需求，
   一遍校验结构并收集目标属性、直接子节点数和 schema 信息；required／optional
   分开记录。完整 END／尾部验证成功前，不发布部分结果或硬件访问资格。
2. **集中检查 phandle。** 优先第一遍收集待核对引用，第二遍统一检查全局唯一性，
   避免每个引用单独扫全树。也可用有界全局索引做到一遍，但不为追求一遍增加
   无界内存或复杂度。保留现有重复 alias 拒绝规则；最终 handoff 在全部必需检查后
   才能发布，optional 失败只撤对应能力。
3. **复用一次解码的结果。** 同一节点的 `reg`／`reg-names` 一次解出全部寄存器；
   pinctrl schema 一次校验。ADT 可单独一次定位全部 handoff slot，校验后分别填写。
4. **使用有界、短生命周期上下文。** 当前 capture 上限为16节点／128属性，不能
   直接扩大栈数组。先统计完整需求，再选择 caller-owned 工作区和相对 offset；
   绑定本次调用的同一份只读 FDT，不跨修改或启动复用。只缓存 metadata，
   不替代真实电源、EMI、DMA、IRQ 和 firmware lease 校验。
5. **提供进展服务及总预算。** 仅在扫描确实推进时定期服务 watchdog／USB，
   同时限制总工作量／时间并记录 CNT 阶段耗时。喂狗不是性能优化，不能把死循环
   永久喂住；底层服务不得修改正在解析的 FDT 或重入 parser。
6. **最后再做读取层优化。** 对已证明4字节对齐的 token／cell 使用32-bit标量读取
   并转换字节序，字符串仍按字节处理；不引入未证明对齐的64-bit或向量访问。

不采用“找到属性立即返回”、删除重复／尾部校验、关闭 watchdog 或贸然开启
MMU／D-cache 来掩盖问题。目标是减少整棵树的重复工作，不预先承诺启动加速倍数。

### 稍后恢复工作的顺序

先统计完整查询／工作区需求，确定 metadata 路线，再实现有界收集与本地查询。
按项目规则先直接单位编译；将扫描次数、CNT 耗时与预算拒绝位置纳入最小可观测性。
后续获确认的启动验证保持内核和参数一致、一次改变一个变量，由用户手动启动；
验收必须看到 ADSP 返回／明确不授权退路 → boot_args／jump → Darwin。
没有新的上机证据前，不标记 loader 回归已修或音频可用。

完整证据和编译边界见 [NOTES.md 的 BOOT-AUDIO-AUDIT](../../NOTES.md#音频-loader-原因复核boot-audio-audit2026-11-11)。
原始日志仍在私有 `.local/x9/work/dual-sim-187/boot130-disk-20261110-164549/`；
只读统计／冻结源码单位对象分析在 `.local/x9/work/audio-loader-audit-20261111/`，不入 git。
本次仅补文档，无代码修改、构建、CI、测试套件或设备操作。

## 当前输出链路状态

**当前是可编译的输出链路基础实现，不是已经出声的驱动。**
没有 MT6993 物理 backend 注册者；`Ready=0`，`Start` 返回
`kIOReturnNotReady`。不访问 off-domain MMIO、不自动起 DSP、不写功放／root，
默认没有自动安装 HAL；新增独立的发布 opt-in 只用于获确认后的最终部署。
麦克风是另一套接口，不在此输出设备中虚报输入流。
PCM ABI 仍为 v1；**board-status v3／160B、dsp-status v2／128B、stream-status v2**，
需配套本轮 `audioctl`，旧工具会拒绝
新版结构。私有 kernel backend 回调也升级为 v2（start 增加预填输入）；不能继续注册
旧版 start 回调。私有AFE platform为 **v6／128B**（explicit memif/producer/generation、
真实validate/producer halt、DSP output committed supplier）；**AFE status为v6／192B**，
新尾部仅缓存producer/generation，旧platform不可注册，旧版`afe-status`工具会拒绝。
AFE/stream/resource 状态 selector 只返回缓存，不申请硬件资源。
新增 `clock-status` 同样只读缓存；`clock-probe` 是 root／mode2 的一次
**有电源、时钟和禁用 OUT4 寄存器写入**的显式事务，不是只读 probe，不能自动执行。
新增 `pll-status` 只读缓存，`pll-probe` 在同一基础事务内额外启停预配置 APLL2、
切换 VLP mux 并写硬件测频器。新增 `path-status` 只读缓存，`path-probe` 进一步配置
禁用的 DL24→OUT4 路由；**clock-probe／pll-probe／path-probe 共用每 boot 一次机会**，
不能连续运行这三个事务。

## 构建

仓库根目录，输出目录必须是新的。当前完整物理接线尚未齐，按单镜像目标
**暂不执行以下 bundle 构建／签名或完整 kernel 构建命令**；开发阶段只直接 `-c` 编译。
以下保留为接线齐后最终构建的入口：

```sh
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I \
  x9/audio/build.py --out x9/out/speaker-hal-only/audio

# 完整 kernel 构建会自行构建、签名并嵌入 speaker HAL，无需先执行上一条。
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I \
  x9/build/build.py kernel --name speaker-final
```

输出 `audio/X9Audio.driver`、`audio/audioctl`（iOS 16 arm64、adhoc 签名）
和 `kernel.development.sweet`。独立 HAL 构建只有显式 `--embed` 才生成资源；
完整 kernel 入口自动传入该参数，嵌入的是当次构建签名后的完整四文件 bundle，
不是旧缓存中的单个 Mach-O。`audioctl` 不嵌入镜像；
`audio/speaker-build.json` 记录内容身份和文件摘要，不推断 runtime backend 已注册。
这不会重建 Metal 插件或修改设备。
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
HAL 原生发现位置是 **`/System/Library/Audio/Plug-Ins/HAL/X9Audio.driver`**，
不是 `/Library/Audio/Plug-Ins/HAL`。首次根盘写入及启用发布必须单独确认；
本构建器只构建／签名／嵌入，不安装、不写 root、不覆盖 Apple `VirtualAudio.plugin`。
尚未验证 20H392 原生路由层是否选中本 HAL，
也没有移植 Sheng 的 Thunderbolt 伪装、VirtualAudio hooks 或 shared-cache 时钟写补丁。

### 最终镜像的完整 HAL 资源与受控发布（阶段40）

完整 kernel 构建先生成 `kernel/generated/x9_speaker_hal.inc`（不入 git），
再编译独立 `x9_speaker_hal.o` 资源对象。资源声明和 PID1 installer 分离；
没有当次完整 signed manifest 时不能链接完整 kernel，不使用空资源或成功 stub。
四个文件严格为 `Info.plist`、`LICENSE.UniXNU`、`X9Audio`、
`_CodeSignature/CodeResources`，拒绝 symlink／额外 entry／空文件，总量上限512KiB。
内容身份为按上述顺序拼接每项 `name ASCII + NUL + size LE64 + SHA256(file)`
后的SHA256；完整资源以原子替换方式生成。

**额外开关 `x9_speaker_hal=1` 默认0，且同时要求 `x9_audio=1` 或 `2`。**
现有 `x9_audio` 参数不会自动安装 speaker HAL；没有修改默认 boot-config。
获确认后，PID1才在可写原生UFS根盘上，将当次四文件写入
`/private/var/x9/speaker/<identity>/hal/X9Audio.driver` 并只读 union 到原生发现目录。
一 boot 仅一次发布尝试；同内容文件校验复用，不截断已有不同文件。
首次写入前检查完整manifest／逐文件摘要／source身份及零尾；逐层目录资格检查
拒绝非root所有权／可被其他用户写入的祖先，不在不合格目录下创建后续内容，
也不修改共享目录权限。完整private bundle目录必须root:wheel／0755且无额外entry。

发布前捕获实际可见原生 HAL（含既有MIC／haptic union），拒绝已有
`X9Audio.driver`；发布后逐文件校验完整bundle和所有原native vnode，失败尝试
unmount并再次检查原native view，分别记录首错和rollback结果，不自动重试。
bindfs例外仅用于实际PID1当前线程、精确source及上述opt-in／UFS条件，不扩大
普通进程的entitlement。发布不boot DSP、不解静音、不注册backend或设置Ready。

本阶段只直接编译实际 `kern_exec.o`／`bind_vfsops.o` 和arm64声明布局，
Python builder只做语法解析。**未执行HAL bundle build／codesign／资源生成／
资源定义对象编译／joint link／安装或20H392最终路由验证**；完整物理后端和唯一
最终镜像仍待完成，不能把默认关闭的发布链称为实体出声。

## 内核／HAL 契约

`kernel/src/bsd/dev/x9_audio.h` 是双方唯一 ABI 定义：

| 项目 | 行为 |
|---|---|
| 服务 | `X9Audio`，IOResources 上注册 |
| open type 0 | 只读 status；不能 start、stop 或映射 PCM |
| open type 1 | root／指定音频 daemon 的独占 writer |
| open type 2 | mode2 的 root 控制会话；不能映射 PCM 或 start 播放，可完成失败后保留的 stop |
| selector 0/1/2 | status / start / stop；拒绝 async、descriptor 和额外参数 |
| selector 7/8 | cached AFE status v6／192B / stream lease + physical cursor status v2；只读，不做 MMIO |
| selector 9/10 | cached PA status v1 / root type2 显式双 PA reg03 probe；不写 PA 值 |
| selector 11 | cached AP resource lease status v1／64B；没有 claim/release 用户接口 |
| selector 12/13 | cached clock status v1／216B / root type2 显式 cold register-clock＋disabled OUT4 配置／恢复 |
| selector 19 | cached LEFT VDD status v1／64B；只读内存，不申请电源或执行 PMIF |
| selector 20 | 同 selector0 的112B PCM缓存，`audioLock`忙时立即返回`kIOReturnBusy`；不触发物理callback、不等待DSP或PA操作 |
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

### 被动发现与真实播放会话（阶段39）

HAL `Initialize` 不再申请 type1 独占 writer、映射PCM或阻挡 `audioctl`。
私有control队列每500ms通过type0读取selector20；缺少服务、旧kernel不支持该
selector或Ready资格不满足时，不提供device／owned-object／stream／control列表，
UID翻译返回unknown，default-device资格与stream-active为0。空列表允许NULL／0B
输出。Busy只跳过本次观察，不回退到阻塞status，也不获取播放权限。
真正Ready迟到或故障时，队列在HAL锁外通知包含列表、Alive／Running、默认输出资格、
ClockIsStable／latency与stream-active；这只是原生HAL发现接点，**不是20H392路由已验收**。

只有显式 `StartIO` 通过被动能力检查和当前Ready后才申请writer／映射PCM，并核对
真实START前后同一控制记录的严格generation+1、格式、时基与anchor。
每个已登记client独立记录running（最多64个），重复START／STOP不增减其他client的
引用，未知client不能播放，remove会停止自己的引用；不新增混音或重叠周期成功路径。
timestamp与WriteMix使用trylock及本次generation／anchor／时基，不等待observer或
物理START／STOP；失去资格显式失败，不发布PCM／时间戳或以墙钟推进游标。
WriteMix要求sample-time-valid、整数且不超过Float64精确整数范围。

最后一个client STOP之后，必须在同generation的真实kernel缓存确认Running=0、
`backend_held=0`与`stop_error=0`，才unmap／close writer；失败保留原连接和maps供
显式STOP再次收尾，remove失败也保留client。历史Fault不会靠observer清除。
内核拒绝generation溢出后再调用物理START，PCM ABI仍v1／112B、已有selectors不变。
这些代码仅直接`-c`编译，未执行HAL、播放、安装、provider或设备操作；完整board／
firmware／双PA／AFE combined backend仍无生产注册者，Ready门没有放宽。

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

AFE platform v6的 `held(context)` 必须是缓存所有权谓词，不做 MMIO；部分 acquire
失败仍 held 时执行 mute/release，只有物理恢复成功才撤 ticket。没有物理 lease 且
无 AFE journal 时不调用 off-domain mute/restore；journal 尚未恢复却丢 lease、
或 release 返回0但仍 held，均隔离并保留 ticket。所有 callback 仍须有界并处理
部分资源，`held` 本身不替代可访问性／quiesce 证明。

`validate(context)` 必须先用 AO-safe 电源／时钟证据证明域可访问，再检查同一
pad／route 所有权与固定 DMA bus／VA／EMI／cache binding；不能 acquire、reset、
reparent 或仅凭 cached held 返回成功。它检查物理租约，不要求 engine-owned
OUT4／DL24 idle。prepare、start、copy、CUR poll、disable、restore 在访问 AFE／DMA
前核对 AP owner/ticket、held 和 validate；失败（含 EAGAIN）在同一次操作锁存，
fault cleanup 不再重试资格化／AFE访问。独立 mute／quiesce 必须自行资格化其硬件路径，
不可读 off-domain CSR；cleanup 失败保留 journal、mapping 与 ticket。之后显式 stop
可重新资格化以恢复，不因历史诊断永久封死 cleanup；不是自动播放重试。

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
就当作恢复。本节AUDIO-10时为私有platform v2；当前为v3（见下），PCM ABI1及原verified注册门不变，仍没有真实调用者。

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
4-bit 布局，支持 MSG_ONLY/PAYLOAD以及56B region-index DMA描述符的字节codec，
不接受用户物理地址。普通`x9_adsp_request`仍拒绝DMA；只有native IPI DMA所有者的
generation-bound physical guard可以发送。HAL/writeback DMA地址形式仍不支持。
OPEN/CLOSE/PREPARE/HWPARAM/START/STOP 等 ID 来自 OPPO 协议定义。
PCM的 OPEN→HWPARAM→PREPARE→START 状态机已在`x9_dsp_pcm`／`x9_dsp_playback`，
但完整AFE、双PA与firmware output authority仍未接齐。外层 mailbox 传输由
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
| `x9_adsp_request_guarded(..., guard)` | native IPI DMA专用；相同generation、完整字节descriptor及fresh physical guard，发送／消费与ACK等待／回复发布均重新资格化；不授Ready |
| `x9_adsp_poll(core)` | 单包接收；无消息返回 EAGAIN，不更新播放 cursor |
| `x9_adsp_next_event(core, event)` | 取 logger/timesync/audio 异步包；最多 16 包，溢出 fail-closed |
| `x9_adsp_shutdown(core)` | 先 halt/reset 再撤 power；reset 失败不释放仍可能使用的资源 |
| `audioctl dsp-status` | iOS 只读查看两个 core 的状态、generation、TX/RX/ACK、超时和错误计数 |

外层头是四个 LE u32（16 B），发送 IPI ID=9、options=`0xffff0000`、
reserved=`0xdeadbeef`；其后是明确编码的 inner audio_ipi，不序列化host struct。
region-DMA按OPPO union ABI显式携带boot-lifetime私有staging的legacy `dma_addr`
token；**不是DSP可解引用地址**，真实supplier必须证明处理的是region-index协议，
不拿这个token替代pool bus或允许用户指针。
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
私有transport platform v3 的 `power_held`＋真实 `validate` 保留部分失败资源与访问资格；
旧platform v1/v2不可再注册，内置MT6993提供者已配套升级。
**sw_ver 未确证，自己取得的 AO vote 保留到本次 boot 结束**，不猜 A0 erratum
已不适用；继承 AO 不撤销。SMC 为 `0xc200052b`，magic 为 `0xad5bad5b`，
a0..a3 原值和首错／撤票错／恢复电源链错分别保存在 cached board status v3；只有零返回被接受。
`registers-valid=0` 时域内 pending/DPSW/ACK 数据标记为 `UINT32_MAX`，不是旧的正常读数；
wake 必须有本次有效域内快照，不能以未刷新字段授权访问。
SMC 本身依赖 secure monitor 返回，AP 不能为一个不返回的 monitor 实现抢占超时。

v3 没有旧 prelock/IPIC 回调。当前不发送 runtime suspend IPI，域持续 pinned；
wake lease 验证全电源链稳定且 DPSW req/ack bit0 均为0，不把 WFI 等同于 suspend，
不将 firmware RSV 状态猜成 Linux enum。新 suspend/DPSW 状态会拒绝邮箱访问，
而不是空回调虚报 wake。固件是否存在／能重载仍须 SMC 结果与真实 fresh READY 证明。

### AUDIO-13：真实 ADSP power／Secure RELOAD／mailbox 访问资格

内置 `x9_adsp_mt6993.c` 的真实validate不是空回调：先核对AP arbiter owner/ticket、
exclusive和owned TOP/INFRA，再读AO HWV AP EN／DONE／global EN／BUSY的CHAIN mask14；
全链稳定且本AP仍持有之后才读CFG pending／DPSW。DPSW req/ack bit0非零拒绝访问；
WFI、firmware RSV、VLOGIC/SRAM ack仅诊断，不推导Linux state或新wake流程。
失败在本次显式control锁存，后续snapshot最多读AO，不读CFG／邮箱，不隐式重新上电。
新control只设置待恢复标志，等取得transport core mutex后的power_on/reset才清本次门；
等待中的管理员stop不会提前清除仍在运行的poll故障锁存。
CORE_STOP／pending wait／SYS_CLEAR、Secure RELOAD和START之间同样重新资格化；
pending的10ms有界轮询每次先核对租约，不只首次检查。

首次cold exclusive接管以真实TOP+INFRA全局off证明两核尚未运行，且本次从未发RELOAD/START，
此时reset直接pending-idle→实际SYS_CLEAR，不先对secure configured状态发无效STOP。
新增private `launch_issued`在RELOAD调用**之前**置位，CORE_START前也保持；加载错误、
加载后资格失败及START错误均保留，必须实际CORE_STOP成功才清。
故障停核、halted验证、再次start、父电源release均检查该状态，
不能把`started=false`或非零SMC返回解释为已停核。该修正不忽略STOP失败、不新增
重试、force reset、monitor op或用户selector；仍须同ticket／真实物理资格及pending0。

保存的slotA ATF中，两条kernel whitelist均选择`c200052b`的ADSP handler，STOP5
只接受per-core状态4、SYS_CLEAR3/reload7核对magic；这与纯源码的EMI READ不可混用。
OPPO `adsp_init_reserve_memory`取得LK `shared_memory`后直接进入WC映射，绕过legacy
MPU设置分支；该行为不是XNU Normal-WB／AP-DSP-AFE权限证据。保存ATF的050b/op2拒绝，
0415/op2为BL-only，不能用旧GPS reader充当speaker memory supplier。保存件不是当前
selected槽位／运行secure ABI证明，RELOAD成功仍不授shared RAM或speaker Ready。
保存件的`0x2a380`通过`0x11aa98`间接调用实际`adsp_mt_run_loader`；`0x59c8`安装
的callback为`0x5520`（dynamic map→copy `0x1a200000`／`0x20000B`→cache sync→unmap），
与CORE_START的`0x55a4`分开。即便这份旧保存件不在loader内启动核，也不凭它对当前
RELOAD失败跳过STOP；若STOP拒绝，保留父电源／票据而不假称halted或直接退资源。

transport在reset完成后清旧RX之前、每次READY接收、发送前及TX-consume／ACK等待
期间调用真实validate。租约失败含EAGAIN作为fatal，不当作“RX空”或可重发；
同次操作不retry validate／reset／power-off，power/mapping/ticket保留。
初次power_on或reset失败不在cleanup自动重做破坏性reset；明确dsp-stop才重新尝试恢复。
普通READY超时／协议错误仍在fresh资格通过时reset后power-off；release返回0但
power_held仍true视为EIO，不虚报释放。wake_unlock必须自行资格化可能触及的硬件；
内置实现只释放软件wake标记。validate耗时不延长2s READY／1ms TX／250ms最大ACK预算，
边界后才观察到的READY或ACK不被发布为成功；失败reply不写回。

`audioctl dsp-status`为 **v2／128B**：主错primary、清理错cleanup分开，lease首错、
首次guard stage和累计checks；cleanup不得覆盖boot/发送的主错误。
cleanup也记录因租约／reset失败而被阻止的清理原因，0不单独证明已掉电；
还需核对board-status和resource-status的实际retained ownership。
`audioctl board-status`为 **v3／160B**：新增首次lease errno、guard-space（lease/HWV/CFG）、
reg及mask下expected/actual、checks；HWV/CFG的reg是各映射相对偏移。
lease空间不是CSR，ticket细节另读resource-status；lease-error0时guard字段无诊断意义。
错误后域内snapshot仍标invalid／UINT32_MAX，新增guard保留拒绝时原值。
读状态不validate、不做MMIO或恢复；guard历史不等于当前资格或Ready。
隔离会话不再交付缓存event，reset generation溢出拒绝boot、不复用代际。

**未执行dsp-boot／SMC／READY物理验证；没有启DMA或功放。** Secure RELOAD仍是
已有固件来源接点，不生成firmware blob；真实固件可重载与IPI13 READY仍须实测。
此轮不注册AFE/TFA write/playback backend，扬声器仍Ready0。

## AFE／功放配置组件与当前缺口

| 文件／入口 | 已实现 | 仍需实际板级提供者 |
|---|---|---|
| `x9_dsp_pcm.{c,h}` | 96B PCM descriptor与32B shared-DRAM record字节codec；真实transport上的INIT／share／OPEN／HWPARAM／AFE PCM配置／PREPARE／DLCOPY／START／STOP／HWFREE／CLOSE；generation-bound消费消息、双快照DTOA与输入环容量检查 | unified AP lease下实际共享DRAM/MPU/cache、task/core绑定与生产调用者；DSP消费不能代替AFE物理played |
| `x9_dsp_playback.{c,h}` | singleton生产PCM生命周期：shared子租约、真实绑定／prepare／预填／IRQ bind／START、同session锁、定向IRQ quiesce及真实producer halt、IRQ引用退休后才drop shared子租约 | 仍需combined backend调用；不注册Ready、不配置双PA供电／profile、不提供output committed或AFE enable |
| `x9_ipi_dma.{c,h}` | 从当前handoff独立IPI_A carveout取得native board child；Normal-WB table/TX及独立GET写回页，动态TASK_SCENE_SIZE／384B region、128B读写索引、DMA_INIT→UPDATE ACK、可信参数→PoC→写索引→guarded SET ACK→实际DSP read_idx；GET payload16／exact地址与容量核对→真实完成supplier→IVAC→私有复制；物理halt后才unmap/drop | 当前firmware/controller/runtime scene／EMI与pool长期排他、可信参数／读命令、reply完成语义supplier无默认实现；没有执行参数加载／GET或校准，不注册Ready |
| `x9_speaker_afe.{c,h}` | explicit DL24=10或DL_24CH=14→OUT4逐字段journal；32-bit I2S slots/S16、48k；AP transition ticket、platform v6真实lease/config guard、combined producer halt；AP copy与DSP外部生产者start分离，DSP模式不碰输出DMA内存，watcher更新独立output committed边界／真实CUR；完全退休后新generation可rebind不同VA | AUDIO/VLP APLL2、pad/物理共享资源lease、DMA bus/EMI/cache、双PA mute、权威selected memif DMA quiesce，以及当前firmware实际output publication supplier |
| `x9_tfa_plj110.{c,h}` | root 显式只读 reg03；PMIF-P SID3 reset/SID5 左供电快照，已开启 I²C6/共享 DMA clock 与 pads 校验、2 CSR bank/1 DMA idle、FIFO WRRD 1+2 BE、逐字段总线恢复 | 不具备双侧供电/reset/write lease；右 rail、真实 binding/profile/校准仍待确认 |
| `x9_i2c6.h/x9_i2c6_native.inc` | 与只读probe共用controller mutex；真实PA epoch下游child、reg8/value16 BE FIFO read/write、双CSR bank/DMA资格、last-prior journal、terminal-only no-replay清理与halt/drain前后屏障；iOS缓存状态 | 必须实际pin住I2C6/APDMA clocks、mux/PLL、GPIO114/115与所有master互斥，现无真实supplier或combined调用者；不授profile/校准/unmute |
| `x9_pa_reset.{c,h}` | 本次FDT SID3双active-low reset拓扑、生产clock PA子租约、低1ms→高1ms/物理DI读回与fresh epoch、PMIF pending no-replay、halt/drain后重新assert low；I2C6下游子租约屏障 | 实际双侧rail/reset物理独占supplier仍无注册者，不提供PA寄存器写／profile／校准／unmute |
| `x9_tfa98xx.{c,h}` | PM container total-size/CRC、双 I2C 类型（container bus=0）的地址、明确名字的两侧 profile、descriptor 边界；共享完整 OPPO stepping 准入＋真实 reg03 revision 比对、AMPE=0 且读回；双侧预检白名单 register/bitfield prefix | 实际完整 revision、immutable container、per-revision 白名单、I2C6/reset/rails 互斥；Probus DSP file/patch/校准/boost 未执行 |
| `x9_tfa_multimsg.{c,h}` | 内核／iOS共用有界 `0x008015` 批消息解析、零起始消息offset view、完整24BE↔signed24-in-32LE转换；全包验证后才输出，无allocation或in-place写 | 只是实际参数材料的framing／word codec；command不获白名单，未证明当前firmware路由／Probus协议／可信profile／校准，未传总线／DSP |
| `x9_pa_native.{h,inc}` | 直接调用真实native I2C6 transport，在同一controller mutex内双侧完整reg03→SYSCTRL→AMPE-off→完整读回；singleton reset-epoch/first fault/unknown write保留，不构造verified=7 | 实际已pin的双侧power/controller supplier及combined调用者；仅安全静音，不授profile/校准/unmute |

AFE start 必须预填至少960帧后才使能 DMA；PA 始终由外部 board 维持 muted。
组件**没有调用 `x9_audio_register_backend`**：service/HAL 已接私有预填／逻辑与物理 timeline
边界，但仍缺真正的 board adapter，将 prepare/start/submit/position/stop 连接至
已确认的资源／PA；underflow 前物理静音和 host anchor 精度仍须验收，
不可把启动前空环的 cursor 冒充已排队播放。
ON 清零不是全部 outstanding AXI DMA 已结束的证据，必须有真实 `quiesce` 后
才恢复地址／释放 lease；任何失败都保留资源。没有共享 lease 提供者时不会触碰 AFE，
也不与麦克风 VUL1/ADDA 争寄存器。captured DT 的 OUT4 是2ch/sync0/mclk0、
`etdm-vlp[4]=1`，不能把普通非VLP APLL2 回调充当已接通的板级资源。

### 完整扬声器续作：DSP PCM 共享区与真实 task 调用链

当前按“完成完整扬声器代码，最终只交付一个测试镜像”推进。此阶段不生成新的
boot候选，也不把组件编译当作完整驱动完成；板级provider与最终backend接线仍在继续。

`x9_dsp_pcm` 根据OPPO `audio_buf.h/mtk-dsp-platform-driver.c/mtk-dsp-mem-control.c`
实现独立PCM task session，所有发送实际进入已有 `x9_adsp_request`，没有成功stub：
INIT25→登记ATOD5/DTOA6→OPEN1→HWPARAM4→AFE PCM HWPARAM0x201／PREPARE0x203→
PREPARE3→预填DLCOPY10→START15→STOP16→AFE HWFREE0x202→HWFREE9→CLOSE2。
部分配置失败不继续后续步骤；失败保留session首错与pending frames，不重发不确定写入。
每次内存读写／sync和消息前后核对同一binding与真实transport READY generation。

| wire record | 格式 |
|---|---|
| PCM `audio_hw_buffer` | **96B**，bridge 6×LE64、attr 4×LE32、threshold／period 4×LE32、hardware／memif／IRQ／memory 4×u8、LE32 counter、u8 ignore_irq和7B零padding |
| `audio_dsp_dram` 登记 | **32B**：bus／AP VA／size／AP VA各LE64；bus必须由真实provider提供，不从CPU VA猜物理地址 |
| HWPARAM／PREPARE／DLCOPY | **8B LE64共享descriptor地址payload**，不是IPI_DMA，也不是把96B结构放到8B地址槽 |

128B是本组件的共享区／cache-line对齐要求，不是descriptor长度。DSP genpool和AFE的
`init_ring_buf_bridge`使用exclusive end=base+bytes；AFE寄存器END仍是inclusive，不能混用。
格式使用Linux S16_LE enum2、48k／2ch／DL1；ETDM valid width15／slot31不属于该字段。
Ring长度必须包含整数个period；provider不能直接假定128KiB可整除480帧period。

单个ATOD record以NEED_ACK保护复用，即便流已经开始，TX-consume／BYPASS不能证明DSP
已经复制descriptor。START／STOP保留源码DIRECT语义，成功只说明发送完成，**不证明AFE已
开始、已drain或有物理anchor**；双PA mute／AFE quiesce仍必须由combined backend执行。
消费事件只接受同generation／scene、DSP→kernel的IPI9／msg27，RESET／UNDERFLOW隔离；
DTOA读回使用两次sync后的稳定双快照，不采用撕裂cursor。读指针、类型、容量和剩余数据
校验通过才前进consumed；它只是DSP输入环消费，绝不能当成扬声器played。

每个DSP generation只允许一次OPEN，避免同代际旧DTOA事件混入第二条流。最终board需
在真实mute／drain后用新reset／READY generation启动下一流，不能换session对象伪造恢复。
本组件不申请资源、不boot DSP、不unmute PA、不注册Ready；实际共享DRAM／MPU／cache、
core通知、AFE／功放与iOS播放backend仍需完成集成。

### 生产 PCM／IRQ 生命周期（阶段29）

`x9_dsp_playback` 已直接调用现有shared／PCM／IRQ／board入口，不接收任意
caller提供的validator，也不注册播放后端。它持有boot-lifetime session，两把锁分工：
生命周期锁串行公开prepare／submit／start／halt／release；session锁覆盖所有PCM
读写，并实际传给SPI570 worker。IRQ bind／quiesce／release不持session锁，避免等待
排队worker pin时死锁。所有memory／route／drain supplier仍必须真实、长期pin且不反向重入。

| 入口 | 实际流程／边界 |
|---|---|
| `prepare(ticket,generation,afe,start_frames)` | 同真实Speaker ticket／Online generation；先hold shared PCM child，再取得真实binding及独立AFE region；显式480帧／64period／S16 stereo48k descriptor由实际route supplier验证后才OPEN、HWPARAM、AFE params/prepare和PCM prepare。每generation一次，失败不换对象重试 |
| `submit(ticket,generation,pcm,frames)` | kernel-resident且调用期pin／immutable的输入；与worker同session锁；只写输入环。拒绝ATOD／DTOA／输入环及独立AFE输出region重叠，不读取或sync输出环；输入容量不足的pre-write EAGAIN可重试，authority／mailbox错误锁存 |
| `start(ticket,generation)` | 尚未满足input start threshold时EAGAIN且不bind IRQ；否则实际IRQ bind，再fresh检查该IRQ首错／owned config及route，最后真正PCM START。输入预填／START均不当output committed或AFE enable |
| `halt(ticket,generation)` | 外层已双PA mute及AFE disable/drain；关新submit，定向IRQ quiesce／worker drain，尽力一次PCM STOP/CLOSE，再独立selected memif drain、board op1及fresh同generation Offline＋actual halted。protocol错误保留但不单凭STOP失败阻断独立安全halt；IRQ／drain失败不reset。无AFE re-entry／恢复／掉电／ticket release |
| `release(ticket,generation)` | fresh actual halt；先定向IRQ release退休所有session引用，再shared PCM drop重新独立drain，最后退休session。任一失败保留对象／子租约；shared release在child存续时在任何unmap或父资源操作之前返回EBUSY |

IRQ quiesce／release现在必须带原`ticket,generation,session`；旧无参数调用已退役。
错配立即EPERM，不mask／restore／drop其他session，也不污染其首错。
`irq_binding_held`只判断cached归属（含partial bind失败），不是Ready；新的IRQ
validate在同session锁下检查真实config／route和IRQ首错，避免worker失败后仍继续START／提交。

shared PCM drain/drop物理资格允许同generation Quarantined和真halted Offline，
不被online-only PCM validator或历史reply／DTOA错误卡住；仍必须真实memory authority、
actual pmap/cache binding和独立selected DMA drain。它不授权任何新RAM访问或START。
若保留SEMA_AUDIO，board halt只能requalify后返回EBUSY，需外层显式resolve semaphore，
随后再次显式halt；不自动重放uncertain toggle。prepare在hold之前失败则本组件没有
reset授权，外层独立halt/drain后才能release。halt返回0只证明安全物理停核，不清protocol首错。

上述native halt是最终AFE `halt_producer` callback要调用的实际实现，**目前尚无combined
platform／backend注册者**。shared memory/route/drain真实suppliers、output publication、
RIGHT／LEFT长期排他供电、controller全master lease与功放可信profile仍缺；iOS PCM／HAL
ABI和Ready门不变，无新用户selector、部署或出声结论。阶段29只直接单位编译，不生成候选镜像。

### 接同一张票据与本次 LK 共享内存布局

**这里是完整backend的内核接点，不是已注册的播放后端。** 同票据/producer halt从
private platform v4起引入，现在v6还要求显式producer与实际DSP output supplier；
PCM ABI1和iOS selectors不变，AFE cached status现在v6/192B。没有修改HAL路由或授予Ready。

| 入口 | 语义 |
|---|---|
| `x9_adsp_mt6993_speaker_control(op,ticket)` | 校验真实AP Speaker ticket，op0首次boot、op1停核并保留父资源、op2在AFE恢复后撤销DSP父资源；不递归claim、不释放caller的ticket |
| `x9_speaker_afe_prepare_on_ticket(ticket)` | 使用同一张票据；没有`halt_producer`的CPU-only platform不能走此入口；fault/watchdog cleanup也不会提前释放borrowed ticket |
| `x9_speaker_afe_quiesce()` | 双PA mute→selected memif/OUT4 disable→权威selected memif drain→独立资格化的真实producer halt；暂不恢复journal/mapping或释放资源 |
| `x9_adsp_halt/release` | 停核与掉电分开；release不接受未成功halt的core。已成功halt的重复请求仅fresh validate，不重放reset；失败后需新的显式halt恢复 |
| `x9_adsp_mt6993_copy_handoff()` | 返回boot-lifetime、两端校验的256B布局；缺失/改变返回ENXIO，输出不变。不映射DRAM、不证明EMI访问权限 |

完整teardown顺序是mute/disable/drain/producer halt→恢复AFE journal→恢复AFE/pads/clocks→
共享RAM unmap／child drop→DSP release→最后释放一张AP Speaker ticket。`halt_producer`也在引擎watchdog和prepare
失败的cleanup执行，不能只在HAL主动stop时停DSP。它必须实际进入board/transport的
halt路径；只收到PCM DIRECT STOP、cached Offline或一个空回调都不够。callback不能
反向重入AFE，后续validate必须能检查同一租约下已停核但仍有电的DSP。
Admin `dsp-boot/stop`在combined association存续期间返回EBUSY，不能重置播放会话。
失败boot即便没有剩余物理votes，association也保留到caller的显式op2。

shim `adsp_handoff.c`从**本次LK FDT**的adspsys `shared_memory`读取两个BE64，按OPPO
`adsp_mem_device_probe`的enum顺序累计非零块的4KiB向上取整大小，跳过零块。
该合同固定已捕获的PLJ110含XHCI variant，而不是猜Linux可变的AUDIO enum数字；L2
property可缺失或为0，重复/畸形property不能当成缺失。CFG/CFG2、四对mailbox
BASE/SET/CLR、两core system image区域和cell/ranges合同同时核对。
`fdt_property_checked`区分absent与malformed/duplicate；既有boolean API语义保留。

基础资格现经 `fdt_capture_nodes()` **一次完整树遍历**捕获root、soc、adspsys、两core及GIC，
随后从同步调用内的局部records解码，不再为每个标量和named-reg反复遍历整棵树。
复用同一严格lexer，全部required nodes唯一存在、被捕获node的所有property key唯一，
valid END和尾部padding核对后才发布；超容量或超过127字节的key直接拒绝、不截断，
失败不发布partial records。ADSP容量96条，已有旧DT五node共66条；**不是当前LK grant**。
没有全局cache，不跨DT修改/boot保留指针；可选I2S4/PA的strict schema/phandle资格仍走
原完整树路径，不能称所有ADSP查询已single-pass。增加入口/捕获/布局三阶段日志供最终
镜像定位。当前 `X9_ADSP_LOADER=0` 不变：130停止在pre-XNU FDT阶段，重复遍历是
已确认源码工作量，**不是已证明的watchdog/启动失败根因**，本轮无重新启动证据。

共享区还必须与本次有效live RAM allocator map完全不相交；小RAM/default boot模式
不授予布局。内核再次核对chosen handoff、完整RAM-map计数/边界/总字节/零尾和交集。
按现有捕获值推导：AUDIO_COMMON offset **0x543000**、bytes **0x852000**，所有分块
累计 **0xd95000**，whole reserve bytes **0xda0000**；以捕获base0x160000000计算pool
bus是0x160543000。这只是源码与旧DT的布局算术，**不是本次启动的grant或EMI证据**。
pool并不16KiB页对齐，后续子分配/映射须保留与XHCI相邻页边界，不可清零整页。

共享RAM映射／daemon登记／core DTOA处理组件现已编写，见下一节；仍没有真实memory
authority／combined caller，因此不访问pool、不宣称登记或播放已运行。native IRQ
provider与实际DTOA线程caller已编写（下节），未被combined backend绑定／启用。
生产AUDIO/APLL2及I2S4 pads子租约已有实现，见下节；真实双PA／drain回调以及PCM到iOS backend的接线仍未完成。
独立cold clock/pll/path probe仍要求TOP/INFRA baseline，不能拿它们的成功flag冒充
combined acquire；生产入口独立按真实DSP父owner的顺序申请。固件/共享RAM不能用Device映射；
不采用WC/WB混合别名。最终只在这些生产接线完成后冻结并构建一个镜像。

### 将 core-A AUDIO IRQ 接到真实 DTOA 线程

**代码入口已写，尚未实际绑定／启用；不是独立输出 committed 证明。**
完整目标继续只构建最终一个镜像，本阶段仅单位编译，不执行前述独立候选构建／部署命令。

- 原OPPO v3 `audioirq`是四cell GIC SPI：`0 / 0x21a / 4 / 0`，对应
  INTID570、level-high。shim一次基础capture新增GIC node，核对当前root／soc parent、
  唯一phandle、完整`sysirq/wdt/audioirq`列表、四cell格式与GICD/GICR区域；
  `interrupts-extended`／不同parent／map拒绝。单独32B chosen
  `x9-adsp-audio-irq`与原256B pool ABI分开，默认全零。**loader仍默认0**，
  旧Android capture不授予本次IRQ绑定。
- `sleh_irq`从真实`ICC_IAR1_EL1`获得INTID，先调用
  `x9_adsp_audio_irq_filter`。只接管已owned的570，其他IRQ继续原external
  handler，timer／IPI和EOIR处理不变。`sleh.o`从prebuilt改为实际源码编译；
  只改源文件而继续链接冻结对象不会使这条链生效。
- native provider使用与实际`pe_init_fiq`相同Device GICD区域和逐页pmap；
  不重置global distributor／redistributor，不改Group／trigger／timer／IPI。
  冷准入要求本SPI disabled／inactive／not-pending、Group1NS／level trigger，
  保存一个priority byte与一个IROUTER，目标采用真实当前MPIDR（IRM0／含AFF3）。
  每次unmask必须enable readback或新的**真实IAR delivery**证明；busy／host time不充数。
- hard IRQ只mask本SPI和wake thread，不拿board/shared/PCM睡眠锁、不访问CFG／RAM。
  worker使用backend同一PCM mutex，fresh ticket／READY generation／shared与实际AFE
  route后，调用板级ACK：CFG+0x5c只清SPM bit0并整字readback，CFG+0x38只写
  AUDIO bit2；**不读CLR当pending，不把ACK当DSP输出完成**。然后实际调用
  `x9_dsp_shared_dtoa()`，在SEMA_AUDIO下处理同session的input consumed。
  任一失败禁止重新unmask并保留首错，不冒充mute、output或Ready。

backend调用顺序（kernel-only API，无新用户selector）：

| 阶段 | 必需调用／条件 |
|---|---|
| 首次绑定 | 真实shared／PCM和AFE route已建立；`x9_adsp_audio_irq_bind(ticket,generation,session,session_lock)`；每generation一次 |
| 所有PCM操作 | 与IRQ worker共用同一`session_lock`；public IRQ lifecycle调用时不能持该锁 |
| 故障／stop | PA mute＋AFE disable／独立drain后`x9_adsp_audio_irq_quiesce(ticket,generation,session)`；禁止新callback，等待worker pin退休（500ms限额）和GIC active清除 |
| 停核 | IRQ child已quiesced才允许真正board halt；child仍pin parents，不允许restart／power-off |
| 最终退休 | real halted后`x9_adsp_audio_irq_release(ticket,generation,session)`清本SPI pending、恢复仅saved priority/route并读回，最后drop IRQ child；然后PCM child退休及原shared/pads/clocks/board清理 |

关闭先阻断新delivery再等待worker **pin**，不只等当前callback；避免已排队线程在
release后访问旧session／mutex。超时／配置第三值／uncertain保留child与journal。
上述实际caller不包含combined注册者，当前没有运行IRQ/CFG/SMC操作或上板证据，
也不替代尚缺的EMI、双PA rails/controller、可信profile/probus保护、真实输出publication
supplier和最终iOS backend。

### 接共享 DRAM、真实信号量和 core DTOA

**本组件没有默认成功的EMI／物理租约／IRQ回调，也没有注册播放backend。**
**private shared policy v3（64B）要求immutable `afe_memif=10/14`及reserved=0；
PCM binding仍v2（136B）。旧policy或缺`validate_afe_route`／`validate_afe`直接拒绝。
iOS selector、96B/32B wire与256B handoff不变。**
`x9_dsp_shared` 的 `validate_memory` 必须逐次证明物理子区独占、当前AP/DSP/AFE
EMI/SMPU权限、实际固件build的daemon scene及core-A task routing；不能从FDT布局、
SMC返回0、cached ticket或用户输入的verified位推断。缺任一必要callback即拒绝
acquire，未映射/清零pool。memory和drain callback逐次收到同一selected memif；
`validate_drained`必须在unmap前独立证明该memif真实drain，不能以DL24的停止证明替代DL_24CH。

输出AFE描述符不能借输入环的权限直接发布。shared提供的`validate_afe`绑定**独立
AFE allocation**的完整bus/122880B边界、DRAM、S16/stereo/48k/DL与整数period容量，
不接受输入环或任意其他DMA地址；另调用mandatory `validate_afe_route`证明同Speaker
ticket/generation/task下真实hardware memif、IRQ usage/ownership及固件route。
PCM先snapshot候选描述符，ATOD写入前、sync后/mailbox前、真实请求后均重新验证；
建立AFE参数后，每次后续admission继续验证原AFE binding。验证失败留首错，EAGAIN
按authority故障处理、不当cursor retry，不能靠NEED_ACK或DSP输入消费量放行。

OPPO `mt6993-afe-common.h`明确`MT6993_MEMIF_DL24=10`，而`DL_24CH=14`是另一
memif；旧DT `/snd-audio-dsp`的playback MEMDL是14、primary为-1，**不是当前firmware
对任一生产layout的授权证据**。`get_taskid_by_afe_daiid()`还要求default bit0与实际
runtime/ref-runtime，不能仅看scene7/32或启用位。当前没有真正输出route supplier；
这些拒绝与验证接线不等于已改好固件route。AFE原start/copy仍只供AP producer；
DSP专用无写环入口已实现（见下节），但其实际firmware output提交边界supplier仍缺，
不能把输入`consumed`冒充输出`produced/played`。

### 选择实际 DL24 或 DL_24CH 生产 layout

**只能显式选择经当前firmware／DMA／IRQ supplier证明的memif；无default、名称fallback或
自动从旧DT授权。`x9_speaker_memif.h`是生产descriptor，旧`x9_speaker_dl24.h`的
PBUF/MIN/MAX=0仍只供cold diagnostic，不能冒充生产trigger profile。**

| 字段 | DL24（C memif10） | DL_24CH（C memif14） |
|---|---|---|
| BASE／CUR／END／CON0 | 0x46b0/46b4、46b8/46bc、46c0/46c4、46d0 | 0x4620/4624、4628/462c、4630/4634、4640 |
| enable／stereo | ON bit28；MONO bit4普通RW=0 | ON bit31；NUM bits24..29直接写2，不是2−1 |
| actions | SW_CLEAR bits18/7 | SW_CLEAR/BUF_EMPTY_CLR/HANG_CLR bits18/7/4；bit4不进RW journal |
| PBUF／MIN／MAX | 256B/64B/64B，source枚举3/3/3 | 64B/16B/64B，source枚举1/1/3 |
| OUT4 CH1／CH2 route | CONN116_2/117_2，bit16/17，I_DL24=80/81 | CONN116_1/117_1，bit22/23，I_DL_24CH=54/55 |

两layout均用FS10、normal S16。BASE/END MSB只写0x1ff，low只写0xfffffff0，
保留非地址低nibble；END由inclusive `base+bytes−1`编码，DSP bridge仍exclusive end。
CUR保留完整low32、high按0x1ff取stable高低高快照。ONE_HEART／DOMAIN／AUTO_MUTE及actions
准入必须inactive，不盲清或重放。全部七个合法route banks持续核对；owned bank由journal
完整核对，另一个memif不得enabled。每次RW后先fresh lease再readback，失权保留uncertain。

private AFE platform自v5使用旧alignment padding存memif，目前v6/128B；cached status
自v5使用旧reserved存memif，目前v6/192B另追加producer/generation，`dl24`旧slot表示selected CON0。
`audioctl afe-status`打印`memif`和`cached-CON0`，不会为诊断访问MMIO。选择layout本身不
注册backend、不enable DMA、不unmute；AP start/copy仍不可直接接DSP输出。

### 接 DSP 外部生产者，而不是把输入消费量当播放量

**无实际`output_committed` supplier，不能注册DSP platform或开始播放。** 本地OPPO
`mtk-dsp-platform-driver.c:mtk_dsp_dl_consume_handler`把DTOA/payload的`pRead`同步到
**输入环**；`afe_pcm_ipi_to_dsp`仅发送AFE参数，没有给AP返回独立AFE output的committed
record。不能把这些源码中的DLCOPY/START ACK、输入pRead或时间换算当输出发布协议。

| 入口／状态 | 行为 |
|---|---|
| platform CPU=1 | generation必须0，output callback必须NULL，sync_device必需；保留AP prefill/copy |
| platform DSP=2 | generation必须真实READY、output_committed/真实halt_producer必需；只接受borrowed Speaker ticket，每generation一次prepare尝试 |
| `start_dsp(anchor)` | 独立supplier证明从AFE ring offset0开始的当前代际单调S16 stereo输出prefix及DSP→AFE可见性；至少960帧后OUT4→memif enable。prefix不足返回EAGAIN，不写enable、不fault |
| CPU start/submit用于DSP | ENOTSUP且不访问DMA内存；DSP路径不调用bzero/memcpy/sync_device，也不clean/invalidate输出 |
| watcher／position | 每次fresh实际output证明后更新committed；拒绝倒退、超过last played＋ring capacity−32或超过committed的CUR。played仅由稳定硬件CUR更新 |
| supplier变慢 | 更新前后都保留**旧已确认prefix**的低水位／deadline；晚到的新count不能掩盖旧prefix可能已耗尽 |
| fault／stop | 原有双PA mute→disable→独立DMA drain→实际DSP halt→journal restore/release；不因provider错误撤掉父资源 |
| `rebind_dsp(new_platform)` | 必须原lease/journal/dirty/uncertain/copy_pending/poisoned全退休且old held=false、new generation严格递增；只替换binding，不再创建watcher，允许新shared map返回不同VA |

supplier必须验证**实际firmware输出发布**，完整prefix与同generation/memif/bus/size，不是
裸write pointer、缓存count或输入消费；缺证据拒绝，不安装成功stub。DMA输出归DSP写，
producer不能覆盖未播放帧及32-frame margin；supplier不反向重入AFE。除已证明的prefill不足，
supplier的EAGAIN也作authority failure，不当cursor retry。anchor只是enable命令时间，非实体出声证明。

`audioctl afe-status`为v6/192B，缓存打印producer（1=CPU/2=DSP）和generation；
`submitted/committed`在CPU模式是AP提交帧，在DSP模式是**独立证明的输出committed**，
不是PCM session的input submitted/consumed。HAL render/control ABI未改，实际backend未注册。

| 分配 | 相对16KiB向内对齐map起点的offset | 大小／wire含义 |
|---|---:|---|
| core flags | 0 | 分配256B，登记的`audio_dsp_dram.size`是**16B**，两个LE64 ATOD／DTOA flags |
| task ATOD | 0x100 | **1024B**原始task分配；PCM descriptor仍96B |
| task DTOA | 0x500 | **1024B**原始task分配；与ATOD不共享cache line |
| DSP输入环 | 0x4000 | **122880B**，64×480帧stereo S16 |
| AFE输出环 | 0x24000 | **122880B**，独立物理子区，不把DSP输入consumed当played |

只映射向内对齐的 **0x44000B**，不占XHCI边界页，不映射整个8.5MiB pool。
使用XNU `ml_io_map_unmappable(...,VM_WIMG_COPYBACK)`／kernel_map，避免Device DRAM
和永久8MiB io_submap；映射前后及每次访问验证逐页pmap地址、cache metadata和现有
physmap别名，不修改全局cache属性。PoC同步在禁抢占后读取本CPU CTR：AP→DSP clean
用`dc cvac`，DSP→AP **只invalidate**用`dc ivac`，前后`dsb sy`；不clean旧DTOA覆盖固件。

MT6993 v3的真实`SEMA_AUDIO`是 **CFG+0x64、AP bit14（4×3+2）**，写入为toggle。
acquire拒绝已有AP bit和递归调用；每次资格化的0读回只允许一次toggle，写前保存
uncertain、写后重新资格化/readback；最多5000次明确未获取的尝试。不确定toggle
不盲重放，失去已持有bit返回ESTALE并保留隔离；新的显式cleanup只可在fresh AP bit0
时退休LOST记录，bit1可能是外来持有者，不toggle，也不清shared/PCM历史fault。独立单调lease token防止旧release
释放新持有者。活动／不确定sem阻止reset/start/power-off，未知继承AP bit同样阻止
reset/撤父资源。资格失败后不继续读域内CSR；显式halt可以重新资格化但会EBUSY，先
`x9_dsp_shared_resolve_semaphore()`处理保留的sem，再重新halt，不用软件清状态假恢复。

映射前的board shared child lease阻止父电源撤销/重启，halt仍允许；失败mapping／
sync／send保留mapping、child、AP ticket和首错。只在fresh Offline generation、
board真实reset_done/无pending/无sem及真实AFE drain同检后unmap，再drop child；
drop失败可以只重试drop，不能二次unmap。每代际只允许一次shared初始化尝试。

登记实际走`x9_adsp_request`：daemon `EINGBUFASHAREMEM8`发送16B whole-pool
bus/bytes；`COREMEM_SET7`发送32B dram record，size16B。二者保持原BYPASS语义，
**TX-consume不是DSP处理成功或扬声器Ready**。daemon enum受AUTO build条件影响，
不能硬编码44；task仅接受固定core-A PRIMARY7或AUDPLAYBACK32，实际route仍要authority。

`x9_dsp_shared_dtoa()`只供生产core-A AUDIO IRQ线程调用；调用者必须先证明/ack真实
IRQ源，不将mailbox IPI9当core IRQ，也不轮询任意RAM。它在真实sem下invalidate并读
已登记core flags，持sem调用同一PCM session的DTOA双快照，成功才清本task bit并clean；
未知scene bit保留并隔离。native IRQ注册／DTOA线程caller现已编写，但未实际绑定／
启用，不声称core通知已跑通。输出ring的
sync/binding和PCM binding已有实现，最终仍待combined电源／AFE／双PA／iOS caller。

### 接生产 AUDIO/APLL2 时钟子租约

**生产时钟代码不是已注册的播放backend。** `clock_acquire`仅在本次真实DSP READY
之后使用同一张Speaker ticket；缺独立的权威DL24 drain callback即拒绝。它不调用
cold probe、不借probe成功flag，也不授予Ready或使能DMA。首次申请前／pin后均检查
真实Online、无transport error及同generation，随后再进行AUDIO SMC与时钟操作。

| 入口／资源 | 实际含义 |
|---|---|
| `x9_adsp_mt6993_clock_hold/drop` | 独立于shared-RAM的单调子租约；pin DSP父电源，阻止重启／父资源释放，允许真实halt；完成clock restore后才drop |
| `x9_speaker_mt6993_clock_acquire(policy)` | 只投票AUDIO bit0，不接管DSP已有TOP/INFRA/AO；AUDIO全球off和全部foreign HWV仍严格验证；失败保留child／journal，不自动撤销父资源 |
| AFE bank1 common CG | `0x10` mask **0x101b**：HOPPING0、F26M1、APLL2 3、H208M4、TUNER2 **12**；先只开HOPPING/F26M，真PLL／测频资格后才开APLL2 leaves |
| tuner／ENGEN／OUT4 CG | AFE `0x38` mask0xfff7写0x375；`AUDIO_ENGEN_CON0=0x14` bit3；bank3 **0x08** bit17只ungate OUT4 leaf，不是engine enable。三字段独立journal，不接管AFE的OUT4格式／DL24 route |
| `clock_validate/clock_afe` | fresh board/HWV/mux/APLL immutable PCW及consumer、逐页AFE CPU→PA与owned clock image；**不是Online/Ready/start准入**，允许同代际transport quarantine／保留历史stream fault时执行安全AFE disable/drain |
| `clock_release(ticket)` | 真实board halt＋mandatory实际DL24 drain→restore tuner/OUT4 CG→PLL→common clock/AUDIO→fresh AO原值／AUDIO-off→drop child；失败保留资源／首错 |

物理validate不要求所有memif idle，因此允许租约内AFE运行；它不会清掉native clock
历史error、`j.stuck/pj.stuck`或不确定meter/update标记。未知时钟动作／第三方journal值
仍拒绝访问与掉电。上层start/submit必须独立验证其真实DSP/PCM session；同代际
quarantine不能成为新流许可。`AUDIO_ENGEN_CON0_MON=0x7c`没有公开bit语义，本子租约
自己的APLL2 generator活跃时不把MON非零当foreign，也不把MON零当DMA drain证明。
运行期同时fresh检查INTBUS／AUDIO_H的VLP CKSTA bit30/29无UPDATE在途、APLL2
FENC0x34 bit4仍on及ENGEN低4位仍只有本子租约的APLL2 bit3，不借SEL读回或旧测频
flag忽略时钟漂移／外来generator；FENC-on仍不等于PLL lock或48k精度证明。

common CG、native tuner与PLL RW journal仅可用**最后一次写前prior**收敛uncertain，
不能用最初cold saved值替代。meter trigger仍须fresh BUSY rise/fall；缺完整cycle
不重放trigger、不撤PLL。首次pin后准入／map失败且未发出任何clock动作时，真实halt
与drain通过即可drop未触碰child，不为了清理强求尚未建立的AFE映射。
完整restore后重试drop只检查AO，不读已断电AFE，不二次restore。下一流必须完整
cleanup并获取新的真实generation；永久CSR mapping首次创建后复用，避免每流泄漏。

锁顺序为AFE→clock→board或transport→board→AP resource leaf。clock policy的drain
callback必须独立资格化、不能反向重入clock或AFE；caller串行化生命周期。cached
`clock_held`仅用于ownership，不替代fresh validate。iOS clock216B／PLL264B／board160B／
DSP128B等既有诊断ABI保持；AFE现在v6/192B，未增加Ready或用户可控的生产验证位。

本阶段只直接编译board／clock／AFE／shared／SweetX9Audio及iOS header ABI对象；
没有重编HAL bundle、联合kernel/boot、运行测试套件或操作设备。生产clock目前
**无combined调用者**；SMPU／实际firmware scene、core-A IRQ、pad／PA／drain与最终
iOS backend仍需实接，不能将这组API当成完成出声或最终镜像。

### 在生产时钟下申请、恢复 I2SIN4/OUT4 pads

**只有元数据和子租约代码已接入；真实双PA mute、DL24 drain及combined caller仍未接入。
`X9_ADSP_LOADER=0`保持，不会因此自动申请pad、执行MMIO或得到扬声器Ready。**

本次LK FDT须同时绑定AFE的`pinctrl-15/17`至I2SIN4/OUT4两组，GPIO108/109/110/111
都为mode5。shim资格化`/soc/pinctrl`的GPIO/IOC_LB2基址、AFE与sound的phandle绑定、
机型speaker enum OUT4=25/IN4=24及ETDM index4的2ch/sync/no-MCLK/VLP/IP配置。
这两个数来自`mtk-sp-spk-amp.h`，不是`mt6993` C DAI枚举。group必须只有3/1个指定leaf；
leaf只允许pinmux、Schmitt、bias-disable和可选phandle，拒绝额外/重复pinconf与children。
`fdt_node_schema`和`fdt_phandle_unique`复用完整树parser，后者保守拒绝重复phandle别名；
没有跳过完整END/reserve验证，旧lookup/checked的absent契约不变。

`x9_adsp_handoff`仍为256B，尾部原8B保留区用于pins/modes向量，
`X9_ADSP_I2S4_VERIFIED`仅表示本次拓扑；另有下面的PA reset拓扑可选mask0x4。
flags严格只接受layout-only 1或带可选拓扑的3/5/7，不接受unknown bits，
无I2S4 flag时尾部必须仍为零；复制端重新完整校验，不把旧capture当本次grant。

| 入口／字段 | 行为与边界 |
|---|---|
| `x9_speaker_pads_acquire(policy)` | 同Speaker ticket、真实Online无error/同generation、validated I2S4 handoff、native clock子租约；mandatory双PA mute成功后再读/改pads，mux mode5最后写 |
| GPIO MODE0x4b0／DIR0x30 | 冷态拒绝任何alternate mode或DIR output；MODE用+12 MWR，低4位为mode，bit4只作modify-enable，不作为saved重放；DIR仅owned bits12..15 |
| IOC_LB2 SMT0xe0／PU0xb0／PD0xa0 | GPIO108..111 bits4/6/5/8（mask0x170），Schmitt是DIR0/SMT1，不改IES；bias-disable仅清PU/PD。非MODE用+4 SET/+8 CLR，不覆盖邻pin |
| DO0x130／IES0x70／drive0x00/0x10 | 保存并fresh验证，完全不写；DT无drive-strength不猜数值。GPIO基址非16K对齐，mapping验证包含VA页内偏移及完整4K bank两端 |
| `clock_pad_hold/validate/drop` | clock mutex下单调token，父native clock持有时子租约存在；pad未drop前clock_release立即EBUSY，不执行drain、restore/vote或污染流故障 |
| `pads_validate` | fresh parent/board/HWV/映射、owned字段与未触碰字段；历史stream error或同代际Quarantined本身不阻止物理cleanup，不等同新流Online许可 |
| `pads_release(ticket)` | 真双PA mute→真实board halt→独立权威DL24 drain→先退出mode5→逆序恢复仅touched电气字段→fresh saved检查→drop clock子租约。开始teardown即撤configured，部分restore不能当mode5有效 |

每次SET/CLR/MWR在动作前记录最后观察prior并标uncertain，动作后先重新资格化父资源
再读回；cleanup只允许expected或最后写前prior，第三值保留父租约/首错、不自动unwind。
drop失败重试fresh saved/parent验证，不二次restore；没有任何pad写动作时可在真mute/halt/drain
后退休untouched child，不强求未建立的pad映射。永久CSR映射首次创建后复用。

锁顺序AFE→pads→PA reset→clock→board/transport→AP leaf；policy回调禁止重入AFE/pads
和clock生命周期。真实PA mute回调可调用read-only PA reset/clock物理资格，
但不能从持clock mutex的回调反向调用它们。
本阶段仅直接编译内核新pads及受影响对象、shim walker与iOS header/ABI对象；没有HAL
bundle重编、测试套件/模拟provider、CI联合kernel/boot、设备操作或播放证据。

### 双PA active-low reset 与 I2C6 父资源屏障

**OPPO `tfa98xx_parse_dt/ext_reset` 明确 `reset-polarity=0` 为低1ms→高1ms。
捕获pinctrl的`output-low`是asserted reset，不是解除复位；旧只读探针的DO/DI低准入已纠正为高。
这是源码与配置纠正，未执行设备reset或读取本次物理电平。**

shim `adsp_pa_reset.inc`本次验证PMIF-P/`spmi-dev-mask`、SID3 PMIC/controller phandle、
GPIO10/11、两侧TFA0x34/35与default pinctrl/active-low配置。group/leaf schema严格拒绝
额外/重复输出pinconf及children，不硬编码捕获phandle。`X9_ADSP_PA_RESET_VERIFIED`是
handoff可选 **mask0x4（bit2）** 的拓扑bit，256B布局不变；它不授予供电、reset epoch或I2C写资格。
`X9_ADSP_LOADER=0`仍关闭，当前没有该live grant或combined caller。

`x9_pa_reset_acquire(policy,&epoch)`必须同真实Online/no-error generation、native clock PA
子租约及本次拓扑，另需**实际已pin的双侧rail/reset独占物理supplier**，覆盖整个reset/I2C
生命周期，不能仅提供一瞬电压/DO快照。右侧没有DT supply，不能把左供电、dummy regulator、
I2C ACK或可控boolean替代它；当前该mandatory supplier没有实现/注册者，故不会开始reset。

| 动作／入口 | 实际行为 |
|---|---|
| cold reset 准入 | SID3 mode0、DIR output、AD_SW_SWITCH/AD_SWITCH数字SW、DO/物理DI均低；拒绝继承high/alternate，不把已解除reset当fresh epoch |
| reset pulse | 在真实PMIF mutex下低态至少1ms→仅DO0xa3 bits2/3 RMW到高→至少1ms→fresh DO/DI高及外层供电/Online复查才mint epoch；不写DIR/MODE/drive/解锁key/rails |
| PMIF 写入／journal | GPIO属于MT6661 `FLAG_MT66XX` 的regmap_update_bits，不使用AP GPIO SET/CLR；保存最后写前prior，只改GPIO10/11、保留邻bit，uncertain在命令前置位；失败不重播reset pulse |
| `pa_reset_validate(ticket,epoch)` | fresh parent/双侧物理lease、PMIF CPU→PA首尾、reset image与DO/DI；允许同代际历史流fault后的物理mute，但不是新流Online/profile/校准授权 |
| `pa_reset_resolve_pmif` | 真halt/drain后显式解决本owner pending；Read只有实际late VALID才能读弃并CLR，不把idle当“未来不会有响应”；timeout write/CLEAR不重放，不清未知error或别人的VALID |
| `pa_reset_bus_hold/validate/quiesced_validate/drop` | I2C6下游单调token。hold须真实Online/noerror/fresh epoch；native controller清理前先quiesced_validate实际halt/独立DL24 drain，真正bus/FIFO/DMA idle及CSR恢复后drop再验证，不提前撤PA reset |
| `pa_reset_release` | 下游bus尚held立即EBUSY，不assert reset/污染flow；真board halt＋独立DL24 drain后撤configured、resolve pending、重新assert low至少1ms且DO/DI确认，再drop PA clock child；失败保留父资源，drop重试不重播高脉冲 |

clock的pads/PA两个子租约使用**同一全局单调leaf serial**，token不跨类型复用；任一held时
clock_release在restore/vote之前EBUSY。已pin物理rail/reset supplier的生命周期必须长于PA
child；成功清理顺序是mute/AFE停流/drain/producer halt→恢复I2S4 pads→I2C6 close/restore→
assert PA reset/drop child→clock restore→物理rail父资源退休，AP ticket最后释放。
新流需新的真实DSP generation，不允许用替换status/context消除reset/PMIF不确定性。

### Native I2C6：连接PA子租约与真实FIFO事务

**物理supplier与combined调用者仍未完成，不会自动注册backend、执行事务或发布Ready。**
实现包含在`x9_tfa_plj110.c`的生产include中，因此和诊断用同一个controller mutex，
但映射、clock image、journal及故障状态完全独立；不把诊断成功当write grant。
native child存续（包括失败）时`amp-probe`返回Busy且不消耗其一次性尝试；
诊断留有poison／未恢复journal时native acquire同样拒绝。

`x9_i2c6_policy.validate_controller`必须提供已pin且覆盖**整个生命周期**的实际
I2C6/APDMA clock、stable mux/PLL、当前GPIO114/115 mode1电气与两CSR bank/DMA所有权，
排除本地及远端master；一次gate/idle快照不是该supplier。PA rail/reset另由真实parent
独立证明，controller supplier还须实际证明APDMA无outstanding，不仅EN0。
回调有界且不得重入I2C6/PA/pads/AFE/clock生命周期。
现有PA handoff检查`mediatek,mt6991-i2c`、两reg tuple与slave/reset拓扑，
不额外冒充I2C pin/clock物理资格。loader仍关闭，没有本次live grant。

| 入口／动作 | 实际契约 |
|---|---|
| `x9_i2c6_acquire(policy,&lease)` | 同真实Online generation与fresh PA epoch，先hold PA bus child再map；输出token非零后即使失败也必须显式release，token本身不是成功配置资格 |
| `read16(ticket,lease,slave,reg,out)` | 只许34/35；FIFO reg1字节、LEN1/AUX2/TRANS2、RS+DIR_CHANGE，START bit14+bit0；仅完成ACK、START退出且FIFO精确2→1→0后返回BE16，失败不改out |
| `write16(ticket,lease,slave,reg,value)` | FIFO reg/high/low三字节、LEN3/AUX0/TRANS1，清RS/DIR_CHANGE/DMA/DMAACK/ASYNC/IRQ/LEN_CHANGE；完成ACK且FIFO0才成功，不验证PA寄存器最终值，上层仍须读回与可信白名单 |
| timing／clock | 不接管gate、PLL、pads或APDMA；用已pin真实parent名义PCW生成保守≤100kHz timing，非400kHz证书；fresh检查CKSTA/FENC与I2C_WEST UPDATE2 bit5、PERI_AXI UPDATE bit1 |
| journal／pending | 两bank逐字段fresh核对；普通RW只接最后一次写前prior或expected，不自动unwind。DATA/START/W1C/FIFO_CLR在动作前登记；未知status/timeout不清、不取消、不重发 |
| `release(ticket,lease)` | 撤configured并禁止后续事务，先真实halt/独立DL24 drain，再只收敛本owner的实际terminal请求，逆序恢复touched CSR，fresh saved/idle后drop PA child；drop失败重试只复验，不再restore |

START曾尝试但仅观察到START0/STAT0，不能证明未来没有响应，必须继续保留资源。
晚到的明确terminal仅可清该owner的ACK/NACK；W1C/FIFO_CLR/START-clear的失败只观察
实际完成效果，不重放动作。START前失败可在真halt/drain后弃自己的queued bytes，
不重发DATA。故障后明确已结束的NACK不阻止另一次安全mute事务，但不确定pending或
RW journal会阻止**所有**后续事务；release一旦开始，只允许cleanup。
如果PA自身PMIF出现pending，先在I2C mutex外执行本owner的
`pa_reset_resolve_pmif`（允许bus child保留），然后继续显式bus release。

`audioctl i2c6-status`使用追加selector18返回独立**128B v1缓存**，含generation、
held/configured/teardown、首错、dirty/uncertain/pending、名义时钟与最后CSR观察、
请求/完成/写尝试计数。仅复制内存，不validate、MMIO、清错或提供用户write/recovery入口。
旧PCM/status/amp ABI不变，HAL Ready门不变；可信full revision/container/profile、
probus保护校准和真实unmute仍须后续生产接线，不能设`x9_tfa_bus.verified=7`。

### 通过原生 I2C6 完成双侧安全静音

**安全静音代码已实现，但没有实际controller/双侧rail supplier或combined注册者，
因此本轮没有执行I2C/PA操作。** `x9_pa_native.inc`实际include进入
`x9_tfa_plj110.o`，与native I2C6共用**整个双侧事务**的controller mutex，不是
每次read/write之间放锁；诊断probe、其他FIFO事务或release不能夹入ID→RMW→读回。
未建第二controller实例、未把cached诊断flags装成`x9_tfa_bus.verified=7`。

| 入口／步骤 | 约束与结果 |
|---|---|
| `pa_native_mute(ticket,lease)` | 已存在且fresh实际native I2C6 child；该child本身pin真实PA reset/power/controller parents。初次还需true Online，同一singleton记录generation/epoch/首错 |
| 完整身份 | 原OPPO v6.14.2支持的full stepping：1a72/2a72/1b72/2b72/3b72、0a73/0b73/1a73、0a74/0b74/0c74；拒绝仅low-byte72/73/74、未知stepping及fallback。初次reg03每侧一次，任何身份读失败/变化撤销该侧写授权，不靠换status对象再probe |
| AMPE-off | 只读reg03与SYSCTRL0；三family AMPE field0x0030即bit3，I2CR0x0010即bit1自动清动作。发现I2CR1拒绝RMW；仅清AMPE、保留其他bits，绝不AMPE-on/DCA/rail/reset/key/init/profile/calibration写入 |
| 完整读回 | mute写前记录attempt/uncertain；ACK不当成功，重新full identity＋SYSCTRL全词wanted，再双侧复查才返回0。失败留下first fault，unknown mute不重发 |
| 双侧错误隔离 | 一侧明确NACK仍尝试安全隔离另一已知侧；native pending/uncertain状态阻止后续全部transfer，不循环重试timeout/FIFO/START |
| `pa_native_muted_validate` | 只读fresh身份/SYSCTRL/parent验证，不写PA值、不清历史fault/uncertain；能供以后pads/AFE安全mute接线，不把cached muted作为当前硬件证明 |
| 新代际 | 不同binding必须由native I2C6真实parents证明更大的READY generation和PA reset epoch；不能凭不同ticket/context抹掉同epoch失败 |
| `pa_native_status` | kernel-only64B缓存，含身份/静音/首错/不确定/attempts；没有新增用户控制selector、IOCTL或Ready位 |

静音入口不需要也不假造trusted container/per-revision profile policy；原OPPO optimal
init中的unlock、OVP bypass、boost、MTP/OTP及probus DSP保护路径均未搬入此安全入口。
历史controller/transport fault允许独立实际物理cleanup，但不授权新播放或消除首错。

### 接通 LEFT PA 的 SID5 LNC0_8 数字电源配置

**本阶段只有生产源码和直接单元编译，没有实际电源操作或新的镜像。RIGHT 电源、
长期排他 supplier 与完整播放 backend 仍未接齐，不能宣布双 PA 供电或 Ready。**
`x9_pa_vdd_left.{c,h}` 已加入 recipe／link-order，使用独立 rail mutex；既有
PA `validate_power` 可以调用 LEFT validate，但必须另行 pin／证明 RIGHT 和所需
模拟供电／boost，不能直接把该函数当双侧 supplier。

| 路径 | 实际代码与来源 |
|---|---|
| 当前 FDT | LEFT `tfa98xx@34/tfa98xx_vdd-supply` 唯一链接 SID5 `/regulators/mt6661-5-lnc0-8`；精确 regulator schema、500000..2090000 µV、200 µs ramp、allowed-modes0/2。追加 handoff flag8，必须同时有 reset flag4；旧256B ABI不变。optional capture复用既有128项存储，缺失不撤销 shared-layout grant |
| 1.8V 配置 | OPPO `tfa98xx.c` 的 set_voltage1800000／set_load200000／enable；`mt6661-regulator.c` 的 SN-LDO 编码给 `0x24b=0xd0`，不是普通低4bit selector |
| normal／enable | `0x2087` LP bit1清、`0x20af` HW_NORMAL_OP_EN bit1置、SW_EN bit0置；仅+1 SET／+2 CLR自身位，selector才整字节写；完整控制读回保护邻位 |
| actual DA | 原 `mt6661-regulator.h` 明确 `0x20a2` EN bit0／LP bit2；不采用 `.c` 模糊的0x3/0x7整mask。≥200 µs后有界等待实际DA，同时复查控制；这不是模拟电压测量 |
| cold admission | 真Online／同generation／同Speaker ticket，独立clock rail child在任何PMIF操作前pin父链；SW_EN0、DA EN/LP0、HW_OP0，SID3 GPIO10/11的MODE/DIR/SW和DO／物理DI均LOW；拒绝继承live rail，不借用未知电源 |
| 访问／故障 | 与原PA共用PMIF mutex；逐事务fresh clock／board／pmap／PMIF INIT和FSM资格。每字段last-prior／expected／dirty／uncertain，第三值、timeout保留child／首错／nonzero cleanup token；不自动unwind或重放acquire |
| cleanup | 真halt＋独立AFE no-outstanding supplier，clock锁内先证明PA／I2C6 child已退休（不反向拿PA mutex）；每次写前真实reset LOW。仅退休本owner late response，先撤SW_EN／HW1，再实际DA OFF，才恢复LP／VSEL；每个VSEL写再次证明OFF。最后fresh saved/reset／parent读回才drop rail child |

`validate_exclusive` 必须提供整个生命周期的SID5目标寄存器／foreign reset writer
排他与pending SWINF2 response归属保护，尤其不能让既有generic VLDCLR清掉本owner
晚到响应。现有PMIF每事务mutex、无writer源码命中、FDT或DA快照均不能替代；本阶段
没有默认成功supplier。GPS的同名LNC0_8是SID3，modem相关LNC0_8是SID6，不能据
寄存器名称合并电源网；RIGHT没有supply property也不授权dummy/shared-net推断。

iOS `audioctl left-vdd-status` 使用追加selector19返回64B v1缓存，包括generation、
held/configured/teardown/restored、主／restore errno、dirty/uncertain/pending、最后
VSEL／SW／DA／HW_OP、ticket／lease。仅复制内存，不validate／MMIO／恢复／写电源；
PCM、现有selector编号、HAL Ready门均不变，没有自动起DSP、安装HAL或解静音。

### AUDIO-12：启动／运行配置资格与缓存故障位置

start 在清零整个 DMA 环之前复查13个 journal 字段（含完整41-bit BASE/END）、
foreign CONN116/117 route words 与 DL24 action／domain／one-heart 位。
清零／prefill cache sync 回调后重查；跨环第二段 memcpy 前同样重查。
OUT4 EN之后、DL24 ON之前及之后复查对应 expected 配置，最终通过才发布
running／anchor并唤醒 watcher。运行期 copy／CUR poll 不只检查 CUR，还复查全配置。
这些检查是当前所有权观察，不是阻止 foreign XPU 并发写入的硬件锁。

全环清零和PCM复制在写 VA 前置 `copy_pending`，sync 或其后校验失败保留，
不能把可能可见的字节报告为“未消费”；DMA ON失败仍走安全停流。
disable 的 DL24失败后不继续写 OUT4；restore 先核对所有 expected 字段，
当时仅对 uncertain 项容许原 saved 值；AUDIO-14 已修正为最后一次写前的 immediate prior，
以覆盖 enable 未落地时仍停在 configured-disabled 的恢复缺口，不接纳第三值。

本节引入的 `audioctl afe-status`（selector7，无新增selector）为 **v3／152B**缓存，
当前已由 AUDIO-14 升为 **v4／184B**：
`lease-errno/config-errno`、首次 `guard` 阶段及 `reg/expected/actual`、累计校验次数。
字段差分均为对应 journal mask 下的值；foreign route／禁用位显示实际拒绝值。
`guard=none`时reg无诊断意义，`reg=0xffffffff` 的租约失败没有读取AFE寄存器。
历史首错保留至下一次合法prepare；只读查询不会调用validate或尝试恢复。
**平台仍无真实注册者、Ready0；此轮没有接通物理双PA、EMI/cache、DSP READY或原生HAL路由。**

AFE register 时才建立 boot-lifetime watcher；空闲时睡眠，运行时每2ms检查真实 CUR。
剩余已提交PCM低于或等于480帧（10ms），或 AP 观察/硬件进度超过剩余安全预算，
即停流，保留主错与 mute/disable/quiesce/restore 各阶段错误。**这是软件保护，不是
硬件不重放保证**：AP 调度超时可能晚于deadline，仍须真实硬件资源与物理静音验收。
watcher不会 unmute PA，也不会注册 speaker backend。
未注册、重复 prepare 等 admission 错误不再污染现有状态；实际部分写入只恢复 touched
字段，已恢复字段不重写。跨环 copy/cache sync 失败保留 `copy_pending`，不能报告
EAGAIN“没有消费”；失败 stop 的 lease/poison 保留，只有完整 quiesce/restore/release
后才能进行新的 prepare。

### AUDIO-14：恢复最后一次不确定写入，不重放已禁用的 CSR

prepare 的基线 `saved` 与 start 写 EN／ON 前的 configured-disabled 通常不同。
旧恢复只接受 expected 或 saved：若 enable 未落地而读回失败，紧邻此次写入前的
disabled 值可能被误判为 foreign conflict。本轮根据源码修复此分支，**未实机复现**。

每个 journal 字段单独保存 `prior`，仅在真正 MMIO 写入前捕获实际旧值；共享五字段
descriptor 不变。disable／restore 只接受当前 expected，或 uncertain 最后一次写入的
immediate prior；普通 prepare／start／copy／poll 不接受 prior。无关的原 saved 不是
第三个可接受值。恢复又发生写入时更新 prior，不累积更早的可接受历史。
最终恢复仍严格核对全部 saved 基线，含 untouched／already-restored 字段，诊断
expected 也采用该次实际比较目标。

若读到的值已经等于本次禁用／恢复目标，仅更新 expected 并清 uncertain，**不写 CSR**；
dirty 由 restore 外层在该字段收敛后清除。动作位拒绝／no-replay、foreign routes、
fresh lease、mute／quiesce 与失败保留 mapping／ticket 的要求全部不变。

`afe-status`当前是 **v6／192B**，private platform v6，无新增selector。首次 guard 若与
journal 有关，缓存 `journal-index/prior-valid/prior/saved`，值均在字段 mask 下；
prior-valid 只表示首次错误时该写 uncertain，不能当作租约／硬件动作重放资格。
租约、动作位或 foreign-route 首错不填 journal 诊断，index=0xffffffff、prior-valid=0。
`guard=none` 时这些字段无意义。新增两个饱和累计计数：

| 字段 | 含义 |
|---|---|
| `prior-observations` | cleanup 比较命中 immediate prior 的次数；同字段可多次观察，不是独立恢复次数 |
| `no-write-reconciliations` | uncertain 字段已处于目标、未写 CSR 即清不确定的次数 |

查询只读缓存，不调用 validate、MMIO 或恢复。**没有真实 AFE platform／双 PA write
provider／speaker backend 注册者，仍 Ready0；本轮没有启 DMA、解静音或物理播放证据。**

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

功放配置新增与离线工具共用的 **baseline-independent prefix exclusions**：在首次
硬件访问前检查两侧 device/profile 的全部 leading register/bitfield prefix，拒绝
chip-ID、status／sticky-fault／IRQ-clear、boost、key／MTP／校准区，以及已知的
power／AMPE／reset／protection动作字段。其余CSR仍必须由真实 full-revision
`allow_patch` 白名单授权；排除检查通过不是正向白名单，也不是完整初始化。
opaque descriptor 仍不执行；遇到它就停止 prefix，不能据此称后续 profile 已应用。

OPPO 72/73/74 字段表确认 `reg00 bit1 I2CR` 和 `reg50 bit13 rst_min_vbat`
为 auto-clear 动作。mute／prefix RMW 写前拒绝继承的非零动作位，显式写动作位0，
不把“mask没有包含动作位”当作安全；AMPE已0时不重复写mute。每项配置前核对
两侧实际 reg03 与 reg00，完整16-bit RMW读回也核对未修改字段。cleanup重新核对
之前已确认的revision，失败／变化即撤销该侧写授权；初次身份失败不会在cleanup
重试读ID并授权写入。主错、部分配置poison与mute错误独立保存，mute成功不抹掉
它们。mute写前记录uncertain；写入／完整读回失败后，final cleanup不重新读写该侧
以盲重放可能已落地的mute，保留首个mute错误和资源lease，等待真实reset。
新增私有 `mute_attempts/sysctrl/mute_uncertain` 仅为真实provider保存证据，不改变PCM／IOKit ABI。
这些重复检查不能代替不可变的reset／rail／bus lease，也不证明远端无竞争者。

### 读取已供电功放的身份

**仅在配套内核和工具已部署、`x9_audio=2` 时使用**；本轮未安装／上机，
没有自动开机探测。`amp-status` 是缓存读取，未探测时 flags=0，raw 字段0不代表实际硬件值。

```sh
audioctl amp-status
# 在设备root shell中，显式一次；只读PA reg03，不播放、不冷上电、不复位
audioctl amp-probe
audioctl amp-status
```

`amp-probe` 要求 SID3 GPIO10/11 已为数字 SW 控制、mode0/output/DO及DI**高**；
按OPPO active-low reset时序纠正旧低准入，探针本身不解除reset、不证明fresh reset。
左 LNC0_8 已为SW 1.8V/on/non-LP/no-HW-vote；保留完整 DA
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

### 生产 I2C6 的完整时序配置

生产 FIFO 与上述只读 probe 使用同一 controller mutex、不同 journal；没有实际
controller／双侧 power supplier 时仍不访问硬件。`x9_i2c6_timing.h` 是 kernel 与
own-code iOS 共用的纯配置算术，不是 controller lease，也不授权更改共享时钟或引脚。

- OPPO `mt6991_compat` 为 `speed_ver=1`，高／低相独立 sample1..8 × step1..64，
  divider 为两个相同5-bit字段，LS period需要额外一个count；HS/SDA/MIS=0、IO=3。
- WEST四个source为26M、UNIVPLL/20、MAINPLL/16、UNIVPLL/10。PCW与两个分频在
  **完整分式中一次向上取整**，不再先ceil PLL再floor fixed divider而低估频率。
- EXT使用 `ceil(source_hz*5/(divider*1000000))`，不再两次向下取整成不足5µs。
  solver在写第一个CSR之前，核对**实际编码**的period／各相和EXT：整体period及
  high/low各扣一个count后，tHIGH≥4µs、tLOW≥4.7µs、EXT≥5µs，并以有理不等式确认
  ≤98kHz；可分解成sample/step但不满足上述约束的候选继续搜索，不写部分配置。
- 配置写完完整fresh CSR／source读回后再核对；configured生命周期每次`n_guard`
  （包括DATA／START／完成处理）复查同一expected journal的时序约束，不建第二份快照。
  显式halted/drained cleanup先撤configured，允许恢复原始配置，不以新的播放时序要求
  阻断历史fault后的安全恢复。首错／pending／uncertain／no-replay规则不变。
- cached128B status布局与selector18不变；iOS `i2c6-status` 明确显示
  `source-bound`／`scl-bound`，不是测得频率或400kHz证书，不新增执行接口。

本地OPPO `cust_mt6993_{zhujiang,changjiang}_audio.dtsi` 均只在LEFT声明
SID5 LNC0_8 VDD及SID3 vs-voter；RIGHT没有独立supply/voter property。该source观察
没有解决实际RIGHT电源网或长期互斥，也不能把两份相似DTS当当前运行绑定或共网证明。

`audioctl` 还可仅离线检查本地 container（不需要 X9Audio 服务、不加载到功放）：

```sh
audioctl amp-inspect /var/tmp/tfa98xx.cnt '<exact-profile-name>'
```

parser 报告两个 PA 的 profile/prefix/opaque 数目，并执行同一 prefix 排除检查；
拒绝时显示PA、device/profile、零起始descriptor index、reg/mask/value和errno并退出1。
检查通过仅表示 leading prefix 未命中这些已知禁写项，**不授权kernel写入**。
CRC通过和 prefix写完均不表示
完整初始化／校准成功。不会执行未知 opaque payload、猜默认profile、写MTP/OTP，
没有 unmute API；真实 profile／芯片证据未齐时，不允许进入 speaker Ready。

### 读取双 PA 的 factory Re25 shadow 与离线回复

**尚无完整校准／保护 supplier 或 combined caller；以下代码不授予 Ready，不触发
校准，也不自动发送参数。** OPPO v6.14.2 的 TFA9872/73/74 字段表与
`tfa_dev_mtp_get` 对应 f0 的 OTC/EX 两个位、f5 的独立16-bit R25C；不是
family1 的0x83或TFA9888的左右双字段。

`x9_pa_native_re25_read(ticket, i2c6_lease, &snapshot)` 为 kernel-only 接点，
使用真实 native I2C6 mutex／transfer／parent，不创建第二条总线或新的用户 selector：

- 已有同ticket、实际reset epoch／READY generation、双侧full revision和AMPE-off
  资格后，fresh读两侧ID/SYSCTRL；first f0之前锁存每epoch仅一次attempt。
- 两侧各读f0→f5，要求OTC+EX、独立非零／非ffff值；逆序第二快照f5→f0，要求
  两个完整word稳定，再fresh核对双侧ID/SYSCTRL、Online和controller idle。
- 全部成功才发布48B snapshot。错误保留首错与相关side；caller的kernel输出须
  全程可写／pin，与native状态和MMIO虚拟／物理字节不重叠，失败保持输出不变。
- 不读status/IRQ、不写key/OTP/trigger、不poll MTP、不复制单声道或用7399补值。
  稳定shadow**不是MTP engine无在途操作、真实板级范围、DSP通道、calibration-done
  或完整保护profile证明**；这些仍须真实supplier独立证明后才能执行参数。

kernel 与 own-code iOS 共用 `x9_tfa_re25.{c,h}`。encode接受两个独立mOhm值和
明确范围，使用64-bit `mOhm*65536/1000`，生成family2 selector/81/05加两个Q16；
显式command为BE24 9B／LE32 12B，canonical batch为18B／24B。不猜cold/warm、
instance或LEFT/RIGHT→word顺序，encoder本身不发送。decode只接受两个raw reply
word（BE24 6B／LE32 8B），先检查canonical signed24，再拒绝负／零／越界值；
`q16*1000/65536`同样使用64-bit，避免vendor unsigned32中间乘法溢出。
alias／长度／capacity失败均不改output/size。

实际 `audioctl` 有 `amp-re25-inspect` 离线入口，在任何IOKit连接之前返回。
语法：`audioctl amp-re25-inspect <local.bin> <be24|le32> <word0-min> <word0-max>
<word1-min> <word1-max>`。四个范围须严格decimal、1..65534，各自min≤max，
无默认范围；最多读9B来拒绝尾随内容。仅打印Q16与floor-mOhm：在量化Q16域
比较边界，floor显示可能比原始下界低1mOhm。文件与本地范围均不可信，word0/1
不代表已证明LEFT/RIGHT；工具不生成／发送SetRe25、不连接服务或访问硬件。
`audio/build.py` 的实际audioctl源码列表和kernel recipe/link-order均已加入codec，
但单位编译通过不等于HAL bundle/signing/router或实体扬声器验收。

### 将双侧 factory 值送入 DSP 并配对读取

**生产事务代码已接真实 IPI 入口及原生 AP program 排他租约，但 mandatory
factory/profile/MTP 和其他控制路径／DSP／firmware writer supplier 尚未实现／注册，
未执行参数发送，不能授予扬声器 Ready。**
`x9_pa_native_re25_program(policy, &result)` 不是用户 selector，也不把离线数值
或 codec 成功当作校准授权。

| 顺序 | 实际调用与约束 |
|---|---|
| 准入 | private policy88B：同ticket／I2C6 lease／实际READY generation/reset epoch，明确wire packing、Set/Get selector、read command/batch形态及互异的side→word排列；独立board/profile bounds，无默认值。mandatory validate证明MTP engine／完整PA保护与foreign writer lifetime；真实`x9_ipi_dma_program_hold`在registered／Online／table／read_idx==write_idx后mint单调非零AP租约，之后再次fresh准入，再读factory |
| factory | 同I2C6 mutex内调用原生双shadow读取；每实际epoch只允许一次program attempt，standalone读取后不能再program。fresh双revision／SYSCTRL与controller/reset/power资格贯穿后续阶段 |
| SET | 按明确side→word排列生成canonical SetRe25C batch，进入真实`x9_ipi_dma_set_on_program`，要求matching实际AP租约并保留原trusted parameters／ACK及actual read_idx消费校验；host SetConsumed阶段不代替DSP实际接受或校准完成 |
| command→GET | 共享GetRe25C codec只生成selector/81/85：显式single-word 3/4B或canonical batch12/16B，不猜cold/warm／selector4或改instance。真实`x9_ipi_dma_exchange_on_program`验证同租约，串行read-command SET→actual consume→GET→真实完成supplier；只接exact两个raw reply word |
| 比较与发布 | 独立bounds／canonical positive signed24 decode后逐word比较requested/observed **Q16**，不是floor-mOhm或容忍范围。mandatory Complete callback仍独立证明回复身份和实际所需校准／保护状态；最终fresh parent/output资格后才发布96B观察result |
| 错误 | factory/SET/GET之后任何失败保留native首错／attempt及parents；不自动重试参数、撤回索引、释放、reset或unmute。按既有mute→AFE disable/drain→真实halt→IPI/I2C/PA等显式release退休 |

整段持有**I2C6 mutex**，排除自身controller释放／PA操作，并pin reset/power parents。
SET与exchange各自使用现有IPI mutex，二者之间的全SmartPA写者排他必须由真实
AP program租约与foreign supplier共同证明，不能仅凭“I2C锁已持有”证明。
租约存于IPI实际singleton，不是caller布尔值：普通SET/GET/exchange在held期间
返回EBUSY，不touch mailbox/staging或清其他pending；on_program错租约拒绝，
原physical guard／parameter／reply／exchange supplier仍独立执行。租约在**成功
和错误后均保留**，没有early cancel/end；仅actual同generation Offline/halt、
memory authority、unmap和board child drop成功才清。serial不重置／不wrap，
不能用旧epoch token或不同context绕过。scalar token输出也禁止kernel singleton
和全部IPI_A物理别名，失败不改它；无新用户selector或Ready ABI。

中央 `x9_adsp_request`/`request_guarded` 在wake/MMIO前接namespace gate：region
INIT `0xadd0`／UPDATE `0xadd1`、AURISYS/SmartPA SET `0x204`／GET `0x205`
仅接受native core-A的精确私有guard function/context。此前plain API虽拒绝DMA，
却不排除PAYLOAD GET；现在plain或另一个caller的guard不能绕过原生IPI owner。
gate只比较immutable identity，不拿IPI lock、不反向callback；原guard继续证明
当前generation/route/table/indices。PCM `0x201..0x203` 与其他非reserved消息不变。
这是本原生region-index AP入口的实际排他，不证明其他AP控制路径、DSP内部或
外部firmware不会改变SmartPA；它们仍是mandatory全writer supplier的剩余范围。

callback有界、不可变、不反向重入
I2C/PA/AFE/clock/transport/status/IPI，不保留私有stack policy/snapshot/reply地址。
observed result不是Ready／unmute token；既有IPI五个supplier仍独立mandatory。

不能用统一“MTPB只读”替代上述真实MTP engine证明：OPPO fieldnames的TFA9873
MTPB=0x1070，所在0x10还含clear-on-read OCP/OTP/UVP/clock位；TFA9874
MTPB=0x11c0，所在0x11也有sticky clear-on-read DCDCA。vendor `get_mtpb`
最终读取整寄存器，不是只读一个无副作用bit。此事务**不新增status/IRQ/MTP busy
读取或GetStatusChange**；不能为了资格检查擦掉故障证据，也不从缺callback返回0。

### 解析真实 TFA 参数批消息

`tfa98xx_{device,music,ringtone,voice}.bin` 是另一种输入，**不能交给 PM `.cnt`
parser，也不能作为校准、完整固件或解静音授权**。本地提取材料的 device 为612B／10条，
music、ringtone、voice各1512B／5条；只确认各自framing，未据相同大小推断系数相同。

```sh
audioctl amp-msg-inspect /var/tmp/tfa98xx_device.bin be24
```

此入口在连接IOKit前返回，只读文件与内存，不需要X9Audio服务，不写功放／mailbox。
packing必须明确为 `be24` 或 `le32`；输出byte offset和opaque command，不推断用途。
不能把本地文件名或多芯片SmartPa XML中的库表当作当前HAL选库证明。

共享codec依据OPPO `tfa_tib_dsp_msgmulti` 与 `tfa_dsp_msg`：

| 检查 | 行为 |
|---|---|
| Framing | 精确`0x008015`头、unsigned16 word-count、至少一条消息、末尾zero count恰好EOF；不接受截断、padding或缺terminal |
| Bounds | 24BE上限4000B，32LE上限5336B；输入整word、长度乘法先验有界，转换时也检查目标上限 |
| Signed words | 每3B BE数据word转换为4B LE，bit23扩展至最高字节；反向拒绝非canonical高字节，不静默截断 |
| 发布 | `scan/get/convert`全包验证后才修改输出；metadata/目的buffer/返回长度禁止与输入或彼此alias，容量不足不产生部分转换 |
| 错误 | 参数／alias为EINVAL，格式为EBADMSG，源／目标超限为E2BIG，容量不足为ENOSPC，消息index越界为ENOENT；输出不变 |

调用者须在整个调用期间保留immutable且已pin输入。codec不分析或执行任何消息语义，
不添加外层批消息、不把SetRe25 flush规则当成已获得真实校准，不注册backend／Ready。
generic register-prefix bus准入现在与native PA共用完整stepping表；低字节family匹配
不再允许未知高字节revision进入读写路径。完整型号匹配仍不替代实际reg03、reset epoch、
双侧rails／I2C6长期租约或revision-specific register policy。

### 将参数发布到真实 IPI DMA region

`x9_ipi_dma` 已接实际native board／transport调用，不是模拟队列。**当前没有安装
memory／parameter／reply／readback／exchange supplier，所以没有执行map、注册、SET或GET，
更没有校准／解静音。**
它仅处理core-A的IPI_A carveout；AUDIO_COMMON PCM、IPI_B和AFE输出均不是此region。

| 顺序／入口 | 实现与边界 |
|---|---|
| `x9_ipi_dma_acquire(policy)` | kernel-only policy **v3／80B**，五个真实supplier都mandatory，旧policy拒绝；同Speaker ticket／fresh READY generation，验证当前firmware task count、controller／CALL_FINAL14或AUDPLAYBACK32、EMI／pool排他，再pin独立board IPI child。只map16K-aligned table/TX＋独立16KiB GET页为Normal-WB（prefix最多0x18000B），拒绝WC/Device alias，不清整个1.5MiB pool |
| Table | header128B；每task有AP→DSP和DSP→AP两个384B region，read_idx在+128、write_idx在+256。`checksum=128+task_count*2*384`只是字段offset，不是CRC／校准证明；pool_offset按其字段地址向上128B对齐，不能按sizeof误算 |
| `x9_ipi_dma_register(ticket,generation)` | 一次DIRECT DMA_INIT `0xadd0` 携带LE64 bus/bytes，再NEED_ACK UPDATE `0xadd1` 携带task_scene。DIRECT消费不等于初始化被接受；UPDATE回复必须由实际firmware语义supplier验证 |
| `x9_ipi_dma_set(...)` | 整包解析／转换为私有wire副本，再由真实参数supplier确认对应profile／silicon／保护状态；只写AP TX ring并补齐128B padding，不把多个消息拆成普通payload |
| 发布与消费 | 数据先CVAC到PoC，再发布AP write_idx；发送72B inner DMA SET `0x204`（rw_idx是发布前index，data_size是真实字节数，type3），guard贯穿TX／ACK。ACK语义检查后还须IVAC独立read_idx cache line、等实际DSP消费到expected end；不清DSP拥有的index |
| `x9_ipi_dma_get(..., output, capacity, &data_size)` | 只接受已pin的kernel可写目的页、1..16384B capacity，拒绝输出／长度／singleton别名及任何IPI_A物理别名。真实readback supplier先证明前序read-command／PA保护状态／排他路由，且transport guard每次重新验证；GET自身不携带selector |
| GET写回与交付 | 专用WB页位于TX ring之后，整页先zero＋CVAC，然后NEED_ACK **PAYLOAD16** GET `0x205`、param2=type3：LE32 memory_size／LE32 data_size=0／LE64 WB bus。不是DMA union、不动TX索引。reply必须是16B payload、echo原memory_size／bus且data_size<=capacity；真实reply supplier还须证明写入已完成／PoC可见／无保留writer，匹配ACK或回显本身不够。IVAC整独立页→私有复制→fresh authority／目的页重新核对，最后才发布output/size；失败二者不变 |
| `x9_ipi_dma_exchange(...)` | 同一mutex内完成可信读命令SET→实际read_idx消费→GET→实际写完→exact长度／canonical signed24检查→最终output/size发布；请求已是session wire packing，仅显式WarmWord一个命令word（BE24 3B／LE32 4B）或ColdBatch一条one-word命令（12B／16B）。响应必须正数、whole wire words、<=对应4000B／5336B codec上限；不会把raw reply交给batch parser |
| 失败与release | 单请求、不pipelining。非法index不modulo修复；任何已发布后的EAGAIN／timeout／未知ACK保留首错、映射和parent pin，不能撤回write_idx／重新发送／free。外层先mute、AFE disable/drain并实际halt，release重新验证同generation Offline＋物理halt＋独立memory authority后才unmap/drop |

Plain mailbox API仍不能发DMA；byte codec成功、DIRECT消费、UPDATE／SET ACK和read_idx
推进都不证明speaker Ready或物理出声。legacy reserve `dma_addr`严格绑定本组件的
boot-lifetime私有staging token；不是bus地址，不接受user pointer或HAL/writeback地址形态。
GET的共享16B `aud_data_t` byte codec同时供kernel与own-code iOS编译，decode不解引用
回复地址，零size可表示但不是校准成功。GET任何发送歧义（包括pre-doorbell EAGAIN）都
保留pending／WB页／parent pin到真正halt；不会像vendor错误路径无条件free，成功也只有
实际完成supplier证明无late writer之后才能复用。没有新增raw IOKit selector或HAL Ready。
五个supplier均不得反向重入IPI DMA；memory／readback／exchange callback运行于transport mutex内，
禁止调用transport/status或任何会反向获取本组件锁的路径。

配对事务在第一条SET前检查全部input／output／size的kernel页绑定、虚拟及物理字节
别名，包括singleton与整个IPI_A；输入由caller全程pin／immutable，内部保留独立16B
私有命令。`validate_exchange`必须证明**当前**cold/warm模式、instance、readonly语义、
完整PA保护状态和所有SmartPA writer的整对排他；Prepared／SetPending／Consumed／
GetPending／Complete只是host journal，不替代firmware完成证据。即使SET已经消费，
之后GET前置检查失败也保留pair／命令／staging token／physical pin，普通SET／GET不能
绕开；只有真实halt＋unmap/drop才清理。安全cleanup不调用online-only exchange准入。
独立SET有pending时同样不再清零legacy staging token。

`x9_tfa_words_scan/convert`提供kernel与own-code iOS共享的独立BE24／LE32 word codec，
完整扫描、canonical sign extension和alias检查后才写输出；不是PCM或multimessage。
本接口不自动包装、不改instance／`spkr_select`、不猜cold/warm、不注册新的raw IOKit
selector或Ready。OPPO的私有copy后`buffer[0] |= spkr_select`可能发生在packing转换后；
不能盲改batch header或命令位置。当前仅支持canonical cold framing，非canonical双实例
header仍未取得真实firmware接线证明；五个supplier／combined backend仍未注册。

当前shim按BOOT-AUDIO-REMOVE继续移除旧音频walker，chosen音频placeholder为零。
`xnu_enter`只按XNU ABI传Apple boot_args（x1..x3清零），loader的deviceTreeP指向Apple
ADT；入口暂存的LK FDT寄存器不是已发布／有生命周期的元数据。尚未有post-XNU当前
FDT／合法长期EMI来源，不恢复旧walker、不盲扫RAM或把captured地址当grant。

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
