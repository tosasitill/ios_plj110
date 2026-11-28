# 自动亮度：开关缺失与不随光变 — 定位与修复（2026-11-27）

症状：设置里没有自动亮度开关；手电筒照射时 ALS lux 正常跳变
（P2 日志 2356→6943→119、dispatch 正常）但面板亮度不动。ALS 传感器链
（SCP→HID）此前已实证健康，断点在 CoreBrightness（backboardd）一侧。
本文结论全部来自 20H392 原厂二进制（DSC 提取 + 反汇编）与本仓库源码，
未上机验证的部分逐项标注。

## 定位结论

1. **开关的门是 MobileGestalt 的 `ambient-light-sensor` 答案**（实证）。
   `DisplayAndBrightnessSettings` 的 `-[DBSBrightnessManager
   mainDisplayBrightnessSpecifiers]` 两次调用 `PSCapabilityManager
   capabilityBoolAnswer:`（同一键），第二次的返回值决定是否创建那个
   PSSwitchCell（set/get = `setAutoBrightnessEnabled:` /
   `autoBrightnessEnabled`）。DBS 与 Preferences 两个二进制里唯一的
   能力键字符串就是 `ambient-light-sensor`，PSCapabilityManager 对这类
   键以 MG 答案为准。
2. **同一个答案也决定 CB 是否理会开关**（实证）。CoreBrightness 二进制
   内有日志串 `ALS is not supported on this device -> ignore setting of
   DisplayBrightnessAuto property`：答案为假时 CB 直接忽略自动亮度。
   两个症状（无开关 + 不随光变）可由这一个假答案统一解释。
3. **答案的持久化风险在 MG 磁盘缓存**（实证机制 + 推断命中）。
   `com.apple.MobileGestalt.plist` 只按 build+UUID 校验（kern_exec.c
   mgwipe 注释），ADT 修正后旧答案可存活。现有 capabilities epoch 的
   哈希元组（`mode/ui/bt/als/camera`）只覆盖 boot-arg 标志位，不覆盖
   标志位所选的 ADT 内容（`x9_als=3` 背后的 AOP ALS 节点）——若缓存曾在
   节点缺失的镜像下重建，假答案会被元组匹配永久保留（P2 日志为
   `epoch matched; cache kept`）。
4. **CB 的曲线/状态读取位置已钉死**（实证）。`CBABCurve
   initWithConfiguration:` 以 `copyPreferenceForAllUsersForKey:` 读
   `com.apple.CoreBrightness` 域：`DisplayPreferences` → `<显示 UUID>` →
   `AutoBrightnessCurve {Version(1|2), Lux1, Lux2, Backlight1,
   Backlight2, LuxThreshold}`，逐键存在性检查。`DefaultCurves.plist`
   在 20H392 根盘中不存在（全盘核查），曲线本体是 CB 内建 Ammolite
   配置按 vendor 索引选择、无匹配时走 `using default curve
   configuration` 兜底——所以"缺曲线文件"不是断点。
5. **我们的显示没有任何身份**（实证）。`CBDisplayModuleHIDiOS
   initWithDevice:` 从显示服务读 `VendorID`、`ProductID`、
   `kUSBContainerID` 三个属性；SweetFramebuffer 此前一个都没发布：
   VID/PID 恒 0（仍走默认曲线配置），且无 container ID 时 CB
   `create ad-hoc UUID as backup`——每启动一个随机 UUID，per-display
   偏好（含曲线与学习状态）永远存不下、也种不进去。
6. **lux 语义**（实证 + 一处样本推断）。我们 dispatch 的是 TCS3720
   驱动 lux（type 116 value[0]）；Android 的标准光感是 OPLUS Fusion
   输出（融合层带闭合面板补偿）。唯一捕获的样本对为驱动 427 vs 融合
   474（≈×1.11），量级温和，落在两段拟合的误差内，本轮不做额外映射；
   若上机发现低光段系统性偏暗，再按多点对照加一层标定。

## 修复（本轮已合入的代码）

- **MG epoch 改名**（kern_exec.c）：`refresh-capabilities-20261126` →
  `refresh-capabilities-20261127`，下一次启动强制重建一次 MG 缓存，
  使 `ambient-light-sensor` 在当前 ADT 下重新求值；元组校验保证之后
  不再重复擦。
- **显示身份**（SweetFramebuffer.cpp）：在 AppleARMBacklight 子服务与
  帧缓冲服务上发布 `VendorID=0x5839`、`ProductID=0x0287`、
  `kUSBContainerID="7C9E6993-1102-4A5B-8C7D-202611270001"`（稳定合成值；
  MT6993 面板栈不暴露 EDID 身份）。CB 的 per-display 状态从此有稳定键。
