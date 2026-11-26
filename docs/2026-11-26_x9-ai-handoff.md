# X9 iOS 移植：AI 账号迁移交接

**交接日：2026-11-26（本机时区 Asia/Shanghai）**  
**原目标：**「继续追这几个方向 开 terrya」——上一轮明确指蓝牙灰钮、通话、短信，并要求用 6.1 Terra。**原目标已暂停，未完成。** 本文是交接，不授权继续构建、刷机、重启、安装 root 或操作 SIM。新账号收到用户恢复指令后再继续。

## 1. 项目与验收目标

- 项目为 `ios-sweet / X9`，本机权威 checkout 是 `/Users/tosasitill/ios_plj110`。设备是 OPPO Find X9（PLJ110 / OP5E17L1，MT6993，12 GiB RAM），蓝牙芯片 MT6653，触控 S3910。由 MTK LK 从 Android v4 boot image 进入 X9 shim、自编译 SWEET XNU（xnu-8796.141.3 补丁）、原生 UFS/HFSX 和未修改的 iOS 16.7.16 用户态（20H392，D22/iPhone X）。
- iOS 32 GiB HFSX root（16+16 GiB）在现有 Android `userdata` 的固定、不可移动 extent 内，由 `ufsroot0` 挂载。必须保留 Android、分区备份和用户数据。面板 1256×2760，ADT canvas 1242×2760；可见界面不等于 GPU 或输入功能验收。
- 本任务最终验收：**蓝牙**不仅按钮不灰，还需原生初始化、发现、连接/配对，BLE、A2DP/HFP 声音分别验；**电话**需原生拨接状态、对端接通及双向音频（不以写入 MD 或单次 CNF 代替）；**短信**需真实发/收、对端结果、相应 WMS ACK，以及 Messages 持久化（不以基带接受代替对端投递）。双卡问题分别追踪，不能擅自把 SIM1 请求改投 SIM2。
- 长期项目另有 Mac 配对、AppSync 安装、rootless Procursus / `/var/jb`、SSH 只走 `ue0`、Wallet/交通卡等目标，均不属于此次自动操作；不要操作支付、SE 或资金。

## 2. 用户偏好和硬性约束

1. 默认简体中文，命令和技术标识用原文。用户经常更换 agent，先读本仓库 `AGENTS.md` 和 `NOTES.md`，每个里程碑及停工前更新 NOTES：源码 HEAD、命令/退出码、产物字节数、是否碰过设备、剩余验证；持久技术合同写子系统 README。
2. 用户明确说过「用 6.1terra」「开 terrya」；如果新账号具备协作工具，恢复本任务时可用 `gpt-6.1-terra` 分工。此轮三个 Terra 分支已退出；不要把旧 agent 当作仍在运行。不要因通用的“继续”授权推定设备写入。
3. 先用真实运行日志和已有 Android/厂商/Apple 静态证据，再改代码。一次只改变一个验证变量；区分观察、推断、host-verified、candidate_ready、设备 boot、功能验收。禁止伪造传感器样本、MD 回包、状态、投递或安装成功。
4. 构建只用本 repo、固定 `.local/`、Xcode 15、KDK_13.5_22G74、Homebrew LLVM/lld 22、ccache、Python、lz4；不指向 `~/HelioDarwin` 等外部源码。验证 `python3 -I x9/build/prepare-local.py --check`。完整候选以 `x9/build/boot-device.json` 为基础，必须新 `--name`。按项目规则成功后不擅自加解包/哈希/反汇编/对比重建或测试套件，只保留内置检查和必要产物尺寸。
5. 每个功能/实验独立分支，平行工作隔离 worktree，`.local` 只软链主树，`x9/build/local.json` 需要才复制。别覆盖/提交/删除 `issues.jsonl`、`.qoder/`、别人的工作树、旧镜像、私有 manifest、备份或日志；完成才合并本地 main，清理只限本轮创建并确认已无独有工作。未经要求不 push。GitHub Actions 用 `gh`；本轮没有 CI。
6. **禁止未经本次明确授权的刷写、切槽、重启、重置、root/mailbox、live sysfs 或分区变更。** 无临时 `fastboot boot`。仅向 `boot_b` 写用户明确认可的镜像；不写 bootloader/GPT/recovery/modem/persist，不动 slot A，不 `set_active`。root/mailbox 另需私有 extent manifest、独立授权与 preflight。禁止整盘 root 读/备份；不能把 Android 的路径当成可写 HFS。
7. Android/APK 静态分析的既定偏好为 ASC/droidasc：`getmanifest → listclass → findrefs → getclass`；大输出用 `-o`。这轮手机无线问题不是 APK 解包任务，不必强用该工具。

