# X9 原生 Apple Wallet 交通卡：目标与分门槛修复计划

## 目标与当前结论

**目标不是“读到交通卡”或“Wallet 显示卡面”，而是：在这台 X9 的 Apple Wallet 中完成真实交通卡开卡，卡片进入有效状态，余额可读，快捷交通设置有效，锁屏刷卡产生发行方认可的真实交易。**

默认范围：中国大陆、Wallet 原生新开交通卡。具体城市/卡种、账户地区和资金渠道尚未选定；最低系统/设备要求和当前发行支持须按选定卡确认，不假定所有现售卡都支持 iOS 16.7.16/D22。

规划阶段完成；**开卡目标未完成，尚未开始设备或资金操作**。最重要的前置未知是原厂 eSE 能否满足目标发卡路径的身份、签名、安全域及服务端接受条件。NFC 总线工作是必要基础之一，不足以证明 Wallet 可发卡。

本轮单代理，只读库存/已有日志、规划和 Git 发布；无 ADB/fastboot、射频/APDU、登录/激活、device enrollment、开卡/充值/迁移、eSE/根盘/分区写入或构建。源码规划基线 `84e1aad5`，其它活跃工作树和主 SMS checkout 不动。

## 1. 本轮新增及复用证据

| 证据 | 观察 | 结论边界 |
| --- | --- | --- |
| W1：根盘克隆应用库存 | `com.apple.Passbook` 主程序确在 removable application bundle 根下；`/Applications` 只有若干 Passbook UI 服务 | 不需要因 `/Applications` 未见主 App 就先找替代 Wallet。仍需新启动验证 LaunchServices、图标和真实 UI，不拿克隆库存冒充 live 状态 |
| W2：passd 原件 launch plist | 发布 device-registration、payment、library、account、nf-events、APS 等服务，监听 keybag first_unlock | 账户、设备注册、安全元件、显示库是独立功能面；service 名字不是整条发卡调用图或强制策略证据 |
| W3：既有 330 归档 passd | `06:06:37.337Z`：`No account, supportsDeviceToDeviceEncryption: NO`；随后 CloudStore 初始化报 `PKPassKitErrorDomain -4008`、`User has an invalid account state` | 已捕获账户组件失败，没有执行目标卡的 add-card 操作；不能把 CloudStore 错误断言为每种卡相同的首个 eligibility guard |
| W4：原件/框架字符串库存 | 有 `NFSecureElementManagerSession`、secureElementIdentifier、`No secure element, cannot sign authToken`、supportedExpressModes、secureElementAttestation 等 lead | 需调查的 SE 会话/签名/快捷模式边界明确；尚未证明目标交通卡强制采用哪条路径，也未证明已有可被接受的证明材料 |
| W5：既有 330 归档 passd 的 SE/注册状态 | `06:06:16.484Z`：`Secure element available: NO, is on supported device: YES, is broker registered: NO`；同秒 `Needs Registration: YES (Has Previously Registered: NO, Has Certificates: NO, User Owns Secure Element: YES)` | 这是客户端当时的状态，不是本轮向服务器注册或被拒绝的结果。`supported device: YES` 不证明平台接受 X9/eSE；peer-payment 相关字段也不作为交通卡资格判定 |
| R1：前一轮 NFC 证据 | 实机 DT 板级 SN220T；NFC I²C14/0x28、400 kHz；eSE SPI5、19.2 MHz 上限；厂商 NCI/eSE 仲裁源码在。nfcd 捕获会话打开失败 | 硬件有工程基础；未成功识别控制器、打开 SE 管理会话或发出 RF |
| R2：前一轮账户/激活证据 | apsd 取得 activation identity 失败；akd provisioning HTTP200 后 ADIProvisioningEnd -42410 / AKAnisetteError -8004，BAA 下层 MobileActivation -1 | 当前账户/设备身份共享基础不完整；不能假设 Wallet 目标卡必需全部使用同一 Anisette/BAA 分支 |
| R3：AKS/密码基线 | 330 wrap 失败；当前源码有后续软件 wrap 修复，但固定已解锁/无密码兼容状态仍在 | 需新启动证明修复；软件混淆不等于真正设备密码、keybag 保护、SE 签名或服务身份 |

前轮详见 [NFC/Apple 服务评估](2026-11-16_reverse-nfc-apple-services-report.md) 和 [指纹评估](2026-11-16_reverse-fingerprint-bringup-report.md)。日志跨多个启动，没有新的卡片请求/发行响应抓包。

### Evidence → Finding → Path

