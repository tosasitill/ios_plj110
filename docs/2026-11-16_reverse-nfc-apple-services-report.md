# X9：NFC、iMessage 与 App Store 的接入和验证门槛

## 结论先行

- **NFC 读标签/ISO-DEP 通信值得优先 bring-up**：SN220T 配置、I²C NCI 驱动和 SPI eSE 通路都有现成硬件线索。但 CoreNFC、卡模拟、Wallet 与 Apple Pay 是不同层，不会随控制器工作自动成立。
- **iMessage 当前存在明确身份门槛**：捕获的 `apsd` 无法取得 activation identity，IDS 还有钥匙串/保护身份和注册状态错误。系统本机显示 Activated 不能代替 APNs/IDS 的服务端注册。
- **App Store 并非缺少程序**：根盘克隆有 AppStore.app，捕获日志中 appstored 实际运行。当前同时有商店网络不可用、Anisette provisioning 失败、BAA 签名失败，以及 FairPlay 后端缺口。不能归结为一个 capability 或 Apple ID 开关。
- **这几项并非都与 Apple Pay 同级无解**：普通 NFC、本机 IPC/网络/存储、部分账户客户端契约可以修；受服务端验证的身份和 DRM 材料不能由本地假成功制造。需先恢复客户端依赖，再逐个判断远端实际接受条件。

本轮为单代理只读评估，基线 `ff9e900f`。无设备命令、射频/APDU 发送、登录/激活请求、短信、购买、账户凭据读取、根盘/分区写入、构建或 CI。日志来自已有 330 磁盘归档，跨多次启动；最新相关记录到 `2026-11-16T06:07:03Z` 左右。根盘克隆用于文件/配置库存，不冒充当前设备全部持久化状态。

## 1. 决定性证据

| ID | 证据 | 观察与边界 |
| --- | --- | --- |
| E1 | `.local/x9/modem/live.dts:4611–4612,9851–9885,18148–18152` | 板级配置 `SN220T`；NFC 在 I²C14 `0x162c0000`、地址 `0x28`、400 kHz，`nxp,sn-nci`；IRQ/使能复位/CLKREQ/VBAT 为 GPIO 213/19/18/15。eSE 在 SPI5 `0x161b0000`，`nxp,sn-ese`，19.2 MHz 上限。是已捕获 DT，不是本轮控制器型号回读或实测 |
| E2 | OPPO `mt6993_24087_sn220.dtsi`、`sn_nci/nfc/i2c_drv.c` | 板级配置与实机 DT 对应；file_operations → nfc_i2c_dev_read/write → i2c_read/write → i2c_master_recv/send。源码有 IRQ 等待、NCI/eSE 冷复位回复隔离，说明不只是裸 I²C 数据转发 |
| E3 | 330 归档 nfcd，`06:06:12.859743232Z` | `-[_NFHardwareManager _loadHW]:799 Failed to open session; hwState=3`。证明本机硬件会话失败；尚未命名状态 3 的枚举含义，也未证明具体失败在匹配、用户客户端还是后续初始化 |
| E4 | 根盘 nfcd launch plist | 发布 `com.apple.nfcd.service.corenfc` 和 `com.apple.nfcd.hwmanager`。本轮限定搜索未找到 tracked X9 NFC 后端，不能由此推断所有参考仓库均无实现 |
| E5 | 330 归档 apsd，`06:06:38.948–38.951Z` | `No identity found in keychain. Copying activation identity.` → `_APSCopyActivationIdentity() failed` → MobileActivation.ErrorDomain `-1`。这是取得身份失败，不是 APNs 服务器已拒绝某个合法身份的证据 |
| E6 | 同归档 identityservicesd | 有 `Unable to load keychain data...`、`No protection identity provided...`；最新相关状态含 `com.apple.madrid` / `registrationError:-1`。内部注册状态不是一条已定位的远端拒绝响应 |
| E7 | 同归档 itunesstored / amsaccountsd | `SSErrorDomain 110` 下层 `NSURLErrorDomain -1009`；amsaccountsd 同样 `-1009`。只能证明这些请求的网络不可用状态，不能据此断言所有端点均不可达 |
| E8 | 同归档 akd，`06:06:56.600–56.614Z` | `End provisioning response code: 200` → `ADIProvisioningEnd failed. Error: -42410` → `AKAnisetteError -8004`；下一次开始 BAA 签名后报 `com.apple.devicecheck.error.baa -10001`，下层 MobileActivation.ErrorDomain `-1`。**HTTP 200 不等于客户端建立了可用 Anisette 状态**。未给 `-42410` 猜测一个未经确认的内部枚举名 |
| E9 | `tools/mkadt.py:112–123`、`sweet-root-tweaks.py:93–108` | 使用合成 ECID/A11 chip-id 和开发板式安全状态；root helper 种入 Activated data ark；源码说明选择 Hactivation。它们是本地 bring-up 配置，不能作为 Apple 签发有效 activation identity 的证据 |
| E10 | `/Users/tosasitill/330.txt` + 当前 SweetKeyStore | 330 中 AKS selector 10 返回 `0xe00002c7`；当前源码已加入后来的软件 wrap/unwrap 修复，但仍是无密码/固定已解锁状态。**新源码不能倒推旧日志中的账户依赖已修好，也没有这些服务的新启动验证** |
| E11 | `kern_exec.c:8335–8348` + 330 console | 当前默认禁用 `com.apple.fairplayd.H2`，`x9_fairplay=1` 才保留；330 显示两个 launchd disabled 数据库已处于 disable=1。禁用是避免等待/重启环，不是完成 FairPlay |
| E12 | 根盘与捕获运行日志、`x9/appsync/vendor/Signing.m:40–54` | AppStore.app 及账户/商店/IDS/APNs/activation daemon 原件均在；appstored 有实际运行事件。AppSync 检查每个 Mach-O slice，非零 `cryptid` 会拒绝，不提供 App Store DRM 解密 |

