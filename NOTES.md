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

本轮文档整理基于 `994d5fe0`，README/规则提交为 `d6f92195`；未构建、未刷机、未重启、未写 root/mailbox，也没有新 CI run。

## 6. 分系统当前交接

### 6.1 蓝牙：从“桥崩溃”转向“原生可用性与音频”

**观察**

- `boot-device.json` 只有 `x9_bt=1`；`x9_bt_init=1` 才启动 worker，`x9_bt_fc=1` 才配置流控。完整初始化候选必须显式追加两者。
- FE01 是 connac3 动态固件 section 请求，不是普通 Command Complete 错误。内核消费它并按 section 重载；Android `btmtk_main.c` 是参考。
- `0xE0..0xFF` 厂商事件在 Apple H4 parser 前消费；普通 Command Complete 不过滤。log 425 证明已定位的 reason-607 崩溃消失。
- 原生 HCI 扫描参数/开关与 advertising report 解析仍在 Apple 栈；没有 X9 自建发现引擎或伪造回调。
- Settings 行可见不等于 `BluetoothManager.available`。后者要求 `[self+0x18]==2`，只有五个原生 setup 调用全部成功才写入。

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

- `0x210000` 现归于 `l5usms_get_l5_res_from_at` 通用 AT ERROR，但缺独立反汇编确认；不能解释为空收件箱。
- query-all/filter、混合异常 status、CDMA type4、durable SIM storage 与 Messages UI 持久化未验收。

**下一步（离线）**：反汇编 `md-symbols.json` 中 bounded `l5usms_get_l5_res_from_at`（2436915934..2436916060）及 caller，确认/否决 0x210000 归因。

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
- `SIOCGIFMEDIA` 到 PDP default case，打印后返回 `EOPNOTSUPP`；caller 未证，不能直接返回 ACTIVE 改变拨号行为。
- `x9_mdlog=0` 只去掉独立高吞吐 capture/save 线程；stage-1 DHL polling、打印与 ring 推进仍在。
- `~/428.txt`：灭屏后 retention sleep，约 1 Hz dark 事件；PDP 每约 2 s 八个失败；WDS 每 3 s；little 40–50%/500 MHz；大量 MIPC/MDLOG。57°C 出现在 `dark=0`，**没有复证 dark 57°C**。

**推断 / 下一步**

- 高置信：PDP/WDS/MDLOG 增加活动；中置信：render-owner 而非 dark timer 造成 GPU churn；低置信：MDLOG 本身造成测温差异。没有匹配 A/B。
- 下一步离线把 428 的 `X9 SAMPLE` 线程记录与 201.107 s PDP burst、201–203 s GPU transitions 对齐。未来 instrumentation 只用有界内核计数/返回地址和既有采样，不 live sysfs。

### 6.7 点击亮屏 / 抬腕唤醒

**观察**

- `sweet_x9_touch.c` 明确不路由 touch IRQ；只处理普通 report `0x11`。没有 gesture 命令、decoder 或 wake key。当前 contact 进入普通 digitizer/HID 链。
- S3910 厂商实现不在本地；现存 Goodix DT 不是本机 S3910 证据。
- XNU 的 SCP 只保留 accelerometer type1、proximity type8、light type5；不消费 pickup/significant-motion one-shot。
- `X9Accelerometer` 发布连续 type13 XYZ；orientation bridge 只做姿态滤波，不做 wake。
- Android nanohub 会为一次性 wake gesture 发 250 ms PM wakeup，但那是传输恢复，不是 UI 亮屏。

**未完成 / 下一步**

