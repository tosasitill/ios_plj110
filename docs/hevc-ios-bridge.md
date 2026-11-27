# HEVC/HEIF 硬解 iOS 兼容层设计（基于 Android MT6993 源码）

2026-11-20，`feat/hevc-hw`（基线 `368cfff8`）。本文是设计文档，不是实现收据：
本轮无生产源码 delta、无编译、无 CI、无设备操作。所有 Android 结论给出
`文件:行号`（相对 `.local/references/oppo-modules-dt-mt6993/kernel/kernel_device_modules-6.12/drivers/`，
下称 `D/`）；X9 结论给出仓库内路径。区分**观察**（源码/已保存原件）与**推断/待证**。

## 1. 范围

让未修改 iOS 16.7.16（20H392）用户态的 HEVC 视频与 HEIF 静态图走 MT6993 硬件解码：

- 视频：VideoToolbox `VTDecompressionSession` → 自有注册解码器 → `/dev/x9-vdec` 会话。
- 静态图：ImageIO/CMPhoto HEIF → 两条路线（§8）：自有解析+单帧硬解（先行）与
  CMPhoto native Tile v3（自动入口，沿用已有 A–H 生产计划）。
- 不含：编码、AV1/VP9（硬件支持但不在本期）、SVP 安全路径、Apple VXD 协议仿真。

## 2. Android 源码关键事实（观察）

### 2.1 mt6993 唯一解码路径是 VCP，AP 不碰 codec 寄存器

- 接口选择：`D/media/platform/mtk-vcodec/vdec_drv_if.c:28-44`，
  `mtk_vcodec_is_vcp()` 位图在 `vdec_vcp_probe` 置位（`vdec/vdec_vcp_if.c:1301-1302`）。
- mt6993 DT 节点 `mediatek,vcodec-dec` **无 reg/interrupts/larbs**，仅 2 个 CG 时钟，
  `mediatek,ipm=<2>`、`vdec-power-in-vcp=<1>`、`vdec-mmdvfs-in-vcp=<1>`
  （dts `arch/arm64/boot/dts/mediatek/mt6993.dts:13093-13108`，相对 modules 仓库根）。
- `power_in_vcp=1` 时 AP 侧时钟/复位/IRQ 全部跳过（`vdec_drv_if.c:261,290`）；
  **codec 域时钟、硬件复位、解码 IRQ 全在 VCP 固件内管理**。AP 的 LOCK/UNLOCK
  CORE/LAT 反向 IPI 在此模式只做 DVFS 记账（`vdec_vcp_if.c:892-929`）。
- LAT 核做 VLD/熵解析、CORE 核做重建，ipm v2 racing（`mtk_vcodec_dec_pm.c:192-196`）。

**对 XNU 的意义**：X9 不需要移植 AP 侧 codec 时钟/复位/IRQ 驱动；需要的是
VCP tinysys 启动 + mailbox IPI + 固件请求的内存供给。这与 X9 现有
`x9_vcp_bus`（mailbox 传输）+ `x9_vcp_wire`（IPI ABI）+ control（INIT/START/输出）
的分层完全一致。

### 2.2 VCP 冷启动：内核不加载固件，只做 SMC 复位释放

- `vcp_enable_pm_clk`（`D/misc/mediatek/vcp/rv_v2/vcp_helper.c:883-914`）：
  首个 feature 注册时 `vcp_enable_irqs()`，若未 READY 则 `reset_vcp()`；引用计数。
- `reset_vcp`（`vcp_helper.c:1144-1224`）精确序列：
  1. `vcp_set_clk()`（rv_v2 下仅 fmeter 调试，无实际时钟写，`vcp_helper.c` 同文件）。
  2. core_nums==2：写 MMUP 的 `DRAM_RESV_ADDR/SIZE/LOGGER`
     （= `R_CORE1_GENERAL_REG1/2/3`，`vcp_reg.h:144-159`），值为
     `VCP_PACK_IOVA(vcp_mem_base_phys)`、`vcp_mem_size`、
     `VCP_PACK_IOVA(vcp_mem_logger_phys)`。
  3. SMC `MTK_SIP_TINYSYS_VCP_CONTROL` = `ARM_SMCCC_CALL_VAL(FAST,64,SIP,0x52C)`
     = **0xC200052C**（`include/linux/soc/mediatek/mtk_sip_svc.h:21-24,172-173`），
     op **6** = `MTK_TINYSYS_MMUP_KERNEL_OP_RESET_RELEASE`，arg2=1
     （op 枚举 `rv_v2/vcp_helper.h:67-75`：VCP RESET_SET=2/RELEASE=3、
     MMUP RESET_SET=5/RELEASE=6）。
  4. 轮询 `is_vcp_ready_by_coreid(MMUP_ID)`（100–200µs 步进，超时停止启动）。
  5. 写 VCP core0 的 GENERAL_REG1/2/3，**logger 寄存器 = logger_phys + logger_size**
     （尾界），SMC op **3**（VCP RESET_RELEASE），等 VCP READY。
