# MT6993 DSP / 音频驱动移植难度评估

- 设备：OPPO Find X9（`PLJ110` / `OP5E17L1`，MediaTek MT6993），X9 分支
- 目标 OS：未经修改的 iOS 16.7.16（`20H392`，iPhone X 身份）用户land + 本仓库 XNU `8796.141.3`
- 源码基线：`/Users/tosasitill/HelioDarwin/oppo-source`（`android_kernel_oppo_mt6993` + `modules/kernel/kernel_device_modules-6.12`）
- 参考方法论：<https://unixnu.com/zh/porting/power>（《DSP、音频与电源驱动》），正文快照存于 `/tmp/unixnu_power.txt`
- 日期：2026-11-03
- 性质：**只读评估**。本文没有改动任何生产代码，没有构建设备产物，没有接触硬件。

---

## 1. 摘要与难度结论

**难度：中高。整体低于 GPU/Metal 那条线，高于显示与触控；可按阶段切分推进。**

决定性判断不是"这颗芯片有没有 DSP"，而是：**MTK 的 DSP 侧协议整套都在我们手里的源码中**（消息 ID 枚举、结构体字节布局、mailbox 寄存器偏移、时钟与上电序列全部可读）；而参考页走的 Qualcomm AudioReach 路线里，图拓扑封在固件内，Linux 只有半套 `q6apm`，只能反推。因此对我们而言**未知量更少，但要照抄的寄存器代码更多**。

一句话分工结论：**参考页可复用的是它的 OS 侧骨架，不可复用的是它的全部硬件与协议部分。**

| 阶段 | 内容 | 难度 | 是否需要 DSP |
| --- | --- | --- | --- |
| A | AFE MEMIF(DL0) → ADDA → MTKAIF v4 → MT6681 → speaker 的纯 CPU 通路 | 中高 | 否 |
| B | ADSP offload：SMC 起核 + mailbox/IPI + 共享环 ABI + Aurisys | 高 | 是 |
| C | 录音链路（DMIC/AMIC、UL、增益、AK7965 类前端） | 中 | 部分 |
| D | SCP-audio 通话 / VOW / BTCVSD | 很高（建议不排期） | 是（另一条 DSP） |

---

## 2. 范围与非范围

**在范围内**：阶段 A/B/C 涉及的硬件层、传输层、协议层与 iOS 侧接入形状。
**不在范围内**：同一参考页里的电池上报（`IOPMPowerSource`）与 CPU/GPU 调频。两者各自是独立工作量，且我们已有部分对应物（sweet 分支的静态电池服务、X9 的 `gpufreq_ipi` 观测）。混进本评估会让难度数字失真。

---

## 3. 证据与标签词表

沿用规范 §15 的标签，全部为英文关键词：

- **SOURCE CONFIRMED** — 我在本次会话里直接读到该文件该行的内容（含 `grep`/`wc` 计数）
- **DEVICE VERIFIED** — 物理设备上观察到的行为（本文没有此类条目）
- **BUILD VERIFIED** — 编译通过（本文没有此类条目）
- **NOT TESTED** — 逻辑上成立但未在硬件上验证
- **INFERENCE** — 推断，附依据

本文绝大多数条目是 **SOURCE CONFIRMED**（源码静态阅读）与 **NOT TESTED**。**没有任何一条音频行为是 DEVICE VERIFIED 的**。

下文链接的每条行号都经过一次 `grep -n` 或 `sed` 定位复核。

---

## 4. 参考页对照：UniXNU(power) 的可复用边界

参考页的方法论骨架（确认接口 → 传输层 → 音频流 → 电池 → 调频 → 构建验证）**通用且应当沿用**；它点名的具体技术栈**全部是 Qualcomm 专有**：GLINK/SMEM/SMP2P、AudioReach/GPR、APM、`q6apm-dai`、WCD938x、SoundWire、PRM 时钟投票、EPSS LUT。

**SOURCE CONFIRMED（negative）**：在 `adsp`、`audio_ipi`、`audio_dsp`、`audio_scp` 四个目录内对 `audio_reach|gpr|apm|tplg|topology` 做递归匹配，命中 **0** 行；对 `request_firmware|iova|iommu|dma_map_` 命中 **0** 行。也就是说 MTK 既没有 graph/图对象概念，也没有固件加载器与 SMMU 依赖。

