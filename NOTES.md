# ios-sweet / X9 当前交接（2026-11-26）

> 本文件只保留目标、固定输入、构建流程、当前候选、验证边界与下一步。旧实验流水账不再逐轮复制；需要历史细节时使用：
>
> ```bash
> git log -S'<关键字>' -- NOTES.md
> git show <commit>:NOTES.md
> ```
>
> 整理前版本为 4,111 行；其内容仍可从本文件的父提交 `994d5fe0` 读取。每次里程碑更新本文件，明确区分**观察、推断、host 验证、设备验证**。

## 1. 目标与安全边界

- 在 **OPPO Find X9（PLJ110 / OP5E17L1，MT6993，12 GiB RAM，1256×2760，Mali v14）** 上由 MediaTek LK 直接启动自编译 XNU（xnu-8796.141.3 + SWEET/X9 补丁），运行**未修改的 iOS 16.7.16（20H392，iPhone X / D22）用户态**。
- 启动链：LK → Android boot.img v4（legacy LZ4 arm64 Image）→ X9 shim → SWEETXNU → 原生 UFS/HFSX 根盘 → launchd。
- 根盘位于现有 `userdata` 的 pinned/immutable extent：HFSX base 16 GiB + extension 16 GiB = **32 GiB**；由 `ufsroot0` 挂载。extent 表和序列号只在私有 `root-manifest.json`。
- 显示：物理面板 1256×2760、stride 5120、fb `0xfc910000`；ADT canvas 1242×2760（414×920 pt @3×，左右各 7 px 黑边）。
- 长期目标：Mac 配对、AppSync 安装 IPA、完整 rootless 越狱（Procursus `/var/jb`，SSH 只走 `ue0`）；以及原生 Wallet 交通卡和锁屏刷卡。当前均未完成，不得代做资金/SE 操作。
- **设备不支持临时 `fastboot boot`**。仅在用户明确授权后写 `boot_b` 并重启；不改 GPT、bootloader、recovery、modem、persist、备份或无关 root extent。每个破坏性步骤单独确认。
- 禁止 live sysfs 探测、盲目全盘/全仓搜索和未经授权的设备操作。不得伪造传感器样本、ACK、安装结果、帧率或性能提升。
- 构建只依赖本仓库和固定编译工具。设备转储、序列号、Apple 产物、私有 manifest、原始日志与候选均不入 git。

## 2. 环境与固定目录

### 2.1 工具与日志

- 构建 Mac：Xcode 15（Apple clang 15 / MacOSX14 SDK）、`/Library/Developer/KDKs/KDK_13.5_22G74.kdk`、Homebrew LLVM/lld 22、ccache、Python 与 lz4。
- 权威仓库为本机 `~/ios_plj110`。不要把构建指向 `~/HelioDarwin` 或其它外部源码树。
- 用户设备日志保存为 Mac `~/NNN.txt`；只读 USB EP0 采集使用 `python3 x9/tools/usb-live/usb_log.py --output ~/N.txt`。
- 当前设备所在模式必须在操作前重新确认；历史 NOTES 中“boot_b holds diag17”不是当前设备状态。

### 2.2 仓库布局

| 路径 | 内容 |
| --- | --- |
| `x9/kernel/` | overlay 源码、`recipe.json`、`link-order.txt`、生成头、prebuilt；当前完整构建通常为 104/104 对象（3 prebuilt），以当次日志为准 |
| `x9/xnu/` | XNU 完整 delta 与 ABI 补丁、`build-xnu.sh`；XNU 本体改动的唯一来源 |
| `x9/shim/`、`x9/tools/` | 裸机 shim、FDT/ADT/payload/boot.img、日志与主机工具 |
| `x9/build/` | `build.py`、`config.example.json`、`boot.json`、`boot-device.json`、插件构建脚本 |
| `x9/metal/`、`x9/mailbox/` | Metal/PanVK 桥与独立插件 staging |
| `x9/bluetooth/`、`baseband/`、`camera/`、`audio/` | 分系统桥与生命周期实现 |
| `x9/appsync/`、`x9/usb/` | AppSync runtime 与 Mac 安装/USB 工具 |
| `docs/` | 设计、逆向与历史报告；`docs/sweet/` 是 SM7150 档案 |
| `.local/`、`x9/out/` | 私有输入与 scratch/构建产物，gitignored |

### 2.3 `.local/` 固定输入

不要在其中新建实验目录。新增长期输入时同步更新 `x9/build/prepare-local.py`。

| 路径 | 内容 |
| --- | --- |
| `.local/dxb/` | XNU macOS kext ABI 对象树 |
| `.local/dxb-iosabi/` | XNU iOS kext ABI 对象树，供原始 USB kext prelink |
| `.local/apple/ios16-20H392/` | 20H392 IPSW 内容 |
| `.local/apple/dsc-20H392/` | 设备 dyld shared cache 静态分析输入 |
| `.local/references/` | UniXNU、XnuPrelink、hackios、Mesa/panfrost 等固定参考 |
| `.local/references/oppo-kernel-mt6993/` | OPPO GKI 源码 |
| `.local/references/oppo-modules-dt-mt6993/` | OPPO 厂商模块与设备树 |
| `.local/vendor/oppo-camera-imgsensor-common/` | 相机 imgsensor 源码 |
| `.local/x9/wifi-fw/`、`.local/x9/wifi-nvram/WIFI` | MT6653 固件与 NVRAM |
| `.local/x9/bluetooth/android-mt6653/` | Android 原始蓝牙 HAL、`btinit.bin` 与配置 |
| `.local/x9/modem/` | mdota、modem/nvdata/nvcfg/protect 备份、live DT、vendor-bin |
| `.local/x9/baseband/` | 私有 Mav17 ABM 与生成 pack |
| `.local/x9/carrier/CBN_cn_iPhone.ipcc` | 运营商包 |
| `.local/x9/root-extension/20261110-16g/root-manifest.json` | 私有 root extent 表 |
| `.local/x9/root/root-20261109.clone.hfs` | 16 GiB 根盘工作克隆 |
| `.local/x9/backups/partitions/` | 分区/固件备份，绝不删除 |
| `.local/x9/usb/native-20H392/` | 原始 USB kext kernelcache 与 BuildManifest |
| `.local/x9/appsync/runtime-20261112-v6/` | 当前 v6 runtime；私有 `local.json` 指向此处 |
| `.local/x9/MaliMetal/`、`.local/x9/work/x9-metal-render/` | 固定插件构建链接点和缓存树 |

```bash
python3 -I x9/build/prepare-local.py --check
```

## 3. 构建、mailbox 与上机

### 3.1 一次性配置

```bash
cp x9/build/config.example.json x9/build/local.json
python3 -I x9/build/prepare-local.py --check
```

`local.json` 是 gitignored 私有文件。工具路径可指向仓库外；所有固件、root、相机、运营商与参考输入必须保持上述仓库内布局。

### 3.2 XNU 对象树

只在 XNU 本体改动时重建：

```bash
x9/xnu/build-xnu.sh macos          # .local/dxb
x9/xnu/build-xnu.sh ios            # .local/dxb-iosabi
x9/xnu/build-xnu.sh macos --setup  # 从零准备，需要网络
```

- 在 `.local/dxb/xnu` 修改后，用仓库脚本规定的临时 index 生成 `x9/xnu/0001-…patch`，保证补丁是唯一真相。
- 并行工作不得重建其它会话共享的 `.local/dxb*`。需要隔离时：
  `x9/xnu/build-xnu.sh macos --isolated x9/out/<unique-name>/dxb`，再用输出目录内的私有 `local.json` 指向该树。
- 构建成功判据是真实重新链接且日志无编译错误；dSYM/install 失败不可忽略。

### 3.3 完整设备候选

```bash
PATH=/opt/homebrew/opt/llvm@22/bin:$PATH \
python3 -I x9/build/build.py all \
  --boot-config x9/build/boot-device.json \
  --name <unique-name>
```

其它形式：

```bash
python3 -I x9/build/build.py kernel --name <unique-name>
python3 -I x9/build/build.py boot --kernel <kernel> \
  --boot-config x9/build/boot-device.json --name <unique-name>
python3 -I x9/build/build.py all --revision <N> --name <unique-name>
python3 -I x9/build/build.py --dry-run
```

- 产物在 `x9/out/<unique-name>/`；同名会覆盖，每次使用新名。
- `boot.json` 只有基础参数；**完整设备候选必须使用 `boot-device.json`**。实验参数用 `--boot-arg` 追加，boot-args 约 607 B 上限。
- overlay 新对象须同时改 `kernel/src/`、`recipe.json`、`link-order.txt`。
- LLVM 22 的 `llvm-objcopy`/`llvm-nm` 不在 PATH 时，显式设置 `X9_OBJCOPY`、`X9_NM` 或前置 LLVM bin。
- 构建成功后不自动追加解包、重算 hash、反汇编、对比或测试套件，除非用户明确要求。

### 3.4 插件 / mailbox

- 当前 allowlist 为 **revision 280**，见 `x9/kernel/generated/x9_render_external_manifest.h`。
- 插件不在 boot.img。内核校验精确 path/size/SHA-256；revision 或任一哈希变化都会改写 allowlist。
- 新插件包需要匹配 mailbox 与 root 安装。纯内核改动且 allowlist 不变时无需 staging。
- 只有获得明确授权后才运行：
  `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/<name>`。
- staging 与安装都必须通过 root preflight；不得绕过检查或把旧 280 包与新 allowlist 混用。

### 3.5 设备步骤

1. 确认候选、源码、boot-args、allowlist 与当前设备/root 状态。
2. 单独取得 `boot_b` 写入和重启授权。
3. 用户执行写入/重启，单 logger 采集新日志到 `~/NNN.txt`。
4. 按最早失败阶段判读；构建、启动、画面和功能验收分别记录。
5. mailbox、root、Android 侧操作也分别确认，不从一次刷机授权推导出来。

**回 Android（保留 b 槽，不切槽）**

1. iOS 正在运行时，经确认执行 `python3 -u x9/tools/ios-reboot-bootloader-usb`。脚本通过 USB 发重启请求并等待 fastboot；如果已在 fastboot 则跳过。遇失败停下，不盲重试。保留原 iOS 日志；需要新一轮 iOS 采集时，另用未存在的 `~/NNN.txt`，重启前启动唯一 USB logger。
2. `fastboot devices` 必须只列出一台设备，以其序列号作为 `$id` 定向命令（不把序列号写入 git）。`fastboot -s "$id" getvar is-userspace`、`getvar current-slot`、`getvar partition-size:boot_b` 分别确认 `no`、`b`、`4000000`（十六进制 64 MiB）。
3. 本机用户指定的 Android 恢复镜像是 `/Users/tosasitill/boot.img`，64 MiB，只作设备恢复输入，不是构建输入。确认文件、大小以及本次刷写与重启授权后执行 `fastboot -s "$id" flash boot_b /Users/tosasitill/boot.img && fastboot -s "$id" reboot`；只有 Sending/Writing 成功才自动重启。不要执行 `set_active`，不改其它分区；若只获准刷写就留在 fastboot。
4. 使用唯一 online adb 设备确认 `ro.product.model=PLJ110`、`ro.boot.slot_suffix=_b`、`sys.boot_completed=1`，才记录 Android 启动。刷写 OKAY、重启 OKAY 或 adb 上线均不能替代 boot-completed。若未观察到，不自动重刷、切槽或再次重启。

## 4. 工作流

- 一个功能/实验一个分支：`feat/<topic>` 或 `fix/<topic>`。
- 并行工作使用独立 worktree：

```bash
git worktree add .worktrees/<topic> -b feat/<topic>
ln -s ../../.local .worktrees/<topic>/.local
cp x9/build/local.json .worktrees/<topic>/x9/build/
```

- scratch 放本树 gitignored `x9/out/<name>/`；不在 `.local/` 新建实验目录。
- 不覆盖、提交或删除 `issues.jsonl`、其它窗口未提交工作、旧候选或备份。
- 每个里程碑更新本文件，记录提交、CI run ID、确切命令、退出码、产物大小、设备操作与未完成项。
- 完成后提交并合回 `main`；push 遵守用户当次明确要求，不把仓库默认规则推导为每次自动 push。

## 5. 当前候选与证据标签

### 5.1 标签

- `host-verified`：编译、静态检查或主机回归通过。
- `candidate_ready`：完整候选构建完成，不代表设备可运行。
- `runtime_verified` / `audio_verified`：必须有对应真实设备链路证据。
- `stageable`：root/mailbox 安装前置通过；不等于已安装。
- `device-verified`：注明候选、日志、设备操作和观察范围。

### 5.2 已知候选