- READY 通知是 IPI `IPI_IN_VCP_READY_0/1`，handler `vcp_A_ready_ipi_handler`
  （`vcp_helper.c:653-681`）同时校验固件上报的 TCM image size == `VCP_A_TCM_SIZE`。
- **rv_v2 驱动内无 request_firmware / TCM memcpy**（全目录 grep 仅 image-size 校验）。
  固件由 bootloader/安全侧预装：TCM（sram 0x80000 @ vcp@31800000，dts:13279）+
  DRAM reserved payload。内核只告诉固件 reserved 区基址/大小/logger 边界（MPU 用）。

**与 X9 已捕获证据吻合**：`x9_vcp_bus.h` 合同「MMUP1 cmd6/READY 然后 VCP0
cmd3/READY，一次每核」「MMUP GPR 收 logger 起点、VCP 收终点」「cfg+44/48/4c」
正是 GENERAL_REG1/2/3 与 op6/op3。`x9_vcp_reset_reply.inc` 已存捕获回包。
X9 shim handoff（`x9_vcp_handoff.h`）的 mbox 0x31b80000+0x10000*i、IRQ 593+i、
双 hart、sram 0x80000 与 DT 一致。

**待证（推断不作为设计前提）**：当前 LK→shim→XNU 链是否已预装 VCP 固件。
ADSP 的先例（NOTES §5.7：LK 只开电源域、无固件在跑）说明**不能默认已装**。
设计采用 probe-first（§5）。

### 2.3 IPI 协议与共享内存

- 五段消息基址（`vcodec_ipi_msg.h:24-28`）：AP 发 0xA000 / 固件 ACK 0xB000 /
  固件发 0xC000 / AP ACK 0xD000 / 异步 0xE000；无实例消息 +0x100。
  枚举 `vdec_ipi_msg.h:58-111` 与 X9 `x9_vcp_wire.h` 常量一一对应
  （a000 INIT、a001 START、a002 DEINIT、a003 RESET、a004 SET_PARAM、
  a005 FRAME_BUFFER、a100 QUERY_CAP、a101 BACKUP、c001–c009 反向、e000 异步）。
- 物理层：tinysys mailbox `mtk_ipi_send(IPI_OUT_VDEC_1)`（`vdec_vcp_if.c:276`）；
  ISR 只入队（:1013-1056），SCHED_FIFO kthread 处理（:677,704,1313）。
  X9 对应：bus 物理 poll + control thread-call worker（已实现）。
- INIT：发 `ap_inst_addr`（AP 指针 cookie），ACK 回 `vcu_inst_addr`（VCP 保留内存
  物理地址），`handle_init_ack_msg`（:373-389）按 `VDEC_MEM_ID` reserved 区偏移
  换算 vsi。**vsi 不是 AP 分配，是固件保留区内的固定布局**。
- SHM（type 8）分配反向 IPI 直接回 reserved 区 va/pa/len，**iova=pa 无翻译**
  （`vdec_vcp_if.c:477-486`）。
- set_param 多数只写 `vsi->dec_params` + change 位，由 SET_PARAM_DEC_PARAMS 一次
  IPI 生效（:1734-1885）；`fb_num_planes` 是 AP 直接写 vsi（:1827，NOTES 已证 f28）。
- ACK 超时：`wait_event_timeout` 常规 5s、**首帧 DEC_START 60s**（:35-40,295-300）；
  超时 → `VCP_SET_HALT "vdec_srv"` 等固件重启、置 abort、返回 -EIO（:344-360）。

### 2.4 内存供给是固件→AP 的反向 IPI（C005/C006）

