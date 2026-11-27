# ios-sweet / X9 当前交接（2026-11-27）

项目在 OPPO Find X9（PLJ110 / OP5E17L1，MT6993，16 KiB 页）上由 LK 启动 SWEET XNU，运行 iOS 16.7.16（20H392 / D22）用户态。当前重点是蓝牙开关、通话注册与失败定位、短信存储和投递。

## 1. 当前状态

- 本地 `main` 为 `1b13021f`，`origin/main` 为 `bb5c8557`，集成工作树在 `.worktrees/main-call-integration`。原 checkout 的 `feat/bt-settings-offline-candidate` 承载未提交改动。
- 下表首个通话／短信候选已上机，见 `~/445.txt`。刷写前确认 Android `PLJ110`、`_b`、`sys.boot_completed=1`，`adb reboot bootloader` exit 0。核对唯一设备、bootloader b 槽、64 MiB `boot_b` 后启动唯一 USB logger（会话 57101），`fastboot flash boot_b` 和 `fastboot reboot` 均 exit 0。
- `445.txt` 确认 SpringBoard 启动，6.575 s MobileGestalt epoch marker 写入成功，25.969/25.971 s 双卡 IMS 快照均为 0，26.017/26.022 s 双卡 `4904` 诊断订阅确认。用户确认蓝牙可搜到设备，配对、重连和音频未测。
- 用户报告拨号时卡死。`445.txt` 在 48.518 s 触摸抬起处截断，1,605,591 B。采集器随后报 libusb -99 并断开，保留会话。末段未见 `VOICE20`、`0901` 或 panic。只读确认设备现为唯一 bootloader fastboot，`is-userspace: no`、`current-slot: b`。未发送新重启或刷写命令。下一次授权启动时先采 prior reset/pstore，再定位拨号入口。
- 用户授权同镜像复测。封存 `445.txt`，旧 logger exit 0。重新确认唯一 bootloader b 槽后启动 `~/446.txt` 新 logger（会话 10004），`fastboot reboot` exit 0，本次未刷写。
- `446.txt` 已捕获 SpringBoard、双卡 IMS 快照 0 和 `4904` 订阅成功。先前复位的 TOPRGU `STA=0`、`NONRST=0`、`NONRST2=0x40002000` 与 445 开头一致，未取得能归因的旧 panic 或 pstore 内容。69.764 s SIM2 发出 `0901`，基带确认成功并报 ECPI130。75.929 s 收到挂断操作后发出 `0905`，75.936 s ECPI133/16，CallList 发布 END。该窗口未见振铃或接通，之后日志持续至 561.514 s。
- 用户要求停到 fastboot。主机最初未枚举到设备，`python3 -u x9/tools/ios-reboot-bootloader-usb` 等待 60 s 后 exit 1，未发出重启请求。用户随后手动进入 fastboot，主机确认唯一设备、`is-userspace: no`、`current-slot: b`。封存 `446.txt`（3,964,400 B），未检测到运行中的 USB logger。
- Terra 离线复核 `446.txt`。SIM2 IMS 快照早于拨号约 47 秒（9427、20666），挂断前约 6 秒内无振铃，旧 441 的自然失败 `2172` 需另行对照。SIM1 短信在 86.425 s 得 `0606/0x21014b`，两个独立的 44 B 重试返回 `0x5e`，未见后续基带发送（22767 起）。SIM2 三次 `0606=0` 均有主机入队收执且未请求状态报告，投递仍待对端证实（19763、23566、34226）。双卡 `0608=0x210000` 无法区分 CPMS 与 CMGL 失败（17674 起）。后续分别补拨号窗口 IMS 状态、重试 TLV 形状和同卡 CPMS/CMGR 诊断。未改代码或操作设备。
- 本轮刷入拨号窗口 IMS／短信诊断候选。核对唯一 bootloader fastboot、b 槽和 64 MiB `boot_b` 后启动 `~/447.txt` 单一 USB logger（会话 30969），`fastboot flash boot_b`、`fastboot reboot` 均 exit 0。日志已到 SpringBoard、双卡 IMS 快照 0、双卡短信 host-ready。双卡 CPMS 均返回可解析响应，CMGR index 1 均返回 `0x210000`。logger 保持运行，待拨号和实际收发复测。
- 用户报告双卡拨号和短信均失败。`447.txt` 仅见一笔 VOICE20，归属 SIM2。69.420 s ECPI130 与同卡 IMS 原始状态 0，约 32.6 s 无振铃或接通，102.018 s 发送挂断 `0905` 后回 `133/16`。SIM1 本轮未见 VOICE20 到桥，源码仍仅准入已验证的 SIM2 拨号路径。SIM1 短信首发回 `0x21014b`，两次重试均带 `0x14/1B` 和 `0x15/4B`，桥回 `0x5e`，字段值和重发语义待核。SIM2 三次短信均获基带受理且未请求状态报告，当前日志无投递证明。
- rev 284 Metal/PanVK mailbox 已写入并逐段读回确认，共 53,411,840 B。iOS 安装与 GPU 性能尚未验收。

