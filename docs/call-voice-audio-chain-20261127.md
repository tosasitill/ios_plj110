# 通话语音音频链路：现状闭合与第一片实现（2026-11-27）

信令与音频是两条独立的链：`X9Calls.inc` 的 VOICE20/CallList46 只证明拨号状态机，
不产生任何声音。本文件把仓库内全部语音音频证据按层闭合，标注每环节的状态与
所有权，并记录本轮落地的第一片（SMEM USIP 描述子观察）。主要证据来源：
`docs/call-ios-bridge.md`「通话专用 voice 音频契约」节及其归档
（主 checkout `x9/out/call-voice-md-handshake/`、`x9/out/call-audio-analysis/`、
`x9/out/call-voice-dsp-owner/`，均为 gitignored 原件静态材料）、
`x9/out/android-calls-20261115-183941/` 原厂捕获、`x9/baseband/README.md`、
`x9/audio/README-speaker.md`。

## 完整链路（自上而下）

### L1 iOS 侧：VirtualAudio「Baseband Voice」设备（原件静态已闭合，无生产接入）
- 原厂 VirtualAudio factory 比较 UID `Baseband Voice`，匹配后创建 592B 独立设备，
  含 `Baseband Downlink`/`Baseband Uplink` 两条 stream；`ttye` 属性接 CTSetTTYMode，
  `ctsr` 接 CTAudioSampleRateSwitchDidComplete。stream 不是写 UID 凭空生成：
  factory 以真实输入 AudioObjectID 经 `stm#` 的 inpt/outp scope 查得真实 stream ID
  再构造 owned wrapper；IOProc 的完整 caller 与真实硬件 provider 未闭合。
- 状态：**完全没有**。本树没有任何向该 factory 提供 Baseband Voice 输入 ID、
  stream 或 IOProc 的生产代码；也不能靠广告 UID 或复用媒体 Speaker 设备冒充。

### L2 AP↔DSP：IPI DMA / REG_DMA / AD20（布局静态闭合，运行时缺同版固件）
- 同机 HAL `ScpSpeechOn` 注册 scene0 callback；`audio_ipi_dma_cbk_register`
  以 ioctl 40046915 注册（20B payload：magic/~magic、scene/reg、双向尺寸），
  ioctl 40046914 取 kernel task-info（每项 8B），AD20 以 ioctl 40046900 下发。
- REG_DMA 布局已闭合：header 128B，AP_TO_DSP / DSP_TO_AP 两 entry 在 +128/+512，
  每 entry 384B（offset+0、size+4、read_idx+128、write_idx+256）；两条 65536B
  region **没有被证明是 MD voice PCM 的 UL/DL 环**，禁止据此补造 PCM 地址/节拍。
- scene0 的物理 core 由 runtime task-info 决定（参考实现 core_total>1 时选
  HiFi3_B），不能用静态常量或 Aurisys core_type 猜。
- 状态：**完全没有**（生产侧）。另外固定输入缺同版 `audio_dsp` 固件 payload
  （OTA 分区列表有此名，现有 .local 分区备份/modem 原件/vendor 文件中没有），
  DSP consumer 无从谈起。媒体侧 ADSP handoff 曾因启动回归被回退
  （`x9/audio/README-speaker.md`：MIC DMA 与 ADSP 交接已从编译列表移除），
  与本链无关且不构成通话音频基础。

### L3 AP↔MD speech 控制：SpeechOn / sph_info（控制响应已闭合，通道与字段未闭合）
- 原 HAL `SpeechOnByApplication` 发送 MD `2f20`/sub25/128B `sph_info`，MD 校验
  保存后经 Spc_SpeechOn → SP_SpeechOn，原厂拨出有 af20 result 0——**控制响应**
  已闭合，但不证明 PCM 环或双向声音。
- `sph_info` 由 `configSpeechInfo` 清零 128B 后从动态 config/hwif 填 +3/+55；
  config/hwif 来自 USIP ioctl（c00844f1/c00844f3 读 phone-call config/hwif，
  本机已抓 config3/hwif0）。iOS 侧没有这组值的 owner。
- 通道不经 MIPC：原厂 MO 捕获的 35 帧 MIPC（`mo-frames-private.json`）全部是
  09xx/49xx 信令，无任何 speech 帧。本桥（MIPC over DPMAIF）发不出 2f20，
  其实际承载（USIP/ADI 侧通道）不在本桥所有权内。
- 状态：**完全没有**，且在字段语义闭合前不应发送。