- `handle_vdec_mem_alloc`（`vdec_vcp_if.c:468-516`）：固件请求内存，AP 用
  `mtk_vcodec_alloc_mem`（`mtk_vcodec_util.c:1108-1190`）从 dma_heap 分配并
  attach/map 到按类型选择的 device，回 va/pa/iova/len：
  - `MEM_SW`（BS/工作缓冲）：VCP 平台取 **`mtk_mm-uncached`** heap，device 为
    `VCP_IOMMU_WORK`（`get_dev_by_mem_type`，`vdec_vcp_if.c:434-466`）；
    长度 ROUND_N(len, VCP_CACHE_LINE)+VCP_CACHE_LINE。
  - `MEM_HW_CACHE`（FB）：`mtk_mm` heap，device 为 **AP `smmu_dev`**
    （`mtk_smmu_get_shared_device`，`mtk_vcodec_dec_drv.c:478`；DMA mask 34bit，:679）。
  - `MEM_HW`/`UBE_HW`：uncached heap + smmu_dev / VCP_IOMMU_UBE_*。
  - SVP 类型换 SEC heap（本期不做）。
- 分配记录挂 `vcu->bufs` 白名单，C006 free 必须命中同记录（:518-558）。
- **对齐硬约束**：BS DMA 64B、FB 每 plane 512B（`vdec_drv_if.c:142,151`）。
- FB cookie：`vsi->dec.vdec_fb_va` 传 **AP 指针**（`vdec_vcp_if.c:1542`），
  KE 用 fb_list/bs_list 白名单反查（:1959-1968）。X9 wire 已按「AP 地址是不透明
  往返 token、不解引用 Linux 指针」设计，一致。

**对 XNU 的意义**：X9 不需要主动给固件「喂」IOVA；要 implementing 的是 C005
反向分配 handler（AP 侧物理页 + 对应域 IOVA）与 C006 退休。X9 现有
`x9_vdec_dma.inc`/`x9_vdec_memory.inc` arena 与 wire 的 c005/c006 dispatcher 是
正确骨架；c006 目前 ENOTSUP，是必做缺口（§7）。

### 2.5 完成语义与 buffer 生命周期

- **DEC_START ACK ≠ 帧完成**（X9 前端已按此拒绝 START-only）。帧输出靠：
  - `VCU_IPIMSG_DEC_DONE`（resource 通道，:821-824）；
  - `PUT_FRAME_BUFFER` 同步（:853-861）/异步 e000（:862-874，先拷 vsi 环到本地
    再加锁消费）；a005/b005 是输出资源注册，不是像素（X9 wire 注释一致）。
- 三个环（`vdec_ipi_msg.h:455-549`）：`list_free_bs`（BS 回收）、`list_free`
  （**DPB 引用已释放的 FB，才可复用**）、`list_disp`（显示序输出）。
  固件管理 DPB：FB 两环都不出现 = 仍被参考。
- 显示序取出：`GET_PARAM_DISP_FRAME_BUFFER` 按 display order 从 list_disp 取
  （`mtk_vcodec_dec.c` get_display_buffer ~:1100-1189）；free 取出后 dma_buf_put
  或重新入队（:1199-1330）。`buf_finish` 做 `dma_sync_sg_for_cpu`（:4070）。
- BS 环消费协议：count==0 为空、read++/count--（`vdec_get_bs`）；FB 环
  count==64 为满（固件 publisher 证据，NOTES 2026-11-15 已纠正）。两端协议不同，
  X9 已分别建模。
- **VCP 路径无 VCU 的 slice dma-fence**（`VCU_IPIMSG_DEC_SLICE_DONE_ISR` 仅
  vcu_if.c:412-427）；VCP 用 general buffer 回传 fence fd（`mtk_vcodec_dec.c:2646-2672`）
  + 异步 put_fb。HEIF 单帧无需 fence（§8）。

### 2.6 输出配置与能力

- 分辨率/stride/fourcc/bitdepth 全由固件写 `vsi->pic`，AP 经 GET_PARAM 读取
  （PIC_INFO 定 sizeimage/bytesperline，`mtk_vcodec_dec.c:3947-3962`；
  DPB_SIZE :3978）；变化经 `V4L2_EVENT_SRC_CH_RESOLUTION`（:1775,3938）。
- 格式能力运行时 QUERY_CAP 向固件取（`vdec_vcp_if.c:1891-1957`），初始表为空。
- fourcc 表含 HEVC/**HEIF**/VP9/AV1 等（`vdec_drv_if.c:97-114`）；
  HEIF 是一等 fourcc（`mtk-v4l2-controls.h:22`），codec_id 区分 HEVC/HEIF
  （X9 wire：HEVC=2、HEIF=3，不可互换）。
- 首个 OUTPUT buffer 触发 NULL-FB header 解析（`mtk_vcodec_dec.c:3875`
  `vdec_if_decode(bs,NULL)`）→ res change → GET_PARAM → INIT/HEADER 顺序；
  NOTES 2026-11-16 已按源码核实（a000 INIT 在 header parse 之后、first-a005
  在 CAPTURE QBUF 之后）。