| 候选 | 来源/位置 | 已验证 | 未验证 |
| --- | --- | --- | --- |
| `x9-bt-vendor-events-20261125` | main 历史提交 `414cbe3f`；boot 15,749,120 B | `~/425.txt`：FE01 section 2 重载、完整 capabilities、`0xE0..0xFF` 过滤后 bluetoothd 存活 >139 s | 搜索/配对/BLE/音频；该镜像不含后续 CallList |
| `x9-batch-20261125-r1` | 源码 `06a1f199`，主 checkout `x9/out/`；boot 15,343,616 B，kernel 31,934,736 B | 完整 host 构建，104/104 overlay；v6 manifest | 未上机；早于当前 call/SMS 合并 |
| `x9-call-sms-merge-check-20261125` | main `994d5fe0` 之前工作；kernel 33,066,592 B | 66/66 call、63/63 SMS init、storage host PASS；104/104 kernel 编译 | 无 boot.img、无设备结果 |
| `x9-bt-settings-diag-20261125-r2` | `.worktrees/bt-default-on`，分支 `feat/bt-settings-root`，HEAD `ab9828ea` | 三项 focused host 检查、完整 boot/rollback/kernel/manifest 构建；`runtime_verified=false` | 未刷入、未安装 root、未观察 Preferences setup |
| `x9-bootloader-reboot-psci-20261124` | `.worktrees/x9-bootloader-reboot`，分支 `feat/x9-bootloader-reboot` | log 421：PSCI 后实际进入 fastboot，helper PASS | 后续重启场景不据此自动重复 |
| camera diag17 | 历史提交 `e125dbf0` | 仅 host 构建；首 boot 未到 retire | 未完成 retire/出帧验收；不声称仍在 boot_b |
| `x9-eight-merge-20261126-r1` | main `2134b216`，主 checkout `x9/out/x9-eight-merge-20261126-r1/`；boot 15,761,408 B，kernel 33,066,848 B，104/104 overlay，allowlist 280 未变（无 mailbox） | `~/430.txt` device boot（内核时间 ~242 s）：shim、SCP stage1-9、GPU pan165 ownership、BT firmware-session+bluetoothd capabilities、CELL AT round3/4+CGCONTRDP IPv6、SMS 双卡 host-ready CNF 均到达；收取后已停回 fastboot | AegirPoster SharedEvent abort 循环（§6.13）；ALS 只记录首个 dispatch（其后未常规打印）、PROX 全程无 ENABLE/DATA、BT 11.4 s 后静默无 setup 注入；无 feature acceptance |

本轮文档整理基于 `994d5fe0`，README/规则提交为 `d6f92195`；未构建、未刷机、未重启、未写 root/mailbox，也没有新 CI run。

## 6. 分系统当前交接

### 6.1 蓝牙：从“桥崩溃”转向“原生可用性与音频”

**观察**

- 当前 `boot-device.json` 已包含 `x9_bt=1 x9_bt_init=1 x9_bt_fc=1`；勿在此配置上重复追加。433 已到 firmware session 和 HCI 扩展能力，不代表 Settings 可用。
- FE01 是 connac3 动态固件 section 请求，不是普通 Command Complete 错误。内核消费它并按 section 重载；Android `btmtk_main.c` 是参考。
- `0xE0..0xFF` 厂商事件在 Apple H4 parser 前消费；普通 Command Complete 不过滤。log 425 证明已定位的 reason-607 崩溃消失。
- 原生 HCI 扫描参数/开关与 advertising report 解析仍在 Apple 栈；没有 X9 自建发现引擎或伪造回调。
- Settings 行可见不等于 `BluetoothManager.available`。后者要求 `[self+0x18]==2`，只有五个原生 setup 调用全部成功才写入。
- 2026-11-26 Android 只读快照：Bluetooth State ON、crashed 0、Discovering false、Bonded devices 3（含 Technics EAH-AZ100 DUAL/LE 与 EDIFIER PK305），`A2dpOffloadEnabled=true`。这是 Android 适配器状态，不是 iOS `BluetoothManager.available` 或搜索验收。

**未完成**

- Preferences 的七个诊断 hook 尚未运行；不知道哪个 setup 调用/结果失败。
- 实际 `0x203b/0x203c/0x200b` 提交、advertising report、配对、连接、BLE 均无验收。
- UART SCO wire 转换已实现，但不证明 HFP/WBS、控制器路由或双向声音。

**下一步（离线）**：静态追 Settings → 原生 scan 提交/完成，列出预期命令与报告；之后只有另行授权才运行默认关闭的 r2 `x9_bt_settings_diag=1`。不得直接写 state 2 或伪造 native 返回。

参考：[`x9/bluetooth/README.md`](x9/bluetooth/README.md)、`x9/out/bt-settings-audit-20261125/`。

### 6.2 光线 / 距离：桥已存在，物理与策略边界未闭合

**观察**

- 共享 MTK SCP worker 已处理 READY/LIST/ENABLE/DISABLE、CRC8 相关 ACK、FIFO 与 DATA。
- `x9_als=3`、`x9_prox=3` 是板级默认且显式出现在 `boot-device.json`。
- ALS wire type 116 → 整数 lux，HID `0x20/0x41`/event type 12；上限 200,000，当前 5 s 是**接收年龄**，不是测量年龄。
- PROX wire type 8 → HID `ff00/8`、Placement=1、receiver mask `0x40`；当前 `latest.near = (oneshot == 0)`。
- 历史 `~/428.txt` 有真实 LIST、ENABLE ACK、lux=232、`near=1/oneshot=0` 与匹配 DISABLE；没有 `near=0` 或 oneshot=1。
- 2026-11-26 Android 只读快照（`x9/out/adb-snapshot-20261126/`）：sensorservice 列出 `tcs3720 ps` type 8 wakeUp/on-change、`OPLUS Fusion Light Sensor Next Gen` type 5 on-change/non-wakeUp，以及后置 `tcs3449 als/rgb/flicker/spectral`、`gesture_prox/phone_prox/pocket` wakeUp。近期 fusion lux 约 540–555，selected sampling 200 ms；logcat `HfLooper pwm_rgb` lux 约 500–513。power 为 Dozing、`mProximityPositive=false`。这是 Android 侧观察，未捕获 near/far 跳变，不能闭合 PS 极性。

**推断 / 未完成**

- SCP PS producer 的“keep far status in high ir or strong lux”分支写 byte 0，序列化不取反；当前 host 极性是具体反向疑点，不是已证翻转。
- SCP timestamp、AP counter epoch、校准字节与真实样本年龄未完全映射。
- CoreBrightness 对 lux 的动作、backboardd/SpringBoard 对 receiver-near 的接受均未验收。

**下一步（离线）**：沿 signed SCP PS producer/serializer 追 `value[0]`，与现有 Android 日志建立可辩护的 0/1→far/near 映射；不猜翻转、不造样本。

### 6.3 通话 / IMS：状态桥已发布，typed Call 与音频未接通

**观察**

- `X9Baseband.cpp` 只发布一次 VOICE09。
- VOICE20 仅允许已绑定/注册的 SIM2 owner、live transport、稳定 generation/binding、空 owned baseline 与实测 `{1,2,1,0,0,0}` profile；同 transaction 的 malformed/changed retry 不再次改 MD。
- CallList46 已启用，仍要求 live allocation、真实 042f/0430 RAT、owned call state、epoch/revision；在 MD 锁内直接发布，避免重入。
- 普通语音状态限定为 `{1,2,3,5,9}`，未知 END cause 不映射为 Apple reason 0。
- Android MO/MT 有真实 0901/0902/4901/4904/8901/8902 等元数据；没有本地 SIP INVITE/REGISTER wire capture。

**未完成**

- 现有 Apple RE 中 typed dial 进入 `initializeNonEmergencyCall`/AP SIP/Bambi，不是 Client 虚拟 transport slot。缺少独立于临时 `ims::Call` 对象的 `(clientId, UUID, native epoch/ID)` owner。
- MD→DSP `3D00` 不是 AP↔DSP REG_DMA。缺 producer/payload/ACK、RV32 consumer ABI、runtime task_info、EMI ring、codec/cadence/scene 与 VirtualAudio/AudioObject/IOProc provider。
- 旧“VOICE09 unannounced”注释与历史报告已过时，以当前代码和本节为准。

**下一步（离线）**：从现有 Apple RE 导出 typed-provider identity/action 契约，再决定 hook 层；不要用 VOICE20 可用推断 stock 电话 App 可拨号。

参考：[`docs/call-ios-bridge.md`](docs/call-ios-bridge.md)、[`x9/baseband/README.md`](x9/baseband/README.md)。

### 6.4 短信：modem 接受、MT ACK 与存储是三层

**观察**

- RawSend20→0605 携带 SMSC+TPDU；0606 是 modem 接受，不是对端投递。历史 SIM2 有 0605/0606，无投递回执。
- MT 8601 type3/0x8104、8603 MTI2/0x8100 会进入有界 inbox；WMS37 校验 token/SIM/slot/generation 后 ACK/NACK。8602/8604 的物理 TX completion 仍不等于 Messages 持久化。
- WMS flags 0..5 = ALL/NEW/OLD/SENT/DRAFT/INDEX；status 0..3 = unread/read/unsent/sent；record 264 B。
- 生产 tag→MIPC flag 固定 `{2,1,3,4}`。`{2,4,0,3}` 与已记录 AT status 2 拒绝矛盾，不采用。
- `0607` 诊断当前硬编码 index 1；它只记录 shape/status/index，不记录 PDU 或号码。

**未完成**

- 已独立反汇编确认 CMGL AT fail 路径调用 `l5usms_get_l5_res_from_at`，input=0 时可产出 `0x210000`；还未证明历史 `0608` 实际经过该路径，不能解释为空收件箱。见 [SMS AT 结果复核](docs/baseband/sms-at-result-20261126.md)。
- query-all/filter、混合异常 status、CDMA type4、durable SIM storage 与 Messages UI 持久化未验收。

**下一步（离线）**：追 CMGL fail 写入的结果槽到 `0608` packer，并区分真实输入零与其他同码来源；不能借静态转换确认历史运行态。

### 6.5 GPU / 高刷：先定义证据矩阵，再做单变量 A/B

**观察**

- 当前 allowlist 是 280；旧 rev249-lo/230/231 的性能数字不能外推到 280。
- 面板 vendor DT 有 60/90/120 timing，DCS 切换代码存在；但 DSI PLL/PHY 重配与 IOFB timing/mode 发布未建立。stock 120 使用不同 PLL。
- 软件 cadence 只有 `hz==120 ? 120 : 60`，选择 90 不会排程 90 Hz；D22 CADisplayLink/用户态上限未知。
- present 使用全局 enqueue watermark，不是 frame-specific fence；完成只表示 CPU 可见，不是校准 GPU 执行时间。
- 两个 CPU flip buffer、pending/ready record 与可选 LK slot2 不等于 render/ready/scanning/free 三缓冲生命周期。
- `x9_fill=1` 默认先 CPU 合成，`directPresent()` 通常再 CPU 拷贝；客户端 surface 直扫关闭。
- run-copy 优化仅 host 编译/逐位等价验证，不是设备性能提升。
- framebuffer 软件计数不是面板 fps；当前没有可信 frame time 分布。

**下一步**：离线整理 mode/cadence/queue/copy/TE 五项 evidence/acceptance matrix。设备 A/B 必须另行授权，固定 `x9_panel_hz=60` vs `120`，比较 TE median、submits/s、misses/timeouts、GPU queue/completion、swap→present 与真实 UI 节拍。

参考：[`x9/metal/README.md`](x9/metal/README.md)、[`docs/graphics/control-center-vs-folder-rendering.md`](docs/graphics/control-center-vs-folder-rendering.md)。

### 6.6 调度 / 灭屏热 / 睡眠

**观察**

- governor 使用 busiest-core 累计 CPU time，不是 Linux PELT；40–50% load 对应 500 MHz 请求。peak≥5% 时 tick 保持 50 ms；1000 ms 还要求 dark、无 GPU work、peak<5% 且 retention idle。
- dark display 不主动唤醒 GPU，使用非阻塞 try-lock；合法 render owner 会唤醒 MCU retention。retention sleep 保留 AS0/L2/images/GPUEB，不是 rail-off。
- WDS 每 3 s CSCON/physical-channel 对是 `x9w_up_diag()` 诊断，不是蜂窝保活；2026-11-26 起默认关闭，仅 `x9_cell_diag=1` 启用。
- 历史 `SIOCGIFMEDIA64`（`0xc02c6938`）到 PDP default，430/428 分别打印 1,940/5,024 行；`c94ddc6d` 对该命令聚合并稀疏打印，保持 `EOPNOTSUPP` 和输出不变。caller 未证、查询本身未减少。
- `x9_mdlog=0` 只去掉独立高吞吐 capture/save 线程；stage-1 DHL polling、打印与 ring 推进仍在。
- `~/428.txt`：灭屏后 retention sleep，约 1 Hz dark 事件；PDP 每约 2 s 八个失败；WDS 每 3 s；little 40–50%/500 MHz；大量 MIPC/MDLOG。57°C 出现在 `dark=0`，**没有复证 dark 57°C**。

**推断 / 下一步**

- 高置信：PDP/WDS/MDLOG 增加活动；中置信：render-owner 而非 dark timer 造成 GPU churn；低置信：MDLOG 本身造成测温差异。没有匹配 A/B。
- log 430 的 AegirPoster abort 循环在灭屏后继续，其 Metal 初始化与部分 GPU PM awake/retention 同窗口（§6.13）。SharedEvent 是待验证线索，尚无止崩或降温 A/B。
- 下一步离线把 428 的 `X9 SAMPLE` 线程记录与 201.107 s PDP burst、201–203 s GPU transitions 对齐。未来 instrumentation 只用有界内核计数/返回地址和既有采样，不 live sysfs。

### 6.7 点击亮屏 / 抬腕唤醒

**观察**

