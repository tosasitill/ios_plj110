# X9 CPU 下线 / 深度空闲（MT6993）

把 MT6993 的 CPU 热下线链从固件接口到调度协同补齐：XNU 原生
`processor_exit/processor_start` 语义 + PSCI CPU_OFF / CPU_SUSPEND 真断电，
由场景策略（x9sched）驱动。结论标注：**实证** = 本机原厂源码 / 冻结 XNU
源码 / 本树代码可直接核对；**待探测** = 无设备无法钉死，代码已留探测
路径，上机第一步按清单执行。

配套：`x9/sched/README.md`（调度诊断与 x9sched 策略 v2）。

## 1. 固件接口证据（PSCI）

- 本机 DTS：`psci { compatible = "arm,psci-1.0"; method = "smc"; }`，
  cpu 节点 `enable-method = "psci"`
  （`.local/references/oppo-modules-dt-mt6993/.../mt6993.dts:464-467,260`）。
  即 EL3 固件（MediaTek ATF）提供 PSCI 1.0，SMC 调用约定。【实证】
- 功能号（PSCI 0.2/1.0 标准编码；CPU_ON / SYSTEM_RESET 已在本机实证可用）：
  | 功能 | FID (SMC64) | 树内现状 |
  | --- | --- | --- |
  | PSCI_VERSION | 0x84000000 | 未用，本轮加只读探测 |
  | CPU_SUSPEND | 0xC4000001 | 未用，本轮加 |
  | CPU_OFF | 0x84000002 | 未用，本轮加 |
  | CPU_ON | 0xC4000003 | `SweetCPU.cpp:sweet_psci_cpu_on`，开机拉核在用【实证】 |
  | AFFINITY_INFO | 0xC4000004 | 未用，本轮加（确认下线/上线状态） |
  | PSCI_FEATURES | 0x8400000A | 未用，本轮加只读探测 |
  | SYSTEM_RESET | 0x84000009 | shim / sweet_early 在用【实证】 |
- 电源状态参数（`arm,psci-suspend-param`，同 DTS idle-states 节点，
  编码 = PSCI 原始格式：[15:0] StateID、bit16 = powerdown、[25:24] =
  最高受影响 affinity level）：
  | 状态 | 参数 | entry/exit 延迟 (us) | min-residency (us) |
  | --- | --- | --- | --- |
  | cpuoff（各级） | 0x00010000 | 87-97 / 178-189 | 4291-9518 |
  | clusteroff | 0x01010001 | 250 / 329-361 | 4996-9518 |
  | mcusysoff | 0x02010007 | 1440-1472 / 1821-1930 | 4996-9518 |
  | system_vcore | 0x020100ff | 940 / 3500 | 35200 |
  | s2idle | 0x020180ff | 10000 / 10000 | - |
  本轮只用 cpuoff 一级：热下线的粒度是单核，cluster/mcusys 的关断由
  ATF 按最后下线核的 affinity 自然聚合（与 Linux 相同：hotplug 只发
  CPU_OFF，cpuidle 的 domain 状态由 PSCI domain 框架管理，本移植无
  对应物，不冒充）。【实证：参数与延迟来自本机 DTS】
- Linux 侧用法对照（`.local/references/oppo-kernel-mt6993`）：
  - 热下线 `cpu_psci_cpu_die`（arch/arm64/kernel/psci.c:68-77）：CPU_OFF
    的 state 参数 = `1 << PSCI_0_2_POWER_STATE_TYPE_SHIFT` = **0x00010000**，
    与 DTS cpuoff 参数逐字一致；注释明言"没有实现真正使用该字段，给个
    合理默认值"。本轮 CPU_OFF 传同一值。
  - 下线确认 `cpu_psci_cpu_kill`：轮询 AFFINITY_INFO 直到目标 OFF
    （约 100 ms 重试窗）。本轮管理器用同一手法（超时放宽到秒级并记录）。
  - cpuidle `psci_cpu_suspend_enter`（drivers/firmware/psci/psci.c:481）：
    powerdown 态走 `cpu_suspend(state, finisher)`——内核先把 CPU 上下文
    存入内存，唤醒点是 `pa_cpu_resume` 原地恢复。**XNU 热下线模型没有
    原地恢复**：下线核的上线必然经 CPU_ON + reset vector 全量重启
    （见 §2），所以本轮的 CPU_SUSPEND 深度与 CPU_OFF 在 XNU 侧走同一条
    上线路径，差别只在 ATF 侧的电源域簿记与唤醒语义（见 §5 待探测）。
  - core_ctl 默认 `min_cpus = {4, 0, 0}`（modules core_ctl.c:168）：
    小核簇 4 核恒在线，大核簇与超大核可按负载排空到 0。本轮策略的
    簇下限以此为参照（默认更保守，见 §4）。