## 3. 中断点和 Git 状态（以交接时检查为准）

| 位置 | 已验证状态 |
|---|---|
| 主 checkout | 当前分支 `feat/bt-settings-offline-candidate`。交接文档写入前源码 HEAD **`03e6f889`**，无未提交源码；已知未跟踪 `.qoder/`、`issues.jsonl` 不碰。交接提交仅改文档，后续以 `git log` 核对。 |
| `main` | **`2370e2fd`**，本轮后续代码尚未合入，不要切回 main 当作最新源码构建。之前 r2 修复已经在 main。 |
| 本轮已集成源码（当前功能分支） | `e7c401c8` 生成三种蓝牙诊断 boot 配置；`55a444ff` 通话 QMI txn → MD 请求关联与匹配 CNF 日志；`03e6f889` 单文件头标记/临时路径保护。 |
| `.worktrees/bt-settings-candidate-host` | 分支 `build/bt-settings-candidate-host` **`55a444ff`**，比功能分支缺 `03e6f889`；已软链 `.local`、复制本机 `x9/build/local.json`，`prepare-local.py --check` exit 0。尚未生成该 worktree 的诊断配置/新 header、未启动完整构建。 |
| Terra worktrees | `.worktrees/bt-diag-safe` `fix/bt-diag-safe` `0d917a9b`（代码已取入功能分支，NOTES 信息在此交接复述）；`.worktrees/call-md-correlation` `feat/call-md-correlation` `5ce4377d`（已 cherry-pick）；`.worktrees/sms-inbound` `fix/sms-inbound` `87ca29cb`（仅文档、不含行为修复；如要合入，按当前 NOTES 手工摘要避免章节冲突）。这些尚保留，别删其他旧 worktree。 |
| 设备与进程 | 最后经真实 ADB 验证：PLJ110 已从 `_b` 启动 Android，`sys.boot_completed=1`（NOTES §6.37）。暂停时没有新的 live 设备查询；因此只说“最后确认 Android”。主机未发现本任务的构建、USB logger 或刷机进程。新一轮未对刷机、重启、root/mailbox 或 push 执行任何操作。 |

**注意三个不同的“r2”**：`x9-native-event-wired-20261126-r6` 是曾运行的 iOS 镜像；`x9-radio-followup-20261126-r2` 是最新已完整构建的通话/短信候选；`.worktrees/bt-default-on/x9/out/x9-bt-settings-diag-20261125-r2` 是更早的 Bluetooth Settings 完整包输入，不是新候选，不要互相替代。

## 4. 真实设备历史与证据链

