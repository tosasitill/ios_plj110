# X9 后摄出图：交接计划（2026-11-17，接 log 350）

## 0. 现状（已证实）
- 分支 `feat/camera-integration`，worktree `~/ios_plj110/.worktrees/camera-integration`，最新候选
  `x9/out/x9-camera-module-cal-r32-20261117/boot.img`（源码 **01f0985b**，本机整包 exit0，未上机）。
  最新设备日志350对应此前 `x9-camera-wlgroup-r27-20261117`（bbd3c57b），不要混作 r32 运行结果。
- **传感器 ID 已通**（log 350）：后摄 hhmain `0x9186`（i2c4/0x1a），前摄 hhfront `0x0615`（i2c2/0x10），
  第一次就读到。根因是 WL28681C 的 pinctrl `reset_high` 组（GPIO203/187/202）里 GPIO187 从没拉高。
- **当前阻塞**：打开 Camera.app 时 `NATIVE CONFIG sensor=1 stage=1 errno=5`，
  `MM: stage/slot=1/0 errno=5 blockers=1ff smc=c2000514/0`。
  - 位置：`x9_camera_resources.inc: cam_receive_power_take()` → `x9_mm_power.c: x9_mm_power_acquire()`
    → SMC `0xc2000514`，x1=`0x803`（smmu-secure PM_GET, MM_SMMU）。
  - 返回值 a0 = `0xc2000514`（调用号原样返回）= 固件不支持（见 `pkvm_smmu_hyp.c:get_target_smmu_hw_semaphore`）。
- 不要再查的东西（都已和安卓逐字节对上）：传感器供电/时序、WL 全部寄存器、BUCK5/VS voter、
  MCLK（内部 + 引脚翻转）、XCLR 引脚、I2C 引脚/速率/格式、DOVDD DA。
- **继续静态核查的新增修复（2026-11-17）**：MM_2 已改 HWCCF，尚无新启动证据；
  native SMMU 首次 CR0 原误写4（EVTQ），已按 Android 改8（CMDQ）再12/13；
  接入 MT6993 普通 wrapper、34个 CAMSV DMA/CQI 默认阈值，修复 prior snapshot padding 和四摄 IRQ slot 越界。
  确切提交/构建结果见 NOTES 最新交接；不能把这些源码修复当 RAW/预览通过。

## 1. 工作规则（用户要求，务必遵守）
1. **先按安卓源码梳理完整流程，再对照 iOS 找缺失，静态证实后才出镜像**；不要一轮轮试错。
2. 照搬安卓每个驱动 probe 里的**全部**引脚/寄存器设置（整组 pinctrl state、整个 init 序列），
   不要自定义"不碰 X"之类的排除，除非安卓确实不碰。
3. 两个独立设备同样失败时，先查它们共用的东西。
4. 用户回复 "3NN" = 分析 Mac `~/NNN.txt`：先 `cp` 到 `~/ios_plj110/.local/x9/logs/`，修复、编一个镜像、
   commit、追加 NOTES.md，用中文汇报并给出完整镜像路径。
5. 安卓侧只读：SPMI PMIC `0-03..0-0d`、`12-0035`、pinctrl pins、clk_summary、regulator_summary；
   **禁止 glob 读全部 regmap（会重启手机）**。读之前先告诉用户读什么。debugfs 需要
   `su -c setenforce 0` + 推送脚本 `sh` 执行，读完 `setenforce 1`。
6. 构建（Mac，zsh 要包 `bash -c`）：
   `cd ~/ios_plj110/.worktrees/camera-integration && export PATH=/opt/homebrew/opt/llvm/bin:/opt/homebrew/bin:$PATH && /Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name <唯一名>`
   （失败后目录残留，要换新名字。）
7. 卡死时先查 io_submap（8 MiB）耗尽和 panic，不要先怀疑别的。