### 2.7 电源/DVFS/低功耗

- open/release 对称：`vcp_register_feature_ex(VDEC_FEATURE_ID)` +
  `mtk_mmdvfs_enable_vcp(true,VCP_PWR_USR_VDEC)`（`mtk_vcodec_dec_drv.c:113-124,328-337`）。
- `vdec-mmdvfs-in-vcp=1`：调频在 VCP 内；AP 只经 vsi 传 op_rate/priority/target_freq
  （`vdec_ipi_msg.h:540-546`）+ SET_PARAM_MMDVFS（`vdec_vcp_if.c:1835-1839`）。
- suspend 用 `is_codec_suspending` 阻塞 IPI + BACKUP/RESUME（a101/a102）。
- HEIF 显式排除 low_pw_mode 组解码（`vdec_vcp_if.c:117`）。

### 2.8 静态图/低延迟参数

- `SET_PARAM_LOW_LATENCY{slice_count,racing_display}`（`vdec_vcp_if.c:1869-1877`）、
  每帧 `vsi->dynamic_low_latency`（:1565）、DECODE_MODE/NO_REORDER/TRICK_MODE/
  WAIT_KEY_FRAME（`vdec_ipi_msg.h:187-224`）。
- 单帧 = INIT → DEC_START → DRAIN_EOS；reset 类型枚举：bs=NULL 时 fb=NULL→FLUSH(0)、
  fb->status==0→DRAIN(1)、否则 DRAIN_EOS(2)（`vdec_vcp_if.c:1517-1524`，
  `vdec_ipi_msg.h:124-128`）。tile/grid 语义在固件内，KE 无 single-frame 专用模式。

## 3. X9 现状对照（观察）

| 层 | 已有（可编译，在 recipe） | 缺口 |
| --- | --- | --- |
| 前端 ABI | `sweet_x9_vdec.c`：/dev/x9-vdec clone、GET_STATUS/CAPS、DECODE_IDR、会话 CREATE/SUBMIT/RECEIVE/PEEK/ACK/DRAIN/DESTROY；严格校验、copyin/out、无用户 DMA | 无真实 provider 注册 caller |
| IPI wire | `x9_vcp_wire.c/.h`：全消息 ABI、ring、输出 record/extent、capture1 门 | c006 free ENOTSUP；BS/FB 排他消费未闭 |
| 传输 | `x9_vcp_bus.c`：rv_v2 mailbox、8 阶段、epoch、service pump、实例路由（32） | **owner 回调未绑定**（guard/halt_and_drain/resources_retired） |
| 控制 | `x9_vdec_control.inc`：INIT/QUERY/START/输出资源、thread-call worker、hardware 回调合同 | clock/wait/qualify_start 回调无真实实现者 |
| DMA/内存 | `x9_vdec_dma.inc`/`x9_vdec_memory.inc` arena、NC slot | C005 真实分配后端、逐 buffer 退休 |
| SMMU | `x9_mm_smmu.c`+`x9_mm_smmu_owner.c`：SMMUv3、installed-storage adopt、STE BBM | **cold NS 授权未闭环**（pKVM EL2 问题，§6） |
| MM 电源 | `x9_mm_power.c`：secure GET/PUT + HWV 校验 + pin | 无自动 GET；VCP 域实际投票者待证 |
| shim | `vcp_handoff.c`：LK FDT 拓扑 128B 传递 | 固件 residency 未探测 |
| 用户态 | `libx9-hevc.dylib`：VT 解码器注册（硬件门控）、HEIF 解析/合成/色彩、vdecctl；内核 opt-in 安装（`x9_hevc_install.inc`，x9_hevc=1 + PID1 + autoload） | Tile v3 未实现；CCV 输出序消费者 ENOTSUP；ImageIO 自动入口未接 |
| 固件原件 | `vcp_b.img` → TCM 184320B + DRAM 2057906B（base 0x200000），`vcp_artifacts.py` 提取/SHA 门 | 未证明当前 boot 已 residency |

## 4. 总体架构

