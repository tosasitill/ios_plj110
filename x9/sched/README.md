# X9 调度与功耗策略（MT6993）

现状诊断、OPPO 原厂对照与本分支落地的策略 v2。结论标注：**实证** = 代码/原厂
源码可直接核对；**推断** = 由结构推导，未上机测量。性能与温升效果一律以
上机验证清单的数据为准，本文件不声称已提升。

## 1. 现状诊断

### 拓扑与调度器

- MT6993（天玑 9500）：cpu0-3 = C1-Pro（capacity-dmips-mhz 793，调频域 0）、
  cpu4-6 = C1-Premium（952，域 1）、cpu7 = C1-Ultra（1024，域 2）；一颗 DSU，
  MPIDR Aff1 = 核序号。来源：OPPO `mt6993.dts` cpu 节点
  （performance-domains 0/1/2）与实测 MPIDR。【实证】
- ADT 把 8 核压成一个调度簇（`x9/tools/mkadt.py`：cluster-id/type 全 0，
  真实分簇只以 `x9-cluster` 私有属性发布）：XNU 按 `MAX_CPU_CLUSTERS=1`、
  无 `__AMP__` 构建，第二个 ADT 簇会越界其簇数组。后果是 XNU 调度器对三档
  核心完全无感知：线程放置不分轻重、没有 capacity 概念、QoS 只在单簇内
  排序。【实证：mkadt.py、`ml_parse_cpu_topology`、`cpu_data_init` 的
  CLUSTER_TYPE_SMP 默认】
- 8 核开机全在线且永不下线：`SweetCPU::quiesceCPU/haltCPU` 直接 panic，
  resume 未实现，树内无 PSCI CPU_SUSPEND 调用点。原厂 dts 有
  cpuoff/clusteroff/mcusysoff 深度空闲态，本移植只能靠 WFI 门控。【实证】

### 频率（governor v1，已在树内）

- CPU 调频不经 XNU cpufreq 框架（`bsd/dev/sweet_cpufreq.c` 是 SM6225 旧物），
  由 `sweet_x9_gpu_dependencies.c` 的 governor 线程独占：FDVFS 写频率请求
  （MCUPM 管电压），50 ms 周期（灭屏空闲 1000 ms），`x9g` bit32 门控，
  默认开（x9g 默认 61）。
- v1 决策：每域取域内最忙 CPU 的窗口利用率（XNU 累计 tick，非频率不变），
  wanted = 当前频率 × util/80，OPP 取整，降频保持 100 ms；触摸只给小核域
  1300 MHz 下限（与原厂 touch_boost 默认同值）；温度取 GPUEB 共享状态，
  ≥85°C 才收敛（85–95 线性到 idle OPP），温度未知时 cap = 开机频率。
- GPU governor 同线程：util/80、活动下限 900 MHz、上限 `x9_gpu_khz`
  （设备配置 1716000）；DDR 由 DVFSRC 在 `ug=5` 下 INIT 锁定高档，
  `x9_ddr_level=9` 另行投票。
- 放置干预只有两处且默认关：bit64 推荐核掩码（触摸+高负载时推荐
  cpu0+大核+prime）、bit128 把 swap 线程绑 prime（x9g 默认 61 不含二者）。

### 卡 / 烫归因

卡：

1. 【实证】调度器无簇感知（上）：交互/渲染线程可能落在 C1-Pro 低频上，
   或与后台任务挤在大核上；默认连 bit64/128 干预都没开。
2. 【实证】调频反应慢且会振荡：50 ms 窗口 + 从当前频率按比例爬升，突发
   负载要多拍才到顶频；util 非频率不变，同一任务低频虚高、高频虚低。
3. 【推断】无场景区分：相机预览、游戏等持续负载与普通滑动共用一条 util
   曲线和 80% 目标，没有帧驱动的大核下限（触摸只抬小核）。

烫：

4. 【实证】热收敛太晚：85°C 前不限制顶频，撞墙后才急刹；温度未知时
   cap = 开机频率而非保守值。
5. 【实证】无 CPU 下线/深度空闲：8 核常驻在线，灭屏只能降到 idle OPP。
6. 【推断】持续负载下 GPU 1716 MHz 上限、DDR 高档与 CPU 顶频可同时
   成立，CPU/GPU 之间没有场景化的功耗让行（只共用温度 cap）。

## 2. OPPO 原厂对照

原厂源码：`.local/references/oppo-modules-dt-mt6993/kernel/
kernel_device_modules-6.12` 与 `.local/references/oppo-kernel-mt6993`（GKI）。