**可复用（来自 `github.com/UniXNU/UniXNU-sheng`，BSD-2-Clause-Patent，本次以 HTTP 取回并阅读）**：

| 文件 | 规模 | 复用价值 |
| --- | --- | --- |
| `hal/ShengAudioPlugIn.c` | 703 行 | 高。它就是标准 CoreAudio `AudioServerPlugIn` HAL bundle，`Info.plist` 走 `CFPlugInTypes = 443ABAB8-E7B3-491A-B985-BEB9187030DB` |
| `package/ShengAudioService.cpp` | 143 行 | 高。内核侧只是 `IOService` + `IOUserClient`：`clientMemoryForType()` 映射两块采样环、`externalMethod()` 收 start/stop、`setProperty("NativeSandboxGrantProcess", "audiomxd,mediaserverd")` 过沙箱 |
| 同文件里的 `setHostClock()` | 约 40 行 | 高，且问题会原样继承：CoreAudio 把 host clock 硬编码为 24 MHz，非 24 MHz 时基下音频服务器的 IO 周期偏长、播放录音都卡；他们用一次 `phystokv` + `pmap_find_pa` 改写共享缓存里那个 double |
| `package/AudioProbe.cpp` / `ShengAudioReach.c` / `ShengGpr.c` / `AdspProbe.cpp` | 约 120 KB | **零复用**。GLINK/SMEM/GPR/APM 在 MT6993 上不存在 |

**INFERENCE**：他们把设备描述放在 kernelcache 内的 probe、把设备外观放在用户land HAL plugin，是 iOS 侧唯一可行的切分（`coreaudited`/`mediaserverd` 只认 HAL bundle），所以这个形状我们会照搬；照搬的是**结构**，不是代码里的 Qualcomm 常量。

---

## 5. MT6993 音频栈实测清单

### 5.1 代码规模（**SOURCE CONFIRMED**，`find … -name '*.c' -o -name '*.h'` + `wc -l`）

| 目录 | 文件数 | 行数 |
| --- | --- | --- |
| `drivers/misc/mediatek/adsp`（含 v0–v3） | 64 | 10,569 |
| `drivers/misc/mediatek/audio_ipi` | 25 | 7,933 |
| `sound/soc/mediatek/audio_dsp` | 17 | 10,214 |
| `sound/soc/mediatek/audio_scp` | 14 | 4,803 |
| **DSP + 传输小计** | **120** | **33,519** |
| `sound/soc/mediatek/mt6993` | 20 | 145,067（其中 `mt6993-reg.h` 111,887，代码约 33,180） |
| `sound/soc/mediatek/common` | 21 | 6,513 |
| `sound/soc/mediatek/common_int` | 64 | 34,504 |
| `sound/soc/mediatek/codec` | 5 | 7,188 |
| `sound/soc/codecs/mt6681.c` | 1 | 35,780 |

关键文件：`mt6993-afe-pcm.c` 13,077 行、`mt6993-dai-i2s.c` 5,923、`mt6993-mt6681.c` 2,731、`mt6993-dai-adda.c` 2,424、`mt6993-afe-clk.c` 934。

### 5.2 硬件接线（**SOURCE CONFIRMED**，全部读自 [mt6993.dts](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts)）