```
iOS 框架（未修改）
  VideoToolbox / ImageIO / CMPhoto
        │  VTRegisterVideoDecoderWithInfo（视频，已实现）
        │  Tile v3 / 自有 HEIF 入口（静态图，§8）
  libx9-hevc.dylib（自有，签名，内核嵌入安装）
        │  ioctl /dev/x9-vdec（独立 ABI，非 AppleVXD）
  ─────────────────────────────────────────────
  XNU 内核
  sweet_x9_vdec 前端（校验/会话/copyout）
        │  x9_vdec_platform + session_ops（§7 绑定）
  x9_vdec_control（INIT/START/输出 journal）
        │
  x9_vcp_bus（mailbox 传输/epoch/phase）←── bootstrap owner（§5 新）
        │                                  │
  x9_vcp_wire（IPI ABI）                   ├─ SMC 0xC200052C op6/op3（ATF）
        │                                  ├─ DRAM_RESV GPR1/2/3
  mailbox 0x31b80000 + IRQ 593+i           ├─ 固件 residency probe/load
        │                                  └─ MM power / SMMU authority（§6）
  ─────────────────────────────────────────────
  VCP tinysys（RISC-V 双 hart：vdec_srv + MMUP）
  固件 owns：codec 时钟/复位/IRQ、DPB、ring 发布、DVFS
        │
  解码硬件（LAT/CORE） + MM-SMMU（codec 域）
```

原则（沿用现有合同，不放松）：
- 前端 READY 只来自真实 VCP READY/QUERY_CAP + 完整固件 ABI + codec 域映射 +
  一次真实完成的硬解；软件 provider 禁止注册。
- ACK 是 host delivery，不是硬件退休；逐 buffer 退休只认 list_free + cache sync。
- 任何 TIMEOUT/EIO 锁存 provider 不可用直至安全恢复；不返回 in-flight 帧。
- 不写 Apple 二进制、不改 launch plist、不绕 AMFI；安装仍走 PID1 opt-in。

## 5. 层 A：VCP 冷启动 owner（新代码，最高优先）

绑定 `x9_vcp_bus_owner` 三个回调所需的全部物理前提。序列按 Android §2.2，
参数按 X9 已捕获 handoff/capture1：

1. **Probe-first（只读，独立 boot 候选）**：
   - 读 VCP cfg 存活指标（参照 ADSP dspprobe 模式）：两核 reset/ready 状态、
     TCM 首字（固件向量）、DRAM reserved 区首字（RV55 payload magic）、
     GPR1/2/3 当前值、mbox 状态。不投票、不 SMC、不 reset。
   - 判定三种状态：(a) 固件在跑/已 READY（LK 已启动）→ 只取得所有权证明后接管；
     (b) 固件已装但核在 reset → 直接走 step 3 释放；(c) 未装 → step 2。
   - **观察前不写任何寄存器**；probe 结果决定后续，不默认 LK 已装。
2. **固件装载（仅状态 c）**：
   - DRAM payload：从内核嵌入的 `vcp_b` 提取件（SHA 门 `200294bc…`，
     `vcp_artifacts.py` 既有）复制到 handoff 指定的 reserved DRAM 区
     （普通 WB 内存，AP 可写；边界由 `x9_vcp_global_layout.h` capture1 门）。
   - TCM：优先验证「核在 reset 时 AP 直写 TCM sram」可行性（只读探测 TCM
     可访问性后再定）；若 AP 不可写，走 ATF（tinysys SMC 是否有 load op 需 ATF
     原件取证，rv_v2 内核侧无此 op——**不得猜 op 号**）。
   - logger/shared 全局区按 capture1 布局一次性分配并保留（bus 合同：一个完整
     logger extent，MMUP 收起点、VCP 收终点，不得 alias 第二个 logger）。
3. **复位释放（每核一次）**：
   - MM power：`x9_mm_power_acquire(VCP)` + HWV validate（已有）；
     VCP 域投票者身份需 probe 证实（LK 遗留 vs 内核自投）。
   - 写 MMUP GPR1/2/3（packed IOVA 格式按 `VCP_PACK_IOVA` 源码核实位布局后实现）
     → SMC 0xC200052C op6 arg2=1 → 等 IPI_IN_VCP_READY_1（有界，参照 Android
     VCP_READY_TIMEOUT；超时即 quarantine，不重试 reset）。
   - 写 VCP GPR1/2/3（logger 尾界）→ SMC op3 → 等 READY_0。
   - 校验固件上报 TCM size == 184320（READY IPI payload）。
   - SMC a0 非零/超时：保留 journal、锁存本 boot 不可用（参照 mm_power PUT 歧义
     处理），不盲重试。