### L4 MD↔DSP：3D00（全缺）
- 3D00 是 MD→DSP 方向，与 AP↔DSP 的 scene0 callback（只分 DA00/DA10/DA11/DA12）
  不是同一 transport。其 producer/payload、同版 DSP consumer/ACK、MD voice DMA
  均无证据；MD speech/SPC 已查窄区间未见 3D00/D300 immediate，两条 legacy
  CCCI producer 链也不是它。状态：**完全没有，禁止虚构帧形**。

### L5 AP↔MD speech EMI：USIP AD50 与 SMEM id 24（描述子在树内，voice owner 缺）
- `mtk-usip.c` open 发送 AP AD50 / IPI_MSG_A2D_GET_EMI_ADDRESS（header 16B，
  payload 两 u64 `{USIP_AP_PA+0x30000, 0x30000}`，BYPASS_ACK，send 成功不等于
  DSP 回执）；`SpeechExtMemUSIP` 以 ioctl c00844f0 读 size 并映射 AP parameter
  region，参数写在 map+0x4000 的区间并发布 `vendor.audiohal.speech.shm_usip`。
- LK 描述子 id 24 即该 EMI 区域：本机 Android LK 日志为 AP=f0d00000、
  MD=42d00000（MD view）、size=0x60000、flags=2。本树 `x9_modem.inc` 的
  `x9md_smem_init()` 已从 LK 表把 id 24 装配进 `x9md_regs[24]`，且两处清除
  逻辑都明确跳过 id 24（SMF_NCLR_FIRST；cacheable 清除循环注释
  「CONSYS id22, USIP id24」）——数据与保护都在，缺的是 voice 侧的
  访问/lifetime owner 与任何消费者。
- 状态：**接一半**（描述子装配与不清零已落地；映射、pin、参数区、PCM 环未接）。

### L6 AFE / DSP trigger（reference 锚定，非同版证明）
- 原 HAL 以 `adsp_calldl_sharemem_scenario`/`adsp_callul_sharemem_scenario`
  两个 control 决定 CALLDL/CALLUL 是否走 ADSP share mem；reference 实现
  （MT6993 kernel 源码）表明 HWPARAMS/PREPARE 传的是物理 descriptor 地址而非
  PCM 样本，且 `use_adsp_share_mem` 时主机 memif/IRQ 启停被跳过——trigger
  决定门在 DSP。本次原厂 MO 实际走 `modemPcmOpen` + 两次 Hostless_Speech
  open（2ch/48000/1024×2），frame-base 分支还会绕过 Hostless。
- 状态：**完全没有**（本树无 AFE/DSP 通话 provider），且不能用 host-only AFE
  播放/录音或 SmartPA owner 冒充。

## 本轮第一片：SMEM id 24 描述子观察（已落地）
选 L5 的装配出口做观察，理由：数据已在树内、纯读、零行为变化，是后续任何
EMI 工作的前置核对（原交接要求「先按锁内 crs/regs 配对读真实 descriptor，
再谈独立访问/lifetime 资格」）。
- `x9/kernel/src/bsd/dev/x9_voice_smem.h`：纯函数 `x9_voice_smem_evaluate()`，
  输入运行时描述子（md_view/size/flags/ap_off/in_lk/exists/nclr_first），
  输出 present 与 android_match。Android 值只是比对常量（来自上面那一行
  LK 日志），不是运行时默认；不一致是发现，不是错误。
- `x9_modem.inc`：`x9md_smem_init()` 成功收尾处加门控日志
  （`x9_voice_diag=1`，默认关）：打印 `X9 VOICE: smem id24 ...` 一行。
  不映射、不清零、不授予，不改变任何现有行为；门控关时该代码不执行。
- 主机测试 `x9/baseband/test_voice_smem.py`：覆盖 Android 描述子命中、
  缺失、零尺寸、尺寸/flags 不符、无 LK 背书五种情形。

## 剩余环节与各自缺的证据
1. **id 24 独立访问/lifetime**：先在设备上用本片核对本 boot 的 LK 描述子与
   Android 日志一致；再参照 CCB region 的既有处理设计 map/pin 与退休规则。
2. **sph_info +3/+55 语义与本机值**：需带 USIP ioctl 日志的 Android 抓包，
   或对 configSpeechInfo 的 config/hwif 来源做完静态闭合。
3. **AD50 的 DSP consumer**：缺同版 `audio_dsp` 固件 payload 本身。
4. **MD 3D00 producer/consumer**：需 MD/DSP 固件更宽区间的反汇编或 MD 内部日志。
5. **UL/DL PCM 环布局、codec、节拍**：无任何已证布局；REG_DMA 64K region 与
   Hostless 参数都不能替代 MD voice 环证据。
