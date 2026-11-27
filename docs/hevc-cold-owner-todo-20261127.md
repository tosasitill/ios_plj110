# HEVC 层 A 冷启动 owner：证据缺口与激活清单（2026-11-27）

随 `feat/hevc-restore` 恢复后整理。设计合同见 `docs/hevc-ios-bridge.md` §5，
总线合同见 `x9/kernel/src/bsd/dev/x9_vcp_bus.h` 文件头。骨架代码在
`x9/kernel/src/bsd/dev/x9_vcp_cold_owner.{c,h}`：三个回调签名与总线合同
一致（已用正式 bsd recipe 命令行编译校验，exit 0），全部 fail-closed
返回 `ENOTSUP`，**未进 recipe/link-order、无任何 caller**——总线头明确
禁止 default provider / success stub，骨架不得伪装成已绑定。

## 已闭合（骨架可以依赖的事实）

- **复位释放 SMC 是总线原生代码，不是 owner 的活**：总线头写明 native
  code 发布 cfg+44/48/4c 并执行真实 reset SMC（MMUP cmd6/READY 后 VCP
  cmd3/READY，每核一次）。Android 侧序列（MMUP GPR1/2/3 → SMC
  0xC200052C op6 → READY_1 → VCP GPR → op3 → READY_0 → TCM size
  == 184320 校验）与 X9 已捕获 cmd3/cmd6、op 枚举（vcp_helper.h）逐项
  吻合（恢复的分支 NOTES 2026-11-20 条）。
- **MM power pin/validate API 已在树内**：`x9_mm_power.{c,h}`
  （acquire/validate_pin/unpin，VCP 域）。总线在调用 guard/halt 前后
  自行 validate/unpin。但总线头同时写明：MM power 与 FDT 拓扑**不能
  替代** owner 的物理所有权证明。
- **residency 分类器已有**：P0 只读 probe（`x9_vcp_probe.c`）按 DRAM
  签名 + 双核 halt 位 + MM_INFRA 门给出 A（运行中）/B（已装已 halt）/
  C（未装）/UNKNOWN 四态，零写零 SMC。候选
  `x9/out/hevc-p0-restore-20261127/boot.img`（`x9_vcp_probe=20`，配对
  已 stage 的 287 mailbox allowlist 构建）已按当前 main 重建，**未上机**。
- **shim 拓扑资格已是有界实现**：`fdt_vcp_patch_adt` 的 layout 校验改为
  一次 `fdt_collect`（看门狗 kick + 2000 ms 预算），handoff v4 记录的
  产生路径不再卡 pre-XNU shim（2026-11-23 回滚修复，见 shim 提交）。

## 逐回调缺口

### guard（缺：所有权取得路径 + 实机 residency 输入）
合同：证明本 cold session 独占 clock/reset/IRQ+mailbox 所有权、固件
签名/residency 配对、已装 SHM 映射/cache。
- 树内**不存在** VCP/MMUP 的 clock/reset/IRQ 所有权取得代码（谁投票、
  谁持有 reset 线、IRQ 归谁 ack 均无 XNU 侧实现）；`x9_mm_power` 只
  覆盖 MM power 投票，总线头明言不够。
- residency 配对是 per-boot 事实：取决于 LK 本 boot 是否已装/已启
  VCP，须 P0 probe 上机分类后才知道走 §5 的 (a)/(b)/(c) 哪条路；
  probe 从未上机。
- SHM 账本（shared/logger extent 的分配、映射、cache 同步记录）只在
  真 owner 于 bind 前安装资源时才存在；现无分配者。

### halt_and_drain（缺：halt 语义本身，设计文档明令取证前不实现）
合同：停两物理生产者与全部上层 DMA/worker，且返回后保持 halted。
- 设计文档 §5.4 原文：VCP_SET_HALT 等价路径「需从 tinysys/ATF 取证；
  Android 经 VCP_SET_HALT "vdec_srv"，XNU 侧对应 SMC op2/op5
  RESET_SET 或 tinysys halt IPI——**取证前不实现**」。
- 已有反证：Android mmdvfs 的完整 pm_disable 只减 count/归零/print/
  return 0，无 IRQ-disable/halt/clock-clear（恢复 NOTES 2026-11-16
  条），不能充当 drain。
- reset SMC（op3/op6）是**释放**不是 halt，禁止拿它冒充。

### resources_retired（缺：owner 资源账本）
合同：只读非等待，全部 epoch 相关 DMA/映射/worker 退休才返回 0。
- 总线自带 cfg+70 双采样（gated + 全 hart halted + AXI not busy），
  但总线头明言这**不证明**上层 DMA/worker 排空；owner 的一半是自己
  的资源账本，随 guard/bind 一起才有意义。账本不存在时返回 0 会在
  未来任何部分实现装入资源后变成假证明，故 fail-closed。

### 冷启动序列其余未闭合点（非回调，但挡在 guard 之前）
- **TCM 装载**：状态 (c) 下 AP 直写 TCM sram 的可行性未证（需只读
  探测先行）；不可写时的 ATF load op **无证据、不得猜 op 号**
  （rv_v2 内核侧无此 op）。
- **VCP 域投票者身份**：LK 遗留 vs 内核自投，须 probe 证实。
- **GPR packed IOVA 位布局**：写 MMUP/VCP GPR1/2/3 前须按
  VCP_PACK_IOVA 源码逐位核实（总线 bind 发布 cfg+44/48/4c 的部分已
  在总线内，owner 侧 logger 首/尾 GPR 值来自 bind 的真实 extent，
  不得用捕获值冒充）。

## 激活清单（按序，前一步证据不到位不进下一步）

1. P0 probe 上机（候选已备）：读 `X9 VCP PROBE` 两次报告，定 A/B/C。
2. 按状态取证：(a) 所有权接管证明形状；(b) 直接释放路径复核；
   (c) DRAM 复制（SHA 门已有）+ TCM 可写性只读探测 / ATF load op 取证。
3. 实现 clock/reset/IRQ 所有权取得 + SHM 账本，guard 才允许返回 0。
4. tinysys/ATF halt 取证到手后才实现 halt_and_drain；账本随之实现
   resources_retired。
5. 接线（recipe/link-order + create caller）必须与真实实现同一提交，
   且保持 opt-in（参照 `x9_hevc=1` 门），不得默认启用。
6. 成功判据（设计文档 §5.5）：两核 READY + TCM size 校验 + d005
   消费 + QUERY_CAP 真实回包；仍不等于 codec READY（需一次真实硬解）。