### Evidence → Finding → Path

- **E1–E4 → F1：NFC 的硬件与协议入口有基础，iOS 会话仍断开 → P1：先证明 NCI 最小会话与标签读取，再适配 CoreNFC。**
- **E5–E6/E9–E10 → F2：本地 Activated 与服务身份分离，APNs/IDS 缺依赖 → P2：先验证钥匙串持久化和 activation identity 取得，再追 APNs/IDS 注册。**
- **E7–E8 → F3：商店网络错误与账户证明材料失败同时存在 → P3：按请求和进程区分网络/客户端校验/远端接受，不做一键登录绕过假成功。**
- **E11–E12 → F4：安装器和 DRM 是分离的功能 → P4：App Store 登录、下载、安装、执行和 receipt/IAP 各自验收。**

## 2. NFC：哪些是驱动工程，哪些是安全平台工程

### 普通读卡/标签通信

概念路径：

```text
SN220T 配置对应的控制器
  → I²C14 + IRQ + 电源/时钟 + NCI
  → X9 内核通路及会话管理
  → NearField/nfcd 的后端契约
  → CoreNFC 应用会话与实际标签响应
```

外围已有厂商源码，可移植总线、IRQ、reset、等待/取消、错误恢复及需要的厂商初始化。硬件协议仍需用实际控制器响应验证；Android bionic/Binder/ioctl 环境不能直接作为 Darwin 接口使用。

原生 iOS 的缺口不一定只在 IORegistry 名字：会话、权限、NCI 厂商命令、固件版本、发现/轮询、错误通知与电源管理都可能影响 nfcd。先沿日志的 `_loadHW` 失败证明首个实际边界，不直接伪造“hardware ready”。

**最小成功标准**：同一会话完成控制器初始化、发现一个已知标签、返回真实 UID/NDEF 或 ISO-DEP 响应；移开标签和取消会话产生正确结果；重复会话、黑屏恢复正常。设备测试需另行安排，本轮未发射 RF。

### 卡模拟、门禁、交通卡和 Wallet

- **HCE 或自定义 APDU 模拟**：需要控制器 routing、应用会话及协议。Android 可 HCE，不代表 iOS 第三方应用在这个 D22/iOS16 环境有同等 API/权限。
- **读取卡信息不等于复制卡**：加密门禁、交通卡或支付应用可能依赖不可导出的密钥、安全计数器和发行方授权。
- **现有 eSE 应用**：SPI 路径存在不等于可管理/复用其中的 applet；会涉及受控安全域、管理授权、持久化状态和 NFC/eSE 共享电源仲裁。
- **Wallet/Apple Pay**：除 NFC/eSE 外仍有 Apple 平台的受保护授权、支付凭据配置和服务端链路。换一个 nfcd driver 不能把原厂 eSE 自动变成 Apple 支付平台。

工作量：在控制器初始化与厂商 NCI 可复用的前提下，读标签原型可按数周级考虑；原生 CoreNFC 稳定兼容为数周至数月级。eSE/Wallet 单独立项，不承诺随普通 NFC 同步完成。

## 3. iMessage：本地账号 UI、APNs 与 IDS 是三个门