| 离线候选 | 镜像与边界 |
| --- | --- |
| 通话 / 短信流程更新 | `.worktrees/main-call-integration/x9/out/x9-call-sms-flow-20261127-r1/boot.img`，15,400,960 B，基线 `bb5c8557` 加本工作树补丁，启用 `x9_call_sip_diag=1` |
| IMS 配置缓存 raw 解码 | `.worktrees/main-call-integration/x9/out/x9-radio-feature-decode-20261127-r1/boot.img`，15,388,672 B，内核 31,997,672 B，基线 `bb5c8557` 加工作树补丁。启用 `x9_call_sip_diag=1 x9_sms_diag=1 x9_sms_srr=1`，未刷写 |
| 拨号窗口 IMS / 短信诊断 | `.worktrees/main-call-integration/x9/out/x9-radio-dial-sms-diag-20261127-r1/boot.img`，15,405,056 B，内核 31,997,464 B，基线 `bb5c8557` 加工作树补丁。启用 `x9_call_sip_diag=1 x9_sms_diag=1`，本轮刷入 |
| 双卡 SMS 状态报告请求 | `.worktrees/main-call-integration/x9/out/x9-radio-sms-srr-20261127-r1/boot.img`，15,405,056 B，内核 31,997,464 B，基线 `bb5c8557` 加工作树补丁。启用 `x9_call_sip_diag=1 x9_sms_diag=1 x9_sms_srr=1`，本轮刷入 |
| 通话 IMS / SIM2 失败诊断 | `.worktrees/main-call-integration/x9/out/x9-call-ims-failure-20261127-r1/boot.img`，15,400,960 B，源码 `95f18613`，标准设备配置 |
| 蓝牙能力缓存 | `x9/out/x9-bt-mgwipe-20261126-r2/boot.img`，15,417,344 B，复用 444 已装诊断 dylib，`x9_bt_root=0 x9_bt_settings_diag=1` |
| rev 284 无线诊断 | `x9/out/x9-radio-284-diag-20261126/boot.img`，15,417,344 B，`x9_bt_root=1 x9_bt_settings_diag=1 x9_sms_diag=1` |
| rev 284 mailbox 包 | `x9/out/x9-metal2vk-mailbox284-20261126/`，保留 stage 日志及匹配包 |

拨号窗口 IMS／短信诊断候选用于 447，双卡短信状态报告请求候选用于 448。IMS 配置缓存 raw 解码候选未刷写。`build.log` 与各镜像同目录。首个候选使用 LLVM 22、MacOSX 14 SDK 执行 `python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_call_sip_diag=1 --name x9-call-sms-flow-20261127-r1`，exit 0，内核 31,997,160 B。MacOSX 14 SDK 下 `python3 -I -m unittest discover -s x9/baseband -p 'test_*.py' -q` 24/24，`python3 -I x9/tools/test_x9_call_bridge.py` 使用 Homebrew clang++ 与该 SDK，ASan/UBSan 85/85，均 exit 0。通话 C++ 全集基线另有四个失败用例，未计入此套件，详见旧 §6.66 和 §6.75。

