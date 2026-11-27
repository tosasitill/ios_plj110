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