6. **iOS Baseband Voice 的 kernel 侧 provider 合同**：VirtualAudio factory 向
   baseband 驱动索取输入 ID/stream/IOProc 的调用面，需对 X9Baseband 接口与
   原件调用链做专项静态分析后才能设计。
7. **runtime task-info[0] 的 8B 回读**：决定 scene0 实际 core；需 ADSP 在线实测，
   且以同版固件到位为前提（见 3）。

## 第二轮：L5 USIP EMI owner（2026-11-27 晚，已落地）

按机主要求以原厂安卓实现为规格，把 L5 从"描述子观察"推进到完整
lifetime owner。Android 侧规格全部来自本机原厂模块源码
（`oppo-modules-dt-mt6993` 的 `sound/soc/mediatek/common/mtk-usip.c`）：

- **所有权模型**：Android 的 owner 是内核 mtk-usip 驱动——
  `usip_get_addr()` 从 CCCI `get_smem_phy_start_addr(0, SMEM_USER_RAW_USIP)`
  取得区域（即 LK cacheable 表 id 24），记录 addr_phy/memory_size/
  memory_ready；`/dev/usip` 的 mmap 把整区以 noncached 重映射给
  audio HAL，HAL 的 SpeechExtMemUSIP 在 map+0x4000 写 speech 参数。
  本树的 owner 同构：描述子只从本 boot 的 LK 表配对读出
  （`x9md_crs` 的 AP pa + `x9md_regs[24]` 的 MD view），不硬编码
  f0d00000；绑定要求区域覆盖完整 EMI 表跨度（0x60000），不足直接拒绝。
- **EMI 表**（mtk-usip.c `EMI_TABLE`）：AP_USIP_PARAMETER 0x0/0x30000、
  ADSP_USIP_PHONECALL 0x30000/0x8000、ADSP_USIP_SMARTPA 0x38000/0x28000，
  恰好平铺 0x60000。
- **lifetime**：纯头文件 `x9/kernel/src/bsd/dev/x9_voice_usip.h` 跑
  bind → acquire（首 pin 要求 glue 映射）→ map_done（失败回滚 pin）→
  release（末 pin 要求 unmap）→ unmap_done 的状态机；内核 glue
  （`x9_modem.inc`）用 `ml_io_map_wcomb`/`ml_io_unmap` 兑现，与其它
  cacheable SMEM 区域同法。区域永不清零（SMF_NCLR_FIRST）、永不被
  本 owner 读写——pin 只确立"地址可发布"的资格。
- **AD50**：`x9_voice_usip_ad50_build()` 按 `audio_messenger_ipi.h`
  的 ipi_msg_t 头 + mtk-usip 的 payload 规则构造整帧（scene 0 与
  scene 19 两份，字节与契约 JSON 的参考推导逐字一致，主机测试锁定）。
  只有 qualified（bound+mapped+pinned）时才允许构造。**构造不等于
  发送**：本树没有 voice IPI transport owner（`x9_ipi_dma` 是
  SmartPA 专用），帧只在 `x9_voice_usip=1` 门控下构造并打印
  "constructed (not submitted)"，不投递 DSP。
- **门控**：`x9_voice_usip=1` 默认关；关时 owner 连 bind 都不做，
  行为与第一片之前完全一致。主机测试 `x9/baseband/test_voice_usip.py`
  覆盖 EMI 平铺、bind 拒绝项、map 失败回滚、双 scene 帧字节、嵌套
  pin 与末 pin unmap 全流程。

仍缺：真实消费者（call owner）出现后 acquire/release 才有调用方；
AD50 的提交等 voice IPI transport 与 scene 路由资格（见 L2/L6）。

## 第二轮：L3 sph_info 动态字段闭合（2026-11-27 晚，已落地）

`docs/call-ios-bridge.md` 与第一轮文档里悬着的 +3/+55 来源，本轮用
原厂 HAL 反汇编（`x9/out/call-md-voice-owner/hal-config-speech-info.asm`）
+ mtk-usip.c 源码 + 本机 live.dts 三方对齐，已完全闭合：

- **取值链**：`configSpeechInfo` 在 +0x03 存
  `SpeechDriverOpenDSP::getAdspPhoneCallEnh()`、在 +0x55 存
  `getAdspPhoneCallHwIf()`；两个成员由 OpenDSP 构造器经 /dev/usip
  ioctl `c00844f1`/`c00844f3` 读入（实际拷贝是 u32，不是 ioctl 编码
  的 8 字节）；mtk-usip 驱动的值来自其 probe 读取的
  `speech-usip-mem` DT 节点属性 `adsp-phone-call-enh-enable` /
  `adsp-phone-call-hwif`。本机 live.dts：**3 / 0**，与原厂 MO 日志
  `configSpeechInfo(), ... opendsp: 3 ...` 一致。