4. **owner 回调绑定**：
   - `guard`：当前独占 clock/reset/IRQ+mbox 所有权 + 固件签名/residency 配对 +
     已装 SHM 映射/cache（每 cold session 一次，phase 全周期可用）。
   - `halt_and_drain`：停止两个物理生产者与全部上层 DMA/worker（VCP_SET_HALT
     等价路径需从 tinysys/ATF 取证；Android 经 `VCP_SET_HALT "vdec_srv"`，
     XNU 侧对应 SMC op2/op5 RESET_SET 或 tinysys halt IPI——**取证前不实现**）。
   - `resources_retired`：只读非等待检查，全部 epoch 相关 DMA/映射/worker 退休
     后才返回 0；两核 cfg+70 双采样（gated + 全 hart halted + AXI not busy）。
5. **成功判据**：两核 READY + TCM size 校验 + service SHM d005 消费 +
   QUERY_CAP 真实回包。此后才允许 control INIT；仍不等于 codec READY
   （还需一次真实硬解，前端合同不变）。

## 6. 层 B：DMA/SMMU 授权（关键风险层）

Android 事实（§2.4）：FB/general 走 AP `smmu_dev`（MM 共享 SMMU，34bit mask），
BS 走 VCP_IOMMU_WORK；SHM iova=pa。Android 的 MM-SMMU 由 `arm_smmu_v3.ko` +
`pkvm_smmu.ko`（EL2 protected）管理；XNU 当前 EL1 进入（321.txt `boot EL=…1`），
protected handler 是否存在于本 boot **未证**（NOTES 2026-11-16 EL2 shadow/donation
证据链）。

设计选项（按证据择一，不预设）：

- **B1 adopt-installed（现有 x9_mm_smmu_owner 路线）**：若 probe 证明本 boot 无
  EL2 protected owner（LK 未加载 pKVM/hyp），shim 在 EL2 阶段或 XNU 直接 adopt
  真实已装 strtab/cmdq/evtq，逐 16K 页 pmap 核对，只对 target SID（codec a0/b0）
  做 BBM + CFGI/CD/TLBI/SYNC，不写全局 CR0。这是现有代码的设计目标，缺的是
  「无 protected owner」的实机证据。
- **B2 shim-EL2 委托**：若 LK 已加载 pKVM 而 shim 仍短暂持有 EL2，评估在 shim
  EL2 阶段完成 codec SID 的 S2/STE 委托后再 ERET EL1。**需要 shim start.S 当前
  EL2 分支行为与 pKVM 加载时序取证**，不凭源码猜。
- **B3 固件侧 IOVA**：核实 MMUP hart 是否实际编程 codec SMMU（c005 回的 IOVA
  是否固件自管翻译）。若是，FB 也可走固件分配域，AP 侧只需提供物理页——
  可大幅绕开 NS 授权问题。**取证点**：MMUP 固件（`mmup.c`/`mmup_ipi_table.h`
  与 vcp_b MMUP payload）+ `vcp_get_io_device_ex` 返回 device 的 DT 归属。
  当前 Android 源码显示 FB 用 AP smmu_dev，故 B3 对 FB 不成立的可能性高，
  但 SW/SHM 域已确认可固件自管。

无论哪条：C005 handler 按 mem type 分域分配（64B/512B 对齐、uncached/WC 属性按
`mtk_mm-uncached` 先例 pgprot_writecombine），C006 白名单退休 + 真实 free ACK；
IOVA 全部 <36bit（wire 合同）；AP 指针 cookie 可逆查白名单（不解引用）。

## 7. 层 C：provider 绑定（session_ops → wire）

在 control/bus 之上实现 `x9_vdec_platform` + `x9_vdec_session_ops` 并注册
（首个真实 `x9_vdec_register_platform` caller）。映射：

| ops | wire 序列 | 关键约束 |
| --- | --- | --- |
| create | QUERY_CAP(a100)→INIT(a000, AP cookie)→b000 取 vsi 偏移→CHECK_CODEC(c008 反向)→SET_PARAM(decode_mode=INPUT_DRIVEN_PUT_FRM(2)、fb_num_planes AP 写 f28) | vsi 在固件保留区；INIT 在 NULL-FB header parse 之后（Android 顺序，NOTES 已证） |
| submit | BS 入 arena（64B 对齐）→写 vsi->dec（bs_dma/non_acp/fb_dma[]/fb_va/index/timestamp/dynamic_low_latency）→a001 START | submit 返回前消费/复制 host AnnexB；token 唯一；NULL-FB 时 fb_va=NULL/index=0xFF |
| receive | display ring（GET_PARAM_DISP_FRAME_BUFFER）+ b005/e000 通知→不可变 CPU-owned 帧 | display order；token=输入身份非 POC；DROPPED 也需真实 completion/cookie |
| drain | a001 bs=NULL + DRAIN_EOS(2) | 每个 accepted token 恰好返回一次；ENODATA 后可再 submit，参考状态保留 |
| destroy | DEINIT(a002)/RESET(a003) + quarantine | 先 drain/隔离再 free DMA；fault 也不泄漏 in-flight |
| release | 仅 CPU-owned 完成帧；逐 buffer 退休另走 list_free + C006 | ACK≠退休；cache sync 后才 CPU 可读 |