| 证据 | 已确认 | 不能据此推断 / 下一条证据 |
|---|---|---|
| `~/430.txt` | 老镜像 S3910 读到 `a5 10 1c 00`，识别 `S3910VA0A0-15.20`，触摸报告存在。 | 非 r6，不可替代最新候选行为。 |
| `~/432.txt`（2,280,528 B） | r6 第一轮已到 launchd、SpringBoard；S3910 两路电源读回已开，复位释放，但首个 TCM header `ff ff ff ff`，2.390 s identify 失败、驱动退出触摸报告循环。backboardd 能打开 HID 不代表有输入。 | 不能从一次失败确认电气、时序或其他初始化干扰的根因。 |
| `~/433.txt`（3,108,681 B） | 同一 r6 不重刷重启，2.166 s 首 header `a5 10 1c 00`，2.173 s 识别 S3910，2.216 s 启用上报，26–27 s 有实测触点按下/抬起并派发 HID；用户确认触摸 OK，但蓝牙仍灰、通话和短信仍不行。 | 非触摸启动故障永久修复。不要把 `433` 当作 r2/r3 候选采样。 |
| `433` 蓝牙 | MT6653 完成 firmware session，动态 section 2 加载后，10.582 s 收到真实 HCI 扩展能力；194.615 s `bluetoothd` PC/FAR=0 退出。stock 静态切片与 LR 支持 H4 可选 teardown 表 `+0x60` 空跳转，与 Vendor16 表不同；有两个静态 BL caller。 | 晚期崩溃不能解释更早灰钮；未运行 Preferences 七个 hook，未知哪一步未完成。未完整确认 teardown 回调生命周期。 |
| `433` 通话 | 82.118 s VOICE20 入桥，82.120 s 实际发出 `0901`（transport=0）。旧 MIPC 非通用 RX 日志。已有 Android 样本显示 `0902` 成功无 call ID，后续完整 `4901` 才带状态/ID。 | 不能以无字面 `0902` 认定没回；未见同笔接通、终态交付、双向声音。已有 Apple 静态证据对 typed provider/IMS 独立 owner 尚不闭合。 |
| `433` 短信 | SIM2 两次 `0605/0606` modem accepted；SIM1 迅速拒绝 `0x21014b`；两卡存储枚举 `0608` 返回 `0x210000`。静态 CMGS/CMGL fail helper 可把输入 OR 进 `0x210000`，CMGS 的 `0x14b` 可形成前者。 | 不能反推现场 AT 调用路径或原因；`0x210000` 不是空 inbox 证据。433 没有可关联的真实 `8603` 投递报告；同样没有接收持久化证明。 |
| 恢复 Android | 先 USB helper 进入 bootloader fastboot，用户随后指定 `/Users/tosasitill/boot.img`（67,108,864 B，Android bootimg），只写 `boot_b`。Sending/Writing/Rebooting OKAY；ADB 的 PLJ110、`_b`、`1` 独立确认。 | `a` 曾为未成功、重试 0，不能猜切 A。恢复镜像不是 repo 构建依赖。 |

原始日志只在本机，可能含设备、无线网络或通信元数据，不要把整份上传到新账号。必要时摘录脱敏片段。旧 `430`、`432`、`433` 的分段结论见 `NOTES.md` §6.13、§6.31–6.43。

## 5. 已确定的实现、验证和已排除方法

### 蓝牙

- 正常设备配置已显式有 `x9_bt=1 x9_bt_init=1 x9_bt_fc=1`；板级默认同样为 1，不能重复追加，也不能把省略当作禁用。 `ee1d1453` 修通用候选 rollback：把 init/fc 原位写成 0，诊断有值时也置 0，不扩 argv。旧 build 不会自动更新。
- `x9/bluetooth/X9SettingsBTDiag.c` 仅 Preferences、匹配 20H392 UUID/地址，转发原函数、保留返回结果，不写蓝牙可用状态。追 SessionAttach/session event 及五个 setup（本地设备默认/注册回调、服务回调、附件管理默认/回调）。 `x9/kernel/src/bsd/kern/x9_bt_settings_exec_env.inc` 默认关闭，仅以 root 上确切文件的 hash、size、owner/mode 通过才注入；已有对应环境变量时跳过，不覆盖。不能只打开 boot-arg 就说诊断生效；旧主树 generated header 不含该 dylib。
- 已尝试全局填 H4 空槽：实验编译/签名通过但有不止一个 caller，未确认完整语义，**撤回、不合并、不安装**。`x9/out/radio-bt-h4-rejected-20261126/` 是明确拒绝的实验，不是候选。勿伪造 BluetoothManager state 2，也勿把 debug 路径替代原生发现。
- `x9/bluetooth/settings_header.py` 先验证完整旧候选 root payload 与 Apple 原件，再仅嵌入 118,416 B 的 `/usr/lib/X9SettingsBTDiag.dylib`；不装 H4、launchd jobs/cache 或 Settings.plist，不接受未知 predecessor。早先 `x9/out/x9-bt-settings-scope-20261126/x9_bt_root.inc` **没有新 scoped marker，不能当最新输入**。
- `03e6f889` 的新生成器写 `X9_BT_ROOT_SETTINGS_ONLY`。对应 `x9/kernel/src/bsd/kern/x9_bluetooth_root.inc` 限定唯一诊断条目，安装和回滚预检临时名不存在，`O_EXCL` 创建且跳过通用末尾清理；遇预存的临时文件（包括 symlink/目录）失败而不是删。generic header 保持旧行为。新的 scope 不意味着 root 已授权、并发路径已形式化排除或设备可装。
- `x9/bluetooth/settings_boot.py` 已生成 `x9/out/x9-bt-settings-boot-config-20261126/` 下三份 JSON，各 601 B：verify 为 `x9_bt_root=0 x9_bt_settings_diag=1`（**只保证蓝牙 PID1 安装器不写，不是整个系统只读**），install mode 1 + diag 1，rollback mode 2 + diag 0 且 BT init/fc 0。标准 580 B + 两选择项 615 B，超过 607 B；只在需要时且查到板级默认确为 1 才移除 render 的 `x9pl=1 x9sl=1`（这两个通过 `sweet_x9_arg` 消费）。不要删 `x9lc` 或其它未经审查的 raw 参数。专用配置未加 r6 的 `x9_surf_events=1 x9ed=1`，不能把它当成 SharedEvent A/B。
- 交接前实际运行 `python3 -I x9/bluetooth/test_settings_{boot,temp,header,diag}.py` 各 exit 0，合计 10 项（2+1+2+5），temp 用本机 Xcode 15 SDK。是主机 fixture/source checks，未跑真正 PID1 install/restore。

