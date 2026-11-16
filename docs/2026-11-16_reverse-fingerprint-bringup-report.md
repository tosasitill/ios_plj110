# X9 屏下超声波指纹：bring-up 与 iOS 生物认证接入评估

## 结论

**有望做出真实指纹识别，并进一步接入本机解锁；但“系统发现设备”“正常注册生物认证”“支付接受认证”是不同工程。没有一条添加 ADT/IORegistry 节点就全部成立的捷径。**

本项目当前为 MT6993 上的 XNU + iOS 16.7.16 / D22 用户态。指纹的主要未知不在 GPIO，而在传感器的闭源识别后端、原厂 TEE 可用性，以及 iOS 生物认证和 keybag 的完整契约。普通应用的本地认证兼容可以研究；Apple Pay 原生支付不能按普通驱动适配排期。

本报告是**只读证据与架构评估**，不是 bring-up 成功记录。不执行 ADB/fastboot，不录指纹、不读模板，不修改根盘、固件或分区，不构建、不上机。详细私有证据放在 gitignored 的 `x9/out/fingerprint-assessment-20261116/`。

## 1. 已确认什么

| 证据 | 观察 | 能推出的结论 / 不能推出的结论 |
| --- | --- | --- |
| E1：已有实机 `.local/x9/modem/live.dts:18348–18400` | `/soc/spi7@161f0000/fingerprint@0` 为 `oplus,fp_spi`，mode 0，最大频率 35 MHz；GPIO IRQ=5、reset=7、CS=85，`dvdd-config` 为 3.2 V | 有具体的 AP 侧总线/供电线索；35 MHz 是上限，不是实测工作频率。不能由此证明芯片型号、采图命令或匹配位置 |
| E2：同一 DT `:15941` + OPPO `oplus_display_onscreenfingerprint.c:38,50` | 面板 `oplus,ofp-fp-type=0x1c0`，源码 bit6 定义为 ultrasonic；`0x1c0 & 0x40 != 0` | 支持“原厂配置走超声波指纹”的判断；不是一次实际采图/识别验证 |
| E3：原厂 `oplus_uff_24087_24203_24204.dtsi` | 与实机 SPI7/管脚/供电一致；注释写 vendor goodix；实机另有通用 `mediatek,goodix-fp` 节点 | Goodix 是合理的供应商线索；通用节点和注释都不足以确认具体型号。供应商模块源树未在本轮找到独立指纹协议/算法实现 |
| E4：已抓取 `330-unified.jsonl` | 最新命中的 `2026-11-16T06:06:12.740247552Z`：backboardd 匹配 `ApplePearlSEPDriver`，结果 `none` | 捕获的运行时正在寻找 Pearl 后端而没有找到。归档含多个启动，不能把每条都归为单次 330 启动，也不能当作本轮新上机结果 |
| E5：已挂载根盘 `com.apple.biometrickitd.pearl.plist` | `_LimitLoadToDeviceTree=pearl-sep`，匹配 `ApplePearlSEPDriver`，Mach service=`com.apple.pearld` | D22 原件的启动/发现入口不是任意指纹 HID 设备；原件服务接入有明确契约 |
| E6：原件 `biometrickitd` 与已提取 20H392 BiometricKit | daemon 有 `ApplePearlUserClient`；框架有 `com.apple.pearld`、`BKDevicePearl`、auth-token 相关名字，也有 Touch ID/Mesa 配置名字 | 证明应该调查的边界；**字符串不是已执行路径证据**。不能断言该构建完全不含 Mesa，也不能反向声称它已有可用 Touch ID 后端 |
| E7：`x9/kernel/src/iokit/Kernel/arm/SweetKeyStore.cpp` | get-lock-state 固定 `0x4`；lock/unlock/assert-consume 等分支直接成功且没有对应锁定状态变更；wrap/unwrap 为固定可逆混淆 | 目前 AKS 是启动兼容层，不是密码或生物认证保护实现。钥匙串可保存项目，不等于访问控制和认证密钥已安全实现 |

### Evidence → Finding → Path