- **CB 偏好种子**（kern_exec.c `sweet_x9_seed_corebrightness`，PID1，
  `x9_cb_seed=0` 可关）：仅当 `com.apple.CoreBrightness.plist` 不存在
  时写入（501:501、临时文件+rename，沿用 purplebuddy 种子惯例）：
  `DisplayBrightnessAuto=true` + 上述 UUID 下的原厂曲线。文件已存在
  时只探测首 2048 字节是否含状态键并打日志，**不改动**（CB 的学习
  状态归 CB 自己管）。曲线锚点为 Android 原厂表（2026-11-27 采集的
  DisplayDeviceConfig）到 CB 两段式的拟合：Lux1=256→Backlight1=
  0.3718、Lux2=4096→Backlight2=1.0，全表最大拟合误差 0.044（主机测试
  断言 ≤0.05）。Android 的 debounce（4s/8s）与迟滞（+10%/−20%）在 CB
  偏好里没有对应键（AAB 内部常量），未移植——这是明确的剩余差距，
  不影响曲线本体生效。
- **主机校验**：`x9/display/test_autobright_seed.py`（7 项：plist 可解析
  且结构/取值正确、双文件 UUID 逐字一致且为大写规范形、帧缓冲三属性
  发布、epoch 已改名、门控与调用点存在、种子 create-only、曲线拟合
  误差）。`x9/display` 既有测试 15/16 过（余 1 项 test_fill 为既有
  libc++ 工具链环境失败，与本次改动文件无关）。

## 上机判别点（下一轮带此内核的镜像）

1. 启动日志应出现其一：
   - `X9 CB SEED: com.apple.CoreBrightness seed written errno=0 ...`
     （种子文件此前不存在，已写入），或
   - `X9 CB SEED: existing com.apple.CoreBrightness.plist kept
     (DisplayBrightnessAuto in first 2048 bytes=1); not modified`
     （文件已存在且含状态键）。若为 `=0`，说明 CB 文件早已存在但无
     状态键——种子按设计不覆盖，下一轮需决定是否做合并写入。
   同时 `X9 MGWIPE: mode=2 ui=1 bt=1 als=1 camera=1` 应为
   `epoch marker created=1`（强制重建发生），而非 `epoch matched`。
2. 设置 → 显示与亮度（及辅助功能 → 显示与文字大小）：自动亮度开关
   应出现且为开。若开关仍缺失，则 MG 答案在用户态仍为假——epoch 已
   强制重建的前提下，这将证明原生答案路径不止 ADT 节点（下一步查
   libMobileGestalt 对该键的求值条件），与缓存无关。
3. 手电筒复测：照射/遮挡前置传感器，面板亮度应在数秒内跟随变化
   （对照同轮 `X9 ALS: dispatch ... lux=` 跳变）。若开关在、状态开、
   lux 在跳而亮度仍不动，断点收窄到 CB 的 AAB 激活条件（如 ALS HID
   服务的附加属性），与本轮三项修复无关，需 CB 状态日志再定位。
4. 亮度跟随的量级核对：在约 250 lux 与约 2000 lux 两个环境光下，
   面板相对亮度应分别接近 0.37 与 0.66（拟合曲线）；若整体被压到
   最低档，则 Backlight 值的单位假设（归一化）错误，种子曲线需按
   nits 量纲（×490）重标——种子是 create-only，改值前需先删设备上的
   CB plist 或 bump 种子版本。

## 2026-11-28 addendum: CB seed merge-write + MG answer chain re-check

- **CB seed is now merge-write, not create-only.** `sweet_x9_seed_corebrightness`
  reads an existing com.apple.CoreBrightness.plist whole (8 KiB cap), and:
  already has DisplayBrightnessAuto -> untouched; XML without the key ->
  fragment (state key + factory curve, or state key alone when
  DisplayPreferences already exists) inserted before the root </dict>,
  all other keys preserved; binary plist (bplist00, which is what
  backboardd/cfprefsd actually write) -> original hard-linked to
  com.apple.CoreBrightness.x9-before-seed.plist and replaced with the
  XML seed, following the keyboard-dictation seed's in-tree pattern.
  Host tests: x9/display/test_autobright_seed.py 11/11 (merge simulated
  in Python: unrelated keys survive, curve lands complete, existing
  DisplayPreferences not duplicated). Gate unchanged (x9_cb_seed=0).
- **MGWIPE `als=1` semantics corrected.** The X9 MGWIPE log line prints
  the epoch tuple of boot-arg flags (als = (x9_als==3)), not a measured
  MG answer. probe2's als=1 never proved the cached
  ambient-light-sensor answer was true; its missing switch is
  consistent with the answer being false then too.
- **MG answer chain.** The port constructs no MG answers; stock
  libMobileGestalt (20H392 DSC extract) computes them natively. The
  exact string IODeviceTree:/arm-io/aop/iop-aop-nub/als is referenced
  by exactly two obfuscated question getters; our published node is
  path-only (phandle + device_type, no als-colorCfg). Whether the
  getter's predicate is existence-only or also reads a property (L3),
  and whether the synthetic ADT's aop subtree reaches the IODeviceTree
  plane (L2), remain unverified offline -- a cache rebuild alone
  recomputes the same false if either link fails. Display identity is
  published at BOTH candidate read sites (backlight child and
  framebuffer service, SweetFramebuffer.cpp), so identity is excluded
  as the missing link. Next logged round: watch for CB's own
  "ALS is not supported on this device" line and the new CB SEED
  merge/replace log lines; MG answer A/B = force an epoch rebuild with
  the ADT als content toggled and diff which CacheExtra key flips.