| 项 | 值 | 引用 |
| --- | --- | --- |
| AFE | `compatible = "mediatek,mt6993-sound"`，`reg = <0 0x1a110000 0 0x10000>` | [L12448](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts#L12448)，reg/interrupts 紧随其后 |
| AFE 中断 | `GIC_SPI 384 IRQ_TYPE_LEVEL_HIGH` | 同上节点 |
| 声卡 | `compatible = "mediatek,mt6993-mt6681-sound"`，`mediatek,platform = <&afe>`，`mediatek,ipm = <1>`（IPM2.0） | [L12429](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts#L12429) |
| 音频时钟控制器 | `afe_clk: clock-controller@1a110000`，`"mediatek,mt6993-audiosys", "syscon"` — 与 AFE 同一区域 | [L1366](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts#L1366) |
| audio-sram | `audio-sram@1a120000`，`"mediatek,audio_sram"` | [L12311](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts#L12311) |
| adspsys | `0x1a000000`，cfg/cfg2/cfg3 + 4 组 mailbox `0x1a350000 / 0x1a360000 / 0x1a370000 / 0x1a380000` | [L5641](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts#L5641) |
| adsp core | `adsp_core0@1a200000` / `adsp_core1@1a210000`，`interrupt-names = "sysirq","wdt","audioirq"` | [L5710](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts#L5710) / [L5727](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts#L5727) |
| ADSP 音频预留量 | `adsp-rsv-audio = <0x5c0000>`（5.75 MiB） | [L5706](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts#L5706) |

**好消息（SOURCE CONFIRMED）**：MEMIF 环形缓冲有 32 位 + MSB 两个寄存器（`reg_ofs_base = AFE_DL0_BASE` 与 `reg_ofs_base_msb = AFE_DL0_BASE_MSB`，[mt6993-afe-pcm.c#L4778](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/sound/soc/mediatek/mt6993/mt6993-afe-pcm.c#L4778)），因此**没有参考页里 SM8550 那条"周期缓冲必须放在 4 GiB 以下"的约束**。

**坏消息（SOURCE CONFIRMED）**：MT6681 不是 smart PA，而是 I2C 总线上的 PMIC codec —— [mt6681-core.c#L373](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/mfd/mt6681-core.c#L373) 用 `devm_regmap_init_i2c()`，[L418](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/mfd/mt6681-core.c#L418) 匹配 `"mediatek,mt6681_pmic"`。所以移植要连带一套 PMIC/稳压器访问，而它的模拟配置表就在 35,780 行的 codec 驱动里。

---

## 6. 接口输入表（对齐参考页 §1）

| 参考页要求的功能输入 | MT6993 上我们实际掌握的 | 状态 |
| --- | --- | --- |
| DSP 固件/PAS ID、内存预留、邮箱中断、共享内存布局、时钟与电源归属 | 预留内存 key `mediatek,reserve-memory-adsp_share`（[adsp_reserved_mem.c#L19](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/adsp/adsp_reserved_mem.c#L19)）；4 发送 / 2 接收 pin、`SHARE_BUF_SIZE 240`（[adsp_helper.h#L19](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/adsp/include/adsp_helper.h#L19)）；起核是 SMC | 布局 SOURCE CONFIRMED；**固件归属 NOT TESTED** |
| codec/功放 ID、总线、时钟、路由、DSP 拓扑、支持格式 | MT6681 over I2C；`playback0 = COMP_CPU("DL0")`（[mt6993-mt6681.c#L639](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/sound/soc/mediatek/mt6993/mt6993-mt6681.c#L639)）；**无 DSP 拓扑**，路由是编译进内核的 DAPM 表 | SOURCE CONFIRMED |
| 电池协议 | 不在本文范围 | — |
| 时钟域 / OPP / 稳压器 / 固件投票 | AFE 需 `devm_clk_get` 一组 DT `clock-names` + `cksys`/`vlpcksys`/`vlpcksys-apll1|2` syscon，外加 `MTK_SIP_AUDIO_CONTROL` 的 SMC 初始化（[mt6993-afe-pcm.c#L12929](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/sound/soc/mediatek/mt6993/mt6993-afe-pcm.c#L12929)） | SOURCE CONFIRMED |

---

## 7. DSP 传输层（阶段 B）

**SOURCE CONFIRMED**：MT6993 走 `adsp-v3`。判定依据是 platform 表而非 Makefile：[adsp_driver_v3.c#L21](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/adsp/v3/adsp_driver_v3.c#L21) 定义 `mt6993_adspsys_desc = { .platform_name = "mt6993", .version = 3, .semaphore_ways = 3, .semaphore_ctrl = 2, .semaphore_retry = 5000 }`，[L56](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/adsp/v3/adsp_driver_v3.c#L56) 匹配 `"mediatek,mt6993-adspsys"`。mt6991 是 v2。

**起核 = 一路 SMC**（**SOURCE CONFIRMED**）：[adsp_core.c#L32](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/adsp/adsp_core.c#L32) 的 `adsp_smc_send()` 就是 `arm_smccc_smc(MTK_SIP_KERNEL_ADSP_CONTROL, opid, …)`，操作为 SYS_CLEAR / CORE_START / CORE_STOP / RELOAD；就绪由 `ADSP_IPI_ADSP_A_READY` mailbox 触发 completion，等待上限 2 s。

**消息框（**SOURCE CONFIRMED**）**：[audio_messenger_ipi.h#L109](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/audio_ipi/include/audio_messenger_ipi.h#L109) 的 `struct ipi_msg_t` = 16 字节头（`magic`、`task_scene`、source/target layer、`data_type`、`ack_type`、`msg_id`、`param1/payload_size`、`param2`）+ 96 字节 payload；`MAX_IPI_MSG_BUF_SIZE (112)`（同文件 [L21](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/audio_ipi/include/audio_messenger_ipi.h#L21)）。**512 字节上限是参考页 Qualcomm 的规则，对 MTK 不适用** —— 我们的大块信息靠"只传物理地址 + 共享 DRAM 环"走。

**消息 ID 面（**SOURCE CONFIRMED**，按枚举项计数）**：playback 48（`AUDIO_DSP_TASK_OPEN = 0x1` 起）、speech 21、controller 12，合计 **81**；控制器有 A2D `0xAD00` / D2A `0xDA00` / D2D `0xDD00` 三个基址（[audio_controller_msg_id.h#L9](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/audio_ipi/include/audio_controller_msg_id.h#L9)）。Aurisys 音效库只有 4 条消息入口：`…_AURISYS_CFG / SET_PARAM / GET_PARAM / ENABLE`（同文件 [L17](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/audio_ipi/include/audio_controller_msg_id.h#L17) 起）。`task_scene` 枚举约 73 项（[audio_task.h](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/audio_ipi/include/audio_task.h)）。

**超时语义（**SOURCE CONFIRMED**）**：`wait_event_interruptible_timeout(..., msecs_to_jiffies(k_wait_ms))`，`retval == 0` 时按 10 ms 超时分支处理并重试（[audio_ipi_queue.c#L555](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/audio_ipi/common/audio_ipi_queue.c#L555) / [#L583](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/audio_ipi/common/audio_ipi_queue.c#L583)）。这正好对应参考页"处理协议超时"的难点条目，逻辑可直接平移。

**打开一条播放路径 = 四条消息**（**SOURCE CONFIRMED**，`audio_dsp/mtk-dsp-platform-driver.c`）：`adsp_register_feature()`（保活/投票）→ `AUDIO_DSP_TASK_OPEN`（需要 ACK）→ `HWPARAM`（带缓冲描述符物理地址）→ `PREPARE` → `START`（`DIRECT_SEND` 免 ACK）。没有 open-graph/prepare-graph 的对象生命周期，也不需要 token 匹配响应，这比 AudioReach 简单一档。

**用户land ABI（**SOURCE CONFIRMED**）**：字符设备 `/dev/audio_ipi`（[audio_ipi_platform.h#L38](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/misc/mediatek/audio_ipi/include/audio_ipi_platform.h#L38)），ioctl magic `'i'`，命令 `SEND_MSG_ONLY(0) / SEND_PAYLOAD(1) / SEND_DRAM(2) / INIT_DSP(20) / REG_DMA(21)`；另有 `/dev/adsp`（`ADSP_REG_FEATURE` 保活）、`/dev/adsp_0|1`（DSP 日志环）。**INFERENCE**：这套 ioctl 是 MTK 半套 HAL 的控制面，我们不会移植 HAL，但它是免费的协议文档 —— 每个 ioctl 的行为都能在驱动里读到。

---

## 8. 缓冲 / DMA / 缓存一致性

**SOURCE CONFIRMED**：`audio_ipi` 用两块 `gen_pool`，都盖在预留 DRAM 上；控制面描述符结构里显式写了 `base_phy.addr_val` / `base_vir.addr`，并且带一个 `checksum = (byte*)&dma->checksum - (byte*)dma` —— 即**字节偏移被当作校验和交给 DSP 重算，结构体布局就是 ABI**。环形描述符把 `read_idx` 放在 +128、`write_idx` 放在 +256（缓存行隔离），AP 与 DSP 互相轮询 DRAM 而不碰寄存器。对齐按核型区分：HIFI3/RV 为 128 B，CM4 为 32 B。

**SOURCE CONFIRMED（negative）**：这四个目录里没有任何 `dma_sync_*` / cache flush —— 一致性是靠一条消息 `AUDIO_DSP_TASK_EINGBUFASHAREMEM` 让 DSP 侧把该物理区间声明为 write-through，以及通过 `mtk_emimpu_*` 设置该区间的 EMI/MPU 权限实现的。

**移植含义（INFERENCE，风险偏高）**：XNU 侧不能照抄"对方负责关缓存"这个隐含约定。我们必须显式决定这块 carve-out 的映射缓存模式，并保证与 DSP 的假设一致 —— 这正是 X9 GPU 线上已经踩过的那类一致性问题（`capable=0`、有界 cache acquire）。**建议在阶段 B 之前先用一次纯 DMA 写 + AP 读回做定向实验。**

**无 IOMMU（SOURCE CONFIRMED）**：这四目录里 `iova|iommu|dma_map_` 命中 0，音频 DMA 用物理地址直达。对我们有利（不必建 SMMU 页表），但要求 XNU 的 `IOMemoryDescriptor` 能给出一个 DSP 可读的物理地址，且注意 X9 的 DRAM 高位 bank（sweet 分支曾把内核放到 4 GiB 以上，MT6993 的 AFE 有 MSB 寄存器可用，ADSP 侧则**未验证**）。

---

## 9. AFE 与最小播放通路（阶段 A）

这是把"音频"从不可控的大项目变成可控的中等项目的关键：**MT6993 有一条完全不经过任何 DSP 的 CPU 播放通路**。

**SOURCE CONFIRMED** 的上电序列（[mt6993-afe-pcm.c#L7183](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/sound/soc/mediatek/mt6993/mt6993-afe-pcm.c#L7183) 起的 `mt6993_afe_runtime_resume`）：
`enable_clock` → `sram_request` → `regcache_sync` → `AFE_SPM_CONTROL_REQ` 请求 26 MHz（[#L7203](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/sound/soc/mediatek/mt6993/mt6993-afe-pcm.c#L7203)）→ `AUDIO_TOP_CON4 = 0`（IPM2.0 放开 AP 侧模块时钟）→ `AUDIO_TOP_CON0..3 = 0` → `AFE_CBIP_CFG0 = 1` → `AFE_MEMIF_CON0` hd_align → `AUDIO_ENGEN_CON0 = 1`（[#L7226](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/sound/soc/mediatek/mt6993/mt6993-afe-pcm.c#L7226)）。

通路：`playback0(DL0)` → DPCM 动态路由 → BE `Primary Codec` = ADDA + MT6681 → MTKAIF v4 → DAC/HP → `Ext_Speaker_Amp`。MEMIF 数量 46（DL0…HDMI），FE DAI 以名字注册；`mt6993-afe-pcm.c` 里约 50%（≈6,535 行）是纯硬件描述表（memif 描述符、686 条 `SOC_DAPM_SINGLE` mux、DAPM 路由、irq 槽位），这部分**在 Darwin 上不需要重写代码，只需要重写承载它的载体**（plist/结构数组）—— 属于体力而非智力成本。

阶段 A 最贵的两点（**SOURCE CONFIRMED**）：
1. **MTKAIF v4 lane 相位校准**：[mt6993-mt6681.c#L355](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/sound/soc/mediatek/mt6993/mt6993-mt6681.c#L355) 逐相位试 42 次（`mtkaif_calibration_num_phase = 42`）。不做或做错 = 有时钟但全静音/爆音，且很难从日志区分。
2. **音频时钟树**：`afe_clk`（与 AFE 同区）+ `cksys`/`vlpcksys`/`apll1|2` syscon。这类工作在 GPU 线上已经做过一次（MFGPLL/bg3d CG），方法可复用，但**MT6993 的序列不能套用 MT6789**（`x9/README.md` 已明令禁止）。

---

## 10. OS 服务接入（iOS 侧）

**SOURCE CONFIRMED（读自 UniXNU-sheng）**：iOS 侧的最小形状是——

1. 内核里一个 `IOService` 子类，`setProperty("IOUserClientClass", …)` + `registerService()`；
2. `IOUserClient` 只做三件事：`clientMemoryForType()` 暴露控制页与两个采样环、`externalMethod()` 收 start/stop、`clientClose()` 停流；
3. 用户land 一个 `AudioServerPlugIn` HAL bundle 放进 `/Library/Audio/Plug-Ins/HAL/`，**样本不经过内核 Mach 调用**，由 HAL plugin 自己按 IO cycle 从环里搬；
4. 沙箱需要 `NativeSandboxGrantProcess` 点名 `audiomxd,mediaserverd`；
5. host clock 修正（见 §4）。

**与本项目现状的衔接（SOURCE CONFIRMED，来自 `NOTES.md` 与 `x9/README.md`）**：sweet 分支上 `mediaserverd` 之所以崩溃，正是因为没有真实音频硬件、默认虚拟设备 `VirtualAudioDevice_Default` 不存在 —— 当时把 `VirtualAudio.plugin` 移到 `HAL.disabled/` 只是**屏蔽症状**，不是解决方案。也就是说 iOS 侧"要什么"我们其实已经见过一次，缺的是硬件那一半。X9 分支的 `x9/README.md` 目前把 audio 列在"未实现/未验证"里。

**NOT TESTED**：`AudioServerPlugIn` bundle 在 X9 的插件白名单/mailbox 安装机制之外能否被 `coreaudited` 加载。X9 的 allowlist 只针对 `X9GeneralMetal*` / `X9PanVK*`；HAL bundle 走的是普通文件系统路径，理论上一视同仁，但要在设备上确认（并且注意任何 `--revision` 变更仍需要 mailbox re-staging，这与音频无关但容易误判为音频回归）。

---

## 11. 分阶段里程碑与验收口径

| 阶段 | 第一个可观察结果（必须能在日志里指出行） | 完成定义 |
| --- | --- | --- |
| A0 | 从 XNU 读到 `0x1a110000` 区间上的 AFE 复位默认值与 `AUDIO_ENGEN_CON0` | MMIO 可读、不被 SLB/MPU 拦 |
| A1 | I2C 读到 MT6681 器件 ID，并打开一路 LDO | codec 在总线上应答 |
| A2 | 跑完 `runtime_resume` 等价序列后 `AUDIO_ENGEN_CON0` 读回 1 | 时钟与 26 M 请求成立 |
| A3 | DL0 环形 DMA 播放 1 s 已知正弦波，人耳/示波器确认 | **一条完整路由传输 PCM 数据、报告进度并释放资源**（即参考页 §3 的结果口径） |
| A4 | `DL0_CUR` 单调推进、指针回调与 CoreAudio 时间戳一致 | HAL plugin 里 `coreaudited` 稳定不重启，无声卡丢失 |
| B1 | ADSP SMC 起核后收到 `ADSP_IPI_ADSP_A_READY`，一次带超时的 `AUDIO_DSP_TASK_OPEN` 往返成功 | 通道可开、可请求/响应、可失败重连（参考页 §2 口径） |
| B2 | offload 路径出声，Aurisys `ENABLE` 有可听变化 | 与 A3 的 CPU 路径可切换、可 A/B |
| C1 | 录 2 s 已知声音，输出周期数、字节数、非零样本比例、峰值（等价于参考页 `sheng-audio=4`） | 声道顺序/采样率/流位置正确，应用内录音可用 |

**INFERENCE（工时，非实测）**：A ≈ 3–6 周，B 再加 4–8 周，C ≈ 2–3 周；D 不排期。前提是我们已经具备本仓库里已有的同类能力（MMIO 映射、I2C master、GIC 中断、`IOMemoryDescriptor` 映射给用户进程），这些都不是新风险。

---

## 12. 风险登记与前置实验

| # | 风险 | 证据强度 | 最便宜的判定实验 |
| --- | --- | --- | --- |
| R1 | **AFE 中断状态是走 SMC 代读的**：`mt6993_afe_irq_handler` 用 `arm_smccc_smc(SMC_SC_NBL_VHM_REQ, 0x90000000, hwirq, …)` 取 `AFE_IRQ_MCU_STATUS`（[#L7049](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/sound/soc/mediatek/mt6993/mt6993-afe-pcm.c#L7049)），`hwirq` 取自 Linux 的 irq desc（[#L12904](file:///Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/sound/soc/mediatek/mt6993/mt6993-afe-pcm.c#L12904)）。这个构建根本没有直接读寄存器 | SOURCE CONFIRMED | 从 XNU 直接 `readl(AFE_IRQ_MCU_STATUS)` 看是否返回合理值、是否触发 secure abort；同时试 `SMC_SC_NBL_VHM_REQ` 在 stock ATF 下是否应答 |
| R2 | **ADSP 固件由谁放进预留内存未知**：四个目录里 0 处固件加载，注释写"Reserved memory allocated from lk" | SOURCE CONFIRMED（代码）+ INFERENCE（归属） | 起 Android、`fastboot boot` 我们的 shim 之后，检查 `mediatek,reserve-memory-adsp_share` 区间是否仍带镜像内容；发 `SYS_CLEAR` 看是否有 READY |
| R3 | **缓存一致性约定**（DSP 侧 write-through + EMI/MPU 权限，AP 侧不 flush） | SOURCE CONFIRMED（negative） | 定向写/读回实验；必要时把 carve-out 映射为非缓存 |
| R4 | host clock ≠ 24 MHz 导致的 IO 周期偏差 | SOURCE CONFIRMED（参考实现）+ NOT TESTED（我们芯片实测时基） | 第一次出声后测周期抖动；X9 抓到的 DT 里 timer 数值需实测复核 |
| R5 | MT6681 是 I2C PMIC，移植会连带稳压器/`mt6681-regulator.c` | SOURCE CONFIRMED | 只读器件 ID 与一路 LDO，不做充电相关部分 |
| R6 | MTKAIF v4 lane 校准失败表现为"有时钟无声" | SOURCE CONFIRMED | 严格按 `#L355` 的 42 相位循环复刻，并把每次 phase 结果打进日志 |
| R7 | 不能套用 MT6789 的 PMIC/时钟序列 | SOURCE CONFIRMED（`x9/README.md` 明令） | 每个 MT6993 寄存器值都从本 dts/驱动取证 |
| R8 | `coreaudited` 侧沙箱/签名是否放行我们的 HAL 与 UserClient | NOT TESTED | 先只注册 `IOService` 并用测试进程 `IOServiceOpen` 打通，再谈 bundle |

---

## 13. 未决问题、下一步与文件索引

**未决（需要用户或设备输入）**：
1. 音频里程碑在 X9 路线里排在什么位置（当前 `x9/README.md` 把它与 storage/scanout/GPU/input/wireless 并列在未实现）；
2. 是否接受"先做 A 阶段（无 DSP）、把 B 阶段当作后续优化"的取舍 —— 我的建议是接受；
3. R1/R2 两个实验是否允许在设备上做一次只读探测（不写分区、不改引导）。

**建议的下一步（不改生产代码）**：
1. 从设备抓活的 MT6993 DTB，确认 AFE/adspsys/audio-sram 的实际 `reg`、中断号与 `mediatek,reserve-memory-adsp_share` 的真实区间（源码 dts 是 SoC 级，板级在未附带的 `android_kernel_modules_and_devicetree_oneplus_mt6993`）；
2. 写一份 AFE 寄存器与 MT6681 I2C 的"只读探针"清单，作为 A0/A1 的产物；
3. 把 §11 的 A3 作为音频第一个 DEVICE VERIFIED 目标写进 `NOTES.md` 的交接段。

**主要取证文件**（均在 `/Users/tosasitill/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/` 下）：
`sound/soc/mediatek/mt6993/mt6993-afe-pcm.c`、`mt6993-mt6681.c`、`mt6993-dai-adda.c`、`mt6993-afe-clk.c`、`mt6993-afe-common.h`、`mt6993-reg.h`；
`sound/soc/mediatek/audio_dsp/`、`audio_scp/`；
`drivers/misc/mediatek/adsp/`（含 `v3/`）、`drivers/misc/mediatek/audio_ipi/`；
`drivers/mfd/mt6681-core.c`、`sound/soc/codecs/mt6681.c`；
`arch/arm64/boot/dts/mediatek/mt6993.dts`。

**外部参考**：<https://unixnu.com/zh/porting/power>（方法论）、`github.com/UniXNU/UniXNU-sheng`（iOS HAL/IOUserClient 骨架与 host-clock 补丁，BSD-2-Clause-Patent，复用其代码时须保留许可证）。