- `sweet_x9_touch.c` 明确不路由 touch IRQ；只处理普通 report `0x11`。没有 gesture 命令、decoder 或 wake key。当前 contact 进入普通 digitizer/HID 链。
- S3910 厂商源码已在固定参考树定位：`.local/references/oppo-modules-dt-mt6993/vendor/oplus/kernel/tp/hbp/hbp/chips/synaptics/s3910/` 与 `vendor/oplus/kernel/touchpanel/synaptics_hbp/`。旧“不在本地”结论作废。静态合同：`DTAP_DETECT=0x01`，`TOUCH_REPORT_GESTURE_ID=0x10`、`TOUCH_REPORT_GESTURE_DATA=0x1b`，`REPORT_DIFF` → `IRQ_REASON_GESTURE_DIFF`；`touchpanel_common_driver.c` 对普通 gesture 上报 `KEY_F4`（`KEY_POWER` 仅 `gesture_debug_sta` 调试路径），不是直接 `KEY_WAKEUP`。
- Android 设置/传感器快照：`double_tap_to_wake=1`、`double_wake_status=1`、`ambient_touch_to_wake=1`、`ambient_tilt_to_wake=1`、`gesture_newdoubletap_support=1`、`sys.oplus.gesturewakeup=1`；sensorservice 有 `pickup_detect` type 65611 special-trigger/wakeUp、`significant_motion` type 17 one-shot/wakeUp。`hf_sensor_type.h` 中 `SENSOR_TYPE_PICKUP_DETECT=75`（`ID_OFFSET=1` → ID 74）。这些是 Android/OPLUS 路径证据，不等于 iOS SpringBoard 接受。
- XNU 的 SCP 只保留 accelerometer type1、proximity type8、light type5；不消费 pickup/significant-motion one-shot。
- `X9Accelerometer` 发布连续 type13 XYZ；orientation bridge 只做姿态滤波，不做 wake。
- Android nanohub 会为一次性 wake gesture 发 250 ms PM wakeup，但那是传输恢复，不是 UI 亮屏。

**未完成 / 下一步**

- 点击亮屏需先沿已定位的 S3910 gesture-mode 命令/report code 与 `KEY_F4`/userspace notifier 映射，再证明 SpringBoard/doze 接受标准 Consumer Power、`ambient_touch_to_wake` 或合格 wake 事件；不能把 Android `KEY_F4` 直接当 iOS wake。
- 抬腕唤醒先查完整 SCP LIST 是否有 pickup/wake ID（Android type 65611 / MTK type 75 已见），再定义 screen-off-only、one-shot rearm/cooldown/stationary veto；不能只增加加速度事件。
- 当前没有 iOS 点击亮屏或抬腕唤醒工作链。
- 2026-11-26 host boundary：`python3 -I x9/motion/test_wake_boundary.py` exit0（5 tests），直接编译真实 `sweet_x9_motion.c`，确认 ACK 前 DATA 拒绝、 stale 过期、disconnect 失效、晚 ACK 不能清 fault；source 检查确认 orientation bridge 无 wake/shake/touch/S3910 合成。功能仍未实现。
- 2026-11-26 唤醒研究子代理（离线静态）结论：① S3910 两套栈（legacy `touchpanel/synaptics_hbp` 与新 `tp/hbp/hbp`）灭屏均经 DC `0xFE`/`0x09` 进 gesture mode、`enable_irq_wake`、suspended 上报 **KEY_F4**；PLJ110 实际加载哪套需 DT `device,chip_name`/cmdline 确认（未知）。② Android 普通 gesture 仅 KEY_F4，UI 亮屏推断在 userspace（touchDaemon/OPLUS gesture service→PowerManager），`tp_gesture_enable_notifier` 无在树调用者；APK 不可读。③ SCP wire 用 `SENSOR_TYPE_*`：significant=17、pickup=75、OPLUS stationary=83（`ID_OFFSET=1` 只作用于 Linux ID，不作用于 wire）；428 LIST 已见 type17/75/83/116；ENABLE=1 带 16B `{delay,latency}`，DATA 36B 共享 FIFO，pickup payload `{u32 value,u16 report_count}`；one-shot rearm/cooldown 在签名 SCP 固件内，AP 树只见重启 restore re-enable（未知）。④ iOS 接受侧：现有 wake 路径仅 PMIC keys→`sweet_hid_key(0)`→Consumer Power；**仓库无 SpringBoard/backboardd 接受 tap-to-wake/raise-to-wake 的 Apple RE 文档**，未知，不得猜。
- 推荐最小修复序（后续子代理）：先 **raise-to-wake**（证据最强）——`x9_als_scp.inc` LIST install+drain 增 type75（可选 17），`sweet_x9_motion.c` 旁加 one-shot 准入模块，合同=匹配 ENABLE ACK 后才 DATA、screen-off-only gate、显式 rearm/cooldown + stationary(83) veto、stale/disconnect fail-closed，扩 `test_wake_boundary.py`；**不翻 PS 极性**。tap-to-wake 在 iOS 接受被静态证明前只做诊断、不发 wake。先闭合未知：活动 TP 栈、SCP one-shot rearm、dsc cache 的 backboardd/SpringBoard wake 接受。

### 6.8 相机

- 历史 diag12–17：close/TG gate-off 与 guard reuse 相关；`CAM_GUARD_REUSE_NS=0` 时 gate-off/receive/DMA stop/IRQ close 通过，但 close 约 100–190 s，随后在 SMMU `retire:cmdq-prod` 附近复位。diag16 观察到 `PROD=0xd`、prior=0、CMDQ_BASE 已恢复 0；diag17 保持 PROD==CONS 并跳过 store，但未完成验收。
- 早期多流问题也已定位：iOS 在第一路后立即请求第二路，replace/reject 都会关闭主摄并触发 retire。候选方向是第二路“影子流”成功但不碰硬件，保持主摄多帧；仍未证明稳定出帧。
- 当前没有非黑多帧、完整 read、PixelBufferHandler/屏幕预览、照片或视频验收。详细组件契约见 [`x9/camera/README.md`](x9/camera/README.md)。

### 6.9 音频 / 蓝牙音频

- 扬声器 ADSP/IPI/AFE/PA 组件有源码与局部编译；`X9_ADSP_LOADER` 默认关闭，未注册完整物理 backend，`Ready=0`，Start 返回 NotReady。
- clock/PLL/path 与部分 PA probe 是显式写事务，不是只读检查；未执行实际播放或 iOS HAL 验收。
- 蓝牙 SCO UART 边界转换与 Apple 标准 0c26 路径有静态证据；私有 BCM `shci`/WBS/VoiceConfig 不能伪造成 MTK 成功。实际 HFP/A2DP/SCO 路由、连接 suspend/FW wake 和吞吐均未验收。
- `X9H4Transport` 只提供 HCI，不提供 PCM；kernel serial FIFO 也不是 codec。X9 自写 AudioServerPlugIn 只有 Speaker/Hawking，没有 Bluetooth PCM consumer。stock bluetoothd 则有 XPC audio publication、A2DP start/stop/data、HFP SCO completion、HFPAudioDevice 与 HCI send-thread 等候选消费者，但未证明在 X9 实例化。
- Android 当前 traced HFP 使用标准 Enhanced Setup Synchronous Connection `0x043D`，payload offset 50/51 为 input/output data path；FC72 只是 MTK voice-interface 初始化，不是 per-codec route。X9 AP↔DSP/AFE ring 不属于这两条蓝牙 PCM 链。
- 2026-11-26 Android 只读快照：audio_policy 配置为 `audio_policy_configuration_a2dp_offload_enable_cg_enable.xml`，`A2dpOffloadEnabled=true`；available output/input 含 Technics `BLE Headset Out/In`，supported/routable 列表含 BT SCO/A2DP。捕获时 `isBluetoothScoOn()=false`，`sco/a2dp/le_audio=false`，`active_acl_num=0`、`le_acl_num=1`。这是 Android 音频策略观察，不证明 iOS stock XPC/PCM consumer 已实例化。
- 最小待证链：A2DP 为 CoreAudio route → stock XPC/audio device → mediaserverd PCM → encoder/offload → AVDTP/ACL → H4；HFP 为 CoreAudio HFP → stock HFPAudioDevice/Gateway → `0x0428/0x0429 + 0x0c26` → native H4 SCO → MTK UART 转换。
- 2026-11-26 host boundary：`python3 -I x9/audio/test_bluetooth_boundary.py` exit0（2 tests），直接编译真实 `X9AudioPlugIn.c`；DeviceList 仅 Speaker/Hawking，Bluetooth/HFPAudioDevice UID 为 unknown，未知对象 StartIO/StopIO/DoIOOperation fail closed。标准 HCI SCO 链已有静态证据，但 stock HFPAudioDevice 的 XPC selector/序列化/publication descriptor 与真实实例化仍未建立，因此未添加 fake PCM endpoint/backend，audio_verified 不变。
- 下一离线步骤：从既有原件/捕获确定 Bluetooth device/stream publication、Start/Stop/timestamp 与 ReadInput/WriteMix buffer 合同，并找到真实双向 AP PCM↔SCO supplier；不得把 serial FIFO/UART 长度转换当 codec。
- 参考：[`x9/audio/README-speaker.md`](x9/audio/README-speaker.md)、[`x9/bluetooth/README.md`](x9/bluetooth/README.md)。

### 6.10 USB / AppSync / 存储用户态

- AppSync v6 已含 storage repair 工具与注入/storage 链；kernel 只验证并注入精确 runtime 环境，不安装文件。
- `install.py` 走 usbmuxd/lockdownd + AFC/installation_proxy；`netlockdown.py` 是 TCP 62078 候选。二者都无设备证据。
- 当前 USB 后端是 CDC-ECM + 私有 bulk，不是原生 AppleUSBDeviceMux。ABI 不匹配，native stack 仍不可加载。
- UFS/HFS 根盘正常；缺 `IOBlockStorageDevice`/`IOMedia` 和 APFS Data-volume 链。49154 是 APFS boot container 枚举失败，不是“HFS 已找到但缺 Data role”的唯一直接根因。
- 路线：先做只读 IOMedia 适配并保持 HFS 可启动，再独立设计 APFS container/System/Data。布局/迁移须备份设计与单独授权；不能 hook/合成 IOMedia 替代。
- 下一步先另出显式 `x9_usb=1` 单变量候选，过 `--session`/`--probe`，再谈 Mac 安装；不得把代码存在称为已能安装。

参考：[`x9/appsync/README.md`](x9/appsync/README.md)、[`x9/usb/README.md`](x9/usb/README.md)。

### 6.11 2026-11-26 八项离线修复合并

main HEAD `b2e58d35`。以下均为 **host 验证 / 源码边界**，不是设备 boot、runtime_verified 或 feature acceptance；未刷机、未写 root/mailbox、未 push。

- SMS：`fix/sms-storage` 已合并。修正 SMS-DELIVER OA semi-octet 边界后的 DCS/class 解码；pinned Xcode 15 下 `test_sms_pdu_class.py` 2/2 OK，`X9Baseband.cpp.o` 单对象编译 True（2,229,712 B）。
- GPU：`fix/gpu-pacing` 已合并。面板 DCS 与软件节拍只支持 60/120； unsupported explicit rate 拒绝/回退 60。`test_cadence.py` 2/2 OK，`SweetFramebuffer.o` 单对象编译 True（595,304 B）。
- 暗屏调度：`fix/dark-sleep` 已合并。RRC 诊断默认关闭，`x9_cell_diag=1` opt-in；`test_cell_diag_policy.py` 3/3 OK，`X9Baseband.cpp.o` 单对象编译 True（2,229,968 B）。无设备温度/功耗 A/B。
- 蓝牙音频：`fix/bt-audio-pcm` 已合并。仅固化 fail-closed publication/IO 边界；`test_bluetooth_boundary.py` 2/2 OK。真实 PCM consumer/XPC/supplier 仍未建立，audio_verified 不变。
- 距离传感器：`fix/sensor-bridge` 已合并。`test_proximity_bridge.py` 8/8 OK，直接编译真实 `sweet_x9_proximity.c`；PS 极性 `oneshot==0 -> near` 仍是未验证假设，ALS 物理链未新增设备证据。
- 通话：`fix/call-owner` 已合并。`test_call_owner_boundary.py` 8/8 OK，直接编译真实 `x9_call_state.h`；typed `ims::Call` owner/SIP/双向 PCM 仍缺。
- 唤醒：`fix/wake-gestures` 已合并。见 §6.7 host boundary；点击/抬腕功能仍缺 S3910/SCP one-shot 合同。
- 蓝牙 Settings 诊断：用户确认后已合并 `fix/bt-discovery`（含基线 `feat/bt-settings-root` `ab9828ea`）。dyld slide 地址修复 `e67688db`；合并后 `test_settings_diag.py` 4/4 OK，`x9/bluetooth/build.py --kernel-objects --out x9/out/bt-settings-merge-20261126` 构建/签名成功（仅 warnings）。诊断仍 Preferences-only、默认关闭、hash gate；未安装/未上机。基线同时带入 `x9_bt_init=1 x9_bt_fc=1` 默认开启，runtime ACK/profile gates 不变。

### 6.12 2026-11-26 adb 只读快照

用户授权“adb 已连接，一次性读完你要的”后，单次有界只读采集保存在 gitignored `x9/out/adb-snapshot-20261126/`（identity、sensorservice、bluetooth_manager、telecom、audio_policy/flinger、thermal、battery、power、input、filtered settings/getprop、logcat -d -t 1000）。未读 live sysfs，未写入/重启/刷机。

- 观察：Android Dozing、`mProximityPositive=false`、`mLastSleepReason=power_button`；thermal HAL 当前 CPU/GPU/SOC 约 34.96°C、SKIN 28.624°C、Thermal Status 0（cached 85.144 与当前 HAL 不同，不混用）。telecom 无 active call，PhoneAccount sub1/sub2 存在。BT/audio/sensor/gesture 观察已并入 §6.1/6.2/6.7/6.9。
- 缺口：`dumpsys sms` 返回 “Can't find service: sms”（sms.txt 空）；`/proc/bus/input/devices` Permission denied，但 `dumpsys input` 给出 `touchpanel` event2（KEYBOARD|TOUCH|TOUCH_MT，Generic.kl）及 pmic/gpio/fp/headset 设备。后续短信证据需另找 `isms`/telephony 有界计数或既有 Android 日志，不读消息正文/号码；不升级到 sysfs。
- 标签：全部为 **Android 侧观察 / 静态源码定位**，不是 iOS device boot、runtime_verified 或 feature acceptance。S3910 源码定位修正了 §6.7 旧结论；PS 极性、iOS wake 接受、stock BT PCM consumer 仍未闭合。