## 2. 参考源码位置（Mac）
- `R=~/ios_plj110/.local/references/oppo-modules-dt-mt6993/kernel/kernel_device_modules-6.12`
- 安卓设备树：`~/ios_plj110/.local/x9/modem/live.dts`
- 关键文件：
  - MM 电源：`$R/drivers/misc/mediatek/mminfra/mtk-mminfra-util.c`、`$R/drivers/misc/mediatek/hwccf/{src/hwccf_api.c,include/hwccf.h,include/hwccf_provider_data.h}`
  - SMMU：`$R/drivers/iommu/arm/arm-smmu-v3/mtk-smmu-v3.c`（`mt6993_data_mm`、`mtk_smmu_power_get`）、
    `$R/drivers/misc/mediatek/iommu/smmu-secure.c`、`$R/drivers/misc/mediatek/pkvm_smmu/hyp/`
  - SMI：`$R/drivers/misc/mediatek/smi/`（larb/common 上电、`mtk-smi-dbg.c`）
  - 相机：`~/ios_plj110/.local/references/oppo-modules-dt-mt6993/vendor/mediatek/kernel_modules/mtkcam/`
    （`imgsensor/src-isp8s/`、seninf、camsys/camsv）

## 3. 阶段 A：修 MM 上电（当前阻塞）
**2026-11-17 续接实现（r28，待上机）**
- 已从 `mminfra_ctrl()` 的 DT `mminfra-type0-pwr=3` 和 `mtk_smi_mt6993_disp_resource_ctrl()` 确认
  时钟/SMI 所需是 MM_2（BACKUP组0 bit3），以该引用保持 AO/infra0 父链；没有改用猜测的 0831 上电。
- `x9_mm_power.c` 已替换 0514 GET/PUT：MMUP dummy read、STA idle、AP SET/EN、global EN/STA、
  DT MM_2 ACK 和父域 ACK；继承 AP 票只借用，只撤本次新增票。v1 **不要求 DONE=1**。
- 父/子 token 保留；已发 SET/CLR 失败保留错误账本，不重发。相机错误路径也保留失败申请返回的 token。
  `smc_get/put` 为兼容字段，此后恒0；实际电源观察看新增 `X9 MM POWER GET/PUT` 日志。
- 电源与完整相机两个对象已编译 exit0。整包结果和实机结果见 `NOTES.md` 当前交接；本节后面的
  阶段 B–E 仍须按真实新日志推进，尚无 r28 MM/RAW/预览成功证据。
- 源码 `06166db7`，104/104 overlay 与 kernel 链接完成；首轮 r28 在 Python3.15 的隔离环境缺 LZ4
  而打包失败，保留目录。改用固定 Python3.13 重做 boot，**r28b exit0**：
  `x9/out/x9-camera-mm-hwccf-r28b-20261117/boot.img`（15085568 B），沿用 GPU280 无需 mailbox。
  r28b 已由含后续静态修复的 **r29** 候选替代；两者都没有新的 MM/RAW/预览运行结果。

**已查清（static）**
- 安卓 MT6993 的 MM SMMU 由 pKVM 管（plat flags `SMMU_SEC_EN|SMMU_HYP_EN`）。smi-dbg 有 `hwccf-support`，
  所以 `mtk_smi_pm_ops` 不注册。
- pKVM 用 SMC `0xC2000831`：x1=0 GET / 1 PUT，x2=smmu_id（MM=0）。它**只拿保持上电的信号量**，
  MM 关着时返回非 0，不负责上电。
- MM 真正上电走 HWCCF：mminfra-util，DT `mminfra-mtcmos-voter = <0x37…>`。
  - `0x37 = 55 = HW_CCF_BACKUP_GRP_0`；`mminfra-mtcmos-data = <1 2 0 3>`，即 MM_0=bit1、MM_1=bit2、MM_AO=bit0、MM_2=bit3。
  - 应答寄存器 `mm-mtcmos-base 0x31b5009c/a0/a4/a8`，mask `0xc0000000`。
  - MM HWV 基址 `0x31c00000`（DT `mm_hwv`）：XPU0 B1 SET `+0x20724`、CLR `+0x20728`，DONE `+0x1136c`，STA `+0x1135c`，GLB_EN `+0x11358`。
- 相机节点没有 power-domains；`seninf-core` 是 `hwccf-apply`，cam 时钟一投票，HWCCF 就自动开对应的 MTCMOS。