概念链路，不是本轮已验证的完整调用图：

```text
AuthKit / Apple 账户登录与所需证明材料
  + 持久化的设备/服务身份
  → apsd 的 APNs 身份认证与推送注册
  → identityservicesd 的 IDS 服务注册与设备/地址映射
  → 消息密钥、投递、收件及实际解密显示
```

### 第一门：设备和账户材料能否真正建立

目前捕获的 apsd 在取得 activation identity 阶段已经失败；本机伪设 Activated 不产生证书与匹配的私钥。身份表面字段如 ECID、序列号、设备类型一致只是兼容条件的一部分，不是密码学信任根。

AuthKit/ADI/Anisette 的失败也必须先闭合。捕获的 BAA 路径确实被执行且失败；不能把 DeviceIdentity 框架有 attestation 字符串当成所有登录流程均强制要求 SEP，也不能把桌面兼容实现的经验无条件套到 D22/iOS16。要追当前执行的策略、是否存在受支持 fallback、同一 provisioning 会话的状态和密钥依赖。

当前 AKS 的后续软件 wrap 修复有机会先消除普通钥匙串存取错误，但它不签发有效 activation identity，也不自动实现设备 attestation。需用新启动验证，不能重新启用服务后直接认定身份问题已解决。

### 第二门：APNs / IDS 真正注册

即使账户登录成功，还需真实 APNs 会话和服务 token、IDS 注册、地址到设备映射与消息保护身份。局部 IPC 返回成功、内部 registration 状态被修改，不等于服务器保存了映射或另一台设备能够投递。

本轮只证明了本地身份获取/状态错误，尚未捕获已建立身份后的远端注册拒绝，因此目前不能判定“Apple 服务器必然拒绝这台设备”或“改几个字段就会接受”。

### 第三门：邮箱身份与手机号身份分开

- **邮箱/账户身份 iMessage**：通常不要求先闭合这台机器的手机号验证短信，适合作为最小服务验收目标；但仍需要账户、推送、IDS 与消息密钥。
- **手机号身份 iMessage**：额外依赖 SIM/手机号状态及运营商验证流程，可能涉及特定验证短信。当前普通短信接入仍有调查交接，不能由普通 SMS 入口存在推导为 iMessage 手机号激活完成。

最小验收：账户地址被 IDS 真正注册 → 第二台设备发送 → X9 收到、解密和显示 → X9 回复对端收到 → 重启仍恢复有效注册。消息变蓝、出现“已激活”标签不算完成。

判断：不是可预测的一两周 UI 修复。客户端依赖可逐步修；原生服务可用性要等身份/ADI/APNs 首个有效闭环后再排期。iMessage 的独立服务端验证门槛不等于 Apple Pay，但目前仍缺可保证成功的路线。

## 4. App Store：登录、购买许可、下载与执行分开

### 已纠正的旧结论

`docs/sweet/README.md` 的早期“没有 App Store/Apple ID”不能代表当前根盘库存。原 AppStore.app、appstored、itunesstored、amsaccountsd、akd、adid 原件均在，捕获日志也有商店 daemon 运行。

这只解决“程序是否存在/是否启动”的问题，不代表登录、下载或购买已经成功。

### 验证链

```text
DNS/TLS/时间/网络路径与 Reachability
  → AuthKit 账户 + 2FA/会话 + 当前流程所需 Anisette/BAA 等材料
  → AppleMediaServices 商店会话、地区与 entitlement/购买许可
  → CDN 下载及校验
  → appstored / installd 安装与签名处理
  → FairPlay 授权/加密代码的执行
  → receipt / StoreKit / IAP 的独立业务验证
```

该图是功能分层；不能据此断言每次登录、每个 App 都执行完全相同的 attestation 流程。

### 当前可修与不可由本地假成功替代的边界