### 6.13 2026-11-26 log 430 设备启动观察与 AegirPoster 崩溃线索

用户授权“重启 fb 刷入最新测试镜像启动日志收取新一轮日志，收取后停到 fb”后，刷入 `x9-eight-merge-20261126-r1` 到 `boot_b`（OKAY），启动并收取 `~/430.txt`（2.83 MB，内核时间 ~242 s），随后 `ios-reboot-bootloader-usb` PASS 停回 fastboot。未写 root/mailbox、未碰其它分区。

**设备 boot 观察（~/430.txt）**

- 启动链到达：shim ready、SCP ALS stage1-9 + READY channel22、GPU pan165 INIT→POWER_ON→ID→ownership acquired、BT complete firmware session chip=6653 + bluetoothd H4/vendor bridge + capabilities（11.435 s ext scan=1 adv=1 sets=16）、CELL AT round3/4 OK + CGCONTRDP 取得 IPv6、SMS SIM1/SIM2 real host-ready CNF + GSM handlers installed。55.388 s `SWEET FB: display off`（自动灭屏）。
- **ALS**：2.677 s 有 type 116 ENABLE 成功响应，2.782 s 记录首次 `dispatch seq=1 lux=824`。源码仅打印首个物理 dispatch 或诊断样本，不能由日志条数推断后续没有采样/分发；type 0 cmd 10 心跳也不能证明采样。见 [ALS 离线复核](docs/sensors/als-observability-20261126.md)。
- **PROX**：LIST `tcs3720 ps` 命中，但 iOS 反复 `DetectionMode=0 interval=200000 requested -> 0`，全程 0 次 ENABLE ACK / DATA / near。428 在 157 s 有 ENABLE+near=1（疑似当时亮屏/交互），430 灭屏后无。PS 极性仍未闭合。
- **BT**：11.435 s capabilities 后无任何 X9 BT 行；无 setup state2 / Preferences 注入观察。Settings 灰仍属未验证（设备已停 fb，无法交互）。
- **CELL/SMS**：WDS `MD-session unobserved` 属 `x9w_probe()` 的状态日志，不是 RRC 查询。430 无 `x9_cell_diag=1` boot-arg，也无 CSCON/PCH 诊断命令或注册日志；SMS stored list queued QMI txn22，无 0x210000 映射证据。

**AegirPoster 崩溃循环（GPU / 灭屏热的运行时线索）**

- 观察：`CRASH AegirPoster` 每 ~10 s 双崩溃（launchd 重生→~0.5 s 再崩→节流 10 s），430 共 44 次、428 共 66 次，**灭屏（55 s）后持续到 242 s**。sig 6（SIGABRT），pc/lr/far 相对 slide 恒定。MapsSpotlightInd 27.4 s 同路径崩溃 1 次；driverkitd 7.9 s sig 9（重生，非异常）。
- host 符号化（slide 0x2d8c0000，`/tmp/dsc-index.json` 重建自 `.local/apple/dsc-20H392/`，atos）：`__pthread_kill ← pthread_kill ← abort ← _objc_terminate ← _dispatch_client_callout ← _dispatch_block_invoke_direct ← __FBSSERIALQUEUE_IS_CALLING_OUT_TO_A_BLOCK__ ← -[FBSSerialQueue _targetQueue_performNextIfPossible]`。即 **FrontBoardServices 串行队列 block 内未捕获 ObjC 异常 → abort**。首次崩溃附近有 `X9 GENERAL METAL: factory caller=AegirPoster` + `SWEET SURF: AegirPoster sel 40`；selector 日志有 log-once 限制，不能断定每次崩溃前的最后动作。
- host 反汇编 IOSurface.framework（实测 host，非设备）：sel 40 = `_IOSurfaceSharedEventAddEventListener`/`AddListener`，sel 41 = `RemoveEventListener`/`RemoveListener`。完整 SharedEvent 族：36=create、37=setSignaledValue、38=initWithMachPort、39=NotifyListener、40=AddListener、41=RemoveListener、45=waitUntilSignaledValue。
- 推断（待验证）：`SweetSurface.cpp` 对这些 selector 默认返回成功但不实现共享事件/通知，可能与 Poster 异常有关；现有日志没有异常名称或完整抛出栈，不能证明因果。灭屏后 Poster 重生与 GPU PM awake/retention 在 201 s 段相邻，支持存在 churn，但不能归因全部 wake 或量化热负载。这是 428/430 共有的**先前存在缺口**，不是八项修复引入的回归。
- 边界：尚未证明实现 SharedEvent 族即可消除崩溃（需 host 合同 + 后续设备复证）；不得把 host 符号化/反汇编称为设备 runtime_verified。

**下一步**：按 §6.16 用诊断候选观察 SharedEvent 实际调用形态，继续追异常来源；唤醒另按 §6.7 推进。

## 7. 推荐下一步顺序

1. 保持文档与源码标签一致；任何新候选使用唯一名并记录 source HEAD。
2. **GPU/灭屏热（最高运行时优先）**：按 §6.13 在 SweetSurface 实现/安全处理 IOSurfaceSharedEvent 族（sel 36-41/45），先止 AegirPoster abort 循环；host 合同验证后申请设备复证。
3. 蓝牙：完成 Settings/scan 静态合同；r2 诊断候选只有重新授权后才上机。
4. 传感器：闭合 PS 极性与时间域；短信：追 CMGL fail 结果到 `0608` packer。
5. 通话：typed-provider identity/action 契约；不要先补音频 hook。
6. 唤醒：按 §6.7 先 raise-to-wake（SCP pickup type75 one-shot 准入），tap-to-wake 待 iOS 接受静态证明。
7. 存储/USB：只读 IOMedia A0 与 AppSync session probe 分开推进。
8. 相机：先证明主摄多帧非黑与稳定 retire，再接第二路/完整 iOS 预览。
8. 点击/抬腕唤醒：S3910 源码已定位，先离线追 gesture report → `KEY_F4`/userspace notifier 与 SCP pickup/significant-motion LIST/one-shot 合同，再谈 iOS doze/SpringBoard 接受；不伪造 iOS 事件或样本。

### 6.14 2026-11-26 SharedEvent 合同复核与离线研究交接

- 基线 `bc6a83d615014f39b6cc3eab80839466a5fb2bab`。host 反汇编复核：selector 40 是以 notification port 和 async reference 注册 listener，而不是已证的 event ID 加普通 struct；listener 初始化独立于 selector 36。selector 36 失败使 `IOSurfaceSharedEvent init` 返回 nil，但不能据此证明 listener 不执行或上层安全降级。selector 40 的零端口 cold 路径在调用之前，不能解释调用后崩溃。详见 [`docs/graphics/shared-event-contract-20261126.md`](docs/graphics/shared-event-contract-20261126.md)。
- 上一会话落盘的四条只读研究位于 gitignored `x9/out/research-{sensor-ps,gpu-hz,appsync-usb,dark-thermal}-20261126/report.md`：PS 极性仍未闭合；GPU 有 mode/cadence/queue/copy/TE 验收矩阵；AppSync 缺真实 USB/session/install；灭屏日志显示 Poster churn 和 PDP/WDS/MDLOG 并存，功耗贡献未量化。
- 本轮 `sed` 复核 exit 0；未改驱动、未构建、无 CI、无设备操作或新设备日志。无新候选或产物大小。下一步追上层 nil/异常处理及 IOKit async 消息 ABI，不把局部 nil 路径或伪成功当作止崩修复。

### 6.15 2026-11-26 并行离线修复：PDP / GPU / ALS

基线 `f239883c`，本地并行合并 `fa21661d`。无 CI、无设备操作、无 push。

- PDP `c94ddc6d`：只对 native DPMAIF 的 `SIOCGIFMEDIA64` 聚合日志（前四次、随后 2 的幂），不改返回值或拨号/接口状态。worktree 执行 `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/out/pdp-ifmedia-object-20261126/compile.py` exit 0；按原 recipe 单编译 `X9Baseband.cpp.o` 2,230,704 B，34 warnings；编译脚本/完整命令日志保留在主 checkout `x9/out/coordinator-pdp-ifmedia-20261126/`（脚本内固定原输出路径）。`python3 -I x9/baseband/test_cell_diag_policy.py` 3/3，exit 0。未量化热影响。
- GPU `58296888`：现有 `shown_gpu_seq` 字段保留，说明改为 swap 时采样的全局 enqueue watermark；等待与采样语义不变。子代理静态断言与 `git diff --check` exit 0。不是逐帧 GPU fence 或高刷验收。
- ALS `1fa6466b`：只读复核纠正 §6.13 日志解读；未改驱动、未翻转 PS 极性。AppSync 无新增设备会话/能力，仍见 §6.10。

### 6.16 2026-11-26 SharedEvent 诊断候选（candidate_ready）

- `4c555890` + `91c4e8dc` 合并至 `07dd9916`。新增默认关闭的 `x9_surf_event_diag=1`；启用后至多记录 64 条 SharedEvent 调用（AegirPoster 48 条专用额度、其余 16 条），只记录进程名、selector、端口存在与参数形状，不打印端口值或引用，不改变返回/同步行为。原生调用方 nil 处理、通知 payload/生命周期及异常原因未闭合。
- `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_surf_event_diag=1 --name x9-shared-event-diag-20261126-r1`：exit 0，104/104 overlay（3 prebuilt），`boot.img` 15,781,888 B、`kernel.development.sweet` 33,067,056 B，产物与 `build.log` 位于 `x9/out/x9-shared-event-diag-20261126-r1/`。allowlist 280 不变；无需 mailbox。未刷机、未重启、未写 root/mailbox、无 CI、未 push。
- 该候选也包含 §6.15，不能当作相对 430 的严格单变量热 A/B。若另行授权上机，从 `SWEET SURF EVENT` 收集 AegirPoster 的 selector/async 形状，并与同一日志的 abort 首发和崩溃记录对齐；仅调用顺序不能证明异常因果。还需追异常名称和抛出点；本轮没有 stop-crash/runtime 验收。

### 6.17 2026-11-26 SharedEvent / 通话 / 短信并行续查

基线 `15f75087`；整合源码 `d9f48a25`。仅离线；无新完整候选、CI、设备操作或 push，§6.16 诊断镜像仍是此前构建，不含本轮通话日志修正。

- SharedEvent：`d04e6f55`（复核子分支 `746921d1`）补齐 native Metal notify 的非零返回抛异常链；不能直接拒绝 selector 39。原生 getter 的共享地址读取只是另一静态边界，不是 430 SIGABRT 根因。Metal 窄反汇编命令见合同文档；430 两进程首栈 `atos` 复核 exit 0，首次 AegirPoster 见非 nil Metal device，MapsSpotlightInd 与其深层栈不同。下一决定点是实际异常名/抛出点及运行 selector 序列；未改 SharedEvent 驱动语义。
- 通话：`caa75676` 更正 VOICE09 与 MT 观察日志，未改变准入、队列、动作或音频。新增 [typed action 合同](docs/call-typed-action-contract-20261126.md)，下一步闭合同一动作捕获的 record/Client/CallInfo 与更新/退休 barrier。合并后 `python3 -I x9/tools/tests/test_call_observation_labels.py` 2/2、`python3 -I x9/baseband/test_call_owner_boundary.py` 8/8，均 exit 0。
- 短信：`2636ac42` 的三个限定 `md-dis` 切片 exit 0；CMGL fail 转换证据见 §6.4。未改存储处理；`python3 -I x9/baseband/test_sms_pdu_class.py` 2/2、exit 0。未验证真实收发、存储或 UI。
- Host 单对象：`PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/out/x9-call-observation-object-20261126/compile.py` 的 `Builder.compile_one` 成功，`X9Baseband.cpp.o` 2,224,768 B，34 warnings；外层 zsh 在编译后因保留变量 `status` 报错 exit 1，不是编译错误。原始输出与产物保留在上述目录；未重编或上机。

### 6.18 2026-11-26 SharedEvent / 通话 / 短信实际修复候选