超时/恢复：首帧 START 60s、其余 5s（Android 先例；X9 前端 DECODE_IDR 当前
5000ms 上限需按首帧放宽或分会话阶段）；超时 → halt vdec_srv 等价路径（§5.4
取证后实现）→ provider 锁存不可用 → generation++，前端已有 ESTALE/quarantine
语义承接。

DPB：固件管理；FB 池按 DPB_SIZE（GET_PARAM）+ 显示/回收环深度分配；
list_free 出现才复用，未显示过的直接回收（Android :1298 先例）。
输出格式：本期只接受线性 NV12/P010（前端 frame_valid 已拒绝 P01H/HYFBC）；
若固件默认 tiled/压缩输出，用 SET_PARAM 协商线性（参数号从 vdec_ipi_msg.h
SET_PARAM 枚举取证，不猜）；协商不成则 provider 不注册，不做软件转换冒充。

## 8. 层 D：iOS 用户态

### 8.1 视频（已有骨架，补全）

- `vt_decoder.c` 注册 kCMVideoCodecType_HEVC 自有解码器（20H392 base v1/
  decoder v2 ABI 已静态断言），门控 GET_STATUS mode2/ready1/prereq31 +
  caps delivery。会话走 CREATE/SUBMIT/PEEK/ACK。
- 缺口：CCV149 输出序 persistence/cancel 消费者（当前 ENOTSUP；iOS16 无公开
  kCMFormatDescriptionExtension_ContentColorVolume key，NOTES 已证 SDK 17.0，
  不新增自定义 key，走 20H392 原件 typed parser 已证路径）；late CLLI/其余
  nested HDR 按既有 bitstream facts 管线。

### 8.2 HEIF 静态图：两条路线

**路线 1（先行，硬件验证载体）：自有解析 + 单帧硬解**
- `heif_items`/`heif_compositor`/`heif_native` 已实现容器解析、grid/overlay/
  transform/clap 计划、working-RGB 合成与色彩策略。
- 解码用会话单帧：CREATE（codec_id=HEIF(3)，若 QUERY_CAP 声明）或 HEVC(2)
  兜底 → SUBMIT 单 AU → DRAIN_EOS → PEEK/ACK → 合成。Android 先例：HEIF
  一等 fourcc、单帧=INIT→START→DRAIN_EOS、无需 fence、排除 low_pw 组解码。
- 低延迟参数（slice_count/racing_display）按 SET_PARAM_LOW_LATENCY 取证后设。
- 此路线不接 ImageIO 自动入口，先由 vdecctl/显式 API 验证硬件全链。

**路线 2（自动入口）：CMPhoto native Tile v3**
- 沿用 `x9/out/hevc-native-tile-plan-20261116-185351/final-full-native-tile-v3-production-plan-v2.json`
  的 A–H 阶段与 5 个生产入口门，不重复设计：v3 class ≥80B（+40 start_tile/
  +48 decode_tile）、双 token ledger、preSUBMIT typed P1 解析、同 native DPB、
  immutable PEEK→caller target 填充、nativeTileEmit、FinishDelayedTiles/
  Invalidate、CMPhoto/ImageIO 权威验收。
- 已闭静态合同（P1 CMSampleBufferCreate、P4 CVPixelBufferCreate 双 producer、
  Emit/callback176、retain/release owner R1–R6、Canvas 业务消费 v3）按原样复用；
  未闭原语（P3/P5/P6 语义、target layout、FinishDelayedTiles dispatch、
  context+28 业务 callback producer）仍按原 exact-proposal/审批流程取证，
  不在本设计里猜语义。
- 路线 2 的硬件前提与路线 1 完全相同（层 A–C）；硬件未 READY 前不做 v3 bump/stub。

### 8.3 安装与门控（不变）

- 内核嵌入签名 dylib（2MiB 预算）→ PID1 + x9_hevc=1 + ufs root 可写 +
  trusted directory → `/private/var/x9/hevc/libx9-hevc-<sha16>.dylib`；
  exec autoload 仍需独立 GET_STATUS/CAPS 复核。不写 Apple 原件。

## 9. 分阶段计划与验收