| 机制 | 原厂做法 | 本项目现状 | 等价落地（本分支） |
| --- | --- | --- | --- |
| EAS + 能量模型 | 按 capacity/能耗选核，FLT 负载跟踪，VIP 任务优先（sched/eas、mtk_energy_model） | 单簇 SMP，无 capacity | 调度器内不可行（MAX_CPU_CLUSTERS=1）；governor 侧场景推荐核掩码近似（bit2）。逐线程 QoS→簇未做 |
| schedutil（MTK sugov） | PELT 频率不变 util，util/目标比，上下调速率限制，nonlinear_opp_cap 按能效选 OPP | tick 窗口 util，比例爬升，仅降频保持 | v2（bit1）：峰值衰减 util 近似 PELT；场景目标 65–80；升频立即、降频几何步进 + 100 ms 保持 |
| touch_boost | 小核 1300 MHz，100 ms 时长，150 ms 间隔（performance/touch_boost） | v1 已同值实现 | v2 保留为 INTERACT 场景小核下限 |
| FPSGO | 帧时间驱动 CPU/GPU 下限与渲染线程放置（performance/fpsgo_v3/v8） | 只有 swap 计数，无帧策略 | v2：swap 活跃 → INTERACT；CAMERA/LOAD 给大核域 50%/60% 顶频下限；GPU 在 LOAD/CAMERA 目标 80→70（bit4） |
| core_ctl | 按负载上下线核，min_cpus = {4,0,0}（大核簇可全下线） | 无下线能力（quiesce panic） | 不做：需先补 SweetCPU quiesce/PSCI suspend，属独立工作；IDLE 场景以推荐小核掩码 + idle OPP 近似 |
| PPM / 热管理 | 温度分级 cap，提前收敛（ppm_v3、thermal） | 85°C 起收敛，未知温度 = 开机频率 | v2：70°C 起线性收到 idle OPP（95°C）；未知温度维持开机频率 fail-safe（与 v1 一致） |
| cpufreq-hw / FDVFS | MCUPM 管电压，内核写频率请求 | v1 已按原厂方式实现 | 不变 |

## 3. 本分支落地（boot arg `x9sched`，默认 0 = 全部旧行为）

- bit1 CPU 策略 v2：纯策略头 `x9/kernel/src/bsd/dev/x9_sched_policy.h`
  （场景目标/下限、峰值衰减 util、70–95°C 平滑热 cap、几何降频），
  glue 为 `cpu_govern2()`，与 v1 共用 util 会计与 FDVFS 写路径。
  主机测试 `x9/sched/test_sched_policy.py`（99 项断言）。
- bit2 场景转向：场景分类（idle/light/interact/camera/load，信号 = 灭屏、
  触摸、swap 计数、GPU util、上拍 CPU 峰值）→ `sched_perfcontrol`
  推荐核掩码：idle = 小核域（0x0f），interact/camera 且温度允许 =
  cpu0+big+prime（0xf1），其余 = 全部（0xff）；interact/camera 同时打开
  bit128 的 prime 绑核窗口。调度器自有饥饿 failsafe 不受影响。
- bit4 CPU/GPU 协同：load/camera 场景 GPU governor 目标 80% → 70%。
- 相机场景经 `sweet_x9_sched_camera_active()` 提示进入；当前无调用方
  （预定接入点是 `sweet_x9_camera.c` 的会话起止，本分支未动相机文件
  以免与并行相机工作冲突），接入前 camera 场景不可达，其余场景不受
  影响。
- 可观测性：启动横幅 `X9 SCHED: x9sched=…`；周期日志 `X9 SCHED:` 行
  （当前场景、五场景驻留 tick、三域 up/down commit 数、热 cap 触发
  tick、steer 掩码），与既有 `X9 CPUFREQ:`/`X9 GPU GOV:` 同周期。
- 前置条件：依赖 x9g bit32（默认开）与 governor 就绪
  （`cpu_boost_ready`）；任一 bit 单独可关，`x9sched=0` 逐字回旧行为。

## 4. 上机验证清单

同一候选分别以 `x9sched=0` 与 `x9sched=7` 启动对照：

1. 桌面滑动 60 s：日志 scene=interact、steer_mask=0xf1；对比
   `X9 CPUFREQ:` 的 request 分布与帧时间（既有 frame 日志）。
2. 持续 GPU 负载（游戏）5 min：约 1 s 持续高负载后 scene=load；
   温度曲线对比——到达 85°C 的时间应变长、峰值温度应下降；
   capped tick 计数随温度上升而增长。
3. 灭屏待机 10 min：scene=idle、steer_mask=0x0f、governor period=1000，
   三域回到 idle OPP。
4. 冷启动重应用：对比启动耗时与小核/大核域 up commit 节奏
   （v2 应更快到顶频、回落更平滑）。
5. 回退确认：`x9sched=0` 时无 `X9 SCHED` 横幅，行为与旧候选一致；
   单 bit 关闭可定位问题来源。