- 基线 `12be8340`，候选源码 `cf753dc1`。SharedEvent `a743b6bd`：由子代理 `7c09743a` 的发现进一步核实 generated `device_server.c` 与 `ipc_kobject_server` 的权属交接，**只在忽略 selector 40 并返回成功时**释放收到的异步 send right；错误回复由 MIG 销毁 request descriptors，不在子类提前释放。同步请求不动。这不实现 listener/notifier，也不证明 AegirPoster 已止崩。原版 AegirPoster 执行档已从固定 20H392、已有只读 OS.dmg 挂载定位；新静态切片在 `x9/out/aegir-consumer-20261126/`。最初重复 attach 因原镜像已挂载返回 1，改用已确认只读挂载；未改挂载/设备。具体见 SharedEvent 合同文档。
- 通话 `ae276dd2`：VOICE21/22 同 allocation/binding/generation 的 in-flight txn 先锁定已拥有请求，再解析未被拥有的新请求；malformed 重试不再先发 ARG、再发原终态。不同 txn 仍依法校验。两个 host 向量在修前触发断言；合并后用 `SDKROOT="$(xcrun --sdk macosx --show-sdk-path)" clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable x9/tools/tests/call_bridge_host.cpp -o x9/out/x9-call-lifecycle-merged-20261126/call_bridge_host`（Xcode 15 `DEVELOPER_DIR`）编译 exit 0，`ios_end_malformed_duplicate ios_answer_malformed_duplicate ios_end_duplicate ios_end_changed_duplicate ios_end_roundtrip ios_end_abandon ios_end_late_cnf` 7/7 exit 0；ASan 环境初始化不通过，不声称有 sanitizer 结果。未完成 typed 电话 UI、接通/挂断或双向音频的实机验收。
- 短信 `9ab42ab3`：CMGL 结果槽继续追至 ILM `0xd9f3`，MIPC 转换器保留 `0x210000`；仍缺中间 dispatch/历史 0608 关联，见 [SMS 结果分发链](docs/baseband/sms-result-path-20261126.md)。`91a2265b`：take 返回后、storage CNF 发布前重检 deadline；迟到不覆写成成功，保持 token 消费和 TAG hold 行为。Xcode 15 `DEVELOPER_DIR` 与 SDKROOT 下 `python3 -I x9/baseband/test_sms_storage_deadline.py` exit 0，覆盖截止前/恰好截止/已应答/非 storage。未证真实 MT/发送对端投递、SIM 读取、Messages 持久化。
- `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_surf_event_diag=1 --name x9-shared-event-call-sms-20261126-r1`：exit 0，104/104 overlay（3 prebuilt），boot 15,781,888 B、kernel 33,067,056 B，完整日志在该候选目录，allowlist 280 未变。仅 **candidate_ready**，无 CI、设备操作、root/mailbox 或 push。设备验证需明确按候选授权；有了新 SharedEvent trace 和实际异常才能判断止崩方向，同时通话/短信按原链路各自验证，不用 queue/CNF 替代最终收据。

### 6.19 2026-11-26 原版事件 ABI / VOICE22 receipt / MO deadline

- 基线 `2ce5e815`，整合构建源码 `8eb88624`。用固定 D22/20H392 原版 kernelcache 的两张 54 行 dispatch 表确认 selectors 36–41/45 的形状：38 是 3 scalar out；39 是 5 scalar in 的同步注册；40 是仅 async reference、无 scalar/struct I/O。见 [原版内核 ABI](docs/graphics/shared-event-native-kernel-20H392.md)。`364559b7` 按原版校验 40 形状，合法调用仍按此前兼容策略释放未使用的 send right，未实现 listener/event。出错仍由 MIG 回收。静态表/handler 切片 `python3 -I x9/out/native-event-abi-20261126/inspect.py` 及其有界 VA 参数命令均 exit 0。
- `3164fed2` 证明 QuartzCore drawable 确实实例化 IOSurfaceSharedEvent，并在局部 present 路径遇到 nil 时回落普通 present；不能推广到全局拒绝 selector 36，也不是 430 崩溃归因。证据仍缺实际异常/selector 序列，及 stock event 映射和 listener 端口通知生命周期。
- `b9f97035` 修正 VOICE22 host queue stub 与生产 receipt 的差异，补接听成功的 terminal queue 反压/恢复回归；没有修改生产行为或把 CNF 当作已接通。Xcode 15 `DEVELOPER_DIR` 与 SDKROOT 下 `clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable x9/tools/tests/call_bridge_host.cpp -o x9/out/x9-native-event-call-sms-verify-20261126/call_bridge_host` exit 0；`ios_answer_receipt ios_answer_malformed_duplicate ios_end_duplicate ios_end_roundtrip` 4/4 exit 0。
- `011ad5e4` 将取回真实结果后的 deadline guard 扩至 RawSend/0606。过期先答 NOT_READY，仍消费结果且不重发；未证明对端投递。Xcode 15 `DEVELOPER_DIR` 与 SDKROOT 下 `python3 -I x9/baseband/test_sms_storage_deadline.py` exit 0（1/1，含 MO 和存储边界）。
- `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_surf_event_diag=1 --name x9-native-event-call-sms-20261126-r2` exit 0（104/104 overlay，含 3 prebuilt）；boot 15,781,888 B、kernel 33,067,104 B，输出见候选 build.log，allowlist 280 未变。无 CI、设备操作、root/mailbox 或 push。功能验收仍未完成；不得以该构建替代现有候选特定授权。

### 6.20 2026-11-26 SharedEvent 映射/通知合同与通话、短信修复

- 基线 `16cc7c53`，整合源码 `fdbf9a1f`。SharedEvent `f6435893` 钉实原版 selector 36 输出为 task-local Mach 名称和只读 counter VA，38 解析该能力并返回 value/VA/trace ID，45 按 ms 计算 deadline 并以真实 counter 判定。固定镜像的 GOT 与有界 handler 静态切片 `python3 -I x9/out/x9-event-mapping-20261126/inspect.py` 及 VA/长度参数均 exit 0，见 [映射合同](docs/graphics/shared-event-native-kernel-20H392.md)。`0095e787` 证实锁内二次判定、thread_call 调度和 async payload 的四个 qword；`python3 -I x9/tools/tests/inspect_shared_event_delivery.py` exit 0，仅短区间反汇编检查。下一步按这些 VA 和合同追最后一个引用释放/客户端退出，以及 pending listener 移除、消息完成与事件解除映射的原子顺序；未改 SharedEvent 运行语义或声称止崩。
- 通话 `a243d895`：VOICE03 畸形同事务重试由已在途 owner 决定唯一终态，不再直接发 ARG 后又回原 success；不可凭此认为 typed 调用已进入现有 VOICE service。Xcode 15 `DEVELOPER_DIR` + SDKROOT 下 `clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable x9/tools/tests/call_bridge_host.cpp -o x9/out/x9-voice-sms-sync-verify-20261126/host` exit 0；`register_malformed_duplicate register_sequence register_timeout register_deleted register_unsupported handover_request_identity ios_answer_receipt` 7/7 exit 0。
- 短信 `0dd98be7`：默认关闭的 CMGR index1 诊断改为 preserve tag 的 `0x103=1`，不标记已读；生产链路未变。Xcode 15 `DEVELOPER_DIR` + SDKROOT 下 `python3 -I x9/baseband/test_sms_diag_readonly.py` exit 0（1/1），不是硬件读写验证。旧诊断镜像若启用 `x9_sms_diag=1` 不含此修复。
- 整合后 `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/out/x9-voice-sms-sync-verify-20261126/compile.py` 以现有 recipe 只编 `X9Baseband.cpp.o`，exit 0，2,224,280 B、34 warnings；各日志均在该 scratch 目录。无新整包、CI、设备、root/mailbox 或 push。r2 仍只是已有诊断候选；上机仍须按候选取得具体授权。

### 6.21 2026-11-26 SharedEvent 内核实现基础 / 拨号 lease / SMS txn

- 基线 `299473f9`，整合源码 `90b27fa4`。SharedEvent `4340b009`：真 kernel counter 对象（专用全零页、只读 task map、128 活对象上限、单调 release-store、可中止 deadline wait）；`2a9af796`：64 槽 client listener registry、成功路径的 MIG right 持有、引用计数和四字段异步 send helper。**selector 语义仍未接线**；没有分配新用户态句柄、发真实通知或止崩验收。下一步实现 task-scoped Mach capability + 映射与 client-close/last-right 约束，以及有界 event→listener pending 队列；不得持 event 锁跨发送。映射必须先释放再释放对应 event，错误不接管 MIG right。
- SharedEvent host（Xcode 15 `DEVELOPER_DIR` + SDKROOT）：`clang++ -std=c++17 -O1 -Wall -Wextra -pthread x9/tools/tests/<name>.cpp -o x9/out/x9-shared-event-foundations-verify-20261126/<name>`，`shared_event_counter_host` 与 `shared_event_listeners_host` 编译与运行都 exit 0。fixture 编译真实 .inc 验证计数器状态/截止边界、端口计数/容量及 add/close 竞争；stub 无真实映射、Mach 消息或调度。
- 通话 `fa037056`：writer 明确在 selection/TX 前拒绝时回收 DIAL lease/job，但保留独立终态 owner；已选、部分发送及不确定 owner 不变。Xcode 15 C++17 `clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable x9/tools/tests/call_bridge_host.cpp -o x9/out/x9-shared-event-foundations-verify-20261126/call_bridge_host` 和 `dial_presend_release dial_presend_fence ios_timeout ios_deleted ios_join dial_roundtrip dial_tx_failure ios_end_roundtrip ios_answer_receipt` 9/9 exit 0。未验收 native UI、接通或音频。
- 短信 `4b8e7e53`：WMS31/22/23 按 slot/generation/txn 保留活动 storage owner，同事务重复/变更不会再提交 0607。Xcode 15 环境 `python3 -I x9/baseband/test_sms_storage_reuse.py` 1/1、exit 0。无跨 WMS 完成后缓存、队列收执或持久化的新证据。
- 合并后 `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/out/x9-shared-event-foundations-verify-20261126/compile-{surface,baseband}.py` 分别执行，recipe 单对象均 exit 0：`SweetSurface.o` 455,168 B/12 warnings，`X9Baseband.cpp.o` 2,226,648 B/34 warnings。编译、host 测试日志位于该 scratch；无新整包、CI、设备、root/mailbox 或 push。

### 6.22 2026-11-26 SharedEvent capability 与通话/短信 owner

- 基线 `9d2ffaac`，整合源码 `9b264ce4`。SharedEvent `33c6334c` 实现 Mach 能力导出/导入辅助层、所属 task 与对象类型校验、只读映射复用、64/client + 512/system 映射上限、失败回滚及 close 清理。**未接 selector**：还需待通知队列、listener 取消顺序、缓存映射的最后权利回收，以及映射与任务退出的实机验证。子代理 pending-queue 此轮因额度未启动实际实现，我继续了能力层。Xcode 15 `DEVELOPER_DIR`/SDKROOT 下 `clang++ -std=c++17 -O1 -Wall -Wextra -pthread x9/tools/tests/shared_event_<unit>_host.cpp -o x9/out/event-capability-integrated-20261126/<unit>`，counter/listeners/capability 编译及执行 3/3 exit 0（模拟 Mach/VM）。capability 初次单对象因内核指针型 MACH_PORT_NULL 与整型 name 不匹配失败，改显式 name 范围后通过。
- 通话 `8586acda` 将真实 QMUX allocation 与原 deadline 带入 DIAL admission 和 writer selection；仍保留已选择/不确定请求，不重发。Xcode 15 下 `clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable x9/tools/tests/call_bridge_host.cpp -o x9/out/event-capability-integrated-20261126/call` exit 0，六项 `dial_guard_{admission,writer,selected}_{release,timeout}` 和 `dial_presend_release dial_roundtrip ios_end_roundtrip ios_answer_receipt` 共 10/10 exit 0。未验证 typed 通话和音频。
- 短信 `f679a205` 在通用 QMI 校验前识别严格限定到 WMS storage owner 的 malformed 重传，免于第二终态；`python3 -I x9/baseband/test_sms_storage_reuse.py` exit 0。storage queue receipt 与 Messages 持久化仍缺。
- 合并后 `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/out/event-capability-integrated-20261126/compile-<unit>.py`（surface/baseband/writer）均 exit 0；`SweetSurface.o` 470,432 B / 12 warnings，`X9Baseband.cpp.o` 2,228,144 B / 34 warnings，`sweet_x9_gpu_dependencies.o` 8,143,520 B / 266 warnings。无新整包、CI、设备操作、root/mailbox 或 push；最新设备证据仍为既有 430，不能验收上述三条功能。

### 6.23 2026-11-26 SharedEvent 生命周期、VOICE03 终态、WMS 收执

- 基线 `20356c07`，整合源码 `a9b2dc2b`。SharedEvent `3f829d17`：Mach identity 使用独立引用对象；挂接的 views 在 client close 或最终引用释放时解除映射，再释放计数器。host 证明导出失败回滚、双任务授权导入、client close 和模拟最后一个 right 后解除映射；未验证真实 Mach no-senders/task death。闭合的 owner 记录仍计入对象有限容量。 `c3b4e552` 实现 64 项 pending 队列，锁内阈值/取消/领取，锁外发送，尚无自动 worker/selector 接线；当前已领取发送不能取消。下一步实现可靠 worker 与 listener 跨队列取消：本地 XNU `sendAsyncResult64WithOptions(..., kIOUserNotifyOptionCanDrop)` 可用零超时送出尝试，但须保留未成功消息、区分队列满与死端口，不能直接丢弃或称已交付。所有 signal 必须经过唯一队列。
- `be76bd4d`：VOICE03 的错误终态待队列收执时保留 waiter/txn owner；不让新请求提前覆盖。合并后 Xcode 15 C++17 host 编译及九向量（`register_error_owner_retained register_malformed_duplicate register_sequence register_timeout register_deleted register_unsupported handover_concurrent_bootstrap handover_request_identity ios_answer_receipt`）均 exit 0；未验证 typed 拨接与音频。
- `6f147930`：增加 WMS storage 完成帧校验/真实 QMUX generation + 队列容量检查的收执入口，只有入队才转移所有权。**尚未接入生产 job**，下一步要保留首个序列化终态、真实 token drain 和 TAG hold 再做重试。Xcode 15 `DEVELOPER_DIR`/SDKROOT 下 `python3 -I x9/baseband/test_sms_storage_receipt.py` exit 0（1/1，mocked bridge，非实机存储/送达）。
- 合并后 Xcode 15 `DEVELOPER_DIR`/SDKROOT 执行 `clang++ -std=c++17 -O1 -Wall -Wextra -pthread x9/tools/tests/shared_event_<unit>_host.cpp -o x9/out/event-lifetime-integrated-20261126/<unit>` 与运行（counter/listeners/capability/pending）各 exit 0。通话 host 同目录 `clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable x9/tools/tests/call_bridge_host.cpp -o x9/out/event-lifetime-integrated-20261126/call`。同目录用 `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/out/event-lifetime-integrated-20261126/compile-<unit>.py` 各 exit 0：surface 539,032 B/12 warnings，baseband 2,231,824 B/34 warnings。未构建整包、无 CI、设备、root/mailbox 或 push。