**原调查清单（MM_2 已实现，以下保留未完成观察项）**
1. 确认相机要投哪几位：
   - 读 `hwccf_api.c` 的 `_v1_hwccf_irq_voter_nowait/_wait_done`（行 776、993），弄清写 SET 后 DONE/STA/EN 的握手。
   - 查安卓谁对 MM_0/1/2/AO 投票：cmdq 用 MM_AO；看 VCP/vmm notifier（`$R/drivers/misc/mediatek/vmm/mtk-vmm-notifier-mt6993.c`）、
     SMI larb runtime PM、`mtk-cam` 的 pm_runtime 链。
   - 之前反汇编 ATF 的笔记写的是 PM 路径投 group0x37 bit3（MM_2）。
2. 确认屏幕是否已经让 mminfra 一直开着：只读 MM HWV `+0x1135c/+0x11358`、`0x31b5009c..a8`。
   安卓开相机前后各读一次也行，但 `mm_hwv` 不在安全读名单里，读之前先问用户。
3. 改 `x9_mm_power.c`：去掉 `0x514` GET/PUT，改为 HWCCF 投票。
   - 上电：写 SET，等 AP EN/global EN，确认 STA 不忙和 ACK mask；v1 **不等 DONE=1**。
   - 释放：写 CLR，只撤自己投的那一位，不碰屏幕/GPU 的票。
   - 是否还需要 `0x831` 拿信号量：只有在访问 SMMU 寄存器（`cam_smmu_observe`）前才需要；
     先静态确认安卓 EL1 是否会直接摸 MM SMMU。
4. 编镜像，日志看 `X9 CAMERA MM:` 的 stage 能否越过 RECEIVE(1)，进入 MAP(2) 和 TAKE(3)。

## 4. 阶段 B：MM 时钟 / SMI / larb（stage MAP→TAKE）
- 对照安卓 `seninf-core` 的 clocks 列表（DT 19312 行）：
  - `clk_cam_seninf, clk_cam_cam, clk_cam_camtg, clk_top_seninf..5, clk_top_camtm, clk_top_ap_step0..3, clk_hw_ccf_csi_bs, mmdvfs_mux`；
  - 以及 camsv 节点的 larb/CG 时钟（`cam_main_larb19/14/29_con, camsv_top_con, seninf_camtm_con, camsv_a_con`）。
- 逐项核对 `x9_camera_mm.inc` 的 `cam_mm_descs` slot 顺序、投票位和父时钟，是否和安卓 clk 驱动
  （`$R/drivers/clk/mediatek/clk-mt6993-*.c`）一致。
- SMI larb：camsv 的 `mediatek,larbs = <0x195 0x190 0x192>`。查 larb 上电和 common 配置（`$R/drivers/memory/mtk-smi*.c` 或 `misc/mediatek/smi`）。
- 期望：`MM:` flags 出现 POWER 和 CLOCKS，blockers 里清掉 POWER/CLOCKS/RATE/SMI。

## 5. 阶段 C：传感器配置（NATIVE CONFIG stage）
- 安卓 hhmain：`hhmainmipiraw_Sensor.c`（open/init 表、mode 表、`hhmain_Sensor_setting.h`）。核对 iOS 的
  `X9_HHMAIN_INIT_COUNT` 初始化表、preview 模式（分辨率、lane 数、MIPI 速率、PLL）是否逐字一致。
- 注意 subdrv 的 `i2c_burst_write_support`、写 group hold、stream on（0x0100）顺序。
- 2026-11-17静态补齐主/前/长焦 `get_sensor_cali/set_sensor_cali`：
  模组本地EEPROM header与两遍完整payload一致后，init/fab patch→QSC→enable→SPC/LRC→preview；
  前摄preview最终3621=0仍按源码保留。`MODULE_ACKED`只指事务ACK，非光学校准通过。
  超广角banked校准、AF/OIS和ISP颜色/镜头校准未闭；精确构建结果见NOTES。
- 期望：`NATIVE CONFIG ... completed=1 errno=0`。
- 长焦preview增益路径已静态补齐原`hhtele_set_gain`的30/36db OB选择：
  code `>3df8`选type1，否则type2，新epoch从type0开始，首次同值SET_GAIN也必须选择。
  OB表通过生成器提取，在真实no-credit边界先直接发送OB、再发原group控制；
  ACK与三次真实计数推进后绑定分别记录，未知OB读语义不假定。
  main/front/tele倒数增益均修正为原`unity-floor(unity*1024/gain)`整数顺序。
  编译与候选终态见NOTES；没有OB/控制生效/黑电平画质的实机验收。