## 2. XNU 侧现状盘点（冻结 XNU 8796.141.3 源码 `.local/dxb/xnu`）

下线链（全部为冻结 osfmk 既有语义，本轮不改其流程，只补平台末端）：

1. `processor_exit()` → `processor_shutdown()`（kern/machine.c:275）：
   `sched_mark_processor_offline` 由调度器迁走线程并摘除 processor；
2. `processor_doshutdown()`（machine.c:397）：调用线程绑到目标核，
   `ml_cpu_down()`（arm64/machine_routines.c:702）把该核 rtclock 定时器
   队列迁到 boot 核并 `cpu_signal_handler_internal(TRUE)` 关信号，
   pset 状态置 PENDING_OFFLINE、运行队列 shutdown，随后 `thread_run`
   切到目标核的 idle 线程（调用线程就此返回，不随核下线）；
3. idle 线程 `processor_offline()` → `machine_processor_shutdown()` →
   `processor_offline_intstack()`（machine.c:575）→ `cpu_sleep()`
   （arm64/cpu.c:310）：置 SleepState、`mt_cpu_down`、`CleanPoC_Dcache`，
   非 AppleARMSMP 构建直接 `PE_cpu_machine_quiesce()` →
   `IOCPU::quiesceCPU()` —— **`SweetCPU::quiesceCPU` 是 panic 桩，这是
   下线链的末端断点**。cpu_sleep 返回即 "zombie processor" panic，
   所以 quiesceCPU 成功路径不得返回。

上线链：`processor_start()` → `processor_startup()`（kern/processor.c）
→ `cpu_start()` → `PE_cpu_start()` → `IOCPU::startCPU()`（SweetCPU 已
实现 PSCI CPU_ON，开机在用）→ reset vector（prebuilt start.o 的
`sweet_x9_secondary_entry`，与开机同一入口）→ 冻结 `cpu_init()` →
`cpu_machine_init()`（arm/cpu_common.c:681，bootb = 该核 StartedState
标志未置位）→ `PE_cpu_machine_init()` → `IOCPU::initCPU(bootb)`：
开机为 true，**热上线为 false —— `SweetCPU::initCPU` 对 !boot 直接
panic，这是上线链的末端断点**；随后 load_context 恢复下线时保存的
idle 线程上下文，核回到调度。

现存的三个阻断点（均为有意防护/未实现桩，非损坏）：

- **B1 `ml_cpu_can_exit` 恒假**（arm/cpu_common.c:847）：放行分支在
  `#if HAS_CLUSTER && USE_APPLEARMSMP` 内；本构建走 legacy IOCPU
  （USE_APPLEARMSMP 未定义，IOCPU.cpp 编 !USE_APPLEARMSMP 分支），
  除系统睡眠 quiesce 外一律 return false → `processor_exit` 恒
  KERN_NOT_SUPPORTED。修复方式：按 overlay 既有惯例（pmap.o、io_map.o
  等）以 overlay 对象替换 cpu_common.o，仅给该函数加门控放行
  （非 boot 核 + 本功能门控开），其余逐字不变。
- **B2 `SweetCPU::quiesceCPU/haltCPU` panic 桩**：haltCPU 是系统睡眠
  （IOCPUSleepKernel）路径，本轮不动、保留 panic；只实现 quiesceCPU
  （运行时下线路径）。