### 6.24 2026-11-26 SharedEvent 接线 r3 / WMS 收执上线候选

- 基线 `41280729`，构建源码 `b29d4e87`。SharedEvent worker `5288da71`（cherry-pick `326433c0`）与接线 `2ecfcb74`：显式 `x9_surf_events=1` 时 36–41/45 真正做 Mach 能力导出/导入、counter、阈值等待及 async 通知；40 接管成功路径 async right，41/close 按 listener 标记撤销并跨队列取消。死端口不宣称交付、队列满保留重试。默认路径不启用。见 [通知合同](docs/graphics/shared-event-delivery-20H392.md)。未验证跨任务真实 Mach no-senders、42–44 用途、拥塞公平性和 AegirPoster 止崩；未改变 Metal 插件 handle 实现。
- 短信 `dabf694f` 将生产 WMS storage job 接到身份受限的 terminal 入队收执：保存首个响应，client 有效时重试，已发 token 仍排空；TAG hold 不得阻止 terminal 重试。Xcode 15 `DEVELOPER_DIR`/SDKROOT 下 `python3 -I x9/baseband/test_sms_storage_<unit>.py` 中 receipt/terminal/deadline/reuse 各 exit 0。测试不证明完整 x9m_poll/TAG 状态机或 Messages 持久化。
- 通话 `242e120e` 仅增加 handover 重复 enable/cursor 回归，未改生产请求行为或打通 typed ingress。Xcode 15 C++17 `clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable x9/tools/tests/call_bridge_host.cpp -o x9/out/x9-event-wired-verify-20261126/call` exit 0；`handover_repeat_enable_cursor handover_concurrent_bootstrap handover_subscription_fence handover_live_queue_wait handover_live_repeated_state handover_live_queue_sequence` 6/6 exit 0。同环境 `clang++ -std=c++17 -O1 -Wall -Wextra -pthread x9/tools/tests/shared_event_<unit>_host.cpp -o x9/out/x9-event-wired-verify-20261126/<unit>`，counter/listeners/capability/pending 编译和执行 4/4 exit 0，日志同目录，非真实 Mach 或设备验证。
- `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_surf_events=1 --name x9-native-event-wired-20261126-r3` exit 0，104/104 overlay（含 3 prebuilt），boot 15,806,464 B、kernel 33,111,312 B；allowlist 280 未改，未执行设备操作、CI、root/mailbox 或 push。候选有实际实验接线，不等于通过功能验收。需要明确针对 r3 的 `boot_b` 写入、启动和日志采集授权；通话/短信完整链路仍需对端和设备收据。

### 6.25 2026-11-26 SharedEvent r4 / native probe / 通话短信验证边界

- 基线 `5a5c22c0`；r4 构建源码 `052d5905`。SharedEvent `3c8e7132`：同一 event queue 内某 listener 端口满，保留该 listener 的响应并延迟重试，但不挡本轮其他就绪 listener；每轮最多 64 次尝试，单 listener 的已就绪通知按订阅序，跨 event queue 不保证该顺序。Xcode 15 `DEVELOPER_DIR`/SDKROOT 下 `clang++ -std=c++17 -O1 -Wall -Wextra -pthread x9/tools/tests/shared_event_pending_host.cpp -o x9/out/x9-event-fair-verify-20261126/pending` 和执行均 exit 0，含满载 A 与 B 仍可送达、恢复有序的合成 host 覆盖。
- `706334ce` 新增 [原生 IOSurface 验收 probe](x9/tools/shared-event/README.md)，iOS arm64 target 编译 exit 0，`x9/out/x9-native-shared-event-probe-20261126/native-probe` 69,392 B；精确命令见 README。首次编译因 iPhoneOS SDK 无 `IOSurface/IOSurface.h` 失败（exit 1），改用系统框架动态加载后通过。未安装或执行。probe 只覆盖同 task Mach 重导入、真实 counter/wait/回调及同连接资源回收；跨 task、AegirPoster 和现场 GPU 行为另需验证。静态切片同时定位 42 为 IOSurfaceClientSignalEvent（stock 查 surface 与 event，另有 surface/accelerator 路径），43 detach mode、44 ownership；未将 42 链接到普通 signal 以免忽略 surface 语义。
- `7386cf0e` 新增实际 `x9m_poll` 的 CNF 消费分支 host 复核，覆盖 receipt pending、deadline、TAG hold / follow。Xcode 15 下 `python3 -I x9/baseband/test_sms_storage_poll_hold.py` exit 0（1/1，stub job_done，不是整链或持久化）。`42972b2a`（文档整合 `1a63d237`）用固定原切片将强 Client 与强 Model→record 定位到同一 manager map/controller；未证同动作捕获/更新顺序、CallInfo→PersonalityID 及晚回调 fence，未改通话生产行为。
- `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_surf_events=1 --name x9-native-event-wired-20261126-r4` exit 0（104/104 overlay，3 prebuilt）；boot 15,806,464 B、kernel 33,111,392 B，build.log 在候选目录。allowlist 280 未改。无 CI、设备操作、root/mailbox 或 push。上机需针对 r4 分别明确授权 boot_b、启动、日志；probe 执行另行指定已有执行路径。目标不可在未验收实际 SharedEvent/通话/短信全流程前标为完成。

### 6.26 2026-11-26 SharedEvent r5 / typed 队列资格 / SMS TAG 回归

- 基线 `ddb869e7`，r5 构建源码 `d17dbb19`。SharedEvent `88c8396d` 回收 Mach 能力中已关闭的 view-owner 槽，避免长寿命事件在顺序导入/关闭后被耗尽；同时打开的 64 槽上限不变。Xcode 15 `DEVELOPER_DIR` + SDKROOT 下 `clang++ -std=c++17 -O1 -Wall -Wextra -Werror x9/tools/tests/shared_event_capability_host.cpp -o x9/out/x9-event-r5-verify-20261126/capability-merged`、运行均 exit 0（96 个顺序 owner、同时打开上限及释放）。同一回归只换回基线 views .inc，host fixture exit 134（预期断言），产物位于 `x9/out/x9-event-view-reclaim-20261126/`；非设备故障复现。
- `0b5b6ca3` 证实 QuartzCore present 的 42 调用指向原版 `_IOSurfaceSignalEvent`；非零路径回退普通 present。仅 `x9_surf_events=1` 的 selector 42 改为明确返回 Unsupported（畸形 BadArgument），不再没有 signal 却返回成功。并非实现真实 surface/accelerator 顺序或证明其他调用者可降级。 `python3 -I x9/tools/tests/inspect_shared_event_42.py /private/tmp/x9sys/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.04` exit 0（已有 read-only SystemOS mount；未改挂载），合并后的 capability 回归覆盖不改变 counter。
- 通话 `58df288f`：原 cache slide-v2 chain + libdispatch export 确认 manager 的队列构造保存到 manager+18；不证明 tracker 队列为同一对象，或一次动作的 Client/record/UUID 被原子捕获。worktree `python3 x9/tools/call_context_evidence.py --mode manager-queue --out x9/out/call-manager-queue-final-20261126` exit 0，原切片/proof 保留在主 checkout 同目录。未部署 typed ingress。短信 `b4030197` 提取真实 TAG 结果分支测试 pre-read、mark、readback、超时及 hold，Xcode 15 下 `python3 -I x9/baseband/test_sms_tag_stages.py` exit 0；无实际 SIM 修改、投递或持久化证据。
- `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_surf_events=1 --name x9-native-event-wired-20261126-r5` exit 0（104/104 overlay，3 prebuilt），boot 15,806,464 B、kernel 33,111,392 B，候选目录含 build.log，allowlist 280 未改。无 CI、设备操作、root/mailbox 或 push。上机须针对 r5 明确授权写入 boot_b、启动、采集；最新设备证据仍是 430。完整功能目标不变。

### 6.27 2026-11-26 SharedEvent 调度去抖

- 基线 `547b4b5d`，修复 `bda953ac`。counter 仍单调更新并唤醒真实 waiter，但 selector 37 仅在有 pending 通知首次跨过阈值时安排 worker。没有通知、尚未达标或只是继续推进已达标 counter 时不安排额外 worker，也不把现有的 Mach 队列满延时重试挤成立即尝试。新订阅若已达标仍立即调度。
- Xcode 15 `DEVELOPER_DIR`/SDKROOT 下 `clang++ -std=c++17 -O1 -Wall -Wextra -pthread x9/tools/tests/shared_event_pending_host.cpp -o x9/out/x9-event-ready-schedule-20261126/pending`，编译与执行均 exit 0，含无订阅推进、未达阈值、跨阈值和不覆盖已安排重试。新回归在旧实现 exit 134（预期断言），不是设备崩溃。 `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/out/x9-event-ready-schedule-20261126/compile.py` exit 0，`SweetSurface.o` 560,016 B/12 warnings。
- 本轮 6.1terra 三条续查均因额度未产出；通话与短信不新增完成证据。无新整包/CI/设备或 push。r5 不含这次调度改动；不影响用现有 r5 收集实验接线与 selector42 回退的首份设备证据。实际止崩、typed 通话及 SMS 全流程验收仍缺。

### 6.28 2026-11-26 WMS storage 收执退休边界

- 基线 `879c95c2`，修复 `96a53adb`。收执入队失败时若已观察到同 slot 的另一个非零 WMS QMUX allocation generation，退休旧终态并清理缓存，不再用旧身份重试；零身份不等于替换，仍保留收执。真正入队仍由 bridge 锁内原始身份校验；客户端本地失效也清除长度、内容及 pending。上述退休不取消 MD token 或解除 TAG hold。没有声称在设备上观察到该竞争。
- Xcode 15 `DEVELOPER_DIR` + `SDKROOT=$(xcrun --sdk macosx --show-sdk-path)` 下，`python3 -I x9/baseband/test_sms_storage_<unit>.py` 的 terminal/deadline/receipt/poll_hold 4/4 exit 0，合成测试覆盖 replacement、暂时零身份、原 token/hold 不变。本轮 baseline fixture 编译成功，执行在 client-loss 未清长度断言 exit 4（外层 unittest exit 1）。`PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/out/x9-sms-receipt-generation-20261126/compile.py` exit 0，`X9Baseband.cpp.o` 2,233,424 B/34 warnings，同目录保存日志。
- 本轮未增加通话或 SharedEvent 的设备证据。无整包、CI、刷机、重启、root/mailbox 或推送；r5 不含 §6.27–6.28 的变更。目标仍要求真实 SharedEvent/AegirPoster、原生拨接与双向音频、短信收发及持久化验收。

### 6.29 2026-11-26 SharedEvent r6 实机证据候选

- 基线 `e75ddb50`，构建源码 `25ac3353`。`x9ed=1` 是 `x9_surf_event_diag=1` 的短别名（长名优先）；与 `x9_surf_events=1` 并用时 argv 604 B（不含 NUL）。诊断按原 48/16 额度记录 selector 入口，并仅在实验路径记录对应返回码；以 comm/PID/seq/selector 对齐，不输出地址、端口值、scalar 值、用户引用。未改变协议返回行为。r6 还包含 §6.27–6.28 的调度及 WMS 收执退休修复。
- `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_surf_events=1 --boot-arg x9ed=1 --name x9-native-event-wired-20261126-r6` exit 0（104/104 overlay，含 3 prebuilt），boot 15,806,464 B、kernel 33,095,048 B；候选目录含 `build.log`，allowlist 280 未改。无 CI、刷机、重启、root/mailbox、push。子代理仍因额度不可用。
- 若明确授权写入 r6 的 boot_b、启动及只读 USB 采集（采集后动作另行确认），先以同一日志的 boot-args、`SWEET SURF EVENT` 与 `SWEET SURF EVENT RESULT` 确定实际分支，按 PID 对齐首个 AegirPoster abort。selector 42 Unsupported 仅覆盖有静态依据的普通 present 回退，不等于 stop-crash；调用日志没有回调交付或通话/短信对端/持久化证据。当前设备观察仍只有 430，不标记全流程完成。

### 6.30 2026-11-26 r6 日志离线判读工具

- 基线 `12ba36bb`，工具 `de73d45a`：`python3 -I x9/tools/shared-event/review_log.py <capture>` 从单次本地日志对齐入口/返回及首次 AegirPoster SIGABRT；重复/孤立诊断项或 boot-args 缺失/不唯一时不输出首次崩溃关联。只打印摘要，不判断止崩或功能通过。 `python3 -I x9/tools/shared-event/test_review_log.py` exit 0（2 项，合成解析 fixture，不是设备样本）。
- 同工具读取已有 `~/430.txt` exit 0：实验/诊断 flags 均未出现，0 个新格式调用记录；44 个 SIGABRT，38 个在首个 display-off 之后；首次 30.682 s。摘要在 `x9/out/shared-event-log-review-20261126/430-summary.json`，不是 r6 trace。无新源码运行改动、构建、CI 或设备操作；r6 已存在，仍等待针对 r6 的写 boot_b、启动和采集明确授权（也可由用户提供已采日志）。通话/短信无新验收。