- **E1–E3 → F1：硬件外围已有定位，识别后端未闭合 → P1：先定位原厂 HAL/TA 和实际采集/匹配路径，而不是先写 iOS 假设备。**
- **E4–E6 → F2：运行时入口是 D22/Pearl，Touch ID 字符串不等于可用后端 → P2：证明能力查询、用户客户端和 daemon 会话的同一条真实路径。**
- **E7 → F3：本机仍没有真实 keybag 锁定/认证保护 → P3：把密码与 AKS 状态机列为独立先决条件，不能用 SpringBoard 解锁动画代替验证。**

## 2. 必须拆开的六层目标

| 层 | 交付目标 | 难度判断 | 通过标准 |
| --- | --- | --- | --- |
| L0：外围 | 稳定供电、reset、SPI、IRQ、休眠恢复 | 中高，但有 DT 基础 | 重复会话正常，超时/取消可清理，黑屏与亮屏切换不破坏其它设备 |
| L1：真实识别 | 采集、质量判断、录入、模板持久化、匹配 | **高到极高，取决于闭源后端** | 新录指纹能匹配，未录指纹拒绝；清除后拒绝；重启和取消后状态正确 |
| L2：iOS 正常注册 | 能力查询、设置页、daemon、录入进度/失败、设备状态一致 | 高 | `LAContext.canEvaluatePolicy` 与录入状态一致；没有指纹时正确返回未录入/不可用，不是常量 true |
| L3：解锁 | 正常系统解锁与密码/keybag 状态相容 | 高；真保护远难于 UI 演示 | 重启后先输密码；错误指纹拒绝；锁定/取消无成功回调；用户点击取消不解锁；锁屏和 keybag 各自状态正确 |
| L4：应用授权 | LocalAuthentication、钥匙串 ACL、必要时密钥操作 | 很高，需要逐应用/策略验证 | 有真实 challenge 的会话不重放；`biometryCurrentSet` 对增删指纹失效；无法用错上下文或取消会话取回受保护项目 |
| L5：支付 | 普通支付 App，或 Apple Pay | 前者因 App 而异；后者是另一个安全平台工程 | 普通 App 以最终服务端授权为准；Apple Pay 需完成凭据配置、受保护授权及实际支付链路，Wallet UI 出现不算通过 |

**不能把 L0/L2 的成功报告成 L1/L3/L4/L5 已成功。**

## 3. 第一道大门：原厂指纹到底由谁识别

需要分别确认四种可能，当前证据还没有选定：

1. AP 能得到有意义的指纹图像，再由普通世界的闭源库提特征/匹配。
2. AP 的 HAL 只是编排，采集/匹配在厂商 TEE 的 Trusted Application 中。
3. 传感器自身承担较多识别和模板存储，主机必须完成加密会话或授权握手。
4. 上述混合实现，且依赖原厂安全存储、校准和特定版本固件。

SPI 可收发不代表能采图；拿到采图不代表有可迁移算法；拿到一个“匹配成功”事件不代表这个事件有可信、不可重放的来源。

如果原厂 TA 必不可少，则还要验证当前 LK → XNU 链下，原厂 secure-world 环境是否能提供所需服务，并实现正常世界的共享内存、RPC、存储等依赖。**保留了 tee/atf/gz 分区，不等于这些服务已经能从 XNU 使用。** Android HAL 的 Binder/bionic/Linux ioctl 接口也不能直接当成 Darwin API 使用。

Android 既有模板是否可复用、是否绑定 Android 用户/认证器 ID/安全存储，没有证据；不把复用或导出旧模板列为方案前提。新录入应使用明确隔离的命名空间，避免破坏 Android 状态。

超声波不能直接照搬光学方案的高亮圆斑/HBM 假设。仍须确认原厂屏幕模式、黑屏唤醒、触摸协同与校准要求；“不依赖光照”并不意味着没有显示/触摸状态依赖。

## 4. 第二道大门：iOS 没有“通用指纹驱动注册即可认证”的接口

概念链路如下，并非本轮已验证的完整调用图：