## 2. 环境与固定输入

### 2.1 工具

Xcode 15、MacOSX 14 SDK、KDK `KDK_13.5_22G74`、Homebrew LLVM/lld 22、ccache、Python 和 lz4。私有构建配置为 `x9/build/local.json`。编译器和 SDK 路径以本机配置为准。

### 2.2 构建与依赖文档

- [构建契约](x9/BUILD.md)与[外部依赖](x9/EXTERNAL-DEPENDENCIES.md)记录工具链、来源和布局。
- XNU 改动放在 `x9/xnu/` 补丁栈并通过 `x9/xnu/build-xnu.sh` 构建。overlay 改动放 `x9/kernel/src/`，新增对象同步 `recipe.json` 和 `link-order.txt`。
- 并行实验使用隔离工作树及独立输出目录。保留私有输入、分区备份和其他工作树的未提交改动。

### 2.3 `.local/` 固定布局

- `dxb/`、`dxb-iosabi/` 为 XNU 对象树，`apple/ios16-20H392/` 和 `apple/dsc-20H392/` 为 Apple 原始输入。
- `references/`、`vendor/` 保存固定上游参考与板级源文件。X9 固件、modem、运营商包、根盘与备份均在 `.local/x9/`。
- iOS 根盘为现有 `userdata` 内 pinned/immutable 的 32 GiB HFSX extents，由 `ufsroot0` 挂载。UUID、extent 和设备标识只从私有 manifest 读取。
- 构建只使用本仓库和固定工具链。完整输入列表以校验脚本为准。

```sh
python3 -I x9/build/prepare-local.py --check
```

## 3. 构建与设备

完整候选使用 `boot-device.json` 和从未用过的输出名。boot args 上限约 607 B。

```sh
PATH=/opt/homebrew/opt/llvm@22/bin:$PATH \
  python3 -I x9/build/build.py all \
  --boot-config x9/build/boot-device.json --name <unique-name>
```

单独编译使用 `build.py kernel`，仅打包用 `build.py boot --kernel <kernel>`。插件在 mailbox/root，当前内核 allowlist 为 284。插件 revision 或 hash 变化才需要匹配的重新 staging/安装。纯内核候选可以复用已 stage 的 284 包。

### 3.5 设备步骤

1. 每次刷写、重启和 root/mailbox 操作按 [AGENTS.md](AGENTS.md) 取得明确授权。设备只写 `boot_b`，不支持 `fastboot boot`。
2. iOS 回 bootloader 用 `python3 -u x9/tools/ios-reboot-bootloader-usb`。独立确认唯一 fastboot 设备、`is-userspace: no`、`current-slot: b`、`partition-size:boot_b: 4000000`。任何条件不符即停。
3. 经授权启动 iOS，先用 `python3 -u x9/tools/usb-live/usb_log.py --output ~/NNN.txt` 开唯一 logger 并选新日志文件。刷写成功后再重启。保留本轮完整日志。
4. 恢复 Android 使用用户指定的 `/Users/tosasitill/boot.img`（67,108,864 B）。刷写成功后才重启。确认唯一 ADB 设备、`PLJ110`、`_b` 和 `sys.boot_completed=1`。
5. root/mailbox 当次执行 preflight 和私有 extent manifest 核对，禁止改其他分区、切槽或 live sysfs 探测。

## 4. 优先验证