- **W1–W3 → F1：Wallet 组件已有，账户状态未正常 → P1：恢复原件 UI、有效系统账户及其真实依赖，不重新做一个假 Wallet。**
- **R1/W4/W5 → F2：客户端没有可用 SE/注册证书；普通 NFC 与可用 SE 签名/发卡资格分离 → P2：最早阶段就定位 SE/device enrollment 门槛，不能等完成全部 UI/读卡后才检查。**
- **R2–R3 → F3：本地 Activated/软件 AKS 不补出服务认可的身份 → P3：验证所需材料的真实来源和会话，不改返回值假装注册成功。**

## 2. 从交通卡目标反推依赖，避免无关前置工作

```text
选定交通卡、iOS/地区/资金路径
     ├─ 原件 Wallet/passd + 有效系统账户 + 真密码/keybag状态
     └─ NFC基础 + 可用eSE管理会话 + 目标路径所需的可信证明
                        ↓
            Wallet/平台 device enrollment 与发卡资格
                        ↓
          交通卡发行请求 + 所需资金授权 + applet/密钥个性化
                        ↓
            issuer状态 ↔ SE状态 ↔ passd/Wallet记录一致
                        ↓
              快捷交通/off-host routing/锁屏与恢复
                        ↓
                   真终端交易与扣费验收
```

这是修复依赖图，不是本轮已验证的全调用图。

**本目标不要求先完成：**

- 用户可见的 iMessage 蓝气泡或手机号验证短信。Wallet 可能复用 APS/IDS 组件，只有实际使用的内部链路才列为依赖。
- 完整 App Store/FairPlay DRM。若选定卡可由 Wallet 内置目录直开，不应先为此修所有 App 下载/执行；只有明确需要发行方 App 的路线才增加这项。
- 超声波指纹。快捷交通交易通常不需要每次生物认证；开卡/充值依目标流程采用其支持的设备密码/账户/付款授权，但不能因此跳过设备安全状态或 SE 契约。
- 全部 CoreNFC 第三方读卡 API。最小标签测试可用于验证 NCI，但 Wallet 主要需 nfcd 的 SE/卡模拟路径，完整 reader API 不应成为虚假必修项。
- 电量耗尽后的备用电量刷卡。D22 身份不能直接假定具备其它机型的 power-reserve 能力；首个目标只要求正常供电下锁屏刷卡。

## 3. 修复分期：每一门都要求可验证结果

### G0：锁定卡种与最小资金路径

**状态：待用户指定卡。**

- [ ] 记录卡名/城市/发行方；区分新开卡、实体卡转入和其它设备迁移，本计划默认新开。
- [ ] 核对 iOS16/D22、账户地区、实名/地区要求、发行可用性及快捷交通支持。
- [ ] 明确充值/首次开卡的实际渠道、最低金额、服务费/押金和失败退款规则。
- [ ] 若必须用 Wallet 内已配置付款卡，显式新增“付款卡可配置/可授权”依赖；不能假定交通卡开卡无需另一条 payment provisioning。

**通过证据**：选定路线、系统/设备资格和资金依赖明确。仅卡目录中出现该城市不算证明发卡资格。

### G1：建立可回放的原件 Wallet 与系统安全基线

**状态：有克隆/旧日志证据，缺新启动验证。**

- [ ] 固定一个已知可启动的源码、镜像及插件状态，记录 hash、boot args、用户操作、唯一日志编号；不盲用“最新 main”混入并发实验。
- [ ] 验证原 `com.apple.Passbook` 的 LaunchServices/容器/启动，passd XPC 与需要的 UI 服务可用。
- [ ] 使用非敏感测试项目验证修复后的 AKS 保存/读取/重启恢复；单独验证设备密码、keybag lock/unlock、首次解锁通知与访问控制。
- [ ] 确认网络、时间、DNS/TLS和每个实际请求的 Reachability；不要因一个 HTTP200 把整体设为成功。

**通过证据**：Wallet 可稳定启动，测试持久化和锁状态真实一致。仅压掉密码提示、固定返回 unlocked 或添加 MG capability 不算通过。

### G2：账户与目标路径所需设备证明——先调查、再受控验证

**状态：捕获的账户/ADI/activation identity 有失败；目标卡具体策略未知。**

- [ ] 从 passd 的真实卡片设置入口追到 eligibility/account/device-registration：把 UI、CloudStore、设备注册的失败分开。
- [ ] 修复其实际依赖的 AuthKit/账户持久化；对确实走到的 ADI -42410、BAA/MobileActivation 错误闭合首个依赖。
- [ ] 记录每类请求需要的证书/签名/nonce/会话材料的来源与消费者，只记录元数据，不提取现有账户/设备/卡片密钥。
- [ ] 判断是否存在本 iOS/目标卡受支持的替代路径；不能从桌面经验或字符串推断 fallback 已可用。