1. **网络与系统服务：可修。** `-1009` 要定位到对应请求的 route/Reachability，而不是全局宣称无网。AuthKit 同时出现 provisioning HTTP 200，说明不能把全部失败都当成 TCP/TLS 不通。时间、DNS、证书校验、接口路径和守护进程 XPC 都是应检查项，尚未逐一测过。
2. **钥匙串和账户持久化：可修兼容层，但需验收。** token/身份状态跨重启有效、正确锁状态/访问策略与失败清理，不能只让 SecItemAdd 返回成功。
3. **Anisette/ADI/BAA：有明确调用和失败证据，需深入还原。** `200 → ADIProvisioningEnd -42410 → AKAnisetteError -8004` 是当前最值得追的客户端分支；BAA 另有 MobileActivation 错误链。确定数据/密钥/身份与策略依赖后，才能判断兼容实现或受支持路径的可行性。
4. **商店会话/许可：以真实服务端结果为准。** 可打开商店页面或本地假设购买成功，不能产生服务器认可的下载许可或购买记录。
5. **FairPlay：独立的大门。** H2 目前默认禁用以规避历史重启/等待环。启用开关本身不会补出原件要求的 FairPlayIOKitUserClient，也不会完成账号授权和代码解密。免费 App 不等于没有 DRM。
6. **AppSync：现有替代安装路径，不是 App Store 修复。** `Signing.m` 明确拒绝非零 cryptid；签名/安装兼容不把密文变为可执行代码，也不提供合法 receipt/IAP 状态。

App Store 浏览/账户客户端的兼容修复值得做；完整“登录→下载→安装→运行”应按数月级且依赖身份/DRM 的项目看待。不能由当前证据承诺原版商店的所有功能必然恢复，也不应把它一概判为必须重建完整 Apple Pay 安全平台。

## 5. 最实际的推进顺序

| 顺序 | 具体动作 | 决定性结果 |
| --- | --- | --- |
| 1 | 用后续 AKS 修复后的新启动，验证非敏感测试项目的保存、读取、重启恢复和错误路径 | 排除旧 330 wrap 失败；不读取现有账户/设备密钥 |
| 2 | 对商店 -1009 与 AuthKit provisioning 200 按时间/请求关联，核对路由/Reachability 和最终错误 | 区分网络阶段与客户端验证阶段，不制造全局联网假象 |
| 3 | 从实际 ADIProvisioningEnd -42410 分支及 BAA 下层 MobileActivation 错误追到首个依赖缺口 | 确认是状态、缺少身份/密钥、接口契约还是远端返回，不猜错误枚举 |
| 4 | 证明 activation identity 获取与 APNs 最小会话，再推进邮箱身份 IDS/iMessage | 真服务注册与双向收发；手机号验证另排 |
| 5 | NFC 独立做 NCI 初始化/标签响应，然后关闭到 CoreNFC 会话 | 真硬件响应、取消和恢复；不涉及 eSE 安全域/钱包写入 |
| 6 | 身份客户端可用后再研究 App Store 原件 FairPlay/安装/执行路径 | 同一原版 App 完成许可、下载、安装、运行；不是 installer 中间成功 |

以上是建议，不是本轮发起的设备操作。最值得深入的两条路径是 **NFC NCI→标签响应** 与 **ADI provisioning 200→客户端 -42410**。iMessage 先邮箱身份能避免把手机号激活与当前短信 bring-up 混成一个不可定位的大问题。

## 6. 可复核产物与限制

报告之外的私有产物归档在 `x9/out/apple-services-assessment-20261116/`：

- `scope.md`、`timeline.md`、`workitems.md`、脱敏 `field-journal.md`。
- `input-hashes.json`：18 个限定源码/原件的路径、长度和 SHA-256；不提交 Apple 原件或设备转储。
- `launch-inventory.json`、`app-inventory.json`：程序与声明服务库存；克隆中的 disabled plist 不能覆盖捕获运行时 disabled 状态。
- `console-proof.txt`：330 AKS/launchd 的限定证据，省略 bag handle。
- `service-log-summary.json`：按进程计数/限定摘要；“包含 error 字样”只用于线索采样，不当成全部是真错误。
- `service-proof-events.json`、`current-boot-anisette-events.json`：决定性消息与时间，无账户地址、token、私钥或登录请求。
- `binary-string-leads.json`、`framework-string-leads.json`：字符串仅供定位，不作为可达性、认证成功或强制策略证据。

只读复核：

```bash
rg -n -B16 -A16 'nxp,sn-nci' .local/x9/modem/live.dts
rg -n -A6 'nxp,sn-ese' .local/x9/modem/live.dts
rg -n 'chipset-24087' .local/x9/modem/live.dts
rg -n -C7 'x9_fairplay' x9/kernel/src/bsd/kern/kern_exec.c
rg -n 'cryptid|encrypted' x9/appsync/vendor/Signing.m
python3 x9/out/apple-services-assessment-20261116/scan-service-log.py
```

最后一条只读取已有归档并输出脱敏摘要，不触发服务；须具备同一 `.local` 输入。重要结论尚无干净启动下的新登录、APNs/IDS、原版 App 下载/运行或 NFC 会话验证，因此仅评估完成，功能实现均未宣布完成。