### 6.31 2026-11-26 r6 写入 boot_b，等待启动

- 操作前 HEAD `04d39b10`。`fastboot devices` 仅一个设备，`fastboot getvar product` 为 `k6993v1_64`、`is-userspace` 为 `no`、`current-slot` 为 `b`、`partition-size:boot_b` 为 `4000000`；查询均 exit 0。
- 按用户要求执行 `fastboot -s "$id" flash boot_b x9/out/x9-native-event-wired-20261126-r6/boot.img`（`id` 取唯一 fastboot 设备，未记录序列号），Sending/Writing 均 OKAY，exit 0。镜像 15,806,464 B，构建源码 `25ac3353`。无新构建、CI、切槽、root/mailbox 或 push。
- 首次以 nohup 启动的 logger 未在执行会话退出后保留；确认其日志 0 B 后，用常驻执行会话 49582 运行 `python3 x9/tools/usb-live/usb_log.py --output "$HOME/432.txt" --quiet`。等待 USB 1209:0001，日志当前 0 B。尚未重启；下一步单独确认后 `fastboot reboot`，再读新日志区分 r6 启动与功能验收。

### 6.32 2026-11-26 r6 重启与首段 USB 日志

- 操作前 HEAD `79273b8c`。确认唯一 fastboot 设备、槽位 b、`is-userspace: no`，用户确认后执行 `fastboot -s "$id" reboot`，exit 0 / Rebooting OKAY；未再次写分区或切槽，无构建、CI、root/mailbox 或 push。
- 原采集会话 49582 保持运行。`~/432.txt` 初始空，随后收到 36,648 B；读到 `USB shim log ready`、`x9 shim: direct XNU loader`、保留 LK scanout 与 LK 记录。文件在数次检查中未继续增长。当前只证实到 shim，未见 XNU 交接、boot-args 或 launchd；不据此判断死机。
- `python3 -I x9/tools/shared-event/review_log.py "$HOME/432.txt"` exit 0，首段无 boot-args、SharedEvent 调用或 AegirPoster SIGABRT。保持 logger，不重启；下一步读取同一捕获的新字节并核对启动阶段。无功能验收。
- 同一捕获随后继续增长；复读时 426,793 B，`review_log.py` exit 0，唯一 boot-args 同时含 `x9_surf_events=1`、`x9ed=1`。设备已到 iOS 用户态：launchd 8.088 s 完成 early boot，SpringBoard 9.141 s 输出进程日志。日志尾部约 11.99 s；该快照尚无 SharedEvent 调用或 AegirPoster SIGABRT，时间窗不足以判断止崩。采集继续。

### 6.33 2026-11-26 r6 触摸失效

- HEAD `f9187ca1`，用户报告触摸失效、无法进入系统界面。`~/432.txt` 显示 S3910 两路电源读回开启、reset 已释放且 IRQ 低，但 2.385 s 首个 TCM header 为 `ff ff ff ff`，2.390 s identify 失败；`touch_run()` 立即返回，未进入触摸报告循环。9.254 s backboardd 打开 SweetHIDTouch 成功，不能证明硬件有输入。
- 对照 `~/430.txt`：同阶段读到 `a5 10 1c 00`，识别 `S3910VA0A0-15.20`，随后记录 512 reports。`git diff --stat 2134b216 25ac3353 -- x9/kernel/src/bsd/dev/sweet_x9_touch.c x9/build/boot-device.json` exit 0 且无输出；一次失败不能确定是偶发、时序差异或其它初始化相互影响。USB logger 仍在运行。无新构建、CI、重启、刷写或 root/mailbox；建议下一次保留 r6 不换镜像，以新日志复核首个 TCM header，设备动作单独确认。

### 6.34 2026-11-26 r6 回到 bootloader fastboot

- HEAD `7f7f7498`，按用户确认执行 `python3 -u x9/tools/ios-reboot-bootloader-usb`：USB 控制请求完成，helper 观察到 fastboot，exit 0；随后 `fastboot getvar is-userspace` 为 `no`、`fastboot getvar current-slot` 为 `b`，均 exit 0。
- 旧 logger 以 SIGINT 停止，`~/432.txt` 保留 2,280,528 B。执行 `python3 x9/tools/usb-live/usb_log.py --output "$HOME/433.txt" --quiet`（常驻会话 99876），等待 USB 1209:0001。没有刷写、切槽、启动 iOS、root/mailbox、新构建、CI 或 push；下一步获确认后不换镜像启动 r6，比较新日志首个 S3910 TCM header。

### 6.35 2026-11-26 同一 r6 重启复测触摸

- HEAD `f63f99ec`；经用户确认，唯一 fastboot 设备经 `fastboot -s "$id" getvar is-userspace` / `getvar current-slot` 确认为 bootloader、b 槽，`fastboot -s "$id" reboot` exit 0 / Rebooting OKAY。未重刷、未切槽、未新建构建、CI、root/mailbox 或 push。
- `~/433.txt`：2.166 s 首个 TCM header 为 `a5 10 1c 00`，2.173 s 识别 `S3910VA0A0-15.20`，2.177 s application status 0000，2.216 s LBP reporting enabled；SpringBoard 8.341 s，SweetHIDTouch 8.561 s 打开。694,900 B 快照中 21.405 s 仍为 reports=0。与 432 对照证明同一镜像重启后识别阶段未复现失败，不等于根因修复、实际触点或解锁验收。原 99876 logger 继续采集，等用户滑动实测。
- 后续同一日志 1,248,692 B 时已见真实触点：26.813 s report 1 contacts=001，26.883 s report 10 contacts=000，27.124–27.199 s 再有按下/抬起，均 HID dispatched。已证实内核触点上报恢复，未确认锁屏 UI 接受或解锁；采集保持运行。

### 6.36 2026-11-26 433 功能反馈与返回 Android 前检查

- 用户确认触摸恢复；蓝牙按钮灰、通话/短信未通过。433 中蓝牙固件 session 和 HCI 扩展能力均到达，未见 Preferences setup 诊断。82.118 s VOICE20 进入已测 SIM2 路径，82.120 s 实际提交 0901，当前捕获未见 0902 或接通证据。SIM2 MO 两次收到 modem CNF（不是对端投递）；SIM1 有拒绝 CNF；双卡 SIM 列表 0608 返回 0x210000。不要把这三项判成未初始化或已修复。后续先用精确事务追 CNF/超时与短信回执，蓝牙仍需 Preferences 诊断。
- HEAD `e231e6fe`；用户要求先回 Android。执行 `python3 -u x9/tools/ios-reboot-bootloader-usb` exit 0，重新进入 bootloader fastboot。只读 `fastboot getvar` 均 exit 0：is-userspace=no、current-slot=b、slot-successful:b=yes、slot-successful:a=no、slot-unbootable:a=no、slot-retry-count:a=0。历史有 Android active-b 记录，不能推定当前 a 是已验证的 Android 启动槽。停止原 USB logger，保留 `~/433.txt` 3,108,681 B。未切槽、未刷写、未启动 Android、无构建/CI/root/mailbox/push；等确认 Android 启动路径。

### 6.37 2026-11-26 Android 恢复成功

- 基线 `d1b76aa2`。用户指定 `/Users/tosasitill/boot.img`；`file` 识别 Android bootimg，`stat -f %z` 为 67,108,864 B。唯一设备处于 bootloader fastboot、b 槽，`boot_b` 大小 `4000000`，预检 exit 0。
- `fastboot -s "$id" flash boot_b /Users/tosasitill/boot.img && fastboot -s "$id" reboot` exit 0，Sending/Writing/Rebooting 均 OKAY。未切槽或改写其它分区。唯一 online adb 设备执行 `adb -s "$id" shell 'getprop ro.product.model; getprop ro.boot.slot_suffix; getprop sys.boot_completed'` exit 0，依次返回 PLJ110、_b、1，确认 Android 已在 b 槽启动。
- `boot_b` 现为 Android 恢复镜像；r6 仍保留在原主机候选路径。流程收录在 §3.5 和 AGENTS.md；无构建、CI、root/mailbox 或 push。蓝牙、通话、短信仍待按 §6.36 继续定位。

### 6.38 2026-11-26 SMS 433 离线跟踪

- 基线 `e9a51543`。433 中 SIM2 0605 token 640/QMI txn 27 在 53.410 s 提交，59.269 s 返回 modem accepted；token 1386/QMI txn 29 在 155.096 s 提交，159.457 s 返回 accepted。均非对端投递；日志无可关联 delivery report 的证据。SIM1 token 1242/QMI txn 26 在 121.551 s 提交，121.554 s CNF result `0x21014b`，QMI error 3；不能仅凭结果码声称根因。双卡 0608 均 `0x210000`，未证明来源是 CMGL fail helper 或空存储。
- 为后续真实采样加 0606 关联诊断，记录 SIM、MD token、QMI txn、known 和结果，不记录 PDU、号码或 message reference，不改变返回逻辑。无错误码猜测或行为修复。Xcode 15 `DEVELOPER_DIR=$(xcode-select -p)` 和 macOS SDK 下 `python3 -I x9/baseband/test_sms_storage_{terminal,deadline,receipt,poll_hold}.py` 各 exit 0；`PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/out/sms-433-verification/compile.py` exit 0，X9Baseband.cpp.o 2,232,632 B，34 warnings。首次错误指定 `/Applications/Xcode.app` 路径导致四个测试 exit 1，改为实际 Xcode 路径后通过；单文件日志原在 worktree 的 `x9/out/sms-433-verification/`，现归档至 §6.39 候选目录的同名子目录。该步骤无完整候选构建、CI、设备操作、发送短信或 push。短信收发、真实回执及持久化均未验收。

### 6.39 2026-11-26 433 通话终态诊断与内核候选

- 使用 6.1 Terra 分析 433。VOICE20 txn 5 在 82.118 s 入桥，82.120 s MIPC 0901 txid 1042 发送返回 0；433 没有可关联的 QMI 给主机终态。MIPC 没有通用 RX 消息打印，**不应推断没有收到 0902**。不能据此断言拨号超时、接通或有音频。
- 通话源码 `31b04115` 增加 owned CNF 解析、take 返回、本地 60 s deadline、MO join 和队列终态/owner 退出日志，不输出号码、不改变处理逻辑。Terra 用 Xcode 15 编译 `x9/tools/tests/call_bridge_host.cpp`，并运行 `dial_roundtrip ios_timeout ios_deleted ios_join dial_tx_failure`，5/5 exit 0。首次错误指定 `/Applications/Xcode.app` 编译失败，后用实际 `/Users/tosasitill/Downloads/Xcode.app/Contents/Developer` 通过。
- 短信补证：`x9/out/call-ios-bridge/tools/md-dis .local/x9/modem/parts/modem_a.img 0x91409380 0x914093a0` exit 0，CMGS fail 读入 +6 半字，调用与 CMGL fail 相同的 AT 结果 helper，在结果槽为 0 时写回。输入 0x14b 可以形成 0x21014b；这不是 433 的运行时调用路径，也未证输入含义。原切片同下候选目录 `sms-cmgs-fail.asm`。
- 构建源码 `31b04115`（含短信诊断 `b20c752c`）：`PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_surf_events=1 --boot-arg x9ed=1 --name x9-radio-followup-20261126-r1` exit 0，104/104 overlay（含 3 prebuilt），boot 15,806,464 B / kernel 33,111,432 B，`build.log` 在同目录。无 CI、设备操作或 push；手机留在 Android。此镜像只新增通话/SMS 诊断，不更新蓝牙 root dylib、不宣称功能修复。

### 6.40 2026-11-26 433 蓝牙崩溃与下次采样

- 6.1 Terra 在 433 定位 194.615 s `bluetoothd` SIGSEGV（PC/FAR=0），晚于 10.582 s controller capabilities。只读 `llvm-objdump -d --start-address=0x1000e6a8c --stop-address=0x1000e6ac8 /private/tmp/x9os/usr/sbin/bluetoothd` exit 0，和去 slide 的 LR 一起支持 H4 transport 的可选 teardown 回调空跳转；与 Vendor16 表分离。两个静态直接调用者，不能推定全局填槽安全，亦不能由这次晚崩溃解释较早的灰按钮。证据见 [Bluetooth README](x9/bluetooth/README.md)。
- 全局填槽实验虽通过 host 条件测试及 iOS arm64 编译/签名（122,912 B），已撤回，**未合并，不安装**。隔离产物在 `x9/out/radio-bt-h4-rejected-20261126/`；集成只含蓝牙文档 `f54f225d`。本轮无蓝牙生产改动。
- r1 未启用 Preferences setup 诊断；当前主树生成的旧 BT root header 不含 `X9SettingsBTDiag.dylib`。下一次独立候选应保留当前 kernel，配匹配的 Preferences-only 诊断 root payload；确认确切已装 predecessor、原件 hash、root preflight 和单独安装授权后，记录每项 setup 调用与真实返回，同时确认退出前首个 teardown 的触发条件。不能只加 boot-arg 就当采样生效。通话/SMS 则可先用 §6.39 r1 捕获新事务。手机仍在 Android，未刷写、重启或安装 root，三项未验收。

### 6.41 2026-11-26 通话 / 短信终态保护 r2