- **B3 `SweetCPU::startCPU` 的重启守卫**：`initialized != 0` 即 panic。
  quiesce 时先清 initialized，下线核的重启即合法；仍在线的核走
  startCPU 维持 panic 不变。

其他已核对项：

- `cpu_exit_wait`（cpu_common.c:638）在本配置下是空转：cpu_sleep_token
  在 cpu_data_init 即置 ARM_CPU_ON_SLEEP_PATH 且非 AppleARMSMP 的
  cpu_sleep 不清它。所以 `processor_exit` 返回成功 **不等于** 核已断
  电；管理器必须以 AFFINITY_INFO 轮询确认 OFF/ON_PENDING→OFF，
  这与 Linux cpu_kill 的做法一致。【实证】
- GIC：per-CPU 接口（redistributor/CPU interface）由冻结 cpu_init /
  ml_init_interrupt 在上线路径重初始化（与开机同一路径）；SPI 的
  IROUTER 亲和在本移植里由各驱动按需单独编程（如 ADSP 音频 IRQ 钉
  当前核），无中央枚举点。v1 策略只下线大核/超大核（与原厂 core_ctl
  排空集合相同），cpu0 与小核簇不动；SPI 亲和全量审计列入上机清单。
- 中断/定时器：目标核的 rtclock 队列由 ml_cpu_down 迁走（上）；per-CPU
  架构定时器由上线路径的 cpu_init 重建（开机同路径）。SGI 发送目标
  检查 `sweet_cpu_signal_is_enabled` 读 cpu_signal 的 SIGPdisabled，
  下线流程经 cpu_signal_handler_internal 关信号，调度器不会向已摘除
  的 processor 派活。【实证：冻结源码】

## 3. 设计

### 门控（boot arg `x9cpuoff`，默认 0 = 一切旧行为逐字不变）

| bit | 值 | 含义 |
| --- | --- | --- |
| 0 | 1 | 设施开：PSCI 探测、quiesceCPU/initCPU(热上线)/startCPU 重启实现生效；策略不开 |
| 1 | 2 | 自动策略开（场景 + 负载驱动，需 bit0） |
| 2 | 4 | 允许下线超大核 cpu7（默认策略保留 cpu7 在线） |
| 3 | 8 | 允许大核簇排空到 0（默认保留至少 1 颗大核在线） |
| 4 | 16 | 深度 = CPU_SUSPEND（实验，见 §5）而非 CPU_OFF |

bit0 关时：quiesceCPU/haltCPU/initCPU(!boot) 的 panic 与 startCPU
守卫与现版逐字一致；ml_cpu_can_exit 对本功能的放行也不生效。

### per-CPU 状态机（SweetCPU.cpp 持有，管理器经导出函数读）

ONLINE → QUIESCING（quiesceCPU 进入时置位，清 initialized）→ OFF /
SUSPENDED（SMC 成功不返回，由管理器 AFFINITY_INFO 确认）→ ONLINING
（startCPU 发 CPU_ON）→ ONLINE（initCPU 置 initialized 后由管理器
确认）。失败分支：quiesce 的 SMC 返回错误 → 有限重试 → 仍失败则
置 PARKED 并在目标核上 WFI 永驻（绝不返回，避免 zombie panic；
该核本启动周期内视为丢失，管理器拉黑并记录）；上线 CPU_ON 返回
ALREADY_ON 且深度为 suspend 时，按 §5 的待探测路径发 SGI 唤醒并
等待 initialized，超时置 PARKED-UNKNOWN 并拉黑该核的上线重试。

硬性安全线：
- cpu0（boot 核）永不下线：策略层与 quiesceCPU 层双重拒绝。
- 小核簇 cpu0-3 永不下线（原厂 min_cpus[0]=4/4 同口径）。
- 同一时刻只允许一个核在迁移中（管理器串行化）。
- 系统睡眠 quiesce 期间（ml_is_quiescing）管理器不动作；
  processor_exit 本身在 sched_is_in_sleep 时也会拒绝非 SYSTEM 原因。
- 下线失败回滚：processor_exit 失败 = 核仍在线，管理器仅记账；
  quiesce 失败 = PARKED（核已从调度摘除，硬件停在 WFI，系统以
  剩余核继续运行，日志明示本周期不可恢复）。