- **门控规则**：ADSP option 关闭且 SCP support feature bit 为 0 时，
  HAL 不调用 getter，两个字段都存 0（汇编 1a6910/1a6938 的回退路径）。
- **其余已钉字段**：+0x00 app（MO 0）、+0x01 bt（getBtInfo，MO 0）、
  +0x02 rate enum（MO 3）；+0x05 与 +0x54 同属一个 HAL 分支
  （+0x05=2、+0x54=1+isDualSmartPA，MO 的 smartPA config=2 证明该
  分支被走）；+0x1c 起 24B common 块（MO com par[0]=6）、+0x34 起
  32B debug 块（MO debug info[0]=3）。
- **MD 消费**（SPC handler 反汇编）：body+3 被保存；body+0x55 存入
  state+0x55 并分支——hwif≠0 时 state+0x54=2、+0x56=((body+3>>3)&3)+1；
  hwif=0 时 state+0x54=(body+3<<1)&2、+0x56=0。本机值（3,0）推出
  state = {2,0,0}。头部 `x9_voice_sph.h` 内含该镜像函数，主机测试
  双分支锁定。
- **合成物**：`x9_voice_sph.h` 的 builder 按 HAL 原样合成 128B body
  （先清零再存已钉字段），输入集 `X9_VOICE_SPH_PLJ110_MO` 逐值来自
  mo-call-audio.txt:790 的日志行，是观测输入而非默认常量；
  wrapper（sph_msg 32B）只填已钉字段（+0=1、+2 msgid、+0x0c type、
  +0x0e size、+0x10 body 指针）。
- **仍未钉**：日志里的 `enh_dynamic_ctrl: 0x3` 偏移未定（可能是
  +0x03 的第二名字），builder 不放；+0x04..+0x1b 簇（path/param/
  ext_dev/loopback/延迟/mute/usip param）各字段偏移未全钉，MO 中
  全为 0，builder 保持 0 与 MO 一致。
- **不发送**：0x2f20 的承载是 SpeechMessageQueue/SPC 流，不在本桥
  所有权内（原厂 MO 的 35 帧 MIPC 无 speech 帧）。builder 只把字节
  合同变成可执行、可测的代码，等 transport owner 出现再接。

## 第二轮：L6/L1 Baseband Voice provider 合同骨架（2026-11-27 晚，已落地）

- **原厂 PCM 合同（本机实测）**：原厂 MO 走 `modemPcmOpen` + 两次
  `Hostless_Speech` open（DL owner flags 0x10000000、UL owner flags 0、
  prepare=false），`mMdConfig = {channels 2, rate 48000,
  period_size 1024, period_count 2, format 0(S16_LE)}`，
  `SetModemSideSamplingRate 48000 => 48000`，SpeechOn 的 modem
  index 为 0（mo-call-audio.txt:625-640,633）。周期几何：1024 帧 ×
  2ch × 2B = 4096 B/周期、8192 B/缓冲。frame-base 分支会绕过
  Hostless，本机本次未走该分支。
- **AFE trigger 的决定门在 DSP**：reference 实现中
  `use_adsp_share_mem` 为真时主机跳过 memif/IRQ 启停，HWPARAMS/
  PREPARE 传的是物理 descriptor 地址而非 PCM 样本——主机侧 provider
  无权冒充 trigger，这条边界写进合同头注释。
- **iOS 侧合同（20H392 原件静态）**：VirtualAudio factory 按 UID
  `Baseband Voice` 匹配后创建 592B wrapper（Downlink/Uplink 两条
  stream），但 wrapper 不是凭 UID 生成：base ctor 把**真实传入的
  AudioObjectID** 存于 device+0xb8，slot88 经 `stm#` 的 inpt/outp
  scope 查真实 stream ID 再构造 owned wrapper；wrapper 还消费
  `ttye`（→CTSetTTYMode）与 `ctsr`（→CTAudioSampleRateSwitchDidComplete）。
  未闭合：谁把真实 object ID 交给 factory（ingress）、以及从
  Baseband Voice 入口到真实硬件 IOProc 的完整 caller 链。
- **骨架**：`x9/kernel/src/bsd/dev/x9_voice_bbdev.h` 把合同变成可
  执行规则——资格函数要求：双向真实 stream、真实 IOProc、非媒体
  owner、格式恰为原厂 modem PCM 合同；单例 registry 只接受合格
  provider，ingress 决策在 registry 为空时恒关。本树目前**没有任何
  provider 能合格**（无 PCM supplier），所以骨架默认即 fail-closed；
  广告 UID 或复用媒体 Speaker 设备被资格规则构造性拒绝（主机测试
  含 Speaker 形状被拒一项）。将来 PCM supplier 出现时在 x9/audio 的
  HAL 插件层注册，内核侧经此合同校验。