**通过证据**：passd 目标路径接受有效账户及所需材料，客户端最终状态成功；HTTP200 或本地 Activated 不算通过。登录/设备注册将改变账户或本机状态，实际执行前另行确认。

### G3：NCI 基础与 eSE 会话，目标不是单纯读标签

**状态：已有 DT/厂商源码，捕获 nfcd 打开硬件会话失败。**

- [ ] 移植 NFC 供电/时钟/IRQ/I²C、NCI 收发和可清理的初始化，先用真实控制器响应闭合最小会话。
- [ ] 在自有驱动层证明 eSE 传输、NFC/eSE 共享电源与冷复位回复仲裁，区分 NFC 和 SE 回复。
- [ ] 闭合 nfcd 的硬件会话及 `NFSecureElementManagerSession` 对应边界；准确报告真实能力/故障。
- [ ] 仅在接口性质已确认时读非敏感版本/能力元数据，不 reset-to-factory、不下载/删除 applet、不修改原厂安全域。

**通过证据**：NFCC 和 eSE 会话可重复、取消/故障后可恢复，原件客户端读到真实元数据。UID/NDEF 成功、eSE ATR/APDU 有回复都不能单独证明 Wallet provisioning 可用。

### G4：最重要的 Go/No-Go——SE 签名、平台注册与发卡资格

**状态：未知，不承诺可实现。此调查必须与 G1–G3 并行早期开展，而不是最后再看。**

- [ ] 从目标卡的注册请求确定它实际使用的 SE identifier、签名/证书链、挑战、平台证明与安全域前提。
- [ ] 证明请求需要的签名确来自该请求所绑定的 SE/安全后端，并属于同一 challenge/会话和匹配的证书/密钥；原厂 eSE 可签某种数据不等于平台接受。
- [ ] 判断 eSE 是否有兼容 applet/个性化机制，是否需要发行平台授权、受控安全域或其它设备绑定条件。
- [ ] 在接口/风险明确、用户确认后做**不扣费的**资格/注册验证；若该操作也写 SE/设备身份，则仍按状态变更处理。

**通过证据**：目标平台接受设备/SE 注册与卡资格，且后续准备个性化的客户端与 SE 状态一致。不能只看前端按钮可点或一条中间响应。

**No-Go 条件**：明确证明目标路线要求的有效证明/安全域授权在当前硬件和启动链下无法提供，且没有已证实受支持的可达路径。此时本目标受架构/信任链阻塞，不继续以 UI patch、常量签名、另设备身份拼接或普通 HCE 冒充原生开卡。

当前证据尚未达到这个 No-Go 结论；也没有资格据此承诺成功。它是早期需要证实的核心条件。

### G5：真实开卡、资金状态与 SE 个性化的同一事务

**状态：未开始；只有 G0–G4 的适用前提已通过后才进入。**

- [ ] 确认付款渠道可用，用户确认卡种、金额、费用和这一次操作。
- [ ] 记录一次尝试的发行方业务 ID、阶段、客户端错误和 SE 非敏感状态，不记录 PAN/完整卡号、密钥或授权 token。
- [ ] 闭合资金授权/发行请求/app provisioning/个性化/激活；每一步以真实消费者结果决定下一步。
- [ ] 在发送新请求前查询/核对已有尝试状态，支持失败/断网/重启后的恢复，避免重复开卡或重复扣款。

**通过证据**：发行方 active 状态、SE 有效卡应用、passd 本地记录/Wallet 卡状态/余额相互对应。只扣款成功、下载 pass、卡面出现或 APDU 末尾成功状态字都不算整条事务完成。

若资金已扣、个性化失败：保留原尝试状态，核对发行方 pending/refund/recovery，不盲目再次支付或删除残留 applet。

### G6：快捷交通与锁屏卡模拟

**状态：未开始。**

- [ ] 从有效卡设置 supportedExpressModes/default card，验证实际生效而非只改 UI。
- [ ] 建立 NFCC listen/card-emulation 与 off-host routing 到目标 eSE 应用；不把 reader polling 或普通 HCE 当成同一实现。
- [ ] 验证正常供电下锁屏、首次密码后、休眠恢复、重启后的默认卡与策略。
- [ ] 证明场进入/离开、超时、重复触碰不产生失配；正确处理余额、计数器和交易状态。

**通过证据**：受控测试终端能走到目标应用并完成受接受的交易协议；锁屏不要求每次指纹，但仍符合已验证的快捷策略。