### 策略（纯头 `x9_cpuoff_policy.h`，主机可测）

输入：x9sched 场景、大核域峰值衰减 util、当前时间、各核状态与驻留。
规则（默认常量，单位 ms）：
- 场景 IDLE（灭屏）持续 ≥ 10000：按序下线候选（cpu7 若 bit2 → cpu6 →
  cpu5 → cpu4，bit3 未开时 cpu4 保留），每次迁移间隔 ≥ 2000。
- 场景离开 IDLE：立即按逆序上线全部策略下线核（响应优先，不受
  最短下线驻留约束）。
- 场景 LIGHT 且大核 util < 10% 持续 ≥ 20000：下线大核至下限
  （同上序，不含 cpu7）；util ≥ 30% 或场景 ≥ INTERACT：上线全部
  策略下线核。
- 防抖：任意上线后 30000 内不再下线（最短在线驻留）；负载原因的
  上线要求下线已 ≥ 10000（场景原因不受此限）。
- 每核失败退避：迁移确认超时/失败后 60000 内不重试该核；连续
  3 次失败拉黑至重启。

### 可观测性

- 开机（bit0 开）：`X9 CPUOFF: psci version=… features cpu_off=…
  cpu_suspend=… aff0 cpuN=…`（AFFINITY_INFO 全核快照）。
- 每次迁移：`X9 CPUOFF: cpuN online->quiescing depth=off` /
  `… quiesce psci rc=…`（失败时）/ `… off confirmed after Xms` /
  `… onlining cpu_on rc=…` / `… online after Xms` / 失败与拉黑行。
- 周期统计并入 governor 既有日志节奏：`X9 CPUOFF STAT: off_mask=…
  transitions=… failures=… off_ms{cpu4..7}=…`。

## 4. 上机清单（摘要，完整版见实现完成后的更新）

1. 只读探测：`x9cpuoff=1` 开机，核对 PSCI_VERSION ≥ 1.0、
   FEATURES(CPU_OFF/CPU_SUSPEND) 返回 0、AFFINITY_INFO 与 8 核在线
   一致；行为与旧版一致（策略未开）。
2. `x9cpuoff=3`：灭屏 30 s 后大核应逐颗 off（日志 + AFFINITY_INFO
   确认），亮屏后全量恢复；反复亮灭屏 20 次无丢失、无 panic。
3. 待机功耗/温度：同候选 `x9cpuoff=0` vs `=3`（及 `=7/11` 放开
   cpu7/排空）灭屏 30 min 对比整机电流与 SoC 温度曲线。
4. 唤醒可靠性：下线状态下闹钟、来电（基带中断）、USB 插拔、触摸
   唤醒各 ≥ 10 次；下线核上线延迟以下线日志时间戳计（预期量级：
   CPU_ON 路径 ≈ 开机单核拉起时间，远大于 DTS cpuidle exit 延迟，
   这是热下线而非 cpuidle 的固有成本，文档不混淆两者）。
5. suspend 深度（bit4）：先单核（只允许 cpu6 一颗）验证 CPU_ON
   返回值与 SGI 唤醒哪条成立，再放开。

## 5. 待上机探测（代码已留路径，不冒充已钉死）

- MT6993 ATF 对已 suspend（CPU_SUSPEND powerdown）核发 CPU_ON 的
  返回：PSCI 规范无此唤醒路径，ATF 实现可能 ALREADY_ON / DENIED /
  接受并唤醒。上线逻辑对三种返回都有分支日志，以实测为准收敛。
- CPU_SUSPEND 传 cpuoff 参数 0x00010000 时 ATF 是否接受来自非
  cpuidle 上下文（无 domain 框架）的调用：FEATURES 只证明功能号
  存在，不证明状态参数组合；首次 suspend 尝试只对一颗大核开放，
  失败即回退 CPU_OFF 并拉黑 suspend 深度（本周期）。
- ATF 的 AFFINITY_INFO 在核 suspend 时返回 ON 还是 OFF 变体：
  探测日志会记录，状态机按实测解释。