- 448 WDS 离线复核（基线 `bb5c8557` 加现有集成补丁，报告提交 `1b13021f`）。完整事务有两笔 SIM1 默认数据 `0x20` 和两笔 `0x2d`，无可关联到 SIM2 IMS 的 WDS Start/runtime settings。`0x5566` 为蜂窝数据状态（8 次 `0x5e`），`0x5568` 为动态数据换卡许可（3 次 `0x5e`），`0x5569` 两次确认 SIM1 默认路由。Apple IMS runtime settings 读取 `0x23`/`0x2e` 代理地址及 `0x32` S-NSSAI，待定位同卡 MD 来源与 IMS context。文档中的 `llvm-objdump` / `awk` 复核命令 exit 0。`SDKROOT="$(xcrun --sdk macosx --show-sdk-path)" python3 -I -m unittest discover -s x9/baseband -p 'test_*.py' -q` 25/25 exit 0，`CXX="/opt/homebrew/opt/llvm@22/bin/clang++ -isysroot $(xcrun --sdk macosx --show-sdk-path)" python3 -I x9/tools/test_x9_call_bridge.py` 86/86 exit 0（MacOSX 14 SDK）。无桥行为改动、构建、CI、设备操作或 push。下一次取得授权后对齐实际 bundle、同卡 IMS context/P-CSCF 和 SIP REGISTER，再判断 SIM2 振铃故障。见 [WDS 复核](docs/network/2026-11-27_ims-wds-report.md)。

- VoNR／视频／WFC／IMS SMS 续查（离线；仅只读诊断）。原厂 SIM2 `radio-baseline.txt:1117–1266` 见 VoNR 和 LTE/NR 视频启用，WFC 与 Wi‑Fi 视频关闭；原厂拨号仍会振铃。ASC `MtkMmTelFeature.convertCapability` 对应 LTE/NR 语音 feature 0/6、LTE/NR 视频 1/7、IWLAN 语音/视频 2/3。20H392 `updateVoNRState` 已定位 NAS `010b`、tag1/u32、值由 enum==1 得出，447/448 未见该请求。`5562` 是 iOS Wi‑Fi 通话状态上报，448 有拒绝日志，不能代作 WFC 开关。原厂 IMS SMS 经 `MtkImsSmsImpl → ImsService → 按 phoneId 选 ImsRILAdapter → RIL 2133/sendImsSmsEx`，iOS 另有按 PersonalityID 创建的 SMS client。下一步闭合原厂 2133 的运行样本、MD 域选择与入站 IMS ACK，及 iOS 运行时 bundle 选择、IMS PDN 与 SIP REGISTER。定点 ASC 和 Apple 汇编保存在 `x9/out/ims-carrier-policy-20261127/`。设备保持 fastboot，未刷写。