### 通话

- `x9/kernel/src/iokit/Kernel/arm/X9Calls.inc`：VOICE09 启动、VOICE20 受真实 allocation/binding、实测 SIM2 profile、empty native baseline 约束；CallList46 另要 RAT/epoch/owned call state。没有允许凭一次 CNF 推断接通的逻辑。
- r2 已有 owned CNF parse、本地 60s deadline、MO join 以及终态队列入队诊断，并在通用 TLV 校验前保护活跃 VOICE20 的畸形重试（`ebae5a35`），不发起第二次拨号。暂停前的 `55a444ff` 额外在 `X9Calls.inc` 和 `x9/kernel/src/bsd/dev/x9_mipc_bridge.inc` 关联 iOS QMI txn、MD 请求和按 txid/id 匹配的 CNF，只记录事务元数据，不含号码/帧内容，不变状态机。
- Terra 已跑 `voice20_dispatch ios_timeout ios_deleted ios_join dial_tx_failure`，5/5 host exit 0；之前路径尝试误设 `/Applications/Xcode.app` 导致失败，实际 Xcode 15 在 `/Users/tosasitill/Downloads/Xcode.app/Contents/Developer`，优先用本机 `xcode-select -p` 和私有配置。音频/VoLTE 不可因为普通语音短测通过而结案。
- 相关合同：`docs/call-ios-bridge.md`（旧 Android `0901/0902/4901`），`x9/baseband/README.md`，`x9/kernel/src/bsd/dev/x9_call_{wire,state}.h`；原生 typed provider 的独立 Client/UUID/native epoch owner、SIP/DSP/VirtualAudio 仍缺。不要用截断的历史 MIPC 报文拼造证据。

### 短信

- `x9/kernel/src/iokit/Kernel/arm/X9Messages.inc`、`X9QMIAT.inc`、`x9/baseband/qcom/baseband/coachz_baseband.cpp` 和 `x9_sms_receipt.h`。r2 的 `349eb082` 绑定真实 WMS QMUX allocation generation，队列满时保留首个 RawSend20 终态，只重试入队，不重传 `0605`；晚到 `0606` 不覆盖超时；活跃同事务的畸形重试不会另发 BUSY/BAD。不是永久去重、网络/卡原因修复或投递证明。
- `8601` MT、`8603` 状态报告按真实 SIM/订阅/route 检查，`8603` 的 `8100` 要 TP-MTI=2/Class 4/status_reports。排队成功仅表示 host 接纳，WMS37 应与原 owner 匹配，再看物理 `8602/8604`，最后另证对端或 Messages 持久化。历史 `0607` 单 index 诊断不证明空收件箱；存储 tag→flag 维持 `{2,1,3,4}`，不要猜改错误和 ACK。
- 本轮 Terra 曾怀疑旧 route 跨 QMUX slot 复用串到新 client，经追真实顺序**撤回**：同一 worker 顺序跑 bridge/poll，`release_pending` 阻止重用，待 `DEL_CLIENT` 调 `x9m_del` 清旧订阅才释放。transport/adapter 各 16 slots，仅看到 full 分支不足以断言正常流丢 PDU。 `87ca29cb` 仅记录静态边界，无新测试/行为修改。
- r2 SMS host 回归 6/6，通话 5/5。已构建 `x9/out/x9-radio-followup-20261126-r2/boot.img` 15,806,464 B，kernel 33,127,840 B，源码 `ebae5a35`；104/104 overlay（3 prebuilt）。此镜像含 r2 的短信/通话可靠性修复及先前诊断，**不含暂停前的 `55a444ff`、新蓝牙 scoped header 或 scoped root 路径**。从未刷入。