- 点击亮屏需先找 S3910 gesture-mode 命令、report code 与 Android KEY_WAKEUP/HAL 映射，再证明 SpringBoard 接受标准 Consumer Power 或合格 wake 事件。
- 抬腕唤醒先查完整 SCP LIST 是否有 pickup/wake ID，再定义 screen-off-only、one-shot rearm/cooldown/stationary veto；不能只增加加速度事件。
- 当前没有 iOS 点击亮屏或抬腕唤醒工作链。
- 2026-11-26 host boundary：`python3 -I x9/motion/test_wake_boundary.py` exit0（5 tests），直接编译真实 `sweet_x9_motion.c`，确认 ACK 前 DATA 拒绝、 stale 过期、disconnect 失效、晚 ACK 不能清 fault；source 检查确认 orientation bridge 无 wake/shake/touch/S3910 合成。功能仍未实现。

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

main HEAD `91be5aff`。以下均为 **host 验证 / 源码边界**，不是设备 boot、runtime_verified 或 feature acceptance；未刷机、未写 root/mailbox、未 push。

- SMS：`fix/sms-storage` 已合并。修正 SMS-DELIVER OA semi-octet 边界后的 DCS/class 解码；pinned Xcode 15 下 `test_sms_pdu_class.py` 2/2 OK，`X9Baseband.cpp.o` 单对象编译 True（2,229,712 B）。
- GPU：`fix/gpu-pacing` 已合并。面板 DCS 与软件节拍只支持 60/120； unsupported explicit rate 拒绝/回退 60。`test_cadence.py` 2/2 OK，`SweetFramebuffer.o` 单对象编译 True（595,304 B）。
- 暗屏调度：`fix/dark-sleep` 已合并。RRC 诊断默认关闭，`x9_cell_diag=1` opt-in；`test_cell_diag_policy.py` 3/3 OK，`X9Baseband.cpp.o` 单对象编译 True（2,229,968 B）。无设备温度/功耗 A/B。
- 蓝牙音频：`fix/bt-audio-pcm` 已合并。仅固化 fail-closed publication/IO 边界；`test_bluetooth_boundary.py` 2/2 OK。真实 PCM consumer/XPC/supplier 仍未建立，audio_verified 不变。
- 距离传感器：`fix/sensor-bridge` 已合并。`test_proximity_bridge.py` 8/8 OK，直接编译真实 `sweet_x9_proximity.c`；PS 极性 `oneshot==0 -> near` 仍是未验证假设，ALS 物理链未新增设备证据。
- 通话：`fix/call-owner` 已合并。`test_call_owner_boundary.py` 8/8 OK，直接编译真实 `x9_call_state.h`；typed `ims::Call` owner/SIP/双向 PCM 仍缺。
- 唤醒：`fix/wake-gestures` 已合并。见 §6.7 host boundary；点击/抬腕功能仍缺 S3910/SCP one-shot 合同。
- 蓝牙 Settings 诊断：`fix/bt-discovery`  commit `e67688db` 已修复 dyld slide 地址并通过 `x9/bluetooth/build.py` 与 `test_settings_diag.py` 4/4，但 **未合并 main**：其基线是另一窗口 `feat/bt-settings-root` `ab9828ea`，main 尚无 `X9SettingsBTDiag.c`。需先确认该基线归属/合并，再 rebase 或合并本修复；不得自行覆盖另一窗口工作。

## 7. 推荐下一步顺序

1. 保持文档与源码标签一致；任何新候选使用唯一名并记录 source HEAD。
2. 蓝牙：完成 Settings/scan 静态合同；r2 诊断候选只有重新授权后才上机。
3. 传感器：闭合 PS 极性与时间域；短信：独立确认 `0x210000`。
4. 通话：typed-provider identity/action 契约；不要先补音频 hook。
5. GPU/调度：完成离线 evidence matrix 与 428 时间线关联，再申请单变量设备 A/B。
6. 存储/USB：只读 IOMedia A0 与 AppSync session probe 分开推进。
7. 相机：先证明主摄多帧非黑与稳定 retire，再接第二路/完整 iOS 预览。
8. 点击/抬腕唤醒停留在离线来源追踪，不伪造 iOS 事件或样本。