### G7：最终验收，不停在 Wallet 中间状态

- [ ] 优先发行方/受控测试终端；需真实闸机时由用户确认一次交通消费。
- [ ] 锁屏刷卡成功，终端/闸机认可交易并有对应扣费/消费记录。
- [ ] Wallet/SE/发行方交易与余额可核对；分别区分本地即时余额和后台可能延迟的记录。
- [ ] 重启并首次正常解锁后，再验证卡有效和快捷策略恢复；保留确切镜像、设置、步骤和日志。

**完成标准**：从固定可回放基线完成真实开卡并可重复刷卡使用。注册状态、卡面或终端 beep 不代替资金/卡应用/终端交易结果。交通卡是持久化外部状态，不为“干净基线”擅自删除/重发或重置安全元件。

## 4. 工作流、状态变更与回滚

- 每个实验单独 feature/fix 分支；需要同时推进时使用项目 worktree + `.local` 链接；本计划未启动代理或并行实现。
- scratch、反汇编、日志放相应 worktree 的 `x9/out/`。XNU delta 仍在 `x9/xnu/`，不新增 `.local` 实验目录或仓库外构建依赖。
- 先单对象编译，再必要的完整镜像；使用唯一产物名。用户手动 temporary fastboot boot，默认不 flash、不改 GPT，不覆盖 Android/恢复/原件备份。
- 账户/设备注册、eSE 写入、开卡/充值/迁移/删卡及资金消费，在每一步执行前单独确认。提出目标不等于已确认金额或允许安全域变更。
- **分区备份不覆盖 eSE 内卡片、密钥和安全计数器**。不能宣称刷回 Android 或还原 userdata 就恢复开卡前状态；禁止盲目清 SE 或原厂 applet。
- 驱动失败清理只恢复自有会话资源；已发生资金/发行状态变更按发行方恢复/退款流程处理，不用本地数据库回滚冒充撤销。
- 交接必须记录 code/commit、CI run ID（没有则写无）、镜像 hash、真实启动/会话/交易结果、观察 vs 推断及下一门尚缺的证据。

## 5. 最近一次调查应交付什么

第一轮实现前调查交付三个小结，而不是先承诺开卡日期：

1. **目标卡路线**：卡种、版本/地区/账户条件、资金渠道、是否另需付款卡 provisioning。
2. **SE/device registration 可行性表**：每项请求材料、产生者、消费者、当前硬件来源、能否证明平台接受；未知明确列出。
3. **最小补丁与验证计划**：只针对 passd 首个失败边界、nfcd SE 会话、真实密码/账户依赖制定小步骤，不绕到完整 iMessage/App Store/指纹。

限定资格调查可先按数个工作日至一两周安排；G4 未过前不排整条开卡工期。NCI、账户和 SE 兼容即使各有可行路线，也可能是数月级工程；当前无法保证原厂 eSE 可成为 Apple Wallet 接受的发卡目标。

私有规划证据在 `x9/out/wallet-transit-plan-20261116/`：scope/timeline/workitems、Wallet/launch/string 库存、脱敏 passd 日志、复用前轮的已保留证据。主程序容器 UUID、Apple 二进制、账号/交通卡材料不进 Git。本轮仅完成计划，G0–G7 均未据此宣称实际通过。

## 6. 离线复核与后续重放边界

本轮原始日志来源是 `.local/x9/logs/330-disk/330-unified.jsonl`，Wallet/launch 库存来自既有根盘克隆和 `.local/apple/dsc-20H392/`。没有为本任务重新启动设备。先复用已经留存的限定证据，避免反复扫描整份跨启动日志：

```bash
python3 x9/out/wallet-transit-plan-20261116/verify-plan-evidence.py
git show --stat --oneline aff607fe
git show --stat --oneline 2998514a
```

第一条只读取本轮已归档的 JSON，核对 Wallet 原件库存、passd 启动路径、SE 不可用/待注册/无证书和账户失败的确切事件；不输出主程序 UUID、不登录、不触发 NFC/SE 或资金操作。它验证的是**规划依据**，不是设备功能。后两条定位前轮原报告提交；均不能取代后续新启动验证。

下一轮最小实机重放应先固定源码/镜像/插件，再记录“打开原件 Wallet → 进入选定卡的添加流程 → 首个真实失败边界”的用户动作与唯一日志；遇到登录、注册、资金授权或 SE 写入的状态变更边界先停止并逐项确认。每次只改变一个依赖，以 passd 最终状态及对应消费者结果定位修复是否有效。