- 本轮静态闭合 Apple 逐 PersonalityID 的 IMS 策略输入，并更正 CommCenter 路径：主程序在 `System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter`（24,740,528 B，本地符号已剥离）。`VoLteControl::initialize` 绑定逐 `SimSlot` 的 `ims_voice_support`、`ims_preference_states`、`current_data_slot`、`operator_bundles`、`in_home_countries_definite`；`/cc/props/volte_and_vowifi_prov_state` 为 `map<PersonalityID, VolteAndVoWiFiProvisioningState>`，由 `sd::IMSSubscriberEventHandler::handleVolteAndVoWiFiProvStateChanged_sync` 的 `$_12`–`$_15` 转成 `BoolWithUnknown`，再进 `SystemDeterminationManager::handleVoLTE/VoNRFeatureSupportChanged_sync(PersonalityID, …)`；解析使用 `map<PersonalityID, shared_ptr<IMSSubscriberControllerInterface>>`，不含 SIM slot。模型 `updateVoLTEEnabled/updateVoNREnabled` 写各自 `DataCache+0x38/+0x39`，`evaluateImsFeature_sync` 依据注册状态重算并打印 `IMS Feature: VoLTE/VoNR feature is turned`，`evaluateIMSServiceMasks` 产生 cell/wifi 掩码。Carrier 事实：20H392 无 `46015` bundle（全 plist 无该串）；`46000` D22 override 设置 `Voice.EnableVolteByDefault=true` 与 SMS `allowCSFBInVolteMode=true`，省略 `EnableVoNRByDefault`。同一 override 提供 `IMS` APN（type-mask `131072`）、`SupportsImsCapability=true` 和 `AttachAPN`。`carrier.plist` 和 `Default.bundle` 的原始 plist 均无 IMSConfig。两卡的实际 bundle 与 provisioning 待实机确认。桥仍只准入 NAS `0x20/0x24/0x4d/0x4f/0x5556`，`0x10b`/`5562` 未证明映射，不放行；诊断仍为只读 `0a02`、拨号 `0a04`、可选 `4904`。主机基带 25/25、通话 86/86 exit 0（SDK 26.5）。详见 [iOS 输入链](docs/call-ios-bridge.md#apple-逐-personalityid-ims-策略静态输入链)。

- 本轮核对原厂 ASC `ImsConfigUtils`、`MtkImsConfigImpl`、`ImsConfigController`、`ImsRILAdapter`、`MtkMmTelFeature`，逐卡 RIL 2136 feature 批次和 2077 聚合入口已定位。固定 MD class 0 `0a02` 返回六字节同卡缓存，现有桥已接只读 `0a02/0a03` 和 `NAS33` 路由／意图诊断。有效运营商策略输入、逐 PersonalityID 绑定及 `0a00` 持久化/驻留副作用仍未闭合。候选 `x9/out/x9-radio-ims-feature-20261127-r1/boot.img` 15,405,056 B，内核 31,997,672 B，`bb5c8557` 加集成补丁；`python3 -I x9/build/build.py all --boot-config x9/out/x9-radio-sms-srr-20261127-r1.boot-device.json --boot-arg x9_call_sip_diag=1 --boot-arg x9_sms_diag=1 --boot-arg x9_sms_srr=1 --name x9-radio-ims-feature-20261127-r1` exit 0。基带主机回归 24/24，exit 0，覆盖逐卡隔离、错卡确认和超时。设备无操作，候选未刷。私有 ASC 输出和 MD 定点反汇编在 `x9/out/ims-carrier-policy-20261127/`。

- 本轮静态闭合原厂 2136 → native `0xcf18` → `0xcf11` → Rmm `0a00/tag102`，并更正严格映射：RIL 2136→`0xcf18`、2077→`0xcf10`，`0xcf11` 是 native 内部构造。`RtmImsController::onHandleRequest` 对 cf10/cf11 都先 `cacheimscfgparams` 再 `requestToMcl`；cf10 在 featureVersion≥2 且 status==1 时把前两个 int 的 `1` 改写为 `3`，cf11 不做。`RmmImsControlRequestHandler` 把两者同一路由到 `requestSetImsCfg`，最终发送 `0a00` TLV100=0、TLV102 六字节；`imstestmode`/`imsdisablesms` 只修正 byte4/5。六字节由逐卡 VoLTE/ViLTE/WFC/ViWiFi、全局 IMS SMS、VoNR/ViNR、dynamic switch 和 status/key1004 门控聚合；byte0/byte1 的 `3` 是语音/视频位组合，byte3=WFC&ViWiFi，byte5 非 Java eims 直接复制。2136 路径还会写回 `persist.vendor.mtk.volte/vilte/vonr/vinr.enable` 并调用 SIP UA；直接注入 `0a00` 不会复现这些副作用。未闭合项仍是 `0a00` 之后的持久化、驻留重注册和运营商门控运行时效果。2133 GSM 则经 AIDL/native 汇合到 MIPC `0605`，TLV104 仅属内部 `DEB9`；这证明 AP 传输汇合，不证明 IMS 域选择、承载或投递。共享 `4606/4607/4603/4609`、`8602/8604` 不可单独当作 IMS ACK。当前桥只把 `0a02/0a03` 六字节同时打印为 raw 与 VoLTE/VoNR/ViLTE/ViNR/WFC/ViWiFi/SMS/IMS 位，不新增 `0a00` writer。主机 `python3 -m unittest x9.baseband.test_ims_boot_snapshot -v` 在 Xcode 15/MacOSX14 SDK 下 2/2 exit 0（含 raw/位解码和超时/错卡）；完整 `python3 -I -m unittest discover -s x9/baseband -p 'test_*.py' -q` 25/25 exit 0。随后用 `x9-radio-feature-decode-20261127-r1` 新名称完成离线构建，exit 0，boot 15,388,672 B、内核 31,997,672 B；未刷写、未重启、未做设备验收。上文 r1 候选早于该日志解码，不能拿来宣称已观测 raw 值。详见 [2136/2133 汇合](docs/call-ios-bridge.md)。

- 原厂 SIM2 在 `0901` 前已 IMS 注册，拨号后 3.584 s 报 ECPI2，3.835 s 捕获 SIP 180。448 同卡两笔拨号后的 `0a05` 均返回状态 0，未见 ECPI2/132/6 或 SIP 诊断事件；435/441 的自然失败 2172 指向 MD 待处理 CC 服务拒绝。优先追真实 IMS feature/注册及承载。原厂 `0a00` 前后分别有注册与 MmTel Voice 证据，不能只补配置 ACK。离线分析未操作设备、未改桥接代码。见 [拨号对照](docs/call-ios-bridge.md#2026-11-27原厂振铃与-448-拨号窗口对照)。

- 本轮 `python3 -u x9/tools/ios-reboot-bootloader-usb` exit 0，独立确认唯一 bootloader fastboot、b 槽、64 MiB `boot_b`。旧 logger exit 0，保留 `447.txt` 4,312,589 B。启动 `~/448.txt` 单一 logger（会话 53540）后，刷写状态报告镜像及重启均 exit 0。
- `448.txt` 已见 SpringBoard，25.469/25.471 s 双卡 IMS 快照均为 0，25.513/33.993 s 分别确认 SIM1/SIM2 短信 host-ready。用户报告双卡通话、短信均失败。捕获两笔 `0901`（token 885、1316），均走 SIM2，`0902=0`、拨号时 IMS 原始状态 0、ECPI130 mode 0，未见振铃或接通。两笔分别在约 25 s 和 34.6 s 后由主机 `0905` 挂断，均回 ECPI133/16，未采到自然失败原因。当前源码仍固定 SIM2 拨出 profile。
- `448.txt` 四笔首发均请求 TP-SRR。`0606` 分别为 SIM2 token 658 `0x21087c`、SIM1 token 1167 `0x21014b`、SIM2 token 1168/1530 `0`（基带受理）。两次失败后的 RawSend 重试均带 `0x14/1B`、`0x15/4B`，在桥内拒绝。封存时仍未捕获 `8603`。
- 本轮 `python3 -u x9/tools/ios-reboot-bootloader-usb` exit 0，独立确认唯一 bootloader fastboot、b 槽。旧 logger 已停止，`448.txt` 封存为 3,685,384 B；设备停在 fastboot，本轮未刷写。
- `bb5c8557` 加集成工作树改动。双卡显式 GSM RawSend 重试要有 `0x14=1` 和 `0x15/u32`，后者须能装入一字节 TP-MR。按 AOSP 给私有 `0605` SMS-SUBMIT 副本设 TP-RD 与 TP-MR，保留 TP-SRR 和现有 owner；无自主重发。启用通话诊断时记录拨号拒绝前的已绑定 SIM 与错误。SIM1 原生拨号和 SIM2 IMS 域选择需要原厂同卡拨号及域配置证据。本地基带 24/24、通话 86/86，均 exit 0（MacOSX14.0 SDK、通话 CXX 指定 LLVM 22 和 `-isysroot`）。候选 `x9/out/x9-radio-sms-retry-20261127-r1/boot.img` 15,405,056 B，内核 31,997,464 B。`python3 -I x9/build/build.py all --boot-config x9/out/x9-radio-sms-srr-20261127-r1.boot-device.json --boot-arg x9_call_sip_diag=1 --boot-arg x9_sms_diag=1 --boot-arg x9_sms_srr=1 --name x9-radio-sms-retry-20261127-r1` exit 0。重试、投递和拨号均待实机验证。

- 依据 [AOSP GSM SMS-SUBMIT](https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android-15.0.0_r1/telephony/java/com/android/internal/telephony/gsm/SmsMessage.java) 补双卡诊断开关 `x9_sms_srr=1`。已准入的 GSM RawSend 在 `0605` 私有副本中设置 TP-SRR 位，保留 SIM 路由、事务和现有 `8603/8604` 路径。SIM1 首发已到基带，`0x21014b` 和重试语义待核。SIM1 拨号缺固定 Android 同卡样本，现有实测仅支持 SIM2 的 profile，未扩展准入。基带主机回归 24/24，`prepare-local.py --check` 通过。`build.py all --boot-config x9/out/x9-radio-sms-srr-20261127-r1.boot-device.json --boot-arg x9_call_sip_diag=1 --boot-arg x9_sms_diag=1 --boot-arg x9_sms_srr=1 --name x9-radio-sms-srr-20261127-r1` exit 0。配置省略板级默认 `x9pl=1`、`x9_wifi_power=6` 以满足 boot-args 上限。无设备操作。

- 新诊断候选在 SIM2 拨号准入后追加一次只读 `0a04`，日志用原拨号 token 对齐 `0a05` 原始状态。短信 `0x5e` 仅记录非 PDU TLV 类型和长度，诊断含现有只读 CPMS/CMGR。主机回归基带 24/24、通话 86/86，`prepare-local.py --check` 通过。执行 `python3 -I x9/build/build.py all --boot-config x9/out/x9-radio-dial-sms-diag-20261127-r1.boot-device.json --boot-arg x9_call_sip_diag=1 --boot-arg x9_sms_diag=1 --name x9-radio-dial-sms-diag-20261127-r1` exit 0。候选配置省略板级默认 `x9pl=1` 以满足 boot-args 上限。无设备操作。

- **蓝牙**：444 有 HCI 扩展扫描和 120 条广播，但 Settings 走 XPC invalid → state 2。固定 20H392 的两个入口都受 MobileGestalt `bluetooth` key 门控，旧缓存值尚未采到。下一次授权上机，先验证缓存 epoch 的 `X9 MGWIPE`、Settings 状态、开关和实际扫描。关机 H4 空回调崩溃单独跟踪。见 [Bluetooth README](x9/bluetooth/README.md)。
- **通话**：435/441 的 SIM2 在 ECPI130 后未振铃或接通，最终为 133/2172。新代码在 OPEN 后逐卡订阅 `4a01`，读取 `0a04/0a05`，并在 SIM2 异常拨出终态后采集同卡 `090f/0910`。ECPI133 保有通话身份与 END 归属。下一步对齐同卡 IMS 快照、RAT 和失败事件，运营商配置、IMS PDN/SIP、CC/CSFB 原因和双向音频待闭合。见 [通话桥](docs/call-ios-bridge.md)。
- **短信**：双卡存储列表 `0608=0x210000` 可来自 CPMS 或 CMGL 失败。先用 rev 284 无线诊断候选确认同卡 CPMS/CMGR，再对照投递及真实入站 `8601/8603`。SIM2 `0606=0` 只确认基带受理。见 [基带 README](x9/baseband/README.md)及[短信结果链](docs/baseband/sms-result-path-20261126.md)。
- **本轮增量**：`090f` 和启动 IMS `0a04` 查询超时后释放只读 token，迟到确认无法匹配新事务。可选 `4904` 仅记录逐卡 SIP 标量，不进入通话事件队列，其专用订阅效果待实机确认。短信终态仅等待主机收执时放行同卡下一项存储请求，原生未结束或结果不确定时仍保持串行。`0601/108` 缺 iOS owner。`4606` 与 `8603` 为不同基带路径，仍缺 `4606` 的 iOS 交付合同。

- 原厂初始化复现（`x9_factory_init=1`，默认关）：诊断会话（`~/radio-r287-20261127.txt`）确认 iOS 启动下双卡 IMS feature cache 全零（`raw=000000000000`）、注册快照 0；原厂基线（本机 `radio-baseline.txt`，2026-11-15）逐卡在 init 期发送 `0a1c`（仅 SIM1，tag 0x100 长 64 全零）→`0a18`（0x100=0）→`0a00`（0x100=0、0x102=`030300000101`），确认 `0a1d/0a19/0a01` 均有真实往返。本轮实现逐卡同序写入：六字节由聚合函数按本机原厂策略输入生成（VoLTE/VoNR/ViLTE/ViNR 开、WFC/ViWiFi 关、IMS SMS 与 eims 开），写后回读 `0a02` 缓存留证；注册证据仍归 4a01/0a04 观察者，配置 ACK 不写成注册。短信 init 在配置包确认后、host-ready 前补发原厂 `0601` tag 0x108=1（同一门控）；原厂 `060f` CBM 捕获含未初始化指针字节，不复现且与 MT 投递无关。主机新增 `test_factory_init.py`（帧序、六字节、拒绝即停）通过；基带套件 18 项中 2 项错误与未改 main 相同（`test_call_owner_boundary`、`test_sms_pdu_class` 的 SDK 27.2 tapi 环境问题，非本轮引入）。候选 `radio-factory-r287b-20261127`（纯内核、allowlist 由已 stage 的 287 mailbox.json 再生、boot 参数 `x9_factory_init=1 x9_call_sip_diag=1 x9_sms_srr=1`，合计 607 字节恰在 mkpayload 上限内）未刷，待上机验收：写后缓存应为 `030300000101`，随后观察 4a01 注册变化、入站短信与 SIM2 拨号。

## 5. 其他子系统

- **GPU / SharedEvent**：主线已有 SharedEvent 接线和 rev 284 Metal2Vulkan 热路径优化。止崩、高刷、性能和降温仍需匹配样本。见 [Metal README](x9/metal/README.md)及[SharedEvent 合同](docs/graphics/shared-event-contract-20261126.md)。
- **相机 / 触觉**：主线保留 SMMU 队列表复用与会话重开路径。非黑多帧及稳定退役未验收。`diag35` 提前 reset 实验仅有旧构建证据。它及三个 A/B/bisect 分支已删除，经过 `git bundle verify` 的归档位于 `x9/out/retained-worktree-artifacts-20261127/`。见 [相机 README](x9/camera/README.md)。
- **传感器 / 唤醒**：ALS、距离与姿态仍需物理动作对照。抬腕可先复核 SCP pickup type 75 的真实 ACK/DATA，触摸手势与 iOS 唤醒接受路径待证。
- **USB / AppSync / 音频**：AppSync v6 有主机工具和运行时链，Mac 配对与安装待实测。双向通话 PCM 与原生蓝牙音频待接通。见 [AppSync](x9/appsync/README.md)、[USB](x9/usb/README.md)和[扬声器](x9/audio/README-speaker.md)。

## 6. 历史与交接

历史编号、精确命令和逐轮日志见 `git show bb5c8557:NOTES.md`。更早证据按 `git log -S'<关键字>' -- NOTES.md` 追溯。历史产物路径可能因清理失效，以 §1 为准。原始日志、设备标识、私有输入和备份不入 Git。更新本页时区分实机观察、推断、主机验证、候选就绪与功能验收。