- 基线 `573a9f33`。短信 `349eb082`：RawSend20 先记录真实 WMS allocation generation；固定结构的成功/错误终态在 QMUX 满或分配失败时保留，稍后只重试入队，不重发 0605。原生 token 仍排空，晚 0606 不改写首个终态；明确检测到 client 替换后不把结果转给新会话。活跃同事务包括畸形重试在通用校验前接管，避免第二个 BUSY/BAD 抢终态。非永久去重，不修改 modem 错码或投递语义。
- 通话 `ebae5a35`：原 VOICE20 owner/待交付终态在通用 TLV 校验前接管同事务畸形重试；不改变正常拨号、MO join 或音频路径。二者是源代码可验证的可靠性修复，未证明触发了 433 的故障。
- Xcode 15 `DEVELOPER_DIR=$(xcode-select -p)` / `SDKROOT=$(xcrun --sdk macosx --show-sdk-path)`：`python3 -I x9/baseband/test_sms_storage_<unit>.py` 的 terminal（2）、receipt、deadline、poll_hold、reuse 合计 6/6 exit 0；`xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable x9/tools/tests/call_bridge_host.cpp -o x9/out/x9-radio-followup-20261126-r2/call` exit 0，`voice20_dispatch ios_timeout ios_deleted ios_join dial_tx_failure` 5/5 exit 0。覆盖满队列保留、首个终态、代际隔离、畸形重试归属；不是设备数据。
- 构建源码 `ebae5a35`：`PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_surf_events=1 --boot-arg x9ed=1 --name x9-radio-followup-20261126-r2` exit 0，104/104 overlay（含 3 prebuilt）、boot 15,806,464 B、kernel 33,127,840 B；候选目录有 build 和 host 测试日志。替代 r1 用于下次通话/短信采样。无 CI、刷写、重启、root/mailbox 或 push；仍为上次确认的 Android 启动状态。蓝牙无新增生产改动；按 §6.40 独立准备 Preferences 采样，不能宣称灰按钮修复。

### 6.42 2026-11-26 SMS 状态报告离线边界复核

- 基线 `32bd65144f0b964d0e41762d2c9565bab2246782`，仅检查现有源码与 §6.38/6.39 证据；无设备操作、构建或 push。`0x21014b` 已知可由 CMGS fail helper 的输入 `0x14b` 形成，输入含义和 433 实际调用未证明，不能据此更改 SIM1 路由或错误码。
- 静态观察：`X9Messages.inc` 的 `8603` 接受 TLV `8100` 且验证 SMSC 偏移及 TP-MTI=2；仅单 SIM mask，发送给同 SIM 的 WMS client，要求 MT events、routes、class 4 和 status_reports 均启用。队列未接纳则保留 PDU 重试；接纳后保留 token 等待 WMS37 ACK；`x9_mipc_messages.inc` 的 publication 和 reply 重新校验原 token/mask/MTI。重试从原 PDU 的 TP-MTI 重新计算，状态报告仍走 class 4。上述仅源码路径，不是实际状态报告或投递验收。
- 433 无可关联 delivery report；已有 Android 快照无短信正文或状态报告 wire。尚缺真实 8603 / Android 同事务的非敏感 shape、route/subscription、WMS ACK 及最终对端结果，无法证明该路由与设备实际合同一致；不合成回执、不改行为。下一次授权采样应优先核对这些边界及 SIM1 modem 实际拒绝上下文。

### 6.43 2026-11-26 Terra 后续：蓝牙回滚与通话复核

- 基线 `32bd6514`。蓝牙候选构建器 `ee1d1453` 先前从回滚 argv 删除 `x9_bt_init=1 x9_bt_fc=1`，但设备板级默认两者均为 1；删除不等于关。现在原位改成 `x9_bt_init=0 x9_bt_fc=0`，若有诊断选择项则改成 0，保留其它 token 和长度。 `python3 -I x9/bluetooth/test_settings_diag.py` 5/5 exit 0，含有/无诊断的回滚配置。此项是主机构建器修复，不会改写已存在的 r1/r2 或旧回滚镜像。
- 6.1 Terra 独立复核 433：仍仅证 VOICE20→0901 TX；已有 Android 记录明确 0902 成功结果不含 call ID，需同一事务后续的完整 4901 作为 ID/epoch 证据。无新证据支持改变通话状态机或音频；r2 的实机事务诊断是下一道验证。
- 当前无新整包或 CI，无设备操作、root/mailbox 或 push。最后一次已验证设备状态仍为 Android；未验收蓝牙、通话、短信。
- 蓝牙诊断准备 `97507016`：`python3 -I x9/bluetooth/test_settings_header.py` 2/2 exit 0。实际 fixture 之外，`python3 -I x9/bluetooth/settings_header.py --payload .worktrees/bt-default-on/x9/out/x9-bt-settings-diag-20261125-r2/root-payload --apple-originals .worktrees/bt-default-on/x9/out/bt-settings-inputs-20261125/apple-originals --out x9/out/x9-bt-settings-scope-20261126/x9_bt_root.inc` exit 0；离线 header 仅含 118,416 B 的诊断 dylib，不含 H4/jobs/cache/Settings plist，未覆盖当前 generated header。完整 payload 的 hash/mode 和 Apple 原件校验仍执行。这不证明现机前序安装或足以上机：原 stage_targeted 无单文件模式，通用 PID1 installer/restore 会按临时文件名清理 regular 文件，尚需收窄或验证所有权，再构建独立诊断镜像与 root preflight。标准配置加 root+诊断为 615 B，超过 607 B；脱离标准配置的变更必须保持现有语义。

### 6.44 2026-11-26 通话 MD token 关联诊断（隔离分支）

- 基线 `2370e2fde87e2d9137fdf1f2d7111c75d3eb59fb`。源码观察：分配时内部 token 同时写为 16-bit MIPC txid；CNF 必须在 state 2 按 txid 和 request id + 1 匹配，才转 state 3 给 `take`。r2 的 VOICE20 日志只打印 QMI txn，无法稳定关联并发的 MD TX/CNF。
- 最小诊断分别在 VOICE20 接纳时打印 QMI txn / MD token / SIM，以及守护 DIAL 的 0902 成功匹配时打印 token / txid / 长度 / cancelled；不打印号码或帧内容，不改线协议、状态机或拨号行为。未匹配 CNF 已有通用 no owner 日志；不代表无日志必然没有物理 RX（流可能提前失败）。
- `DEVELOPER_DIR=$(xcode-select -p) SDKROOT=$(xcrun --sdk macosx --show-sdk-path)` 下 `xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable x9/tools/tests/call_bridge_host.cpp -o x9/out/call-md-correlation/call` exit 0；依次运行 `voice20_dispatch ios_timeout ios_deleted ios_join dial_tx_failure` 均 exit 0。仅 host 测试，不是设备结果；无完整构建、CI、设备操作或 push。后续需以新候选和真实采样确认；不声称通话或音频已修复。

### 6.45 2026-11-26 AI 账号迁移（任务暂停）

- 交接文件：[X9 AI 账号迁移](docs/2026-11-26_x9-ai-handoff.md)。当前功能分支 `feat/bt-settings-offline-candidate`，文档前源码 `03e6f889`，main 仍 `2370e2fd`；保留当前分支和本轮 worktrees，未合入/推送。
- `e7c401c8` 生成 verify/install/rollback 各 601 B，命令 `python3 -I x9/bluetooth/settings_boot.py --out x9/out/x9-bt-settings-boot-config-20261126` exit 0；`55a444ff` 添加通话 QMI→MD CNF 关联，Terra 针对 host 5/5；`03e6f889` 限定蓝牙单文件并拒绝预存临时名，`python3 -I x9/bluetooth/test_settings_<boot|temp|header|diag>.py` 10/10 exit 0（Xcode 15 SDK）。
- 中断前只建了 `.worktrees/bt-settings-candidate-host`，HEAD `55a444ff`、`.local` 链及私有 config 就绪，`prepare-local.py --check` exit 0；未生成该工作树的 scoped header 或完整候选，无活动构建、logger、设备操作或 CI。最新完整候选仍为 §6.41 r2；最后已验证手机状态仍是 §6.37 Android。恢复后先按交接文档核对并快进专用 build worktree；任何上机动作重新确认授权。

### 6.46 2026-11-26 SMS 入站 owner 静态边界复核（补录 `87ca29cb`）

- 来源：`.worktrees/sms-inbound`（分支 `fix/sms-inbound`，HEAD `87ca29cb`，基线 `2370e2fd`，纯文档无代码改动）。
- 只读源码：`coachz_baseband_poll()` 与 `x9q_poll()`/`x9m_poll()` 由同一 worker 顺序调用；QMUX slot 在 `release_pending` 清零前不可复用，而清零前先调用 `x9m_del()` 清除订阅。因此未证旧 routes 在正常顺序下转移给复用 slot；单次 admission generation 栅栏实验已撤回，不称为修复。transport/adapter 各为 16 slots；不能仅因 `x9m_event` 的 full 分支就断言正常流丢失 owner，尚需证明容量关系失效的可达路径。
- 实际入队收执只证明 host 队列接纳，不是 WMS37、物理 ACK、持久化或对端投递；队列满保留原 PDU，成功入队擦除 PDU 并等待同 SIM/slot/local generation 的 WMS37。MD ACK TX 失败保留 owner 不重试。真实 8601/8603 事务及最终接收仍未验收。

### 6.47 2026-11-26 恢复推进：scoped BT verify 候选、194 s teardown 根因与 Call/SMS 非 PII 诊断

- **01a216b2 对话恢复与首版 verify 候选（`candidate_ready`，未上机）**：
  - `.worktrees/bt-settings-candidate-host` 快进至 `03e6f889`，`prepare-local.py --check` exit 0；生成 `x9/out/x9-bt-settings-radio-plan-20261126/boot-{verify,install,rollback}.json`（各 601 B）与含 `#define X9_BT_ROOT_SETTINGS_ONLY 1` 的 `x9/kernel/generated/x9_bt_root.inc`（嵌入 118,416 B `X9SettingsBTDiag.dylib`）。
  - 构建 `x9-bt-settings-radio-verify-20261126` exit 0：104/104 overlay（3 prebuilt），`boot.img` 15,409,152 B，`kernel.development.sweet` 32,095,296 B，`kernel.development.sweet.unstripped` 38,195,464 B，allowlist 280 未变。
- **433 ~194 s `bluetoothd` PC/FAR=0 teardown 崩溃离线根因（静态分析，未改生产 dylib）**：
  - 直接外部诱因是 194.325 s USB reboot-bootloader 触发关机，`bluetoothd` 进入异步关机路径并经 `0x1000f3b4c` 调用 `0x1000e6aac`（另一处调用点 `0x1003ca25c` 在 halt 路径）。
  - `0x1000e6aac` 读取 HCI Transport 表 `*0x100844850 + 0x60`（`H4BC`/`APPLEBT`/`PCIE`/`PCIESW` 实现为 pre-close halting mark），但 Apple 仅豁免 `type==0`（USB），漏判通用 `H4` 表（`0x100844228`）`+0x60 == NULL` 导致空跳转。与 Settings 灰按钮无直接因果；与 `X9Vendor16.c` 管理的 type-0 vendor 表分离。
- **Call / SMS 零行为变更可观测性补强与只读单文件 BT preflight（源码 `515fd28b`，`host-verified`）**：
  - `x9_call_state.h` / `X9Calls.inc` / `x9_mipc_calls.inc`：补录 `0902` CNF 的 32-bit `result`、`MO epoch joined` 的 `id`/`epoch`，以及 `x9mc_rx` 处理 `0x4901` 时的受限无 PII 摘要（`mask/id/dir/mode/ev/cause/apply/ep/blk`）与 parse/frame 毒化日志；不打号码/PAU/detail，不改状态机。
  - `X9Messages.inc`：将受限失败 TLV 摘要扩展到 `X9M_SEND`（`0606`）与 `stored`（`0608`），对 `tag < 0x8000 && 1..4 B` 短标量打印 `tag/len=0xval`，对 `>=0x8000` 或长载荷只打 `tag/len`。
  - 新增 `x9/bluetooth/preflight_settings_diag.py` 与 `x9/bluetooth/test_preflight_settings_diag.py`（严格只读，禁止 write，校验 clean journal、`/usr/lib` 非 symlink、`.x9-bt-temp` 不存在、单文件前提及 Apple `bluetoothd`/`BlueTool` 哈希）。
  - Xcode 15 SDK host 回归全部 exit 0：Call `voice20_dispatch ios_timeout ios_deleted ios_join dial_tx_failure` 5/5；SMS `test_sms_*.py` 8 文件 10/10；Bluetooth `test_preflight_settings_diag.py`（6）+ `test_settings_{boot,temp,header,diag}.py`（10）合计 16/16；离线 CLI preflight 校验 r2 payload exit 0（`device_probed=false`）。
- **最新 verify 候选 `x9-bt-settings-radio-verify-20261126-r2`（源码 `515fd28b`，`candidate_ready`，未上机）**：
  - 在 `.worktrees/bt-settings-candidate-host` 快进至 `515fd28b` 执行 `PATH=/opt/homebrew/opt/llvm@22/bin:$PATH python3 -I x9/build/build.py all --boot-config x9/out/x9-bt-settings-radio-plan-20261126/boot-verify.json --name x9-bt-settings-radio-verify-20261126-r2` exit 0。
  - 104/104 overlay（3 prebuilt），`boot.img` 15,409,152 B，`kernel.development.sweet` 32,095,336 B，`kernel.development.sweet.unstripped` 38,195,856 B，allowlist 280 未变。
- **设备边界与未完成**：未触碰设备，无刷写、无重启、无 root/mailbox 写入、无 CI、无 push；最后已验证设备状态仍为 §6.37 Android。下一步需分别向用户确认：① 是否将 `x9-bt-settings-radio-verify-20261126-r2` 写入 `boot_b` 并采集新 USB 日志；② 若 root 尚未装 `X9SettingsBTDiag.dylib`，是否另行授权只读 HFSX preflight 与单文件安装。