```text
原厂传感器 / 匹配后端
        ↓ 真采集、真录入、真匹配
X9 硬件驱动 + 受控的后端桥
        ↓ 类型、能力、会话、进度、错误、模板标识
iOS 生物认证设备后端 / BiometricKit daemon
        ↓ 生物认证状态 + 会话绑定的授权结果
LocalAuthentication / SpringBoard / securityd / keybagd
        ↓ 按策略执行钥匙串访问或密钥操作
应用及其服务器；Apple Pay 另有受保护支付平台
```

当前首先必须选择并验证接入面：

- **面向 Touch ID/Mesa 做替代后端**：语义更合适，但应先证明 D22 的可达路径以及缺哪些组件。不能靠 `TouchID` 字符串认定可用。
- **桥接/替代 D22 原有生物认证入口**：更贴近捕获的 Pearl 路径，但不能把指纹在 UI/API 上虚称为已实现的 Face ID。Touch ID 类型报告、屏下提示及录入 UI 都要单独解决。

不建议为了显示 Touch ID 全局改为 iPhone 8 身份：本项目其它显示、音频、GPU、相机等适配依赖 D22，副作用面过大。先限定在生物认证能力和后端边界。

最低契约不仅是 `match=true`，还包括：

- 可用/禁用/未录入/故障的能力状态与真实设备一致。
- 密码确认后才能进行受保护录入和修改；录入质量、重试、取消、进度与删除全生命周期。
- 模板 ID、用户域、认证器状态；添加或删除指纹后可正确失效相关授权。
- 一次匹配属于哪次 challenge、哪个调用者、哪个上下文和哪项策略；过期/取消后不能复用。
- 失败次数、软硬锁定、重启后的首次密码要求和其它密码回退条件。
- 普通 App 不得通过设备接口自行伪造“已认证”；不能让任意客户端直接宣布成功。

给 MobileGestalt 回 true、发布假 IOService、发一个 HID “biometric”事件，最多解除发现门槛，不会自动补齐这些契约。iPhone 原生 Home 按钮式录入/提示也不是 X9 屏下指纹的位置提示，需要 UI 适配。

## 5. 第三道大门：认证成功不是“返回一个布尔值”

### 本机可替代的功能，与原生安全等价是两回事

目前 `SweetKeyStore` 把设备保持为无密码、已解锁状态。可以在这种基线上做“真指纹驱动锁屏”的原型，但必须称为**UI/本机功能原型**，不能称为密码保护、数据保护或支付安全已完成。

更完整的功能兼容需要补齐可持久化的密码/keybag 状态、锁定后的访问策略、生物认证授权消费、模板集合变化失效和密钥操作。仅给 `LAContext.evaluatePolicy` 回调成功，会在需要 securityd/AKS 的应用路径上暴露不一致。

MediaTek TEE 可以成为自行设计的可信后端，但它不是 Apple SEP。Android 的 Hardware Authentication Token 与 Apple 的生物认证授权不是可直接转发的同一格式/密钥体系。需要协议转换和新的信任根设计，不能通过重命名设备或拷贝 SEP 固件获得原生 Apple 信任；Apple SEP 固件也不是可以直接运行在 MT6993 上的普通 arm64 程序。

### “支付”应按两类独立验收

**普通支付/银行 App**：

- 某些流程以本地 LA 成功为重新使用已注册凭据的条件，兼容后端可能使真实指纹用于这类授权。
- 另一些流程依赖受生物认证保护的钥匙串项目、Secure Enclave 密钥或应用自身的 challenge 签名；还可能执行设备完整性检查。
- 每个 App 的实际链路和远端策略不同，本轮没有测试任何支付 App，不能承诺“支付宝/微信/所有银行都能用”。

**Apple Pay（含 Wallet 原生卡支付及相关应用内支付）**：