## 6. 阶段 D：SENINF / CSI 接收 / CAMSV DMA
- 安卓 seninf：`mtkcam/.../seninf`（csi phy 配置、`mtk-iomem-ver`、端口映射；sensor0 endpoint 连哪个 csi 口，看 DT `remote-endpoint`）。
- 核对 iOS 的 `x9_camera_phy.inc / x9_camera_mac.inc / x9_camera_outmux.inc / x9_camera_native_source.inc`：
  CSI 口、lane 数、settle、outmux 路由、camsv 通道。
- SMMU 映射：安卓 MM SMMU 是 dma 模式（`mtk,iommu-dma-range`）。iOS 要么配 SMMU 页表，要么确认 bypass 合法。
  `x9_camera_smmu.inc` 里 `SMMU_SECURE` 相关的假设要重新核对（pKVM 平台）。
- 已修正 native CMDQ 开启顺序；普通 wrapper 用 DT resource1 `309e0000`，
  保存/验证/退休完整原值；secure WPCFG/AID-SID 策略尚无运行验证，不能由普通寄存器匹配推定。
- CAMSV 已接入 MT6993 默认静态 FIFO 的完整 DMA/CQI 阈值；后续仍需核对 Android
  runtime resume 的局部 reset、DDR/BW_QOS/COH 软件请求及其关闭顺序（本轮实现见下，尚未实跑）。
  SET/CLEAR/SW_R 独立记账，不作为普通几何字段按完整 RW 字还原。
- **本轮冷复位源码已补齐（2026-11-17，r30整包exit0，未上机）**：在 native SMI 完整取时钟后、
  MAC/PHY/OUTMUX 和全部 request/FBC 之前，按 ISP8s `sv_reset_by_camsys_top()` 执行
  CQ stop0→1/ACK bit1→0，以及 MRAW+0xa4 的0→0x30→0连续脉冲。
  三条GIC必须无enable/pending/active，sensor standby、输入和全部CAMSV0通道outstanding为0；
  局部reset不代替实际RAW/DONE/退休。保留独立前后快照，失败锁存并保留依赖。
- **SID来源已核清**：live DT `mm-smmu-cam` 的 `iommus=<MM_SMMU 0x80>` 由
  `arm_smmu_of_xlate()` 原样使用；CAMSV的`mediatek,smmu-dma-axid`列表仅注册fault callback，
  不是SID或需要手动执行的路由写表。`mtk_smmu_aid2sid()`仅在DEBUG配置编译，未新增该SMC。
- **DDR/COH/BW与normal reset源码接入（2026-11-17，r31整包exit0，未上机）**：
  按ISP8s默认SW-mode依次执行DDR SET/OR_CQ、COH SET/OR_CQ、BW_QOS SET/OR_CQ，
  保留六次真实RMW输入/输出，SET/CLEAR/SW_R不按普通保存字回写；SW_R仅原始观察，不当DMA授权。
  在自然RAW/CPU/worker/GIC/FBC退休和SMMU恢复、SV及OUTMUX/PHY/MAC恢复、sensor reset/MCLK
  真实隔离之后，独立执行`sv_reset()`的DMA/STG/CQ stop/ACK与central reset。
  两份完整post配置、三个请求都实际恢复到本次冷基线，才允许撤SMI/MM；
  复位前自然FBC和复位后读数各自存档，后者不替代自然DONE/退休。
- 期望：CAMSV 收到帧中断、DMA 写出、`X9 CAMERA` 帧计数增加，Camera.app 出画面。

## 7. 阶段 E：收尾
- 前摄同样流程（i2c2/0x10、MCLK 引脚 72、复位脚 170、WL LDO1/LDO3）。
- 清理诊断：`X9 CAMERA PAD`、`ID LATE`、1 s settle 可以保留或删除；r23b/r25/r26 的 latch 调整要保留。
- 稳定性：确认没有 io_submap 泄漏、没有卡死、相机开关多次正常。

## 8. 每轮汇报模板（中文）
- 日志结论（关键行）→ 安卓对照（源码文件:行号）→ 根因 → 改动 → 镜像完整路径 → 下一轮看哪些日志行。