| 阶段 | 内容 | 验收（观察，非推断） |
| --- | --- | --- |
| P0 | VCP residency 只读 probe boot 候选（§5.1）+ SMMU/pKVM 现状 probe | 日志给出三态判定与 SMMU owner 证据；不写寄存器 |
| P1 | 冷启动 owner（§5）+ halt 取证 | 两核 READY、TCM size 校验、d005 消费、QUERY_CAP 回包；guard/halt/retired 全绑定 |
| P2 | C005/C006 后端 + 单帧 DECODE_IDR（§7） | 一次真实硬解：非零 completion、frame_valid 通过、像素 copyout；START-only 拒绝复验 |
| P3 | 连续会话：BS/FB 排他、durable token journal、DPB/list_free 退休、drain | 多帧 display-order、token 恰好一次、DRAIN_EOS 后再 submit、close/destroy 无泄漏 |
| P4 | VT 视频全链（§8.1）+ CCV 输出序 | 真实播放器/VT 客户端硬解播放；CCV 消费者按原件路径 |
| P5 | HEIF 路线 1 全链（§8.2） | 真实 HEIF（grid/alpha/10bit）经硬解合成输出 |
| P6 | Tile v3 A–H（§8.2 路线 2） | CMPhoto/ImageIO 自动选 native v3 → 硬解 → caller P4 填充 → 最终显示 |

每阶段独立 boot 候选、用户手动 fastboot boot；只有全部代码齐全后才做最终整合
boot（沿用 goal 约定）。P0 之前不交付任何「部分初始化」镜像。

## 10. 风险与开放问题（待证，不作为已完成前提）

1. **固件 residency**：LK 是否预装 VCP TCM/DRAM（ADSP 先例不利）。P0 判定。
2. **TCM 装载路径**：若未装，AP 直写 TCM 可行性 / ATF load op 存在性。无 op 取证前不猜。
3. **SMMU NS 授权**：pKVM 是否在本 boot 加载；B1/B2/B3 择一（§6）。最高风险。
4. **halt/恢复路径**：VCP_SET_HALT 的 XNU 等价（SMC op2/op5 vs tinysys IPI）需 ATF/tinysys 取证。
5. **线性输出协商**：固件是否支持线性 NV12/P010 SET_PARAM；不支持则需格式转换层或拒绝注册。
6. **首帧 60s**：前端 deadline 上限与 boot-arg 预算；会话阶段化 deadline。
7. **VCP 域电源投票者**：LK 遗留 HWV 位 vs 内核自投（ADSP TOP/INFRA 先例）。
8. **Apple 侧未闭原语**（Tile P3/P5/P6、FinishDelayedTiles、CCV 消费者）：
   沿用既有 exact-proposal/库存审批流程，与本设计硬件层并行不阻塞 P0–P3。

## 11. 证据索引

- Android：`D/media/platform/mtk-vcodec/`（vdec_drv_if.c、vdec/vdec_vcp_if.c、
  vdec_ipi_msg.h、mtk_vcodec_dec.c、mtk_vcodec_dec_drv.c、mtk_vcodec_util.c、
  mtk_vcodec_dec_pm.c、mtk_vcodec_intr.c）；`D/misc/mediatek/vcp/rv_v2/`
  （vcp_helper.c/.h、vcp_reg.h、vcp_status.c、mmup.c）；
  `include/linux/soc/mediatek/mtk_sip_svc.h`；mt6993.dts。
- X9 内核：`x9/kernel/src/bsd/dev/{sweet_x9_vdec.c,x9_vdec.h,x9_vdec_control.*,
  x9_vcp_bus.*,x9_vcp_wire.*,x9_vcp_handoff.h,x9_vcp_reset_reply.inc,
  x9_mm_power.*,x9_mm_smmu*.*}`；shim `vcp_handoff.c`。
- X9 用户态：`x9/hevc/`（vt_decoder.c、heif_native.c、heif_items.c、
  heif_compositor.c、vdec_sessions.inc、build.py）；`x9/kernel/src/bsd/kern/x9_hevc_install.inc`。
- 固件原件（gitignored）：`.local/x9/backups/partitions/firmware-originals/vcp-real-android-active-b/`；
  提取工具 `x9/tools/vcp_artifacts.py`（TCM/DRAM SHA 门）。
- 已保存合同：`x9/out/hevc-native-tile-plan-20261116-185351/`（Tile v3 plan v2）、
  `x9/out/heif-business-callback-20261116-191331/`（Canvas v3）、
  `x9/out/hevc-tile-retain-20261116-191331/`（retain/release R1–R6）等，
  按各 final-*.json 原样消费，不重采。