## 6. 目录地图和恢复命令

入口：`AGENTS.md`（设备与仓库约束）、`NOTES.md`（当前候选/日志/交接），`x9/build/`（`build.py`、`prepare-local.py`、`boot-device.json`、私有 `local.json`），`x9/xnu/`（XNU 本体 patch/构建），`x9/kernel/src/` + `recipe.json`/ `link-order.txt`（overlay），`x9/bluetooth/`、`x9/baseband/`（合同和测试），`x9/tools/usb-live/`、`x9/tools/ios-reboot-bootloader-usb`，`x9/out/`（gitignored 新候选/日志）。原件和固定私有输入参见 NOTES §2.3，不把输入/manifest/原件转发给外部。

收到用户恢复指令后，**第一件事是核对工作树和进程，不是刷机**：

```bash
cd /Users/tosasitill/ios_plj110
git status --short --branch
git log -8 --oneline
git worktree list
sed -n '1,120p' AGENTS.md
tail -85 NOTES.md
```

下一步建议在已准备的 build worktree 继续。先看它没有别的窗口在工作、目标提交仍存在且真能 fast-forward；`03e6f889` 是当前完整源码截点，不是最终主机 build 提交：

```bash
cd /Users/tosasitill/ios_plj110/.worktrees/bt-settings-candidate-host
git status --short --branch
git merge --ff-only 03e6f889
python3 -I x9/build/prepare-local.py --check
export DEVELOPER_DIR="$(xcode-select -p)"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export PATH=/opt/homebrew/opt/llvm@22/bin:$PATH
for unit in boot temp header diag; do
  python3 -I "x9/bluetooth/test_settings_${unit}.py" || exit 1
done
```

以下是**计划但未执行的离线构建命令**。用未存在的目录名；任一步失败就停止，不宣称候选完成。只应把新 scoped header 写入专用 build worktree 的 generated 路径，别覆盖主树旧 header。旧候选的 payload 和 Apple 原件用于生成输入，不代表实际安装中的 predecessor：

```bash
# 仍在 .worktrees/bt-settings-candidate-host；仅主机操作。
set -e
root=/Users/tosasitill/ios_plj110
inputs="$root/.worktrees/bt-default-on/x9/out"
plan=x9/out/x9-bt-settings-radio-plan-20261126
test ! -e "$plan"
test ! -e x9/kernel/generated/x9_bt_root.inc
python3 -I x9/bluetooth/settings_boot.py --out "$plan"
python3 -I x9/bluetooth/settings_header.py \
  --payload "$inputs/x9-bt-settings-diag-20261125-r2/root-payload" \
  --apple-originals "$inputs/bt-settings-inputs-20261125/apple-originals" \
  --out x9/kernel/generated/x9_bt_root.inc
python3 -I x9/build/build.py all --boot-config "$plan/boot-verify.json" \
  --name x9-bt-settings-radio-verify-20261126
```

完整 verify 构建成功后，只按项目内置检查、build.log、退出码和尺寸记录；如后续确实需要另外的 install/rollback 镜像，经源码和 root 风险审计后，可用同一已编译 kernel 通过 `build.py boot --kernel … --boot-config "$plan/boot-install.json"` 或 rollback JSON 配不同新名离线打包。**生成镜像不是获得安装授权；不能刷并启动 install 模式来代替 preflight。** 当前蓝牙一文件路径还需针对真实 extent/root 的可审计 preflight；`stage_targeted.py` 已知是旧七文件流程，不是单文件部署器。