## 第二轮：L4 再取证——结案（2026-11-27 晚）

本轮对固定 MD 固件（`.local/x9/modem/parts/modem_a.img`）做了比
第一轮两个窄区间宽得多的取证，结论：**"3D00 帧协议"作为 AP 可
实现的 producer/consumer 不存在，此路结案。**

证据链：

1. **ABI 头先行定性**：AP 可见的 `audio_speech_msg_id.h`
   （oppo-modules 参考源 `drivers/misc/mediatek/audio_ipi/include/`）
   定义 M2D 基址 0x3D00、D2M 基址 0xD300，而整个 M2D 空间**只有
   一条消息** `IPI_MSG_M2D_CALL_DATA_READY = 0x3D00`。3D00 不是
   帧族，是 MD↔DSP IPI 域内的一条握手消息号。
2. **全量符号表**：用树内 dbginfo 恢复法（`x9/tools/md_call_evidence.py`
   的解析）解出 md1_dbginfo 的全部 146,852 个符号。全固件与 AUIF
   相关的符号**只有一个**：`SAL_ExtAUIF_SetMemAddr` @9063b446；
   语音簇（9063xxxx–9080xxxx）内没有任何 M2D/IPI 发送符号，其发送
   路径是 `spcIO_sendMsgViaCCCI`（即 2f20 的 SPC/CCCI 流）与 codec
   状态通知。字符串扫描同样无 CALL_DATA_READY/M2D/D2M 协议串
   （`m2d` 命中均为二进制噪声与无关 token）。
3. **AUIF 的真实语义**：固件内日志串显示 `SAL_ExtAUIF_SetMemAddr`
   按 MemType 编程内存窗口（base/ENH/OD/BLE 各一组 StartAddr/Len）——
   它是**共享内存窗口编程接口**，不是帧通道；逐帧 PCM 不以 IPI
   消息形式流动。窗口的输入正是 CCCI runtime feature 数据
   （`SP_SetUSIP_MPU_Info` 从 feature 记录取 {addr,size} 调它），
   而本桥已经经 feature 31 把 id 24 区域通告给 MD——MD 侧编程
   AUIF 所需的数据已经在既有路径上流动。
4. **仍未闭合但不再阻塞**：0x3D00 这条握手在 MD 内部的确切发送点
   与同版 DSP 消费体，需要 nanoMIPS 能力的反汇编会话（Mac 上有
   IDA 9.1）或原厂通话时的 MD 内部日志；两者都只影响 MD/DSP 内部
   叙述，不产生任何 AP 侧实现面。后续投入转向 L5（EMI owner 已
   落地）+ L6（Hostless 合同）路线。

## 第二轮：L2 材料清单（2026-11-27 晚，仅清单不写代码）

L2（AP↔DSP IPI DMA/REG_DMA）的布局已经静态闭合（header 128B、
AP_TO_DSP entry +128、DSP_TO_AP entry +512、每 entry 384B），缺的
纯粹是材料：**与本机固定 MD/ROM 输入同版构建的 `audio_dsp` 固件
本体**。本机 OTA 分区表里有 audio_dsp 之名，但 .local 的分区备份、
modem 原件与已捕获 vendor 文件里都没有这份 payload；树内现有的
只是 task 名表与消息布局等参考输入，不是固件。

- **需要哪一版**：与 `modem_a.img` 及 2026-11-15 原厂捕获同一
  构建列车的 audio_dsp 分区 payload（同版 DSP 固件），版本错位
  的同名固件不可用——REG_DMA 布局与 task 表都按同版解释。
- **从哪提取**：本机（PLJ110）原厂全量包 / OTA 包的 audio_dsp
  分区镜像；提取后先做哈希与分区表对照存档，再进探针构建。
- **提取后第一步验证**（P0 式 residency probe，只读不消费）：
  ① 探针构建加载该 payload 并确认驻留；② 读 runtime
  task-info[0] 的 8 字节（scene 0），按参考平台规则由
  core_total 判定物理 core（>1 → HiFi3_B），**不静态猜 core**；
  ③ 核对 REG_DMA header 与 +128/+512 两条 entry 的落点与已闭合
  布局一致。三项全过之后，才谈 AD20/scene 注册等消费者代码；
  在此之前 L2 不写任何消费者实现。