- 生物认证只是授权步骤，不会生成可用支付凭据。
- 仍有 Secure Element/eSE、Apple 平台的受保护授权链、卡片 token provisioning 和服务端受理等独立门槛；非接触支付还需 NFC 通路。
- 现有 OPPO NFC/eSE 或 MTK TEE 不能仅凭“硬件功能相似”推导为 Apple 支付平台兼容。
- 当前无证据表明这些门槛已具备，**不把 Apple Pay 放入此次指纹 bring-up 的可交付承诺**。这是架构/信任链约束，不是“指纹再多调几周”能消除的问题；也不能据此断言任何替代支付方案均不可能。

## 6. 实际工作量与建议顺序

以下是工程量级判断，不是基于已闭合实现给出的工期承诺：

| 阶段 | 排期判断 | 投入前的止损条件 |
| --- | --- | --- |
| 限定调查 | 可先用数个工作日到一两周定位 HAL/驱动/TA 与 D22 接口 | 明确型号、活动模块、后端依赖和最小会话；不能只得到 DT 和型号传闻 |
| 真识别原型 | 依赖可用原厂后端时，数周级有机会；闭源采集/算法/TEE 未解时可能变成数月研究 | 新录/真匹配/错指拒绝/删除/重启的闭环，不用假成功占位 |
| 系统注册与锁屏 | 完整后端可用后仍是数周到数月量级，与密码/keybag 工程耦合 | 能力、UI、LA、daemon 和 keybag 同一状态；优先保留密码回退 |
| 多 App 授权兼容 | 数月级项目，必须按策略和 App 测试，不能预估通吃日期 | 受保护钥匙串、集合失效、challenge/replay、密钥操作实测通过 |
| Apple 原生安全等价 / Apple Pay | 当前不能给可实现工期 | 先拿到受保护平台及服务端链路的可达证据，再另行立项 |

**推荐第一步不是写 AppleMesa/ApplePearl 假驱动，而是只读定位原厂指纹服务和安全后端，确认 XNU 下能否得到一次可复现的真实识别。** 第二条同步先决条件是密码/keybag 状态机调查。此时就能决定“自用解锁”“LA 功能兼容”“硬件保护”三种路线的投入边界。

调查若需要新设备采集，另行安排明确的只读动作；本轮未发起。新录入、安全存储写入或 Android 模板状态变更不属于只读调查，需单独确认。

## 7. 只读复核方法

在仓库根运行；需要 NOTES §2.2 的已有输入与已挂载的只读 iOS 根盘。不要为了本报告重新采集设备：

```bash
rg -n -A38 'fingerprint@0' .local/x9/modem/live.dts
rg -n 'ofp-fp-type' .local/x9/modem/live.dts
rg -n 'TYPE_ULTRASONIC|GET_ULTRASONIC' \
  .local/references/oppo-modules-dt-mt6993/vendor/oplus/kernel/display_driver/oplus_drm/oplus_display_onscreenfingerprint.c
rg -n 'SWEET_AKS_LOCK_STATE|kAKSLockBag|kAKSAssertConsume|sweet_aks_wrap_mix' \
  x9/kernel/src/iokit/Kernel/arm/SweetKeyStore.cpp
```

读取现有 launch plist 的关键契约：

```bash
python3 - <<'PY'
from pathlib import Path
import plistlib
p = Path('/private/tmp/x9root/System/Library/LaunchDaemons/com.apple.biometrickitd.pearl.plist')
d = plistlib.loads(p.read_bytes())
for k in ('Label', '_LimitLoadToDeviceTree', 'MachServices', 'LaunchEvents'):
    print(k, d[k])
PY
```

限定读取已有磁盘日志匹配结果，不输出其它项目/用户数据：

```bash
rg -m 10 'ApplePearlSEPDriver' .local/x9/logs/330-disk/330-unified.jsonl |
python3 -c 'import sys,json; [(print(d["timestamp"], d["message"].strip())) for s in sys.stdin if (d:=json.loads(s))]'
```

本轮复核结果：4 条匹配 `none`，跨多次启动；指纹 SPI 配置和超声波位如 E1/E2；没有真实匹配、录入、解锁、支付的成功证据。输入 SHA-256、范围、时间线和采样结果在上述 `x9/out/` 目录；不将设备转储、Apple 二进制或生物认证数据提交到 git。