已验证的日常设备流程（只有收到对应本次明确授权才执行）：

- iOS → bootloader：`python3 -u x9/tools/ios-reboot-bootloader-usb`。必须独立见唯一 fastboot 设备、`fastboot getvar is-userspace` 为 no、`getvar current-slot` 为 b；USB ACK 不等于已进 fastboot。不要失败自动重试。 Android 当前如何进 bootloader 应按用户确认，不把 iOS USB helper 用于 Android。
- 启动 iOS 前开唯一只读 USB logger：`python3 x9/tools/usb-live/usb_log.py --output "$HOME/434.txt" --quiet`（**仅在 434 不存在时**，否则用新编号）。等待 USB 1209:0001；不要用会丢后台进程的临时 nohup 会话。日志先可能只见 shim，原 432/433 会隔一段才进入 XNU/用户态，不要因短时没日志断言没启动。
- 返回 Android：参见 AGENTS/NOTES §3.5。唯一 bootloader fastboot 且 b、`partition-size:boot_b=4000000`；恢复镜像 `/Users/tosasitill/boot.img`。本次明确授权涵盖刷写和重启时，`fastboot -s "$id" flash boot_b /Users/tosasitill/boot.img && fastboot -s "$id" reboot`。随后仅查唯一 adb 的 model/slot/`sys.boot_completed`。不要将历史授权用于未来一轮。不要把序列号写到聊天或 git。

## 7. 未完任务与执行顺序

1. **P0：恢复后完成并记录主机候选。** 如 §6 将专用 build worktree 快进至包含 scoped marker 的源码、重生成 header、验证三配置、先完整构建 verify。不要误用旧无 marker header 或旧蓝牙 r2；不要刷新主树 generated。测试/生成器有结论，完整镜像还没有。随后再整理/合入源码分支，不丢尚未合入的 SMS 文档和测试日志。
2. **P0：分开准备设备验收授权。** 先问用户是否愿意换下 Android 运行新 iOS，明确哪一个镜像、写 `boot_b`、是否立即 reboot、USB 采样；蓝牙诊断如尚未在 root 上安装，另问单文件安装授权并完成私有 manifest/preflight。不要使用旧七文件整体更换以获取诊断。无新捕获不必反复从 433 推断未知结果。
3. **P1 蓝牙：** 收集 hash gate 与七个 setup 实际返回，按 SessionAttach → session callback → 五 setup → native availability → inquiry/LE scan 的因果顺序找第一失败分支；另定位谁触发约 194 s 的 teardown。若诊断因现有环境或文件未验证而跳过，先修可观察性，勿强推 state 成功。然后做原生发现/配对/连接，音频另验。
4. **P1 通话：** 把同笔真实拨号的 VOICE20 本地事务、物理 0901、按 txid/id 匹配的 0902、MO 4901/epoch、用户态终态入队和可见结果串起来。只测用户指定非紧急号码/对端，用户确认测试动作；不自动拨号。真实收据再选择修 descriptor、owner join/typed provider 或 transport。接通后再独立测双向声音。
5. **P1 短信：** 用户实际发/收后按 SIM 分别对比 0605/0606、RawSend host queue receipt、对端结果；若有状态报告，跟同笔 8603、WMS 事件、WMS37、物理 ACK、Messages 持久化。SIM1 拒绝和双卡存储失败要用真实上下文继续定位。不要测试未经用户确认的实际收件人或反复重发。
6. **P2：** 保留两份触摸启动证据，若再次 `ff`，先对照真实首个 SPI/TCM 响应与先前 Android→fastboot/暖重启路径；勿因一轮恢复而声称修好。SharedEvent 不是本轮主目标；433 仍有 AegirPoster SIGABRT，别把任何“诊断参数生效”解释成止崩。

**保密交接：** 本文不包含密码、API Key、访问 Token、设备序列号、号码、短信正文、私有 extent 数据或原始日志。换 AI 账号若仍访问同一 Mac，仅需要本文与同一 checkout，不需要传凭据。若换主机，另由用户决定怎样提供私有输入或脱敏日志；Git push 不在本次授权内。
