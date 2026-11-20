# ios-sweet / X9 工作笔记（2026-11-12 整理重写）

> 2026-11-12 之前的完整流水账（4188 行：GPU/蜂窝/UFS/相机/麦克风/GPS/USB-NATIVE1–17 等全部交接）在
> `git show c52d6f3:NOTES.md`；更早的在 `a8f1955`、`636edc2`。查细节：`git log -S'<关键字>' -- NOTES.md` 再 `git show`。
> 本文件只保留：目标与硬件、仓库/输入布局、**完整构建步骤**、工作流规则、当前状态与下一步。
> 约定（AGENTS.md）：每个里程碑更新「当前交接」；区分**观察**与**推断**；序列号/设备转储/Apple 产物不进 git。

## 1. 目标与硬件

- 在 **OPPO Find X9（PLJ110 / OP5E17L1，MT6993，12 GiB RAM，面板 1256x2760，Mali v14）** 上由 MediaTek LK 直接启动自编译
  XNU（xnu-8796.141.3 + SWEET/X9 补丁），运行**未修改的 iOS 16.7.16（20H392，iPhone X / D22 身份）**用户态。
- 启动链：LK → Android boot.img v4（legacy LZ4 arm64 Image）→ x9 裸机 shim → 内嵌 SWEETXNU payload → XNU。
  每次由**用户手动** `fastboot boot x9/out/<name>/boot.img`；不 flash、不 `fastboot getvar`、不写分区。
- 根盘：`userdata` 内 pin+immutable 的 extent 文件（HFSX，base 16 GiB + extension 16 GiB = 32 GiB），内核原生
  `ufsroot0` 挂载（`rd=ufsroot0 rootrw=1 ufsrw=1`）。extent 表/序列号在 `root-manifest.json`（只在 `.local`）。
- 显示：ADT canvas 1242x2760（414x920 pt @3x，左右各 7 px 黑边）；物理面板 1256x2760 stride 5120，fb 0xfc910000。
- 用户目标（2026-11-12）：**Mac USB 配对、AppSync 安装 IPA、完整越狱（Procursus rootless /var/jb，SSH 只走 ue0）**；
  方法上**以 UniXNU 为主要参考，不闭门造车**。

- 用户新增目标（2026-11-16）：**原生 Apple Wallet 新开交通卡并锁屏刷卡使用**；城市/卡种及资金渠道尚未指定，当前只完成修复规划，未开始开卡或资金/SE 操作。

## 2. 机器与目录

- **Mac**（构建/反汇编/大工具全部在这）：`ssh -p 2222 tosasitill@localhost`（反向隧道，偶尔断，重试）。仓库 `~/ios_plj110`
  是**权威仓库**；服务器 `/root/ios_plj110` 只读文件/编辑/git。非登录 shell 没有 PATH：用
  `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3`、`/opt/homebrew/opt/llvm@22/bin`。
- **设备日志**：用户放在 Mac `~/NNN.txt`（最新 `~/243.txt`）。抓日志：`python3 x9/tools/usb-live/usb_log.py --output ~/N.txt`；
  iOS logd：`x9/tools/pull_ios_logs.py` + `x9/tools/tracev3_strings.py`（Android 下读 root||extension）。
- **构建只依赖本仓库 + 编译工具**。仓库外允许的只有：Xcode 15（`developer_dir`，Apple clang 15 / MacOSX14 SDK）、
  `/Library/Developer/KDKs/KDK_13.5_22G74.kdk`、Homebrew（LLVM/lld 22、ccache、python3.13 + lz4）。不再读 `~/HelioDarwin`。

### 2.1 仓库（git 跟踪）

| 路径 | 内容 |
| --- | --- |
| `x9/xnu/` | **XNU 源码全部改动**：`0001-x9-sweet-xnu-8796.141.3.patch`（相对 apple-oss `xnu-8796.141.3` 的完整 delta，329 文件）、`0002-x9-ios-kext-cxx-abi.patch`（iOS kext C++ ABI）、`dxb-build.sh.patch`（blacktop/darwin-xnu-build 663906528d 的改动）、`build-xnu.sh` |
| `x9/kernel/` | 内核 overlay：`src/`（重编对象源码）、`recipe.json`、`link-order.txt`、`generated/`、`prebuilt/` |
| `x9/shim/`、`x9/tools/` | 裸机 shim、ADT/payload/boot.img 打包（`boot-xnu.py`）、各类工具 |
| `x9/build/` | `build.py`（总入口）、`config.example.json`、`boot.json`（基础参数）、**`boot-device.json`（当前完整设备参数）**、`prepare-local.py`、`scripts/`（Metal/PanVK 插件构建、`build-usb-prelink.py`） |
| `x9/metal/`、`x9/mailbox/` | Metal 前端 + PanVK 桥；插件 mailbox 更新器 |
| `x9/usb/`、`x9/appsync/` | USB 主机工具/分析（`install.py`、`native_bridge.py`）、AppSync runtime 构建器 |
| `docs/` | 分系统评估文档 |

### 2.2 `.local/`（gitignored，长期输入；**只放下表这些，不再新建实验目录**）

| 路径 | 内容 | 来源 |
| --- | --- | --- |
| `.local/dxb/` | XNU 构建树（macOS kext ABI）：`xnu/`（= tag + 0001）、`build/xnu.obj/DEVELOPMENT_ARM64_SWEET`、`fakeroot/` | `x9/xnu/build-xnu.sh macos [--setup]` |
| `.local/dxb-iosabi/` | 同上但 iOS kext ABI（+0002）和 sparse prelink 内存保护（+0003），USB 原件 prelink 用 | `x9/xnu/build-xnu.sh ios [--setup]` |
| `.local/apple/ios16-20H392/` | 20H392 IPSW 内容（OS/SystemOS/AppOS/RestoreRamDisk dmg） | Apple，不入 git |
| `.local/apple/dsc-20H392/` | 设备实跑 dyld shared cache 拷贝（反汇编用） | 设备 |
| `.local/references/` | 参考仓库：`UniXNU`（sheng e893717）、`XnuPrelink`（440589b）、`hackios-xnu-kernel`、`hakios-mt6768`、`hakios-metal`、`ShengUfs`、`QcomBaseband`、`linux-mtu3`、`oppo-mt6993-usb`、`mesa-panfrost`、`data-a-live` | git clone |
| `.local/references/oppo-kernel-mt6993/` | OPPO MT6993 GKI 内核源码（4bf60bcea，2025-12-18，CPH2791 16.0.1.302，1.9G） | `git clone --depth 1 https://github.com/oppo-source/android_kernel_oppo_mt6993` |
| `.local/references/oppo-modules-dt-mt6993/` | OPPO MT6993 厂商模块 + 设备树（4d505a42，同上，1.2G）；**MTK 驱动在 `kernel/kernel_device_modules-6.12/drivers/misc/mediatek/`（ADSP：`.../adsp`）** | `git clone --depth 1 https://github.com/oppo-source/android_kernel_modules_and_devicetree_oppo_mt6993` |
| `.local/vendor/oppo-camera-imgsensor-common/` | OPPO 相机 imgsensor 源码（`camera_vendor_root`） | OPPO 开源 |
| `.local/x9/wifi-fw/`、`wifi-nvram/WIFI` | MT6653 Wi-Fi/BT 固件、设备 NVRAM | Android 分区 |
| `.local/x9/bluetooth/android-mt6653/` | 当前 Android 原始 `libbluetooth_mtk.so`、完整6448 B `btinit.bin`、`properties.txt`、优先 `BT_FW.cfg`；仅原件，派生命令不作输入 | 2026-11-15只读HAL对象及配置补采；`bluetooth_vendor.py`版本/分支/配置严格校验 |
| `.local/x9/modem/` | `mdota/`（MCF 配置）、`parts/`（**modem/nvdata/nvcfg/protect 分区备份**）、`live.dts/dtb`、`vendor-bin/` | Android |
| `.local/x9/baseband/` | `Mav17-9.00.02.Release.bbfw`（原 ABM 资产）、BuildManifest、生成的 pack | Apple |
| `.local/x9/carrier/CBN_cn_iPhone.ipcc` | 中国广电运营商包（sha1 8eb8ada7…） | Apple |
| `.local/x9/root-extension/20261110-16g/root-manifest.json` | 根盘 extent 表（含序列号，**绝不提交**） | mailbox/provisioning |
| `.local/x9/root/root-20261109.clone.hfs` | 根盘工作克隆（16 GiB 稀疏，Mac 挂载于 `/private/tmp/x9root`） | 设备拉取 |
| `.local/x9/backups/partitions/` | **分区/固件备份**：`x9_dump/`（lk/boot/init_boot/vendor_boot/vbmeta/dtbo）、`legacy-output/`、`firmware-originals/`（lk_a/b、tee/atf、bl2_ext、aee、scp_b、vcp_b、vendor_boot_b、connsys_gnss、gz） | 设备，**不得删除** |
| `.local/x9/usb/native-20H392/` | 20H392 kernelcache（USB kext 原件来源）+ BuildManifest | Apple |
| `.local/x9/appsync/runtime-20261112-v3/`、`appsync-signer/` | 已构建 AppSync runtime（4 个签名二进制）与 signer 缓存 | `x9/appsync/build.py` |
| `.local/x9/MaliMetal/` | 插件构建的 X9/General 链接点（`prepare-local.py` 维护） | — |
| `.local/x9/work/x9-metal-render/` | Mesa/metal2vulkan/libdrm/Rust venv 构建树（Meson 缓存不能搬家，路径固定） | `x9/build/scripts/` |

`python3 x9/build/prepare-local.py --check` 校验以上布局；不带 `--check` 时重建插件脚本软链。

## 3. 完整构建步骤（Mac，仓库根）

### 3.0 一次性准备

```bash
cp x9/build/config.example.json x9/build/local.json      # 全部是仓库相对路径；只需改 developer_dir 等工具路径
python3 x9/build/prepare-local.py --check
```
`local.json` 键：`developer_dir`、`llvm_clang`、`lld`、`homebrew`、`compiler_launcher`（ccache）、`dxb`、`root_manifest`、
`baseband_firmware(_storage)`、`camera_vendor_root`、`wifi_firmware`、`bluetooth_firmware`、`wifi_nvram`、`mdota_source`、
`carrier_cbn_ipcc/_sha1`、`appsync_runtime`。相对路径按仓库根解析（`build.py load_config`）。

### 3.1 XNU 对象树（只在改 XNU 本体时需要）

```bash
x9/xnu/build-xnu.sh macos          # .local/dxb：校验 xnu/ == xnu-8796.141.3 + 0001，然后 make install DEVELOPMENT ARM64 SWEET
x9/xnu/build-xnu.sh ios            # .local/dxb-iosabi：同上 + 0002 + 0003 + 0004/0005（诊断）
x9/xnu/build-xnu.sh macos --setup  # 从零：clone blacktop/darwin-xnu-build@663906528d + dxb-build.sh.patch、apple-oss xnu tag + 补丁，
                                   #       harness 构建 bootstrap_cmds/dtrace/libfirehose/libsyscall/headers（网络）
x9/xnu/build-xnu.sh ios --setup    # 已有 .local/dxb 时 APFS clone 它再加 0002（不占空间）
```
- 参数：Xcode clang 15、`KDK_13.5_22G74`、`RC_DARWIN_KERNEL_VERSION=22.6.0`、banner `Mon Aug 14 05:06:07 UTC 2023` / `xnu-8796.141.3`。
  make 总会因 lldbmacros dSYM 安装返回非零；脚本以「本次重新链接了 kernel 且日志无 `No rule`/`error:`」判成功。
  **搬动过的树**：`.d`/Makefile 里是旧绝对路径（会报 `No rule to make target <旧路径>`），先对 `build/`、`fakeroot/` 的文本文件
  `perl -pi -e "s#<旧>#<新>#g"`（2026-11-12 dxb-iosabi 从 `.local/x9/work/` 搬出后做过）。
- **改 XNU 源码的流程**：在 `.local/dxb/xnu` 改 → `git -C .local/dxb/xnu` 生成新 0001（命令见下）→ 提交到功能分支。脚本拒绝
  源码与补丁不一致的树，保证仓库补丁就是唯一真相：
  ```bash
  cd .local/dxb/xnu && GIT_INDEX_FILE=/tmp/x9idx git read-tree HEAD && GIT_INDEX_FILE=/tmp/x9idx git add -A . && \
  GIT_INDEX_FILE=/tmp/x9idx git diff --cached --binary --full-index HEAD -- . ':(exclude)libkern/libkern/c++/OSMetaClass.h' \
    > ../../../x9/xnu/0001-x9-sweet-xnu-8796.141.3.patch
  ```
- **已验证（2026-11-12）**：两棵树的 `xnu/` 都与「tag + 补丁」逐文件一致；`dxb-iosabi` 的 make install 通过（只有 dSYM 失败）。
  `--setup` 从零路径按原始 harness 参数写成，**尚未从零完整重跑过**。
- 注意：`.local/dxb/xnu/libkern/crypto/sweet_aes.c` 曾有一处未编译过的 rev252cn ECB 解密改动（xnu make 下编不过；链接用的一直是
  10-26 的旧对象），整理时已与补丁对齐丢弃。Wi-Fi GTK 解包在 X9WifiNet.inc 里用 ccaes_cbc 单块零 IV 实现，不依赖它。

### 3.2 内核 + boot.img（日常）

```bash
python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --name <唯一名>      # 内核 + boot.img
python3 -I x9/build/build.py kernel --name <名>                                               # 只编/链内核
python3 -I x9/build/build.py boot --kernel <kernel> --boot-config x9/build/boot-device.json --name <名>
python3 -I x9/build/build.py all --revision <N> ...                                           # 先构建插件 N 再整包
python3 -I x9/build/build.py --dry-run                                                        # 打印全部命令
```
- 产物 `x9/out/<name>/`：`kernel.development.sweet`、`boot.img`、`boot.json`、`build.log`、`shim/` 等。同名会覆盖，每次用唯一名。
- `boot.json` 只有基础参数；**设备当前完整参数在 `boot-device.json`**（43 个 token，含 Wi-Fi XO 校准、GPU/UFS/蜂窝开关），
  其它实验参数用 `--boot-arg` 追加。boot-args 有约 607 B 上限。
- overlay 加对象：源码放 `x9/kernel/src/`，在 `recipe.json` 加条目（模板），在 `link-order.txt` 放位置。
- **已验证（2026-11-12）**：整理后的 main（`.local` 新布局，全部相对路径）`build.py all --boot-config x9/build/boot-device.json
  --name x9-main-cleanup-20261112` exit 0；build.log 里仓库外路径只有 Xcode。未上机。

### 3.3 Metal/PanVK 插件与 mailbox

- 插件不在 boot 分区。内核只认 `x9/kernel/generated/x9_render_external_manifest.h` 的 path/size/SHA-256（当前 **280**）；
  重建插件非确定，allowlist 头随 revision 一起提交。构建：`build.py all --revision <N>` 或 `build.py plugins --revision <N>`
  （脚本在 `x9/build/scripts/`，第三方树 `.local/x9/work/x9-metal-render`，补丁见 `x9/PANVK-PATCHES.md`）。
- **只有出现新 plugin-package 时**才需要 stage（Android 开机、用户给序列号）：
  `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/<name>`，成功后用户再 `fastboot boot`。纯内核重编直接说“无需 mailbox”。
- 最新已确认的运行日志 **320/321 使用 GPU280**；main 的 allowlist 仍为 **280**，2026-11-15 的 GPU 优化集成未更换插件。
  若设备仍保持该插件状态，纯内核镜像无需 mailbox；若插件确实不匹配，280 包为 `x9/out/x9-gpu280-purge-20261112`，先确认设备状态再 stage。

### 3.4 USB 原件 prelink（进行中）

```bash
python3 -I x9/build/scripts/build-usb-prelink.py <name>
```
用 `.local/dxb-iosabi` 编内核 → `.local/references/XnuPrelink/emit_prelink.py` 把 20H392 的 AppleUSBCommon / IOUSBDeviceFamily /
AppleUSBDeviceMux 链进内核空的 `__PRELINK_*` 段（slide `0xfffffe000a400000-0xfffffff004dc8000`）→ `build.py boot`。
产物 `x9/out/<name>-kernel/`、`<name>-prelink/`、`<name>/`。XnuPrelink 把 `__PRELINK_INFO` 按 4K 放在 `__LINKEDIT` 之后，脚本会把它挪到下一个空闲 16K 位置（shim 校验和 XNU 16K 页的 `arm_vm_init` 都要求 16K 对齐；其 XML 无自引用）。

### 3.5 上机

用户 `fastboot boot x9/out/<name>/boot.img`，跑场景，日志放 Mac `~/N.txt`。需要 Android 侧操作（mailbox、logd、adb）时先确认。

## 4. 工作流规则（2026-11-12 起）

- **一个功能 = 一个 git 分支**：`feat/<主题>`（修复 `fix/<主题>`）。需要并行时用 git worktree，放在仓库内被忽略的 `.worktrees/`：
  ```bash
  git worktree add .worktrees/<主题> -b feat/<主题>
  ln -s ../../.local .worktrees/<主题>/.local          # 共用同一份长期输入
  cp x9/build/local.json .worktrees/<主题>/x9/build/
  ```
  完成后合回 main（`git merge --ff-only` 或 rebase 后合），`git worktree remove`，`git branch -d`。
- **不在 `.local` 下新建实验/分析目录**。临时抓取、编译日志、分析产物放在功能树自己的 `x9/out/<name>/`（gitignored），随树删除；
  有长期价值的脚本/结论提交进分支（`x9/<子系统>/`、`docs/`）。新增长期输入必须同时更新 §2.2 表和 `prepare-local.py`。
- 构建只用仓库内路径 + 编译工具；新增外部依赖先复制进 `.local` 对应位置并写进 §2.2。
- 交接写在本文件 §5，标明提交、run ID、确切构建/启动结果和观察 vs 推断。push 到 `origin`（GitHub tosasitill/ios_plj110）。

## 5. 当前状态与交接

#### 2026-11-20 日志 363 TBUF 专项分析：MMUP 早夭于 DRAM MPU 配置前 + RV33 自定义指令纠正「TBUF 垃圾」 framing

- **目的**：NOTES 此前覆盖 356–360 与 365/366，但 **361–363 的 TBUF PC 轨迹未专项分析**。本轮离线分析 363（`~/363.txt:6697-6718`，含 SNAP/DIAG4/5/INTC1/TBUF/HDR/PROG/STK）+ 保存的 `x9/out/mmup.dis`（tinysys-mmup-RV33_A），细化 MMUP 首错位置并验证 path-b 几何。无代码改动、无镜像、无设备操作。
- **SNAP 时间线（实测 363）**：`pre/rst: st=4 pc=0 lr=0 sp=0 rstn=ff00`（复位中）→ `1ms: st=4 pc=3bc lr=3a4 sp=0 rstn=ff01`（已释放、执行早期代码）→ `end: st=4 pc=0 lr=14e4 sp=fe19ccc4 rstn=ff01`（落入异常环）。`DIAG2: c1_latch=0 lr=14e4 sp=fe19cb38 sram=1a0b5/1a0b5 exc=0/0/0`（两核 SRAM 已由 LK/TF-A 加载）。
- **1ms PC=0x3bc 是字拷贝循环（实测 mmup.dis:236-241）**：`0x3a8 add t5,t3,t2; lw t4,0(t5); add t5,t0,t2; sw t4,0(t5); addi t2,t2,4; 0x3bc bltu t2,t1,0x3a8`——src=t3+t2、dst=t0+t2 的 memcpy 循环（用途推断：早期 .data/向量重定位）。**若 dst 是需要 SID 0xa0 翻译的 DRAM 而翻译缺失，sw 即 fault**——与 path-b「MMUP_RELEASE 前装翻译」序列吻合。
- **TBUF 末段有效跳转 0x19c→0x4a14（实测 mmup.dis:105/5703-5707）**：`0x19c: jal 0x4a14`（紧接 `0x198 csrs mstatus,t0(0x800)` 之后）；`0x4a14-0x4a20` 是 **4 条 RV33 自定义指令** `0800507b/080050fb/0800517b/080051fb` + `0x4a24 ret`，标准 riscv objdump 标 `<unknown>`（opcode 0x7b=custom-3）。其后 `0x4a2c` 起是 MPU region 写入函数（`sw t1,0(t0)/sw t2,4(t0)…` 成对 base/attr，间插自定义 cache op）。**纠正先前 framing**：TBUF 尾部 32-bit「垃圾」值（`87edc837…`）**非已证损坏**——部分是轨迹进入 RV33 自定义指令/数据区，标准 objdump 不可解码；不应当作纯 corruption 证据。
- **MPU/GPR 状态（实测 363 DIAG4/5 + PROG）**：`GPR1=0x58000001=PACK(0x158000000)`、`GPR2=0x20e000`(total)、`GPR3=0x5808d001=PACK(0x15808d000)`(logger offset 0x8d000)——**GPR 打包与 IOVA 窗正确**（pa=0x158000000 ∈ [0x140000000,0x1c0000000)，与锁定 path-b identity 几何一致）。`PROG: nest=3af0d/3af0c mpu10=0..0 mpu11=0..0`——**即使 pa 在窗内，DRAM MPU region 10/11 仍全 0**：MMUP **未完成** remap 函数 `0x15f64`（mmup.dis:27626，配置 DRAM MPU region 10–13）→ 在到达/完成 DRAM MPU 配置前已 fault。
- **因果链强化（推断，仍待 boot 实证）**：MMUP 从 SRAM 启动并执行早期 memcpy（0x3a8），但在完成 DRAM MPU 配置（0x15f64）前 fault → 递归异常（lr=14e4=0x14e0 jal 0x1511a）→ sp 下穿 → 死循环 → 不服务 backup_vote_int → MM_2 BACKUP1 DONE 永不置位（见上一条因果链条目）。mpu10=0 + 早夭与「DRAM 访问缺 SID 0xa0 翻译」（分歧 #2）一致，**强化 path-b 直击第一阻塞**。
- **诚实边界/未决**：**首错指令未精确定位**——TBUF 在 0x19c→0x4a14 后进入 RV33 自定义指令区，离线无 RV33 ISA 映射不可解码；memcpy 的 dst(t0) 是否即未翻译 DRAM 需 boot 期单步/MPU dump 实证。不把「TBUF 尾部垃圾」或「mpu10=0」单独当作 SMMU fault 的直证。path-b 几何（IOVA/PA 0x15FC00000、0x400000、MMUP_RELEASE 前）仍由 boot DIAG（STE a0 + core1 状态）最终门控。commit 见下。

#### 2026-11-20 MM_2 errno=60 根因因果链源码实证：BACKUP1→VIP(MMUP) 备份投票未 ACK（验证 path-b 直击第一阻塞）

- **目的**：在 boot 门控期间，用源码把「分歧 #2（SMMU 翻译缺口）」与「第一实机阻塞 MM_2 stage4 errno=60」之间的**因果链**实证出来，验证 path-b 确实直击第一阻塞（而非旁支），并定死 boot 日志中确认/推翻该链的具体行。此前审计覆盖启动顺序/重复投票语义，但**未**对 sta/done/fsm/int_raw 做 BACKUP1→VIP 逐位解码。
- **寄存器身份（实测 `hwccf.h`）**：X9 `MM_POWER_STA 0x1135c`=`CCF_BACKUP1_STA`(:120)、`MM_POWER_DONE 0x1136c`=`CCF_BACKUP1_DONE`(:121)、`MM_POWER_GLOBAL 0x11358`=`CCF_BACKUP1_EN`(:119)——三者属 **「Backup VIP Status」** 块(:115)，即**转发给 VIP（远端 tinysys=MMUP）的备份投票器**，非主 MTCMOS 路径。`MM_POWER_FSM 0x10000`=`CCF_STATUS`(:64) bit0=`hwccf_idle`(:66)；`MM_POWER_ALL_STA 0x13704`=`CCF_ALL_MTCMOS_STA_0`(:108)；mt0sta `0x1131c`=`CCF_MTCMOS_STA_0`、mt1sta `0x11334`=`CCF_MTCMOS_STA_1`。`MM_POWER_INT_BACKUP1_TIMEOUT=(1<<5)`=`backup1_timeout_int_to_vip`(:150) ✓。
- **VIP 中断/ACK 机制（实测 `hwccf.h`）**：`CCF_INT_STATUS 0x50` bit14=`backup_vote_int_to_vip`(:159)、bit30=`backup_vote_int_to_ap`(:176)；`CCF_OUTPUT_STATUS 0x10004` bit28-31=`hw_ccf_to_up_int_b_0..3`「interrupt event to vip」(:76-80)；`CCF_BACKUP1_VIP_STA_CLR 0x10`(:129) 由 **VIP w1c** 应答。即 BACKUP1 投票经中断送 VIP，**VIP 固件必须处理并 ACK**（DONE 置位 / VIP_STA_CLR）。
- **Android 等待语义（实测 `hwccf_api.c:993-1067 _v1_hwccf_irq_voter_wait_done`）**：is_set 先轮询 `all_en_ofs`(=BACKUP1_EN/global) 位 SET(:1008-1021)，再轮询 `sta_ofs` 直到 `IS_MASK_CLR(val, done_ack_msk)`（**STA 位清=idle**，:1044-1053）。**与 X9 逐项一致**：stage3 `mm_irq_wait_global` 等 GLOBAL bit3 SET、stage4 `mm_irq_poll(STA,target=0)` 等 STA bit3==0。X9 `mm_irq_poll` 第三参是 `step_us`（非 mode），判据 `(reg&MM_POWER_BIT)==target`（:531）。
- **356 状态逐位解码（实测日志 + 上述源码）**：`global=8`→BACKUP1_EN bit3 SET（**stage3 通过**）；`sta=8`→BACKUP1_STA bit3 **卡 1 不清**（**stage4 超时点**，timeout_off=1135c/raw=8）；`done=fffffff7`→BACKUP1_DONE **bit3=0**（VIP 从未 ACK 该备份投票）；`fsm=80000001`→CCF_STATUS **bit0=1=hwccf_idle**（本地 HWCCF FSM 空闲，卡在等远端 VIP；bit31 语义未在枚举定义，不臆断）；`int_raw=0x4000`→**bit14=backup_vote_int_to_vip**（HWCCF 已向 VIP 发出备份投票中断，NOTES:319 曾观察到此位）。
- **因果链（推断，待 boot 实证）**：MM_2 备份投票已送 VIP（bit14），但 VIP/MMUP **从未 ACK**（DONE bit3=0、STA 卡 busy）。最可能根因：**MMUP（core1 RV33）固件未运行**——其从 VCP carve-out 取指经 MM SMMU SID 0xa0 翻译，而 X9 打包裸 PA 却无翻译（分歧 #2，已源码实证）→ MMUP 取指 fault/跑飞 → 不服务 backup_vote_int → BACKUP1 DONE 永不置位 → stage4 errno=60。**此链把第一阻塞直接归因于分歧 #2，验证 path-b 是正确修复方向。**
- **boot 确认/推翻该链的具体行（dual 候选 `103e465f…` 会打印）**：① `X9 MM POWER VCP STATE: core0=… core1=… mmup_gpr=…`（`cfg_core1+0x70`=MMUP 状态——若 core1 halted/not-ready 而 core0 ready，支持「MMUP 未运行」）；② `X9 MM VCP STE a0:`（若 SID 0xa0 STE=ABORT/zero-invalid，支持「无翻译」）；③ `X9 MM VCP BOOT:` errno（VCP boot 本身成败）。**若 core1 ready 且 STE a0 已有有效 S1 翻译，则本因果链被推翻**，MM_2 超时另有根因（如 VIP 中断路由/GIC），需重定向——届时不坚持 path-b。
- **诚实边界**：BACKUP1→VIP 机制、寄存器身份、Android 等待语义、356 位解码均**源码/日志实证**；「MMUP 因 SMMU 缺口未运行」是**推断**（MMUP 取指是否确经 SID 0xa0 需 boot core1 状态 + STE a0 实证）。本轮无代码改动、无镜像、无设备操作；commit 见下。

#### 2026-11-20 SMMU 快照双失败路径触发修复 + 新候选 x9-camera-smmu-dual-20261120（取代 15634256，待上机）

- **目的**：修复上一版 DIAG 候选（`15634256…`）的**诊断触发缺口**——SMMU 只读快照原仅在 **VCP-boot 失败**路径（`vcp_boot_diag_log`）输出，但 **356 基线实际失败在 MM_2 投票路径**（`mm_vote_take` stage4 `ETIMEDOUT`），VCP boot 当时已成功。若用户 boot 旧候选而失败仍落在投票路径，将**拿不到任何 SMMU 输出**，task #7 几何门控空转。
- **356 是旧构建（实测 `.local/x9/logs/356.txt`）**：全文**无 `X9 MM VCP BOOT:` 行**（该打印由 `vcp_boot_ensure` 引入，356 早于此基础设施），仅 `:6086` 一行 `X9 MM POWER GET: client=1 backend=hwccf MM_2 bit=3 stage=4 owned=1 held=0 ap=8 global=8 sta=8 done=fffffff7 ack2=c2f10152 infra=e3fc0000 all_sta=0 fsm=80000001 … errno=60`。故 356 的失败点是投票 stage4，不是 VCP boot——印证触发缺口真实存在。
- **改动（仅失败路径、只读，`x9/kernel/src/bsd/dev/x9_mm_power.c`）**：把内联 SMMU 快照抽成 static helper `vcp_smmu_snapshot(void)`（static 缓存 `smmu_mmio/smmu_stb/smmu_l2`，`ml_io_map(0x30800000,0x20000)`，读 IDR0/1/5、CR0/ACK、GBPA、STRTAB_BASE/CFG、CMDQ_BASE/PROD/CONS、EVTQ_BASE，按 fmt 线性/两级 dump SID 80/90/a0/a1 各 8 字 STE；`idr0` 无效门控）。`vcp_boot_diag_log` 改为调用该 helper；并在 `mm_vote_take` 的 `log_history` 块（stage4 `ETIMEDOUT`）`mm_irq_history_log` 之前**新增同一调用**。如此**无论哪条路径先失败，一次 boot 都采到 SMMU 状态**。无任何写操作。
- **编译验证（实测）**：`kernel-unit.py` 单对象 `compile_one(x9_mm_power.o)` → `result True`、exit 0、2 条既有导出头 warning（kcdata.h:1612 / vm_param.h:607），`x9_mm_power.c` 零 warning。
- **机器码实证诊断有效性（实测 objdump）**：unstripped 内核含 `_vcp_smmu_snapshot`（`0xfffffe00080cd000`）及静态局部 `.sids/.smmu_mmio/.smmu_stb/.smmu_l2`；`_mm_vote_take`（`0xfffffe00080cb270`）内 `+0x4cc`（`0xfffffe00080cb73c`）处 `bl 0xfffffe00080cd000 <_vcp_smmu_snapshot>`——**356 失败的投票超时路径确会输出快照**，非仅源码层面。
- **打包修复（实测，环境）**：`build.py` 子脚本以 `sys.executable -I`（homebrew python3.15，**隔离模式忽略 user site**）运行；lz4 4.4.5 原仅在 user site `~/Library/Python/3.15/…`，故 `bootimg.py:35 import lz4.block` 报 `ModuleNotFoundError`。修复：`python3 -m pip install --break-system-packages --ignore-installed --target /opt/homebrew/lib/python3.15/site-packages lz4` → `python3 -I -c "import lz4.block"` OK。（先前误装到 framework python3.13 无效，因构建不用该解释器。）
- **整包构建（实测）**：内核 104/104 对象 + 链接 + strip OK（19:58）；`build.py boot --kernel …/kernel.development.sweet` 单独打包 **exit 0**（`build.py all` 重跑会因 audio output bundle 已存在而 exit 2，故用 boot 步复用已构建内核）。`Image 32268288 B`、`shim-boot.img 15118336 B`、`entry 0x1000`、`PSTORE MIRROR`。**当前候选** `x9/out/x9-camera-smmu-dual-20261120/boot.img`，**boot.img sha256 `103e465f3f53c0a6ee9d955b4bc71aa48ab7d788ce82410ebe11bad21b3eb431`**、kernel sha256 `a068df1681051d211bcd333b81520778ecb96e6f2427a86c7fcce062adeb53ea`。类型 SWEET-XNU-candidate；无 UEFI、无设备写、**未硬件验证**。
- **候选取代关系**：本 dual 版**取代** `15634256…`（x9-camera-smmu-full-20261120，仅 VCP-boot 失败触发）——快照内容相同但触发更稳健；`15634256` 仍有效但优先 boot dual 版。更早的 `edf59964…`（4 字 STE/无队列）已废弃，勿 boot。
- **待上机（用户手动）**：`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-smmu-dual-20261120/boot.img`，回传下一份 `~/NNN.txt`。抓取 `X9 MM VCP BOOT:`（errno 判 VCP boot 成败）、`X9 MM VCP SMMU:`、`X9 MM VCP SMMUQ:`、`X9 MM VCP STE 80/90/a0/a1:`（8 字）各行。功能性诊断候选（失败路径只读快照），非纯探针镜像。**task #8 写 SMMU 代码仍门控于此 boot 实测 + 用户确认，本轮未写任何 live-SMMU 代码、未上机。**

#### 2026-11-20 RAW staging 格式审计：FORMAT_TAG1 固件中介证实 + X9 format=8 标记为未证推断

- **目的**：闭合目标 RAW staging 阶段 NOTES:308/367 标记的未决项（CAMSV FORMAT_TAG1=1 vs 8 与 iOS RAW10 解包）。
- **寄存器偏移（实测 `isp8s/cam/mtk_cam-sv-regs.h:327/330/336`）**：`GRAB_PXL_TAG1=0x548`、`GRAB_LIN_TAG1=0x54C`、`FORMAT_TAG1=0x554`——与 X9 `pipeline.inc:44` 注释一致。
- **正常路径 FORMAT_TAG1 由固件 CQ 编程（实测，证实 NOTES:308）**：main `mtk_cam_sv_dev_config@1894` 调 `mtk_cam_sv_df_config@1588`，后者**只写** `SHARE_SRAM_TIME_OUT=0x2000000` 与 `DMATOP_CTL=0x1C0`（auto mode），**不写 FORMAT_TAG1**。全 isp8s `cam/*.c` **无任何 `SV_FMT_MIPI_RAW10`/`SV_FMT_RAW10`/`camsv_fmt` 枚举的寄存器编程使用点**（grep 零命中）。内核唯一直接写 FORMAT_TAG1 是 **slave stress-test 路径** `mtk_cam_slave_sv_dev_config:1870`=`0x1`（配 `camsv_stress_buf`、stride `0x1400<<16|0x10`），非正常预览。
- **IPI 侧（实测 `mtk_cam-fmt_utils.c`）**：`sensor_mbus_to_ipi_fmt`(:968) 把 V4L2 `S*10P`(packed)→`MTKCAM_IPI_IMG_FMT_BAYER10_MIPI`(:419-423)、`S*10`(unpacked)→`BAYER10_UNPACKED`、`MTISP_S*10`→`BAYER10`。`mtk_cam_get_pixel_bits`(:226)：**BAYER10_MIPI=10bpp packed**（4px/5byte，匹配 iOS `camera_isp.c:unpack_line`），BAYER10_UNPACKED=16bpp。IPI 格式经 `cfg_in_param->fmt` 传固件，由固件 CQ 翻成 CAMSV FORMAT_TAG1。
- **X9 现状与分歧（实测代码）**：`pipeline.inc:49 p->format=8`（SV_FMT_MIPI_RAW10），注释引 `mtk_cam-sv.h:142`+`fmt_utils:419-423`。**NOTES:367「X9 FORMAT_TAG1=1 匹配 Android:1870」已过时**——代码现为 8。`=8` 是**推断**（MIPI packed payload ↔ SV_FMT_MIPI_RAW10=8 ↔ IPI BAYER10_MIPI 10bpp 三者命名/位宽一致），**非源码直证**（正常路径值固件中介，内核不可见）。slave=1 不能反推正常路径=1（不同数据路径）。
- **决策（不猜测）**：**保留 `format=8`**（与 BAYER10_MIPI 10bpp packed + iOS 4px/5byte 解包一致的最强推断），**不**无证据翻回 1。1 与 8 离线均不可直证。
- **实证验证计划（待 CAMSV 通流后，门控于 MM_2→boot）**：同一正常预览请求回读 `FORMAT_TAG1(0x554)` 实际值 + 抓 RAW payload 首行字节序样本，对照 iOS `unpack_line` 的 4px/5byte MIPI RAW10P 排列；若回读=1 且 payload 为 packed，则需重新评估寄存器语义（FORMAT_TAG1 可能非 payload packing 的唯一决定位，packing 另由 CQ/pak_sel 控制）。在此之前不把任何静态对照当作 RAW 格式验收。
- 本轮无代码改动（避免无证据翻转）、无镜像、无设备操作；commit 见下。

#### 2026-11-20 路径 b STE 模板逐位源码实证（Android make_cdtable_ste + MTK setup_features）+ ASID 门控修正

- **目的**：在 boot 几何门控期间，把 task #8 将写的 SID 0xa0 STE 模板**逐位对照 Android 源码定死**（非猜测），消除模板层面的不确定性。
- **STE Config 编码（实测 `arm-smmu-v3-common.h:259-270`）**：`CFG[3:1]`：ABORT=0、BYPASS=4、**S1_TRANS=5**、S2_TRANS=6；`S1FMT[5:4]` LINEAR=0/64K_L2=2；`S1CTXPTR[51:6]`、`S1CDMAX[63:59]`。故 X9 `native_smmu.inc:163` 的 `1|(4<<1)`=0x9 是 **BYPASS 默认 STE**（非 S1_TRANS，先前用语需澄清）。
- **权威模板（实测 `arm-smmu-v3.c:1437-1491 arm_smmu_make_cdtable_ste`）**：word0=`V|CFG=S1_TRANS(5)|S1FMT|cdtab_dma&S1CTXPTR_MASK|S1CDMAX`；word1=`S1DSS|S1CIR=WBRA|S1COR=WBRA|S1CSH=ISH|S1STALLD(若 FEAT_STALLS&&!stall_enabled)|EATS(ats?TRANS:0)|STRW(NSEL1)`；word2=`S2VMID=0`（NS-EL1 无 E2H）。普通 domain attach 用 `S1DSS=SSID0(2)`（:2847）。
- **MTK 覆盖（实测 `mtk-smmu-v3.c:1592-1634 mtk_smmu_setup_features`）**：① TCU_PF：`FEAT_TCU_PF && !ats && s1_vld` → word1|=`TCU_PF(data->tcu_prefetch)`(bits57:56)。② **NC CD fetch：`!FEAT_COHERENCY && s1_vld` → 清 S1CIR/S1COR 并设 NC(0)**——MT6993 MM SMMU 非一致，故 NC 是 MTK 正确选择（证实 X9 相机注释）。③ MPAM：word4 PARTID/word5 PMG 从 prior STE 恢复（SID 0xa0 prior=ABORT→0）。④ MTK **不改 S1CSH**。
- **X9 相机引擎已匹配（实测 `x9_camera_smmu.inc`）**：word0 `:206/281`=`1|(5<<1)|cd_pa`=0xB|cd_pa ✓；默认 `:247` ABORT(V=1)、DISPLAY `:248` BYPASS、`:249` ATTR_TYPES_OVR→SHCFG_INCOMING(1<<44) ✓；word1 `:82 cam_smmu_s1_ste_word1`=`2|(2<<6)|((idr0>>24)&3==0?1<<27:0)|tcu_prefetch<<56`。
- **路径 b SID 0xa0 STE 模板（定死，镜像 X9 相机/MTK）**：word0=`0xB|cd_pa`（V+CFG5+S1FMT0+CDMAX0，cd_pa 16K 对齐）；word1=`0x82 | (IDR0.stall_model==0 ? 1<<27 : 0) | (TCU_PF<<56)`（S1DSS=SSID0、S1CIR/COR=NC、S1CSH=OSH、EATS=0、STRW=NSEL1）；word2=0；words3-7=0（MPAM=0）。**S1STALLD/TCU_PF 由 boot IDR0/MTK 策略运行时定**；TCU_PF 默认 DIS(0) 安全。
- **一处真分歧（记录待决，非阻塞）**：X9 相机 word1 `S1CSH=OSH(2)`，Android mainline=`ISH(3)`（`ARM_SMMU_SH_ISH=3`，common.h:185），MTK 不覆盖 S1CSH。NC fetch + CD 已 PoC clean 下 OSH/ISH 皆一致安全；路径 b 暂镜像 X9 相机 OSH(2)（与已验证相机路径一致），如需严格 Android 对齐可改 ISH(3)。
- **ASID 门控修正（推翻先前「空闲 ASID 缺口」 framing）**：attach 只写 SID 0xa0 自己的 STE/CD，**从不改 ATF protected SID 0xa1**；`TLBI_NH_ASID` 仅无效化 TLB（ATF 表仍有效会重 walk），故 ASID 编号与 ATF 碰撞**非破坏性**。选 ASID=1（或运行时按 IDR0.ASID16）即安全——**free-ASID 不是真门控**，无需 ATF RE 定 protected ASID。真门控（NS 写权限/CR0/STRTAB fmt/CMDQ shift/IDR5 granule/STE 0xa0 默认）全由 boot DIAG 覆盖。
- **结论**：task #8 的 STE 模板与 CD/S1 描述符编码至此**完全源码实证**，仅剩 boot DIAG 的运行时几何/可行性门控。本轮无代码改动、无镜像、无设备操作；commit 见下。

#### 2026-11-20 ATF setup_init 反汇编细化（所有权边界，实测 disassembly；CR0 使能仍待 boot 实证）

- **目的**：履行目标「先 dump ATF `setup_init` 确定 MM SMMU 状态与所有权边界」，定位 CR0 使能与 protected-SID/ASID 赋值，填补路径 b 的空闲 ASID 缺口。对象 `tee.img.atf.original.bin`（load base `0x88800000`，下列地址为 file offset/VMA-from-0）。
- **实测（disassembly）**：① `setup_init@0x34880` 循环 `cmp w2,#3`（3 个 SMMU）；每轮 `ldr x10,[x24,#1496]` 取 per-SMMU 结构，`str x8,[x9,#280]`=strtab base（取自 `global_table[0x128000]`）、`str w8=0x40000,[x9,#288]`=strtab size（256KiB），`ldrb w8,[x10,#36]; cmp #0` 读 per-SMMU enable 标志；`bl 0x21f5c`（w2=39/w3=40/w4=0x1000）分配一个 4KiB 页并 `str x0,[x8,#208]`（结构+208，疑 CMDQ 或 CD 页）。② 循环后 `bl 0x47670`。**修正先前假设**：`0x47670` 经反汇编是**通用数据结构初始化**（`memset` 两块 `0x1000`/`0x2000`、写 magic `0x80028002`、sentinel `0x7fe`=2046、置 `[0x132000+1223]=1`），**不是** SMMU CR0 使能。③ `0x52e3c` 在 `setup_init` 尾部被多次以 `adrp` 字符串页参数调用，是**日志助手**而非寄存器编程。
- **推断（待证）**：`setup_init` 可见循环体**只做软件分配/标志**，未写 `0x30800000+0x20`(CR0)/`+0x80`(STRTAB_BASE) MMIO；硬件使能应在另一函数（消费 enable 标志 `[x10+36]` 者，或独立 initcall）。strtab `0x40000`/64=4096 STE → SID 空间 12-bit（与相机私有 STES=256 不同，MM SMMU 更大）。
- **决策（避免冗余 RE）**：CR0 enable 态、STRTAB fmt/base、CMDQ 几何、STE 0x80/0x90/0xa0/0xa1 默认值由 **task #7 boot DIAG 快照实证给出更可靠**（运行时真值优于静态 RE）；空闲 ASID 可由 boot dump 的 STE 0xa1 `S1ContextPtr` 运行时 guarded 读 live CD 取得。故**不再深挖 ATF CR0-使能函数**（与 boot 实测冗余），critical path 仍门控于用户手动 boot 综合 DIAG 候选 `15634256…`。本轮无代码改动、无镜像、无设备操作。

#### 2026-11-20 MM SMMU 只读状态快照诊断镜像（路径 b 先决条件实测，待上机）

- **目的**：用实测取代昂贵的 ATF `setup_init` 反汇编，直接量取 VCP-boot 失败时刻 MM SMMU（`0x30800000`）的状态，为 task #8「路径 b：XNU 直接编程 SID 0xa0 STE+S1 页表」提供 gating 事实（NS EL1 能否访问、是否上电/enable、STRTAB 布局、SID 0x80/0x90/0xa0/0xa1 STE 默认值）。
- **改动（仅失败路径、只读）**：在 `x9/kernel/src/bsd/dev/x9_mm_power.c` 失败 DIAG 末尾追加 `X9 MM VCP SMMU:` 一行（`ml_io_map(0x30800000,0x20000)` 后读 IDR0/IDR1/IDR5=`0/4/14`、CR0/ACK=`20/24`、GBPA=`44`、STRTAB_BASE=`80`(64-bit)、STRTAB_BASE_CFG=`88`），再按 `x9_camera_smmu_attach.inc` 的 STE 布局解析 4 个 SID：`fmt=(cfg>>16)&3`；fmt0 线性 `STE@strtab+sid*64`；fmt1 两级 `L1[sid>>8]`（span 须=9）→`L2+(sid&255)*64`，输出 `X9 MM VCP STE 80/90/a0/a1:` 各前 4 字。**无任何写操作**；`idr0==0||0xffffffff` 时跳过 STE 读取（防 power-gated/总线挂死）；strtab/L2 各 `ml_io_map` 一次（static 缓存）。偏移与寄存器语义全部对照 `x9_camera_native_smmu.inc`（OPPO arm-smmu-v3）与既有相机 SMMU 基础设施，非猜测。
- **增强（2026-11-20 续，一次 boot 采全 path-b 几何）**：补读 `X9 MM VCP SMMUQ:` 一行——CMDQ_BASE=`90`(64)/PROD=`98`/CONS=`9c`、EVTQ_BASE=`a0`(64)（均在已映射 page0 MMIO 内）；STE dump 由前 4 字扩到 **8 字（DW0-7）**，覆盖 Config/S1Fmt/S1ContextPtr/S1CDMax/S2 全架构字段。CMDQ shift 由已 dump 的 IDR1.CMDQS 推得。如此一次 boot 即定死 task #8 attach 所需全部几何（NS 访问、enable、STRTAB fmt/base、CMDQ base/shift、STE 默认），避免第二轮上机。STE 默认编码佐证：`x9_camera_native_smmu.inc:161` abort STE DW0=`1`（V=1/Config=0）、:163 bypass=`1|(4<<1)`。仍只读、同 `idr0` 门控、无新任意 PA 追踪（不追 STE 指向的 CD，防读 secure 内存挂死）。
- **编译验证（实测）**：`x9/out/camera-smmu-state-20261120/kernel-unit.py` 单对象 `compile_one(x9_mm_power.o)` → `result True`、exit 0、**2 条既有导出头 warning**（与 357 基线一致），`x9_mm_power.c` 零 warning（修掉一处 declaration-after-statement：`sids[4]` 移到所有语句之前）。
- **整包构建（实测）**：`build.py all --boot-config x9/build/boot-device.json --name x9-camera-smmu-full-20261120` → **exit 0**。`Image 32268288 B`、`shim-boot.img 15134720 B`、`entry 0x1000`、`PSTORE MIRROR`、boot-arg 含 `x9_camera=2`。**当前候选** `x9/out/x9-camera-smmu-full-20261120/boot.img`，**boot.img sha256 `15634256705eba2c7e6236fb36e902a0056e50f2fc90d6703ff942b3ad69e237`**、kernel sha256 `8b511d3c255f2bcc96aaecb9944ee8649f721baedbb3819441c8ec4afd718596`。类型 SWEET-XNU-candidate；无 UEFI、无设备写、**未硬件验证**。（前一版仅 4 字 STE/无队列的 `x9-camera-smmu-state-20261120` boot sha256 `edf59964…` 已被本增强版取代，勿再 boot 旧版。）
- **待上机（用户手动）**：`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-smmu-full-20261120/boot.img`，回传下一份 `~/NNN.txt`。需抓取 `X9 MM VCP SMMU:`、`X9 MM VCP SMMUQ:` 与 `X9 MM VCP STE 80/90/a0/a1:`（8 字）各行。这是 358/359/360 同模式的**功能性诊断候选**（失败路径只读快照），非纯探针镜像。
- **推断（待实测证实）**：① MM SMMU 此时应已上电——ATF boot 期用 MM SMMU 映射 protected VCP loader buf（IOVA `0x106c00000`），故 MMIO 可读概率高；若仍 power-gated，读 MMIO 有 365 式挂死风险，已用 `idr0` 有效性门控 STE 读取降低暴露。② 预期 SID 0xa0 STE 为 ABORT 或 zero-invalid（源码「STE 默认 ABORT + 无任何 normal 窗映射请求」支持），SID 0x90 可能 bypass（display）；实测将确认 NS EL1 是否真能读到这些 STE 以及 STRTAB 是线性还是两级，从而定死路径 b 的编程序列与所有权边界。

#### 2026-11-20 分歧 #2 升级为完全源码实证 + 路径 b 映射设计锁定（静态，待 boot 几何）

- **分歧 #2 由「推断」升级为「实证」**：Android `vcp_alloc_iova`（`rv_v2/vcp_helper.c:1953`）经 `dma_heap_find("mtk_mm-uncached")`→`dma_buf_attach`→`dma_buf_map_attachment_unlocked`→**`*start_phys = sg_dma_address(sgt->sgl)`（:2001）= IOMMU DMA API 返回的 IOVA**（失败日志 :2004 亦称 "iova 0x%llx"，变量名 `phys` 是误导）；:2203 `vcp_mem_base_phys = mblock_start_phys`、:2205 logger 同理。该 IOVA 经 `VCP_PACK_IOVA` 写入 GPR（:1155/1158/1192/1194），由 mm_hfrp（SID 0xa0）SMMU 翻译到真实 backing PA。**X9 打包裸 carve-out PA 却从不建此翻译**——源码确证的缺口，非猜测。
- **打包/解包语义（证实，`rv_v2/vcp_status.h:11-13`）**：`PACK(addr)=(uint32_t)(addr|((addr>>32)&0xF))`、`UNPACK(a)=(a&0xFFFFFFF0)|((a&0xF)<<32)`。要求 16B 对齐，把 bit32-35 塞进低 nibble。X9 宏（`x9_mm_power.c:88`，显式 `&0xffffffff`）与之等价。**实测核算**：`PACK(0x15FC00000)=0x5FC00001`，`UNPACK(0x5FC00001)=0x15FC00000` 精确往返。故 MMUP 从 GPR 重建出 36-bit 地址 `0x15FC00000` 作为 normal 口访问地址。
- **路径 b 映射目标锁定（identity）**：为 SID 0xa0 建 S1 翻译 **IOVA `0x15FC00000` → PA `0x15FC00000`**，size `0x400000`（=`X9_VCP_DRAM_HI-LO`，含 logger 子区：total `0x20e000`<`0x400000`）。该 IOVA 落在 normal 窗 `[0x140000000,0x1c0000000)`，与 ATF protected 窗 `[0x100000000,0x140000000)`（VCP loader IOVA `0x106c00000`）不冲突。**GPR 写入无需改**——MMUP unpack 回 `0x15FC00000` 即成有效 IOVA；缺的纯是 SMMU 翻译。
- **S1 几何（实测核算，16K/IAS34）**：`first_root=iova>>25=0xAF(175)`、`leaves=1`、`pages=0x400000/0x4000=256`、`root_end=176≤512`；iova/pa 均 16K 对齐、iova<IAS34(`1<<34`)。`X9_CAMERA_MAX_BYTES=16MiB`（`x9_camera.h:22`）→256 页<1024 上限 ✓。属性复用相机已验证的 `CAM_S1_PAGE_ATTR`（Normal NC/outer-shareable/AP RW/nG/AF/PXN/UXN，匹配 Android "uncached" heap）+ `CAM_S1_MAIR=0xf404ff44` + CD word0（T0SZ30/TG0=2/IPS/ASID）。
- **复用边界（证实）**：`x9_camera_s1_create`（`x9_camera_s1.inc:185`）**自分配** kernel raw（`kmem_alloc`）并 `pmap_find_phys` walk VA→PA 建 PTE，明确拒绝外部 PA（:86-87 "No externally supplied PA or LK carveout"）；carve-out 在 X9 是 `ml_io_map(0x15FC00000,…)` 设备映射（`x9_mm_power.c:223`）。故 **identity 映射需一个 PA 输入的 S1 构建变体**（复用上述描述符编码/几何/PoC clean+readback 校验 rigor，但 PTE 直接指向 carve-out PA 而非自分配 raw）。attach 复用 **prior-preserving live NS attach 模型**（`x9_camera_smmu_attach.inc`：BBM 直写 STE + 继承 CMDQ 的 CFGI/CD/TLBI-ASID/SYNC，不改 STRTAB/CR0/GBPA），**非**私有 reset-init（`x9_camera_native_smmu.inc` 要求 CR0=0 复位态，不适用于 ATF 已启用的 live 控制器）。
- **实现序列（待 boot 几何后写）**：MM acquire/上电后、`MMUP_RELEASE` 前 → 建 S1 identity 表 → 选空闲 ASID（避开 ATF protected SID 0xa1 所用）→ prior-preserving attach 装 SID 0xa0 STE(S1_TRANS→CD) → CFGI/TLBI/SYNC → 释放 MMUP。**仍待 task #7 boot 日志定死**：NS EL1 读/写权限、CR0 enable 态、STRTAB fmt/base、CMDQ base/shift、空闲 ASID、STE 0xa0 默认值。在此之前不写 attach 代码（避免猜测/半成品）。
- **attach 引擎复用面与安装序列（源码证实，2026-11-20 续）**：精读 `x9_camera_smmu_attach.inc` 全文。**引擎深度绑定相机私有对象**（`struct x9_camera_smmu`、`m->map` 自分配 raw、`CAM_SMMU_SID=0x80`/`ASID=1`、「SID80 first leaf」硬编码 :113），且要求调用方提供 **native host**（`x9_camera_smmu_host`：live 控制器 installed storage 的 provider-pinned CPU 映射 strtab/cmdq/strtab_l2 + enter/guard/leave 租约回调，证明 NS 权限与跨生产者序列化）。**MMUP SID 0xa0 在 VCP-boot 时无此 camera host** → 路径 b 需新写：① PA 输入 S1 identity 构建器；② live ATF MM SMMU 的 host adapter（从 MMIO 读 STRTAB/CMDQ/EVTQ_BASE 并 ml_io_map 实际存储，enter/guard/leave 证 NS 访问+序列化，租 SID 0xa0/SSID0/空闲 ASID）；③ STE 0xa0 模板。**非直接复用引擎**，但复用其编码与序列。
  - **常量（`x9_camera_smmu.inc:21-31`）**：`CR0=0xD`(SMMUEN bit0|EVTQEN bit2|CMDQEN bit3)、`CR1=(2<<10)|(2<<4)`(NC outer-shareable)、`CR2=6`、`CMD_SYNC=0x46`、`CFGI_STE=3`、`CFGI_CD=5`、`TLBI_NH_ASID=0x11`、STES=256、STE_WORDS=8。
  - **命令编码（attach.inc:263-275）**：CFGI_STE/CD=`op|(SID<<32)` word1=1；TLBI_NH_ASID=`op|(ASID<<48)` word1=0；SYNC word0=0x46。16B 命令写 `cmd+((prod+n)&(ring-1))*16`→PoC clean→发布 PROD(0x98)→轮询 CONS(0x9c) **两连样==prod** 才算消费；ring=`1<<shift`，shift=`CMDQ_BASE&31`。
  - **capture 前置（attach.inc:335/365-366）**：要求 `CR0==0xD`、ACK==CR0、GBPA bit31=0、IRQ/IRQACK=0、EVTQ prod==cons、`shift≥2 且 ≤IDR1.CMDQS(bits21-25)`、STRTAB fmt0 须 cfg==8（线性）或 fmt1 SPLIT8（`(idr0>>27)&3==1`、span9、bits8-20）；**STE80 DW0≤1（zero-invalid/abort）且 words1-7==0**，否则 EBUSY（不接管 live/bypass/translated）。快照所有 256 STE+L1+regs，guard 每次 mutation 前重验。
  - **安装 BBM 序列（attach.inc:400-412）**：capture→store word0=0（清 V）→submit(CFGI_STE+SYNC)→逐字 store words1-7（取自 `m->ste.va[SID*8+w]` 模板）→store word0（S1_TRANS+V）→submit(CFGI_STE+CFGI_CD+SYNC)→submit(TLBI_NH_ASID+SYNC)→guard→100ms deadline。STE 逐字 atomic64 volatile store+PoC clean+expected[] journal（:303-315）。RETIRE 反向恢复 prior STE（:467-472）。
  - **boot 日志须证实的 gating**：CR0 是否==0xD（ATF 可能留不同值→引擎 capture 拒，需适配）；NS 能否**写** STE/CMDQ（快照只读证读，写权限待 install 时 guard 证）；STRTAB fmt（决定 host adapter 映射线性页还是 L1+L2）；STE 0xa0 DW0≤1；CMDQ shift。**唯一快照未直接覆盖**：空闲 ASID（未追 STE 0xa1 指向的 CD 读 ASID，防读 secure 内存挂死）——host adapter 运行时 guarded 读 live CD 或 ATF RE 定 ATF protected ASID 后选空闲值。

#### 2026-11-20 VCP/MMUP 启动链静态审计（源码逐项对照）+ READY pin 证实 bug 修复

- **范围/边界**：纯静态审计，不出镜像、不上机、不操作设备。对照 Android `rv_v2/vcp_helper.c`、`mmup.c`、`vcp_logger.c`、`mtk-mbox.c`、`arm-smmu-v3.c`、`mtk-smmu-v3.c`、`live.dts`，并只读反汇编原 ATF `tee.img.atf.original.bin`（load base `0x88800000`）与 `lk_b.img`。改了一处证实 bug + 删两个死函数；单对象交叉编译验证。提交 `e56b7f54`。
- **证实 bug #1（已修）——READY 轮询 pin/offset 错误**：旧代码看 `clr_irq_reg(+0x10c)` **bit7**、消息槽 **+0x38**。按 `live.dts` recv-table + `mbox_setup_pin_table()`（`vcp_helper.c:2781` / `mmup.c:268`，`is64d=1`）逐项推导：`IPI_IN_VCP_READY_0`（vcp chan `0x05`）是 vcp mbox2（`0x31ba0000`）recv 表第一项，`IPI_IN_VCP_READY_1`（mmup chan `0x1a`）是 mmup mbox1（`0x31bc0000`）recv 表第一项 → 两者均 **pin_index 0 / offset 0**。DT 无 `mbox%d_recv`，故状态读 `clr_irq_reg` bit0、写同位清（`mtk-mbox.c mtk_mbox_read_recv_irq`/`mtk_mbox_clr_irq`），消息在 `base+0x00`。**旧常量即使 MMUP/VCP 已启动也永远看不到 READY**；357 handoff 曾称 bit7/+0x38「逐项一致」属误判，本轮纠正。修复：`VCP_MBOX_READY_BIT=(1u<<0)`、`VCP_MBOX_READY_MSG_OFF=0x00u`，DIAG3 消息槽同步改 +0x00。
- **逐项一致（源码证实，无需改）**：SMC `0xc200052c` 值/序（RESET_SET 2 → MMUP_RESET_SET 5 → 写 core1 GPR → MMUP_RELEASE 6 → 轮询 → 写 core0 GPR → VCP_RELEASE 3 → 轮询）；GPR 偏移 `cfg+0x44/48/4c` 与 `VCP_PACK_IOVA` 打包；保留内存布局（总长 `0x20e000`、logger 偏移 `0x8d000`/size `0x180000`、buf 1 MiB）；`vcp_log_ctrl` 初始化（base=`0x504C5402`、enable=0、info_ofs=24、buff_ofs=32）；3000 ms 超时；`vcp_enable_pm_clk` 在本 DT 无 regulator 操作（仅 IRQ + reset）；mbox 读/清寄存器语义；TBUF dump 对应 `mmup_do_tbufdump_RV33`。
- **差异 #2（证实的架构缺口，未修）——GPR 内容 IOVA vs 裸 PA**：
  - **Android**：`vcp_alloc_iova`（`vcp_helper.c:1953`）经 `dma_heap_find("mtk_mm-uncached")` → `dma_buf_attach/map_attachment`（mm_smmu_hfrp 共享设备，SID `0xa0`，IOVA 窗 `[0x140000000,0x1c0000000)`）→ `sg_dma_address` 是 **IOVA**，写入 GPR。翻译由内核 DMA API 经 pKVM hypercall `HYP_PMM_SMMU_CONTROL=0xBB00FFAE`（`mtk-smmu-v3.c:80`）→ ATF 建立。STE 默认 **ABORT**（`arm-smmu-v3.c:1412 arm_smmu_make_abort_ste`），设备驱动 attach domain 后才切 S1_TRANS。
  - **ATF（反汇编证实）**：内建完整 SMMU 管理器（`mtk_smmu_setup_init`、`map_smmu_pgtable`@`0x26954`、`smmu_aid2sid_linear_map`、SMC `MTK_SIP_TEE_SMMU_CONTROL=0xc2000831`/`MTK_SIP_HYP_SMMU_CONTROL=0xc2000834`）。boot 期映射经 region 表（`0x898d8`，8 项）：entry0–2 IOVA 全在 **protected 窗** `[0x100000000,0x140000000)`（entry1 = VCP loader buf，IOVA `0x106c00000`/size `0xe00000`），entry3–7 为 PA-direct 区。**没有任何 boot 期映射覆盖 normal hfrp 窗**。`map_smmu_pgtable` 仅按 IOVA 查 iovaGraph 按需写 PTE；调用者只有 region_map（boot 装载）与运行时 SMC handler（`0x337f8`）。
  - **LK（扫描证实）**：`lk_b.img` 只调 `0xc2000413`（VCP 装载，ATF 内名 `MTK_SIP_TEE_UFS_CONTROL`，handler `0x54408`，op=2 装载），**不调任何 SMMU 控制 SMC**。
  - **X9（审计证实）**：GPR 写裸 PA `0x15FC00000`（打包成 IOVA 形式），**从不为 SID 0xa0 attach domain / 建翻译**；VCP boot 前不写任何 SMMU 寄存器、不发 SMMU SMC。
  - **结论**：X9 释放 MMUP 时，normal 口 DRAM 访问（GPR3 logger buf、MPU region 10–13）没有翻译。359/360 的异常环（`lr=14e4`、sp 下穿、`sram_w=0`）与此**吻合（推断，非实证）**。唯一未直接 dump 的细节是 ATF `setup_init` 留给 SID 0xa0 的 STE 默认值；但「STE 默认 ABORT 生命周期 + 无任何映射请求」使翻译失败成为源码支持的预期。
- **顺序审计（task 3，证实）**：shim 的 camera/SMMU handoff（`camera_handoff.c`/`camera_smmu_handoff.c`）只读 FDT，无 SMMU MMIO 写；XNU 中仅相机代码引用 `0x30800000` 且在 MM acquire/VCP boot **之后**运行。loader buffer `0xbd200000`（mblock-51）低于 XNU 内存起点 `X9_RAM_LO=4 GiB`，**不被 XNU 映射/覆盖**（363「内容完好」推断现有源码支撑）。Android `call-owned-runtime/dmesg.txt` 每次 resume 重释放 MMUP 均 ready、无额外 SMMU 操作 → TF-A protected 翻译长期保持。**故 X9 在 VCP boot 前不破坏 SMMU/MM 电源状态**；stage=4 失败不能归因于 X9 早期扰动。
- **365/366 状态**：365 hang 的 bzero(Device) 假说已用 volatile 32-bit store 修复（`97fe8e79`）；carveout2 镜像待 366 日志区分 `ml_io_map` hang vs bzero hang。**本轮未出镜像**。
- **编译验证**：`x9/out/camera-vcp-ready-pin-20261120/kernel-unit.py` 单对象 `compile_one(x9_mm_power.o)` → `result True`、exit 0、**2 条既有导出头 warning**（与 357 基线一致；删死函数后 unused-function warning 清零）。无整包/boot.img。
- **0x21a50 反汇编结论（证实，2026-11-20 续）**：`0x21a50` = `hyp_smmu_handler`（名字串 `0x70050`），即 `MTK_SIP_TEE_SMMU_CONTROL=0xc2000831/0x82000831`。它是**两命令电源/硬件信号量分发器，不是映射接口**：入口取 `x1=cmd`、`x2=smmu_id`（校验 cmd≤0xfffffffe、smmu_id<0xffffffff），`cmd0→0x343c0(smmu_id,2)`、`cmd1→0x34530(smmu_id,2)`，第二参固定 `2`=mod_id（失败日志 `0x706d9` "mod_id:%u, smmu_id:%u, cmd:%u fail" 证实）。`cmd0`=`hw_sema_vote_count_inc`（名字串 `0x72224`）：表基址 `[0x126da0]`、步长 0x58，锁 entry+0x28，累加 entry+0x18/1c/20 三 mod 投票，**总和为 0（首次）→ 调 0x28d48 HWCCF 上电**，否则 entry+24+mod_id*4 计数 +1（溢出日志 `0x6d401`）。`cmd1`=对称递减/释放：归零日志 "hw_sem already released" `0x6fa48`，全 0 → 0x880c 下电序列。**故 0xc2000831 只做 SMMU 引用计数上/下电，不建 IOVA→PA 翻译**；分歧 #2 修复不能走它。
- **0x4496c 反汇编结论（证实，2026-11-20 续续）——路径 (a) SiP SMC 映射是死路**：
  - SMC 名字表 `0x78c50` 项 q2 确指 `0x8884496c`，即 `MTK_SIP_HYP_SMMU_CONTROL=0xc2000834/0x82000834`。但本体是**日志 putc 桩**：`cbnz w0, return 0`（任何非零 cmd 空操作返回 0）；cmd==0 时把 `x1` 字节追加进 `0x12ddb8` 的 256B 环形缓冲，带 `\r`(0xd)/`\n`(0xa) 检测。**对真实 SMMU 命令什么都不做，不建翻译。**
  - 上轮猜测的「`0x44ac4` 是 SMMU 映射分发器」被名字表证伪：`0x44ac4`=`MTK_SIP_TINYSYS_SCP_CONTROL`（0xc2000525），`0x44a18`=`MTK_SIP_KERNEL_GET_RND`（0xc2000120），均非 SMMU。
  - ATF 真正映射器是 **2MiB 块映射、服务 protected 窗、由 pKVM 调用而非 SiP SMC**：`map_smmu_pgtable@0x26954`（PA 须 2MiB 对齐 `tst x3,#0x1fffff`，经 `iovaGraph_lookup@0x470d4` 取页表，按 2MiB 块写 PTE，attr `0xe41`/`0xe01` 差 bit0x40）；运行时包装 `0x337f8`（IOVA 向下 2MiB 对齐→`0x460c8` 分配页表→`0x26954`）**ATF 内无 BL 调用者、无指针引用**，应由 pKVM 经安全服务表调用。`map_pgtable` 的 ATF 内调用者仅 `region_map@0x2ec4c`（boot 装载，7 处含 VCP load `0x56900`）与 `0x337f8`。
  - **Android 本机 MM SMMU 是 hyp+secure 介导（源码证实）**：`mt6993_data_mm`（`MM_SMMU`，compatible `mediatek,mt6993-mm-smmu`，mtk-smmu-v3.c:3187）flags=`SMMU_DELAY_HW_INIT|SMMU_SEC_EN|SMMU_HYP_EN|SMMU_EXTRA_DCM_EN|SMMU_HANG_DETECT`。`HYP_PMM_SMMU_CONTROL=0xBB00FFAE` 只用于 `hyp_smmu_debug_smc` 故障调试（`enum hyp_smmu_cmd` 仅 STE_DUMP/TF_DUMP/REG_DUMP，受 `SMMU_HYP_EN`+`is_protected_kvm_enabled()` 双门控），**非映射通道**。normal 窗主映射走标准 io-pgtable `ops->map_pages`（`mtk_map_pages`:348）+ 直写 STE（`set_dev_bypass_smmu_s1`:1321 经 `arm_smmu_write_ste` 切 BYPASS↔S1_TRANS）；`mtk_smmu_hw_init`:937 先 `arm_smmu_init_sid_strtab`「STE installed before smmu enable」。
  - **推断（待证实）**：X9 启动链（LK→shim→XNU）**无 pKVM hypervisor**，NS EL1 对 MM SMMU 的访问不像 Android 那样 trap 到 EL2，理论上可直接编程 STE+S1 页表（路径 b）；但 ATF EL3 已配 secure/protected 上下文（SID 0xa1），可能留有访问保护或把 SID 0xa0 STE 置 ABORT。
- **下一步具体动作**：为 SID 0xa0 建立 carve-out 的 IOVA→PA 翻译，再释放 MMUP。**路径 (a) 调 ATF SiP SMC 已排除**（0xc2000831 电源投票、0xc2000834 日志桩，ATF 无 normal 窗动态映射 SiP SMC）。**走路径 (b) 从 XNU 直接编程 STE+S1 页表**，复刻 Android 内核 `arm_smmu_init_sid_strtab`→装 SID 0xa0 STE（S1_TRANS）→io-pgtable 映射 carve-out（`0x15FC00000`/`0x400000`，2MiB 对齐）→`arm_smmu_write_ste` 的序列。**先决条件**：反汇编 ATF `setup_init`（`0x34880` 区，全局初始化 `0x47670`）确定其留给 MM SMMU 的状态——是否上电/时钟、SID 0xa0 STE 默认值、NS EL1 能否访问 normal 上下文（secure 寄存器访问保护），再定编程序列与所有权边界。本轮分析脚本在 `x9/out/camera-vcp-ready-pin-20261120/`（gitignored）。

#### 2026-11-20 日志 360 分析与 MMUP TBUF/时间线/SRAM 里程碑诊断镜像

- **360 实机结果（`/Users/tosasitill/360.txt`，镜像 `x9-camera-vcp-sramlog-20261120`）**：
  - `X9 MM VCP BOOT: stage=4 pa=190000000 init=0/4 end=0/4 pc=0/0 rstn=ff00/ff01 smc=0/0/0/0 mmup=0/0 vcp=0/0 errno=60`。
  - `X9 MM VCP DIAG1: pwr_ack=80f0010c pdn=8e8a8285 fenc=f0500180 slp=10100007/fccff11 sys=106c200 sec=0/0 wdt=0/0`。
  - `X9 MM VCP DIAG2: c1_latch=0 lr=14e4 sp=fe1ce758 gpr5=1 c0_latch=0 gpr5=1 sram=1a0b5/1a0b5 exc=0/0/0`。
  - `X9 MM VCP DIAG3: mbox4=0/0/0/0 msg=0 init=0 exc2=0/0/0 sram_w=0 r=0`；无 `X9 MMUP LOG:` 输出（`0x31837000+0x4430c..0x44b14` 全 0）。
- **观察与静态核对**：
  - **已证实**：`x9_mm_power.c` 相对 Android `vcp_helper.c`/`mmup.c`/`vcp_logger.c`/`vcp_helper.h`/`vcp_reservedmem_define.h` 的启动序列（SMC `VCP_OP_RESET_SET=2`、`MMUP_OP_RESET_SET=5`、`GPR1/2/3` 打包与偏移 `0x8d000` / 总长 `0x20e000`、`MMUP_OP_RESET_RELEASE=6`）逐项一致；`0x190000000` 落在重映射窗口 `[0x140000000, 0x1c0000000)` 内，MPU region 10–13 均非空。
  - **已证实**：反汇编 `tinysys-mmup-RV33_A` 表明 `0x15d1c`（`MPU_setting`）在 region 5（`0x500000==0x500000`）必调 `0x1965e`（`printf`）写非缓存区 `0x4430c`（MPU region 3 `w1=0x0e0`）；360 实测 `sram_w=0` 说明 MMUP 未完成 `0x15d7e` 处的 `printf`。
  - **推断**：359（`sp=fe114830`）与 360（`sp=fe1ce758`）均满足 `(0x21ff0 - sp) % 8 == 0` 且约 `32 MiB`，同时 `lr` 固定在 `0x14e4`（`0x14e0: jal 0x1511a` 返回点），高度疑似 Core 1 在 `0x1511a` 之前/之内发生递归异常导致栈下穿 `0x21000`（MPU region 1 只读区）后在 `0x13f4: addi sp, sp, -8; 0x13f8: sw t0, 0(sp)` 死循环；`exc`/`exc2`（`0x29b00..24`）位于可写回缓存区（MPU region 2 `w1=0x3e0`），若 D-cache 已开则 AP 直读 SRAM 仍见 0。
- **本轮改动**：
  - 在 `x9/kernel/src/bsd/dev/x9_mm_power.c` 仅失败路径补充：`X9 MM VCP SNAP pre/rst/1ms/end`（在 `RESET_SET` 前、后、释放后 `1ms` 及超时各采样 Core 1 `STATUS/PC/LR/SP/LATCH/WPTR/RSTN/SYS`）、`X9 MM VCP DIAG4/5`、`X9 MM VCP INTC1`、按 Android `mmup.c mmup_do_tbufdump_RV33()` 导出 16 项硬件 `X9 MMUP TBUF` PC 轨迹（退出时恢复 `R_CORE1_DBG_CTRL`），以及只读 `X9 MMUP HDR`/`PROG`/`STK`（LK/TF-A 写入的 MMUP SRAM 头、进度里程碑与启动栈帧）。正常路径无额外写操作。
- **构建结果**：`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --name x9-camera-vcp-tbuf-20261120`，exit 0，`x9_mm_power.c` 零新增 warning；Candidate 为 `x9/out/x9-camera-vcp-tbuf-20261120/boot.img`（`15134720` B，`boot_sha256=bb3db4dcda4f76cb024db9036922d7d806168492caba235ccc02fbdc28594692`，`kernel_sha256=22f66715e7e3085c9f819889f63ee2b7a2c83fa331b5f66e780b3ae885d9f986`，含 `x9_camera=2`）。未使用 CI，无 run ID，待用户手动 `fastboot boot` 实测并回传下一份日志以一次性定位首错指令。

#### 2026-11-20 日志 359 分析与 MMUP SRAM 日志/MBOX 诊断镜像

- **359 实机结果（`/Users/tosasitill/359.txt`，镜像 `x9-camera-vcp-win-20261120`）**：
  - `X9 MM VCP BOOT: stage=4 pa=158000000 init=0/4 end=0/4 pc=0/0 rstn=ff00/ff01 smc=0/0/0/0 mmup=0/0 vcp=0/0 errno=60`（保留内存成功落在 `[0x140000000, 0x1c0000000)` 窗口内）。
  - `X9 MM VCP DIAG1: pwr_ack=80f0010c pdn=8e8a8285 fenc=f0500180 slp=10100007/fccff11 sys=106c200 sec=0/0 wdt=0/0`（`mm_mmup_sel` FENC bit31=1 时钟正常，`VCP_SYS_CTRL` 已带 `0x8200`，但 `VCP_R_SLP_EN` 仍保留 bits 1/2=1，表明 MMUP 未运行到 `0x153de` `0xbc38`）。
  - `X9 MM VCP DIAG2: c1_latch=0 lr=14e4 sp=fe114830 gpr5=1 c0_latch=0 gpr5=1 sram=1a0b5/1a0b5 exc=0/0/0`：
    - `sram=1a0b5/1a0b5` 证实两核 SRAM 均已由 LK/TF-A 正确加载。
    - `lr=14e4` 精确命中 `tinysys-mmup-RV33_A` 异常/系统调用入口 `0x14e0: jal 0x1511a`（`ecall` 分支跳过 `0x29b0c/14/1c`，仅写 `0x29b00`/`0x29b24`，随后由 `0x14c5e`/`0x14ecc` 处理 `a7=0x32` `configASSERT`）。
- **本轮改动**：
  - 反汇编 `tinysys-mmup-RV33_A` `0x1965e`（`printf`）→ `0x17810`（`logger_puts`）定位 MMUP 固件自带 12 KiB SRAM 日志环形缓冲：`w_pos` `@0x31837000+0x4430c`、`r_pos` `@+0x44310`、ASCII 数据 `@+0x44314`（长 `0x3000`）。
  - 在 `x9/kernel/src/bsd/dev/x9_mm_power.c` 仅失败路径追加 `X9 MM VCP DIAG3`（读 `mmup_mbox4 +0x100/104/108/10c/038`、`mbox1_init`、`0x29b00/04/24`、`sram_w/r`）及 32-bit 对齐只读输出 `X9 MMUP LOG:`，直接读取 MMUP 真实启动/断言日志。
- **构建结果**：`python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --name x9-camera-vcp-sramlog-20261120`，exit 0，零新增 warning；Candidate 为 `x9/out/x9-camera-vcp-sramlog-20261120/boot.img`（`15130624` B，`boot_sha256=492564bca799bc9efc46a9ec5dc8b72beed2050cc02fbada2b0f0e6b95018133`，`kernel_sha256=cd2880dbc8dc8c541b8b303b649061e8c3f1f53074d3a709da888abac45f6700`，含 `x9_camera=2`）。未使用 CI，无 run ID，尚未上机测试。

#### 2026-11-20 日志 358 分析与 VCP/MMUP 保留内存重映射窗口修复

- **358 实机结果（`/Users/tosasitill/358.txt`，镜像 `x9-camera-android-vcp-20261120`）**：
  - `X9 MM VCP BOOT: stage=4 pa=11e2cc000 init=0/4 end=0/4 pc=0/0 rstn=ff00/ff01 smc=0/0/0/0 mmup=0/0 vcp=0/0 errno=60`。
  - 四次 TF-A SMC（`VCP_OP_RESET_SET` / `MMUP_OP_RESET_SET` / `MMUP_OP_RESET_RELEASE`）均返回 `0`，`R_CORE1_SW_RSTN_CLR` 由 `0xff00` 变为 `0xff01`，但 MMUP 在 `3000ms` 内未发出 `IPI_IN_VCP_READY_1`，且 `R_CORE1_MON_PC=0`。
  - 357 异常卸载修复已生效：`X9 CAMERA: start sensor=1 errno=60 cleanup=0 retained=0 step=configure`，无供电/MCLK 泄漏。
  - 实机 FDT 含 `mblock-51-me_vcp_reserved reg=0xbd200000 size=0xe00000`，对照 `lk_b.img` 反汇编（`app_load_vcp` / `verify_load_vcp_image`）证实 LK 仅在 `SMC 0xc2000413` 装载 `vcp_b.img` 成功后才保留该 mblock。
- **根因实证（反汇编原厂 `vcp_b.img`）**：
  - `tinysys-mmup-RV33_A`（表 `@0x20440`，函数 `@0x15f64`）与 `tinysys-vcp-RV55_A`（表 `@0x1c220`）均硬编码 22 项 36-bit → 32-bit 重映射表，DRAM 仅接受 `[0x140000000, 0x1c0000000)`（16×128 MiB → VA `0x30000000..0xafffffff`，与 `live.dts` `mm_iommu_group_1 = <0x01 0x40000000 0x00 0x80000000>` 一致）。
  - 358 分配的 `pa=0x11e2cc000 < 0x140000000`，导致 MMUP `0x15f64` 命中 `err ap_addr:0x%llx` 返回 `0`，破坏 MPU region 10–13 配置。
- **修复与诊断**：
  - 修改 `x9/kernel/src/bsd/dev/x9_mm_power.c`：`vcp_alloc_reserved()` 将连续保留内存约束在 `[0x140000000, 0x1c0000000)` 且不跨 128 MiB 重映射块边界（临时候选块不加 `KMA_PERMANENT` 并在选定后释放）。
  - 在 `vcp_boot_ensure()` 仅失败路径增加只读诊断输出（`X9 MM VCP DIAG1/2`：读取 `VCP_PWR_ACK`/`VCP_PDN`/`VCP_FENC`/`VCP_R_SLP_EN`/`VCP_POWER_STATUS`/`VCP_SYS_CTRL`/`SEC_GPR2..3`/`WDT_IRQ`/Core1 `MON_PC_LATCH`/`LR`/`SP`/`GPR5`/SRAM 首字与异常记录），用于定位若仍超时时的 `pc=0` 根因。
- **构建结果**：执行 `python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --name x9-camera-vcp-win-20261120`，exit 0，零新增 warning；Candidate 为 `x9/out/x9-camera-vcp-win-20261120/boot.img`（`15130624` B，`boot_sha256=0261e1e1f5fd776701ea96d08e23a167efbfdc5425d37e4064c8996c381723a7`，`kernel_sha256=0abe972644b09106e4f0611a9195bf740a2ffca6fc9cba54d9fb4605b2caabcc`，含 `x9_camera=2`）。未使用 CI，无 run ID，尚未上机测试。

#### 2026-11-20 相机 Android VCP/MMUP 对齐测试镜像

- 基于 `3736da1d` 执行 `python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --name x9-camera-android-vcp-20261120`，exit 0；Candidate 为 `x9/out/x9-camera-android-vcp-20261120/boot.img`（`15130624` B，`boot_sha256=73013650d8c4a05686a5a0083252668286826b7d147a641d943b31594a8c8ae8`，`kernel_sha256=ac38f2bd6630e6a7dacf16bfe4faa5824b52ed5207b6f25778331607110b25eb`，含 `x9_camera=2`）。未使用 CI，无 run ID。
- 尚未上机，等待用户手动 `fastboot boot` 实测并回传串口/pstore 日志（重点核查 `X9 MM VCP BOOT`、`X9 MM POWER GET`、`X9 CAMERA MM`、`NATIVE SMMU` 与后续 CAMSV/RAW 日志）。

#### 2026-11-20 357 逐项审计与 Android 流程对齐修复（VCP/MMUP 启动、MM 槽位、异常卸载、RAW10P）

- **357 实机硬阻塞实证**：对原厂固件 `.local/x9/backups/partitions/firmware-originals/vcp-real-android-active-b/vcp_b.img` 检索直接证实 `hwccf_irq_init`、`hwccf_backup_handler`、`hwccf_backup1_cnt[]`、`mminfra_framework_on_off` 位于 **MMUP（VCP Core 1）**；`357.txt` 中 `BACKUP1`（`0x20724`）slot12 投票触发 `int_raw=0x4000`（`backup_vote_int_to_vip`）后因 X9 未执行 Android `vcp_helper.c` + `mmup.c` 启动链而在 `stage=4`（`CCF_BACKUP1_STA` bit3）超时。
- **已实施修改（均未出整包镜像，待上机验证）**：
  1. **`x9_mm_power.c` 对齐 Android VCP/MMUP 启动链**：按 `vcp_helper.c`（`vcp_device_probe`、`reset_vcp`、`vcp_reserve_memory_ioremap`）、`vcp_logger.c`、`mmup.c` 与 `live.dts`（`vcp@31800000` / `mmup@31a00000`），分配连续 36-bit 可寻址、清零并 `dc civac` 到 PoC 的 `0x20e000`（`0x210000` 页对齐）保留内存（含偏移 `0x08d000` `VCP_A_log_ctl` 头初始化、`enable=0`）；发 SMC `0xc200052c` `VCP_OP_RESET_SET(2)` + `MMUP_OP_RESET_SET(5)`；写 Core 1 GPR1/2/3（`cfg_core1+0x44/48/4c`）后发 `MMUP_OP_RESET_RELEASE(6)` 并轮询 `mmup` mbox1/hw mbox4（`0x31bc010c` bit7、`0x31bc0038`、`3000ms`）；再写 Core 0 GPR1/2/3 后发 `VCP_OP_RESET_RELEASE(3)` 并轮询 `vcp` mbox2（`0x31ba010c` bit7、`0x31ba0038`、`3000ms`），失败时锁存 `vcp_fault` 且不释放硬件已引用内存。
  2. **`x9_camera_mm.inc` 修正 `mm_infra_ao`/`mm_infra0` 投票器**：按 Android `clk-mt6993-pd.c:315-329` 将 Slot 0/1 从 `CAM_MM_BACKUP`（`0x20724` bit0/1）改为 `CAM_MM_DOMAIN`（`MTCMOS0` `0x20700` bit31 与 bit29）；`cam_mm_slot_take` 对 `n < X9_CAM_MM_VMM` 保持允许 `MM_2`/显示继承 `0x12900` ACK 为 1（不强求 `0x11318` 同步），`cam_mm_slot_restore` 卸载 Slot 0/1 时不等待 `0x12900` bits 31/29 归零（因 `MM_2`/显示仍持有）。
  3. **`x9_camera_sensor_session.inc` 修复 MM_2 失败后供电泄漏**：`cam_sensor_session_close_locked` 在 `cam_cold_isolate` 成功后，若 `cam_rx_sensor_mm2_take_failed_locked(epoch)` 为真（CSI/MM/DMA 从未准入），即使 `cam_rx_sensor_close_locked` 返回 MM 错误仍执行 `cam_restore(&cam_journal)` 关闭传感器供电/MCLK/I2C，并保留 `receiver-close` 错误码。
  4. **`x9_camera_pipeline.inc` 对齐 MIPI RAW10P 格式**：`p->format` 由 `1`（`SV_FMT_RAW10` MTISP 打包）改为 `8`（`SV_FMT_MIPI_RAW10`，`mtk_cam-sv.h:142` / `V4L2_PIX_FMT_S*10P`），与用户态 `x9/camera/camera_isp.c:unpack_line` 匹配；帧头 `head.format=1` ABI 不变。
- **单对象交叉编译验证**：`x9/out/camera-android-vcp-mm-20261120/kernel-r2/build.log` 中 `x9_mm_power.o` 与 `sweet_x9_camera.o` `compile_one` 均返回 `True`、exit 0、零新增 warning（总计 183 条既有 warning：2 条 `x9_mm_power` 导出头 + 181 条 `sweet_x9_camera`）。未构建整包 `boot.img`，未上机测试。
- **后续待验证项**：构建测试镜像上机验证 VCP/MMUP READY、MM_2、VMM（`0xc00`）、MMDVFS/MMQOS、MM_SMMU 安全/非安全接受状态（`mtk-smmu-v3.c` `SMMU_SEC_EN`）及 CAMSV 首帧图像；传感器 open/EEPROM 8.22 秒耗时（378 kHz FIFO + 双遍 3456B + 每笔 WL guard）暂保留并在通图后再按 Android 1 MHz + 缓存优化。

#### 2026-11-19 MMUP/VCP 启动路径与只读核状态

- **Android 启动链核实**：rv_v2 `vcp_helper.c` 在 probe 中按 `core-nums=2` 调 `mmup_init()`，注册 `IPI_IN_VCP_READY_1`；probe 末尾对 core0/core1 发 RESET_SET，再 `vcp_register_feature(RTOS_FEATURE_ID)` 触发首次 power/clk。`vcp_enable_pm_clk()` 在两核未 ready 时调用 `reset_vcp()`：写 MMUP/VCP 保留内存 GPR，经 `MTK_SIP_TINYSYS_VCP_CONTROL(c200052c)` 的 `MMUP_KERNEL_OP_RESET_RELEASE` 与 `VCP_KERNEL_OP_RESET_RELEASE` 释放复位，并等待 IPI READY。HWCCF feature 位于 MMUP 核。这段说明原 Android 的 MMUP/VCP 并非无需初始化；但 X9 可能由 LK 遗留其运行态，当前无 357 中的直接状态证据。
- **只读诊断**：`x9_mm_power.c` 现在在 stage4 超时短行中读取 core0/core1 `cfg+0x70` STATUS 及 MMUP `cfg+0x44/48/4c` GPR1/2/3；live DT 对应 core0/core1 cfg 基址 `31a0a000/31a0d000`。这些寄存器来自 Android vcp_reg.h，映射失败不参与、也不改变 MM_2 投票成功判据；不清中断、不 reset/release 协处理器。`x9_mm_power.o` 单对象编译 exit 0、`compile_one result True`、2条导出头既有 warnings，日志 `x9/out/camera-mm-global-read-delay-20261118/compile-vcp-state-357.log`。尚无新镜像或实机采样。

#### 2026-11-19 357 MM_2 与 MMUP 前置条件调查

- **实机观察**：357 的 `int_raw=0x4000`；Android `hwccf.h` 将 bit14 命名为 `backup_vote_int_to_vip`，BACKUP1 status bit3 同时为1、DONE bit3为0，AP/global bit3为1。日志长行在 `timeout_raw=8` 后与 IRQ history 交叠，`int_before` 没有可靠显示，不能证明 bit14 是此次投票新产生，也不能确定哪个 backup 资源触发。
- **Android 对照**：该机 DT `vcp@31800000` 有 `core-nums=2`；rv_v2 `vcp_feature_table.c` 将 `HWCCF_FEATURE_ID` 关联 `MMUP_ID`。`vcp_helper.c:reset_vcp` 在双核配置下设置 MMUP 保留内存参数、调用 secure reset release，并等待 MMUP READY，然后才启动 VCP。原 Android 留存 `x9/out/call-owned-runtime/dmesg.txt` 显示 core id 1 READY 及 MMUP notifier READY；这是 Android 侧事实，不是 357 的 MMUP READY 证据。当前 X9 源码未找到上述 Android MMUP 启动/READY 流程，357 也无 MMUP READY 证据；但无日志不能证明固件停机，亦不能仅凭 feature 归属断言 MMUP 就是 VIP 中断处理者或超时根因。
- **诊断变更**：stage4 超时将已有 `int_before/int_status` 和 Android debug 寄存器 OUTPUT_STATUS `0x10004`、FSM_REQ_ACK `0x10008` 加到短行 `X9 MM POWER IRQ SAMPLE`，避免长行混写丢失前后状态；只读，不清中断或改投票。`x9_mm_power.o` 单对象编译 exit 0、`compile_one result True`、2条导出头既有 warnings，日志 `x9/out/camera-mm-global-read-delay-20261118/compile-irq-sample-357.log`；未构建新镜像或上机。
- **下一步**：追踪 MMUP 的固件来源、实际启动/继承状态、READY 和 HWCCF VIP 消费关系；在有证据前不写 VIP STA_CLR、不伪造 ACK、不直接 reset 协处理器。新诊断须下一次构建并测试后才能提供样本；357 仍是最新运行证据。

#### 2026-11-19 实机日志 357：MM_2 阻塞复现

- 用户测试 `x9-camera-wrapper-irq-20261119/boot.img` 后提供 `~/357.txt`；boot-args 包含 `x9_camera=2`。主/前摄 ID 分别为 `9186/0615`，native register errno=0；主摄本次读取 EEPROM 3456/3456、apply 250/250，mode 1/1，VCM 初始化 errno=0。这些只证明相应命令/读取，并非画面或光学校准正确。
- **首次阻塞**：19.626s `MM POWER GET` MM_2 bit3 stage4，`ap=8 global=8 sta=8 done=fffffff7 ack2=c2f10152 infra=e3fc0000 all_sta=0 fsm=80000001 errno=60 timeout_off=1135c timeout_raw=8`；与 356 的主要状态一致。IRQ history pointer d，slot12 为 `addr=21420724 data=00000008`，仅为记录值，尚不能证明原因。receiver-open 返回60，未到 capture；清理 `dependency guard errno=5`，保留依赖与首错。没有本次 SMMU combined IRQ、CAMSV SOF/DONE、RAW 或预览成功证据。
- 构建成功与启动了相机配置已确认；下一步应以 Android MM_2 HWCCF 的完成条件和真实硬件状态为依据查 stage4 阻塞，避免把下游代码编译成功当成设备通过。日志原件在 `~/357.txt`，不提交。

#### 2026-11-19 相机 wrapper IRQ 测试镜像

- 用户要求编译供测试；基于 `57111b64` 执行 `python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --name x9-camera-wrapper-irq-20261119`，exit 0，Candidate 为 `x9/out/x9-camera-wrapper-irq-20261119/boot.img`；设备参数含 `x9_camera=2`。本次未使用 CI，无 run ID。
- 此镜像尚未上机，等待用户手动 `fastboot boot` 和本次设备日志；之前最新实机结果仍为 356 的 MM_2 超时，不能推断新 SMMU IRQ、RAW 或预览已通过。纯内核构建没有更换 GPU 插件；若设备仍为已确认的 GPU280 状态无需 mailbox。

#### 2026-11-19 SMMU wrapper IRQ 计数确认（静态对照）

- **改动**：根据 Android `arm_smmu_combined_irq_handler()` / `mtk_smmu_irq_handler()`，自有 native SMMU 在已校验电源、wrapper 和队列配置后，按 GERROR → wrapper → EVTQ 顺序处理；仅真实 IAR 对应 worker 对 wrapper ACK。对 IRQ_STA 采样的已知 0–19 位按 Android TCU/TBU 顺序分别读 IRQ_CNTx，RMW 更新 ACK_CNT 低8位并写 IRQ_ACK；逐项打印 status/bit/count。MM wrapper 是共享来源，不把事件归属推断为相机 SID；未知位不清并返回 EIO。首个已确认 GERROR 错误不阻止后续 wrapper/EVTQ 处理，未经处理的 GERROR 拒绝继续。
- **验证**：`sweet_x9_camera.o` 单对象交叉编译 exit 0、`compile_one result True`、181 warnings，日志 `x9/out/camera-smmu-irq-drain-20261119/kernel-compile-wrapper.log`；未出镜像、CI 或上机。
- **剩余与边界**：wrapper ACK 是静态移植，实际计数、edge 重投递及退役均未验证；TF monitor 采集、逐 SID/master callback 与安全清除尚未实现。真实 RAW 格式、iOS handler 和可见预览无实机成功证据；356 MM_2 超时仍为首个硬件阻塞。继续遵守暂不构建探针镜像和不上机的要求。

#### 2026-11-19 SMMU IRQ 关机动态状态和待处理中断

- **基于 `b20d9a34` 修正**：关闭 `IRQ_CTRL` 等 ACK 的过程中允许硬件实际更新 GERROR/GERRORN、EVTQ PROD 和 CMDQ CONS；不放宽控制器配置、CPU producer/EVTQ consumer、wrapper 或其他寄存器的比较。之前整个 snapshot 必须静止，会在 source-off 时新的真实事件到达而错误返回 ESTALE。正常启动的 IRQ 控制比较仍严格。
- **关机期间已有 edge**：IRQ_CTRL 已经 ACK 为0 后，若 GIC 有 pending、非 active、IRQ 被先前故障 mask，且已安装的 edge/route/priority 三项都仍匹配，则重新使能该线让真实 IAR 送达 worker，不写 ICPENDR。worker 即使第一错误已锁存，仍验证当前 namespace/backing 并处理真实 EVTQ/GERROR，再保留原始错误；硬件迟迟不交付会打印 pending/active/enable/worker 状态并超时保留。SFM 已关 EVTQEN 时，仍消费此前已发布到自有 EVTQ 的项。
- **验证与限制**：`sweet_x9_camera.o` `compile_one=True`、exit0、181既有 warnings，日志 `x9/out/camera-smmu-irq-drain-20261119/kernel-compile.log`。未出镜像、CI 或上机；该并发路径、真实 GIC IAR/EOI 和错误时完整收尾仍需运行证据。Android MTK wrapper fault/device callback、真实 RAW 格式、iOS 预览也尚未完成，目标保持 active；356 MM_2 仍是首个实机阻塞。

#### 2026-11-19 IRQ 后续：EVTQ 到达期间重采和 RAW 格式边界

- **队列**：基于 `3a71dedd`；Android `arm_smmu_evtq_thread()` 每次排空后 `queue_sync_prod_in()` 并继续直到为空。X9 EVTQ service 现处理一批后重读 PROD（含 overflow），持续处理已实际发布的后来项；在真实队列持续变化时最多处理 `4*ring` 项/轮，超过有明确日志与 EOVERFLOW，不伪造清空；每轮最多输出前16条原始项及压缩计数，避免故障风暴把内核日志拖死。处理事件仍返回 EIO，IRQ worker 会停用本 GIC 线并保留依赖，Android MTK wrapper fault/device callback 和故障后复位/退役未完成。
- **RAW 静态对照**：ISP8s `mtk_cam-fmt_utils.c:415..428` 将 V4L2 `SBGGR10P` 等映射到 `MTKCAM_IPI_IMG_FMT_BAYER10_MIPI`，而 MTISP Bayer10 映射到不同 IPI 格式；iOS `camera_isp.c:cell/unpack_line` 期望每4像素5字节的 MIPI 低两位交织。ISP8s 的 `SV_FMT_RAW10=1` 与 `SV_FMT_MIPI_RAW10=8` 是不同枚举，normal CAMSV 的实际格式来自固件 CQ，未找到 IPI 格式到 ISP8s `FORMAT_TAG1` 的实现或同请求的寄存器/RAW 样本。跨代 `isp7s/cam_legacy/mtk_cam_sv_format_sel()` 对 MTISP 和 10P 均选 `SV_TG_FMT_RAW10=1`，`pak_sel` 都选 packed，但这只能作为跨代参考，不能证明 ISP8s payload 的字节顺序。保留当前寄存器值，修正 `x9_camera_pipeline.inc` 把 `1` 直接叫做“MIPI packed”的不确定注释。
- **编译**：`sweet_x9_camera.o` `compile_one=True`、exit0、181既有 warning，日志 `x9/out/camera-smmu-evtq-drain-20261119/kernel-compile.log`；无镜像、CI 或设备操作。继续保持目标 active：IRQ 实际 IAR/EOI与故障退役、MTK wrapper/设备 fault 接收、同一 ISP8s 请求的格式和实际 RAW、iOS handler/可见预览均未实机验证。最新实机仍为 356 MM_2 超时。

#### 2026-11-19 相机 SMMU combined IRQ 初版实现（未上机）

- **起点/范围**：基于 `cf36ee18`，实现 FDT INTID511 edge-rising 的 GIC 单线接管、实际 IAR filter、worker、正常退出及启动失败收尾；本项尚未实机验证，也不是 Android 全路径等价证明。
- **启动**：`cam_smmu_irq_bind_locked()` 读取同次 LK 的 `x9-camera-smmu-irq`，核对 ADT GIC、映射、线数及空闲/非活跃；保存原 ICFGR/route/priority，只改该 SPI 的 edge 配置及目标 CPU，写前重核已观察状态、每步记录首错；`IRQ_CTRL=5` 经 ACK 后启用 GIC，并要求 enable 回读或已见实际 IAR。位于 EVTQEN 之后、SMMUEN 之前，符合 Android 的中断安装阶段；缺失 topology 会拒绝启动。读取期间的必要检查不宣称排他硬件租约。
- **IRQ/错误**：现有 `sleh_irq` 的 `x9_camera_irq_filter()` 分发真实 INTID511，filter 在 IAR 后只屏蔽本 SPI 并唤醒 worker，EOI 仍由 `sleh_irq` 原路径完成。worker 在 `camera_smmu_lock` 下调用已有 EVTQ/GERROR service；错误锁存、屏蔽 CAMSV 并唤醒等待者，保留绑定资源；无错误时重启本 SPI，mask 期间新 edge 留 GIC pending。`X9 CAMERA SMMU IRQ` 显示 bind step、IAR 与 retire，避免在硬中断中打印。
- **正常关闭**：最后的目标 CFGI/TLBI 完成后，native retire 释放 `camera_smmu_lock`，IRQ shutdown 将 SMMU IRQ_CTRL 关0并等 ACK、等待 GIC pending/active 与 worker 清空，再 mask 本线并按实读恢复保存的 priority/route/ICFGR；随后重新持锁并复核 producer 和 SMMU 才关闭控制器。启动失败也尝试关闭 IRQ。无法确认清空/恢复即返回错误并保留资源，不清 pending 制造成功。故障导致 source/stream 无法正确停止时，尚未证明所有失败路径可正常退役，不能称完整关闭通过。
- **编译**：`sweet_x9_camera.o` `compile_one=True`、exit0、181既有 warning，日志 `x9/out/camera-smmu-irq-owner-20261119/kernel-compile.log`。未修改 `sleh.c`；实接路径是既有 camera filter。无构建镜像/CI/设备操作。尚无真实 IAR/EOI、worker drain 或任何画面证据；356 的 MM_2 timeout 依然是第一处实机阻塞。下一步核验错误收尾（包括 mask 时边沿、GIC 配置共享与 source CTRL 异常）、Android MTK wrapper fault/device callback，再完整对照 CAMSV→iOS。

#### 2026-11-19 SMMU GERROR 同步处理（IRQ 尚未安装）

- **修改/依据**：在 `eeaae489` 的同步检查路径加入 OPPO `arm_smmu_gerror_handler()` 与 `__arm_smmu_cmdq_skip_err()` 处理：记录 GERROR/GERRORN 和 CMDQ CONS；SFM 写 CR0=0 并等待 ACK；CMDQ fetch abort/ATC timeout 不替换命令；illegal 或其他非零错误仅在定位到当前自有 pending batch 且命令字匹配时替换为 CMD_SYNC 并 clean 缓存；按采样的 GERROR 写 GERRORN 进行 toggle 确认。未知 GERROR 位或无法定位自有队列时不确认。任何实际 GERROR 均向原调用者返回错误，保留资源，避免将被跳过的失效命令当成功。处理过程中若 EVTQ 同时有事件，会先记录/消费 EVTQ 再处理 GERROR。
- **边界/待办**：目前仅 `cam_smmu_live()` 采样时同步处理；没有 combined IRQ，Android 的 MTK wrapper fault 清理/设备 fault callback 尚无实现，且保守地保留报错状态、不实现完整自动恢复。这里不代表 Android GERROR 全路径或设备验收完成。SFM 尚无真实寄存器证据，CR0 ACK/替换/确认仅静态对照和编译。
- **验证**：`sweet_x9_camera.o` `compile_one=True`、exit0、181既有 warning，`x9/out/camera-smmu-gerror-20261119/kernel-compile.log`；无新镜像、CI、设备操作；356 MM_2 仍为最早实机失败。下一步实现 FDT 确认的 edge IRQ GIC owner/filter/worker 与安全退役，确保 worker 不与命令持锁和关闭互锁。

#### 2026-11-19 SMMU EVTQ 同步消费（combined IRQ 尚未实现）

- **本轮**：起点 `d434779f`；OPPO `arm-smmu-v3.h:623..652,711..718` 的 EVTQ 消费先观察 PROD、读取 4×64-bit 项，再推进 CONS，最后同步 overflow。`cam_smmu_live()` 在 native 且 EVTQ 已启用时调用锁内服务：校验自有 PROD/CONS 掩码、软件持有的 CONS 及距离，失效专属设备写入页的 CPU 缓存，逐项记录原始 4 个字与 event ID/SID/SSID/stall/read，顺序更新 CONS 和 overflow；最多处理一次实际快照的一环。出现事件或 overflow 时仍返回 EIO，让调用者保持原故障，不能把消费视为故障恢复。未接 GIC、未实现 Android 的 device fault callback、wrapper fault clear 或 GERRORN。
- **边界**：这是现有同步检查中的真实 EVTQ 消费，不是异步 IRQ，也不是成功的端到端采集。队列由现有 `camera_smmu_lock` 串行，涉及其他 SID 的事件也原样打印；不声称它们已交付对应设备处理。`cam_smmu_live` 的同步调用若在 GERROR 同时出现，先消费合格 EVTQ，随后仍报告故障；不消除全局错误。已记录的 overflow 保持 EIO，不假设事件没有丢失。
- **验证**：`sweet_x9_camera.o` `compile_one=True`、exit 0、181 条既有 warning；日志 `x9/out/camera-smmu-evtq-20261119/kernel-compile.log`，首次编译182条时新增 declaration warning，调整后181。无 CI、镜像或上机。最新实机仍是 356 MM_2 失败，不能验证事件或画面。
- **下一步**：实际安装 combined IRQ 和 worker，以 FDT 验证 edge-rising、GIC route/config/实际 IAR/EOI 和 masked 期间 pending；worker 与命令持锁、退出路径协调；完善 Android GERROR 的 SFM/CMDQ_ERR 及 GERRORN、MTK wrapper fault、stall/device fault 回调，再核对 CAMSV 至 iOS。**目标仍 active**。

#### 2026-11-19 相机 SMMU combined IRQ 拓扑（实现未完成）

- **设备树事实**：本机 `/soc/iommu@30800000` 的 `interrupt-names="combined"`，`interrupts=<0 0x1df 1 0>`，GIC 四单元中 type=0 为 SPI、编号479 加32得到 INTID511，flags=1 为 edge rising；不是现有 CAMSV 三条 level-high IRQ。`/interrupt-controller@6800000` 的 GICD/GICR 寄存器范围、四单元格式以及继承的 interrupt-parent 逐项核对。此为保存的 Android live.dts，与下次 LK 设备树值仍须比较。
- **交接实现**：新增 32B `x9-camera-smmu-irq` chosen slot。shim 单次 FDT collection 验证路径、GIC 寄存器、父 phandle 唯一性、combined 名称和完整中断规格后写入；新增 feature 与 `CF_SMMU` 独立，不因中断信息缺失拒绝既有 SMMU policy。内核 SMMU 准备时读取并打印“IRQ not installed”，不接管 GIC、不宣称中断工作。GIC handle 复用批次 phandle 校验，容量由6增至7。
- **验证**：`camera_smmu_handoff.c`、`camera_handoff.c` 的 aarch64 freestanding 交叉单对象编译均 exit 0、`-Wall -Wextra -Werror`；`sweet_x9_camera.o` `compile_one=True`、exit 0、181 既有警告；`mkadt.py` 语法编译通过。首次内核编译因 `.inc` 内 `LCK_MTX_DECLARE` 的行号和另一个 include 重复而失败；已把新 include 移到锁声明后，重编 exit 0，失败记录在交互输出，最终日志 `x9/out/camera-smmu-irq-fdt-20261119/kernel-compile.log`。无整包、CI、上机。
- **尚未对齐**：目前只发布和读取拓扑，没有 IRQ filter/owner/worker，没有 EVTQ 消费或 GERRORN ACK。下一步必须将 actual IAR 接入 `sleh.c`，按 edge GIC `ICFGR` 读取验证并处理 pending/重入；保证 masked 期间新边沿不丢；梳理 CMDQ/EVTQ 内存屏障与非一致缓存、Android `arm_smmu_evtq_thread` 和 `mtk_smmu_evt_handler`、GERROR 的 CMDQ_ERR/SFM 分支；修改 `cam_smmu_live()` 目前非空 EVTQ 即 latch 的语义；先关闭源/IRQ、等待 IRQ worker、解绑 GIC，再退休 SMMU 和所有 backing。不能使用 CAMSV level 模式的 idle 判定冒充 edge IRQ 正确处理，也不能仅清 GERRORN 或丢弃事件。351–356 尚未通过 MM_2，以上均无实机结果。

#### 2026-11-19 相机按 Android 复核 · SMMU 冷启动顺序（未上机）

- **提交/推送**：`2aecbefb`，`git push origin feat/camera-integration` exit 0（`579cead8..2aecbefb`）。

- **用户要求**：相机各阶段按 Android 逻辑与流程对照并修正，范围持续进行；之前不生成探针镜像、不做设备操作的要求仍有效。
- **本轮源码依据和修改**：OPPO `arm-smmu-v3.c:3924..4006` 先 CR0=0 并等 ACK，写 CR1/CR2、stream table、CMDQ base 和初始 prod/cons，打开 CMDQ 后执行 CFGI_ALL/可选 EL2 TLBI/NSNH TLBI，才设置 EVTQ base/prod/cons 并打开 EVTQ。本轮将 native 冷启动改为此顺序；初始四个 queue 游标仍须实测为0；guard 在 EVTQ base 编程前校验先前 base、之后校验本次 base，避免 CMDQ 失效命令错误地要求事件队列已配置。新增的每个写入均有 `step=` 首错；保留冷控制器所有权准入。没有更改现存其他 stream 的映射策略。
- **确认尚有 Android 差异，未完成**：live DT `/soc/iommu@30800000` 指定 combined 中断 SPI 479（GIC INTID 511），Android `arm_smmu_setup_irqs()` 安装 handler 并启用 EVTQ/GERROR；`arm_smmu_evtq_thread()` 消费队列，`arm_smmu_gerror_handler()` 处理并确认全局错误。X9 `cam_smmu_live()` 目前不注册该 IRQ、不消费 EVTQ、不确认 GERROR；先 latch 并保留现场。完整移植需要设计同一 GIC 与 SMMU 队列的 owner、处理 teardown/并发、缓存同步和未知/其他 SID 事件，不能把当前代码称为 Android 等价。SMMU 启动还保留拒绝活动/继承 controller、普通 bank 自有队列等 XNU 所有权约束；Android 完整驱动的安全固件/共享内存、SVA/PRI/ATS、错误中断处理及 iOS 输出流程均未证明已等价。未知运行配置不能据源码假定这些分支实际启用。
- **验证与限制**：基于 `579cead8`；`sweet_x9_camera.o` 单对象交叉编译 `compile_one=True`、exit 0、181 条 warning，日志与命令脚本在 `x9/out/camera-android-smmu-20261119/`。无 CI、整包或上机。356 MM_2 final STA bit3 超时仍是最早实机阻塞，根因和后续阶段硬件行为均未验证。下一步优先完成可追溯的 MM SMMU combined IRQ/GERROR/EVTQ 生命周期与 Android 条件分支对照，再继续 CAMSV 和 iOS 端到端检查。

#### 2026-11-19 相机后续链路与首错日志核对（未上机）

- **提交与推送**：代码和本轮交接提交 `ad27c972`；`git push origin feat/camera-integration` exit 0，远端 `65c9ddb1..ad27c972`。

- **范围**：基于 `65c9ddb1`，复核 ISP8s `mtk_cam-job.c:1456/1477` 与 X9 `start()` 的 CAMSV 先启动、sensor stream 后启动；`mtk_cam-ctrl.c:1235` 通过 cookie 关联 DONE 的对应为 X9 同请求 SOF/DONE、FBC、DMA 交接后才 publish。对照 OPPO `arm-smmu-v3.c:3924` 的 CR1/CR2、表与队列、CMDQEN ACK、CFGI_ALL/TLBI、EVTQEN 与 SMMUEN 顺序；X9 在 CMDQ 使能前写 EVTQ BASE，要求初始游标为零，且不重置已有活跃 controller，与 Android 的通用 reset 并非逐项相同。先前 static review 的 MM/SMI/receiver/SMMU/CAMSV 阶段及逐层 guard 见后续条目；静态调用路径存在不等于真实硬件可达或已完整。
- **本轮修改**：SMMU 后半段记录 `step=` 首错（wrapper、CR1/CR2、bases、CMDQ/EVTQ/SMMU ACK、CFGI/TLBI、live）；不添加 MMIO 读取、不改变成功判据。RAW 拒绝记录 identity/layout-time/sequence/controls/reader-busy 及请求字段。iOS RAW header/时间拒绝输出实际值；帧率 admission 第一次等待输出当前帧长度与范围；graph 启动前/后各记录一次输出配置、handler、enable 位图；不放宽 admission。
- **验证**：`sweet_x9_camera.o` 交叉编译 `compile_one=True`、exit 0、181 条已有 warning；`camera_device.o` iOS 交叉编译 exit 0、无诊断。命令、日志及对象在本 worktree `x9/out/camera-pipeline-audit-20261119/`。无 CI、无整包、无设备动作；仍遵守用户不出探针镜像的要求。
- **未完成**：最新实机仍是 356 MM_2 final STA bit3 超时，根因未证实；后续时钟、SMI/SMMU、SOF/DONE、RAW、handler、画面都无实机通过证据。ISP8s `FORMAT_TAG1=1` 与 iOS RAW10 packed 解包仍缺同一正常请求的真实格式回读/RAW 样本；identity CCM/无 LSC 并非真实颜色校准。下次有实机日志按 MM POWER → NATIVE CONFIG/PREPARE → NATIVE SMMU → FIRST ARM/IRQ → RAW PUBLISH/REJECT → first RAW/graph admission → first picture 顺序判断，禁止把静态对照当可见预览验收。

#### 2026-11-18 相机静态复查 · MM_2 的 IRQ 状态前后样本

- Android `clkchk-mt6993.c:5163` 的 timeout 诊断读取 `CCF_INT_STATUS`。`hwccf.h:150/167` 将 bit5/21 定义为 BACKUP1 timeout，bit9/25 为 vote-before-ready。native `int_before` 在 MMUP dummy read 后采样，`int_raw` 在投票路径退出时、IRQ history 扫描前采样。若 bit5/21 在 `int_before` 已置位，单凭结束采样无法归因给本次请求。若从 0 变 1，再结合 `timeout_raw` 和 history 核对事务。该变更仅采样状态，SET/CLR 与 `STA bit3=0` 成功判据照旧。
- 主 checkout 的 `x9/out/call-owned-runtime/dmesg.txt:1754` 在 Android 120→60 Hz 切换时记录 `dpc get mminfra`。同一次归档的 MM_2 `T[4]` 与 DISP `user[2]` 的起止毫秒数一致（`:2138/2142`）。live DT 的 `disp-dpc@3eff0000` 配置 `mminfra-pwr-idx=3`、`mminfra-pwr-type=2`，无 `power-domains`。`mtk_dpc_driver.c:2093/554/4493` 将这个引用走到 `mtk_mminfra_on_off()`。这组记录可用于对照 Android 正常运行中的 MM_2 引用，启动首笔投票和 356 中 final STA 未清的原因仍待实证。
- 起点 `ddf4f745`。`x9_mm_power.o` 单对象交叉编译 `compile_one=True`、exit 0、两条 XNU 导出头 warning，见 `x9/out/camera-mm-irq-status-20261118/`。先前两项已推送为 `d6e60044`、`ddf4f745`，候帧 IRQ 的 11 个 MMIO 调用点逐项对照通过。实机仍以 356 为准。范围为静态复查和单对象编译。

#### 2026-11-18 相机静态复查 · 首帧 RAW 的交接证据

- Android `mtk_cam-sv.c:3870` 将 CAMSV FSM 的事件交给 `mtk_cam_ctrl_isr_event()`，`mtk_cam-ctrl.c:1256` 以完成 cookie 调用 `handle_frame_done()`，查找同一 job 并标记 engine DONE。X9 在真实同请求 SOF/DONE、FBC 和 DMA CPU 交接通过后调用 `x9_camera_deliver()`。现在 `X9 CAMERA RAW PUBLISH` 每代在首个 staging copy 完成后记录一次，带 generation、capture_id、序号、尺寸、stride、字节数和 SOF/DONE。
- iOS `first RAW read` 发生在完整读取、header 和时间检查通过、`isp_measure()` 已处理有效载荷之后。每次物理镜头启动记录一次，用相同序号和 SOF/DONE 对上 kernel 事件，并打印已有采样统计。`first picture given` 对应后续 handler 调用。采样统计可以协助辨认黑帧，正确字节序、色彩和可见预览仍需真实帧验证。
- RAW 格式核对发现 ISP8s 的 `mtk_cam-sv.h:135/142` 区分 `SV_FMT_RAW10=1` 和 `SV_FMT_MIPI_RAW10=8`，`mtk_cam-fmt_utils.c:419` 区分两种 V4L2 输出格式。当前 X9 `FORMAT_TAG1=1` 与 ISP8s `mtk_cam_slave_sv_dev_config():1870` 的直接编程值一致，iOS 解包按 `V4L2_PIX_FMT_*10P` 的 4 像素 5 字节排列。ISP8s 普通 CAMSV main out 经 IPI 交付格式，现有原厂日志缺对应预览 tag 的实际 `FORMAT_TAG1` 和 RAW payload。本轮保留格式值。核对证据应为同一正常预览请求的 IPI 输出/CQ 或 CAMSV 格式回读加 RAW 样本。
- 起点 `d6e60044`。`sweet_x9_camera.o` 单对象交叉编译 `compile_one=True`、exit 0、181 条 warning，iOS `camera_device.o` exit 0、无诊断。日志、编译 argv 和产物在 `x9/out/camera-first-raw-20261118/`。无 CI run 或实机结果。当前首个实机阻塞仍为 356 `:6086` 的 MM_2 final STA。

#### 2026-11-18 相机静态复查 · CAMSV 序号与候帧 IRQ

- ISP8s `mtk_cam-sv.c:328/367/3688` 的硬件 DONE 使用前一 SOF 绑定的 inner cookie。`mtk_cam-engine_fsm.h:166/214` 在漏帧恢复分支使用 outer，正常 DONE 不比较双 bank。X9 初次 arm 通过 `cam_sv_request_guard_locked()` 校验双 bank FH，重复请求经 `cam_sv_commit()` / `cam_sv_target_observe()` 确认双 bank 后才发布新请求及放行 IRQ。本轮保留无 CQ、单请求路径的 outer 一致性检查。`X9 CAMERA IRQ` 增加预期序号、first/last tag、group 原始样本。
- `cam_repeat_idle_irq_locked()` 之前可在 READY 或 POSTED 的实际 IAR 上锁存错误并屏蔽中断，同时控制线程已睡在 `cam_repeat.completed`，队列为空时没有 worker 唤醒来源。现在发布错误后直接唤醒此等待点，并独立唤醒 worker 打印 `X9 CAMERA PARKED IRQ`。日志记录发生错误时的 role/phase 与已经读取的状态、FBC 控制和计数器，`status_sampled` / `fbc_sampled` 标记实际读过的寄存器组。该记录不进入帧队列，原有 IAR、RCNT 和 EOI 顺序一致。
- 起点 `dd5b1b16`。`sweet_x9_camera.o` 单对象交叉编译 `compile_one=True`、exit 0、181 条 warning，见 `x9/out/camera-parked-irq-20261118/`。本轮只做源码与单对象检查。356 的 MM_2 final STA 超时仍是最早实机阻塞。

#### 2026-11-18 相机静态复查 · 预览配置首个失败调用

- ISP8s `mtk_cam-sv.c:4509` 把 SMI larb、时钟和 `sv_reset_by_camsys_top()` 放在 CAMSV runtime resume 的不同调用。native `NATIVE CONFIG stage=2` 同时覆盖 `cam_smi_clocks_open_locked()`、sensor standby 和 `cam_sv_cold_reset_locked()`。`NATIVE CONFIG` 现在记录 `step=`，可区分同阶段内的失败调用，也记录配置入口的 standby、重复尝试和残留会话拒绝。末尾分别报告 receiver、SMI、rate 守卫或 `step=configured`。
- 起点 `57e4454a`。静态比对确认配置函数的 31 个原有 `cam_*` 调用的源码顺序一致。`sweet_x9_camera.o` 单对象交叉编译 `compile_one=True`、exit 0、181 条 warning。路径 `x9/out/camera-native-config-step-20261118/`。当前交付范围为静态复查和单对象编译。356 的 MM_2 final STA 超时仍是首个实机阻塞。

#### 2026-11-18 相机静态复查 · MM_2 acquire 失败后的 VCM 收尾

- 356 `:6086–6090` 先报 MM_2 final STA 超时，后报 `VCM RELEASE ... positions=1/1 modes=1/1 closed=0 errno=60`。计数来自 VCM init。收尾的第一次模式读经 `cam_sensor_ap_guard()` 命中已经锁存的 MM_2 错误。板级 `camera_af_main@0c` 使用独立的 I²C client 和 `vin-supply`。Android `ak7316m_24085.c:123/206/316` 在镜头 close 先归位并写 `0x02=0x20`，随后归还该驱动的供电引用。
- VCM release 和 lens open 模式恢复在同一 receiver epoch 的 MM_2 acquire 失败且尚未进入 CSI 配置时，使用原有 AP 供电、时钟、引脚和 FIFO 检查。close 入口继续校验采集退役及 `session.capture`。普通 sensor 事务和进入后续 CSI 阶段的 close 继续校验 receiver。`X9 CAMERA SENSOR CLOSE` 记录实际收尾步骤、原始故障和收尾返回值。MM_2 的失败票仍由共享 power authority 保留。
- 起点 `fb77d106`。`sweet_x9_camera.o` 单对象交叉编译 `compile_one=True`、exit 0、181 条 warning。路径 `x9/out/camera-vcm-mm2-close-20261118/`，无 CI run。最新运行证据仍为 356。VCM 归位与收尾结果待实机日志。

#### 2026-11-18 相机静态复查 · MM_2 IRQ 历史

- `clkchk-mt6993.c:3964` 和 `hwccf_dbg.c:259` 的 Android 调试表包含指针 `0x124cc`、14 个地址字 `0x124d0..0x12504` 和 14 个数据字 `0x12508..0x1253c`。`x9_mm_power.c` 在 MM_2 GET final STA 超时后读取该窗口。先输出 `X9 MM POWER GET`，再输出 `X9 MM POWER IRQ HISTORY` 的两个原始指针和 14 组原始寄存器值。两次指针读值变化说明采样期间有更新，读值相同也不能排除环回。寄存器表没有提供记录的编码规则，暂以槽号保存原始值供对照。
- 起点 `d647bebd`。`x9_mm_power.o` 单对象交叉编译 `compile_one=True`、exit 0、两条 XNU 导出头 warning。复现脚本和日志在 `x9/out/camera-mm-irq-history-20261118/`。本轮交付源码和单对象，无 CI run 或实机启动结果。356 仍是最新的运行基线，尚缺 `int_raw`、末次 STA 样本及 IRQ 历史值。

#### 2026-11-18 相机静态复查 · MM SMMU 电源分支与 MM/SMI 时钟投票

- live DT `smi-dbg` 带 `hwccf-support`（`:12622`），因此 Android `mtk-smi-dbg.c:1534` 跳过 `mtk_smmu_set_pm_ops(MM_SMMU, …)`。`iommu@30800000` 选择 `mt6993_data_mm`（`mtk-smmu-v3.c:3187/3235`），该配置无 `SMMU_CLK_AO_EN`。源码的实际分支是 `mtk_smmu_power_get/put()` → `mtk_smmu_pm_get/put()` → secure PM。日志 350 的 native 旧 SMC 路径在 `:5173` 已报 `errno=5 smc=c2000514/0`。现有 Android 记录仍缺 MM_2 首笔 0→1 的调用者与完整投票序列。
- `mtk-mminfra-util.c` 的 AO/infra0 和 `mtk-vmm-notifier-mt6993.c` 的 VMM buck/AP 位走 v1 IRQ BACKUP。`clk-mt6993-pd.c` 的 CAM VCORE/MAIN/MRAW、`clk-mt6993.c` 的 CAMTM/SENINF 和 `clk-mt6993-cam.c` 的 CAM/CAMSV gate 走 v1 raw MM voter。`mm_cam_sel` 用 `mtk_mux_generic_hwv_al_ops`，无 enable/disable 回调。`x9_camera_mm.inc` 分开使用 IRQ AP EN 期限 1250 µs（间隔 2 µs）和 raw AP EN 期限 200 µs（间隔 1 µs）。IRQ global EN 是 10001 次以内的先读、等待、判断循环，raw global EN、pre/final STA 与 raw FSM 使用 10000 µs 期限。IRQ BACKUP 和 raw MTCMOS CLR 在 AP EN 清除成功后等待 100 µs。`MM VOTER` 失败行记录 slot/op/step、末次采样、掩码和目标值。
- live DT 的 CAM subcommon APB 与 larb14/19/29 SMI clocks 对应 CG35/34。`mtk-smi.c:693` 使用 `clk_prepare_enable()`，对应物理 gate 的 `mtk_clk_gate_generic_mm_hwv_ops`。`x9_camera_smi_clocks.inc` 的 SET 现按 raw voter 的 AP EN → global EN → final STA 顺序确认，再进入物理 gate 路径。CLR 在 AP EN 后确认 final STA。`SMI VOTER` 记录各等待阶段的末次采样。native 继续使用已有分组掩码、借票和依赖守卫。
- 起点 `89a90e0a`。仅交叉编译 `sweet_x9_camera.o`，`compile_one=True`、exit 0、181 条 warning，产物与复现脚本在 `x9/out/camera-mm-slot-poll-20261118/`。本轮为静态复查，无镜像、CI run 或设备操作。实机仍以 356 `:6086` 为准，MM_2 final STA bit3 忙，`errno=60`。在这个前置条件通过后，依次核对 CSI、MM/SMI、SMMU、SOF/DONE、有效 RAW、iOS handler 和可见预览。

#### 2026-11-18 相机静态复查 · CSI_BS_RX 投票期限

- live DT `seninf-core` 的 `clk_hw_ccf_csi_bs` 指向 clock ID `0x58`。`clk-mt6993-pd.c:331` 将其连到 `csi_bs_rx` 的 MM MTCMOS bank1 bit11。`clk-gate.c:416/481/1011` 经 `v1_raw_hwccf_voter_ctrl()` 进入 `_v1_mm_hwccf_voter_ctrl()`。`hwccf_api.c:313` 使用 `regmap_read_poll_timeout_atomic()`，AP EN 的总期限为 200 µs，pre-STA、FSM、global EN 和 final STA 均为 10000 µs，各以 1 µs 为轮询间隔。CLR 等 AP EN 清除成功后才执行 100 µs settling，再查 STA。
- `x9_camera_resources.inc` 的 CSI GET/PUT 对齐这些期限、末次采样和 CLR settling 顺序。失败日志 `X9 CAMERA CSI POWER` 记录 `step`、`errno` 和导致超时的原始采样。`timeout_off=0` 表示没有原始 HWCCF 轮询超时采样。`PM_ACK` 按 native 原有供电守卫检查。CSI 使用通过 MM_2 校验的 token，SET/CLR 操作 XPU0 bank1 bit11。
- 356 的 CSI 步骤尚未执行。`cam_mm_take()` 在 `cam_receive_power_take()` 的 MM_2 acquire 返回后停止。紧接的 VCM close 先通过退休接口检查，再经 `cam_sensor_ap_guard()` 调用 `cam_rx_sensor_guard_locked()`。`cam_mm_session_check()` 返回之前锁存的 MM_2 `errno=60`，使 release 在第一次读 VCM mode 的守卫处退出。356 的 `positions=1/1 modes=1/1` 与该路径一致，计数属于之前的 VCM init。
- 起点 `bffc8e19`。`sweet_x9_camera.o` 单对象交叉编译 exit 0、`compile_one=True`、181 条 warning。脚本与日志在 `x9/out/camera-csi-mtcmos-deadlines-20261118/`。实机基线仍为 356 的 MM_2 stage 4。CSI 变更的实机结果待 MM_2 完成后取得。
- `mtk_cam-seninf-drv.c:835/850/916` 通过本机 `hwccf-apply` 选择的 HWCCF PM 循环在启用 CAM/SENINF 寄存器时钟前处理 `clk_hw_ccf_csi_bs`，并打印时钟开关结果。native 启动失败现在即时打印 `X9 CAMERA RX OPEN`。MM 失败还会在关闭前打印已有 `X9 CAMERA MM` 与有效 `MM SLOT` 样本，`receive_stage=` 标出 MM_2、MM_INFRA、CSI 或 SMMU 快照步骤。日志读已有 journal。起点 `917f75f1` 的诊断变更再次通过 `sweet_x9_camera.o` 单对象交叉编译，exit 0、`compile_one=True`、181 条 warning，见 `x9/out/camera-rx-open-log-20261118/`。

#### 2026-11-18 相机静态复查 · 记录 MM_2 超时的末次采样

- OPPO GKI `regmap.h:152` 用期限到达后的最后一次 `val` 决定轮询返回值。native 之前只记录超时后重新读取的 `ap/global/sta`。`x9_mm_power.c` 现在将导致 IRQ 轮询超时的实际样本记为 `timeout_off/timeout_raw`。字段来自原有读操作，`timeout_off=0` 表示这次调用没有 IRQ 轮询超时样本。GET stage 4 超时时应同时核对 `timeout_off=1135c` 的 `timeout_raw` 和后续 `sta` 快照。若两者 bit3 不同，说明该位在作出超时判定后至诊断采样之间变化。
- 起点 `88dd73c8`。仅交叉编译 `x9_mm_power.o`，`compile_one=True`、exit 0、两条既有 XNU 导出头 warning。复现脚本和日志在 `x9/out/camera-mm-timeout-sample-20261118/`。最近实机仍为 356，该版仅有事后 `sta=8` 快照。

#### 2026-11-18 相机静态复查 · 首帧失败定位

- ISP8s `mtk_cam-job.c:1456/1477` 先调用 `mtk_cam_sv_dev_stream_on()`，再调用 `ctx_stream_on_seninf_sensor()`。`mtk_cam-sv.c:2278` 对启用的 tag 逐一开流。native 入口 `start()` 先完成 `hw.capture(on)`，再写 sensor `0x0100=1`。`cam_irq_begin_locked()` 在现有路径上依次布置 DMA 和 CAMSV 请求，发布 IRQ，提交 FBC credit，再执行接收与源切换。
- `X9 CAMERA NATIVE PREPARE` 新增 `step=`，可区分 standby、RAW layout、source/rate guard、控制线程、SMMU 初始化或重开、映射导出。`NATIVE FIRST ARM` 和 `IRQ ARM` 报告初始控制、DMA 检查、CAMSV 请求、GIC 路由与解屏蔽、FBC credit、接收与 source transition 的首错。`X9 CAMERA: start` 附带失败回调的名称。硬件操作和失败保留路径按原顺序执行。
- cdev `cam_read()` 在已有读期限到达时记录 `RAW WAIT`，限同一代第一次仍无 RAW 的超时。iOS worker 对同一物理会话的每个输出和实际错误各记一次，覆盖 PTS metadata、pixel buffer lock/unlock、RAW 渲染和 metadata 复制及挂载。原有 buffer pool 与 layout 报告继续覆盖各自失败。`first picture given` 对应真实 handler 调用。
- `NATIVE CONFIG` 的 `completed` 是 `%x` 位图。`x9_camera_native_source.h:6` 定义阶段 1–8，因此完整配置行应为 `stage=9 completed=1fe errno=0`，更正旧条目的 `completed=1`。356 在 `stage=1 completed=0 errno=60`，未进入本轮补齐诊断的首帧阶段。
- 起点 `7b61e448`。只交叉编译 `sweet_x9_camera.o` 和 iOS `camera_device.o`，两者 exit 0。前者 `compile_one=True`、182 条既有 warning，后者无诊断。首次 kernel 单对象编译记录保存在 `compile-initial.log`。最终日志、复现脚本和对象位于 `x9/out/camera-first-frame-stages-20261118/`，无 CI run。本阶段仍为静态复查，最新实机日志仍是 356。MM_2 最终 STA 的失败原因仍缺运行证据。

#### 2026-11-18 相机静态复查 · GLB_EN 采样顺序与 Android MM_2 运行记录

- `hwccf_api.c:1007` 的 `wait_done()` 每次先读 `GLB_EN`，再 `udelay(1)`，然后判断该次读值。`x9_mm_power.c` 的 GET stage 3 已按该顺序执行，最多仍为 10001 次采样。stage 4 继续要求 fresh `STA bit3=0`，单调期限为 10000 µs。356 已通过 stage 3，现有日志无法验证这项时序修正是否影响 stage 4。
- Android `mtk-mminfra-util.c:130/355` 在 HWCCF 成功后单次检查 MM_2 ACK，失败时打印并 dump。native stage 5/6 则是 token 发放前的 MM_2 ACK 与 infra0/AO 守卫，`cam_receive_power_take()` 还会校验 token 和父链，再进入 CSI/SMMU。两级守卫保留在 stage 4 之后。live DT 的 `mminfra-util` 无 `mminfra-vlp-base`，源码的可选 VLP `irq_rdy_bit` 只有带该基址时才在 shutdown 清除。
- 原厂留存 `~/x9-android-dmesg.txt` 从 uptime `15.555922 s` 起，`:897` 记录 GCE0 secure context setup，`:3931` 记录显示 C3D 的 CMDQ flush 调用点。主 checkout 的 `x9/out/android-incoming-20261115-192820/dmesg.txt:245` 还保留 MM_2 `max_latency=[1]ms in [1348]ms`，可确认该次 Android 启动早期已有 MM_2 引用转换。主 checkout `x9/out/call-owned-runtime/dmesg.txt:2147` 的 MM_2 user 累计值显示 CG_LINK `[0]`、CMDQ `[1]`、SMI `[5]` 都有运行使用记录，类型定义见 `mtk-mminfra-util.h:10`。现存记录未给出首笔 0→1 的调用者，也未找到可对照 356 的 Android 开相机 runtime-resume 序列。
- 起点 `7b08c87c`。`x9_mm_power.o` 单对象交叉编译 `compile_one=True`、exit 0，两条 XNU 导出头 warning。复现脚本与日志在 `x9/out/camera-mm-global-read-delay-20261118/`。本轮为静态复查，实机基线仍为 r44 的 `.local/x9/logs/356.txt:6086`。下一验收点仍是最早 `X9 MM POWER GET` 的 `stage/sta/errno/int_raw`。取得 stage 4 完成证据后再沿 MM clocks → SMI/SMMU → CAMSV SOF/DONE → RAW → iOS handler → 实际预览核验。

#### 2026-11-18 相机静态复查：MM_2 IRQ 轮询单位与 Android 启动调用者

- Vendor `hwccf_api.c:776/993` 的 pre-STA、EN、final STA 调用 OPPO GKI `include/linux/regmap.h:152` 的 `regmap_read_poll_timeout_atomic()`。第五参数是 `ktime_add_us()` 的微秒期限，第四参数是 `udelay()` 的步长。纠正上轮 `1250×2 µs` 的解读：EN 为 **1250 µs 总预算，2 µs 间隔**，pre-STA/final STA 为 **10000 µs 总预算，1 µs 间隔**。`GLB_EN` 用独立显式计数循环。`x9_mm_power.c` 现在用单调期限和期限到达后的最后一次采样实现这三处 regmap 轮询。GET 在 dummy read 后先检查已投 EN，借用票仍进入 global/STA 检查。SET 前保留 fresh EN 检查，只有自己发出的 SET 才登记为自有票。
- `clkchk-mt6993.c:5163` 的 `check_mm_hwv_irq_sta()` 读取 `0x50` 并用 `0x07fd07fd` 判定是否触发 dump。`X9 MM POWER GET/PUT` 同一份只读样本保留 `int=`（bit5）并新增 `int_raw=`，可区分 BACKUP1 timeout 的 VIP/AP bit5/21 和 vote-before-ready 的 VIP/AP bit9/25。一次失败后样本不能确定这些状态首次置位的时间。`irqhis=` 只有环指针，单独不能识别投票者。
- live DT `mminfra-debug` 的 `mminfra-bkrs=0` 使 `mtk-mminfra-debug.c:1389` 读取 `vcp-gipc`。`:1584` 的 `init-clk-on` 在该配置只增加 `clk_ref_cnt` 并打印日志。GCE0 则配置 `gce-fast-mtcmos`、`ao-ctrl-by-mminfra` 和 `mminfra-all-on-pwr-idx=3`。Android `mtk-cmdq-mailbox-ext.c:764/795/3900` 的本地 mailbox 路径在相应首次引用时申请 AO 和 MM_2，并在末次释放。两者属于不同的引用。源码未给相机第一次 MM_2 acquire 增加常驻 AO 投票的依据，Android 当次实际首个 MM_2 调用者也未由日志确认。
- 起点 `e388a373`。最终仅交叉编译 `x9_mm_power.o`，`compile_one=True`、exit 0、两条 XNU 导出头 warning。复现脚本、日志和 exit 文件在 `x9/out/camera-mm-irq-deadline-static-20261118/`。首次脚本因输出目录尚未创建而停在 Builder 初始化，保留为 `compile-setup.log`。本轮无 CI run、无新镜像或设备操作。
- 已有运行证据仍以 `.local/x9/logs/356.txt:6086` 为准。该次 AP EN 与 GLB EN 均通过，final STA bit3 未清。时间单位修正影响轮询实现，尚无运行结果。MM 时钟、SMI/SMMU、CAMSV SOF/DONE、有效 RAW、iOS handler 和画面仍待实测。下次只在用户允许实测时，先检查最早 `X9 MM POWER GET` 的 `stage/sta/errno/int_raw`。


#### 2026-11-17 相机静态复查：351–356 主链重放、MMIO/HWCCF 初始化排除（不出镜像）

- **范围/边界**：用户要求“继续看351/352/353/354/355/356、完全按安卓流程、不出探针镜像”。本轮只读检查相机 worktree、351–356日志、`hwccf_api.c/hwccf.h`、`mtk-mminfra-util.c`、live DT和 native 电源/相机源码；没有上机、没有 boot/reboot/flash/ADB、没有运行测试脚本、没有构建 boot.img，也没有改电源/时钟/相机源码。
- **351–356按时间重放**：351 `:7001` 首错是 WL 总线 `AP REQUEST errno=60 ... status=0 start=ffffffff`，EEPROM `read=3246/0 errno=60`，尚未进入MM；352 preload已 `read=3456/3456`，但逐寄存器apply在 `:9754` 到 `writes=1562 acked=1562 applied=0 errno=60`；353 burst后 `:9932` `writes=2004 acked=2004 applied=0 errno=60`；354 `:5829` 的18B FIFO写形状触发 `errno=22`，`writes=2/acked=1`；355首次 `apply writes=250/acked=250 applied=1`、`mode=1/1`和`VCM INIT complete=1`全通，错误前进到 `:6122` MM_2 `stage=4 sta=8 errno=60`；356在 `:6086` 稳定复现同一MM失败（新增 all_sta/fsm/mt0sta/mt1sta 只读字段）。没有出现绕过或跳过MM的日志。
- **MMIO映射排除**：live DT `mm_hwv: syscon@31c00000` `reg size=0x74900`；native `x9_mm_power.c`映射`0x31c00000+0x22000`、`0x31a04000+0x1000`、`0x31b50000+0x1000`。HWCCF路径用到的最大XPU偏移`0x2072c`、诊断偏移`0x13704/0x12900/0x10000`，MMUP dummy read `+0x1ec`与ACK `+0xa8`均落在对应映射内；没有基数或映射长度不足导致的误读。
- **HWCCF probe排除**：`hwccf_api.c:1494..1563`的`hwccf_drv_probe()`只把两个syscon转成regmap、映射MMUP GPR、注册raw/is_enabled/FSM retrigger回调并设置`_inited=true`，不写任何HWCCF控制寄存器；`HWCCF_INIT_DONE_REG`在参考树内没有实际probe使用点。因此 native 没有漏掉一段Linux probe执行的硬件初始化。Android的v1 IRQ voter也只做`nowait`(STA prepoll→SET→EN)+`wait_done`(GLB_EN/STA)，失败仅assert并返回；`CCF_BACKUP1_VIP_STA_CLR`和`CCF_INT_STATUS`只有寄存器定义，源码没有把它们纳入这条成功路径，不能清W1C或伪造成功。
- **SMI caller关系补充**：`mtk-smi.c:2783`的`mtk_smi_mt6993_disp_resource_ctrl()`也只调用`mtk_mminfra_on_off(MM_PWR_MM_2, MM_TYPE_SMI)`；larb/common resume在`4228..4272/6005..6025`只把该调用包在时钟开关外，不额外投BACKUP1 bit3或操作`0x1135c`。日志355/356尚未到CSI/MM时钟阶段，MM_2失败不是SMI larb漏掉资源控制。
- **只读诊断增加EN6（无镜像）**：`v1_hwccf_freeze()`会写`HWCCF_REG_EN6(0x152c)`的`block_voting(bit16)`，参考树没有任何正常调用点；为避免把冻结/阻塞投票误判为普通bit3 busy，仅给`X9 MM POWER GET/PUT`加只读`en6=...`字段，不改投票/等待/STA判据，不清W1C，不调用freeze/unfreeze。单对象`compile_one(sweet_x9_camera.o)` exit0/True/182既有warnings，日志在`x9/out/camera-mm-time-static-r2-20261117/compile-en6b.log`与`compile-en6b.exit`；没有整包/boot.img、未上机。
- **ACK/父链符号核对**：`x9_mm_power.c`的`ack2=c2f10152 & 0xc0000000`等于`0xc0000000`，`infra=e3fc0000 & ((1<<31)|(1<<29))`也全为1；这与DT中MM_2 `mm-mtcmos-base=0x31b500a8/mask=0xc0000000`以及`CCF_MTCMOS_PM_ACK_0(0x12900)`中AO/infra0位要求一致。日志没有显示父ACK缺失，问题仍只在BACKUP1 `STA bit3`；`ack2`的其它位保留原值解释，不清/不改。
- **ATF VM/secure bank排除**：复看原ATF `21424/215cc/37f4/385c/3988`：VM型资源`0x38/0x39`按`w0`选择AP `0x1c040000`或secure `0x31c30000` bank；MM BACKUP0 `0x37`始终用MM `0x31c00000`。`37f4`先经`385c`完成SET/EN/global/STA，成功后只有VM型(`w19=1`)在`21564`写`0x31c3748=2`，MM BACKUP0失败或成功都绕到`215b0`返回，不写该命令；此前对MM路径`0x31c3748=2`的笔记表述应以此为准。native没有少做VM/secure bank写，不能据此增加寄存器操作。
- **ATF 385c轮询闭合与只读INT**：`385c`在VOTE/UNVOTE前读`w0+w24`检查AP EN；SET后经`218b0`写SET/CLR，再按资源类型以1250×2us或10000×1us等AP EN；随后`3988`只等传入的第二个状态寄存器`w19`（VOTE为global EN，即MM BACKUP1 `0x11358`）置位。VOTE路径在`3988`之后没有再次轮询STA；此前“ATF也最终等STA”的旧表述不准确，应以native/Android v1 voter的`wait_done()`为仍需STA idle的依据。为区分硬件已报backup1 timeout与单纯状态未清，只给MM日志增加`int=...`读`CCF_INT_STATUS(0x50)&BIT(5)`，不写W1C、不改投票/等待/STA判据。单对象编译exit0/True/182既有warnings，日志`x9/out/camera-mm-time-static-r2-20261117/compile-int.{log,exit}`；未出镜像、未上机。
- **`CCF_INT_STATUS`使用面复查**：`hwccf.h:143..167`定义`0x50`的`bit5=backup1_timeout_int_to_vip`和`bit21=backup1_timeout_int_to_ap`；参考树内`hwccf_dbg.c:172`、`clkchk-mt6993.c:3572/3876`只把该寄存器放进debug dump，v1 IRQ voter的nowait/wait_done都不读或清它。新增的`int=`只是下一次实机日志的只读诊断，不能据此清W1C、改STA成功条件或宣称已确认硬件超时；现有356日志仍无该字段。
- **BACKUP_GRP0 共享bit审查（静态）**：`hwccf_api.c:1190..1221`只把resource55映射为同一组`XPU_B0_SET/CLR/EN/STA/GLB_EN/DONE`，不保存调用者身份；`mtk-mminfra-util.c:113..125`的MM_2只用DT `vote_bit=3`，但`mtk_vdisp.c:427`、`mtk-vmm-notifier-mt6993.c:343/392/444/468/477/764/774/1640/1725/1734`也以`hwccf_irq_voter_ctrl/multi_voter_ctrl`对同一组投AP bit0、VMM CAM/IMG/IPE bit14/15/16及buck/VMM组合位。日志356的`ap/global/sta=8`只显示bit3，没有日志把这些其它XPU位当成失败；源码也不允许从`STA bit3`反推出共享FSM被其它投票卡住。native保留“只CLR自己新增bit3、借票不动”的现有策略，不新增VMM/CG47/DSU动作。
- **stale MTcmos FSM retrigger排除**：`_v1_mm_hwccf_mtcmos_fsm_err_handle`只接受`GET_ID_FROM_SET_ADDR()`识别的CG资源（0..51），BACKUP SET `0x20724`返回`0xffffffff`并bypass；`_v1_mm_hwccf_mtcmos_fsm_err_handle_2`要求`mtcmos_sta_0/1 != 0 && all_mtcmos_sta_0/1 == 0`，而356为`mt0sta=mt1sta=all_sta=0`。因此不能用CG47 retrigger解释或修复`STA bit3`，native不补这段动作。
- **VMM CAM/IMG/IPE投票顺序定位（静态）**：`mtk_cam.c:6027/6111`在`mtk_cam_runtime_suspend/resume`里调用`vmm_disable/enable_cvfs(..., VMM_CVFS_CAM_SEL)`；`mtk_imgsys-sys.c:2903/2904/2960/2961`和`mtk_imgsys-cmdq.c:204/205/261/262`对IMG/IPE做同样成对投票；`vmm_enable_cvfs()`最终经`hwccf_irq_voter_ctrl/multi_voter_ctrl`投BACKUP_GRP0的bit14/15/16。它们属于CAM/IMGSYS runtime PM阶段，而日志356在MM_2 `x9_mm_power_acquire()`就返回errno60，尚未进入`X9 CAMERA NATIVE CONFIG`后续时钟/SMI/CAMSV阶段；不能为越过MM_2提前补这些VMM票。`mtk_vdisp.c`的bit0 DPC显示路径同理不是本失败点。
- **只读IRQ history指针诊断（无镜像）**：`hwccf_dbg.c:259`把`0x124cc`列为`HW_CCF_VOTE_IRQ_HIS_INDEX_POINTER`，它后面的`0x124d0..0x1253c`是IRQ addr/data history；`hwccf_dump`也只读这些寄存器。为避免下一次日志只能看到最终`sta=8`，给`X9 MM POWER GET/PUT`再加只读`irqhis=mm_read(0x124cc)`，用于观察失败时硬件环形历史指针；不读取/清除W1C，不写history，不改投票/等待/STA判据。单对象`compile_one(sweet_x9_camera.o)` exit0/True/182既有warnings，日志`x9/out/camera-mm-time-static-r2-20261117/compile-irqhis.{log,exit}`；未出镜像、未上机。
- **交接状态同步**：本阶段累计提交 `fd587284`（`int=` timeout只读位）、`da9e8fc7`（共享BACKUP_GRP0/retrigger排除）、`9399f2e0`（VMM CAM/IMG/IPE投票时序）、`6ff5ea02`（`irqhis=`只读指针），均已推送到 `origin/feat/camera-integration`；三次文档改动之外只有 `x9_mm_power.c` 加只读诊断，没有电源成功条件/投票/等待语义变化。`CAMERA_PLAN.md`顶部“源码待提交”已随 `6ff5ea02` 纠正。
- **ATF `3988`逐指令复核纠正（只读）**：用Capstone按原ATF物理地址重新反汇编 `0x3988`：它先按resource类型决定是否延时100us（`39d0..39e4`），然后读取第二状态寄存器到`w22`（`39e8..39f4`），按resource类型比较该寄存器**置位**或**清零**（`39fc/3a24`的`bics wzr,w20,w22`），接着在`3a30..3a4c`以`0x2710`次1us等待`w19`的`w20`位清零，超时返回`-0x1002`。`37f4`把`w4=0x1135c/STA`直接作为`3988`的`w19`，所以ATF VOTE路径确实最终等STA bit3清零；此前“3988只等global EN/没有再轮询STA”的表述错误。结合`385c`已经等过AP EN/global EN，`3988`的`w21=w5`是STA地址、`w19=w4`是STA mask，global EN只由`385c`传入的`w22`参数轮询一次。纠正不改native流程：native本来就在stage4要求STA bit3清零；Android v1 voter与ATF在这一点一致，仍不清W1C、不延长、不绕过。
- **ATF bit3投票条件与native差异（观察，不改流程）**：逐指令复核`28d48/28f08`调用点：`28d48`只在`ref_cnt`从0变1、且读到`w8 bit1=1`并设置`0x1325dc bit0`后才以`w0=1,w1=0x37,w2=0(HWCCF_VOTE),w3=3`调`21424`；`28f08`只在`ref_cnt`减到0且`0x1325dc bit0`仍置位时才以`w2=1(UNVOTE)`调同一函数，随后`21424`成功路径`/失败且type=1`会写`0x31c3748=2`或`0x31c3748=1`，并清`0x1325dc bit0`。这说明ATF并非每次camera power请求都投票，也不在已有引用时重复投票；native首次parent会直接写AP BACKUP1 SET/CLR。但这些差异发生在ATF自身引用计数/secure flag条件下，当前日志没有这些ATF状态，不能据此把native改成“等ATF投票”或清flag；native继续按Android mminfra2参考计数直接投bit3并要求STA clear。
- **距离正确画面的精确进度**：已通过传感器ID `9186/0615`→sensor-open→EEPROM preload 3456/3456→calibration apply/mode→VCM init；未完成MM_2 `STA bit3` clearing→MM clocks/SMI/SMMU→CAMSV/IRQ/SOF/DONE→RAW staging→iOS handler→可见预览→画面正确性。日志351–356无任何`completed=1`的NATIVE CONFIG、CMDQ启动、真实SOF/DONE/RAW、handler或画面证据。下一步仍只在源码/日志内解释`ap=8/global=8/sta=8`，不延长/绕过STA、不清W1C、不加额外投票、不出镜像、不反复上机。

#### 2026-11-17 相机静态复查：FSM高位排除，SMMU/CAMSV后续阶段闭合（不出镜像）

- **范围/边界**：继续不出探针镜像、不上机、不 OTA/ADB/flash/reboot。只读复查`hwccf.h`、`hwccf_api.c`、原ATF `37f4/385c/3a70/3ba4`、DPC `0x31c10000`用法、Android `arm-smmu-v3.c`、`mtk_cam_sv_ddren_qos_coh_config()`、SENINF runtime_resume和native对应阶段；没有改源码。单对象`compile_one(sweet_x9_camera.o)`复核`exit=0/True/182既有warnings`，脚本/日志在`x9/out/camera-mm-time-static-r2-20261117/`，没有整包/boot.img。
- **`fsm=80000001`排除**：`hwccf.h:64..67`只定义`CCF_STATUS bit0=idle`；`hwccf_api.c:325`判据为`(val & 0xffff)<=1`。DPC v3对同一`0x31c10000`实测掩位为`value & 0xfffe`，注释是“all fsm idle”；bit31不在这两种判据里，原ATF `3b84`对`0x31c10000`也只看`0xfffe`。因此日志低16位=1满足idle，bit31不能解释为BACKUP1失败，也不能触发清W1C/额外投票/延长等待。
- **ATF低级判据补充**：`37f4/385c`的VOTE路径仅等AP EN→写HWV命令→等global EN→等STA；资源地址若属于CG/MTCMOS/MUX，另有`3a70/3ba4`的附加等待，但BACKUP GRP0只走`385c`。这再次证明native对MM_2 bit3没有少做ATF类型资源的附加步骤，`sta=8`仍是唯一确定的未完成条件。
- **SMMU阶段静态闭合**：Android `arm_smmu_device_reset()`在`arm-smmu-v3.c:3977`先写`CR0=CMDQEN`并等CR0ACK，随后`CFGI_ALL`、可选`TLBI_EL2_ALL`、`TLBI_NSNH_ALL`，再开EVTQ/SMMU。`x9_camera_native_smmu.inc:236..257`按`CMDQEN → CFGI/CD → TLBI → CMDQEN|EVTQEN → 全使能`实现，并在提交命令前要求`CR0ACK.CMDQEN`。这不是缺少代码；没有新实机日志时不能说SMMU已经启动成功。
- **CAMSV QoS/时钟静态闭合**：`x9_camera_sv_qos.inc`已按ISP8s `mtk_cam_sv_ddren_qos_coh_config()`的SW-mode顺序写DDR SET/OR_CQ、COH SET/OR_CQ、BW_QOS SET/OR_CQ，记录原始RMW、`SW_R`只作观察，并要求两次OR_CQ采样；`x9_camera_sv_reset.inc`已按ISP8s resume的CQ stop/ACK与MRAW `0→3<<(id*2+4)→0`脉冲实现。SENINF runtime_resume源码要求先core PM、CAM/SENINF/CAMTG/CAMTM clocks、TSREC clock/common reg、vcore/byteclk/phya选择再disable all outmux；native `cam_mm_descs[]`/`x9_camera_native_clock.inc`/`x9_camera_native_rate.inc`分别有对应域/AL/MUX、实测时钟和带宽/电压资格检查。由于MM_2尚未通过，这些只是后续阶段无已知源码缺口的静态结论，不是已生效证据。
- **当前进度不变**：最早失败仍在MM_2 `stage=4 sta=8 errno=60`；不能把SMMU/QoS/SENINF的静态闭合当成越过MM、拿到RAW或显示画面。下一步继续只按源码/日志解释bit3 busy，不延长/绕过STA，不出镜像，不反复上机。

#### 2026-11-17 相机静态复查：ATF 21424/37f4/385c 的 HWCCF bit3 判据闭合（不出镜像）

- **范围/边界**：继续遵守不出镜像、不上机、不 OTA/ADB/flash/reboot；本轮只读反汇编原ATF `tee.img.atf.original.bin`中的`28d48/28f08 → 21424 → 37f4 → 385c`，并对照`hwccf.h`的`HWV_XPU_0=0/CCF_XPU()`和`x9_mm_power.c`。没有改源码、没有编译、没有构建、没有操作设备；只更新三份handoff并 commit/push。
- **ATF地址映射已闭合**：对`type=1`（AP）选用MM HWV基址`0x31c00000`；resource `0x37=B0(BACKUP_GRP0)`时，VOTE(`w2=0`)写`base+0x30724`，UNVOTE(`w2=1`)写`base+0x30728`，并把`base+0x11358/+0x1135c/+0x1136c`作为global EN/STA/DONE传给`37f4`。`CCF_XPU(HWV_XPU_0)+XPU_B0_SET/CLR`按`hwccf.h`正是`0x20724/0x20728`，与native `MM_POWER_SET/CLR`及Android v1 wrapper一致。`28d48/28f08`使用的都是AP XPU0 bank，不是固件私有XPU3 bank。
- **ATF等待判据**：`37f4/385c`先写AP SET/CLR，再最多`0x2710(10000)`次每次1us等AP EN bit3；随后`218b0`写`0x31c3748=2`触发HWV FSM；再最多10000次等`+0x11358`的bit3；最后最多10000次等`+0x1135c`的bit3清零。`DONE`虽作为参数传入（`w4=0x1136c`），这条AP BACKUP路径没有轮询DONE；`STA`清零才是完成条件。超时分别返回`-0x1000/-0x1001/-0x1002`；成功返回0。`28f08`丢弃`21424`返回值并最终置`w0=0`，所以外层PUT的0不能证明内部成功。
- **与native的逐项结论**：native `mm_vote_take()`同样在SET后等AP EN `1250×2us`，在global EN和STA处各`10000×1us`，最终把`sta=8`判为ETIMEDOUT；没有遗漏ATF/Android的额外HWV写。`218b0`的FSM触发是写SET/CLR后由HWCCF API在硬件内部处理的过渡，native没有、也不应另造一个任意CMD寄存器写。ATF路径没要求DONE，native不轮询DONE正确；ATF路径同样要求STA清零，native不能忽略或绕过。
- **当前唯一确定失败仍是硬件状态**：日志356 `ap=8/global=8/sta=8/done=fffffff7/all_sta=0/fsm=80000001/mt0sta=0/mt1sta=0/errno=60`说明AP EN和global EN已到、最终STA bit3未清；Android v1 IRQ voter与ATF原始实现都会在此判失败。没有源码依据增加额外投票、清W1C、改bank、延长10ms或跳到RAW。下一步仍按静态复查追“为什么同一bit3在AP/global已置后仍保持busy”，并只在源码明确时补错误判定或缺失代码。

#### 2026-11-17 相机静态复查：重复票语义与 ATF bit3 事务收束（不出镜像）

- **范围/边界**：继续按用户要求只做静态复查，不出探针镜像、不上机、不 OTA/ADB/flash/reboot。只读复查了`hwccf_api.c`的三套 voter、`mtk-mminfra-util.c`、live DT、`x9_mm_power.c`、日志351–356，以及原ATF中已记录的`28d48/28f08/21424`路径；本轮没有改源码、没有编译、没有运行测试、没有操作设备。只更新`NOTES.md`、`CAMERA_PLAN.md`、`x9/camera/README.md`三份handoff。
- **重复投票语义纠正**：前一条handoff写“Android重复投票直接goto skip且跳过后续等待”不严谨。`_v1_hwccf_irq_voter_nowait()`的`goto skip`只跳过nowait里的dummy read之后的prepoll/SET/EN；`_v1_hwccf_irq_voter_ctrl()`随后无条件调用`_v1_hwccf_irq_voter_wait_done()`，后者仍执行ON的`all_en`与`STA`等待。`_v1_mm_hwccf_voter_ctrl()`同样在重复票时`goto skip`，但它是一条完整voter ctrl，不拆分nowait/wait_done。放宽投票的时钟路径不应把这两套skip混为一谈；当前native“EN已置则借用并继续global/STA”在`wait_done`语义上更接近irq voter组合，不能据“跳过后续等待”再改代码。
- **三套源码路径的失败判据收束**：`_v1_irq_nowait`在SET前要求`STA bit3=0`，SET后等AP `EN bit3=1`；`_v1_irq_wait_done`在ON时等`GLB_EN bit3=1`，再等`STA bit3=0`，不等`DONE`。`_v1_mm_voter_ctrl`在is_set时额外要求`CCF_STATUS & 0xffff <= 1`，SET/EN/all_en/STA顺序与irq组合相同。`_v1_hwccf_irq_voter_ctrl`失败只assert+返回，BACKUP的CG47 retrigger bypass。日志356的`ap=8 global=8 sta=8 done=fffffff7`在任何一套路径里都会停在最终STA，不是等待步长或重复票语义造成的假失败。
- **ATF路径原始数据**：对原ATF `tee.img.atf.original.bin`做只读反汇编，`28d48`在first reference时以`w0=1,w1=0x37,w2=0,w3=3`调用`21424`，即`HWCCF_VOTE/BACKUP_GRP_0/bit3`；`28f08`在last reference时以`w0=1,w1=0x37,w2=1,w3=3`调用同一函数，即`HWCCF_UNVOTE/bit3`。`28d48`只在`w0`的bit31置位时转发错误；`28f08`在`bl 21424`后直接恢复`w0=0`返回，因此外层PM返回0不能证明内部HWCCF成功。这个源码事实与356的`ap`仍置位吻合：可能是先前事务遗留或当前硬件状态未清，但不能仅据返回0断言取消仍能成功。
- **ATF限制/不能推出的结论**：原始反汇编没有提供`21424`的C源码级错误语义，也没有显示它会清除或修正`CCF_BACKUP1_STA` bit3；不能把“有ATF投票”当成额外补救、不能读/写ATF私有XPU3 bank、不能延长/绕过native最终STA。native只投AP XPU0 bit3，与Android `mminfra2_onoff()`一致。ATF返回的bit31检查只说明错误转发很窄，不能据非零低bits推断成功或失败类型。
- **阶段B静态边界**：`clk-mt6993-pd.c`给出`cam_vcore(bit16,parent=mm_infra0) -> cam_main(bit15) -> cam_mraw(bit8)`，CAM/CAMSV下级再挂在其后；live DT的`seninf-core`列出SENINF/CAM/CAMTG/top/clk_hw_ccf_csi_bs/mm_dvfs_mux，camsv1列出`cam_main_larb19/14/29_con`、`camsv_top_con`、`seninf_camtm_con`、`camsv_a_con`及三个中断。现有`cam_mm_descs[]`已有16/15/8域和寄存器访问级CAM/CAMSV CG；这些是后续阶段清单，不是已生效证据。当前还没有MM成功，不能把阶段B/C/D写成已通过；下一步仍先在源码层面解释硬件`STA bit3`为何10ms未清，不能跳到RAW或补画面数据。

#### 2026-11-17 相机静态复查：按Android修正MM IRQ vote步长，不出测试镜像

- **范围/边界**：用户明确要求“不出探针镜像，现在只开始静态复查，严格按照安卓日志和源码跑一遍我们的启动/上电/预览，遇到可能出错的地方就打印对应错误，不用反复测试；缺失代码严格按安卓逻辑补全”。本轮只读核查351–356日志、`hwccf_api.c/hwccf.h`、`mtk-mminfra-util.c`、`mtk-vmm-notifier-mt6993.c`、live DT和相机native启动/上电/采集链；只改一处源码常量语义，没有编译boot.img、没有生成探针镜像、没有boot/reboot/flash/ADB/设备操作、没有跑测试脚本。
- **源码排除**：`MMUP_CORE_R_GPR11_ADDR=0x31a041ec`与native `mmup=0x31a04000+0x1ec`一致；`HW_CCF_BACKUP_GRP_0(55)`经v1 wrapper映射到`XPU_B0_SET=0x20724/CLR=0x20728/EN=0x2072c/STA=0x1135c/GLB_EN=0x11358/DONE=0x1136c`，bank未选错。v1 IRQ voter不使用`CCF_STATUS` idle判据，FSM idle只属于`hwccf_voter_ctrl`；`fsm=80000001`低16位为1仍满足idle判据。VMM对本组投票使用bit10/11/14/15/16/18..22，`VMM_VOTE_VAL=0x3c00`不含bit3；日志`ap/global/sta=8`只落在mminfra2的MM_2 bit3。`_v1_mm_hwccf_mtcmos_fsm_err_handle`只对CG资源生效，BACKUP资源直接bypass，不能套CG47 retrigger。
- **确定修正**：`x9/kernel/src/bsd/dev/x9_mm_power.c`把`mm_wait`拆成`count`与`step_us`；BACKUP1 IRQ vote/unvote的EN等待从`1250*1us`改为Android `MTK_WAIT_GHWV_IRQ_VOTE_CNT=1250`/`MTK_WAIT_GHWV_IRQ_VOTE_US=2`，其它prepoll/global/STA/ACK等待保持`10000*1us`。未延长最终STA预算、未忽略STA、未清DONE/W1C、未改投票顺序、未新增额外父域投票。曾短暂尝试“已投EN则借用并继续检查”，复核`_v1_hwccf_irq_voter_nowait`发现Android在重复投票时直接`goto skip`且跳过后续等待，已撤回该偏离。
- **静态编译检查**：仅单对象`compile_one(sweet_x9_camera.o)` **actual exit0 / True，182既有warnings**，脚本`x9/out/camera-mm-time-static-r1-20261117/kernel-unit.py`，控制台`x9/out/camera-mm-time-static-r1-20261117-console.log`。没有整包、没有输出供测boot.img；GPU/mailbox不涉及。
- **预览链当前判断**：356的`NATIVE CONFIG stage=1 ... errno=60`是MM_2失败的传播，不是新的SMI/CAMSV故障。`cam_pipeline_plan()`已按DT/source给出主摄VC1/前摄VC0、DT=0x2b、CSI port/stride/IOVA范围；当前ABI的completed frame没有VC/DT字段，不能凭空补ABI。后续继续静态检查的重点是SENINF/CAMSV时钟与resume顺序、SMMU/S1实际写表和CAMSV IRQ/DONE路径；所有新增判定必须有Android源码对应。

#### 2026-11-17 相机主链继续：日志356闭合r44诊断，MM_2仍停在STA未清

- **起点/范围**：用户要求切到相机分支、按handoff继续并“完全按照安卓流程”。工作树在`97124a0f` tracked clean；`~/356.txt`只读保存到`.local/x9/logs/356.txt`（不入git）。本轮只读核查351–356、`hwccf_api.c/hwccf.h`、`mtk-mminfra-util.c`和native电源代码，没有改上电流程、编译、构建、跑测试、boot/reboot/flash、操作ADB/分区或设备。
- **运行观察**：`:741` `NATIVE REGISTER errno=0 IDs=9186/0615`；`:5255` EEPROM两遍读取`read=3456/3456 preloaded=1 errno=0`；`:5823` `phase=apply ... writes=250 acked=250 applied=1 errno=0`；`:6081` `phase=mode ... mode=1/1 errno=0`与`VCM INIT ... complete=1 errno=0`。这与355相同，继续确认校准FIFO消息形状和VCM init通过；ACK仍只是事务收据，不证明光学效果。
- **356的r44新证据/最早失败**：`:6086` `X9 MM POWER GET: client=1 backend=hwccf MM_2 bit=3 stage=4 owned=1 held=0 ap=8 global=8 sta=8 done=fffffff7 ack2=c2f10152 infra=e3fc0000 all_sta=0 fsm=80000001 mt0sta=0 mt1sta=0 errno=60`，紧接`:6087` `NATIVE CONFIG ... stage=1 completed=0 errno=60`、`:6088` VCM release errno60。失败仍是`x9_mm_power.c:mm_vote_take()` stage4的`CCF_BACKUP1_STA` bit3在10ms末尾未清；`all_sta=0/mt0sta=0/mt1sta=0`排除了普通MTCMOS0/1 status显示“嵌套电源未完成”的解释。
- **源码判定**：`hwccf.h:64..67`只把`CCF_STATUS` bit0定义为`hwccf_idle`；`hwccf_api.c:325`的FSM判据是`(val & 0xffff) <= 0x1`，所以`fsm=80000001`的bit31不在该判据内，低16位为1仍是idle，不能把bit31单独当失败。`hwccf_api.c:1190..1239`确认BACKUP_GRP0使用`XPU_B0_SET/CLR/DONE/STA/GLB_EN`；`:776..870`为pre-poll STA→SET→poll EN；`:993..1050`为poll GLB_EN→poll STA清；`:1093..1125`把两步串在同一个v1 ctrl内。`v1_hwccf_irq_voter_ctrl`失败后只`configASSERT(0)`返回，不做额外补救。注册在v1 ops的`_v1_mm_hwccf_mtcmos_fsm_err_handle`（`:1314..1412`）在`GET_ID_FROM_SET_ADDR`非CG或超过`HW_CCF_CG_GRP_51`时直接bypass，CG47 retrigger不适用于本资源；因此不新增该流程。结论：native顺序/地址/预算与源码一致，硬件STA未清是下一步的唯一确定失败点，不延长、不忽略STA、不清W1C、不改投票顺序。
- **当前进度**：已完成传感器ID→sensor-open→EEPROM preload→calibration apply/mode→VCM init。未完成MM_2→MM clocks/SMI/SMMU→CAMSV/IRQ/SOF/DONE→RAW staging→iOS handler→可见预览→画面正确性；目前仍不能宣称预览成功。下一步继续围绕`0x1135c/0x20724/0x2072c/0x11358/0x1136c`和v1 voter源码查污染/资格，不把诊断候选称为MM修复。

#### 2026-11-17 相机主链继续：MM_2源码对齐与可判定诊断（r44整包成功，未上机）

- **范围**：用户说「继续」后，只读对照`hwccf_api.c:776..1125/1190..1239`、`hwccf.h:50..55/116..124/697..707`、`mtk-mminfra-util.c:55..126/250..360`与live DT。确认`mediatek,mt6993-mm_hwv`使用v1 ops；`HW_CCF_BACKUP_GRP_0(55)`映射`XPU_B0_SET=0x20724`、`B0_EN=0x2072c`、`B0_DONE=0x1136c`、`B0_STA=0x1135c`、`B0_GLB_EN=0x11358`，与native地址一致。v1 IRQ voter顺序是pre-poll STA bit3清→写SET→poll EN→wait_done先poll all_en→再poll STA清；native `x9_mm_power.c`已按此顺序，10ms预算也与`MTK_WAIT_GHWV_*`同量级。355的`ap=8 global=8 sta=8`就是最后STA未清；`done=fffffff7`只作诊断，Android该路径不把DONE当成功条件。因此本轮没有延长、忽略STA、清DONE或改投票顺序，避免偏离源码。
- **实现/边界**：仅给`X9 MM POWER`日志增加Android判定所需的只读观测：`all_sta`(`CCF_ALL_MTCMOS_STA_0=0x13704`)、`fsm`(`CCF_STATUS=0x10000`)、`mt0sta`(`0x1131c`)、`mt1sta`(`0x11334`)，从同一已映射`mm_hwv`读取。没有改变任何电源/时钟/寄存器写入或等待语义，也没有扩大映射范围。下一份日志应能区分`XPU_B0_STA` bit3、整体MTCMOS STA、FSM与MTCMOS0/1状态，仍失败时只按新增状态继续源码对照。
- **编译/构建实绩**：正常Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，182 warnings**，脚本`x9/out/camera-mm-diag-compile-r1-20261117/kernel-unit.py`、日志`x9/out/camera-mm-diag-compile-r1-20261117-console.log`、exit文件`compile.exit=0`。固定源码后正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-mm-diag-r44-20261117` **actual exit0**，104/104 overlay +3 prebuilt，Image32247808 B / boot15110144 B。唯一候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-mm-diag-r44-20261117/boot.img`**，原控制台`x9/out/camera-mm-diag-r44-20261117-console.log`。GPU280无需mailbox；默认后/前双摄，未加`x9_camera_lenses=1`。本机无CI run ID；未上机，目标未完成。
- **下一上机判定**：用户从主checkout手动fastboot boot r44并打开Camera.app。若MM_2仍errno60，新日志重点看`X9 MM POWER GET ... sta=... all_sta=... fsm=... mt0sta=... mt1sta=...`。只按这些真实值与Android `v1_hwccf_irq_voter_nowait/wait_done`及`hwccf_is_enabled`的条件继续，不把诊断候选称为MM修复。

#### 2026-11-17 相机主链继续：日志355，校准链闭合，失败前进到MM_2（源码无新改动）

- **起点/范围**：用户把误写的255更正为355。工作树`b2fff359` tracked clean；`~/355.txt`已只读保存到私有`.local/x9/logs/355.txt`（不入git）。本轮只读核查日志与MM上电源码，没有改源码、编译、构建、运行测试、操作设备或触碰共享输入；构建的r43候选仍是最新供测版本。
- **355运行证据**：`:747` `NATIVE REGISTER errno=0 IDs=9186/0615`；`:4677`主摄sensor-open完成；`:5310` EEPROM preload `read=3456/3456 ... preloaded=1 errno=0`。**新的通过里程碑**：`:6042` `phase=apply ... writes=250 acked=250 ... applied=1 errno=0`：r43按Android `2B reg+14B payload`、总16B FIFO消息形状完成3456B校准写；`:6116` `phase=mode ... mode=1/1 errno=0`；同一行`VCM INIT ... complete=1 errno=0`。这证明之前的逐字节事务和18B消息偏离均已修正；ACK仍不是光学校准或物理SRAM可读证明。
- **新的最早失败**：`:6122` `X9 MM POWER GET: client=1 backend=hwccf MM_2 bit=3 stage=4 owned=1 held=0 ap=8 global=8 sta=8 done=fffffff7 ack2=c2f10152 infra=e3fc0000 errno=60`，紧接`:6123` `NATIVE CONFIG ... stage=1 completed=0 ... errno=60`，VCM release errno=60、DRAIN restore=60 latched=1、start errno=60。没有本次MM之后的clock/SMI/SMMU/CAMSV/SOF/DONE/RAW/handler或真实画面证据；日志无唯一构建标识，不能独立核定源码SHA。
- **当前定位**：`x9_mm_power.c:mm_vote_take()`在stage4执行`mm_wait(hwv, MM_POWER_STA, MM_POWER_BIT, 0, 10000)`；日志的`sta=8`表示10ms等待结束时bit3仍置位，因此返回ETIMEDOUT。stage1的初始STA idle已过、AP SET已写、AP EN与global EN均为8，说明单调到STA idle这一步，不是旧的`c2000514` SMC失败，也不是校准/I2C失败。源码对照显示`mtk-mminfra-util.c:mminfra2_onoff()`实际只调用`hwccf_irq_voter_ctrl(MM_HWCCF, voter55, HWCCF_VOTE, bit3)`，而`mtk_mminfra_is_on()`读取的是DT `mm-mtcmos-base/mask`；本机当前把SPM HWV的SET/EN/global/STA与MM2 ACK组合成多个阶段，与源码的voter完成语义并非同一件事。这里区分**观察**（sta=8导致stage4 timeout）和**推断**（阶段模型可能偏离hwccf voter完成语义）；不把推断当实机结论。
- **本轮处理/下一步**：由于用户明确要求按源码和日志、不猜测，本轮没有继续改MM电源。当前候选仍为`x9/out/x9-camera-cal-burst-fifo-r43-20261117/boot.img`（已push源码`b2fff359`，本机整包exit0）。若继续修MM，必须先逐行对齐`hwccf_api.c`的`_v1_hwccf_irq_voter_nowait/_wait_done`与`mminfra2_onoff`的真实投票/等待/ACK路径，再出一个新候选；不能仅延长10ms或忽略STA。

#### 2026-11-17 相机主链继续：日志354，回到Android FIFO消息形状（r43整包成功，未上机）

- **起点/范围**：用户指定「354」并明确要求完全按Android流程、不猜测。工作树从`4d2f6757` tracked clean起步；`~/354.txt`已只读保存到私有`.local/x9/logs/354.txt`（不入git）。只读核查日志、源码与Android参考；未触主checkout、设备、ADB、分区或共享`.local`输入；未运行测试脚本/套件，构建成功后没有解包/hash/反汇编等二次检查。
- **354运行证据**：`:974` `NATIVE REGISTER errno=0 IDs=9186/0615`；`:4877`主摄sensor-open完成；`:5325` `SENSOR CAL phase=preload ... read=3456/3456 ... preloaded=1 ... errno=0`。`:5829`（21.241s）`SENSOR CAL phase=apply ... block=0 offset=0 reg=c000 writes=2 acked=1 ... applied=0 errno=22`，随后OPEN RESTORE errno=0、DRAIN与`start sensor=1 errno=22`。没有本次MM/SMMU/RAW/SOF/DONE/handler或真实预览证据；日志无唯一构建标识，不能独立核定源码SHA。
- **确定源码错误（不是硬件猜测）**：r42的`cam_cal_write_seq()`按Android的`msg.len=2+payload`想一次写满16B payload，但native `cam_bus_transfer_n()`入口仍保留写方向`count<=4`限制，于是18B消息在START之前直接返回`EINVAL`。这解释了`writes=2/acked=1`且没有对应AP WAIT/REQUEST；不是传感器NACK或总线失败。`adaptor-i2c.c:358..413`的`adaptor_i2c_wr_seq_p8()`每次是单个写消息，`msg.len=2+cnt`、`reg += cnt`；`i2c-mt65xx.c:1967`规定`msgs->len<=fifo_size(16)`走FIFO，超过才DMA。之前把payload上限设成16使`msg.len=18`，偏离了本机FIFO路径。
- **实现/边界**：写方向传输允许完整16B `msg.len`（WRRD仍保留4B TX前缀与16B RX限制）；校准顺序写定义`CAM_CAL_WR_PAYLOAD=CAM_I2C_FIFO_BYTES-2=14`，每笔正好是`2B寄存器+14B payload`、最大16B，末块按剩余长度，寄存器按payload字节数前进，不跨block。严格保持已拥有AP总线、控制/FIFO/terminal/W1C守卫，不启用DMA/APDMA/SCP，不重试、不写EEPROM、不扩大寄存器范围；`writes/acked`仍计事务数，ACK不是光学校准证明。
- **编译/构建实绩**：正常Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，182 warnings**，脚本`x9/out/camera-cal-burst-fifo-compile-r1-20261117/kernel-unit.py`、日志`x9/out/camera-cal-burst-fifo-compile-r1-20261117-console.log`、exit文件`compile.exit=0`。固定工作树修改后正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-cal-burst-fifo-r43-20261117` **actual exit0**，104/104 overlay +3 prebuilt，Image32247808 B / boot15110144 B。唯一候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-cal-burst-fifo-r43-20261117/boot.img`**，原控制台`x9/out/camera-cal-burst-fifo-r43-20261117-console.log`。GPU280无需mailbox；默认后/前双摄，未加`x9_camera_lenses=1`。本机无CI run ID；仍未上机，目标保持未完成。
- **下一上机判定**：用户从主checkout手动fastboot boot r43并打开Camera.app。新日志先看`SENSOR CAL phase=apply ... writes≈249 acked≈249 applied=1 errno=0`（mirror+QSC+payload+SPC/LRC+enable事务数）与`phase=mode ... errno=0`，再看`VCM INIT`/`NATIVE CONFIG ... completed=1 errno=0`，随后才按MM/SMMU/RAW/SOF/DONE/handler推进。若仍失败，只按真实日志中首个errno/AP状态定位，不继续改软件预算或启用DMA。

#### 2026-11-17 相机主链继续：日志353，校准sensor写入改顺序burst（r42整包成功，未上机）

- **起点/范围**：用户指定「353」继续主线。工作树从`4f0413ee` tracked clean起步；`~/353.txt`已只读复制到私有`.local/x9/logs/353.txt`（不入git）。只读核查日志、源码与Android参考；未触主checkout、设备、ADB、分区或共享`.local`输入；未运行测试脚本/套件，构建成功后没有解包/hash/反汇编等二次检查。
- **353运行证据**：`:785/:808`主/前摄ID `9186/0615`，`:810` `NATIVE REGISTER errno=0`；`:4634`主摄sensor-open完成；`:5488` `SENSOR CAL phase=preload ... read=3456/3456 ... preloaded=1 ... errno=0`。`:9932`（33.952s）`SENSOR CAL phase=apply ... writes=2004 acked=2004 ... applied=0 errno=60`，随后OPEN RESTORE errno=0、DRAIN与`start sensor=1 errno=60`。没有本次MM/SMMU/RAW/SOF/DONE/handler或真实预览证据；日志无唯一构建标识，不能独立核定源码SHA。
- **确定差异/推断**：r41已给apply独立15s预算，但apply在约17.2s内只完成2004笔单寄存器写，全部ACK后才因预算到点失败。Android `hhmainmipiraw_Sensor.c:3040..3083`的`set_sensor_cali`使用`subdrv_i2c_wr_seq_p8`；`adaptor-i2c.c:358..413`实现为16-bit起始寄存器加连续payload字节的单条I2C消息，每批最多`MAX_VAL_NUM_U8=253`，由MTK I2C控制器在超过16B FIFO时走DMA。native当前把每个校准字节拆成`reg[2]+value[1]`一笔，且每笔完成观察含`delay(5)`与多次状态/控制守卫，导致软件开销主导。选择**观察**：2004/2004 ACK与15s预算结束；**推断**：逐字节事务开销是未能完成的主因，不把它当作硬件ACK失败。
- **实现/边界**：新增`cam_cal_write_seq()`，只用于校准payload：保留`cam_cal_guard()`、sensor session/epoch/live状态、既有AP总线所有权与控制守卫，按Android形状一次发送16-bit起始寄存器加最多`CAM_I2C_FIFO_BYTES=16`个连续payload字节，不跨校准block，不用DMA/APDMA/SCP，不写EEPROM，不扩大寄存器范围。`cam_sensor_forbidden()`仍拒绝stream/hold/OTP/ID类寄存器；mirror与QSC enable继续走原单字节writer。日志的`writes/acked`现在计**FIFO事务数**而非payload字节数，全部ACK仍不是光学校准或物理SRAM可读的证明。apply独立15s预算成功/失败后恢复configure截止的语义保持；失败仍首错即停、不重发、不掩盖错误。
- **编译/构建实绩**：正常Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，182 warnings**，脚本`x9/out/camera-cal-burst-compile-r1-20261117/kernel-unit.py`、日志`x9/out/camera-cal-burst-compile-r1-20261117-console.log`、exit文件`compile.exit=0`。固定工作树修改后正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-cal-burst-r42-20261117` **actual exit0**，104/104 overlay +3 prebuilt，Image32247808 B / boot15110144 B。唯一候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-cal-burst-r42-20261117/boot.img`**，原控制台`x9/out/camera-cal-burst-r42-20261117-console.log`。GPU280无需mailbox；默认后/前双摄，未加`x9_camera_lenses=1`。本机无CI run ID；仍未上机，目标保持未完成。
- **下一上机判定**：用户从主checkout手动fastboot boot r42并打开Camera.app。新日志先看`SENSOR CAL phase=apply ... writes≈217 acked≈217 applied=1 errno=0`（mirror+QSC+payload+SPC/LRC+enable的事务计数；不是旧3458字节计数）与`phase=mode ... errno=0`，再看`VCM INIT`/`NATIVE CONFIG ... completed=1 errno=0`，随后才按MM/SMMU/RAW/SOF/DONE/handler推进。若burst产生`AP REQUEST`/NACK/EIO，应保留真实失败状态，不继续盲目加大预算或启用DMA。

#### 2026-11-17 相机主链继续：日志352，校准apply独立预算（r41整包成功，未上机）

- **起点/范围**：用户指定「看352」并继续剩余部分。相机工作树从`8951dfb4` tracked clean起步；`~/352.txt`已只读保存到私有`.local/x9/logs/352.txt`（不入git）；只读检查源码、日志与现有文档。未触主checkout、设备、ADB、分区或共享`.local`输入；未运行测试脚本/套件，构建成功后没有解包/hash/反汇编等二次检查。
- **352运行证据**：`:753` `NATIVE REGISTER errno=0 IDs=9186/0615`，`:4610`主摄sensor-open完成。`:5260` `SENSOR CAL phase=preload ... read=3456/3456 ... preloaded=1 ... errno=0`，说明r40的AP FIFO等待与EEPROM 16B分块修复已越过351的preload阻塞。`:6337` `AP WAIT: gate=1 slave=35 tx=1 rx=1 polls=2 elapsed_us=54960 budget_us=50000 status=00000001 start=00004000 bus=WL`：terminal状态在54.96ms被观察到，START已停止，资格检查后继续，不再把`ffffffff`占位当失电。最早失败转移到`:9754`（31.972s）`SENSOR CAL phase=apply ... writes=1562 acked=1562 preloaded=1 applied=0 errno=60`，随后`:9755` OPEN RESTORE errno=0、`:9756` DRAIN、`:9757` `CAMERA: start sensor=1 errno=60`；本次没有MM/SMMU/RAW/SOF/DONE/handler或实际预览证据。日志无唯一构建标识，不能独立核定源码SHA。
- **失败定位**：`cam_sensor_configure()`用同一个15s `io->deadline`覆盖init table、calibration apply与preview table。352的preload在17.173s完成，apply约14.8s后失败；所有1562次sensor写均已ACK，才在常见writer的ACK后预算检查返回ETIMEDOUT。该时间线与共用预算耗尽一致，不是新的WL/I2C硬件失败；apply附近也没有新的AP WAIT/AP REQUEST失败行。这里区分**观察**（ACK计数/时间线）和**推断**（预算到点），不把后者当作硬件寄存器读数。
- **实现/边界**：`cam_cal_apply()`在原有live epoch/session/preload资格通过后，先保存configure截止，以独立15s `end`设置`io->deadline`；所有校准SRAM写、mirror与enable都在该预算内，成功或失败都恢复原configure截止，preview/readback仍按原预算运行。独立预算不足仍返回ETIMEDOUT、保留真实ACK计数与依赖，不重试、不改寄存器范围、不动EEPROM、不掩盖写失败，也不把ACK当光学校准通过。`cam_sensor_configure()`的注释同步说明三阶段各有独立预算；校准preload仍保持原有15s与两遍完整payload/header校验。
- **编译/构建实绩**：正常Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，182 warnings**，脚本`x9/out/camera-cal-deadline-compile-r1-20261117/kernel-unit.py`、日志`x9/out/camera-cal-deadline-compile-r1-20261117-console.log`、exit文件同目录`compile.exit=0`。固定工作树修改后执行正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-cal-deadline-r41-20261117` **actual exit0**，104/104 overlay +3 prebuilt，Image32247808 B / boot15110144 B。唯一候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-cal-deadline-r41-20261117/boot.img`**，原控制台`x9/out/camera-cal-deadline-r41-20261117-console.log`。GPU280无需mailbox；默认后/前双摄，未加`x9_camera_lenses=1`。本机无CI run ID；仍未上机，目标保持未完成。
- **下一上机判定**：用户从主checkout手动fastboot boot上述r41并打开Camera.app。新日志先看`SENSOR CAL phase=apply ... writes=... acked=... applied=1 errno=0`与`phase=mode ... errno=0`，再看`VCM INIT`/`NATIVE CONFIG ... completed=1 errno=0`，随后才按MM/SMMU/RAW/SOF/DONE/handler推进。若apply仍errno60，应记录新的writes/acked与绝对时间来判断是否真的超过独立15s，不能沿用本次预算解释或猜测I2C reset。

#### 2026-11-17 相机主链继续：日志351，AP FIFO等待及EEPROM分块修正（r40整包成功，未上机）

- **起点/范围**：用户指定看`~/351.txt`，随后继续未完成的相机主线。相机工作树起点`08a894f8` tracked clean；原始日志已只读复制到私有`.local/x9/logs/351.txt`（不入git）。未触主checkout、设备、ADB、分区或共享`.local`输入；未运行测试脚本/套件，构建成功后没有解包/hash/反汇编等二次检查。
- **351运行证据**：`:801` `NATIVE REGISTER errno=0 IDs=9186/0615`；`:4492`主摄ID成功，`:4530` `SENSOR OPEN ... AF=c prior=20 mode=0 issued/acked=1/1 complete=1 errno=0`。最早失败在`:7001` `AP REQUEST gate=1 errno=60 status=0 start=ffffffff fifo=1 request=1`，紧接`SENSOR CAL phase=preload ... read=3246/0 bytes=3456 pass=0 block=1 offset=174 reg=7cce errno=60`，随后`OPEN RESTORE errno=22`、`DRAIN restore=22 retained=1`。没有该次MM_2、SMMU、RAW、SOF/DONE或handler记录；`gate=1`是WL28681C所在i2c12守卫总线，`reg=7cce`只是校准进度而非失败WL寄存器。`start=ffffffff`是未采样START的日志占位，不是实际硬件读数，不能当作失电证据。日志没有唯一构建标识，不能独立核定其精确源码SHA。
- **Android对照与确定差异**：本机`live.dts`的i2c4/i2c2/i2c12均`mediatek,mt6991-i2c`，对应`i2c-mt65xx.c:737..748`的`mt6991_compat.fifo_size=16`。同一文件`:137`定义`I2C_POLLING_TIMEOUT=50ms`，`:2256..2265`在其FIFO polling路径循环读INTR_STAT并在超时后补最后一次采样；`:2355..2362`才从DATA_PORT逐字节取RX。原实现把完成观察限为5ms/1000次且先查绝对时间再读状态，线程调度恢复后可能丢掉已经粘住的terminal；校准读每笔仅2字节，使电源守卫读取次数增多。这些是源码确定差异，不把50ms、FIFO容量或旧日志解释为已证明硬件正常。
- **实现/边界**：`x9_camera_i2c_xfer.inc`的观察预算改为50ms，先读STAT再判断时间，退出补一次STAT；仅在真实terminal且停止START、稳定控制、其他bank/APDMA空闲、FIFO数量与本次RX长度精确相符后完成W1C/收尾。无terminal仍ETIMEDOUT并锁存，不做reset、resume、W1C、FIFO清理重试或把late status改成功。sensor/VCM调用仍保持原寄存器和1B RX范围；只有EEPROM按`CAM_I2C_FIFO_BYTES=16`分块，且不跨一个校准block。保留两遍完整payload、各轮前后header、QSC/SPC/LRC范围及失败不重发。`AP WAIT`在无terminal或观察耗时达到50ms时记录真实START、polls、elapsed_us和bus，`AP REQUEST`明示sensor/WL，避免再用`ffffffff`占位猜硬件。
- **编译/构建实绩**：正常Builder`compile_one(sweet_x9_camera.o)` **actual exit0/True，182 warnings**（session83450终态），日志`x9/out/camera-i2c-poll-fifo-compile-r2-20261117-console.log`；r1为加bus标识前的同类通过记录。固定工作树修改后执行正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-i2c-poll-fifo-r40-20261117` **actual exit0**（session87184终态），104/104 overlay +3 prebuilt，Image32247808 B / boot15110144 B。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-i2c-poll-fifo-r40-20261117/boot.img`**，原控制台`x9/out/camera-i2c-poll-fifo-r40-20261117-console.log`。GPU280无需mailbox；默认后/前双摄，未加`x9_camera_lenses=1`。本机无CI run ID。
- **提交/上机判定**：固定源码 **`ac41d1d2ff93721e7b6fb46abddd9285464c8ea2`** / tracked clean，待push。用户从主checkout手动fastboot boot上述r40并打开Camera.app，新日志重点看：`AP WAIT ... status=... start=... elapsed_us=... bus=WL/sensor`、`AP REQUEST ... errno=0`或资格失败、`SENSOR CAL phase=preload ... read=3456/3456 ... preloaded=1`，再按`VCM INIT`→`NATIVE CONFIG`→MM/SMMU→RAW/handler顺序推进。即使这些通过也只说明离开已定位的AP/校准阻塞，成功预览仍需实际画面与对应运行证据；整体目标保持未完成。

#### 2026-11-17 首次预览主链复核：沿用 r39，下一步取得新运行日志

- **本轮范围**：在`feat/camera-integration`、`35023dc7` clean起点只读核查首次预览必经代码，未继续扩展FOCUS/AF、录像或新增硬件配置。未改主checkout、共享`.local`输入或设备，未运行测试脚本/套件，也未重复构建、解包/hash或检查成功镜像。本轮只有本段交接及`CAMERA_PLAN.md`更新，无新源码候选/CI run ID。
- **审查记录（源码事实，非运行通过）**：检查`start()`的power→configure→capture→0100→core STREAMING发布；`cam_native_capture_prepare/capture`的独立S1 namespace与首request；`cam_irq_begin_locked`的cache→地址/FH→FBC→GIC→单RCNT→receive/TG/source顺序；冷reset的post样本更新、repeat/AE的no-credit边界、IRQ worker等待core发布与SOF/DONE/FBC/cache交付；core staging/header peek与完整read，再到bridge的帧校验、软件ISP、输出admission和真实PixelBufferHandler。另核build.py每次编入camera bridge及PID1安装/mediaserverd注入路径。该审查范围内未得到新的确定代码缺陷，不能将“未发现”写成整条硬件链已通过。
- **最新运行仍旧**：按Mac`~/[0-9][0-9][0-9].txt`实际mtime重新检查，最新仍`350.txt`（2958194 B，2026-11-17 13:06:33本地时间）。原日志`:1219`为4.566s `NATIVE REGISTER errno=0 IDs=9186/0615`，`:5172`为16.300s `NATIVE CONFIG stage=1 completed=0 errno=5`；`:2797`记录camera包含在同一DYLD注入键。它只证明旧r27已到物理sensor配置入口，未提供r28b–r39的MM_2、SMMU、RAW、handler或预览结果。
- **下一具体动作**：候选仍为下面r39的完整绝对路径，GPU280无需mailbox，默认后/前摄。由用户手动`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-vcm-lifecycle-r39-20261117/boot.img`，打开Camera.app并移动画面，保存本次新`~/NNN.txt`及是否出画面的观察。收到后按**最早失败阶段**定位：MM POWER GET/ACK→CAMSV COLD RESET/CONFIG、MMQOS、NATIVE CONFIG→NATIVE SMMU started/CMDQ→FIRST ARM→真实SOF/DONE/RAW→`first picture given ... PTS=SOF=... DONE=...`。handler日志只说明回调已执行，成功预览仍须实际画面；没有新运行前不把AF旁支或反复出包当作首次预览进展，整体目标未完成。

#### 2026-11-17 相机主链继续：AK7316M/T normal lens生命周期与实际停流后的模式恢复（r39整包成功，未上机）

- **起点/范围**：上一goal turn属progress（SOF PTS代码、r38整包和push）。本轮单代理继续既有相机工作树/`feat/camera-integration`，`c8283cf5` tracked clean；最新实机仍`~/350.txt`，r28b–r38没有新MM/RAW/预览运行证据。未改主checkout/共享输入，未操作设备、ADB、boot或分区；未运行测试脚本/套件。
- **确定关闭缺陷**：`sweet_x9_camera.c::stop():193..207`在实际source-off/capture-off成功后只清`session.capture`，必须等`hw.power(false)`成功才清`session.stream`；它是attempt历史，不能当当前物理stream状态。r37的`cam_sensor_open_guard`却在这个power(false)内部无条件拒绝`session.stream`，即使完整DMA/translation退休也必返ESTALE。现仅close_attempted分支允许历史stream，且每次AP事务仍重新检查`cam_native_sensor_retirement_guard_locked(epoch)`及真实rails/pins/clock/receiver；原capture=true仍拒绝。未提前清历史或把布尔作为退休证明，不能据此声称r37在设备上已复现该失败。
- **Android可达依据**：`.local/references/oppo-modules-dt-mt6993/vendor/oplus/kernel/camera/lens/vcm/v4l2/ak7316{m,t}_24085/*.c`的normal`open→power_on→init`与`close→power_off→release`，不是另一路`set_setting`/NVM或注释DAC mode。main`:93..100/157..201/229..275`在VIN后5100us等待，首mode样本后3ms，读PID16（期望20，仅提示不同）、read_word84→swab16>>6原点、原g_last_pos511位置store，再02=00；tele`:98..117/176..211/239..282`的两份初始cache同511，goto diff0直接return，无position store。main`:433..441`与tele`:435..443`的V4L2 focus control默认0，**不是g_last_pos511**。release main`:123..154`/tele`:120..153`按focus->val每100步/1ms回0，仍无条件最后写0，再02=20；tele额外2800us。live DT i2c4/0c、i2c7/2d的VIN与当前3.0/3.1V rails相符，没有可选vcamaf pinctrl属性。Linux`drivers/i2c/i2c-core-smbus.c:378..385`确认WORD_DATA低byte先传，故source`swab16(val<<6)`线上为`00,(val<<6)>>8,(val<<6)&ff`，511为`00,7f,c0`。
- **实现/边界**：新增`x9_camera_vcm.inc`，mode2/mode3实际sensor配置末尾执行正常lens init，ID-only注册不动。独立1s初始化/关闭预算，窄地址/寄存器范围，全部issued在可能START前记账，ACK在后置guard/预算检查前留证；模式实际双采样，位置仅标记ACK，不猜可读DAC或机械到位。main真实PID/原点与tele缓存分别留read标志，不以PID不匹配自动写AE/03/NVM。AP读/写出错保留首错，不使用source的猜位置fallback/重发来掩盖不确定状态。没有放宽generic sensor/EEPROM writer。
- **关闭/归档**：原native实际退休通过、capture=false之后，先normal lens release/02=20（tele2800us），再sensor-open恢复借用原模式：原20只观察，原00/40唯一写回并等3ms。只有独立完整VCM close journal允许sensor-open把20当本次已知合法中间态，未知/partial不得覆盖或卸依赖。V4L2 control0与ACKed init DAC511分开保存，保持source无FOCUS命令时的release行为。journal随原exported epoch归档，成功recycle另检查完整init/位置ACK/mode/close，8个pin上限不变。append-only CFG_VCM，config720B/RAW120B/ioctl ABI不变；公开FOCUS与RAW仍NO_FOCUS，完整帧绑定/AF/OIS、front09/0c冲突、UW及光学校准继续未闭。
- **编译里程碑**：正常Builder`compile_one(sweet_x9_camera.o)` **actual exit0/True，183 warnings**（session17490终态），原日志`x9/out/camera-vcm-lifecycle-compile-r1-20261117-console.log`，输出`x9/out/x9-camera-vcm-lifecycle-compile-r1-20261117/compile.exit=0`。随后移两处声明、补phase guard和PID/原点read来源字段，由固定clean源码最终整包覆盖。下一正常layout检查、源码commit、唯一r39整包并push；本机无CI run ID。整体目标继续active，本轮尚无运行/对焦效果证明。
- **r39整包/推送终态**：固定源码 **`8640f9a3b73a4ca9d019d69dc9b6f3cc55751052` / tracked clean**，固定Python3.13正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-vcm-lifecycle-r39-20261117` **actual exit0**（session81319终态），104/104 overlay +3 prebuilt，相机最终182 warnings，原日志Image32247808 B / boot15110144 B。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-vcm-lifecycle-r39-20261117/boot.img`**，原控制台`x9/out/camera-vcm-lifecycle-r39-20261117-console.log`，输出目录保留`source-head.txt/source-status.txt/boot-input.json/build-command.txt/full-build.exit/build.log`。layout **actual exit0**，日志`x9/out/camera-vcm-lifecycle-layout-20261117.log`。源码push **actual exit0 `c8283cf5..8640f9a3`**（session20681终态）。本机无CI run ID，GPU280无需mailbox；默认后/前双摄，未加`x9_camera_lenses=1`；未上机、跑测试或二次检查成功产物。
- **下一运行/代码项**：用户从主checkout手动`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-vcm-lifecycle-r39-20261117/boot.img`，按最早MM_2/SMI/reset/QoS→SMMU/CMDQ→真实SOF/DONE/RAW/cache/handler推进。主摄应先有`VCM INIT ... pid=... read=1 expected_pid=20 origin=... read=1 control=0 DAC=511 known=1 positions=1/1 modes=1/1 complete=1 errno=0`；完整退休后的关闭再看`VCM RELEASE ... DAC=0 positions=2/2 modes=3/3 mode=20 closed=1 errno=0`→`SENSOR OPEN RESTORE ... restored=1`→`DRAIN ... restore=0 retained=0/0/0`。这些是期望日志，不是已观察结果；默认前摄没有此VCM init，不要求tele日志。后续代码沿原`set_position:93..100/88..95`的0..1023 absolute control接到CPU/no-credit边界、immutable request及完成metadata，不能让queued/ACK-only命令冒充机械到位；bridge旧AF含redwood/PM8008i范围/等待假设，不能直接启用或据此宣告本机AF实现。前/UW、OIS、光学校准、多摄同步/原生UI、硬件VENC及照片/视频验收仍未闭；整体目标active，本轮属于progress。

#### 2026-11-17 相机主链继续：媒体 PTS 沿用同请求 SOF（r38整包成功，未上机）

- **起点/范围**：单代理在既有相机工作树/`feat/camera-integration`，`588e6871` clean。最新实机仍`~/350.txt`；r28b–r37无新MM/RAW/预览运行证据。未触主checkout或共享`.local`输入，未运行测试脚本/套件或操作设备。
- **Android可达链**：`.local/references/oppo-modules-dt-mt6993/vendor/mediatek/kernel_modules/mtkcam/camsys/isp8s/cam/`中，`mtk_cam-sv.c:3643..3694`正常SOF handler取`ktime_get_boottime_ns()`并push FIFO，`:3870`thread交`mtk_cam_ctrl_isr_event(CAMSV)`；`mtk_cam-ctrl.c:1242..1302`CAMSV FRAME_START调用`ctrl_vsync_preprocess`，`:1010..1012`保存first SOF的`sof_ts_ns/sof_ts_mono_ns`，`:1095..1130`发VSYNC/SOF事件，`:648..675`复制runtime info给state；`mtk_cam-job.c:4358..4394`正常singleframe进入PROCESSING时将这两个SOF复制为job timestamp，`mtk_cam.c:1137..1138/993..1007`buffer done使用job时间而非重新取DONE。不是未调用的`job_utils::_set_timestamp`或其它硬件的推定。
- **确定缺口/修正**：当前native RAW v2已提供同请求真实SOF并保留DONE，worker也验证两者前进，但`camera_device.c`的metadata/handler/per-output单调门仍用DONE。现在统一使用`hakios_camera_sof_time(f)`，保留DONE头合同、双时间合法/单调检查、still not-before与跨冷切镜不复位的输出PTS门。没有生成时间、按帧率补点或倒算物理曝光开始；没有在未确认Apple sample-buffer时间合同前加入重定时。首handler新日志`PTS=SOF=... DONE=... host units`只在实际callback后输出。
- **间隔含义**：切镜`source_gap`现取前后确实调用handler的帧SOF；另记`done_gap`（旧镜头最后已读RAW到新handler帧的DONE）与`start_to_RAW`（新镜头选择完成后至该DONE）。不混作物理曝光/屏幕显示缺口，未调用handler的过渡RAW不更新source基线。
- **编译/其余核查**：正常`x9/camera/build.py --developer-dir <local.json> --vendor-root .local/vendor/oppo-camera-imgsensor-common --out x9/out/x9-camera-sof-pts-bridge-r1-20261117` **actual exit0**，bridge159616B，原控制台`x9/out/camera-sof-pts-bridge-r1-20261117-console.log`；只有原unused helper及static initializer warning。`prepare-local.py --check`通过，日志`x9/out/camera-sof-pts-layout-20261117.log`。S1 DEVICE交接CIVAC、完成后CPU交接IVAC及逐页cache line边界/DSB路径已读，未发现有确定源码证据的新cache修正。STG/CQI仍不凭统计或阈值表擅自启用；AF/OIS、ISP光学校准、原生多摄UI/同步源和硬件VENC仍未闭。下一固定clean源码r38整包，完成后补实际SHA/exit/路径/push；本机无CI run ID，整体目标仍active。
- **r38整包/推送终态**：固定源码 **`d7a572b84e715d982f53c77f62c059bc689c3154` / tracked clean**，固定Python3.13正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-sof-pts-r38-20261117` **actual exit0**（session31639终态），104/104 overlay +3 prebuilt，相机182 warnings，原日志Image32231424 B / boot15106048 B。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-sof-pts-r38-20261117/boot.img`**，原控制台`x9/out/camera-sof-pts-r38-20261117-console.log`，输出目录保留`source-head.txt/source-status.txt/boot-input.json/build-command.txt/full-build.exit/build.log`。源码push **actual exit0 `588e6871..d7a572b8`**（session81687终态）。本机无CI run ID，GPU280无需mailbox；默认后/前双摄，未加`x9_camera_lenses=1`，未上机/跑测试/二次检查成功产物。
- **下一运行/代码边界**：用户从主checkout手动`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-sof-pts-r38-20261117/boot.img`；先按最早MM_2 ACK→SMI/reset/QoS→SMMU/CMDQ→真实RAW/SOF/DONE/cache/handler检查。首次输出应出现`first picture given ... PTS=SOF=... DONE=... host units`，只证明实际callback，仍须移动画面/照片保存重开/视频封装验收。切镜双间隔仅适用于另有四摄准入的候选，默认双摄不要求tele或后置跨镜日志。源码后续可沿已定位AK7316M/T的真实VCM init/归位/位置控制检查，不能把02模式激活当FOCUS已实现；front地址冲突仍缺本机证据。没有新实机日志，整体目标继续active，本轮属于progress。

#### 2026-11-17 相机主链继续：补齐主/长焦 sensor open 的 VCM 模式激活（r37 整包成功，未上机）

- **起点/范围**：单代理继续相机工作树/分支，`544497bc` clean。上一回合属progress（MMQoS源码、r36整包与推送）。最新实机仍`~/350.txt`，r28b..r36均无新MM/RAW/预览结果；未触主checkout、共享`.local`输入、ADB/设备/分区，未跑测试脚本或套件。
- **主链核查事实**：当前开源`mtk_cam-sv.c`的STG路径只有reset、dump与寄存器定义，未见可移植配置写者；native持续检查central STG triggers为0、CQ SUB_THR0未enable，CQI阈值配置不代表CQ取指。不因Android动态统计里有stash/CQI就增加端口或推定完整STG生命周期。现有真实SOF/hybrid状态读取、FBC/CPU cache/worker→staging→软件ISP/PixelBufferHandler已接线，没有由这轮源码检查取得实际画面证据。
- **确定遗漏/真实顺序**：Android `hhmain_mipi_raw/hhmainmipiraw_Sensor.c:28/2990..3006`明确`HHMAIN_AF_WRITE_ID=18`、先i2c4/0c一字节reg02=00→3ms→get_imgsensor_id→sensor_init；`hhtele_mipi_raw/hhtelemipiraw_Sensor.c:27/2041..2057`为ID→i2c7/2d reg02=00→3ms→init。live DT `camera_af_main@0c:9175`、`camera_af_tele@2d:9420`确认地址与同bus，原native完全漏此调用。AK7316M/T驱动init实际读取reg02，正常代码使用00/20/40模式；sensor `.close=common_close`本身没有VCM关闭命令。**这只是确定的open调用遗漏，不证明日志350的旧MM电源错误或无画面由AF造成**。
- **实现/边界**：新增`x9_camera_sensor_open.inc`，独立固定main/tele helper复用原AP FIFO、rails/pins/clock/receiver守卫；不扩大通用sensor/EEPROM slave和寄存器范围。仅配置阶段按两者原顺序重读真实16/17 ID、发一次02=00并等待3ms；ID-only注册不触发VCM。实际模式稳定样本只接受源驱动使用的00/20/40，保存原值；attempt/issued在可能START前记账，ACK在后置预算检查前记录，观察active与整个open完成分开。1s软件总预算，源3ms等待不猜其它芯片时序；失败不重发或把晚到readback改成功。新增append-only `CFG_OPEN` stage，720B config/120B RAW/ioctl ABI保持。
- **关闭/归档**：原native实际退休guard与capture=false通过后，仍在sensor close标记、reset隔离、receiver/MM/rails恢复之前确认mode00并唯一恢复本次借用原模式；原本00只观察不写。此原值恢复是native资源journal约定，不冒充Android common_close原有VCM写者。mode命令不确定或恢复失败保留低层依赖；若激活完整观察后才发生sensor ID/table错误，可独立恢复模式但不清原错误。完整journal随exported epoch归档，recycle要求对应epoch/sensor和实际mode恢复，不放宽8个pin上限。没有位置、PID/NVM存储、前/UW猜地址或FOCUS能力；完整VCM init/归位、前摄09/0c冲突、AF/OIS与光学校准仍未完成。
- **编译里程碑**：正常Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，182 warnings**（session15835终态），日志`x9/out/camera-sensor-open-compile-r1-20261117-console.log`，输出`x9/out/x9-camera-sensor-open-compile-r1-20261117/compile.exit=0`。随后补窄helper的命令单次检查、稳定原模式采样和文档，由固定clean源码正常整包编译唯一r37；SHA/实际结果/push待补。本机无CI run ID，不复查成功产物。
- **r37整包/推送终态**：固定源码 **`306ee6953a5da6f9693c5d1e07f66a5b489e5b20` / tracked clean**，固定Python3.13正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-sensor-open-r37-20261117` **actual exit0**（session5609终态），104/104 overlay +3 prebuilt，相机182 warnings，原日志Image32231424 B / boot15106048 B。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-sensor-open-r37-20261117/boot.img`**，原控制台`x9/out/camera-sensor-open-r37-20261117-console.log`，输出目录保留`source-head.txt/source-status.txt/boot-input.json/build-command.txt/full-build.exit/build.log`。layout检查exit0，日志`x9/out/camera-sensor-open-layout-20261117.log`。源码push **actual exit0 `544497bc..306ee695`**（session9582终态），本机无CI run ID，GPU280无需mailbox。默认后/前双摄，未加`x9_camera_lenses=1`；未上机、跑测试或二次检查成功产物。用户从主checkout手动`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-sensor-open-r37-20261117/boot.img`；最新实机350不能混作r37运行结果。
- **下一验收/后续工作**：默认后/前准入，main应有`SENSOR OPEN ... sensor=1 AF=c issued/acked=1/1 id=9186 bytes=2 complete=1 errno=0`；原mode恢复单独看`SENSOR OPEN RESTORE ... restored=1 errno=0`。长焦同源顺序已编入，但默认双摄不要求tele日志。仍需用户新启动日志按最早MM_2 ACK→SMI/reset/QoS→SMMU/CMDQ→RAW/SOF/DONE/cache/handler推进；不能把本轮open补齐称完整代码或相机已完成。

#### 2026-11-17 相机主链继续：追到 ISP8s caller 的 KB/s 换算（r36 整包成功，未上机）

- **源码事实与修正**：沿`isp8s/cam/mtk_cam-dvfs_qos.c:435..440/789..792/1322..1357`追完整`calc_bw → to_qos_icc → mtk_icc_set_bw`，相机专用`mtk_cam-dvfs_qos.h:87..90`明确为`kBps_to_icc(Bps/1024)`，不是通用`Bps_to_icc(Bps/1000)`。r35已正确修枚举3/ratio9/乘3顺序，但caller换算仍应修正。native现按同源helper先/1024，再按MMQoS/1000，然后`ceil(mix_MBps/512)*3`，默认1/上限64；`max`可在此单路径单调取整前取，无需重新估计线时。完整RAW需求仍保留原preview最小帧周期/实际stride的上界，不假装与Android纳秒linet四舍五入逐字节相等。
- **channel与端口区分**：MMPC AP供给保留原字节域10/7、16MB/s向上取整的静态上界，可能高于Android /1024后再逐步向下取整的动态channel需求；没有据此改用更小预算，也不将该余量喂给端口OSTD。真实posted-write、mask事务、A0 idle/guard/restore与所有权限保持。没有测试脚本、额外设备操作或产物复查。
- **r36完整构建/推送终态**：固定源码 **`52f5e88ee60d92efdb68a990afdaf45cabffb0e7` / tracked clean**，固定Python3.13正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-mmqos-kbps-r36-20261117` **actual exit0**（session61261终态），104/104 overlay +3 prebuilt，相机182 warnings，Image32231424 B / boot15101952 B。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-mmqos-kbps-r36-20261117/boot.img`**，原控制台`x9/out/camera-mmqos-kbps-r36-20261117-console.log`，输出目录保留`source-head.txt/source-status.txt/boot-input.json/build-command.txt/full-build.exit/build.log`。代码两提交`2bb51eda/52f5e88e` push **actual exit0 `c4a7a892..52f5e88e`**（session25797终态）；本机无CI run ID，GPU280无需mailbox。r35原件保留但不作为最新候选，未上机、未跑测试/套件或二次检查成功产物。
- **运行边界与下一具体项**：默认后/前双摄准入，未加`x9_camera_lenses=1`。用户可从主checkout手动`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-mmqos-kbps-r36-20261117/boot.img`。最新实机仍350；MM_2 ACK、CMDQ消费、RAW/SOF/DONE和实际预览尚无新日志，不能宣称完整代码或相机验收完成。新日志关注`SMMU FDT ... prefetch2`、`NATIVE SMMU ... tcu_pf=2`、`MMQOS TAKE ... mix_MBps=... OSTD=... errno=0`及实际IRQ/RAW/handler。继续核实际启用的CAMSV通道（含STG/CQI是否可达），再沿IRQ/FBC/cache/worker/handler检查；本轮已读Android`mtk_cam-sv.c:3456..3638`与native`cam_irq_sample`，SOF/hybrid基础状态读源一致，未发现新的确定IRQ修正，不凭多引擎统计扩端口权限。

#### 2026-11-17 相机主链继续：修正 L19/P0 的 MMQoS OSTD 枚举与取整（r35 整包成功，由 r36 后续修正）

- **起点/范围**：单代理在既有相机工作树/分支，源码`d5da42e7`、r34交接补记`c4a7a892`已提交推送（actual exit0）。最新运行仍`~/350.txt`，r28b..r34均无新MM/RAW/预览结果；主checkout、共享输入与设备未改，未跑测试脚本/套件。
- **确定源码错误**：`x9_camera_native_bw.inc`将live DT `w-hrt-ostdl=3`错当`OSTDL_2`并用`ceil(peak_Bps/256000000)`。Android `drivers/interconnect/mediatek/mmqos-mtk.c:268..274/1395..1441`明确枚举3为`OSTDL_3`，write路径先对整MB/s按ratio向上取整，再乘3；`mmqos-mt6993.c:262..263/831`确认L19/P0 write、bw_ratio9、max_ratio64。不能据静态公式错误认定它导致日志350的旧MM电源失败。
- **完整输入链/修正**：`drivers/misc/mediatek/mtk-interconnect/core.c:224..265 aggregate_requests_v2`使用`max(v2_avg_bw,v2_peak_bw)`，只有`MTK_MMQOS_MAX_BW`哨兵才设置`v2_max_ostd`；同目录`mtk-interconnect.h:16`的`Bps_to_icc`与`mmqos-mtk.c:58 icc_to_MBps`依次整除1000，等价先截断成十进制MB/s。native finite RAW需求因此改为`mix_MBps=floor(max(avg,peak)/1000000)`、非零时`ceil(mix_MBps/512)*3`，否则保留源码默认1，最后clamp64。无哨兵、不引入display/max-BW override；A0原posted-write/idle/lease/关闭恢复均保留。
- **channel余量边界**：本机DT `mmqos-state=13f42d`含`SRT_DATA_BW(bit14)`；`mmqos-mtk.c:1117..1129/567..579`对SRT/HRT乘10/7再向下换16MB/s units。现有native是同单位的静态上界供给，保留原向上取整以覆盖RAW上界，不把它误称逐整数位复制动态ICC请求；端口OSTD不再混入这个channel余量。`MMQOS TAKE`追加`mix_MBps`与原OSTD目标，便于真实上机核对，不改公开ABI或权限。
- **正常编译实绩**：Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，182 warnings**（session52161终态），日志`x9/out/camera-mmqos-ostd-compile-r1-20261117-console.log`，输出`x9/out/x9-camera-mmqos-ostd-compile-r1-20261117/compile.exit=0`。随后固定clean源码构建唯一r35；整包SHA/结果/push待补。本机无CI run ID，没有额外测试或成功产物二次检查。
- **r35构建终态与替代原因**：源码 **`2bb51eda81b22d2396dab31641164bba36702098` / tracked clean**，正常完整构建 **actual exit0**（session60748终态），104/104 overlay +3 prebuilt，Image32231424 B / boot15101952 B；路径`x9/out/x9-camera-mmqos-ostd-r35-20261117/boot.img`，原控制台`x9/out/camera-mmqos-ostd-r35-20261117-console.log`，source/input/command/exit/build.log同目录保留。layout检查exit0。本轮继续追到ISP8s实际调用发现相机侧helper使用/1024而非通用/1000，该候选不作为最终供测版本，由下述r36修正；未上机，无CI run ID。
- **下一主链项/未闭**：继续核Android实际CAMSV MMQoS输入、MM clocks/SMI→SMMU→CAMSV IRQ/FBC→RAW cache/worker→PixelBufferHandler。Android动态路径还对SMMU stash/CQI单独记需求，native当前是单图像、无CQ运行路径；是否存在实际未覆盖consumer须从可达硬件配置核定，不能见源码字段就添加其它端口票。secure WPCFG/实际CMDQ消费、SOF/DONE、预览/照片/视频与AF/OIS/ISP色彩标定等仍无新验收，完整目标保持active。

#### 2026-11-17 相机主链继续：本次 FDT 的 SMMU TCU 预取策略（r34 整包成功，未上机）

- **范围/起点**：用户目标明确继续MM时钟→SMMU→RAW/SOF/DONE→实际预览，直至剩余代码闭合；本轮沿此主链，未转去马达。单代理续用相机工作树/分支，起点`84eb3625` clean。上一回合属progress（r33源码、整包、推送）；最新实机仍`~/350.txt`，r28b..r33均未有新运行证据。主checkout/共享输入未改，未运行测试脚本/套件或操作设备。
- **新确定遗漏**：Android `arm-smmu-v3.c:2453..2482 arm_smmu_install_ste_for_dev`在写入前调用MTK `smmu_setup_features`；`mtk-smmu-v3.c:2889..2894`从DT `tcu-prefetch`设置feature和值，`:1592..1614`在有效S1 CD pointer且ATS关闭时将该值写到STE word1 bits57:56。live DT `iommu@30800000:17203`明确为2，`mm-smmu-cam:20411..20419`为SID80且无ATS/DCP/bypass/coherent override。原native STE仅有SSID0/NC/shareability/stall字段，漏掉此安装钩子的真实DT策略。**尚无实机证据说明该遗漏是无画面的根因，不能由源码缺口推断RAW已通**。
- **真实启动输入**：新增独立32B `x9-camera-smmu-policy` ADT零占位与`camera_smmu_handoff.c`，使用原有一次bounded FDT collect及一次批量phandle验证，无新whole-tree逐字段walker。核root/SOC cells/ranges、三项controller compatible完整列表、实际两resource、enabled、#iommu-cells1、TCU2、master compatible/iommus SID80/supply引用及影响STE的属性真实缺省；SMMU phandle必须全树唯一。camera handles5→6，原IRQ/CSI校准各自资格保留。只在全部通过后复制本boot元数据；未知/malformed留下零占位，不用captured live.dts或生成常数冒充本boot输入。shim不摸MMIO/IRQ/SMMU/SMC。
- **内核实现/生命周期**：native startup在SMMU MMIO与backing export之前读取该policy并验证固定MT6993 MM/SID80 profile，保存在本namespace。有效SID80 S1 STE word1按本boot `tcu_prefetch<<56`发布；初始abort、非camera STE、继承host API合同不改。统一word1构造用于export、完整storage比较和BBM phase1，withdraw phase2/3仍按原真实abort/bypass目标清除。live wrapper guard同时重核原本boot policy，旧epoch/backing/policy随已pin namespace保留，不新增ioctl或READY授权、不改CMDQ消费/EVTQ/GERROR。新日志`X9 CAMERA SMMU FDT ... prefetch2`和`NATIVE SMMU ... tcu_pf=2`；缺policy拒绝native startup，不默认填2。
- **正常编译**：`compile-r1`四个camera shim C正常target编译exit0，但kernel因新增SecureDT读取缺显式`pexpert/device_tree.h`而exit1，7个声明错误；原日志/失败目录保留。补该include后正常Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，182 warnings**（session22779终态），日志`x9/out/camera-smmu-policy-compile-r2-20261117-console.log`，输出`x9/out/x9-camera-smmu-policy-compile-r2-20261117/compile.exit=0`。r1为`x9/out/camera-smmu-policy-compile-r1-20261117-console.log`及对应目录。随后仅补shim注释/交接，最终固定clean源码构建唯一r34，整包SHA/结果/push待下条补记。
- **r34整包/推送终态**：固定源码 **`d5da42e796969a833ade94014cccd95c2431a124` / tracked clean**，固定Python3.13正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-smmu-policy-r34-20261117` **actual exit0**（session50260终态），104/104 overlay +3 prebuilt，相机182 warnings，Image32231424 B / boot15101952 B。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-smmu-policy-r34-20261117/boot.img`**，原控制台`x9/out/camera-smmu-policy-r34-20261117-console.log`；目录保留`source-head.txt/source-status.txt/boot-input.json/build-command.txt/full-build.exit/build.log`。源码push **actual exit0 `84eb3625..d5da42e7`**（session8410终态），本机无CI run ID，GPU280无需mailbox。默认后/前双摄，未加`x9_camera_lenses=1`；未上机、跑测试或二次检查成功产物。最新运行仍日志350，不能当r34运行结果。
- **核查边界/下一主链项**：同源device_reset的DVM分支由`mediatek,dvm`决定，当前MM节点未设置，保留CR2=6，不按其他SMMU的广播配置猜DVM connect。`sv_run_df_reset()`默认static FIFO分支的monitor另有`is_fmon_support()`条件，不擅自启用调试IRQ。继续沿真实MM parent/rate/MMPC/SMI→SMMU normal bootstrap→CAMSV request/RCNT→SOF/DONE/FBC→RAW cache/worker/PixelBufferHandler的可达调用核遗漏；固件重定向/secure WPCFG、实际MM_2 ACK/CMDQ消费及真实预览仍无新运行验收。保留完整目标active，不把TCU编译或单对象成功称为全部代码/相机完成。

#### 2026-11-17 相机继续：长焦 OB 选表与 Sony 增益整数顺序（r33 整包成功，未上机）

- **位置/证据**：单代理在既有相机树/分支，源码起点`01f0985b`，r32交接补记已提交推送`19cd31f9`（push actual exit0）。主checkout与共享`.local`输入未改。最新运行依据仍`~/350.txt`，r28b..r32无新实机结果；本轮未操作设备/ADB/分区、运行测试脚本/套件或二次检查成功产物。
- **确定缺口/实际源码顺序**：Android `hhtele_mipi_raw/hhtelemipiraw_Sensor.c:1591/1602/2462..2517` 的preview/normal_video按`rg_gain>0x3df8`选36db/type1，否则30db/type2；`open/control:2046/2533`重置type0，初始preview八个值不等于优化表。`set_group_hold:2159`仅调用`set_i2c_buffer`，ISP8s `adaptor-subdrv-ctrl.c:312..363`仅在commit发送缓冲；`adaptor-subdrv.h:893`→`adaptor-i2c.c:626..700`的OB表直接发送。因此原线上顺序是**OB八次store在hold/gain/release发送之前**，不能因为C调用先写hold就误判为OB在group内。
- **实现**：扩展既有`x9/camera/build.py`从tele的Sensor.c提取两份八对OB表、保留原notice/source SHA manifest，缺vendor输入生成count0且不可用，不把厂商表值放进tracked overlay。生成器与kernel sensor preflight均限制精确寄存器顺序，byte值检查保留。native AE在真实RAW/CPU/no-credit PARKED边界依据实际gain code首次选表/跨阈值切换；首次同值增益也不能走原memcmp快退。只由专用OB路径发八条窄写，原AE writer及generic stream/hold/OTP限制不放宽；先OB→原hold/帧长/曝光/增益/release→稳定controls→三个真实0005计数前进→下一request metadata。每条尝试在可能START之前记录，真实ACK在后置预算检查之前计数；首错停、不重发或猜OB可读。`ob_acked`与`ob_bound`独立，不能以ACK替代画质/生效验收。
- **取整与生命周期**：核main/front/tele各自`get_gain2reg:3119/1012/2169`均为`unity-floor(unity*BASEGAIN/gain)`，原native写成`floor(unity*(gain-BASEGAIN)/gain)`可少1码，已按三颗真实公式统一修正；UW线性公式保留。完整AE/OB状态（含最后table的逐寄存器尝试/ACK进度、独立serial与切换次数）随实际关闭后的native epoch receipt归档；已started epoch要求对应AE seed，任何同epoch AE error或OB ACK/bound不一致禁止recycle，未seed的never-started不伪造AE记录。原8个exported epoch pin上限不变。
- **正常编译里程碑**：构建器正常`x9/camera/build.py`（生成表并编bridge/ctl）及Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，182 warnings**；输出`x9/out/x9-camera-tele-ob-compile-r1-20261117/`、日志`x9/out/camera-tele-ob-compile-r1-20261117-console.log`，`compile.exit=0`。之后仅补公式注释、生成器括号和文档，将由固定clean源码整包编译。下一唯一候选r33；实际SHA/整包/推送结果待下条补记，本机无CI run ID。
- **下一验证/未闭范围**：仍先靠新启动日志确认MM_2→SMI→cold reset/QoS→CMDQ→RAW/SOF/DONE/handler。长焦真实首次/阈值切换时应有`X9 CAMERA AE ... ob_change=1 ob_writes=8 ob_acks=8 ob_target=1或2 ob_acked=同target ob_bound=同target advances=3 errno=0`；同type不重复发送，失败保留原partial journal/旧frame metadata。上述仅源码期望，无实机结果。AF/OIS/超广角banked校准、ISP颜色/镜头标定、原生虚拟三摄/真正多路无缝、VENC以及预览/照片/视频验收仍未闭；继续推送相机分支，不合main或删除树。
- **整包/发布终态**：固定源码 **`3945315035c88fc24880909ece8eba25337aeae0` / tracked clean**，固定Python3.13正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-tele-ob-r33-20261117` **actual exit0**（session54398终态），104/104 overlay +3 prebuilt，相机182 warnings；kernel/bridge/shim/boot成功，原日志 **Image32231424 B / boot15101952 B**。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-tele-ob-r33-20261117/boot.img`**，原控制台`x9/out/camera-tele-ob-r33-20261117-console.log`，输出目录保留`source-head.txt/source-status.txt/boot-input.json/build-command.txt/full-build.exit/build.log`。`prepare-local.py --check` exit0，日志`x9/out/camera-tele-ob-layout-20261117.log`。源码push **actual exit0 `19cd31f9..39453150`**（session40990终态），本机编译无CI run ID；GPU280无需mailbox，未上机、跑测试或二次检查成功产物。
- **运行顺序/准入边界**：用户从主checkout手动`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-tele-ob-r33-20261117/boot.img`。此候选沿用r32的默认后/前两摄准入，**未添加`x9_camera_lenses=1`**；先取得最早MM/RAW失败的新日志，不能要求默认dual包产生长焦OB日志。长焦OB的运行验收仍需显式四摄/tele准入候选与真实长焦增益请求，代码已编入不等于长焦已运行。
- **构建期间新增源码事实/下一具体项**：`hhuwide_mipi_raw/hhuwidemipiraw_Sensor.c:3024..3041`的`get_sensor_cali`仅probe EEPROM，`set_sensor_cali`函数直接return；现有这份Android源码**没有banked GGC/校准写表可以继续搬**。保留UW unsupported/factory blocker，不猜该注释下的寄存器。下一可实现项是lens真实运行期初始化→FOCUS→实际关闭归位，源路径已核定为`vendor/oplus/kernel/camera/lens/vcm/v4l2/{ak7316m_24085,ak7316t_24085,dw9800s_24081,jd5516w_24085}/`，不是`proprietary/`。main `ak7316m_24085.c:96..174`的position为SMBus word `swab16(val<<6)`、release有100-step/1ms归位和02=20；init依赖sensor open先激活02=00，后面还需继续读完整init/PID分支，不能只截取激活写即公告AF。front DT09/源码强制0c冲突仍在，未触设备核定；本轮未修改马达逻辑，kernel继续NO_FOCUS。

#### 2026-11-17 相机继续：按本模组 EEPROM 补齐 sensor QSC/SPC/LRC（r32 整包成功，未上机）

- **工作位置/运行证据**：单代理续用 `.worktrees/camera-integration` / `feat/camera-integration`，基线 `6e9802f4` clean。主checkout在SMS分支且NOTES有未提交修改，未触碰。最新运行依据仍`~/350.txt`，r28b..r31及本轮均无新上机结果。没有ADB/设备/分区动作、并行代理、测试脚本/套件或成功产物二次检查。
- **确定缺口/Android依据**：`.local/vendor/oppo-camera-imgsensor-common/hhmain_mipi_raw/hhmainmipiraw_Sensor.c:131..149/2990..3089/3349..3410`、`hhfront_mipi_raw/hhfrontmipiraw_Sensor.c:103..118/881..911/1202..1338`、`hhtele_mipi_raw/hhtelemipiraw_Sensor.c:114..131/2041..2140/2400..2475`：正常open的init后、preview前调用`set_sensor_cali`，使用`get_sensor_cali`从本模组EEPROM取得的QSC/SPC/LRC。原移植仅加载init/preview，漏了此调用。ISP8s `adaptor-subdrv-ctrl.c:476..508`按EEPROM+6四个little-endian字节匹配header；live DT的main/tele EEPROM地址50、front54与上述源码一致。
- **实现/数据范围**：新增`x9_camera_sensor_calibration.inc`，复用本会话既有AP FIFO/rails/pins/clock/receiver守卫，维持WRRD最多2B，不扩FIFO/DMA权限。main header015e012c：EEPROM1e30/3072B→sensor c000、7c20/384B→d200与d300各192B；tele header01b1012f：2210/3072B→c000、2e20/384B→d200/d300；front header01510008：c90/1560B→c500、14b0/260B→7a98与7b1c各130B。两遍完整payload及每遍前后header必须一致，才开始任何sensor表写；不扫描地址、不写EEPROM数据/写保护、不带其它模组缓存、不猜checksum、不使用固定校准字节。原payload与实际读取/写入ACK进度留在独立epoch journal，未打印数据内容。
- **顺序/失败语义**：同一mode2/mode3 sensor-session先preload，再原init/fab patch→main镜像（仅main原set_sensor_cali有此写）→QSC→enable→SPC/LRC→原preview→原镜像/稳定controls→最终QSC开关读回。main/tele enable3206，front3621；**front preview原3621=0必须保留**，不因open校准曾写1而强行改回。没有假定sensor校准SRAM可读/已物理生效，`X9_CAM_CFG_MODULE_ACKED=512`仅表示完整校准事务ACK，不清factory blocker。追加config stage EEPROM/CALIBRATION/CAL_MODE，720B config与120B RAW ABI不变。校准preload与含校准的整个byte配置各15s软件预算，逐事务首错即停、不重发；保留原AP不确定请求/冷关闭规则。ACK后总预算超时仍保留真实ACK计数。原standby日志不再笼统称无校准，明确没有光学验收。
- **生命周期**：校准成功不改变capture/stream/IRQ/RAW/DMA权限；自然退休和整个物理关闭后，native epoch receipt再归档校准payload/journal，recycle要求对应sensor/epoch完整无错，八个exported epoch pin上限及旧backing保留不变。超广角banked校准未接入，日志显式unsupported；软件ISP仍无本机LSC/CCM，不能把sensor QSC/SPC/LRC载入称为色彩/画质通过。
- **AF/OIS定向核查**：本轮在`.local/references/oppo-modules-dt-mt6993/vendor/oplus/kernel/camera/lens/`找到真实驱动（先前只查MTK树未找到不代表缺源码）。live DT front=i2c2/dw9800s_24081@09、main=i2c4/ak7316m_24085@0c、tele=i2c7/ak7316t_24085@2d、UW=i2c9/jd5516w_24085@0c。**front驱动init却强制client->addr=18>>1=0c，与DT09不同**，没有新运行证据不能猜目标。main/tele sensor open还有原02=00/3ms的AF激活；本轮未实现马达供电/激活/归位、FOCUS命令或帧绑定，kernel仍NO_FOCUS。后续应按这组源与实际bus/ACK证据补齐，不能用bridge旧PM8008i范围代替X9驱动。
- **编译里程碑**：初版正常Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，189 warnings**，原日志`x9/out/camera-module-cal-compile-r1-20261117-console.log`，输出`x9/out/x9-camera-module-cal-compile-r1-20261117/`。随后消除本文件7条新warning、补header末采样/ACK计数及文档，最终由正常整包编译；`prepare-local.py --check` **exit0**，日志`x9/out/camera-module-cal-layout-20261117.log`。下一固定clean源码提交后构建唯一r32；实际整包SHA/结果/路径与push实绩待下一条补记。本机编译无CI run ID；没有新的MM/RAW/handler/预览、照片/视频/切镜或校准效果证据，完整相机仍未完成。
- **整包/推送终态**：固定源码 **`01f0985b20d71bb3b6d01031fc6877856afd35aa` / tracked clean**，固定Python3.13正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-module-cal-r32-20261117` **actual exit0**，104/104 overlay +3 prebuilt，相机182 warnings，kernel/bridge/shim/boot成功，原日志 **Image32231424 B / boot15101952 B**。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-module-cal-r32-20261117/boot.img`**；控制台`x9/out/camera-module-cal-r32-20261117-console.log`，输出目录保留`source-head.txt/source-status.txt/boot-input.json/build-command.txt/full-build.exit/build.log`。源码push **actual exit0 `6e9802f4..01f0985b`** 至相机分支。本机编译无CI run ID，GPU280无需mailbox；未上机、未运行测试或二次检查成功产物。
- **下一确定代码缺口**：`hhtele_set_gain():2462..2517`在preview/normal_video按实际gain code `>0x3df8`选择36db OB表，否则30db；`open/control`重置`wr_ob_type=0`。当前native AE仅更新曝光/增益/帧长，既漏首次SET_GAIN选择也漏阈值切换。表源在同文件1591/1602，需通过现有生成器提取，不能直接复制厂商表到tracked overlay。先追`set_group_hold/set_i2c_buffer/commit_i2c_buffer`，按真实发送顺序接入CPU/no-credit边界；不把初始preview表值当已应用30db，不提前修改下一帧metadata。AF/OIS等上述边界仍未闭。

#### 2026-11-17 相机继续：ISP8s 默认 DDR/COH/BW 请求与独立最终关闭复位

- **范围/依据**：单代理在既有相机树/分支，起点`465811bd` clean，上一回合归类progress（r30源码/整包/推送）。最新实机仍`~/350.txt`，没有r30运行证据。按同一ISP8s `mtk_cam-sv.c:1920..1923/2140..2205`默认SW-mode和`:1261..1304/797..969`正常关闭路径推进；没有执行测试脚本/套件、ADB、boot、分区操作或修改共享输入。
- **六条启动RMW**：新增`x9_camera_sv_qos.inc`，DDR+150、COH+280、BW_QOS+158依Android顺序分别SET bit4→OR_CQ bit1。冷基线拒绝继承的timer/mode/OR_CQ/SET/CLEAR/SW_R请求；所有真实RMW输入、完整输出和issued在store之前记账，保留Android第二次RMW中真实读出的SET/SW_R位，不另造重试。OR_CQ普通控制实际外bank两次目标读回，inner仅记录实际传播、不杜撰inner命令ACK。只有已发SET的SET/SW_R与已发OR_CQ字段允许变化，其余含CLEAR保持严格；SW_R仅原始状态观察，**不据字段名宣称DMA/供电请求已验收**。原geometry/FIFO/FBC/地址恢复不回写这三个命令字，运行中的普通完整寄存器守卫继续逐字核对。
- **关闭依据与时机**：同源ISP8s没有显式三个CLEAR关闭写者，`central_common_disable()`实际调用`sv_reset()`。新增`x9_camera_sv_shutdown.inc`，放在原自然帧/RAW/CPU/worker/GIC/FBC退休与SMMU恢复、SV普通配置恢复、OUTMUX/PHY/MAC恢复之后，SMI/MM撤引用之前。此时sensor已进入CLOSING并由`cam_cold_isolate()`真实拉低reset、断开MCLK引脚，不能再调用要求未close的AP I2C读；新guard检查实际pin隔离、独立native退休授权、GIC全idle、源输入不变及全部七通道双outstanding零。首次reset前再次真实核对原自然FBC、RET/CANCELLED slot与S1 raw状态；reset不会制造自然帧完成或让未提交credit通过。
- **正常reset实现**：按同源DCM inhibit1、四条outer/inner CAM/SOF_SUB RMW、inner CQ subsample RMW，接DMA0/CQ0/STG0→DMA1/STG1/CQ1，各poll ACK bit1/100ms（DMA→STG→CQ），然后原9条CQ0x11/DCM0/central DCM0/SW_CTL1/DMA0/STG0/CQ0x10/CQ0/SW_CTL0。共21条store逐条先记账；持central reset期间保持原连续尾序列，不插入引擎读/传感器I2C。没有引入另版本第二SOF CLEAR或Android错误debug sweep；ACK超时不重发，保留依赖。
- **释放/归档条件**：只在三个请求完整raw字都实读回本次冷基线、各stop/DCM/reset实读零、两份完整CAMSV post配置稳定且源输入仍未变后，标记QoS退休并更新receiver的实际SV expected（原before与原自然FBC/IRQ/DMA历史不改）。新增SMI close独立QoS release guard，final reset或readback失败不能绕过它撤电。epoch recycle另要求这份完整reset/QoS回执，并把两个新journal和原历史一同永久存档，原8-epoch pin预算不变。**这里实现了待验证的同源关闭序列，不假定SW_CTL一定清除软件请求；若实机不回冷值，实际EBUSY并保留SMI/MM**。
- **编译里程碑**：初版正常Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，184 warnings**；日志`x9/out/camera-qos-shutdown-compile-r1-20261117-console.log`、输出`x9/out/x9-camera-qos-shutdown-compile-r1-20261117/`。随后补OR_CQ目标等待、移声明消掉两条新warning并补注释/交接，最终由正常整包编译。没有CI run ID、成功产物二次检查或硬件效果；下一固定clean源码构建唯一r31。完整目标仍active，真实MM/RAW/handler/预览、四摄拍照/视频及AF/OIS等尚未验收。
- **整包实绩**：固定源码 **`77736a75` / tracked clean**，固定Python3.13正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-qos-shutdown-r31-20261117` **actual exit0**，**104/104 overlay +3 prebuilt**，相机最终182 warnings，kernel/bridge/shim/boot全成功；原日志 **Image32231424 B / boot15093760 B**。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-qos-shutdown-r31-20261117/boot.img`**，原控制台日志`x9/out/camera-qos-shutdown-r31-20261117-console.log`和输出目录`build.log`；确切`source-head.txt/source-status.txt/boot-input.json/build-command.txt/full-build.exit`同目录保留。`prepare-local.py --check` exit0，本机编译无CI run ID；GPU280、无需mailbox，未跑测试/重检成功产物/操作设备。
- **下一运行证据与后续工作**：用户手动从主checkout执行`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-qos-shutdown-r31-20261117/boot.img`；仍先验MM_2/时钟/SMI与`CAMSV COLD RESET`，再看新增`CAMSV QOS issued=3f seen=7/... configured=1 errno=0`及CMDQ/RAW/SOF/DONE/handler/预览。正常关闭新增`CAMSV SHUTDOWN phase=4 issued=1fffff ack=2或3/2或3/2或3 samples=2/2/2 post_samples=2 qos_retired=1 complete=1 errno=0`；若三个请求未回冷值，收其原EBUSY与完整保留journal，不改成reset写成功即退休。**没有新运行日志，不称上述源序列、完整相机或照片/视频/切镜验收通过**。后续按最早失败阶段推进；AF/OIS/实际标定、原生虚拟多摄UI/真正双路无缝和硬件VENC仍为未闭范围，kernel仍明确NO_FOCUS，不能由bridge AF状态机推定镜头马达已接入。源码与交接继续提交推送相机分支，不合main、不删树。

#### 2026-11-17 相机继续：ISP8s 冷复位与完整恢复后的 receive/TG 观察器生命周期

- **位置/范围**：单代理续用 `.worktrees/camera-integration` / `feat/camera-integration`，起点 `993909e0` clean；主 checkout 在其它任务且 NOTES 有未提交修改，本轮未触碰。最新设备证据仍 `~/350.txt`：MM stage1/0514失败；r28b/r29以及本轮代码均无新上机结果。未执行测试脚本/套件、ADB/boot/分区操作或并行代理。
- **Android依据/实现**：ISP8s `mtk_cam-sv.c:4509..4580` 的正常 runtime resume 在 larbs/clocks后调用 `sv_reset_by_camsys_top(:696..784)`；`:727..747` 为 CQ stop0→1/ACK bit1/100ms→0，`:771..779` 为 MRAW `SW_RST_1` 的0→`3<<(id*2+4)`→0。live DT `camsv1@3a600000` 的id=0、SCQ=3a602000，regs.h给MRAW=3a680000/+a4。新增 `x9_camera_sv_reset.inc` 在 native SMI clocks成功后、MAC/PHY/OUTMUX/geometry/request/FBC之前执行上述正常冷复位。三条真实GIC无enable/pending/active，sensor仍standby、全部输入/receive IRQ/其它stop关闭，DT七个CAMSV0通道external/internal outstanding零才进入。MRAW连续三写之间不插入I2C或引擎读；每条store先记账，不因timeout重发。没有移植serror调试DEADBEEF/selector写。
- **证据/失败生命周期**：保留独立reset前、过程末值和两份reset后RX快照；只有全部完成、两份SV稳定且源输入未改，才发布本次实际SV默认值给receiver expected，原RX before不改。reset部分失败保留错误和MM/SMI依赖，receiver close先查独立reset完成记录；不以关闭电源清掉未消费的reset请求。新增 `X9 CAMERA CAMSV COLD RESET ... issued/ack/ack_samples/post_samples/complete/errno`。exported epoch最终物理关闭时，reset记录和原全部SV/IRQ/FBC历史一并复制进不可变receipt；没有松动8个永久pin上限。
- **另一个确定代码缺陷**：原 `cam_sv_close_locked()` 成功后置`held=false/request_held=false`，但保留`receive_issued`和TG恢复历史；后续 `cam_out_close`/SMI/MM的`cam_rx_session_guard→cam_rx_sample`会进入旧receive/TG采样器，因`!held`必返ESTALE，关闭链及下个epoch无法推进。新增`cam_sv_controls_restored()`只在完整恢复、无dirty/pending/commit/request/error且RESTORED成立后退出旧live采样器；此时FBC/TG历史比较不再归一化，按raw值精确比较。部分恢复/在途帧仍走原严格守卫，所有原始字段留给epoch archive，不清零伪造退休。
- **SID与DDR核查实绩**：live DT `mm-smmu-cam` 的`iommus=<0x25b 0x80>`由`arm_smmu_of_xlate`原样采纳；`mtk_cam-sv.c:4282..4297`的`mediatek,smmu-dma-axid`只注册fault callback，不能把262/260/1c0/3a0当STE索引或secure路由表。`smmu-secure.c:218+` 的`mtk_smmu_aid2sid`在`CONFIG_MTK_IOMMU_DEBUG`内且仅debug switch调用；未新增AID-SID SMC。ISP8s全树对DDR/COH/BW_QOS仅有SET写者和SW_R字段定义；ISP8第二次SOF的DDR/BW_QOS CLEAR属于不同硬件模式且没有COH对应，不擅自移植为ISP8s关闭序列。三个软件请求及normal `sv_reset`关闭仍待闭合，不能称全部相机代码完成。引用列表留本树`x9/out/camera-aid-sid-refs-20261117.txt`、`camera-ddr-isp8s-refs-20261117.txt`。
- **当前编译**：初版冷复位正常Builder `compile_one(sweet_x9_camera.o)` **actual exit0/True，183 warnings**，原日志`x9/out/camera-resume-reset-compile-r1-20261117-console.log`，输出`x9/out/x9-camera-resume-reset-compile-r1-20261117/`；随后调整为连续reset脉冲、保留双post样本，并加入完整恢复观察器修复，最终由整包正常编译。下一固定clean源码SHA构建唯一r30；无CI run ID或实机验证，整包/发布结果待下一条补记。
- **整包终态**：固定源码 **`64b34fcb` / tracked clean**，固定Python3.13正常`build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-resume-reset-r30-20261117` **actual exit0**。**104/104 overlay +3 prebuilt**，相机最终编译182 warnings，kernel/bridge/shim/boot成功；原构建日志报告**Image32215040 B / boot15089664 B**。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-resume-reset-r30-20261117/boot.img`**；原日志`x9/out/camera-resume-reset-r30-20261117-console.log`及输出目录`build.log`，同目录保存`source-head.txt/source-status.txt/boot-input.json/build-command.txt/full-build.exit`。`prepare-local.py --check` exit0。本机编译，无CI run ID；GPU280、无需mailbox。没有测试/成功产物额外检查或设备动作。
- **下一步与验收边界**：用户从主checkout手动`fastboot boot .worktrees/camera-integration/x9/out/x9-camera-resume-reset-r30-20261117/boot.img`，采新日志和屏幕现象。先查HWCCF MM_2与MM时钟/SMI，再查`CAMSV COLD RESET issued=3f ack=2或3 ack_samples=2 post_samples=2 complete=1 errno=0`（CQ最终0/0）、SMMU CMDQ与NATIVE CONFIG；之后才看真实RAW/SOF/DONE、handler、后前摄关闭重开及预览/照片。**本轮没有实机冷复位/完整关闭/RAW证据，不称目标完成**。仍待对照ISP8s关闭`sv_reset`补齐DDR/COH/BW_QOS请求生命周期；不能直接拿ISP8第二SOF的CLEAR作依据或用reset清历史替代自然帧退休。源码与本次交接提交推送相机分支，不合main、不删相机树。

#### 2026-11-17 相机继续全链核查：CMDQ 启动、普通 wrapper、CAMSV FIFO 与四摄 slot

- **位置/证据**：单代理继续 `.worktrees/camera-integration` / `feat/camera-integration`，基线 `94f43c8a` clean。已读 NOTES、CAMERA_PLAN、x9/AGENTS；最新设备证据仍是 `~/350.txt`，本轮无设备/ADB/debugfs/分区动作，不修改主 checkout 或共享输入。
- **确定缺陷一**：Android `arm-smmu-v3-common.h:80..86` 定义 CMDQEN=bit3、EVTQEN=bit2；`arm_smmu_device_reset()` 在 CFGI_ALL/SYNC 前先使能 CMDQ。原 native 首次 `CR0=4` 只开启 EVTQ，随后提交命令必然无正常消费。已改具名位 **8→12→13**，提交 PROD 前还要求实际 CR0ACK.CMDQEN；全启用/退休比较使用同一常量。已核查部分启动失败：`native_setup_issued` 后仍禁止进入 never-exposed 回收，不重发、伪造 CONS 或释放不确定资源。
- **普通 SMMU wrapper 缺口**：按 `mtk_smmu_hw_init/smmu_init_wpcfg()`、`mt6993_data_mm` 与 live DT `iommu@30800000`，接入第二 resource **`309e0000`** 的 GLB_CTL0/TCU_CTL1/TCU_CTL10/TCU_CTL0：DCM/CFG_TAB/EXTRA_DCM、MM STD_AXI_MODE_DIS、AXSLC=e0 与 SB_ONLY、GID8、QoS5、stash3。四个普通 RW 字完整双采样，按 Android 顺序 masked 写；CMDQ/live 同时核对 wrapper，已发 store 前记账，部分失败保留 MM/backing。正常退休在实际 OFF/RAW/worker/GIC/A0 资格与 CR0/ACK=0、原 normal bases/cursors 恢复后，才逆序恢复本次改过的 wrapper；fresh-token reopen 同时核对其原值。没有猜加另一条 SMC、secure WPCFG/AID-SID/DVM/MPAM/ELA 初始化；这些平台策略及真实 DMA 路由仍需运行证据，**普通 wrapper 配置不是 firmware grant**。
- **确定缺陷二**：`cam_native_snapshot` 的 17×u32 与 u64 base 之间原有4B padding；退休/重开仅 memcpy 数组，随后全结构 memcmp 会读取未初始化栈字节并可能误报 ESTALE。统一 `cam_native_prior()` 先 bzero 再复制所有字段（含本轮 wrapper），覆盖正常退休、never-exposed、retirement_check 与 reopen 全部构造点。
- **CAMSV 缺口**：Android ISP8s `mtk_cam_sv_dev_config()` 必调 `mtk_cam_sv_dmao_common_config(0,0,0)`，原 native 只有几何/stride，34个 DMA/CQI 阈值只被快照而没有初始化。按 **mt6993** 默认静态 FIFO（DF默认关闭，IMG1536/LEN36/CQI64）及原 `FIFO_THRESHOLD` 乘后除法，接入全部三core IMG/LEN CON3/2/1/4 和两CQI CON0..4，保留既有 outer→inner 消费/DB、pending、首错与逆序恢复。私有字段48、dirty扩为64bit；公开 RAW/ioctl ABI 不变。不启动 CQ/其它tag、不将 FIFO 配置当帧证据。新增 `X9 CAMERA CAMSV CONFIG ... geometry=10 dma_cqi=34` 日志。
- **确定缺陷三**：四摄接线已存在，但 `cam_irq.slots` 仍为2元素，prepare用 `sensor-1` 会令 sensor3/4 越界；reopen另硬编码 `which>=2`。已统一 `X9_CAMERA_SENSOR_COUNT` 分配/边界，并在 prepare 索引前验证 sensor。历史完整 slot 仍保存，不解除8个 exported epoch pin 上限。
- **编译里程碑**：正常 Builder `compile_one(sweet_x9_camera.o)` **exit0/True，182 warnings，无 error**；原日志 `x9/out/camera-normal-start-compile-r1-20261117-console.log`、目录 `x9/out/x9-camera-normal-start-compile-r1-20261117/`。该次已含 SMMU/CAMSV主改动，随后四摄slot与配置日志改动将由最终整包正常编译。没有运行测试脚本/套件，也没有对成功产物二次检查。最终源码 SHA、整包路径/结果和 push 实绩在下条补记。
- **整包/发布实绩**：源码 **`eebb907d`**，`prepare-local.py --check` **exit0**。固定 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-normal-start-r29-20261117` 实际 **exit0**，**104/104 overlay +3 prebuilt**、kernel link/shim/boot 完成；构建器报告 **Image 32215040 B、boot 15085568 B**。候选 **`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-normal-start-r29-20261117/boot.img`**，控制台原日志 `x9/out/camera-normal-start-r29-20261117-console.log`、输出目录 `build.log`；本机编译，无 CI run ID。沿用 GPU280，无需 mailbox；未重检成功产物、未跑测试、未上机。源码已通过既有 gh HTTPS credential **push exit0 `94f43c8a..eebb907d`** 至 `feat/camera-integration`，没有改 origin。本补充交接另提交推送。
- **构建期间继续核查的具体结果**：ISP8s `mtk_cam-sv.c:65/69` 的默认值是 `debug_ddren_camsv_sw_mode=1`、`disable_camsv_df_mode=1`；`mtk_cam_sv_df_config()` 默认不请求共享 SRAM。`mtk_camsv_runtime_resume:4509+` 在 clocks 后调用局部 `sv_reset_by_camsys_top()`（先 CQ stop/ACK，再 MRAW reset bits4/5 的0→30→0）。`mtk_cam_sv_ddren_qos_coh_config:2140+` 默认依次 DDR_SET/OR_CQ、COH_SET/OR_CQ、BW_QOS_SET/OR_CQ；同源只见 SET 写者，没有 CLEAR/恢复或 SW_R 读者，故暂不能从字段名推导回收序列，也未将它们放进普通全字 RW/FIFO 恢复表。FMON 另受 `is_fmon_support()` 的平台/调试资格约束，不是无条件应开的生产中断。这些是下一段源码/既有证据核查入口，不是已经实现或已证可跳过。
- **继续项/未验证边界**：不能称全部相机代码完成。下一继续对照 Android `mtk_camsv_runtime_resume/sv_reset_by_camsys_top`、`mtk_cam_sv_ddren_qos_coh_config()` 的普通/命令寄存器生命周期（尤其 DDR/BW_QOS/COH SET/CLEAR 与 SW_R），以及 secure/AID→SID 平台策略；不能只凭字段名发 reset/clear。RAW→原 IRQ SOF/DONE→FBC→cache→kernel staging→iOS RAW10→PixelBufferHandler 已追到现成调用链，没有另造 adapter 或假帧。**还没有 MM_2 ACK、CMDQ消费、物理 RAW、预览/照片/视频或四摄切换的新实机结果**；后续候选仍需用户手动 boot 验证。

#### 2026-11-17 相机 log350 续接：MM_2 HWCCF 电源入口替换，r28b 已构建待上机

- **工作位置/基线**：现有 `.worktrees/camera-integration` / `feat/camera-integration`，起点 `0430c9a3`；先读本文件与 `CAMERA_PLAN.md`。主 checkout 的 SMS 未提交交接和其它功能树未动。本轮没有设备操作、Android debugfs 读取、共享输入修改或合 main。
- **观察与源码核定**：`~/350.txt` 的后/前摄真实 ID 为 `9186/0615`，Camera.app 在 `NATIVE CONFIG stage=1`、`MM stage/slot=1/0` 停于 `smc=c2000514/0 errno=5`。Android `mtk-mminfra-util.c:395` 的时钟回调使用 DT `mminfra-type0-pwr=3`（MM_2），`drivers/memory/mtk-smi.c:2783` 同样申请 MM_2；DT voter55、bit3、ACK `31b500a8 & c0000000`。`hwccf_api.c:776/993/1191` 的 v1 顺序是 MMUP `31a041ec` dummy read → BACKUP1 STA bit3 idle → AP SET/CLR 与 EN 回显 → ON global EN → STA idle；**不轮询 DONE 置位**。`0xc2000831` 是 pKVM 的保持上电 semaphore，不替代 HWCCF 上电，本轮没有猜加另一条 SMC。
- **改动**：`x9_mm_power.c/.h` 将不支持的 0514 GET/PUT 换成 AP BACKUP1 bit3 引用，保留父/子 token；继承 AP bit3 只借用，最后一个 parent 只 CLR 自己新增的 bit3，不碰其它 bit/XPU。新增两次 AP/global/STA、MM_2 ACK 与 AO/infra0 ACK 资格检查；SET/CLR 已发后的错误锁存且保留 token/依赖，不补偿/重发。`cam_receive_power_take` 在 acquire 错误时也记录非零 faulted token，防止 close 假称未持有；ABI 尺寸/旧 smc 字段保留（此后始终0），MM 日志标为 `CSI/MM`，新增 `X9 MM POWER GET/PUT: backend=hwccf ... stage/ap/global/sta/done/ack2/infra/errno`。这是供电引用，**不是 SMMU namespace/semaphore、DMA 或画面验收**；原 normal-bank 冷启动与退休守卫仍适用。
- **编译**：`prepare-local.py --check` exit0。首个单对象编译因新 `delay()` 缺 `kern/clock.h` 声明 exit1，原始日志 `x9/out/camera-mm-hwccf-compile-20261117.log`；补 include 后 `x9_mm_power.o`、`sweet_x9_camera.o` 两对象正常编译 **exit0**，日志 `x9/out/camera-mm-hwccf-compile-r2-20261117.log`，产物在对应 `x9-camera-mm-hwccf-compile-r2-20261117/`。未运行测试套件/测试脚本，未二次检查编译产物。
- **提交/整包结果**：源码 **`06166db7`**。首次 `python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-mm-hwccf-r28-20261117` 实际 exit1：**104/104 overlay 对象（另3 prebuilt）及 kernel 链接成功**，但 PATH 优先 Homebrew Python3.15，`boot-xnu.py -I` 在 LZ4 打包时报 `ModuleNotFoundError: lz4`。失败目录/原始日志完整保留，未覆盖。随后用 **`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py boot --kernel /Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-mm-hwccf-r28-20261117/kernel.development.sweet --boot-config x9/build/boot-device.json --boot-arg x9_motion=3 --name x9-camera-mm-hwccf-r28b-20261117`** 实际 **exit0**。复用本次已链接 kernel，只重做未完成的 shim/boot 打包；内置检查通过后没有额外解包/hash/反汇编或测试。
- **最终候选**：`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-mm-hwccf-r28b-20261117/boot.img`；构建器报告 **Image 32215040 B、boot 15085568 B**。完整日志在 `x9/out/camera-mm-hwccf-r28-20261117-console.log`、`camera-mm-hwccf-r28b-20261117-console.log` 和各输出目录的 `build.log`。沿用 GPU280，**无需 mailbox**；没有 CI run ID（本机编译）、新设备启动或 RAW/预览证据。源码与本交接提交后推送 `origin/feat/camera-integration`。
- **发布实绩**：源码 `06166db7`、主交接 `73ba2e7a` 已推送远端 `feat/camera-integration`（`01e486ee..73ba2e7a`）。两次普通 SSH push 因连接关闭各 exit128；随后 `GIT_TERMINAL_PROMPT=0 git -c credential.helper= -c 'credential.helper=!gh auth git-credential' push https://github.com/tosasitill/ios_plj110.git feat/camera-integration` 实际 exit0；使用本机已有凭据，origin 配置保持原值。本条发布结果补记随文档提交推送。
- **下一步/验收边界**：用户手动 `fastboot boot` 上述 **r28b**，进入 Camera.app，尝试后摄启动/关闭及前后切换并提供新 `~/NNN.txt`。先验 `X9 MM POWER GET ... backend=hwccf ... stage=6 ... ack2=c0000000 ... errno=0`（ack2 为完整寄存器，允许其它位）、MM RECEIVE→MAP/TAKE 与失败后的实际 `CSI/MM` retained，再看 SMMU 快照、NATIVE CONFIG、物理 RAW/SOF/DONE 与 PixelBufferHandler/实际预览。旧 `smc=0/0` 此后仅是兼容字段，不能据此认定 SMC 成功。MM_2 父链 ACK、SMMU normal bank 和阶段 B–E 均须按真实新日志验证；**相机全功能目标仍未完成，不能把本轮编译当相机完成**。

#### 2026-11-16 相机合入最新main与整包重编（进行中）

- **用户范围/基线**：用户明确要求“合并重编”，单代理在既有 `feat/camera-integration` / `.worktrees/camera-integration` 执行，原相机head **595f28f8** clean；fetch实际exit0，锁定当次远端main **6266ec76**，包含灵动岛/20px状态栏 **00478b31**、全屏与UI默认on **bfbc0e51**、方向 **266d3de9** 与最新CPU/GPU/锁屏策略。本轮不合其它未发布分支、不修改主checkout或其它活动树。scratch仅本树 `x9/out/camera-main-integration-20261116/`，原r6/失败日志/私有输入/备份保持。
- **合并处理**：正常 `merge --no-ff --no-commit origin/main` 首次exit1，只有NOTES、`kern_exec.c`、`mkadt.py`三处冲突，无生产改动被丢弃；NOTES原两段逐字并存，mkadt同时保留camera与island参数/ISP发现/product artwork。MG函数兼容main UI2556与ALS的mode2 epoch、UI关闭+mode1回退清旧UI标记，保留相机mode3独立标记/必须camera2的守卫；显式mgwipe0仍关闭全部缓存改动，**不自动启用mgwipe3**。灵动岛默认on继承main，因此用户首次启动时按main原逻辑可能进行UI own-dylib安装及一次MG缓存重建（与旧r6默认不写缓存不同）；构建本身不执行根盘操作。
- **构建范围/待办**：保留相机boot-device的camera2、加lenses1及motion3；UI/fill按main板级默认on，不塞重复token，ALS1与其余基础参数保持，motion走既有shared SCP worker。GPU280不变，无新插件计划。`x9/xnu`相对双方无变更，无须修改或重建共享DXB源树；使用正常overlay构建。下一正常编译新UI/方向桥和冲突合并后的kern_exec，再固定clean SHA提交/push，以唯一r7整包。没有新设备/CI/功能验收，不运行测试套件或成功产物二次检查。
- **编译里程碑**：`prepare-local.py --check`实际exit0；resolved staged diff-check exit0、无剩余unmerged项。正常 `x9/display/build.py` **exit0/86112B**、`x9/motion/build.py` **exit0/95456B**（均原initializer linker warning），正常Builder `compile_one(kern_exec.o)` **True/exit0、149 warnings**；原session54664终态，UI/方向生成头仅在本树generated，不修改共享`.local`。下一merge commit/push，固定SHA构建完整r7，构建日志保留本轮out，不运行测试/额外产物检查。

#### 2026-11-16 iPhone式切镜连续性与视频能力：r6构建成功，尚未上机验收

- 单代理续用 `feat/camera-integration` / `.worktrees/camera-integration`，基线 `c3289258` clean；仍无新r4运行日志，最新330为mode1。scratch仅本树 `x9/out/camera-seamless-video-20261116/`。不操作设备/ADB/mailbox/分区，不运行测试套件，不二次检查成功构建产物。
- **确定缺口**：bridge仅公告Photo格式；未实现帧率与`VideoRecordingInProgress`控制。当前物理DMA/供电/退休合同只允许一颗传感器，冷STOP/close/start必有采集缺口；不能把保持graph、冻结旧帧/改PTS或复用未退休映射称iPhone同步双路无缝切换。八个exported epoch lifetime上限仍在，必须保留安全守卫。
- **参考/原件新证据**：hackios `userland/src/inject_media.c:406–440`注册Apple软件HEVC；本次20H392 DSC原VCPHEVC导出`HEVCVideoEncoder_CreateInstance`，`VCPHEVCRegisterEncoder`原4B为ret；H264SW的`VCPAVCRegisterEncoder`亦ret且无导出encoder create，不能把HEVC编码器登记成H264。原VT有`VTRegisterVideoEncoderWithInfo`及真实encoder-list keys。三个Mach-O UUID/loadcommands/nm与bounded原disassembly保留本轮out。初次`llvm-objdump --macho`忽略范围生成大日志，随后不用`--macho`获得bounded正文，未把大输出送上下文。
- **下一源码范围**：资格化CMCapture的D22 plist读入方法，按真实X9单源能力适配内存中的Photo/最高1080p30模式与软件HEVC注册；保留原音频节点，不篡改Camera.app或磁盘Apple资产。补真实帧率约束、稳定graph/host-time、切镜防抖与真实新帧的曝光/颜色过渡，不宣称同步融合/无采集间断。视频编码/音视频封装/Photos保存与原生变焦按钮仍需各自运行验收；本条非完成声明。
- **源码里程碑**：新增 `camera_profile.h/media_profile.m`，精确hook `+[FigCaptureFlatPlist flatPlistWithContentsOfURL:]` 原IMP `1a208b90c+slide`，先读取真实D22原plist，仅native ready后内存替换单源Photo/1080p30/720p30/640×480，保留D22 :0/:1/:2身份与原音频，去缺失virtual/depth并关闭未实现的布尔能力；不改磁盘Apple原件。原HEVC factory/VT及真实keys逐image UUID验证后注册hvc1，`IsHardwareAccelerated=false`，透明三参factory仅记录实际创建结果；注册失败Photo-only。帧率属性2..30通过物理line/step→frame length生效，录像固定有效上限、metadata保持实际时长，各输出拒绝旧PTS。后置目标稳定120ms/驻留450ms，真实STOP/close/start保留graph/pools/clock，只携bounded WB、不携旧LSC/CCM，旧曝光ns/gain按新sensor限幅seed，实际控制记录一致后8个新RAW亮度easing，日志显式记source_gap，不重放旧帧。
- **退休预算/并发修正**：新增32B只读 `GET_SWITCH_CAPACITY/0x9f`（旧88Bstatus/80Blenses不变），在writer同`camera_control_lock`读取8个epoch历史，remaining=limit-retired-1（正值时），预留当前流最终退休。Zoom setter和worker在STOP前拒绝无预算；拒绝时只回退仍相同的请求，不覆盖新zoom，zoom写入与worker目标验收同锁；没有新增lease/MMIO/映射释放。此限制仍意味着有限次数切镜，非无限无缝功能。
- **编译记录/修复**：bridge `compile-r1` actual exit0（142912B，当时后续变化未齐）；`compile-r2` actual exit1（5条错误：rate guard误放RequestStreams，无f/isp且continue不在loop）。原失败保留，已移到worker曝光块后。最终 `compile-r3` actual exit0，原log报告bridge159616B（1 unused static warning +既有initializer linker warning）；无二次产物检查。新kernel ABI正常单对象编译在本轮out `kernel-compile-r1`，终态下一条补记。README已新增当前状态/边界，旧Photo-only/backend缺失标为历史。没有新运行日志/测试套件/设备动作，目标未验收完成。
- **kernel单对象终态**：正常Builder `compile_one(sweet_x9_camera.o)` **True/actual exit0，172 warnings**，原session52402已终态，新32B预算ABI与现有native源共同编译通过；bridge session11307亦已终态。接下来固定源码commit/push后以唯一r5整包；保持camera2、加lenses1、GPU280和其余MG参数不变，不启用未确认的mgwipe3。本轮没有CI run ID、设备启动/编码/保存结果。
- **r5构建/能力修正**：源码 **d3a17b05 / tracked clean** 的完整 `all --boot-config x9/build/boot-device.json --boot-arg x9_camera_lenses=1 --name x9-camera-seamless-video-r5-20261116` **actual exit0**，103/103 overlay、3 prebuilt，kernel/bridge/shim/boot成功，原log Image31977472B/boot15036416B；session79713终态，无二次产物检查。源码push actual exit0 c3289258..d3a17b05；gh run list实际exit0 `[]`，没有CI run。随后源码复核发现原2fps下限超出广角/前置的16-bit帧长能力（4361ns×65532、4415ns×65530），不能公告可固定2fps并在setter成功后worker才ERANGE。因此统一四颗镜头共同范围为**4..30fps**，更新格式、preset、setter、ISP和默认值，2fps请求提前unsupported；上述2..30描述为修正前历史。r5保留为成功中间包，**不推荐其低帧率能力**。下一唯一r6整包使用新提交，不覆盖r5或失败日志。
- **帧率修正编译**：bridge `compile-r4` **actual exit0**，原log159616B，与前次相同的1 unused static warning和initializer linker warning，session49824已终态；这不是二次验证旧产物，而是新4..30源码的正常编译。未编译期间改输入，未运行测试套件/产物重检查，原r5与r2失败记录不动；下一提交后完整r6以固定clean SHA构建。
- **最终r6终态/发布**：固定源码 **`4ad0671671002cb3fd3efafacdc9e91b14e3aa14` / tracked clean**，正常 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_camera_lenses=1 --name x9-camera-seamless-video-r6-20261116` **actual exit0**；103/103 overlay、3 prebuilt，bridge159616B，kernel/bridge/shim/boot全成功，原log **Image31977472B / boot15036416B**。session72028已取终态，没有成功后二次解包/hash/反汇编/复建对比。帧率修正源码push **actual exit0 d3a17b05..4ad06716**（session73806终态）；最终gh run list亦 **exit0 `[]`**（session52362终态），本轮无CI run ID。
- **候选/证据路径**：最终只推荐本树 **`x9/out/x9-camera-seamless-video-r6-20261116/boot.img`**；同目录保留 `source-head.txt/source-status.txt/boot-input.json/local-input.json/command.txt/full-build.log/full-build.exit/build.log`，输入SHA固定4ad06716，status空。r5成功中间包与compile-r2失败保留，不覆盖/重命名。GPU280 unchanged、没有新plugin-package，设备保持280则无需mailbox；mgwipe仍未启用。最后仅notes-only提交/push，不修改构建输入SHA，不合main或删除活动相机树宣称功能完成。
- **下一最小运行验收/限制**：用户从主checkout执行 `fastboot boot .worktrees/camera-integration/x9/out/x9-camera-seamless-video-r6-20261116/boot.img`，先原生后置移动预览→前置→照片保存，成功后一次0.6→1→3物理切镜；若原生按钮缺失，记录该事实，不把现有ZoomFactor路由说成已接UI。再录像10–15秒、停止、相册重开并核画面/声音同步；保留新 `~/N.txt` 和屏幕现象。日志应逐阶段验native mode2/four-ID/close→RAW/handler→PROFILE实际D22适配→VIDEO registration/真实factory→recording start/stop，以及真实 `source_gap`。**仍无r6上机/编码/音视频封装/Photos保存结果，完整相机与真正iPhone无缝功能未完成**：底层仅单路冷切，8 exported epoch上限保留，native0.6/3/虚拟三摄、同步预热、多路采集、硬件VENC、AF/OIS/标定均未闭；不能用保持graph/新RAW亮度easing或工厂创建0代替无间断/录像验收。

#### 2026-11-16 四镜头接线与后置变焦切换：r4 完整构建成功，尚未上机验收

- **开始状态（历史，以下源码里程碑已推进）**：当前树 `feat/camera-integration`，基线 `b1f6bce3`。上轮未提交源码已接收；开始审查并正常编译，不执行测试套件或设备操作。最新有效运行证据仍330/mode1，r3/mode2未验收。
- **开始时缺口（非最终状态）**：长焦已有物理端口/ID:2、独占 RequestStreams 与冷供电/字节表；超广角当时仅 catalog/CSI plan，banked word init 与 SID5 DVDD/DVDD1 别名转换未闭，保持 ENOTSUP、不公告 READY。PHY open两路硬编码、tele rate-band/CTLE与主摄CAMTG7选择冲突待核对；这些随后按下述源码里程碑修正。D22原件无超广角/虚拟三摄preset仍为真实未闭边界。
- **源码里程碑**：超广角新增 bank-aware word table validator/loader 与混合16-bit init/8-bit控制协议，生成pre/init的short/long+preview五表；真实0010高字节07选择short，先12-pair pre-init、5ms延迟、init、preview，最终page4000。禁止控制页stream/hold/非pre-init复位写，内部4001:0100不当stream；不把未知byte邻寄存器当readback要求。新增I2C9/CSI2/WL6/SID5电源、drive字段shift3；DVDD/DVDD1只建一个journal，SN-LDO请求1.008V按厂商selector公式取1.010V(51)，限定本boot冷拥有的51→50请求转换，不借用/重调live foreign rail；该实现遵循pw_seq请求意图，**未证明Android regulator core两consumer请求的最终实机电压**。失败保留journal/前置依赖，无复位重试。
- **长焦及共享链审查**：真实tele PHY1/CPHY1.624G使用<2.5G设置、默认CDR10，static CTLE只BW3/CDR0、不套主摄的force/CDR9；主摄DT mclk ID23与厂商CAMTG4/pin208一致，修旧CAMTG7配置为off70/shift0/status7/update24。receiver changed比较改按真实port找sensor map；OUTMUX last_vc改profile值(tele0/UW3)，不再误拿CSI端口作VC。新增只读80B GET_LENSES，旧88Bstatus不变。四路校准仍来自本boot bounded FDT/devinfo，无固定efuse词/Apple ISP compatible。
- **iOS接线**：保持D22独立tele port/ID:2，不冒用虚拟dual ID:3；后置wide流按请求ZoomFactor的本地X9 UI锚点0.6/1/3选择真实UW/wide/tele，实际crop=global/base，metadata报告真正应用的global zoom。worker在RAW/handler外的边界收到严格STOP+close成功后才open/start下一颗，保留同graph、推进run_epoch/重置AE/AWB；待拍票据存在则延后切镜，切镜中拒新票据，失败不回滚/重试。前置不参与后置路由。没有伪同步组/融合；**D22原preset没有UW/三摄，0.6/3按钮尚未闭，锚点不是实测光学校准、不是Apple虚拟三摄合同或无缝切换**。
- **正常编译**：原compile-r1的bridge/ctl exit0、kernel True/exit0/166 warnings；word路径compile-r2的bridge/ctl exit0、kernel True/exit0/171 warnings；最终compile-r3 bridge/ctl exit0（原log报告136848B），kernel True/exit0/172 warnings。三个编译句柄均已实际终态。原命令/日志/exit及分析原件在本树gitignored `x9/out/camera-multilens-20261116/`，未执行测试/套件、产物二次解包/hash/反汇编/对比、CI或设备操作。
- **准入/构建前计划**：默认仍只dual；新增opt-in `x9_camera_lenses=1`启用四镜头准入（旧tele/uw单路opt-in保留），不是绕过ID/close资格。boot-device原546B，加该token565B，未改其他token/MG缓存。当时下一步为固定源码正常完整r4候选构建（现已成功，见下）；仍须用户手动上机并提供新mode2日志，核四真实ID/close、RAW/handler/屏幕、0.6→1→3路由日志及拍照保存。8 exported-epoch pin上限、AF/OIS/光学校准/虚拟三摄按钮与真实拍摄验收仍缺，不合main宣称完成。
- **完整r4构建终态**：固定源码 **`b32d21a635130448376721dfe66eec15ab32b08b` / tracked clean**，执行 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --boot-arg x9_camera_lenses=1 --name x9-camera-four-lens-r4-20261116`；原session **44708** 已取终态 **exit0**，`full-build.exit`亦为0。原日志报告 **103/103 overlay、3 prebuilt**，kernel link/strip、相机bridge、四路bounded CSI校准shim与boot pack成功；**Image31961088 B / shim-boot15024128 B**。构建内置检查保留，没有测试套件、构建后二次产物检查或设备操作。
- **提交/原件**：源码 **`b32d21a6`** 已push成功 `b1f6bce3..b32d21a6`；`gh run list --branch feat/camera-integration`实际exit0、返回`[]`，无新CI run ID。确切源码/开始状态/boot输入/命令/原构建日志/exit分别保留本树 `x9/out/x9-camera-four-lens-r4-20261116/{source-head.txt,source-status.txt,boot-input.json,build-command.txt,full-build.log,full-build.exit}`；本次仅追加交接并正常commit/push，不以事后交接SHA替换构建输入，不合main、不删相机树，不碰其他工作树或共享私有输入。
- **当前候选/下一最小权威验证**：用户在主checkout手动执行 `fastboot boot .worktrees/camera-integration/x9/out/x9-camera-four-lens-r4-20261116/boot.img`。沿用GPU280，未生成新plugin-package，设备保持280则无需mailbox；本版未启用`x9_mgwipe=3`。先看本次mode2四真实ID/physical close与CSI校准资格，再核真实RAW/handler/连续屏幕预览；请求ZoomFactor为0.6→1→3时核`rear lens switched`和真实镜头画面，再前置、关闭重开、拍照保存/重开。**未有r4上机证据，四摄功能不能称完成；原D22界面0.6/3按钮未接齐，不能保证可从Camera.app直接发出这些请求**。图标若仍缺先收本次日志/界面现象，不擅自刷新MG缓存；后续优先从最早失败阶段修，再闭UI/虚拟三摄合同、光学校准、AF/OIS与录像/照片验收。

#### 2026-11-16 相机图标缺失：补 metadata-only ISP 发现与独立一次性 MG 刷新

- **用户现象/范围**：用户报告桌面没有相机。仍没有新增数字日志（最新330，mode1）；本地根盘工作克隆只读检查 `/Applications/Camera.app/Info.plist` 发现 app 存在、bundle 为 com.apple.camera、UIRequiredDeviceCapabilities 只有 arm64、SBAppTags 为 still-camera；旧克隆 IconState 中没有 camera。这些是克隆观察，不冒充当前设备 App/LS/限制状态。
- **确定源码缺口/参考**：此前 r2 的 mkadt 没有 `/arm-io/isp`。固定 `.local/references/hackios-xnu-kernel/xnuloader/loader.c:3226–3233` 在 camera_requested 时放无 compatible/device_type/reg 的 isp/camera-rear，专供 MG still/rear-camera 与 SpringBoard Camera.app 发现。当前20H392原 libMobileGestalt 在 `19c56341c/19c56c59c` 走真 ISP path 与 camera-rear hashed key；`_MobileGestalt_get_cameraCapability` 的原CFString等于 `MD5(MGCopyAnswer+still-camera)` base64（nv4RoLkNoPT0/rsO8Yaiew），rear/front/video exported key同样对应资格。原 helper 19c557414 取实际 IORegistry property，19c559e74 实际执行 MGCopyAnswer+property 哈希匹配。原cfstring tagged pointer保留raw，仅mapped-low36字符串读取，不将其称live/authenticated pointer；answer-table候选查找未命中，不补称已闭原native callback表。
- **实际修复**：只有 `x9_camera=2` 打包才加 metadata-only `/arm-io/isp` 的 camera-rear/front=1，并独立于 torch 放 `/product/camera` 节点。没有 Apple ISP compatible/device_type/reg，不修改 Apple Camera.app、LaunchServices/桌面布局或 physical READY/实际RAW守卫。新 opt-in **x9_mgwipe=3** 仅 native mode2，在 PID1 用独立 `/private/var/x9/mg/refresh-camera-20261116` 标记复用两份 MG 缓存重建；先前 ALS epoch不能使它跳过，两次 unlink 成功后才写标记。1/2旧语义保留，默认仍0；构建不执行此操作，须用户确认后才手动启动刷新候选。
- **当前编译/记录**：正常 Builder `compile_one(kern_exec.o)` **True/exit0、145 warnings**，session54272已终态；没有测试脚本/套件或设备写入。原始 app tags、MG符号/反汇编/headers、选中raw/string/hash映射、失败原件/未命中候选、source-input与exec-compile log/exit/result 保留本树 gitignored `x9/out/camera-icon-20261116/`。即便发现节点修复成立，也不声称这是设备图标唯一原因或已拍摄成功。
- **r3 构建终态**：固定源码 **`ab6df9a80725f4f539527689dae1751f341d45cd` / tracked clean**，正常 `build.py all --boot-config x9/build/boot-device.json --name x9-camera-native-preview-r3-20261116` **exit0**；**103/103 overlay、3 prebuilt**，kernel/bridge/shim/boot 全部成功。同 kernel 用 `build.py boot --kernel x9/out/x9-camera-native-preview-r3-20261116/kernel.development.sweet --boot-config x9/build/boot-device.json --boot-arg x9_mgwipe=3 --name x9-camera-native-preview-r3-mg-20261116` 打包 **exit0**；原 session34385 已取终态。两版原构建日志均报告 **Image31911936 B / shim-boot14991360 B**。原日志、exit、source-head/status 保留各自目录；没有构建后二次解包/hash/反汇编/复建对比、测试套件、CI run ID 或设备操作。
- **候选/下一步**：普通版为本树 `x9/out/x9-camera-native-preview-r3-20261116/boot.img`，不额外刷新 MG；可选版为 `x9/out/x9-camera-native-preview-r3-mg-20261116/boot.img`，**尚未获得用户缓存重建确认，不启动**。用户从主 checkout 手动启动普通版可用 `fastboot boot .worktrees/camera-integration/x9/out/x9-camera-native-preview-r3-20261116/boot.img`；设备保持 GPU280 则无需 mailbox。仍无新 r3/mode2 日志、图标/真实预览/拍照验收；收到新日志后从发现节点→native ID/close→硬件/RAW→handler→屏幕的最早失败阶段推进。源码提交 `ab6df9a8`，本次交接随后正常提交/push，保留相机分支，不合 main 宣称完成。
- **镜头范围（回答用户2026-11-16询问）**：目前只有**后置主摄（广角）和前置**两路接线：`sweet_x9_camera.c` 的 sensors[] 仅主摄0x9186和前置0x0615；`camera_device.c` 的 cameras[2] 仅 Back/Front；status supported_mask 精确要求 back|front。**后置超广角与长焦未接入**，没有对应独立 sensor entries/供电与I2C/CSI路由/模式表/iOS stream；数字变焦不代表长焦支持。前置/主摄也仅源码与候选构建阶段，未通过真实预览、拍摄、AF/OIS完整验收。

#### 2026-11-16 相机恢复工作：NS GIC 阻断已修，正常 mode2 r2 已构建

- **范围/基线**：单代理进入现有 `.worktrees/camera-integration` / `feat/camera-integration`，起点 `4d6e4c5b`；合入当前 main `2c34c540`，只 NOTES 尾部并行追加冲突，保留完整双方交接；相机 native/source/repeat/AE/cancel 与 bounded shim handoff 全部保留，其他工作树和共享输入不改。合入带回已落地 board-default 音频/麦克风与蓝牙/AKS/振动修复。
- **最新观察**：`~/330.txt:481` 相机仍 `mode=1 / platform=0`，`:3006` boot args 为 `x9_camera=1`，不是 r1 mode2 镜像验收；目前没有当次 physical ID/RAW/PixelBufferHandler/屏幕预览证据。`~/324.txt:629` 相同 GICD 实读 `ctlr=12 / typer=a07f051e / group=0`；对应 pe_fiq 当前 NS 两安全态配置，与已实跑 BT IRQ 修复相符。
- **确定源码阻断/下一动作**：native source→`cam_irq_prepare_locked`→`cam_irq_controller_impl` 仍要求 `(ctl&0x11)==0x11` 和可见 IGROUPR bit；0x12 必然 EBUSY。按 NS view 修为 ARE_NS+EnableGrp1A(0x12)，不把读为0的不可见 group 当拒绝依据；保留实际 topology/line count/level trigger/RWP、route/priority/enable/pending/active、真实 IAR/EOI 与 DMA/退休守卫，不改 GIC 全局或安全组寄存器。随后单对象交编及正常完整 mode2 功能候选，不跑测试套件/构建后二次检查，不自行 boot/ADB/mailbox/写分区。完整相机目标未完成，后续以新 mode2 日志和真实预览/拍摄效果验收。
- **合入/单对象终态**：merge 为 `430fcfbc`；原 merge diff-check 因 main 原有 `0006-x9-bt-uart-irq.patch` 的 unified-diff context 空格+tab 报警（未改该继承补丁），本轮 camera diff-check exit0。上述 NS 守卫与 thread-only prepare 拒绝快照已实现；正常 Builder `compile_one(sweet_x9_camera.o)` **True/exit0、153 warnings**（与 r1 最后联合编译数量相同），原 session99090 已终态。没有执行测试套件/额外产物检查。`prepare-local.py --check` exit0，gh 当前相机分支 run list `[]/exit0`，没有 CI run ID。
- **完整构建/源码终态**：固定源码 **`c85ece2db3b8c1ea70389c57f010e1d9cd6aa49a` / tracked clean** 执行 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --name x9-camera-native-preview-r2-20261116`，原 session31556 已终态 **exit0**；**103/103 overlay、3 prebuilt、完整 kernel link/strip、相机 iOS bridge、bounded camera IRQ/CSI shim 和 boot pack 成功**。内置日志 **Image31911936 B / shim-boot14991360 B**。boot-device 仍 camera2、其他 token 不变，继承 main 音频默认值；不额外启用 BT root 安装/MG 清缓存。没有测试套件、构建后二次解包/hash/反汇编/复建对比、CI run ID 或设备操作。
- **产物/提交**：权威候选本树 **`x9/out/x9-camera-native-preview-r2-20261116/boot.img`**，同目录 `source-head.txt`、`source-status.txt`、`boot-input.json`、`full-build.log`、`full-build.exit`、`build.log` 保留确切输入/原结果；source-status 空。基线/单对象/输入校验/gh/摘录在 `x9/out/camera-resume-20261116/`；旧 r1 原件不改。源码与 merge 已 **push exit0 `4d6e4c5b..c85ece2d`** 到 origin/feat/camera-integration，本条最后 notes-only 正常提交/push，不以事后交接 SHA 替换构建输入。不合 main 宣称完成，不删除现有相机树/辅助树，不碰其他工作树或共享 `.local`。
- **下一最小权威验证**：用户在主 checkout 执行 `fastboot boot .worktrees/camera-integration/x9/out/x9-camera-native-preview-r2-20261116/boot.img`；GPU 仍280，无新 plugin-package，设备保持280则无需 mailbox。进入原生相机先观察后置随镜头移动的连续真实预览，成功后切前置、关闭重开一次；记录新 `~/N.txt` 与屏幕现象，预览成功后再核拍照、照片保存/重开与录像。先看本次 `X9 CAMERA FDT/IRQ/CAL`→mode2 native register/两真实ID与physicalclose→MM/CSI/CAM/MMPC/SMMU→SOF/DONE/RAW→graph/handler→真实屏幕，失败从最早阶段修，不拿成功注册/编译代最终效果。**当前仍无 r2 上机、真实 RAW/handler/预览或拍摄验收，完整相机未完成**；8 exported epoch pin 上限、partial/uncertain startup 保守 retain 仍在。此前 11-15 等待审计为历史状态，本轮已通过新 GIC 证据推进源码与候选，但尚不能替代新 mode2 运行结果。
#### 2026-11-16 方向传感器：内核 + iOS 接入（整包候选已构建，待实机）

- **最终v2交付（仅推荐此版）**：集成源码 **`147f9998`**；GitHub **PR #5** 第二次merge实际exit0，于`2026-11-16T12:20:41Z`合入main **`98f0be50`**，生产方向代码已发布。v2 `candidate.py`实际exit0/完整104/104 overlay（3 prebuilt）/iOS桥build+sign95456B；Image **33173504B**、boot **15372288B**。同kernel正常打包方向off-v2亦exit0，大小同上。两个整目录已无覆盖APFS复制到主checkout：**`x9/out/x9-orientation-native-v2-20261116/boot/boot.img`** 与 **`x9/out/x9-orientation-off-v2-20261116/boot.img`**。v2保留新main的fill/island默认on，off-v2仅关闭方向，不关闭显示功能；BT7文件/root1/UART IRQ、ALS3/PROX3、GPU280不变。双方参数605B，仍无测试/成功产物二次检查/设备结果/CI run。原非v2两版只作中间产物，**不要拿旧版off回退最新显示默认行为**。

- **范围/观察**：单代理 `feat/orientation` / `.worktrees/orientation`，基线 `ff9e900f`，不改其它窗口。原日志 `~/330.txt` 的完整 SCP LIST 已证明 type1 `icm456xy acc` / gain1000、type4 gyro / gain1000000、type27 dev_orient；本轮只做 **真实加速度 → iOS 横竖屏/face-up/down**，不把 type3 欧拉角或 type27 的未知枚举冒充 Apple 方向，不宣称指南针/陀螺仪/CoreMotion 全套已接入。
- **决定性 iOS 契约**：原 backboardd `BKOrientationManager 100079fe4` 匹配 built-in ff00/3；`BKAccelerometerInterface 100043cc8..100043cf8` 读 type13 XYZ。另一路 `CMDeviceOrientationManager` 仍走 Apple motion userclient（原 `191c78324` 送 selector0x20/3B），不能仅靠 HID 发布称自动旋转成功。新 own-code bridge 仅替换原五个 public 方法：start `1919ba350`、stop `191c6f84c`、available `1919baad0`、active `191c6f838`、blocking getter `1919c022c`；保留原 NSOperationQueue/CMDeviceOrientation 对象与 BK/SpringBoard 旋转锁/策略，不改 Apple 文件或 VM text。原 selector 表已用 `init` 与全部21个同类 selector 一致解码（共享池 base `18229979f`），不是乱猜方法名。
- **实现**：`sweet_x9_motion.c` 校验 LIST gain、type1/DATA/accuracy/单调时间/±32g，保 raw 与原 SCP time，整数换算 Android specific-force → iOS 16.16 g；`kern.x9_motion_{sample,status}` 有版本/长度与500ms receipt-age界限。共用既有 ALS 的唯一 SCP worker、完整 LIST 后配置，ENABLE/DISABLE/cadence 只在匹配 ACK 后生效；缺少 accel 只隔离本通道，shared fault 全部失效。`X9Accelerometer` 发布原生 esuc ff00/3，ReportInterval0 只关 push、保留 copy/方向桥取样，不重置 SCP。`x9/motion/orientation_bridge.m` 真实样本低通/150ms 稳定判向，掉数据返回 nil，不伪造默认竖屏/重复时间；stop/重订阅 generation 过滤排队回调。`x9_motion=3` 才安装 hash-addressed 自有 dylib 并注入 backboardd；已有 DYLD chain 原样保留。
- **收尾资格**：原 CoreMotion 完整 metadata 证明 blocking 为 `@16@0:8`、初始化为 `@28@0:8i16d20`，五入口精确 ABI 在安装前全匹配；原 stringForOrientation 表0..6是 Ambiguous/Portrait/PortraitUpsideDown/LandscapeLeft/LandscapeRight/FaceUp/FaceDown。原 blocking `1919c0404` 调同 initializer 并返回对象，不以名字猜 ABI。左右重力轴按 top-edge-left x<0→3 / x>0→4修正，仍待板级实测；换算约分5避免极端int32乘积溢出，initializer显式init-family声明保ARC ownership。回调保DATA有效、generation和最新对象一致。最初metadata读取用了extracted文件缺少共享selector池失败，改读已有只读SystemOS cache后成功；没有运行测试。
- **已构建/未验收**：初版 iOS arm64 build/sign exit0（95008B，后续 ABI/lifecycle 修正待整包重编）；`prepare-local.py --check` exit0，真实 recipe 单对象 `sweet_x9_motion.o/sweet_x9_als.o/SweetHID.cpo/KernelConfigTables.cpo` 均 True/exit0（首次脚本因未建 out 子目录失败，未发生编译；修正目录后成功）。没有运行测试脚本/套件、设备/ADB/mailbox/分区操作或构建后二次产物检查；`gh run list` 为 `[]`，无 CI run ID。原证据/本轮只读 SystemOS mount、精确编译日志在功能树 `x9/out/orientation-20261116/`。整包、提交/push 与卸载/归档结果随后记录；**横竖屏左右映射、旋转锁和实际自动旋转仍需新镜像实机验收**。


- **初版整包（中间版本）**：实现 `266d3de9`，吸收当时main `343b2409` 的源码merge **`43396f2`**（显示/状态栏/服务新改动保留；NOTES和gitignore冲突只保完整incoming并加入本轮段/生成头规则）。`candidate.py` 实际exit0：重新构建蓝牙runtime/root payload，完整104/104 overlay（3 prebuilt），新iOS桥build/sign **95456B**；隔离DXB来源为已保留rootboot13的编译树，自带0001+0006 UART IRQ，构建前按本仓库patch逐源diff成功。嵌入7个BT文件1110953B、8个已知旧版本，可保留完整原BT链路，不能冒称手机端蓝牙已验收。Image **33173504B**、boot **15372288B**，原rootboot13参数仅新增`x9_motion=3`（605B），GPU280/ALS3/PROX3/camera1不变，UI/fill缺省off。无测试套件/额外解包/hash/反汇编/复建检查、无CI run（gh为[]）、无设备操作。
- **初版镜像/回退（中间版本）**：主checkout `x9/out/x9-orientation-native-20261116/boot/boot.img`；同一kernel正常二次打包（不是重建检查）`x9/out/x9-orientation-off-20261116/boot.img`，实际exit0/Image33173504B/boot15372288B，唯一motion token为0，停止桥/加速度注册但不删除自有dylib，BT仍为root1。candidate自动生成的`boot-rollback/boot.img`是 **BT恢复Apple jobs**，不是方向回退，不推荐混用。本轮仅生成文件，**尚未启动或安装**；motion3第一次用户启动会在`/private/var/x9/motion/`安装hash-addressed自有dylib；保留既有BT PID1 root installer，若它尚未安装会写既有7文件并保Apple原件备份，需用户确认启动这些根盘安装行为。
- **下一验收**：用户手动临时boot（无需GPU mailbox，前提设备仍GPU280）；新日志按 `X9 MOTION SCP: LIST name=icm456xy acc gain=1000` → `X9 MOTION: ENABLE ACK` → `accelerometer valid`/真实DATA → `X9 MOTION USER: ... adapter installed/subscriber started/orientation=` 检查，ACK不当DATA。打开**支持横屏的Safari页面**，关闭旋转锁后分别左/右转、恢复竖屏、打开旋转锁重复；平放观察5/6、故障/陈旧时不造默认portrait。D22桌面可只竖屏，不能以桌面不转判驱动失败；板级轴正负/旋转锁/实际UI自动旋转与故障回调仍未实机证明。源码入口`x9/motion/`/`X9Motion.h`/`sweet_x9_motion.c`，完整输入/argv/编译和失败原日志保留在主checkout `x9/out/orientation-20261116/`；只读SystemOS本轮mount已按本轮attach plist正常detach exit0，其它mount/窗口/备份不动。
- **发布/收尾**：feature初次push exit0；停止前将完整out无覆盖APFS复制到主checkout（不追加hash检查），更新正式main合并/发布及清理状态。本轮整合时root main出现其它任务的未提交NOTES，故不覆盖/切换root、不更新它的index；以GitHub PR合main，精确结果随后记录。

- **同期main更新/候选版本边界**：PR #5第一次merge实际exit1（远端main已由其它工作推进，NOTES冲突），未force push。fetch后确认 **`bfbc0e51`** 生产更新使display fill和SpringBoard island默认on；这不是仅文档更新。保留完整incoming NOTES并插入本轮段，生产代码自动合并；原`43396f2`候选仍单独保全、**仅中间版本，不推荐**。将按新集成源码打包唯一v2（不是对成功旧镜像的复建/二次检查）；原rootboot13完整参数仍605B，只新增motion，不额外强制关闭已默认on的显示功能。原旧候选UI/filloff说明仅属于旧源码，不能套给v2。


- **最终收尾**：本轮只读SystemOS已正常detach exit0，v2两个交付目录已复制exit0；源/输入/argv/完整build与所有失败原日志即将无覆盖归档主checkout `x9/out/orientation-20261116/`。保留root其它任务的未提交NOTES，feature在自有树已ff到published `98f0be50`；本条交接只改NOTES，将直接fast-forward push remote main，不切换或覆盖root/index。随后仅删除本轮自有worktree/本地及远端feature；最终exact commit/push/归档/删除状态写同out `finalization.json`，不复检或重编成功v2，也不称手机方向功能已完成。

#### 2026-11-16 Wallet 交通卡目标：分门槛修复计划（未开卡）

- **实际整合/发布**：报告 `4a42639`、初始交接 `b245f4d`；吸收同期 main **`74b12c98`** 时仅 NOTES 插入点冲突，保留完整 incoming 原文后只插本轮目标/交接，merge **`a1ee044`** actual exit0。feature push、GitHub **PR #4** merge actual exit0，`2026-11-16T12:13:27Z` 合入main，merge **`398870ba`**；报告祖先复核 actual exit0。仅2份文档，无本轮代码/构建/启动/资金结果；命令/退出码与离线依据核对结果在同out的 publication/finalization。停止前将私有证据无覆盖归档到主checkout同名out，清理自有worktree/ref；不操作其它会话。

- **范围/报告提交**：用户把目标缩到原生 Wallet 开交通卡；单代理隔离 `feat/wallet-transit-plan`，main 基线 **`84e1aad5`**。报告 **`4a42639`**：`docs/2026-11-16_reverse-wallet-transit-plan-report.md`。仅被动库存/复用日志、规划及 Git 发布；不切换或改写主 checkout 的并发 SMS 工作，不操作其它工作树。无构建/CI run ID、新启动、ADB/fastboot、RF/APDU、登录/身份注册、开卡/充值/迁移/交易、SE/根盘/分区写入。
- **观察/纠偏**：根盘克隆确有 `com.apple.Passbook` 主程序，位于 removable application bundle 根，不能仅查 `/Applications` 就断言 Wallet 缺失；原件 passd 发布 device-registration/payment/library/account/nf-events 等服务。330 已捕获 **06:06:16.484Z `Secure element available: NO` / broker未注册，以及 Needs Registration YES / Has Certificates NO**；本地 supported-device YES 不等于平台认可 X9。06:06:37.337Z 无iCloud账户、CloudStore **PKPassKitErrorDomain -4008**；没有目标卡 add-card、发行请求或远端拒绝证据。NFC 板级 SN220T/I²C14/eSE SPI5 及 nfcd 打开会话失败、ADI -42410/BAA/activation identity、固定已解锁软件 AKS 基线复用前轮，不重采身份/卡片材料或推测错误枚举。
- **修复门槛/推断**：G0 选卡/地区/iOS16-D22资格/资金路线 → G1 原件UI/XPC/真密码keybag基线 → G2 目标路由实际账户/证明依赖 → G3 NCI/eSE/nfcd管理会话 → G4 平台接受SE签名/证书/安全域与发卡资格 → G5 同一次资金授权/个性化/激活 → G6 快捷交通/off-host routing/锁屏恢复 → G7 真终端交易扣费一致。**G4 必须早期调查，不等全UI/读卡做完；当前未知，既未承诺可行，也未证明必然失败。**完整 iMessage、App Store、指纹、CoreNFC reader API 和备用电量不是默认前置；若卡种需发行方App或已配置付款卡，再明确增加该依赖。
- **验收/回滚**：最终以发行方 active、SE有效卡应用、Wallet余额/状态及锁屏真实交易核对；卡面/UID/APDU成功/HTTP200/扣款单步不能代替完成。账户/平台注册、SE写入、开卡/充值/迁移/删卡/消费逐项确认；**分区备份不恢复eSE卡片/密钥/计数器**，不能以还原userdata或刷回Android冒充事务撤销。故障按同发行业务ID查询恢复，避免重复扣款。
- **复核/下一步**：私有证据 `x9/out/wallet-transit-plan-20261116/`（scope/timeline/workitems/Wallet库存/脱敏passd证据/前轮限定证据/field-journal）；`verify-plan-evidence.py` 仅读已保留JSON，**actual exit0**，8项规划依据核对通过，不代表实机资格或交易通过。报告 staged diff检查/commit actual exit0。下一先问具体城市/卡名并锁资金路线，再按目标入口做不扣费的资格/SE契约调查；所有状态变更先确认。提交/合并/push确切结果另补，规划完成不等于交通卡目标完成。

#### 2026-11-16 显示 / 灵动岛 / 状态栏：用户要求默认开启（已构建，未实机）

- **基线/范围**：单代理隔离 `feat/display-default-on` / `.worktrees/display-default-on`，真实main/origin **`9233c1ac`**。用户“默认打开”按两开关执行：`pe_serial.c`共享board table `x9_fill=1`、`x9_ui=1`；FB/HID从裸`PE_parse_boot_argn`改用同`sweet_x9_arg`，打包器原`x9_boot_defaults`自动投影subtype2556，无需额外token、无bootargs膨胀。保留414x920pt@3x，scanout拉宽与触摸逆映射不变，非X9不启用此路径。
- **关闭/恢复**：显式`x9_fill=0`仅关闭铺满；`x9_ui=0`禁用SpringBoard hook并打包ADT2436，但不保证清旧MG缓存。两项均关用`x9_fill=0 x9_ui=0`；完整缓存恢复另加单次`x9_mgwipe=1`，之后继续显式0镜像（不带mgwipe）。新默认镜像会再次启用UI，不能把“普通不传参数”当保持关闭。hook仍只覆盖SpringBoard竖屏主屏状态栏20px，不扩大应用注入/移动原生岛窗口。
- **执行边界**：用户已要求默认开启，未来该镜像首次启动沿原自有dylib/epoch安装和MG缓存重建路径；本轮不操作手机、不写根盘/分区。按x9/AGENTS只编译/打包，不执行测试脚本、不额外解包/反汇编/重算成功镜像hash；原测试断言同步新默认但未执行。旧v2镜像未改且仍为旧默认off kernel。构建输入/原日志在 `x9/out/display-default-on-20261116/`，确切build/boot结果见下条；无CI run ID或实机验收。

- **构建/交付实际结果**：生产提交 **`bfbc0e51`**；`prepare-local.py --check` actual exit0。`build.py all --boot-config x9/build/boot-device.json --name x9-display-default-on-20261116` actual exit0，**103/103 overlay（3 prebuilt）**、link/strip/shim/boot均成功，自有dylib签名86112B；日志Image **31903744B**、boot **14938112B**。同一kernel的两次boot-only关闭与rollback包装均exit0（相同Image/boot大小），保留所有既有warning原日志。默认输入546B且不含两开启token；off564B、rollback576B，均不改设备基础参数/不删no-op参数。没有额外解包/hash/反汇编检查，未跑测试/硬件场景。
- **主目录镜像**：整目录无覆盖迁回主checkout：`x9/out/x9-display-default-on-20261116/boot.img`（新默认双开）、`x9/out/x9-display-off-20261116/boot.img`（显式两项0）、`x9/out/x9-display-default-rollback-20261116/boot.img`（两项0+单次mgwipe1）；输入及console/exit在主checkout `x9/out/display-default-on-20261116/`。旧fill-v2/UI镜像未改；GPU280 allowlist不变、无plugin-package，设备仍为280时无需mailbox。本轮无设备/root/cache写入，键盘/边缘/点击/岛/剪裁/性能仍未实机验收。合main/push与清理实际结果另记，不覆盖主checkout SMS工作或其他分支。

- **同期main整合**：构建后其它窗口将SMS文档checkpoint合main/origin **`3c1ca34a`**并占用主checkout main；本轮不切换或更新该工作树。最初ff-only整合被安全拒绝；随后merge NOTES冲突actual exit1，保留完整incoming SMS交接，仅顶部插入本轮默认开启段。同期只3文档、无生产delta，`bfbc0e51`新镜像无需重编。功能分支经GitHub PR服务端合main，不force main、不操作其他工作树。确切发布/清理结果见下条。

#### 2026-11-16 SMS ACK concrete owner（同 ID 内存退休静态收束；物理 ACK 仍缺）

- **实际git整合**：原报告commit **fb62045a**；main同期由其它任务推进 **9233c1ac**，正常rebase仅NOTES冲突actual exit1，保留完整stage2 incoming原字节后只插入本轮SMS小节，continue exit0，报告变为 **3c1ca34a**。已ff合main并 **push actual exit0 `9233c1ac..3c1ca34a`**，只有3文档/177行，无生产delta/新kernel/CI/boot。最后交接并将报告Mach-O delete符号精确写为 `__ZdlPv`，继续同feature commit/ff/push，再仅正常删除本轮本地 `feat/sms-ack-owner`；没有本轮worktree/远端feature/代理、未决reader或build/device worker，保留主out/其它树/共享输入/成功kernel/备份。所有确切argv/stdout/stderr/数值exit与最终sha/清理状态见同out `finalization-commands.json` 及各 `*.exit.json`；完整通话短信继续active，不以静态退休/发布文档替功能验收。
- **交付/下一边**：正式报告 `docs/2026-11-16_reverse-sms-id-retirement-report.md`；合同 `x9/out/sms-ack-owner-20261116/ack-owner-contract.json`，保留exact FS、retained slot proof、真实argv/cwd/stdout/stderr/数值exit及scope/timeline。13函数2128B/1import从保留库存资格，reader/promote全exit0；报告离线命令仅读既有产物，`ack-offline-replay-r1` exit0，不是测试或设备验收。曾猜错旧constructor/report文件名，stderr/纠正顺序如实保留，不以失败命名猜测作资格。下一从旧parser结果helper1007f72bc在**1007f7720的ACK function virtual+30**追真实callback类型/安装及原token→driver WMS37，同时复用旧parser callback100813f14的supplied ACK copy consumer；不再把app map退休当物理ACK，不重采本轮13函数或前轮handler。server+128 Registry/current concrete选择、publisher原init entry、GUID/record/COMMIT重开、MO及CALL owner/媒体仍缺，goal保持active。git提交/合main/push终态随后补记。
- **last-owner/观察面里程碑**：新control lastowner1000c6784..1000c679c/24B从control+18取原SmsOperation，tail调其virtual+8；原SmsOperation AP101561878+8真slot101561880→10042d120..10042d1a4/132B，析构只见string delete及Registry+58 pair-release10000f394..10000f3ec/88B，Registry最后owner副作用另未穷尽，不称物理ACK。count同Model+d8真slot101528de8→1000c0264..1000c0334/208B，core1000c6b40..50/16B确实读 **同Model+68**，两个52B queue invoke传回原u64结果。累计 **13新完整FS函数2128B、1新exact import**，promote/readers全actual exit0，原text/page零重采。当前production `X9Messages.inc`/`x9_mipc_messages.inc`/`coachz_baseband.cpp`只读核对：WMS37原command token/原QMI与QMUX owner→8602/8604 writer→physical TX result/host admission，与app Operation通知ID退休分开；没有证据支持把kSmsAcknowledgeMessage接到该kernel ACK API，故不加无caller adapter、不改生产行为。完整功能仍未验收。
- **同ID map退休已闭（条件静态）**：+e0的两个queue invoke1000c7964/1000c7998各52B把core返回值写原result byte；core **1000c7878..1000c7964/236B**按原u32 ID对同Model+60树作unsigned lower-bound/等值确认，miss返回0；match更新需要的Model+58 begin、**Model+68 size减1**，调实际detach100006610..100006994/900B，再以node+28调pair-release1000c6834..1000c688c/88B、真operator delete，返回1。全部从原库存promote actual exit0；delete精确import新资格1处，queue imports复用旧proof。pair helper只在control+8原count为0的last-strong分支调control virtual+10；**map已移除不等于Operation必已析构**，另有retrieve owner可延迟释放。此前原admission1000bfc18写同Model+60/node+20 ID/+28/+30 pair并加Model+68；这里同结构退休，不称COMMIT/物理ACK。已定位原admitted SmsOperation control AP101529378的+10真slot101529388→1000c6784/24B，尚未展开；下一只追其实际Operation析构边界。
- **首资格里程碑**：bounded两key reader actual exit0：10123c5e5=`kSmsMessageIsMobileOriginated`，10123c603=`kSmsMessageWasAcknowledged`。精确旧proof1010817b4为bool dyn_cast_or_default、101085084为xpc_bool_create，只复用无新import读取；因此handler跳过MO=true或ID=-1分支，否则保ID调+e0。原format6保留page5/actual slot raw证明 **101528df0→1000c05b0**，无新slot/page遍历；原完整FS1000c05b0..1000c0694/228B从库存promote actual exit0，只构造捕同object/原u32 ID的block与返回byte。尚未展开invoke1000c7878/1000c7964/1000c7998，不称map退休或物理ACK。
- **开始/范围**：上一轮checkpoint 2086b125/ff9e900f已合main/push，属progress，完整通话短信goal仍active。当前main/origin **ff9e900f** clean、旧句柄终态；本轮单代理 `feat/sms-ack-owner`、scratch `x9/out/sms-ack-owner-20261116/`。沿已资格handler1004bad6c的service+e0，先资格第二key10123c5e5/reply10123c603、精确imports1010817b4/101085084及同Model AP101528d10的slot101528df0，再追同u32 ID map mutation/下游ACK。两key保留data矿actual exit0/无覆盖，尚未读新原数据或资格名字；原text库存/80页chains只复用，不重扫、不重编/复检成功kernel、不测试/人工向量/设备/共享输入动作。


- **实际发布/分支清理**：GitHub **PR #3** `https://github.com/tosasitill/ios_plj110/pull/3` 于 **2026-11-16T12:11:33Z** 服务端merge **`74b12c98`**，head **`253babac`**，source **`bfbc0e51`** 已资格为published main祖先。feature正常ff到origin/main后转本轮工作树为分离HEAD；带精确远端head253babac lease删除远端feature actual exit0，正常 `branch -d feat/display-default-on` actual exit0（没有-D）。主checkout由其它窗口自行转main/推进SMS，本轮未切换/写它；其它工作树不动。生成头/local配置/pycache **35项**已无覆盖迁到主checkout `x9/out/display-default-on-archive-20261116/`，仅保留本轮`.local`链接待最后unlink，target/备份不动。最后此notes-only交接沿published main分离HEAD commit/push，再正常remove本轮临时树；确切最终commit/push/remove/remote结果留同out原日志。无新生产delta/重编/CI/测试/设备结果，默认开启不等于硬件验收。

#### 2026-11-16 display-fill：用户批准合 main / 删除分支（源码收尾，未实机）

- **范围/基线**：仅整合 `feat/display-fill`（head **`51fbd533`**，包含UI **`5af3d156`**）并清理该树/本地及远端分支；原UI与其它活跃分支不动。独立 `.worktrees/display-fill-main` 使用main，先ff到真实origin/main **`7ca335c5`**；NOTES合并actual exit1，保留完整最新main Apple服务/SMS交接后插入原fill/UI段，无生产代码冲突，相对feature生产delta为空。主checkout仍在SMS分支，未切换、未改其未提交NOTES。
- **产物保全**：功能树 **1034项ignored文件/链接**（除共享`.local` symlink）无覆盖APFS复制并逐文件SHA-256一致；清单/原两方NOTES/确切命令测试及退出码在主checkout `x9/out/display-fill-retirement-20261116/`，完整副本 `archive/`。`.local` target和所有备份保留。已交付四份v2镜像仍在主checkout原路径；镜像绑定 **`4cc05e25`** 源码，不冒称重编了新main。
- **验证/边界**：本轮复跑4项UI host测试、3项fill测试（LLVM22 ASan/UBSan）均actual exit0；四份主目录v2 boot SHA-256复核一致；无新kernel构建/CI run/设备启动/ADB/fastboot/mailbox/根盘写入。`x9_fill`、`x9_ui` 默认off；全局应用状态栏未覆盖，原生岛、键盘/触摸/边缘和性能仍待实机。用户批准的是git整合/清理，不是MG缓存/dylib持久写入许可。实际git收尾见下条；测试通过不是实机验收。

- **合并/清理实际完成**：merge **`84e1aad5`**，`git push origin main` actual exit0（`7ca335c5..84e1aad5`）；published main包含原feature **`51fbd533`**。归档后正常 `git worktree remove .worktrees/display-fill` exit0；最初从root SMS HEAD执行 `branch -d`被安全拒绝exit1，转到merged main工作树后正常 `branch -d feat/display-fill` exit0，没有 `-D`。以原head精确lease删除远端feature exit0；`ls-remote`复核该ref不存在。本地feature/工作树均不存在，root SMS HEAD、未提交NOTES SHA与起始状态一致；原UI和其它活跃树保留。此次收尾仅文档更新，无生产delta/重编/新CI/设备结果。最终交接提交与远端main确切SHA写 `x9/out/display-fill-retirement-20261116/finalization.json`，随后正常删除临时main工作树；不是手机功能完成。

#### 2026-11-16 显示铺满 / 兼容逻辑尺寸（v2候选已打包，待实机）

- 单代理 `feat/display-fill`，功能树 `.worktrees/display-fill`，继承已推送 UI 候选 `5af3d156`；root 的 SMS 未提交 NOTES 原样保留。scratch `x9/out/display-fill-20261116/`。未上机、未写设备、无 CI run。
- **运行时观察**：330.txt 的 AppleCLCD=1256x2760/stride5120、申报1242x2760；HID=同尺寸、offset=-8,0；真实 CA 层=1242x2760，a/b均0,0,1242,2760。123键盘历史不一致有107.txt记录，不能重新扩大 UIKit 为418.67pt。笔记“各7px”是名义居中；当前16B对齐offset=-8意味着实际左8px/右6px，用户称7pt仍需实测，不能冒称已核对截图。
- **方案**：保留414x920pt@3x；默认关闭的`x9_fill=1`在独立逻辑shadow合成后仅横向1242→1256，HID按同一像素中心逆变换。独立shadow不读回/重复拉伸已铺满的物理帧；原有GPU fence、retained holds、双缓冲与TE提交顺序保留。默认RAM surface亦逻辑尺寸，禁走direct/staged绕过，allocation failure拒绝显示启动而非display/input不一致。灵动岛仍为独立`x9_ui=1`候选、上轮缓存/dylib安装未获上机确认。
- **离线里程碑**：3项fill测试、4项原UI测试均exit0；exhaustive1256/2760映射、全帧/14重复像素/无反馈/padding/裁剪/bounds/offset/alpha/模拟键盘-overlay/20px通过。LLVM22+显式Xcode14 SDK的ASan/UBSan likewise exit0（仅host，不是手机）。两真实单文件编译r1均exit0；随后只改诊断日志，将在整包再编。初r0错猜Xcode路径失败、Apple15 ASan在Darwin27初始化崩溃、LLVM自动SDK27与旧linker不匹配失败均保留；显式SDK14解决。第一FB编译因ushort?:4→int narrowing失败，4u修复后通过，不隐藏失败。
- **同期main吸收**：第一次e8c5d2ec整包与四镜像构建/实际boot解压/ADT验证均exit0，保留原件；随后真实fetch发现main已到ff9e900f，含CPU/GPU/显示休眠的生产更新，不能交付会回退这些更新的旧基线。merge自动合FB，NOTES发生冲突；仅在完整incoming main NOTES顶部插入原有fill/UI交接，保留其它记录。将重编唯一v2镜像再交付，原镜像是中间产物，不推荐。
- **最终v2交付**：源`e8c5d2ec`、验证工具`484d0517`、吸收main `ff9e900f`的merge **`4cc05e25`**（含新CPU/GPU/显示休眠，不回退）；v2完整构建103/103 overlay（3 prebuilt）exit0。v2 ASan/UBSan的3项fill＋4项UI测试、prepare-local检查均exit0；v2四镜像同kernel打包/从实际boot解压到Image再核payload/ADT/真实参数/sha均exit0。boot **14938112B**，Image **31907840B**。初版最初独立留在feature树，仅v2四整目录无覆盖APFS复制到主checkout，逐boot SHA复核通过；收尾前全部ignored产物另行归档（见上条）。
- **主目录产物**：`x9/out/x9-fill-v2-20261116/boot.img`（fill1/UI0，603B args，SHA **561fe368f4646f79c0cc65fc08d20d13624687486e93f9f9982b9a4abe4b60db**）；`x9-fill-island20-v2-20261116`（fill1/UI1，598B，SHA **fd48b4612b6a6754d7de355e4422f66c02fb3d6652f89b179d1b42a140d6c504**）；`x9-fill-baseline-v2-20261116`（0/0，593B）；`x9-fill-rollback-v2-20261116`（0/0+单次mgwipe，600B）。只fill相对330参数仅多fill；联合/rollback为607B限额去掉本kernel未嵌BT-root payload的no-op参数，不是rootboot13BT整包，不删除已安装BT文件。GPU280 allowlist不变，无plugin-package/CI run/设备操作。
- **交接与边界**：报告`docs/2026-11-16_display-fill-report.md`，实际构建/失败/验证/复制证据`x9/out/display-fill-20261116/`；文档/交接提交 **04f0702**，`git push -u origin feat/display-fill` 实际exit0；首次发布该远端分支成功，确切最终head/remote结果记finalization。默认关闭；先临时boot只fill进行实际白底玻璃屏/边缘点按/123/emoji/中文/横竖屏/息屏/性能对照，再用户确认UI缓存和dylib持久动作后试联合。原UI仍仅SpringBoard状态栏hook，不是全app的20px位移，真岛/Jindo/safe-area/裁剪待测。未上机，未证明用户要求的全部兼容；用户本轮明确批准将默认关闭候选合main并删除display-fill树/分支；仍不将候选冒称硬件完成。
- 下一：用户临时boot v2实际验收；硬件键盘/边缘手势/旋转/岛避让/状态栏20px和性能待用户临时boot验证，未满足不能称保证。

#### 2026-11-16 状态栏 / 原生灵动岛（候选实现，未上机）

- 单代理 `feat/statusbar-island`，隔离树 `.worktrees/statusbar-island`，基线 **39a02fa8**；不碰 root checkout 的 SMS 未提交交接。scratch `x9/out/statusbar-island-20261116/`。用户要求：识别/启用路径、参考 GitHub 强开灵动岛、状态栏整个下移20像素。
- **观察**：最新 `330.txt` 同时有 `X9 STATUSBAR: ... DynamicSplitMedium ... errno=0` 和 SpringBoard `WARNING: Using overridden ...`；已是岛式两侧状态栏，但不证明原生岛。当前 ADT artwork-device-subtype=2436，兼容身份D22/iPhone10,3。20H392 `SBUIIsSystemApertureEnabled` **1a345a39c** 在SpringBoard检查有效subtype2556/2796；`SBSIsSystemApertureAvailable` **191555160** 的once body **1915551a0** 还检查FeatureFlags `SpringBoard/Jindo`，随后读MG `oPeik/9e8lQWMszEjbPzng.ArtworkDeviceSubType` 并比较2556/2796。root克隆的原件SpringBoard.plist里Jindo=FeatureComplete，非运行时flag返回证明。
- **参考/候选**：DynamicCowTS@372fed4738b576a190a78325203d8e48fe16fa03只改相同cache字段（2556 Pro/2796 Pro Max），不照搬其MDC/TrollStore利用或分辨率修改。增加默认关闭 `x9_ui=1`：ADT只改subtype2556，不改ProductType/board/画布；独立UI epoch一次重建MG缓存（显式x9_mgwipe=0可禁止）；hash-addressed自有statusbar dylib仅注入SpringBoard，AppSync/任何既有DYLD链优先、不覆盖。不写Apple二进制或GPT。
- **偏移路径**：20H392 UIKitCore UUID 1242978a-2c2c-3781-8d6c-9777edce2804，`-[UIStatusBar_Modern layoutSubviews]` **189244f3c** 每次把内层_UIStatusBar.frame重置为wrapper.bounds；hook原函数之后整体加 **20/nativeScale = 6.666667pt @3x**。布局不累加、不逐图标修改，不动桌面/触摸映射/画布。仅竖屏主屏；原生SystemAperture窗口不随该状态栏hook挪动；App内自绘bar尚未扩大注入，必须先证明桌面/锁屏剪裁、手势与计时器/音乐的原生岛。
- **验证里程碑**：iOS arm64 dylib编译/签名实际exit0（86112B）；kern_exec.c真实overlay单文件实际exit0（148条既有风格warnings，无error）。4项离线tests在匹配Xcode15/MacOS SDK下实际exit0（第一次用了CLT MacOSX27 SDK与旧linker，失败保留，不算通过）；prepare-local --check exit0。此初步验证之后完整构建/打包已完成，见下条；无CI run ID、无fastboot/设备写入。


- **完整打包里程碑**：源码 **00478b31**；`build.py all --boot-config x9/out/statusbar-island-20261116/baseline-330.json --boot-arg x9_ui=1 --name x9-ui-island20-20261116` 实际exit0，**103/103 overlay（3 prebuilt）**；Image **31887360 B**、boot **14929920 B**。同一kernel再打包baseline/rollback均exit0；从**实际payload header/ADT**复核candidate2556、baseline/rollback2436，启用与baseline的原始cmdline仅多一个`x9_ui=1`（593→601 B），Image内payload字节/sha及boot sha均匹配。候选boot SHA **7c9ac834fcceb221cc03bb18e840fc9eda22aad1fa64a667007f27048644ce7c**。
- **恢复/边界**：rollback用`x9_ui=0 x9_mgwipe=1`单次恢复ADT/cache并只移除自有UI epoch，随后普通镜像；600 B参数，因本main/UI kernel**没有嵌入BT root payload**，删去实际no-op的`x9_bt_root=1`以容纳mgwipe，不恢复/删除已经安装的BT root文件。本轮不是rootboot13 Bluetooth整包；不能拿此UI编译结果证明Bluetooth/XNU IRQ或其它硬件回归通过。首次rollback包装因证据脚本将adt.parse tuple当Node而未产生config，实际exit1，修正后exit0，失败日志保留。
- **交付/下一步**：三个整目录已无覆盖APFS复制至主checkout `x9/out/x9-ui-island20-20261116/`、`x9-ui-baseline20-20261116/`、`x9-ui-rollback20-20261116/`，均有boot.img；证据继续留feature scratch。报告 `docs/2026-11-16_reverse-statusbar-island-report.md`。原UI分支已提交并push；本轮由继承它的display-fill合main，原UI分支/树不在清理范围；候选默认off，不改root SMS未提交内容。首次启用会重建MG cache和安装自有dylib/epoch，需明确确认；无mailbox/plugin变化（GPU280）。未证明原生岛或实际20px效果，下一用户确认后测计时器/音乐+桌面/锁屏截图/剪裁与手势；通过才扩大到应用进程。


#### 2026-11-16 NFC / iMessage / App Store：只读验证门槛评估（未实现）

- **发布/复核**：GitHub **PR #2** 于 `2026-11-16T11:53:33Z` 合入main，merge **`1ba69325`**；报告 **`aff607fe`**、初版交接 **`1a9f0521`** 均为published main祖先。复核为限定DT/launch库存、旧日志脱敏摘要和源码分支，非新账户/射频测试。停止前归档私有scratch至主checkout同名out，删除自有feature工作树/ref；不切换或覆盖主checkout及其它活跃分支。
- **范围/提交**：用户续问接入/验证修复门槛；单代理 `feat/apple-services-assessment`，初始main/origin **`ff9e900f`**，不操作同期其它工作树。报告提交 **`aff607fe`**，`docs/2026-11-16_reverse-nfc-apple-services-report.md`。无ADB/fastboot、RF/APDU发送、登录/激活、短信、购买、账户凭据读取、根盘/分区/共享输入修改、构建/CI/设备启动；CI run ID：无。
- **NFC观察**：已有实机DT板级SN220T；I2C14@162c0000、0x28、400kHz，IRQ/VEN/CLKREQ/VBAT GPIO213/19/18/15；eSE SPI5@161b0000、19.2MHz上限。对应OPPO sn_nci源码提供IRQ/NCI收发与eSE冷复位回复隔离，不是已验证XNU会话。330持久化归档最新nfcd `_loadHW` 于06:06:12.859Z **Failed to open session; hwState=3**，枚举/精确失败点未命名。先NCI真初始化/标签响应→CoreNFC；HCE/eSE applet/Wallet/ApplePay另层，读卡≠复制卡或支付资格。
- **Apple服务观察/纠偏**：根盘克隆AppStore.app与原商店daemons在，归档appstored有实际运行，旧sweet README“没有AppStore”不代表当前。itunesstored/amsaccountsd有 **NSURLError -1009**；但同boot akd06:06:56出现 **End provisioning HTTP200→ADIProvisioningEnd -42410→AKAnisetteError -8004**，另有BAA签名 **devicecheck.baa -10001→MobileActivation -1**，不能以200称身份验证成功或把所有请求判成断网。apsd06:06:38 **No identity in keychain→Copy activation identity→_APSCopyActivationIdentity失败**；IDS有keychain/protection identity错误和madrid registrationError -1，内部状态不是已闭合的远端拒绝。
- **关键门槛/推断**：本地Hactivation/Activated与有效activation identity、APNs/IDS注册不同。330 AKS wrap sel10还unsupported；当前源码已含后续软件wrap修复，**未用新启动验证这些Apple服务**，也不会自动签发身份/实现BAA。fairplay.H2默认禁用的源码及330两个disabled DB状态已复核，是避免等待环不是DRM修复；AppSync Signing.m明确拒绝非零cryptid，安装/重签≠解密/商店许可/receipt。不能从串名判所有流程强制SEP，也不能承诺用常量标识/头修复服务端信任。
- **产物/下一步**：私有证据 `x9/out/apple-services-assessment-20261116/`（scope/timeline/hash18输入/launch+app库存/脱敏console/service-proof/current-boot-anisette/字符串leads/field-journal；最终归档到主checkout同名out）。优先新AKS非敏感持久化验证→按请求闭合-1009与ADI -42410/BAA下层依赖→activation identity/APNs→邮箱IDS双向收发；手机号验证SMS另排。NFC独立NCI到已知标签闭环，再CoreNFC。未宣称实际NFC、iMessage、原AppStore登录/下载/运行成功；本轮只完成评估。

#### 2026-11-16 SMS 同 ID 退休 / ACK（入口 checkpoint；下游仍未闭合）

- **真实ACK入口**：原已读registrar1004b9764..1004b977c供应 **10123c366=kSmsAcknowledgeMessage → handler1004bad6c，w3=3**，不猜flags语义。完整原FS1004bad6c..1004baf70/516B从上轮保留NEW原字节promote，actual exit0、没有新读原text。经Registry::getServiceMap与旧getter1000a95fc得到service；kSmsMessageID按unsigned转换，default **-1**（retrieve为0），另取尚未资格key10123c5e5，import1010817b4的default为0。service非空时，只有 `((ID == -1) OR 第二转换值)` 的bit0为0才在1004bae74实际调service virtual **+e0**，x1保原ID；否则以w0=0进入reply包装。第二字段、包装import101085084和reply key10123c603均未补猜测名字；不能把该virtual调用/回复认作map删除、Messages COMMIT或物理ACK。
- **两lead纠偏**：bounded原CString10123c31c实际为 **kSmsGetRecievedMessageIds**（保留原拼写），不是ACK/退休；handler1004ba4f4..1004ba8f4/1024B仅经同getter调virtual+100填两组u32 vectors，无request单ID。相邻10123c336实际 **kSmsGetIncomingMessageCount**，handler1004ba8f4..1004baa84/400B仅virtual+d8 count。两body都从保留raw资格、actual exit0；不以临时vector/free称map退休。其余7个原registrar精确名字资格exit0后只展开真ACK，不扩MMS/模拟/emergency/phone-reg业务；初始暂定文件名root-registered-retire-handler原样保留，角色以此纠偏为准。
- **范围/停止点**：本轮单代理 `feat/sms-retire-ack`，clean parent **5f406973**；checkpoint时观察main/origin已由其它工作推进至 **f1d91a25**，不冒充本轮输入。累计 **3完整原FS函数1940B、无新import资格**，全部复用上轮16998412B原text库存，80页chains不重遍历。报告 `docs/2026-11-16_reverse-sms-ack-handler-report.md`；raw/精确argv/stdout/stderr/exit、scope/timeline及checkpoint合同在 `x9/out/sms-retire-ack-20261116/`。无生产delta/测试/人工向量/compile/kernel/boot/CI run/设备/ADB/mailbox/mount/数据库/分区或共享输入操作，无待取reader句柄；最新有效CALL/SMS kernel仍41deaefb输入的 `x9/out/x9-call-end-native-owner-20261116/kernel.development.sweet`，未复检。
- **下一唯一窄边**：先矿保留data，再bounded资格10123c5e5/10123c603及精确imports1010817b4/101085084；读取同Model AP101528d10的+e0 slot **101528df0** 后只沿真target查同u32 ID/Model+60 map的callback、erase、size变化与下游Controller ACK，先矿旧body再promote原库存，不重扫原text或80页chains。server+128 Registry安装/current concrete选择、publisher原init entry仍条件化。完整通话短信goal继续active：GUID/record/COMMIT重开、MT物理ACK/MO、CALL逐动作owner/UUID/nativeepoch/双向媒体均未验收。git提交/整合/push确切结果随后记录；checkpoint不是完整功能完成。
- **checkpoint实际git结果**：原文档commit **2307f93c**；rebase时main已再推进为 **09fbe9cb**，NOTES冲突actual exit1，保留完整stage2 incoming原字节后仅插入本轮交接，continue actual exit0，文档commit变为 **2086b125**。已ff合main并 **push actual exit0 `09fbe9cb..2086b125`**；只2文档，无生产delta/CI/boot结果。离线报告命令仅读既有证据，`checkpoint-evidence-replay-r1` actual exit0；合同仍是边界未闭的checkpoint。最后notes-only交接沿同feature commit/ff/push，再只正常删除本轮本地feature；无本轮worktree/远端feature/新reader或设备worker，保留主out与其它树/输入/旧kernel/备份。最终命令/sha/数值exit写同out的finalization记录，不以尚未执行的收尾动作冒充成功。


#### 2026-11-16 屏下超声波指纹：只读可行性评估（未 bring-up）

- **最终发布/复核**：报告/交接经 GitHub **PR #1** 于 `2026-11-16T11:38:35Z` 合并，merge **`f1d91a25`**；祖先复核 `2998514a` 在 published main。同期main不断前进，两个atomic直接push均安全拒绝（refs未改变），改用服务端merge，未force push或操作其它工作树。纯文档复核，未构建/上机；退出前保留私有证据至主checkout `x9/out/fingerprint-assessment-20261116/`，清理自有worktree/ref。
- **范围/提交**：用户要求评估系统发现/注册、解锁和支付的真实难度；单代理隔离 `feat/fingerprint-assessment`，基线 main/origin `39a02fa8`；报告提交 **`2998514a`**、交接 **`da00836e`**，`docs/2026-11-16_reverse-fingerprint-bringup-report.md`。主checkout当时在SMS分支且有未提交交接，未切换或修改它。合并保留同期main **`d738222b`** 的完整SMS/电源交接；main当时由power-policy worktree占用，不操作该工作树。本轮无 ADB/fastboot、模板读取/录入、根盘/分区/共享输入修改、构建、CI或设备启动；CI run ID：无。
- **观察**：已有实机DT为SPI7 `0x161f0000`、mode0、35MHz上限，GPIO IRQ/reset/CS=5/7/85，dvdd 3.2V；面板 `ofp-fp-type=0x1c0`，与OPPO bit6 ultrasonic定义吻合。Goodix仅通用binding/注释线索，**型号、闭源算法/TA、实际采集与匹配位置未确认**。20H392根盘launch plist为 `biometrickitd.pearl`，限制 `pearl-sep`、匹配 `ApplePearlSEPDriver`；已有330磁盘归档跨多boot，4条backboardd该provider匹配均none，最后命中06:06:12Z，不是本轮新上机。框架同时有TouchID/Mesa名字和Pearl/auth-token leads，未证明可达Mesa后端，不作“D22绝无TouchID代码”断言。
- **关键门槛/推断**：当前SweetKeyStore固定unlocked(0x4)，lock/unlock/assert为无状态成功，wrap固定可逆混淆；不能把真指纹驱动SpringBoard UI解锁称为密码/keybag/支付安全已完成。分层L0外围→L1真识别→L2原生注册→L3解锁/keybag→L4 LA/ACL/密钥→L5最终支付；普通支付App逐策略实测，ApplePay另有SE/受保护平台/凭据provisioning/服务端约束，本轮不能承诺原生支付。TEE可研究自建可信后端，但Android HAT/MTK TEE不是Apple SEP授权，可改本地契约不等于Apple原生信任链。
- **产物/下一步**：私有只读 `x9/out/fingerprint-assessment-20261116/`（scope/timeline/workitems/evidence.json/field-journal，原DT/Apple文件hash及限定日志摘要），最终归档回main同名out保持路径可找。推荐先限定只读定位原厂HAL/driver/TA，判断XNU下是否能完成真实新录/匹配/错指拒绝/删除/重启闭环；密码/keybag状态机独立调查。**尚未证明任何真实指纹录入、匹配、系统注册、解锁或支付成功**；用户只要求评估，未启动实现任务。



#### 2026-11-16 CPU/GPU 原厂参考负载调频与显示空闲（编译候选，待实机）

- **范围/基线**：单代理 `feat/power-policy`，功能树 `.worktrees/power-policy`，实际 main 基线 **39a02fa8**；不改主 checkout `feat/sms-retrieve-route` 的未提交 NOTES，不动其它工作树/设备。overlay 三源文件加 resident include，未改 XNU patches、GPU280 allowlist、设备配置或原厂固件。
- **实证**：只读最新 `~/330.txt`：56–70s GPU平均4–18%，CPU请求104/135/162×26MHz，`ui_boost=39/39` 多次；GPUEB ceiling=7、actual idx7，旧 governor 仍请求idx0，71s restores约600。旧输入路径250ms touch/recent swaps→500ms lease→CPU所有簇top/GPUfastest，静态与实跑一致。旧显示路径 `sweetfb_stayon` 缺省1忽略off；X9没有Qualcomm fDsi，原off分支只在fDsi非空时关面板；VSync不停，正常亮度又将DBV0夹到2。
- **原厂来源/边界**：`.local/references/oppo-modules-dt-mt6993`（4d505a42）`vendor/oplus/kernel/cpu/uad/touch_boost.c` 默认小核1300000kHz、大/超大核0，100ms lease/150ms输入间隔；`cpufreq_uag_main.c` 默认target load80（multi_tl sys95、hmbird90是不同策略，未混用）。GPU参考GED按工作量调频及a16w `mali_kbase_csf_firmware.c:3523–3572`/PM IN_SLEEP、register macros：host-power GLB_REQ_STATE12..14 SLEEP=2，MCU_STATUS HALT=2且MCU_CONTROL bit31，wake先ACTIVE=0再清bit31+doorbell。这些是公开源码默认/协议，不宣称已采到PLJ110当前ColorOS闭源PowerHAL实际参数。
- **实现**：CPU使用XNU `processor_cpu_load_info`（recount累计user/system/idle，32bit tick差值可回绕），按每簇最忙核×当前请求/80选真实LUT OPP；500MHz以上最低OPP为idle，100ms降频滞回，touch只有短时小核托底，lease到期/灭屏即时去托底，不因换帧/按住手指拉全簇top。85–95°C渐进CPU cap，未知温度不超LK基线；电压仍交MCUPM。GPU按真实采样间隔与doorbell→fence累计时间缩放，不再touch→max；900MHz只是活跃渲染floor，idle降最低OPP；GPUEB ceiling/floor纳入目标，仅有真实工作时纠正stack drift，不循环对抗热限频，负回包/uncertain停GPU策略但CPU继续。
- **休眠实现**：灭屏清两边旧touch lease、事件序列防lost wake；无运行owner/async队列完成后先走原CSG SUSPEND+flush+AS1 detach，再经原host-power接口进MCU retention SLEEP，必须state ACK+HALT+doorbell-disable三者成立。AS0/L2/GPUEB POWER/ACTIVE及全部buffer保留；每个RenderOwner访问先ACTIVE→doorbell→fresh PING，display timer仅非阻塞触发唤醒。错误保留全部资源、不reset/不新DMA。`x9_gpu_sleep=0`禁MCU sleep但保留队列退休。确认display-idle且CPU后台peak<5%时 governor50→1000ms，display/job事件及时唤醒；删除每2s绑三核跑依赖加法的测频，暗屏日志30s。
- **显示**：X9缺省 `sweetfb_stayon=0`（其它板仍1，可显式1恢复），off由既有LK DCS DBV0真清屏、不走正常亮度最小值夹制，wake还原最新亮度；取消/排完已武装GUI后停软件VSync/通知，on重arm；不新增SLEEP_IN/共享rail电源写。触摸/SCP/无线/UFS原工作流保持，不冒称整SoC suspend；特别 `dv=1` INIT-only/DDR高票与`ug=5`安全floor不改，不能把本轮称为原厂整机待机功耗。
- **构建里程碑**：`prepare-local.py --check` actual exit0。首轮完整 `build.py all --boot-config x9/build/boot-device.json --name x9-power-policy-20261116` actual exit0，103/103 overlay，已链接/打包；它早于显示修补，不推荐该中间镜像。未跑测试脚本/测试套件、未重解包校验成功产物；无CI run ID（本轮是本地overlay构建）。最终源码提交后用唯一v2名称重编，结果待下条补记。scratch/原始构建输出 `x9/out/power-policy-20261116/`，没有设备写/ADB/fastboot/mailbox。
- **构建失败/修正**：源码 `15ec6524` 的最终v2编译 actual exit1，102/103：user client直接访问protected `AppleCLCD::fVsync` 两处报错；没有v2 boot，旧对象未用于链接。改为public `x9PowerTimerArm()`封装，并在亮屏重置deadline。最终改用唯一v3名称重编；不得推荐失败v2或首轮中间包。
- **待验收**：用户手动boot最终v3，连续触摸/滚动不应全簇top，停止活动后降频；锁屏应DBV0、停止VSync、`X9 GPU PM: MCU retention sleep confirmed`，随后`X9 POWER: display-idle ...1000ms`（后台负载忙可维持50ms）；电源键/自动亮屏需MCU awake+fresh PING、画面/触摸/亮度恢复，多轮锁/解锁和后台音频/蜂窝必须验证。尚无该策略的实机帧率/温度/睡醒结果，GPU热度仅保守CPU温度代理，不等价全SoC温控。

- **最终v3构建**：源码 **fb94992b**，`build.py all --boot-config x9/build/boot-device.json --name x9-power-policy-v3-20261116` actual exit0；103/103 overlay、Image **31821824 B**、boot **14929920 B**，沿用GPU280，未生成plugin-package，设备保持280时无需mailbox。随后吸收同期main **5f406973**（SMS源码报告+NOTES-only，生产源码未变），保留全部SMS交接；NOTES插入冲突按两边内容完整合并，不覆盖主checkout未提交记录。本轮无新CI/设备启动结果。最终可交付为v3，不是330/rootboot11的单变量整包，也未合其它活动实验分支。
- **实际合并/发布**：功能提交 `15ec6524`、编译修正 `fb94992b`、同期SMS合并与最终构建交接 `d738222b`；main从 `5f406973` fast-forward至`d738222b`，`git push origin main` actual exit0（5f406973..d738222b），feature push亦exit0。四个输出目录已无覆盖迁到主checkout `x9/out/`，最终用户手动boot路径 **`x9/out/x9-power-policy-v3-20261116/boot.img`**；保留中间/失败日志，不推荐首轮/v2。未改变主checkout当前SMS分支的未提交记录。功能树34项ignored配置/生成头/pycache已无覆盖归档于 `x9/out/power-policy-archive-20261116/`，仅unlink该树`.local` symlink，target未动；local feature branch已正常`-d`删除（原d738222b留在main）。本条推送后正常remove本轮worktree，其他会话不动。全部硬件验收仍待用户新日志，本条为交付源码与编译候选，不是已验证待机功耗。

- **分支清理完成（2026-11-16）**：按用户“合入 main 并删除”要求，复核远端功能 head `d738222b7448a9f73084a0def92da71a464d0f26` 已是 main 祖先；带该精确 lease 删除 `origin/feat/power-policy` actual exit0，随后 `ls-remote` 确认该远端 ref 不存在、main 已由同期工作推进至 `ff9e900f`。原本地 `feat/power-policy` 与 `.worktrees/power-policy` 均已删除；v3 镜像、四目录构建输出与 `x9/out/power-policy-archive-20261116/` 归档保留。清理交接独立工作树提交，不改主 checkout SMS 未提交 NOTES，不动其它工作树。本轮没有新构建、CI run 或设备操作；实机调频/休眠/唤醒仍待验收。


#### 2026-11-16 SMS retrieve 实际 route（静态收束；退休/物理ACK仍缺）

- **git实际里程碑**：原文档commit b81b1e02；rebase吸收同期main **39a02fa8** 时仅NOTES冲突，保留全部stage2 incoming内容后只插入本轮独立交接，continue实际exit0，文档commit改为 **c7177be4**。随后checkout main、ff merge、push origin main均实际exit0（39a02fa8→c7177be4）。无kernel/boot/CI/上机结果；本轮功能分支仅供最后交接提交，完成后只正常删除该本地ref，证据保留主checkout的out。

- **本轮收束**：保留NEW库存原字节最终资格 **25完整原FS函数18340B**，新 **19精确imports**，均实际exit0、没有再次读取这些函数。真kRequest→kSmsRetrieveMessage名查找100a3be0c（memcmp/length）从同server+c8 node+30/+38取得handler/flags，capture到100a35494的+38；policy返回1且handler非null时100a35aa0真实间接调用，x0=server+128，x3=input owner，x4=reply pair。100a32770把100a33884 block交xpc_connection_set_event_handler；注册vector101674010的实际消费者100a3a0dc逐项以同server调用registrar。**xpc_dictionary_handoff_reply不是dictionary_apply**。旧同Model ctor1000bec28..94以与retrieve getter完全相同token1010da781、原returned strong pair写Registry ServiceMap+40，新insert10006a170证明仅新key插入、不替换重复项；server+128 Registry的安装来源/当前runtime选中该Model还未闭合，不把静态同token当live同owner。
- **交付/下一**：报告 `docs/2026-11-16_reverse-sms-retrieve-route-report.md`；合同/argv/退出码/raw资格在 `x9/out/sms-retrieve-route-20261116/`。retrieve getter只按u32 ID查同Model+60/retain pair，Operation+c8/f8下游未穷尽；reply body/MCC/type/reply-enabled写入不作保存、退休或ACK证据。下一沿注册器真name10123c31c→handler1004ba4f4资格ACK/erase，同步补publisher原init entry与server+128 Registry来源，先矿缓存/保留raw/80页chains；不重采本轮成功范围。完整通话短信goal保持active，无生产delta、构建/CI/boot/设备动作；提交/合main/push的确切结果以同out finalization记录为准。
- **真实注册/取件里程碑**：uncached原text三精确目标库存actual exit0（排除旧instruction occupancy66756地址，新16998412B，原每读≤1024B），两个真地址引用落原FS1004b96cc/100a31ae4。后续从本次保留raw完整FS资格/解码 **12函数共6136B**，没有重读这些code：注册器用name10123c308、handler1004b9f5c、原w3=3调用100a318c8；insert100a3bcbc把handler/flags存node+30/+38，挂同object+c8树。handler从kSmsMessageID取xpc值→转换→service getter1000a95fc→virtual+d0；Operation+40覆盖原ID寄存器为type，不能把reply kSmsMessageType误作原ID。Model+d0真slot101528de0→1000c0334，queued invoke1000c6bb8按原u32 ID查同Model+60树，仅retain/copy node+28/+30 pair，**本body不erase/退休/COMMIT**。service getter使用的token1010da781原__const字符串为MessageCenterModelInterface；仍须闭合同Registry安装与实际dispatch，不能只凭类型名判定runtime选中具体Model。
- **续接/失败记录**：旧session35319实际exit0后才推进。service-token-data-r1错把__const地址当__cstring，metadata后exit1、未采该name；按实际section修正conststring reader后exit0。后发现此token已有上一轮RTTI缓存，此次name属于重复小读取，不计新发现，后续只复用。retained-ref-r1跨所有out缓存且靠文本猜endianness，在100531f60冲突exit1，未产资格结论；下一改为已知CommCenter文件逐文件/mnemonic验证，不合并不同原件，不隐藏失败。完整功能goal继续active，无测试/向量、生产delta、构建/CI/设备/共享输入操作。
- **开始/范围**：上一轮19函数7848B的新条件send与factory角色纠偏属progress；首核对main/origin为f3425e5b，建分支时同期归档任务已将main推进至 **3e4d50e7**，当前feature实际source-head已记录该clean基线，保留其全部交接。本轮单代理 `feat/sms-retrieve-route`，scratch `x9/out/sms-retrieve-route-20261116/`。下一从cached kSmsRetrieveMessage两字符串lead10123c308/101247a60建立真实注册/handler边，再沿同Operation通知ID→同Model+60取件/退休/reply。先缓存矿、不按factory槽猜业务，不重采上一轮/已闭parser/creator/SQL，不重编成功END kernel；无新代理/设备/共享输入操作。完整通话短信goal继续active。
- **key/数据里程碑**：两次all-format缓存矿actual exit0，只有其它page offsets，未命中该key/新slot的确切代码引用。两原CString10123c308/101247a60 bounded资格均kSmsRetrieveMessage。single-target pair format6库存复用11旧页、仅遍历69新页（合80），actual exit0/stderr空，唯一指向该CString的真slot **10158b318→101247a60**；selected slot/window亦exit0。这里不是CFString对象：318前两words零，后4真pointer/name资格为kSettingsModelGetPhoneNumber/kCUPolicySet/kCUPolicySetEx/kSmsSendMessage。邻接AP10158b350 RTTI DataProvisioningDataPlanManager仅说明相邻独立对象，不作SMS handler角色证据；没有扩其code。下一仅对两个原CString与已资格318地址做uncached text地址引用库存（≤1024B读、排除旧instruction occupancy），再按真实引用的原FS资格函数；尚未命名route handler/送达/取件或COMMIT。


#### 2026-11-16 远端仅保留本地同名分支（实际删除完成）

- **用户范围/基线**：单代理 `feat/remote-local-prune-20261116`，main输入 **3e4d50e7**。真实ls-remote的26条远端heads与本地10条heads按名称比较（排除本轮临时分支）；用户明确要求删除远端独有分支，不自动push本地独有分支，也不合入未知旧生产delta。根活动SMS的未提交NOTES从未由本轮切换/写入/恢复。
- **恢复证据**：删除名单22条及原始hash保留 `x9/out/remote-local-prune-20261116/{plan,retired-remote-heads}.json`。与全部保留本地/远端tips相比，只有 **fix/sms-startup-registration / 2db0d507** 有1个独立历史commit；删除前生成增量 **remote-only-unique.bundle**，bundle create/verify/list-heads均实际exit0，原始ref和可恢复依赖保留。其它被删head仍在保留历史中。没有为清理名称而将该旧delta盲合main。
- **实际删除/校验**：首atomic push因SSH连接关闭实际exit128（外层prune.py exit1），日志完整保留；重新ls-remote核原hash、再核本地没有同名branch后，仅重试仍存在的原名单。重试 **atomic push + 每ref旧hash lease实际exit0**，删除22条远端独有分支；fetch --prune、真实ls-remote/本地集合复核exit0。远端当前 **4条**：main、feat/camera-integration、feat/camera-native、feat/hevc-hw；**remote-minus-local为空**，本地当时10条，无本轮本地分支删除。同期camera-integration已更新发布，保留当前head，不回滚。
- **交付/边界**：本轮只有本交接的tracked修改，无生产delta、新kernel/boot/CI run ID或设备动作。正常commit/rebase同期main、ff/push后，仅归档本轮out并清理临时worktree/branch；最后sha/argv/stdout/stderr/exit及分支集合保留同out的finalization.json。恢复命令见同out的README.md，私有输入/原件/备份未动。

#### 2026-11-16 一天无新提交分支清理（实际删除完成）

- **用户范围/快照**：单代理 `feat/stale-branch-cleanup-20261116`，初始main/origin **e0e7815c**；后续ff吸收同期SMS的已推送 **2c34c540**，保留其全部报告/交接。按筛选时刻 **2026-11-16 18:47:04 +0800** 的固定24小时窗口（截止11-15 18:47:04）检查本地/真实远端同名分支的所有head，使用全部可达commit的最新committer时间，避免merge/非单调时间误选。只有 **feat/audio-dsp-boot**（最新11-13 18:04:09，1a76d62c）、**feat/audio-ipi**（11-14 23:18:18，6717e8af）符合。两者均为published main祖先、未合提交0，两次ff merge均Already up to date、tree未变，main push为Everything up-to-date；无需重新制造生产merge。
- **实际归档/删除**：音频树tracked clean；16项整个x9/out目录/日志无覆盖迁至主checkout同名路径，29项额外ignored文件（生成输入/配置/pycache）存 `x9/out/stale-branch-archive-20261116/audio-dsp-boot/`，共保存1165个ignored文件；仅unlink该树`.local` symlink，不改target。tree清空ignored且tracked clean后正常remove及branch -d，原始hash均保留在published main。随后 **atomic push + 每ref旧hash force-with-lease** 实际exit0，远端两音频分支均删除；fetch --prune及真实ls-remote复核通过。所有原镜像/日志/私有输入/备份保持可找，不编译/上机。
- **同期变化/库存**：本轮仅删除上述两音频ref。另有三条HEIF辅助local ref同期消失、HEVC/SMS新增提交及root checkout变化，均非本轮动作；不恢复/覆盖其它工作。初版退役wrapper在已成功删除后因“其它ref必须不变”的保守断言exit1；库存r1又因同期未push数量变化exit1，均保留失败，不重复删除。修正r2仅记录实际库存，实际exit0：当时本地 **7（排除临时树、含main）**、远端 **26（含main）**、名称去重 **29**，独立未push **45**（camera-ios-preview33/camera-source12）。这是当时快照，不是对其它活动任务未来库存的保证；最终库存/sha以同out的finalization记录为准。
- **交付/边界**：报告 `docs/2026-11-16_branch-retirement-report.md`，原始库存、rename/argv/stdout/stderr/exit在 `x9/out/stale-branch-cleanup-20261116/`。仅提交本交接/报告、ff/push main，归档并清理本轮临时树/分支；root原SMS未提交内容从未由本轮切换/写入/恢复，后续已推送SMS内容直接ff吸收。相机/HEVC和其它不足24小时分支不在删除名单，不随执行耗时滚动扩大范围。没有生产代码delta、kernel/boot/CI run ID或设备操作；最终收尾的确切commit/命令/状态保留finalization.json。

#### 2026-11-16 SMS fanout / 同 Model factory 分流（静态收束；retrieve/retirement仍缺）

- **开始状态/范围**：上一轮owner/Operation ID证据属progress，当前main/origin **9e25175b** clean；本轮单代理 `feat/sms-retrieve-retirement`，scratch `x9/out/sms-retrieve-retirement-20261116/`。复用上轮18函数/9imports；仅沿fanout wrapper100b7366c下游与同ID取件/退休新边，不重采成功范围、重编END kernel或新增代理/设备动作。完整通话短信goal保持active，不以静态payload/内存pair替通知送达、Messages COMMIT或物理ACK。
- **新fanout里程碑**：all-format缓存矿实际exit0，确认100b73080/100b7352c无旧body；`fanout-r1`实际exit0/stderr空，新原FS完整函数612B+320B＝932B，原code/data每读≤1024B。envelope保原code/dictionary，code经sxtw给真xpc_int64_create；四个key和type/GOT尚未资格，不命名其字段。fanout捕获同envelope、code及100a32664返回强pair，实际invoke指针100b742e8，ptr+48传100a3a604；尚未资格helper/invoke或subscriber送达。本轮恢复点没有活跃reader/build/device句柄，下一先矿缓存后只读三条缺的新边。
- **新队列/字段里程碑**：又6新函数812B，累计8函数1744B，两个缓存矿及函数/data/3import readers实际exit0/stderr空。四个bounded原cstring资格为 **kEvent/kNotificationEvent/kNotificationDict/kNotificationAdditional**；原undefined imports资格xpc_bool_create、__xpc_type_null、__NSConcreteStackBlock，非live GOT。envelope先kEvent=true、kNotificationEvent=code；真实innerinvoke100b742e8把同envelope的 **kEvent改为signed code** 再带同service/code/dict及flag1进入100b7439c→100b74464。helper100a3a604捕获inner block指针和service+48处pair的成功weak lock结果；outerinvoke100a3a70c真实12B tail-branch inner+10，非新命名helper。一次`send-cache-r1`库存exit1为误猜100a38cec非原FS、未读原text，已隔离且回到确证100b74464；不要续追该猜测。当前仍无真实subscriber送达或同ID server retrieve/退休资格，无生产delta/编译/测试/设备操作。
- **原send/owner分流里程碑**：累计16新完整函数6168B，原predicate8B真实返回1；code0x1d路径按node+50/+58的订阅vector匹配该code或-1通配，**100b74570..578** 用node+20连接和同envelope进入真 **xpc_connection_send_message**。dispatch_async与mutex lock/unlock亦exact imports资格，累计7imports；这是条件发送调用，不是app收到/取件/COMMIT。inner copy显式retain同XPC envelope与service强pair；outer copy36B只retain成功weak-lock pair，无显式inner block retain，generic block布局/lifetime保留未资格。旧构造器同Model pair→factory+18真slot10154f188→10036a0c4→100585858，新3348B creator将同pair存+a0/+a8，原两AP RTTI却为 **TerminalRegistrationController**，**不是retrieve server**。`server-object-data-r1`在后续control RTTI tagged name guard实际exit1；前两RTTI/chain从保存raw离线恢复exit0，原reader仍exit1，不重采/掩tag。下一旧实际factory+20 call1000bead4→新真slot10154f190→10036a1ac；尚未闭此body/route。同ID retrieve/Operation退休、GUID→成功record/COMMIT、MT ACK/MO及完整CALL仍缺，goal继续active。
- **最终分流/范围**：随后+20 wrapper172B→1009cded4 creator324B→1009ce018 ctor1184B，三者actual exit0/stderr空；同Model pair强留object+58/+60，getter返回object+174/u8为零时返回empty pair。真AP1015beed8 RTTI **33PhoneNumberRegistrationController**，仍不是已资格retrieve server，不按byte位置命名功能开关/预测设备值。最终 **19新完整函数7848B、7exact imports**；所有function reader actual exit0。selected-data只有上述晚段RTTI guard exit1，前两RTTI合同明确来自其保存raw恢复，失败原件/脚本/输出完整保留。一次原text库存误猜guard失败亦保留；不扩任意slot、tag或dead名字。
- **报告/交接**：`docs/2026-11-16_reverse-sms-fanout-and-factory-report.md`、桥接文档新增小节；合同 **fanout-factory-contract-r1.json**，actual argv/exit清单 **actual-reader-commands-r1.json**（字母序非时间线）、timeline/workitems/field-journal与raw在本轮out。下轮回到最早未资格的 **kSmsRetrieveMessage实际注册/handler**，既有10123c308/101247a60字符串偏移仅cached lead，本轮没有执行新key/whole-text reference库存；再闭同Operation ID→同Model+60取件/退休→成功record/COMMIT/重开与物理ACK/MO。真实subscriber收件、generic outer block lifetime、逐动作CALL record/Client/UUID/epoch及独立双向媒体仍缺。本轮无生产delta/编译/测试/人工向量/CI run/bootpack/设备/ADB/mailbox/mount/数据库/分区/构建后二次检查，所有reader句柄已终态；最新有效kernel仍41deaefb输入的 `x9/out/x9-call-end-native-owner-20261116/kernel.development.sweet`，不重编/改名。静态进展改变了下一动作，goal保持active，不称完整验收或blocked。接下来仅正常文档commit/ff合main/push，清理本轮本地feature，不动其它树/长期输入/旧镜像/备份。
- **实际整合/push**：原文档提交 **dffeb3a3**；其他硬件任务同时把main推进至 **e0e7815c**，首ff实际exit128，随后push为Everything up-to-date（不是推送本轮文档）。本feature正常rebase main；NOTES冲突按“当时main逐字保留＋本轮完整SMS小节”解决，equivalence/原失败保留，未覆盖蓝牙/振动交接。incoming c14d363b是历史319日志复核、镜像/source未匹配，不冒充新CALL/保存/物理ACK验收。rebased文档 **2c34c540** 已实际ff合main，**push exit0 `e0e7815c..2c34c540`**；只有3文档/136行，无生产变化/新kernel/CI run。最后notes-only交接仍走本feature正常commit/ff/push，再只删除本轮本地 `feat/sms-retrieve-retirement`；无本轮子树/远端feature/agent/build/device worker，原件/旧kernel/其它树/共享输入/备份不动。最终命令及数值exit在同out `finalization-commands.json` 和各 `*.exit.json`；完整目标保持active。

#### 2026-11-16 用户要求合并/清理蓝牙与原厂波形核对（本轮）

- **范围/合并验证**：单代理 `feat/bluetooth-merge-20261116`，main输入 **3fad865c**、本地主蓝牙 **e3647484**，原远端蓝牙 **f3332b89**。用户明确要求合main后删除，并统计所有剩余分支/未push提交。三方merge实际 **exit0、无冲突**；69个蓝牙改动文件，除NOTES外全部合并blob与e3647484一致，main-only文件全部保留，20个Python文件AST解析通过。纠正初记：最终cached diff-check在0006序列化patch上下文有 **6个space-before-tab警告**，不是无警告通过；未改原patch。没有新kernel/boot/CI/实机验证，不能拿合并成功当蓝牙/振动功能验收。根目录另一个SMS任务的未提交NOTES不动，原diff私有保存。
- **辅助蓝牙历史**：`feat/bluetooth-integration`（7）、`feat/bluetooth-ios-power`（6）、`feat/bluetooth-own`（5）、`fix/bluetooth-launch-cache`（1）原始非主蓝牙提交逐一 `git cherry e3647484` 全为 **`-`/patch-equivalent**。实际主蓝牙生产树先正常merge；辅助分支随后仅用history-only ours merge保留等价原始提交，不覆盖生产树，push成功并确认祖先后再移除蓝牙五个worktree/分支。所有ignored输出/候选/日志无覆盖归档到主x9/out，任何未忽略或额外私有输入先审查保留；不动其它活动分支、`.local`、旧镜像/分区备份。实际提交、push、清理与最终库存随后补记。
- **原厂新证据/转换器建议**：官方同平台 `haptic_common_driver.c` 为device_id0815按f0选择162/166/170/174/178Hz版本，按此前记录172.9Hz会选174Hz bucket；这些后缀是调校版本，**不是实际字节单载频证明**。本地官方header有140个真实RTP数组，其中0815为35个。源码默认AW_PWM_24K且按键路径未另设rate；按此条件离线FFT，174Hz版本INPUT110/111/112频谱峰约281/284/283Hz，SG302/303约344Hz、SG304/305约342Hz，均为短宽带复合波形，不等于设备机械输出或固定正弦频率，也未抓当前ColorOS写入/寄存器。没有本机现版完整RichTap资源/实时PCM证明覆盖80–300全部频点；不能说ColorOS所有输入都固定173Hz。故不宜盲目把80–300全部压到170–173：连续效果可能近共振更强，短tap/锐度/单向推动与刹车可能反而损坏；先实测透传基线再设计按类型的有限适配。本轮**没有修改转换器**。原始步骤/数值exit/解析与库存归档 `x9/out/bluetooth-merge-20261116/`，原厂引用限定静态官方参考，不冒充当前ColorOS已执行。
- **实际push/清理/库存**：生产merge **d0609eda**；四辅助history-only merge **790c3b78**、旧status等价history merge **792f2bf7**，两次history merge前后tree各相同。main已实际push到792f2bf7；远端feat/bluetooth-native已删除，本地主蓝牙/四辅助/旧status共六条分支和对应五个worktree已清理，均先确认pushed main祖先。117项ignored产物无覆盖迁存主x9/out，冲突/额外ignored输入在 `x9/out/bluetooth-branch-archive-20261116/`，rootboot13候选仍在主 `x9/out/bt-native-rootboot13-20261116/boot/boot.img`，未上机。真实ls-remote库存：删除本轮临时分支后本地 **11（含main）**、远端 **28（含main）**、同名去重 **34**；未被任何远端head包含的独立commit **45**，camera-ios-preview **33**、camera-source **12**，其它本地分支均0。未合main与未push分开统计；SMS未提交NOTES不计commit，本轮没有写入/恢复该工作区，前后diff因同期SMS措辞更新而不同，不伪报完全相同。完整报告 `docs/2026-11-16_reverse-coloros-haptic-waveforms-report.md`；库存/FFT脚本实际exit0。最后仅提交本报告/交接、ff/push main，归档并删本轮临时树/分支，不动其它硬件/活动SMS树；最终命令/sha/exit保留同out的finalization记录。本轮无新kernel/boot/CI run ID/设备操作，用户尚未实测。

#### 2026-11-16 蓝牙分支振动透传：芯片接口支持，机械手感尚未认证

- **范围/提交**：用户询问蓝牙分支最新 iOS 波形透传是否被本机马达支持。单代理核对树 `feat/haptic-pass-review-20261116`，创建时 main=`9e25175b`；报告提交 **`6fcd2138`**，`docs/2026-11-16_reverse-haptic-passthrough-report.md`。只检查本地蓝牙 `e3647484` 的功能提交 **`66dd5f8c`**；实际 origin 蓝牙 ref仍为11-15的 `f3332b89`，不是本地11-16透传实现，未改写/推送活动蓝牙分支。
- **观察/结论**：已有330日志chip9381/RTP12000/FIFO2048、mediaserverd writer、state08及非零submitted证明旧版真实通路。原厂 IOCTL_STREAM_MODE→aw_stream_mode→opbuf->data→aw8693xs_set_rtp_data确为直接RTP；支持该输入方式，**不等于OPPO LRA原样还原Taptic Engine触感**。原厂非0816的f0_pre=1700；蓝牙NOTES此前记Android f0=1729，但本轮未独立重采该原输出；XNU log f0=173000是默认/参数，不能当实测。新converter保留80/173/230/300Hz，改f0=190或sharpness=0输出不变；没有自动机械共振适配。D221配置本身有按频率增益（Default Sine Lookup在172.44Hz=-5.3521、230Hz=-10.8338），不能认作通用未调校波形。
- **实际离线验证**：按66dd5f8c提取真实converter并编Mac C wrapper。首轮link **exit1**（默认CLT MacOSX27 tbd与旧linker不兼容），失败完整保留；明确项目Xcode15/MacOSX14后 **exit0、stderr空**。固定 `.local/apple/ios16-20H392/OS.dmg`新只读挂载，原MicroTap230/MiniTap150/FullTap80转换后非零跨度 **6.75/11.167/33.333ms**、峰值127、1/4 intensity峰值32。仅数字层观察，非物理强度/延迟验收。旧 `/private/tmp/os20H392` 确来自仓库外镜像，未作为本轮权威采样；root clone短暂辅助检查亦未作为最终分析输入。本次新mount已detach **exit0**，原有其它mount不动。
- **边界/下一步**：保持透传作基线，再用少量明确事件验证力度、起振和余振，按本机响应决定有限频率相关增益/瞬态补偿；不盲目恢复×6削顶/增益下限/固定载频重合成。当前保留工厂OSC source，未复现原厂upload OSC校准；不抄F0 trim当OSC或机械均衡。20ms提前放包与200ms静音续流不保证整链1ms延迟或每次按键免GO（100ms无write、最后client StopIO仍会park/STOP）。私有分析/源码切片/实际argv/stdout/stderr/exit/attach-detach/field-journal归档 **`x9/out/haptic-pass-review-20261116/`**，原Apple资产不入git。本轮没有生产行为delta、kernel/boot/CI build或run ID、设备/Android/sysfs/mailbox/分区操作；未宣称rootboot13或全部振动场景通过。收尾仅合入本报告/交接、push main、归档并清理本核对树，活动蓝牙及其它树不动。
- **实际合入/归档**：报告 **`6fcd2138`**、交接 **`8c6b27a8`** 已ff合main并 **push成功 `9e25175b..8c6b27a8`**；整个分析目录已无覆盖迁回主checkout `x9/out/haptic-pass-review-20261116/`，本次mount已卸载。根工作区同期处于另一任务的 `feat/sms-retrieve-retirement` 且NOTES有未提交修改，未切换或修改该工作区；通过本核对树暂时checkout main完成正常ff/push。此最后补记完成后只清理本核对树/分支，不动活动SMS/蓝牙及其它树。源报告在main，当前根工作区若尚未合main可用 `git show main:docs/2026-11-16_reverse-haptic-passthrough-report.md`，分析归档另保留 `report.md` 便于直接打开。

#### 2026-11-16 SMS Controller+9c0 owner → Operation通知ID（静态证据收束）

- **基线/范围**：单代理 `feat/sms-notification-consumer`，main/origin **ea32708b**；只复用原件与必要新静态采样，不新增代理/子树，不重采旧parser/creator或重编成功END kernel，不操作设备、共享`.local`或备份。完整通话短信goal继续active。
- **supplier/安装**：新single-target direct-BL库存唯一lead **10036a00c**，新caller **100369f88..10036a0c4 /316B** retain/透传x2。原format6 actual slot **10154f180→100369f88**，AP10154f170 RTTI **MessageCenterModelFactory**，slot+10。wrapper新direct-BL库存零命中不代表dead；随后Model-create库存guard因发现旧cached caller **exit1、未扫原text**。旧 `call-provider-install/factory-install-functions.asm:100b936d8..100b93720` 已有实际factory AP安装/调用1000bde4c，复用其raw words生成116B `root-factory-install.cached.*`，**不重采旧installer、不称其本轮新bytes或live调用**。旧provider-install UUID与当前metadata均20b0ce28-18a1-3eea-aa2d-ef53e33c039a；错误LLVM boost标签不作语义。
- **同owner/新对象**：新 **1000bde4c..1000bf424 /5592B** 创建同MessageCenterModel强pair，原AP101528d10/RTTI与public offset0 MessageCenterModelInterface base资格；**1000be73c..1000be798** retain该pair传actual factory+10→supplier→旧creator的Controller+9c0/+9c8。旧Controller consumer virtual+40的真slot **101528d50→1000bf9ec /556B**，自身weak_count::lock、string own-copy及dispatch_async_f强context→ **1000c6258 /1200B**。后者新构造独立 **SmsOperation**（10042c780 /328B，AP101561878），**不是原sms::Model强指针**；Registry/content own-copy已读。
- **ID/通知payload**：新base ctor **101074578 /120B** 把w3=-1写Operation+0xc；真实get/set **10042c948/950各8B** 仅读/写该u32。新admission **1000bfc18 /528B**：当前getter==-1时取同Model+80的 **MessageIdManager+8/u32** postincrement，写Operation+0xc；同ID作key、同Operation强pair入Model+60内存树。manager原初值低u32=0x80000000，不预测设备当前ID；既有key不替换，nullOperation返回-1。Operation+80等未资格副作用保留，**分配结论限定当前ID==-1，不从ctor推所有路径**。queued保同SIM与返回ID，非-1调 **1000c2938 /656B**；SIM1将ID零扩展给真xpc_int64_create，真CFString **kCTMessageIdKey/typeKey** 经真xpc_dictionary_set_value落dictionary，type=1/code0x1d进入同Registry的send wrapper与fanout边界。**SIM=Model+8、PDU Model+34/u16、Operation通知u32、GUID/SQL RowID/nativeepoch仍分别处理**；不宣称0x1d事件名、app送达、COMMIT或物理ACK。
- **实际范围/异常**：最终 **18新完整函数11552B、9exact imports**，函数/data采样≤1024B，全部函数与selected-data/import reader实际exit0/stderr空。factory AP库存排除旧code，initial cachegrep只认Capstone漏旧LLVMinstaller，后来all-format BL矿已纠正；额外304B/1260B候选未证明安装，不扩其业务语义。name库存r1虽exit0但rfind失败→offset0形成越界/超长记录，原reader/巨大输出保留且隔离，有效subset仅lead；两处selected32B names与actual RTTI/chain独立资格。修正bounded-name-r2 **未执行**，不伪报reader成功。read-functions执行版留 `read-functions.r1.py`，当前版修正完整text exclusion上界，修正版亦未执行。最终合同 **notification-consumer-contract-r2.json**，r1保留；命令清单 **actual-reader-commands-r2.json** 非时间线，actual先后见timeline.md。
- **报告/下一边**：正式 **docs/2026-11-16_reverse-sms-notification-consumer-report.md**，全部私有raw/offset/argv/stdout/stderr/数值exit在 `x9/out/sms-notification-consumer-20261116/`。下一 **真实fanout/subscriber receipt→同通知ID retrieve server与Operation map退休→app CTMessage.messageId/GUID→成功record/COMMIT/重开**，不要重采已闭18函数或拿内存pair当durable保存。完整CALL逐动作record/Client/UUID/native退休、typed DIAL、CallList/handover/双向DSP、物理MT ACK/MO仍缺。本轮无生产delta/测试/人工向量/compile/build/CI run/bootpack/上机/mount/数据库读取或构建后二次检查；最新有效kernel仍41deaefb输入的 `x9/out/x9-call-end-native-owner-20261116/kernel.development.sweet`。静态证据改变下一动作属progress，不称目标完成或blocked。
- **实际提交/合main/push**：报告/桥接文档/交接提交 **6c697522**，staged diff-check实际exit0；已ff合main，**push实际exit0 `ea32708b..6c697522`**。差异只有3个文档，无生产代码、新kernel或CI run ID。精确argv/stdout/stderr/exit在同out `*.exit.json`，最终原件合同使用r2。本条最后notes-only收尾沿同feature正常commit/ff/push，仅删除本轮本地 `feat/sms-notification-consumer`；没有本轮worktree/远端feature或agent/build/设备worker，不动其它树/共享输入/旧镜像/备份。所有本轮新reader已终态；下一从上述fanout/retrieve实际consumer开始，不重复已闭18函数或重编END kernel。

#### 2026-11-16 parser response → concrete SMS Model/status（本轮静态证据收束）

- **基线/范围**：单代理分支 `feat/sms-parser-model-owner`，基于 main/origin **1960aedd**；不新增代理/子树，不重采上一轮七个 SMS 函数，不重编成功 END kernel，不操作设备或共享 `.local`。完整通话短信 goal 继续 active。
- **实际新原件**：consumer `1007f622c..1007f72bc /4240B`、共同 helper `1007f72bc..1007f7918 /1628B`、两个 ctor/两个 wrapper/ACK function copy 合 **2420B**、两个 own helper/base ctor/Controller+e0 target 合 **10236B**、其下游 `1007f2004..1007f3e2c /7720B`、routing gate `1007f6004..1007f622c /552B`，六次实际 reader **exit0、stderr 空**。新 text 均按 ≤1024B chunk，原 header/load commands/LC_FUNCTION_STARTS 与已保存资格一致；raw/精确 offset/命令源码/stdout/数值 exit 在 `x9/out/sms-parser-model-owner-20261116/`，没有重读旧 code。
- **类型/纠偏**：新 exact RTTI/format6 slots/imports 资格 r2 **exit0**，确认 ctor 安装 **sms::GSMModel / sms::C2KModel**，base 为 **sms::Model**。原 details+0/u32 → Model+8/u32 同源，但后者明确传入真 `subscriber::isValidSimSlot(..., SimSlot)`，**这是 SIM slot，不是通知 messageID**；此前 consumer 临时“候选 messageID”命名撤销。旧 Model+34/u16 accessor 仍是独立 ID 字段，不与 SIM/MD transaction/通知 uint/GUID 按数值等同。数据 reader r1 因错误寻找不存在的 `.reads.json` 元数据位置 **exit1**，仅读 header/commands、未读新 slots；r2 改用已保存 proof 的 exact `segment_starts_offset`，失败 stderr/reader原版均保留，不掩盖为成功。
- **已闭/未闭**：同 Controller+90 Registry 强pair、原 SIM、decoded PDU → concrete Model 强pair → 共同 helper → 最终 raw status 与当前pair传原 ACK callback 已读；**未reset分支保同Model，特殊reset分支为raw1/null pair**。原 callback copy helper10080d87c走实际 clone/copy virtual+10/+18，而非裸引用。Controller+e0 真 slot指向1007f8994，下游1007f2004仍是多门控处理/同Controller+9c0外部consumer，不据 scratch `publish/notification` 文件名宣称已发 app通知。raw4/5/7可被后续门调整成1/8，入口raw2/null Model提前返回且不callback；不是COMMIT或物理ACK。
- **最后窄采样/归档**：concrete Model+160目标 **1008485e8..100848b94 /1452B**、**1007e4e58..1007e57cc /2420B**及两个8B +140 predicate，postparse-r1实际 **exit0/stderr空**，未观察直接写Model+34；virtual calls全副作用仍未资格，不据此证明ID分配/保存或无mutation。+140仅读GSM+22c/C2K+1b4/u8，不命名业务状态。终态review又补唯一必要 **1007eced4..1007ecf30 /92B**，pair-mutation-r1 **exit0/stderr空**；该helper先清ptr/control再release，1007f75bc后raw1回null pair，纠正初版合同“callback带同Model”未加限定的措辞。本轮共 **18个新完整函数30776B**，12个exact imports；最终 **parser-model-contract-r2.json / actual-reader-commands-r2.json**，初版contract和全部raw/offset/数值exit及失败均原样保留。正式报告 **docs/2026-11-16_reverse-sms-parser-model-report.md**，call-ios-bridge追加链接，旧证据原文件/采样标签均不覆盖或改名。
- **下一窄边/未完成**：旧creator1007ea650的 **x2 supplied pair → 同Controller+9c0/+9c8强owner → 实际virtual+40 consumer → 通知uint ID分配/发布**；actual supplier caller尚未资格，不据SIM、Model+34、MD transaction或scratch发布标签补身份。Model+34/u16到app ID、GUID到成功record、Messages COMMIT/重开/物理ACK/MO、逐动作CALL record/Client/UUID/native退休、DIAL typed backend/profile、CallList/handover/双向DSP仍缺。无生产SMS delta/测试/人工向量、compile/build、CI run、boot pack/上机、mount/数据库读取或构建后二次检查；最新有效kernel仍41deaefb输入的 `x9/out/x9-call-end-native-owner-20261116/kernel.development.sweet`。所有本轮采样均终态，无代理/build/设备worker；goal继续active，不称通话短信验收完成，不标complete/blocked。实际commit/合main/push终态随后记录。
- **实际提交/合main/push**：报告及窄交接提交 **8918bbee**，staged diff-check两次实际 **exit0**；已ff合main，**push实际exit0 `1960aedd..8918bbee`**，没有生产代码delta/新kernel或CI run ID。所有命令argv/stdout/stderr/确切exit在同out的 `*.exit.json`；原件合同用r2，r1保留纠偏，命令清单不是时间线（实际先后见timeline.md）。本条最后notes-only交接走同feature正常commit/ff/push后仅删除本轮本地 `feat/sms-parser-model-owner`，没有子worktree或远端feature分支，不动其它硬件树、共享输入/旧镜像/备份。不重采本轮18函数或重编END kernel；下一从上述+9c0实际supplier开始。

#### 2026-11-16 END actual-QMUX owner 到 native allocator/writer（续接生产修复）

- **续接分类/基线**：上一轮生产修复、静态owner归档、完整kernel及main push属于progress。实际main/origin **ef9047f0** clean，上一root功能分支/三子树均已删除，没有待取build/代理worker；本轮单代理 **fix/call-end-native-owner**，不重新采原CALL/SMS字节，不动其它硬件树/共享`.local`/设备或备份。完整通话短信goal继续active。
- **实际新代码**：修复前post-wait检查之后`end_submit`仍可等MD lock，state1 writer只核native target；该已证production空窗本轮不再只加一次getter。END job捕原actual QMUX `bridge_generation`，强制现有END submit带原local/node/allocation/64-bit deadline，并与native epoch/revision/ID保存到同token。新内部`coachz_baseband_call_guard`在**MD→bb_lock**双锁下核service9/nonrelease/channel/server/exact allocation及原deadline，并持锁同步commit state1 admission或state2 selection。callback仅原slot/frame/native lease mutation，无heap/API重入/physical TX；BB poll的send/DEL原本在解BB锁后，不新增锁反转。submit guard失败不分新token；writer只对未selected state1回原state6/take=-3，已selected/partial原owner仍排空real CNF、不重发/retarget。
- **源码/compile-only证据**：9个新增host源向量覆盖第三MD lock与BB等待、无QMI poll时writer的owner/deadline拒绝及selected后release旧token；未执行。CALL fixture r1/r2 **exit0**，共享SMS fixture r2 **exit0**，均`/usr/bin/clang++ -std=c++17 -Wall -Wextra -Werror -c`；r2修正源码向量不调用必断言stage2的旧CNF helper，SMS新增CALL guard stub一律拒绝（无SMS production delta）。原源码diff、精确命令、contract与log/exit在主`x9/out/call-end-native-owner-20261116/`，不把编译当并发/设备验收。实际object/kernel build/main push终态随后记录。
- **实际生产单对象终态**：生产提交 **fcdacd4c** / tracked clean输入，固定`.local` check **exit0**；两个实际消费者 **X9Baseband.cpp.o / sweet_x9_gpu_dependencies.o均True/exit0**，warnings **34/252**（新END own-copy沿用C99声明方式，非error）。原session **48308已终态**；`compile-source-head/status`、`compile-commands.json`、recipe、single-compile-r1.log/exit、recipe-results.json在主`x9/out/call-end-native-owner-20261116/`。没有新增fixture执行或构建后二次对象检查；之后仅纠正旧fixture注释（无kernel行为delta），不重编成功对象。下一用同生产delta补正常完整kernel，确认新C ABI guard在整kernel link成立，不靠宿主fixture替代production。
- **正常完整kernel终态**：以 **41deaefb / tracked clean**（生产delta为fcdacd4c，后续仅fixture注释/交接）实际执行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py kernel --name x9-call-end-native-owner-20261116`，原session **3061已终态/exit0**，**98/98 overlay对象、3prebuilt、完整link/strip成功**，无未解析guard符号。最新有效kernel **x9/out/x9-call-end-native-owner-20261116/kernel.development.sweet**；source-head/clean status、commands.json、full-build/build日志与确切exit在同目录。没有postbuild hash/解包/反汇编/对比、测试、boot pack或上机；旧039f917a kernel/镜像/备份均不动。实际`gh run list --branch main --limit3 --json databaseId,status,conclusion,headSha`结果保留旁边`ci-run-list.*`，本轮未启动CI，无本轮run ID。所有compile/build句柄均终态，不重编成功对象/本kernel。
- **尚待/下一边**：这里是actual QMUX allocation与原native END epoch/ID同token guard，**不是personality record版本/逐动作record+Client+UUID接线**。VOICE09保持关闭、20无productioncaller；generic staged905/DIAL能力不升级。SMS **1007f622c**消费者及Model/status→通知ID本轮尚未读，returned BlastDoor GUID→成功record、Messages COMMIT/重开/物理ACK/MO对端、CallList/handover/双向DSP仍缺。无测试/原件采样/CI run/boot pack/上机/构建后二次检查；最新完整kernel已更新为上述41deaefb输入，不用旧039f917a kernel冒充本修复，也不把新kernel编译当功能验收。不会因这些未完成项标complete/blocked。

- **实际合main/push/下一采样范围**：生产 **fcdacd4c**、对象交接 **41deaefb** 与完整kernel交接 **85cd6d2e** 已ff合main，**push实际exit0 `ef9047f0..85cd6d2e`**，当时main/origin clean同步；kernel输入仍41deaefb，不改写为事后notes head。本条最后交接走同fix分支正常commit/ff/push后仅删该root本地分支，确切命令/exit在`x9/out/call-end-native-owner-20261116/{merge-push-commands.json,finalization-commands.json}`，没有本轮新子worktree/远端feature分支。无活跃compile/build/采样/设备句柄。仅从**已保留**SMS函数表导出下一consumer lead **1007f622c..1007f72bc /4240B**，未读其body或新原件；`sms-next-consumer-lead.json`是下一scope线索，不是Model/status资格或运行验收。完整goal保持active；下一先补此实际parser consumer的同owner Model/notification链，不重编本修复/重采七个旧SMS函数；actual逐动作record/Client/UUID、媒体及最终保存/ACK仍未完成。

#### 2026-11-16 END post-wait owner fence 与 concrete CALL/SMS 下一边（已收束，最新完整kernel通过）

- **基线/分工**：root `feat/sms-controller-owner`基于main/origin **`a8729e20`**；按用户明确要求复用三个代理，`call-action-owner`追真实CallModel强record更新、`sms-controller-dispatch`追supplied concrete Controller的MT/parser边、`call-sms-receipt-review`审现行CALL/WMS37收据。scratch仅各树`x9/out`，不动其它硬件树、共享`.local`、设备或备份。完整通话短信goal仍 **active**。
- **实际生产修复/编译终态**：receipt代理原 **`11f0cb55`**已review/cherry-pick为root **`ec47c9db`**：stage1 `take/end_snapshot`两个可等待MD mutex的操作后，建新`0x905` action前再检查原owner live与deadline，关闭等待期间actual QMUX已release、DEL尚未dispatch的已证空窗。**仅post-wait fence，不是BB身份检查与MD submit原子化**，最后check以后/submit内部等待与writer的host-generation/deadline guard仍缺；已selected/partial旧mutation drain合同不改。四个新增fixture源向量未执行；代理宿主`clang++ -std=c++17 -Wall -Wextra -Werror -c` **exit0**。root以实际生产head **ec47c9db/tracked clean**单对象编译`X9Baseband.cpp.o` **exit0/True、34warnings**；session **3293已终态**，命令/source-head/status/recipe/log/results保留主`x9/out/call-end-postwait/`，不再重编或复检对象。WMS37窄链未发现独立缺陷，未改SMS生产代码。
- **Root新dictionary gate证据**：head/blast/callback/continuation/selector-class-target/helper-export六个模式各实际 **exit0**，在主`x9/out/sms-dictionary-owner/`；dictionary入口实际4B thunk（首读1024B含邻函数，不将邻字节认作本method）。`_processReceivedDictionaryInBlastDoor`捕原dict `g`与同storage/completion，交真实IMBlastdoor；callback的returned对象只有通过真实`BlastDoorSMSMessage` class gate才进入`_processReceivedSMSMessage`，不是原dictionary。nested continuation仅转发原完成参数，非SQL收据。原continuation proof的origin文案误写外层method，已通过维护erratum/docs/合同纠正，保留原proof不重采；未资格copy-helper仍标ABI推断，returned GUID producer及dict g→成功record身份仍未闭。
- **CALL/SMS已收束及整合观察**：续接实际只有root运行，没有待取代理/编译句柄；root自行收束旧CALL未提交diff，不新增代理。CALL五次原采样均actual **exit0/stderr空**，共5个text4320B、3个format6槽24B：driver AP+1e0→incoming record→PID map的强Model；实际`IMSCallModel+70/+78` update先retain新control、存pair、再release旧control；同ctor AP+118指向旧dial。缺Model factory+48目标未资格，不能从map无条件声称每个动态Model就是该类型；仍不是同动作record/Client/UUID snapshot或native epoch。SMS原 **dd214307** 已整合为root **27d83047**，7函数10388B＋8named stubs96B，13条窄采样actual exit0；concrete `sms::Controller+68/interface+20`→parser async→同response/details与callback owner已闭，**1007f622c消费者body/server未读**，Model/status→通知ID仍缺。无SMS生产delta。dictionary erratum及未资格copy-helper边界已写原`metadata-erratum.txt`/合同与docs，不重采或覆盖旧proof。
- **实际完整kernel终态（续接）**：CALL维护/证据收束提交 **`039f917a`**、tracked clean作为完整build输入（生产delta含ec47c9db），固定布局check **exit0**；实际命令`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py kernel --name x9-call-sms-owner-postwait-20261116`，原session **8665已终态/exit0**，**98/98 overlay对象、3prebuilt、完整link/strip成功**。最新有效kernel **`x9/out/x9-call-sms-owner-postwait-20261116/kernel.development.sweet`**，source-head/clean status、commands.json、prepare-local/full-build/build日志及确切exit在同目录。不改名/覆盖旧90079648 kernel，也没有构建后二次hash/解包/反汇编/对比、测试、CI run、boot pack或设备操作。不能把完整kernel编译当最终通话短信验收。

- **归档/维护里程碑（续接）**：三路scratch已无覆盖rename到主`x9/out/{call-action-owner,sms-controller-dispatch,call-sms-receipt-review}/`，原source-head/绝对采样路径不回写。CALL原未提交diff与exact62行append保留`x9/out/call-sms-owner-integration-20261116/`；root保持原reader逐字转移及文档append，再补action/format6 load-command count/size/end、duplicate fixup与最小header guard。最终 **AST parse exit0、git diff --check通过**，仅维护语法/static review，不重跑reader或测试。原receipt/GUID gate合同及保存raw复核在`static-integration-review.json`；build前`gh run list --branch main --limit3 --json databaseId,status,conclusion,headSha`实际 **[]/exit0**，无CI run ID；完整build终态如上。三子树tracked clean/out归档空后正常移除；CALL原未提交diff精确归档/转移后仅restore对应两文件，SMS/receipt先确认`git cherry HEAD <branch>`全为patch-equivalent，再删除仅本轮本地分支，命令/exit保留`cleanup-commands.json`。没有活跃build/采样/设备句柄；未动其它硬件worktree、共享`.local`、旧镜像/原件/备份。实际main/push终态随后记录。

- **完整目标尚待/下一真实边**：goal继续 **active**。VOICE09关闭、VOICE20无production caller；CALL沿已资格driver record入口和同Model+118 dial接点，补实际updater队列与逐动作强record/Client/CallInfo UUID及更新退休/native epoch，不能拿Model lifetime或44成功当版本。SMS下一新范围从现已闭parser callback传同response/details的 **1007f622c** 消费者开始，追同Controller的Model/status→通知uint ID；此body本轮未读，不重采七函数/旧取件/SQL。returned BlastDoor GUID→实际成功record、当前Messages COMMIT/重开/物理ACK/MO对端，以及CallList/handover/独立双向DSP仍缺。精确vendor默认CLIR jar仍须同版原件或用户允许的设备读动作；本轮不自行ADB/boot/mailbox/mount/写分区，不标complete或blocked。

- **实际合main/push与最后交接**：生产 **ec47c9db**、SMS **27d83047**、CALL维护 **039f917a** 与完整build/归档交接 **86bf9711** 已ff合main，**push实际exit0 `a8729e20..86bf9711`**；当时main/origin clean同步。完整kernel输入仍039f917a，不将后续notes-only commit重命名为构建source。三子树/本地分支已清理，所有build/push句柄已终态，无测试/设备worker。本条最后交接仍走同feature正常commit/ff/push后仅删root本地`feat/sms-controller-owner`，实际命令/exit记录`x9/out/call-sms-owner-integration-20261116/{merge-push-commands.json,finalization-commands.json}`；其它活动硬件树、长期输入与旧镜像/分区备份不动。完整目标继续active，不交付完整通话短信验收镜像。

#### 2026-11-16 同 txn 重复请求终态 owner 修复与当前接线（本轮收束，完整目标未完成）

- **基线/分工**：root `feat/call-context-capture`基于main/origin **`252f3003`**；按用户要求复用三个代理与新树`call-context-capture/call-terminal-audit/sms-notification-owner`，分别追loaded manager factory/同queue owner、现行03/21终态代码、强SMS Model到通知ID。scratch仅各树`x9/out`，不动其它硬件树或共享`.local`。完整通话短信goal仍active。
- **实际生产修复**：terminal代理原 **`1fa2cab8`** 已整合为root **`90079648`**：合法21同txn改ID，旧路径可先回BUSY而原owner随后成功；现在同slot/local generation/txn且actual binding/QMUX仍live时消费repeat，不改原ID/token/epoch或重发MD。03已answered同txn改options，旧路径可追加ARG；现在保首终态，pending changed首ARG与fresh txn行为不变。未改44、MD mutation、REGISTERED或VOICE公告；generic malformed重复保护未扩展。原实际helper `coachz_baseband.cpp:1528..1560`仅拒过deadline的success，error仍可receipt，排除了此前deadline猜测。
- **确切编译**：代理宿主fixture新增两项源码断言，`xcrun --sdk macosx clang++ -std=c++17 -c` **exit0**，未执行。root同生产head90079648/tracked clean输入，fixed layout check **exit0**，单对象`X9Baseband.cpp.o` **exit0/True、34warnings**，命令/log/recipe/result在主`x9/out/call-context-capture/`。`gh run list --branch main --limit3`实际 **[]/exit0**，无CI run ID。正常完整kernel的历史session **97925**已取终态，结果见下；没有待取编译worker。
- **完整kernel终态**：同生产source **`90079648`**（不是事后交接head），session97925已实际 **exit0**；**98/98 overlay对象、3prebuilt、完整link/strip成功**。实际命令`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py kernel --name x9-call-duplicate-owner-20261116`；新kernel **`x9/out/x9-call-duplicate-owner-20261116/kernel.development.sweet`**，source-head/clean status、exact command、full-build/build日志与exit保留同目录。未boot pack/上机，不作构建后二次hash/解包/反汇编或产物比较；原068d93e0旧kernel/镜像/备份未改。当前无活跃编译/设备session，不能把完整编译当完整通话短信验收。
- **Root短信新静态边**：当前root cache原method entry/LC_FUNCTION_STARTS确认`_convertCTMessageToDictionary:requiresUpload:1f3cbec88..1f3cbf8e4` **3164B**；metadata/新code/keys/selector targets/2原export五次各actual **exit0**。同CTMessage uniqueIdentifier的UUIDString或fallback `_StringGUID`结果仅非null时写原dict key **g**；不是把通知uint/messageId转换为GUID。conversion内conditional `acknowledgeIncomingMessageWithId:`在返回/外部length-gated payload写盘/保存链之前，不能称COMMIT或WMS37物理ACK。raw/原pagechain/selector/export/命令与`identity-contract.json`在主`x9/out/sms-guid-owner/`，未重采旧取件/SQL；独立只读review已完成无阻断。GUID helper仅保留未命名bit0门控、ACK predicate含义未资格，dict g到实际成功record仍未闭。
- **SMS同controller静态owner**：代理原 **`480b12db`** 已整合为 **`0f0df3ff`**；10段新code合 **4324B**，实际caller强controller→driver+80/+88弱pair→成功weak lock临时强lifetime→同controller virtual+20的原MT details/同capture ACK callback dispatch。原件/raw/合同在主`x9/out/sms-notification-owner/owner-boundary.{json,txt}`。原采样独立数值exit **null/not retained**，重放命令 **reconstructed/未再次执行**，不补称exit0；仅代理diff-check/commit实际exit0。首缺supplied/loaded concrete controller+20实现及Model/status→通知uint messageID，不能升级为强Model通知或运行时安装验收。
- **CALL实际factory选择**：代理原 **`848ddcf6`** 已整合为 **`749c7e5e`**；4新text **1956B**、原root import12B/槽8B，Registry强pair→unique factory AP1015494a8→真Manager create/C2→manager+60→原format6 AP+30槽1015494d8→旧同record creator100315488闭合。完整stdout/proof、stderr空，但外层zsh readonly `status`致 **wrapper exit1/reader独立数值exit未知**；r1后exact ctor/export/C1/pagechain末节点/load-command guards仅ASTparse成功与diff review，未重跑reader。原件静态选择、manager+18同queue参数与Client getter不等于逐动作record+Client+CallInfo UUID snapshot，更不等于live GOT/已装MTK backend。维护入口`x9/tools/call_context_evidence.py --mode selection --out x9/out/<新目录>`只用于新的输出目录，本轮不重放。
- **实际归档/清理与复核**：三个代理完成停写，最终独立CALL/生产修复及SMS复核均无阻断。CALL scratch整目录已无覆盖rename到主`x9/out/call-context-capture-review/`，保留原source-head/采集路径；terminal与SMS分别在主`call-terminal-audit/sms-notification-owner`。CALL cherry-pick文档冲突保留全部root CALL/SMS小节并精确追加代理51行；tracked reader与原848ddcf6逐字相同，docs严格等于root0f0df3ff加原child append。`git cherry`因追加hunk上下文改变为`+`，不伪称patch-id等价，源文本等价检查PASS后才删本地review分支；归档wrapper此前assertion exit1及branch -d拒绝也保留`integration-equivalence.txt`。本轮三子worktree/分支已移除，不动其它硬件树、`.local`长期输入或旧镜像/备份。
- **尚待/限制**：完整goal仍 **active**。下一真正接线为同owner逐动作record/Client/CallInfo UUID与更新/native epoch、loaded typed backend/profile/terminal，以及上述SMS concrete controller→Model/status→通知ID→GUID/成功record；CallList/handover身份、独立双向DSP、当前Messages COMMIT/重开/物理ACK/MO对端仍缺。VOICE09关闭、VOICE20无productioncaller。同版vendor CLIR default jar仍缺，不自行采集。没有活跃编译/采样/设备worker，无测试/人工向量/设备/boot pack/构建后二次hash/解包/反汇编/比较/ADB/mailbox/mount/分区操作；不把静态证据或编译当通话短信完成。本条交接提交后ff合main/push并记录实际区间，不交付功能验收镜像。
- **实际合main/push**：生产修复 **`90079648`**、SMS文档 **`0f0df3ff`**、CALL维护/文档 **`749c7e5e`**及收束交接 **`07b0e10f`**已ff合main，**push实际exit0，`252f3003..07b0e10f`**。不是新增kernel编译，kernel生产输入仍90079648。所有本轮子树/分支已清，raw在上述主`x9/out`；无feature远端push。本条最后交接正常提交/ff/push后仅删除root本地`feat/call-context-capture`，不触其它worktree。最终goal继续active，不重复本轮编译、原件成功范围、已排除路径或设备操作。

#### 2026-11-16 同 record 构造关联、MD CLIR consumer 与 SMS 同步取件身份（本轮静态进展）

- **基线/分工**：root `feat/call-record-snapshot`基于main/origin **`6dba5aeb`**；按用户要求复用三个代理与新树`call-context-join/call-profile-input/sms-save-authority`，分别闭普通factory强Client/逐动作关联、MD raw CLIR真实消费、通知ID到CTMessage同步取件。本轮是完整通话短信目标的前置进展，goal仍active，**没有新增生产行为修复或最终镜像**。
- **CALL实际新证据**：原Client/ClientHandle RTTI新增40B与原page-chain/4真export资格 **exit0**，确认原Client→ClientHandle adjustment0；临时Client强pair在每次factory返回后释放，原Call只own-copy UUID/clientId。第三actioncaller只把原CallInfo+b8 join本次新Call/setRelay，没有独立record/PBI。原代理 **`1abee699`** 已整合为 **`8ec28c18`**；代理曾push远端`feat/call-context-join`。raw已实际无覆盖rename至主`x9/out/call-context-join/`，final `typed-context-join-contract.{json,txt}`。
- **Root同owner构造链**：维护reader新`instance`模式实际 **exit0，2NEW text/2真export**，controller C1 `1f2e66b4c`直接到C2 `1f2e66b6c`；x6 PBI保存+68/+70，x5 Model保存+a0/+a8，+d0是全局atomic旧counter值，不native epoch。对三个旧named imports的exact branch inventory仅定位后，新`factory`模式实际 **exit0，1NEW text/3原root indirect-symbol imports**：root `100315488..5924`持有sp60同record、建立sp50单PBI（同record+31/u8→PBI+0、+38/u32→PBI+4），分别传Config/Model/Controller create；后者参数为Model+PBI而非directrecord。未新核manager loaded factory+30选择，不将一次构造关联升级成每次Call action原子snapshot。复用旧model update：先plain store新record+38到原PBI+4，再换Model/Config强record；同PBIcontrol不冻结其值、也不生成revision/epoch。原件/命令/actual stdout/exit、compact `record-instance-contract.json`在主`x9/out/call-record-snapshot/`。
- **Reader与review边界**：`x9/tools/call_context_evidence.py`新增两模式限定上述NEW范围，factory原import用当前root indirect/nlist命名，不套cache slide-mask到root磁盘fixup。call代理只读复用新raw与diff复核，ABI/同owner链无阻断，两个维护提醒（exact ctor exports/flags0、LC_FUNCTION_STARTS硬编码）已补fail-closed guard及C1单B校验；**r1实际资格在补guard之前，最终guard只静态AST parse/diff review，不为已闭范围重采或跑测试**。原review `findings.txt`不改写，已随call目录归档。没有将强Client/PBI/counter/44成功当REGISTERED。
- **MD CLIR actual consumer**：profile新增10个nanoMIPS函数2802B+selected data31B，各decoder **exit0**，旧108/u8→owned req+d0→db19→保message/local_para的enqueue→实际selected dispatch→ordinary dial/prefix，保留queue/type/ECC/special-case门。原literal证raw1=`#31#`、raw2=`*31#`，raw0才查existing CLIR；format `%s%s`拼prefix/原owned号码。仅现有header注释与docs，**无Apple映射API/default/行为变化**。原 **`92847f8f`** 整合为 **`46661476`**；尾追加冲突逐项保留CALL/CLIR小节各一次，SMS段亦保留。raw已无覆盖归档主 **`x9/out/call-profile-input-next/`**，没有覆盖旧`call-profile-input/` current-array证据。
- **SMS actual 同源取件**：通知uint ID→request `kSmsMessageID`→返回CTMessage `setMessageId:`沿原w21静态同源；`188224bfc` 12B→原GOT/pagechain→真export `xpc_connection_send_message_with_reply_sync`，helper原out-reply回填之后才造CTMessage，错误字段`kPosixError/kMachError`。同步取件不是SaveStore/COMMIT收据，也未闭强sms::Model/MD token到通知ID、CTMessage到GUID/成功record。原 **`e458a370`** 整合为 **`99d618f7`**；raw已无覆盖归档主`x9/out/sms-save-authority/identity-boundary.{json,txt}`，不重读SQL/数据库或扫描新server。
- **操作/下一边**：无kernel/host编译、测试/人工向量、CI run、boot pack/设备启动、构建后二次产物检查、ADB/mailbox/mount/分区或共享`.local`修改；没有活跃build/设备worker。最新实际kernel仍生产 **`068d93e0`** 的`x9/out/x9-call-bind-receipt-20261116/kernel.development.sweet`（完整98/98编译link/strip exit0，未boot pack/上机）。下一应在真实同controller/queue边界own当前record与Client并关联本次CallInfo/UUID及实际更新/native epoch，再接loaded typed backend/profile/terminal；SMS下一缺强Model→通知ID→GUID/成功record的同owner边，不重复已闭取件/SQL。VOICE09仍关闭，VOICE20无生产caller，CallList/handover/双向DSP及当前Messages COMMIT/重开/物理ACK/MO对端仍缺；vendor default精确jar仍须用户提供或先确认设备读动作。
- **实际合入/收尾**：三项代理整合及root reader/原件结论提交 **`cf217ae9`** 已ff合main且 **push成功 `6dba5aeb..cf217ae9`**。三路必要scratch已实际rename归档，原source-head/绝对采集路径不回写；CALL/SMS `git cherry` patch-equivalent，CLIR因尾追加context不同显为`+`，已逐字确认原55行小节与完整header均保留，结果在`integration-equivalence.json`。三个本轮worktree tracked clean/剩余out为空后正常remove，再删已整合的本地分支；仅本轮远端`feat/call-context-join`以原source commit的force-with-lease正常删除，未触其它活动树/输入/旧镜像/备份。本最后交接走root功能分支正常commit/ff/push后删root本地分支；所有采样/push句柄已实际终态，无测试/设备worker，完整通话短信goal仍active，不能把这些静态身份边标complete。

#### 2026-11-16 VOICE44 receipt后提交换绑（本轮生产修复，完整kernel编译通过）

- **本轮基线/分工**：root分支`feat/call-bind-receipt`基于main/origin **`0029d93f`**；按用户要求复用三个代理与新树`call-bind-review/contract/lifecycle`，分别核实际patch、既有原件wire/guard、旧terminal及mutation owner生命周期。没有重采旧证据/运行测试或设备动作，不动共享`.local`和其它硬件树。完整通话短信goal仍active。
- **实际代码**：44原路径在best-effort回复前改变binding，另可吞掉已answered注册waiter尚未receipt的旧terminal03。现在新44先在现有32项response pool保存exact11B原req/txn、localgen、actual QMUXgen、旧binding snapshot（initial unbound独立live）、预留unique nonwrapping未来lease；active route/lease/registration只在`bb_x9_call_result`实际14B44 queue receipt后同QMI线程commit。queue/OOM重试不重分lease/不重发MD；same-SIM/同allocation no-op不清reg/handover。changing admission挡旧live非bind response/未answeredwait/同slot-localgen end和stagingdial used owner；pending changing-success挡新03/21/staging20。DEL/identity失效清本地pending/cache但保原MD token/epoch。
- **guard/cache证据**：call代理确认既有`ios-bind.asm`实际仅cleanup尾部，保留25000ms constructor明确属于Dial/已核03/21，**44原timer未闭**。本轮明确local25s admission policy，超时error不commit；完成缓存pin commit后binding，same live exact repeat只回原Result，唯一completed replay用UINT64_MAX（无新mutation）；changed txn保留首owner/result，new44 admission退休per-client旧cache，不称永续去重。BB仍只放行14B44，03/20/21原guard/shape限制保留。
- **静态里程碑（编译前记录）**：三代理已apply exact `x9/out/call-bind-receipt/production-r1.patch`独立review，均无合入阻碍；报告分别本树`x9/out/call-bind-{review,contract,lifecycle}/`，当时待root无覆盖归档。review提醒generic guard措辞已改owned admission guard（不外推44原timer）；existing host fixture仅改queued bind基线/shape断言和旧route event先flush预期，**未执行**。当时尚无正常单对象/CI/kernel/boot结果；实际编译与归档见下，不把静态review称runtime验收。
- **确切编译结果**：生产提交 **`068d93e0`** tracked clean时，固定布局check exit0；scratch r1 recipe漏建输出目录，Builder open build.log前FileNotFoundError exit1（未调用clang），没有掩盖或覆盖。仅修recipe在唯一`compile-r2/`建目录后，真实`X9Baseband.cpp.o` **exit0/True，34warnings**。宿主fixture `xcrun --sdk macosx clang++ -std=c++17 -c` r1/r2均 **exit0/无输出**；r2只让第二次delete/rebind基线按原qmi reply累计值检查，patch原文保留，没执行向量/套件。命令/source-head/status、r1失败、r2正常日志/recipe/results与.o在`x9/out/call-bind-receipt/`。三个stopped代理scratch已实际无覆盖rename至主`x9/out/call-bind-{review,contract,lifecycle}/`，未改旧产物或共享输入。
- **正常完整kernel里程碑**：同生产source **`068d93e0`**，实际执行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py kernel --name x9-call-bind-receipt-20261116`，**exit0，98/98 overlay对象（3 prebuilt）、完整link/strip成功**。新kernel为`x9/out/x9-call-bind-receipt-20261116/kernel.development.sweet`；source-head/clean status、exact command、full-build/build日志和exit均在同目录。完整build后只有上述host fixture源码检查计数修订/交接（非kernel consumer），未重编内核作比较；无CI run/boot pack、构建后二次hash/解包/反汇编检查或上机，不称最终功能镜像。必要产物原本在主checkout，不需迁移。
- **未完成边界/下一项**：VOICE09仍不公告、20无生产caller；record/config不是revision observer/nativeepoch。typed caller/owned record→profile/binding/epoch→matched terminal、CallList/handover后身份、双向DSP及当前Messages同token COMMIT/重开/物理ACK/MO对端验收仍缺；不造ready/ACK，不称通话短信修好。精确vendor CLIR jar仍需同版原件/用户允许设备动作，不为本修复自行ADB/boot/mailbox/写分区。下一真实接线从已owned record/client的变化边界接实际typed backend/profile和nativeepoch，不能以44本地成功冒充record版本；SMS沿当前iOS最终COMMIT/ACK authority，不重扫已闭release fence。完整goal保持active。
- **实际合入/收尾**：生产 **`068d93e0`** 与编译交接 **`b7dcb9d3`** 已ff合main且 **push成功 `0029d93f..b7dcb9d3`**；三路scratch已无覆盖归档主checkout，三个review树的tracked差异逐项确认仅父代理提供的exact r1 patch后restore，再正常删除本轮worktree/本地review分支，未动其它活动硬件树/共享`.local`/旧kernel/备份。root feature已合入，此最后交接走同功能分支正常commit/ff/push后只清root本地分支；本轮未建远端feature分支。所有compile/build句柄均已终态exit0（r1 scratch错误保留），没有活跃测试/设备worker；新kernel未boot pack/上机，不以此宣告完整通话短信完成。

#### 2026-11-16 model+58 原件类型纠偏与短信释放 fence（新窄接线前置）

- **基线/实际前轮收尾**：CLIR生产 **`b7b4301b`** 后，最终r2交接 **`401a92c7`** 已ff合main且 **push成功 `b7b4301b..401a92c7`**，旧 `feat/call-profile-producer` 已删除。root新分支 **`feat/call-record-config`** 基于401a92c7；复用用户授权的call代理窄追新增ctor/config，不重新扫描旧getter/queue/CallList，另用profile代理静态review维护reader。完整通话短信goal仍active，不把原件资格当功能完成。
- **新证纠偏**：原 `IMSSubscriberModel C1 1f2e76ed0→C2 1f2e77538` 的x2为按值ConfigInterface强pair（ABI间接地址），强copy到model **+58/+60**；x4的PersonalityBoundInstance另在+48/+50。**+58不是此前推测的第三方observer**。Config原ctor `C1 1f2e42d6c→C2 1f2e42d70` 安装addresspoint **20e056820**；原rebase-chain槽 **20e056830/+10→1f2e430fc** 由真export核为 `IMSSubscriberConfig::updatePersonalityInfo`。两对C1/C2都是同参数直接forward，不沿用其它类的VTT假设。
- **一条完整owned flow**：复用旧model-update原raw先写+28/+30，再直接call强Config；Config8B `add this,#38; b1f2e43104` 进入此次新核120B helper，先retain incoming、写config+38/+40、再release old、同步return。旧control最后强引用释放的virtual/deleter不是新record订阅通知。此确切调用链没有第三方observer登记/独立dispatch，不推断全系统无其它观察机制；更不是本地record revision、native epoch或registered/media-ready。不能根据这个字段建假订阅器。
- **确切原件资格/失败保留**：call代理 **r4 exit0**，原七份新scope text于失败r2已与当前cache比较成功，r4复用它们和原page，仅新增helper **120B / cache31 offset61c3104**及必要export/slide元数据；7个真callable/vtable export均 **flags0 REGULAR**。r2/r3 empty-name失败真实原因是scratch trie suffix cursor被提前推进，不是ABS kind或缺vtable；root review定位后修复，全部失败/首个path失败/`--macho`忽略range的巨大invalid均保留。成功 `x9/out/call-owned-next/record-observer/{qualified-r4/original-config-proof.json,original-proof-r4-exit.txt,model-config-contract.json,commands.txt,reader-failure-and-resume.txt}`，原source-head仍 **aad2b3ab**，原绝对采样路径不回写。
- **维护改动/验证范围**：`x9/tools/call_context_evidence.py`新增 **config-only**，未来仅8个新text/1data/system export，真pagechain/update/vtable export校验，不读旧producer/client/queue/cxx/dispatch范围；NEW输出仍限本树x9/out，无scratch import或设备/mount。独立代理对7text版本static review无阻碍，root再审新helper120B/额外vtable失败关闭guard；最终仅 **Python static syntax parse OK**，实际资格来自上述scratch，不为维护版重新采样。维护输入/语法结果保留 `x9/out/call-record-config/`；旧三模式输出名/keys保留，docs纠正+58旧类型。无kernel生产delta、编译/CI/boot pack/测试或构建后产物复检，本轮不重编CLIR对象/旧kernel。
- **短信排除有据、无猜测补丁**：SMS代理确认 `coachz_bb_qmux.h:133,147–159`不复用release_pending槽；`coachz_baseband.cpp:1674–1686`在DEL_CLIENT callback完成后才清pending2；`X9QMIAT.inc:864–873`先x9m_del清旧订阅/jobs再递增local generation，MT helper亦拒绝pending/unused。故helper未带显式actual generation形参并不能证明跨client发布，不新增无据API/ACK/ready。这条窄链无独立新production缺陷；确切txt **`x9/out/sms-owned-next/owner-boundary.txt`**，当前Messages同token保存/重开/物理ACK及MO对端验收仍缺，旧319/220和Android raw ACK不替代本版本。
- **实际归档/下一动作**：三个代理均已完成停写；本轮 `call-owned-next`（含全部失败/r4）、`sms-owned-next`、`call-config-review`已整目录 **无覆盖rename到主x9/out**。仅恢复root在review树apply的同一diff，确认两树tracked clean后删除本轮三子worktree/分支；其它硬件树、.local长期输入、原件/镜像/分区备份完全不动。下一需真正typed action同owned record/revision→logical allocation/txn→native epoch→matched terminal的关联与更新/退休producer；44只带SIM、20只带号码，+58强Config赋值不能补这条。selected default仍缺同版 `/system_ext/framework/oplus-telephony-common.jar`，不自行采集；VOICE09未公告、20无生产caller，CallList/handover/双向DSP仍未完成。上一有效完整kernel仍 **3c4a49a0 / x9/out/x9-call-terminal-receipt-20261116/kernel.development.sweet**（未boot pack/上机），没有通话短信最终修复镜像。维护/docs正常提交合main/push的确切commit与root分支清理结果随后记录。
- **实际维护合入/最终交接**：维护reader与原件类型/docs纠正 **`38fc7fc8`** 已ff合main且 **push成功 `401a92c7..38fc7fc8`**；最终维护源码的语法结果 `x9/out/call-record-config/{syntax.log,syntax-exit.txt}`实际 **exit0**，输入head401a92c7＋`reader-source.diff`，未执行维护reader/重新原件采样或kernel编译。所有本轮scratch与失败/r4证据已经在主x9/out，三子树/分支已删除；这条最后交接正常提交ff合main/push后仅删root本地 `feat/call-record-config`，不改其它树或产物。完整goal仍 **active**，不是complete/paused/blocked；下轮从真实typed owned关联与独立call/profile/state/media边继续，不重跑本次成功CLIR编译、config原件或已排除SMS释放类。当前没有可等待的build/采样句柄，不能拿维护commit或旧日志拼出通话短信验收。

#### 2026-11-16 显式 Android CLIR profile 保真（本轮，代码已单对象编译）

- **续接/范围**：上一轮actual context reader与typed强Client/re-add证据为progress。本轮主checkout `feat/call-profile-producer` 基于干净main/origin **`aad2b3ab`**，单代理；不重读getter/queue、旧CLIR/prefix/CallList原件，不连接设备、不动其它worktree或`.local`输入。完整通话短信goal仍active。
- **新的真实代码改动**：已有matching Java temporary SC31 producer给raw1/2，先前已认证的Rtm/Rmm non-ECC普通decimal路径将它们原值下送108；缺vendor default只阻断默认选择，不是已明确raw1/2的值。当前 `x9_call_wire.h` 仍把所有profile限定clir0且写常量0，不能接这两个有据显式输入。本轮扩展**现有**profile validator/native901 validator到raw0/1/2，encoder逐字段复制validated profile而非覆盖CLIR；在输出前own-copy scalar profile与digits。没有新增API、Apple enum/byte cast、prefix解释/default selector，SIM1、type1/domain0/ecc_domain0/号码/宽度/order/padding限制保持。
- **边界/操作**：仅codec能力修复，不等于真实iOS producer/owned binding/profile已接线；QMI附加CLIR TLV仍明确拒绝，不从缺字段生成默认值。VOICE09公告与VOICE20 caller保持关闭，短信生产路径未改。测试源补显式1/2只改108与无效3/0x100的期望，**未运行测试脚本/套件**。主目录 `prepare-local.py --check` **exit0**；本次仅编译两个实际消费者 `X9Baseband.cpp.o` / `sweet_x9_gpu_dependencies.o`，输入是aad2b3ab＋本codec diff，日志/脚本 `x9/out/call-profile-producer/{compile-recipe.py,single-compile.log,prepare-local.log}`，确切完成结果见下，不称已link或新镜像。selected CLIR同版vendor jar仍缺，完整typed backend/epoch/terminal、CallList/handover、独立双向DSP及当前Messages最终验收未完成。
- **确切编译结果/里程碑**：上述本地recipe单对象编译已实际 **exit0 / 两项True**，Baseband **34warnings**、MD consumer dependencies **251warnings**；原输出 `compile/recipe-results.json` / `compile-exit.txt` / `single-compile.log`。source-head仍 **aad2b3ab**，源码改动单独保留 `source-input.diff`，不以事后commit替换构建输入。当前diff已review；现有docs末节注明陈旧clir0限制与明确raw1/2可独立修复，语义/枚举转换仍不猜。没有运行人工向量、测试套件、完整kernel link/boot pack、CI、构建后二次产物检查或设备操作；无新最终镜像，旧3c4a49a0 kernel与所有镜像/备份不动。下一实际接线应将真实typed privacy/owned record绑定到明确raw输入及native epoch，而不是从空pair/isPresent/default0授予profile；完整goal继续active。
- **最终源码r2/并行续接**：review后改为**先copy scalar profile，再验证同一个local snapshot**，避免验证caller storage后重新读取字段；这不承诺无锁并发读取原profile为原子record。最终diff保留 `source-input-r2.diff`，仅因此真实源码变化执行 `compile-recipe-r2.py`。续接等待原session **29457**取得终态 **exit0**，`compile-exit-r2.txt=0`、`compile-r2/recipe-results.json`两项True，`single-compile-r2.log` **34/251warnings**；未检查对象二进制，未link/pack/上机。用户本次明确多代理，新增三个独立 `feat/{call-profile-review,call-owned-next,sms-owned-next}` / `.worktrees/`树只读静态审查最终diff、真实call owned接线及SMS owner下一窄缺口；root负责提交/合入和构建，代理不跑测试、不采集设备或重扫已收束原件。结果/确切提交与收尾随后记，不把新增审查当功能验收。
- **独立review结果**：profile代理已停写，`static-review.txt`确认alias/长度/width/padding和unsigned CLIR准入无合入阻碍；真实物理提交 `x9_mipc_bridge.inc:131→x9cw_request→x9cw_native_dial` 消费此修复，`x9cw_dial_join_begin`亦复用profile validator，但仍不能以强Client/scalar copy授予typed binding或REGISTERED。`git diff --check`通过；`gh run list --branch main --limit3`实际 `[]`，无CI run ID。通话owned/SMS两代理仍只读进行中，结果不先行编成成功。
- **实际生产合入/下一窄范围**：CLIR源码/最终r2交接 **`b7b4301b`** 已ff合main且 **push成功 `aad2b3ab..b7b4301b`**。review代理停写后原scratch整目录已无覆盖rename到主 `x9/out/call-profile-review/`；只恢复root在其树apply的同一源码diff，tracked clean才移除该树/分支，不动其它输入/镜像/活动树。call源码审查确认44只有wire SIM、20只有号码，当前lease只感知44/实际QMUX generation，不感知同ID强record替换；现有getter不是可用的record revision producer，不能靠开放09或默认profile接通。下一新增原件范围限 **IMSSubscriberModel create/ctors→model+58具体observer→virtual+10 owned通知**，只读既有update raw、不重采旧getter/queue/CallList；`call-owned-next`代理继续这条新窄静态链。SMS代理尚未发现独立生产缺陷，当前版本最终保存/物理ACK/MO验收仍缺；完整goal仍active，本轮不重编已成功对象或旧kernel，不交付最终镜像。

#### 2026-11-16 actual context producer / typed强Client / re-add 边界（本轮）

- **当前基线**：主checkout单代理 `feat/call-context-producer`，HEAD/main/origin为 **`b93ea5a5`**，续接时tracked干净。上一轮原始证据在 `x9/out/call-context-producer/`，21个text/6个data/25个export已读取；**`qualified/original-producer-proof.json` 的四个 optimized_queue_stubs target 因decoder漏LDR/GOT无效**，不能用其target命名dispatch。保留旧输出；下一仅新增queue/weak-lock补证，不重复21个范围。
- **已观察的链**：真实CommCenter tracker callback `1007a5150` → manager added/enabled → 强controller `getModel_sync1f2e421c8` → model `updatePersonalityInfo1f2e77c04`，替换model+28强pair。PersonalitiesTracker独立serial child queue target来自原handler+18；unbind先清旧hookup+20，两个异步阶段检查旧hookup，不等于native epoch fence。removed走原controller清理，尚未证明erase manager map或立即退休Client。
- **执行/剩余**：上一轮无tracked修改、commit/push、构建/测试/CI/boot/ADB/mailbox/分区操作；本次先补交接再修queue原件decoder、追Client生命周期和实际typed producer接线。完整通话短信goal仍active；VOICE09关闭、VOICE20无生产caller，selected CLIR仍缺同版 `/system_ext/framework/oplus-telephony-common.jar`，不自行重新采集。上一有效生产kernel仍 `3c4a49a0` 的 `x9/out/x9-call-terminal-receipt-20261116/kernel.development.sweet`（kernel build exit0，未boot pack/上机）。
- **本次确切补证**：scratch decoder新增queue-only，按原mapping slide-v2 `mask=00ffff0000000000/add=0/page4096`与实际page chain核四个GOT，真实dispatch targets `18dba5310/18dba555c/18db970d0/18dba5814`；weak-lock veneer `1f2f7b31c→194712078`亦由原12B与真export闭合。`qualified-queue/original-queue-proof.json` **exit0 / 0旧text / 0旧data / 5exports / 5targets**，不覆盖或重跑21个旧范围。旧proof四个target仍标无效。
- **新的typed强owner/availability边界**：普通factory准备Client的helper `100dd1168→100dd3498` 经slot→原PersonalityID→强Registry manager（RTTI import `10151b320`）→primary virtual+18 **`getIMSClientRef1f2e4c210`**。原queue manager+18用sync/async-and-wait，map block **`1f2e61a04`** 按真实ID查强controller→virtual+60 **`getImsClientRef1f2e68b38`** 返回controller+d8/e0强Client。首predicate veneer `1f2fb16cc→1f038b750` **实际为isPresent，不是sd::isSimPersonality**；只判record存在，不证type/物理slot/REGISTERED，slot→ID/presence/排队Client三步非原子record/revision。本轮首次target校验因猜错predicate名字而exit1，9个text全部比较成功且raw已保留；未重读它们，resume只读新isPresent text/targets，结合保留raw与7个data/vtable实际rebase，`qualified-client-targets/original-client-proof.json` **exit0 / 9保留text+1新text / 7data / 11exports / 3targets**。旧failed目录/两个空objdump/short-VA invalid都保留，不作成功证据。
- **retirement明确收束**：完整manager removed函数不erase其map；controller removed+a0/+a8为普通deinit/network-services。**若同ID entry仍在**，added会更新model强info并走controller+170，然后+90 createClient；旧 `createIMSClient_sync1f2e68054..58` 在controller+d8非空时直接返回，故Client/clientId可能跨record更新/re-add保留。不能把clientId或临时Call析构作为native epoch，不能以旧Client强owner寿命证明新record绑定有效；不宣称实机已发生这条条件流或全系统没有其它erase路径。
- **维护改动/验证边界**：新增 **`x9/tools/call_context_evidence.py`**，独立Mach-O reader/固定范围/真export/rebase chain，producer/queue/client三模式，NEW输出限定本树`x9/out`，无scratch import、mount/设备动作或旧输出覆盖。只静态Python syntax parse OK；本轮原件解码实际执行的是scratch版本，不为维护版重复采样。原链与证据修订写入现有 `docs/call-ios-bridge.md`，无production call/SMS行为修改、构建/测试/CI或新镜像。下一需真正owned record→logical binding/native epoch→profile/admission→typed terminal接线；不能交付空loader/default profile。记录在本条，未新建Markdown过程报告/实验`.local`；单代理，无活跃build/采样句柄可等待。
- **实际合入/收尾**：reader与原件ABI结论提交 **`e4773f9e`** 已ff合main且 **push成功 `b93ea5a5..e4773f9e`**。全部raw/decoded与compact `context-producer-contract.json` 都原本就在主checkout `x9/out/call-context-producer/`，没有迁移/覆盖其它树、旧镜像、长期输入或备份。此最终交接正常提交/ff合入/push后仅删除本轮本地 `feat/call-context-producer`（没有新worktree或远端功能分支），其余活动worktree不动。完整goal仍active；下一离线工作从现已闭合的typed强Client/availability与re-add边界接真实binding，不重扫旧CallList/CLIR wrapper或用isPresent伪注册。

#### 2026-11-16 per-personality 强owner与真实路由 ABI（新caller前置，无生产补丁）

- **续接分类/基线**：上一轮selected CLIR factory原DEX改变下一采集动作，属于progress，不以两条docs commit冒充修复。本轮先核main/origin **`396d74ea`** tracked clean，selected jar仍缺且没有新的设备确认；单代理分支 **`feat/call-personality-context`**。CallList第七byte/Call_Type现有证据未改变，未重扫旧parser/producer或给默认0；转向独立的**真实per-personality context producer**，不把缺CLIR jar当作所有离线工作的阻断。
- **新可复用producer与枚举**：原cache真exports/text证实 `PersonalityIDToSimSlot1f0323a38` 经initialized Registry/强PersonalityShop按真实ID查强record；缺shop/record为0，但record+31==1且原配置owner+111 false时会**策略返回slot1**，不能把返回值无条件当物理卡。`sd::isSimPersonality1f03390cc`只接受非空record+31==0，两次query不是原子record/revision。`simSlotAsSubType1f1ab81f4`保留1/2/3其它转0；`simSlotAsInstance1f0322cc8`是1→0/2→1/其它−1；原VOICE44另把subscription2→wire1。当前native bridge仍限定sim0/1，未加SIM3/缺失fallback或09公告。
- **强owned context ABI**：`PersonalitySpecificImpl::personalityInfo1f032651c`在x8返回实际this+8/+10强pair并increment control，不是借用pointer；同对象 `simSlot1f031f1fc`直接读owned record+34，不走Registry单slotfallback。原真export **C1 `1f0392bbc`** 的pair形参x1，**C2 `1f0392b84`** 的x1为construction/VTT、pair在x2，不能按相同demangled名混调。personalityId getter `1f0392cbc`有x8 sret，但本轮不替未新核的尾helper命名，也不认作callUUID/clientId。这闭的是可用强owner及路由枚举，**具体typed caller/object adjustment、原queue/rebind fence、native epoch与事件delegate接线仍缺**，未添加unused adapter/API或空loader。
- **确切原件/操作**：`x9/out/call-personality-context/` 的两份original proof JSON分别覆盖5个routing与4个新owning ranges/exports，全部与当前根盘Cryptex cache text相符；第一次5项输出保留，第二次只读新增4项不覆盖前者。保留原bytes/decoded、`personality-context-contract.json`、source-head、原stdout/stderr和实际/重放command记录；最终 `read-original-context.py routing|owned <新out>`可在唯一scratch重放，不覆盖旧输出。14个exact `PersonalityIDToSimSlot` CommCenter stub callers和一个原caller切片已读，但**不宣称所选caller就是CallFactory/typed dial**。初次M库名字少`Drivers`导致objdump exit1，改到已有的真实文件后exit0；两个原cache qualification均exit0。无构建/测试/CI/boot/mount/ADB/mailbox/分区或产物复检，未触其它树/共享`.local`。
- **完整目标/下一动作**：新证据使下一context接线能使用实际per-personality强owner并明确拒绝不具物理provenance的helperfallback，而非从临时Call*/clientId猜卡。仍须具体原caller/queue/fence、完整loaded typed backend/profile/terminal、CallList/handover、专用双向DSP及当前Messages同token COMMIT/重开/物理ACK/MO对端验收；**VOICE09仍关闭，无新生产补丁或最终镜像**。selected CLIR仍需用户确认Android/ADB并准许只读取得 **`/system_ext/framework/oplus-telephony-common.jar`**，或提供同版本地原件。完整goal保持active，本轮是新caller依赖证据，不标complete/blocked。
- **实际收尾**：原件ABI/交接提交 **`c277b66a`** 已ff合main且 **push成功 `396d74ea..c277b66a`**；必要raw/decoded产物原本在主checkout `x9/out/call-personality-context/`，没有迁移/覆盖其它输入或重写source-head。此最终交接正常ff/push后仅删本轮本地 `feat/call-personality-context`，本轮无worktree/远端功能分支，所有其它活动树与旧镜像/备份不动。无活跃采样/构建句柄可等待；下一可离线窄追上述实际owner getter的具体typed caller/queue，不再重读已闭的5项路由枚举/4项owned getter。

#### 2026-11-16 selected CLIR factory 原 DEX 续接（新依赖证据，无生产补丁）

- **基线/范围**：干净 main/origin `bc91abb1`，本轮单代理、功能分支 `feat/call-clir-selected`。上一轮 `3c4a49a0` terminal ownership 修复/完整kernel已交付，本轮不重编它、不重复旧CallList/profile扫描、不操作设备/分区/mailbox或其它硬件树。typed caller/context/backend、媒体及当前短信最终验收仍缺，完整goal保持active，不把本条静态进展当功能完成。
- **新的确切依赖**：沿 `ImsPhoneCallTracker.mInterfaceImpl → OplusTelephonyFactory` 首次窄追 factory；matching `telephony-common.jar` 的真实 `getInstance` owner-user 分支指定 **`/system_ext/framework/oplus-telephony-common.jar`**、**`com.oplus.internal.telephony.OplusTelephonyFactoryImpl`**。该 jar 不在已抓 `files-manifest.json`，本树原件/备份 exact-basename 查找也未找到；已有 `oplus-framework.jar` 是 wrapper，不能代用。不是再次声称已抓的 Java/DSP 全部缺失，也不能把 Android user0 当 SIM0/personality。下一应取得同版这个精确 jar 后闭 `getFeature` 的 tracker 选择/default override；本轮未重新连接或采集ADB，设备操作仍先确认。
- **原 bytes 与反编译冲突**：独立读取原 `classes.dex` 四个 code_item：factory getInstance **`0x12b348`** / newInstance **`0x12b4a4`**、tracker getClirMode **`0x20ed00`**、interface default **`0x329aec`**。ASC此次 Java 遗漏 `mLock` monitor 及反射 `Exception` fallback；原try/handler证实加载失败会构造 base factory、清 valid flag，catch-all exit monitor后rethrow。真实 per-sub `clir_sub_key+getSubId` 和 interface default0原指令已确认，但 vendor选择/实际default值仍无runtime证据，不能归零或开09/20。保留main/radio日志没有此窄factory/default检索的决定性输出，不据零命中推断fallback。
- **工具/产物/重放**：`x9/out/call-clir-selected/` 保留 `selected-factory-dependency.json`、四份 `*.dex.asm`、`original-method-locations.json`、`read-original-methods.py`、ASC stdout/stderr与 `executed-commands.txt`。ASC getmanifest exit1（JAR没有AndroidManifest），list/findrefs/getclass exit0。初次 Androguard 原DEX读取因Android17 hidden-API enum4 exit1，decoder仅保留未知metadata numeric值后exit0；archive/DEX未改，每条指令与原offset/raw bytes相符。重放用同目录脚本＋manifest所列telephony-common原件，但选新的output目录，不覆盖本次原记录。不跑测试套件/编译/CI/boot或构建后产物复检；无新镜像、无新的短信COMMIT/重开/物理ACK/MO对端验收。
- **实际收尾**：契约/交接提交 **`0a3f35e5`** 已ff合main且 **push成功 `bc91abb1..0a3f35e5`**；输出原本就在主checkout同名 `x9/out/`，没有迁移/覆盖其它输入或产物。此最终交接按功能分支ff/push后仅删除本轮本地 `feat/call-clir-selected`，无本轮worktree或远端功能分支；其它树不动。尚需用户确认Android/ADB状态并批准只读提取上述精确jar，或提供其同版本地路径；不要求拨号/来电、boot或刷入探针。完整goal仍active，未满足连续三turn阻断审计，不标blocked/complete。

#### 2026-11-16 CALL terminal reply 所有权续修（本轮生产代码，完整目标未完成）

- **基线/范围**：main `e4f33dae`，`feat/call-terminal-receipt` / `.worktrees/call-terminal-receipt`。本轮单代理，不操作设备、分区、mailbox或其它硬件树。读取既有CallList原件契约后确认第七byte/Call_Type仍未闭合，不猜字段、不再重跑已排除producer路径。转而修复实际源码的独立缺口：END21终态、staging DIAL20汇合与注册03失败回复原来通过void/port-recognition发送后擦除owner，无法确认主机队列真正接收。
- **实际生产修改**：新增32个无号码terminal response owner，在接受03/21及staging20、MD操作之前预留；保存原txn/msg/result、独立VOICE SIM与本地generation、真实QMUX generation和不回绕binding lease。job决定结果与host queue receipt分离，cleanup不擦掉待投递结果；queue/OOM只重试本地投递，不重发MD。已决定的错误不改写，尚未投递成功跨原25s/60s guard转NOT_READY，bridge mutex内再核原deadline与真实QMUX generation。原成功注册Result03＋54仍全有或全无，新增实际QMUX generation guard；54批次也复核真实allocation。删client/换binding丢旧host结果但保物理token/epoch，SIM切换再切回不能复活旧owner。staging dial不再借NAS sim_slot作VOICE路由；仍无生产DIAL调用者。
- **编译里程碑**：主checkout固定布局`prepare-local.py --check`实际exit0；worktree的check因共享maintained-source链接仍指main而exit2，未改共享链接。首次recipe中修改的`X9Baseband.cpp.o`成功，未修改的C dependencies单编因本树缺gitignored `x9_mdota.inc`失败，不掩盖。宿主fixture只更新API签名/独立VOICE owner输入并`xcrun --sdk macosx clang++ -std=c++17 -c`编译，最终exit0/无输出；没有执行人工向量或测试套件。重构dial poll时编译发现pointer `sizeof`警告，已改回显式4096B take/erase后重新编译通过。日志/recipe在本树`x9/out/call-terminal-receipt-compile/`，正式完整kernel构建结果随后记录，不把单对象成功当整包成功。
- **正式完整kernel结果**：固定生产源码提交 **`3c4a49a0`**，tracked clean；实际执行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py kernel --name x9-call-terminal-receipt-20261116`，**exit0，98/98 overlay对象/3 prebuilt、完整link与strip成功**，修改的C++单元34warnings（包括仍unused的staging dial）；正常builder生成缺失MDOTA输入后C对象也编译通过。产物`x9/out/x9-call-terminal-receipt-20261116/kernel.development.sweet`及source-head/status、exact command、full-build/build日志、exit code在同目录；gh main查询`[]`，无CI run ID。没有boot pack、测试执行、构建后二次产物检查或设备启动，不称最终镜像。构建交接提交不替换source-head。上述完整目录和单编目录在合main前无覆盖归档，实际push/清理另记。
- **实际归档/合入**：代码`3c4a49a0`及构建交接`51603a0f`已ff合main且`git push origin main`成功 **`e4f33dae..51603a0f`**；两组输出目录已实际rename到主checkout同名`x9/out/`，预先确认目的地不存在，没有覆盖旧镜像/输入或回写原构建绝对路径。此最后交接正常ff/push后仅清理本轮worktree/本地功能分支，其它活动树和共享`.local`不动。
- **完成边界/下一项**：这些是回复所有权和真实allocation fencing的生产修复，不是CallList publisher、typed caller/backend/profile接入、handover后ID/CS动作或通话双向媒体完成。VOICE09仍关闭，Bind44/BUSY/unsupported等即时拒绝仍为旧best-effort，未宣称全VOICE响应可靠。SMS现有生产链未见新的matching契约矛盾，本轮不制造ready/ACK，当前镜像Messages同token COMMIT/重开/物理ACK及MO对端验收仍缺。完整goal保持active；先完成本次真实源码的正常kernel编译，再沿未完成caller/profile/CallList/media与当前SMS验收继续，不能交付“通话短信已修好”的最终镜像。

#### 2026-11-16 提取与三路新证据收尾（已归档/push；仍无最终修复镜像）

- **实际完成**：18个同版原件仍在 `x9/out/android-call-inputs-20261116-092442/`；追加五份当前日志/状态快照已无覆盖rename至主 `x9/out/call-owned-runtime/`。三个原代理因服务stream资源错误停写，复用其原功能树由三个短收尾代理消费既有产物，不重跑ASC/提取/反汇编/hash/设备。三组scratch均已无覆盖归档主 `x9/out/{dial-java-producer,voice-dsp-payload,init-sms-java}/`，原日志/源码head/失真摘录保留，原件与衍生不混写。
- **CLIR新契约**：原 **`0240bb29`** 整合 **`27ce06db`**。matching ASC反编译闭显式SC31 action*=raw2、#=raw1、temporary非空actual number、per-sub preference key及MTK session/adapter→HAL clir原值。核心只有反编译、没有原DEX opcode复核；实际厂商default实现/运行值仍缺，interface0不是运行默认；phone index不等于iOS personality/subscription/native epoch。完整边界及现有命令产物索引为 `matching-java-producer-final.json`；replay是明确标记的重建recipe，不伪造历史命令执行。
- **DSP新契约**：原 **`49f9b40f`** 整合 **`096bd861`**。真实audio_ipi section asm闭48×8B task-info producer及384B ioctl copy；scene0只在getcoretotal>1条件下派生dspid3→physical1/controller43，fallback1→2/0→4，不授予实跑8B。真实snd-soc-mtk-common USIP AD50生产两个u64 `{AP PA+0x30000,0x30000}`，非DSP receipt。DSP四段/cert容器候选与字符串不是loader/core/执行证；首缺独立主adsp loader/getcoretotal、scene0消费和实际通话路由/独立双向I/O。原依赖追取因ADB断连exit255未读分区，失败证据留存，不重试或用旧备份替matching。`final-dsp-contract.json`/`final-evidence-index.md`可只读复核。
- **SMS新契约**：原 **`01bda47b`** 整合 **`683e20e6`**。matching Java/保留DEX摘录闭条件化CS普通接收：raw insert成功排事件2返回1，事件1随后ACK；duplicate5转1不排新事件2，result−1跳ACK。Android decimal RIL37不同于QMI WMS37，不等最终app保存/物理completion。长ctor摘录标题被method-name覆盖，真实长code_item **0x175c0c/512B**、短 **0x175e1c/26B**，原JSON资格及derived-ref/异常表缺失界限已记录，不重写原错标题。现有memory-full D3无新矛盾，不制造ACK/ready；`matching-sms-final-contract.json`保存精确offset/重读入口。此Android链仍不能替代当前iOS Messages COMMIT/重开/物理ACK和MO对端验收。
- **实际整合**：只读快照工具 **`d0bd4695`**（正式Python语法编译exit0，新增snapshot action未重新上设备运行）及118行三组新契约已ff合main，`git push origin main`成功 **`ea18da65..683e20e6`**。两处append conflict逐一确认原commit仅尾追加，完整原小节在main各保留一次后才清理三个子worktree/分支；未操作其它硬件树或共享`.local`。无kernel生产delta/编译、CI run、boot pack或设备启动；最后本交接随root分支正常提交/ff/push后清理。
- **下一真实边/完整目标**：不再重复“Java/DSP文件没有”旧阻断；matching意图及当前模块可继续用。ADB若重新连接，只读追取主adsp模块前应重核sole selected device/active slot和确切partition大小，沿已有vendor_boot ramdisk来源，不猜sysfs/ioctl/路由。完整typed caller/backend、per-sub owned profile/generation、CallList/handover与媒体、当前iOS短信端到端仍未落地/验收，**VOICE09仍关闭，没有修好通话短信的最终镜像**；此次输入/契约收束不算生产修复，不要求再拨10086或补来电。

#### 2026-11-16 同版输入续接：追加当前只读日志快照并开始三代理静态分析

- 原件采集提交 **`ea18da65`** 已实际ff合main并push成功，原`call-input-capture`树/分支已移除；18个原件保留在主checkout `x9/out/android-call-inputs-20261116-092442/`，没有重新提取/覆盖。复用用户授权的三个代理，新树分别为`dial-java-producer`、`voice-dsp-payload`、`init-sms-java`，root为`call-owned-runtime`；共享`.local`及其它硬件工作树不动。
- root同一私有serial/唯一connected device前提下补抓 **radio/main logcat、telephony.registry、telecom、dmesg** 五份只读快照，原stdout/stderr及确切命令/exit/bytes/SHA在本轮`x9/out/call-owned-runtime/`；未清日志、拨号/短信、安装、setprop、重启或写分区。radio现有窗口为 **11-16 09:15:01.518–09:31:55.485**，不能代替11-15已保存MO/呼入，也不因当前窗口缺初始化/SMS事件而断言未初始化。当前telephony snapshot两个Phone Id的call state为0，telecom `mCalls`为空，当前不是通话路由/媒体成功证据。
- 将上述窄只读快照流程保存在现有 **`x9/tools/record_android_calls.py snapshot --out <case>`**：沿用case私有session、确认sole selected device、固定五个读取命令、45s单命令上限、0600文件、原stdout/stderr保留及私有manifest。预先检查所有输出名并exclusive创建，禁止覆盖已有快照；记录可识别源形状而不单信su exit0，不称通话/SMS receipt。不启动后台worker，不包含任意shell/ioctl/设置探针。正式Python语法编译通过；没有重跑采集、测试套件/人工向量、生产源码编译/CI或boot pack。
- 同版`audio_ipi.ko`分析发现`get_adsp_core_total`/主loader为外部import；原`/proc/modules`确有独立`adsp`，完整mounted模块inventory没有其文件。只读补列system_dlkm亦无匹配；基于此新依赖尝试读取active `vendor_boot_b`的槽位/长度时，ADB返回 **device not found**，未读分区。失败stdout/stderr保留，不将exit/空文件当提取成功，不重试、不改设备或拿旧备份默认同版。已抓18个原件和五份日志快照不受此断连影响；未获得主adsp模块，不能宣称firmware loader/core数量已闭。
- 当前新增输入供三个代理继续matching CLIR producer、DSP容器/实际模块consumer与Java初始化/SMS ACK owner。完整owned iOS caller/backend、CallList/handover、独立双向voice及当前iOS Messages/对端验收仍未实现；**VOICE09未开放，无最终通话短信镜像**。本轮采集/工具不计作生产修复；代理结果与实际归档/整合收尾待下条记录。

#### 2026-11-16 用户重新连接ADB：同版Java/音频原件已只读提取（首批输入阻断解除，功能仍未完成）

- 用户明确 **“adb已连接 提取吧”**，本轮从`e6e7b0c0`建`feat/call-input-capture`，只读ADB。先要求唯一connected device，并与旧call session私有serial一致才采集；identity/root身份和build实际回读均已保存。当前fingerprint仍为`OPPO/PLJ110/OP5E17L1:17/CP2A.260605.016/B.46c413f-20895a6-20bdbdd:user/release-keys`，slot **`_b`**。没有拨号、发短信、安装、logcat clear、setprop、重启/flash或分区写入；其它工作树、`.local`和分区备份未改变。
- **已抓18个原件/239438658B**：7个JAR（含此前首缺`telephony-common.jar`5996240B、`framework-telephony.jar`539925B，以及framework/oplus/mediatek/ims/telecom依赖）；3个APK通过本次实际`pm path`取得TeleService/Contacts/ImsService位置，不猜路径；7个同版audio模块通过实际模块文件inventory匹配loader名取得；当前active **`audio_dsp_b`33554432B**按实际by-name链接/blockdev长度只读cat。10个Java ZIP均含真实classes*.dex，模块均ELF，DSP长度匹配；这是抓取原件资格，不是构建产物复检或功能验收。
- 原始/衍生材料已无覆盖归档主checkout **`x9/out/android-call-inputs-20261116-092442/`**，raw文件在`files/`保持设备路径，`files-manifest.json`含每项确切ADB command/exit/bytes/SHA，私有serial仅`session.json`等gitignored文件。实际模块名是`adsp-v3.ko`、`audio_ipi.ko`、`mt6993-mt6681.ko`、`snd-soc-audiodsp-common.ko`、`snd-soc-mt6681.ko`、`snd-soc-mt6993-afe.ko`、`snd-soc-mtk-common.ko`；原件与旧模块名inventory区别已保留，不将su返回0单独作为成功（部分错误在stdout）。
- 本次实际`/proc/asound/cards/pcm`给出 **card0/PCM41=Hostless_Speech**。这是当前Android descriptor，不替代旧MO的call-time mux/trigger、scene0 task-info8B或DSP receipt。ASOC debugfs、ADSP/audio_ipi module参数面不存在，tiny*工具不在PATH；未盲发ioctl或新增探针，未把idle状态当通话路由。ASC已按原件做ImsService manifest、telephony/IMS窄class inventory，真实GsmCdmaPhone/GsmMmiCode/ImsPhoneCallTracker类存在；尚未将class存在当producer闭合。
- **下一真正动作已改变**：matching Java和同版DSP payload不再是“文件未抓到”的阻断，可以从新原件继续CLIR意图→raw producer、初始化/短信owner及DSP容器/consumer；仍需完整owned iOS caller/backend、CallList/handover、独立双向音频与当前iOS短信端到端验收。VOICE09仍关闭，无production delta/编译/CI/boot镜像，本次只完成用户要求的只读原件提取，不声称最终修复。用户明确续接后的blocked审计重新开始；不以历史blocked阻止本次获准采集，也不替用户控制的goal恢复状态。

#### 2026-11-16 完整通话/短信目标：连续三轮无进展后的阻断交接

- 三代理加速收束的上一轮产生新证据；其后**三个连续goal turns均无功能/输入进展**，每轮核实main/origin仍为`6e7b46fb`、tracked clean、已抓两个JAR路径均missing。本轮再核三个代理全部completed，gh main为`[]`，没有可认定为verified wait的活动代理/构建句柄。未重启相同任务、未重扫已排除类、未重编旧kernel，也未操作设备。
- **同一真实阻断未改变**：缺matching Android17 `CP2A.260605.016/B.46c413f-20895a6-20bdbdd`的`telephony-common.jar`与`framework-telephony.jar`实际code bytes，无法闭真实意图/profile producer；同版audio_dsp/实际scene0 task-info、通话硬件选择和独立媒体owner亦缺。已有源码仍没有完整typed caller→owned context/profile→kernel admission→terminal delegate连接，不能通过空loader、默认profile/UUID或单开09绕过这些输入。当前image/source对应的短信保存/重开/ACK和MO对端证据也缺；旧运行记录不替代。
- 已达到blocked审计阈值，目标状态由本轮goal工具更新为**blocked，而不是complete或paused**。完整要求仍为handover、CallList、生产拨号策略、通话专用双向音频、短信端到端验收及最终修复镜像；无最终镜像，无新生产修复/编译/上机。下一需要用户给已导出同版原件本地路径或相应外部输入变化，再按原完整目标续接；不要求再打10086/补来电/试刷探针。证据仍在上一条四组`x9/out/`，本轮仅此必要交接，不将状态记录提交计为功能进展。

#### 2026-11-16 三代理加速后的生产缺口收束（无新增生产修复，不能交付最终镜像）

- **基线/本轮分工**：main/origin `a2b9036c`，用户再次明确多开代理。复用三个代理及 `call-production-connect`、`dial-intent-input`、`sms-production-audit` 三树，root为 `voice-hostless-route`；仅核已抓文件与当前源码，不操作手机、不要求10086/呼入、不出探针，不运行测试/人工向量，不碰其它audio/BT/camera/HEVC树或共享`.local`。四组结论均仅追加现有call docs，**无production delta、编译、CI、boot pack或上机**；gh main查询`[]`。前一有效生产kernel仍为`e8103c88`/`x9/out/x9-call-nas-bind-owned-20261116/`，本轮不重编旧产物，不称最终镜像。
- **root实跑Hostless新owner，`af9aafe`**：原ctor GOT44d900真实relocation→keypcmHostlessSpeech，绑定this+3f8/+420；open分别flags10000000/0、mMdConfig、prepare=false。MO625..633两次Speech open及2145..2166/2256..2275 stop/close闭合basic Hostless owner，HW_SRC_1/3 controls明确mPcm==NULL。card/PCM实际由名称匹配动态descriptor、miss=-1，不是固定card0/PCM号；pcm_is_ready/open0不是DSP receipt。缺实际通话DAPM/mixer选择、同版snd_soc_mt6993_afe.ko/mt6993_mt6681.ko、audio_dsp/scene0 task-info8B、独立USIP_PHONECALL与iOS BasebandVoice I/O；不凭缺start日志断言未启动，不拿reference候选图写硬件路由。
- **真实生产加载与caller，原`e7aeaa0`整合`eb496a2`**：现可复用build.py→torch/camera编签/hashed embed→PID1 install x9_bbfw_write/equal→exec环境→constructor，但目标仅mediaserverd/AppSync；没有CommCenter runtime builder/embed/env/factory-action连接。持久原launchd job/cache不是实时加载证。`X9Calls.inc:372 x9c_dial_submit`只有定义；`X9QMIAT:780→x9c_request`只44/03/21，20unsupported。**缺的是连接代码和owned输入producer，不是下载一个库**。需要typed UUID/clientId、完整deep-owned参数、真实per-subscription broker/profile/generation→kernel admission→匹配terminal delegate；不造空loader/unused API、默认profile/UUID或只开09。未复读已闭broker asm。
- **Android首缺文件精确化，原`372b0dc`整合`4d27409`**：现有capture-manifest124条中0 APK/JAR/DEX，限定实际文件树亦无Java code。matching maps给出的首缺为`/system/framework/telephony-common.jar`、`/apex/com.android.telephonycore/javalib/framework-telephony.jar`；其它framework/oplus/mediatek扩展与`/system_ext/priv-app/ImsService/ImsService.apk`路径在docs/JSON。build明确Android17 `CP2A.260605.016/B.46c413f-20895a6-20bdbdd`；3302实际com.mediatek.ims，不是Phone/Dialer。不编造Phone/Dialer位置或DEX sidecar，不拿AOSP替matching intention→raw CLIR，不启动无输入的ASC，不开放默认0。
- **SMS当前生产/验收边界，原`dacc7c8`整合`e409547`**：当前WMS初始化→MO605/writer→606/result/MR及MT8601/3→host receipt→WMS37→物理ACK→terminal receipt均有生产入口；未找到新的独立codec/dispatch矛盾，不重复已修queue/malformed/lifecycle类。writer0仅AP CCCI ring完整commit，不是MD消费、Messages SQL或对端送达。旧319 token703/旧220 token1与Android原形状不能套当前HEAD，首缺当前image/source匹配的一条完整短信链及同token commit/重开/ACK、MO对端收到。没有新增当前运行证据，不制造ACK/ready或“补全”行为。
- **剩余工作/停止重复边界**：VOICE09仍关闭，完整goal未完成。本轮已将实跑Hostless owner、可安装source入口、缺失matching Java文件和短信版本缺口收束；现有输入没有支持新的独立生产mutation，后续不能继续用重复静态扫描/只改交接/旧kernel编译冒充功能推进。真正下一项是实现完整owned typed caller/backend连接；需要matching Java输入才可闭production profile，需要同版音频输入/实际owner才可闭媒体，当前版本权威状态才可验收。未达到连续三goal-turn阻断审计阈值，本轮不调用blocked/complete、不宣告最终通话短信镜像。
- **本轮本地材料**：四组ownout目标为主checkout `x9/out/voice-hostless-route/`、`call-production-connect/`、`dial-intent-input/`、`sms-production-audit/`，主要JSON分别`hostless-production-boundary.json`、`production-connect-source.json`、`dial-intent-input-proof.json`、`current-sms-final-evidence-boundary.json`。只读复提入口在对应JSON/现有docs。三个代理已completed停写；三次append conflict只保留各原完整追加小节，未修改行为。实际无覆盖归档、ff/push和本轮四树清理结果在收尾记录，不提前宣称成功。
- **实际整合/归档收尾**：四组out已检查目的地不存在后逐一rename迁回主checkout，无覆盖、原日志/source-head不回写。四小节及交接`8f3c97ae`已ff合main，push成功`a2b9036c..8f3c97ae`，main tracked clean且与origin相同，gh main仍`[]`。三份原追加小节逐字确认在整合docs中各保留一次，才清理三个子worktree/本地分支；原提交因append union不依赖patch-id假判。root仅留本条最后NOTES正常提交/ff/push后清理，不触其它树。此轮仍只是输入/可达性收束，没有新增生产修复或可称最终版的镜像；下一不可重复本轮审计当功能推进。

#### 2026-11-16 通话 backend ABI / Android策略 / voice owner 续接（静态新证据，未完成生产接入）

- **基线/实际分工**：从 main/origin **`9db480c4`** 建四树：root `.worktrees/call-context-backend` / `feat/call-context-backend`，复用用户授权的三个子代理及 `call-action-adapter`、`call-profile-input`、`call-voice-dsp-owner` 三树；未重复启动相同任务。只读已抓 Android MO/呼入、原 Apple/MTK 文件与固定 reference，必要结论只写本 NOTES 和 `docs/call-ios-bridge.md`，未操作手机、不要求再拨10086/补来电、不出探针、不执行测试套件/人工向量。共享 `.local` 链接、Android/分区备份、其它 audio/BT/camera/HEVC 工作树不动。
- **root broker 新纠正，已提交 `584fdc9`**：旧生成契约的 inner `+18/+20 callback fields` 有误。原 `State::send19ecd2c7c` 的 **inner+18是同步写u16 txn的borrowed输出地址，+20才是copied response block**；async40B capture保owned XPC/msg/block及txn值，不保原输出指针。原raw `QMIServiceMsg::createRequest19ecff82c` 的输入是4B id/TLV-size header+TLVs，不是7B QMI SDU/QMUX。response capture拥有msg/block/weakpair及State+81配置bit快照；flag0仍invoke，flag1需weak lock成功且identity非空，setter/stop不更新旧capture。State+10/+78是不同配置槽，不能推断运行时queue必不同或形成fence；同步txn分配也不是MD接受/终态。peer独立原asm复核无阻断问题。22处本轮范围匹配当前root Cryptex，另四支岛实际解至Block copy/release及libc++ weak lock/release；是原件来源资格，不是构建产物检查或运行成功。
- **typed action/owning ABI，原 `55e4059e` 整合 `207d9c79`**：真address point **+10=ring、+80=dial五参**；16槽全部参数/深owner/AP隔离已列齐。CallConfig完整copy **`100dd7560`** 与whole dtor **`1007cd508`** 由同destination正常/异常cleanup闭合，extent0xf8；排除附近大package/局部析构，不raw memcpy。IMSCallOptions0x13a0 owning copy/dtor为私有local而非可链接export；LocationInfo48B只是header，后四strings原本deepcopy。不能调用原dial/pull仅“生成options”，它们会排入AP；text/relay同样有AP work、CLIR才field-only。原件8处新范围相符；无新增空loader、unused API或未连接production adapter。
- **通话DSP owner，原 `41093f0` 整合 `cb768950`**：原HAL两个calldl/callul sharemem controls锚至qualified MT6993 reference动态MEMDL/MEMUL/MEMREF→独立ADSP AFE pool→task-owned physical PCM descriptor；`use_adsp_share_mem`真分支禁主机memif/IRQ trigger，owner归DSP。reference外/内层忽略部分返回，setter/hwparams不能授予DSP receipt。**真实本次MO走Hostless_Speech双open**；原frame-base分支会绕过它，不能把另一分支符号存在当本次执行。/dev/usip30000是AP参数段、当前MO参数size/index0，不证明PHONECALL8000/PCM消费。首缺同版`audio_dsp`固件，实机task-info8B、独立PHONECALL owner/consumer和iOS实际I/O链仍缺；scp_b/SmartPA/通用AFE不替代。只改必要docs。
- **Android策略，原 `b36eb0a4` 整合 `2edad5b`**：原MCL1是CARD_TYPE、初始int=-1，MD4512/tag100→SIM1/USIM2、tag101→RUIM8/CSIM4（tag101==3→0xc）；voiceGsmRat的&0xc不是NR capability，tag102附加0x10不猜名称。原GET_CURRENT_CALLS实际AP ID c938，response→pendingAP owner/token→AP data clone→72 reset→type6 owned variant/slot/key→MCL setValueByRfx(w5=1防反馈)→owned storage已补；AP同值notify=false也仍同步MCL。34组fixed原件切片及hash/编号asm保留，空error/缺tag不得照抄成桥的成功idle。尚缺matching设备CLIR intention producer/framework输入，不拿raw CLIR1/2或AOSP参考冒充完整跨enum生产准入；无profile默认0或生产行为修改。
- **真实构建/目标边界**：本轮只有静态契约/docs提交，**没有生产源码修改、编译、CI run、boot pack、镜像复检或上机**；gh本分支查询`[]`。上一轮有效编译仍固定生产源码 **`e8103c88`**，`x9/out/x9-call-nas-bind-owned-20261116/kernel.development.sweet`、98/98 overlay/link/strip exit0，未为本轮静态材料重编旧产物。它仅是NAS45前置修复kernel，不是完整通话/短信镜像；更早main整包/source a6854c83亦不含本轮或最新生产修复，不替换其source-head。
- **完整目标仍active、下一真正缺边**：VOICE09未公告、VOICE20仍无生产admission/caller。需把完整typed actions、实际加载/install、原broker logicalclient/context、Android完整policy、UUID/clientId↔SIM/native epoch/ID和correlated receipt→typed terminal delegate连成可达生产flow；同步txn/注册Result不是通话成功。handover后的ID/RAT/CS动作、专用双向voice及短信Messages当前commit/重开/物理ACK/MO对端收到仍需端到端验收，**没有最终修好镜像**。新证据不是完成验收，不能靠另编一个09关闭的镜像冒充最终交付。四组out已实际无覆盖归档主目录 `x9/out/call-context-backend/`、`call-action-adapter/`、`call-profile-input/`、`call-voice-dsp-owner/`，原记录/脚本/source-head不改写；三个代理都completed停写。actual docs整合完整保留四小节，append conflict仅union两独立小节，无行为修改；合main/push和本轮四树清理结果在收尾条记录，不重复已闭切片或重跑旧编译。
- **实际整合/push/清理**：四份契约及交接 **`96c0595d`** 已ff合main并push成功 **`9db480c4..96c0595d`**；gh main查询仍`[]`。diff只有NOTES及现有call docs（338行新证据），没有production delta。三个子树out都已实际在主目录，相同name目的地先检查不存在再rename，无覆盖；子树/本地分支已清理，未推这些临时分支。action/voice `git cherry main`为`-`；profile因append union显示`+`，另外直接比较其完整新小节与main末小节**逐字一致**后才删除分支，不把patch-id差异当缺失。root功能树/分支仅留本条最后NOTES的正常提交/ff/push，成功后移除；其它树/长期输入/原件/旧镜像完全不动。全套原件重读入口在各JSON及root `replay-commands.txt`，不重复运行已收束路径；完整goal仍active，当前不能将这次静态收束称为通话/短信修复完成。

#### 2026-11-16 通话生产接入 / NAS45初始化续修（生产窄修已编译，完整目标未完成）

- **基线/分工**：从 main/origin `25abe26e` 建 root `feat/calllist-production` / `.worktrees/calllist-production`；用户要求多子代理，复用三个实际代理及 `call-provider-install`、`call-privacy-policy`、`call-client-bootstrap` 三树，分别追原 factory/action、Android拨号策略、Apple初始化/注册。只读既有日志/原件，修改生产源码及必要契约；不操作手机、不要求10086/补来电、不出探针、不执行人工向量/测试套件，不触其它audio/BT/camera/HEVC活动树。
- **已提交的状态发布证据**：root `f8b6212` 证明 TRYING `1007c55f0` 只按UUID lookup并日志 `Ignoring`，不发布状态。原RINGING `1007c57d0→command+30→10104879c→Call+338` 实际送 **CallAlerting**；BSS表 `101673178` 由原 `10105e078` 初始化，只有前态FSM Dialing4的handler `10104ec28` 准入，经strong status+28、原UUID filter和 `101018718` 写 **Call+a0 Alerting7**，随后FSM置Alerting6。payload byte5、FSM6、CT7、QMI event不能互抄；仍无MD producer/完整UI验收。静态材料已实际无覆盖归档主checkout `x9/out/calllist-production/`。
- **已整合的factory契约**：代理原 `6e6500a` cherry-pick为root **`b2ba58e0`**，仅docs。实际factory表 `101532938+40` 消费已证，不替整个driver；每个action可重建临时Call，D1/D0不hangup/END，不能用对象释放退休MD epoch。text/relay setter另有异步AP发送，不能passthrough；shared embedded Call必须nondeleting destructor。续查原初始化仅在RadioPersonality+204==2进入factory创建，此值不是SIM2，factory无MD/broker context。现有original broker→ABP selector2→QMUX/CBQ→x9c_request可复用，但VOICE09无server导致CTL分配cid拒绝、20无admission、无UUID/epoch scoped事件receipt→typed delegate backend，不能只公告09或安装+40称完整接入。完整raw/JSON已无覆盖归档主checkout `x9/out/call-provider-install/`。
- **唯一生产窄修已完成，设备尚未验收**：代理原 **`e7452336`** 整合为root **`e8103c88`**，三份源码及独立初始化docs。NAS45/tag1/u8实际由 `QMINasClient` event2发送；只有其真实Result0按owned name/subscription找delegate→`nasSystemInitialized`，才继续owned DeferredInit/StackConfig→`_initializeFromSIM`→Bambi/SipStack。旧 `X9QMIAT` 先改SIM route再legacy发送，queue/OOM丢reply会漏初始化推进。现owned45 terminal Result只有实际queue receipt后换route，保旧jobs原token/SIM，pending换route阻断新NAS44/03/45可能失活的owner；25s本地guard从admission计时、在bb锁下实际frame transfer前复核，迟到success不改route；success cache精确replay仅回复，NAS03无新增timer，DMS/DSD原路径不变。不重发MD、不造registered/ready。19处原件code/table/owner窄切片与当前Cryptex cache相符，只是来源资格不是运行成功。代理正式单对象recipe exit0/34 warnings，完整证据/源码编译日志已无覆盖归档主checkout `x9/out/call-client-bootstrap/`。
- **privacy/Android策略证据已收束**：代理 `1750d1a5` 整合为root **`546bd320`**，仅docs。Apple两privacy byte→真实header/anonymous From/owned stack配置及CLIR policy已闭；Android普通non-ECC十进制raw1/2原值保留、prefix原literal已证，仍不把未闭合输入意图producer当完整跨enum准入。TLV104真实MCL1/31/72谓词及voiceRAT producer不是kernel NAS serving/data或IMSA注册快照；pref72==0数学子集需真实reset/owned来源，不能默认0。30处原件切片已取证，完整材料含原大asm保留在主checkout `x9/out/call-privacy-policy/`，未加encoder白名单或unused API。
- **正式固定源码编译**：HEAD **`e8103c88`** 的 `python3 -I x9/build/build.py kernel --name x9-call-nas-bind-owned-20261116` **exit0，98/98 overlay（3 prebuilt）、link/strip成功**；仅kernel，不pack boot、不上机、不执行测试/人工向量或构建后二次检查。产物已实际迁回主checkout **`x9/out/x9-call-nas-bind-owned-20261116/kernel.development.sweet`**；HEAD/console/exit在 **`x9/out/call-nas-bind-integrated-compile/`**，原日志绝对路径不回写。主checkout `prepare-local.py --check`通过；worktree运行同检查因共享 `.local` 的maintained-source link合法指向main而失败，未重指/改shared symlink，kernel-only不重建插件。gh该feature查询`[]`，无CI run ID。
- **完整目标仍未完成**：VOICE09保持关闭；typed→MD可达provider/生产拨号/UUID与native epoch映射、handover后ID/RAT/CS动作、双向通话专用媒体及短信Messages当前commit/重开/物理ACK/MO对端收到仍未闭合，**没有通话短信最终修复镜像**。下一从完整原broker logical-client/策略及原factory action协同补真实caller/backend/事件receipt，不重复以初始化45成功、TRYING或source编译绿色代替最终状态。所有本轮out已无覆盖归档，三个子代理完成停写；收尾合main/push、只清本轮四树，实际push/清理结果另补。
- **实际合入/清理**：生产 `e8103c88` 和契约/编译交接 **`0f89df93`** 已ff合main，`git push origin main`成功 **`25abe26e..0f89df93`**；push后gh main查询仍`[]`。四组静态/单对象证据、集成compile、kernel目录共六组已经实际在主checkout相同 `x9/out/` 路径，无覆盖、未重写原日志。三个原提交 `git cherry main` 均为已整合的 `-` 后，已清理本轮四个worktree和三子分支；root功能分支留作本条最后NOTES提交并正常ff/push后删除。其它audio/BT/camera/HEVC树、长期输入、旧镜像、分区备份完全保留；未在main重编、pack或二次检查产物。完整goal仍active，当前产物只是NAS45前置修复kernel，不要求用户试刷。

### 5.1 已在设备上观察到能用（详情见旧 NOTES）

- 启动到 SpringBoard，UFS 原生根盘读写，UFS HS-G5/FAST/RateB/2 lane（180）。
- Mali GPU：Metal 经 PanVK 插件渲染（仍有少量三角形闪烁/竖缝等渲染错误，未修完）。
- Wi-Fi（MT6653，WPA2）联网；蜂窝 4G 数据可上网（241），5G SA 能注册但存在回落/双卡问题；原生短信部分实现。
- 未完成：相机、麦克风/扬声器（音频 loader 已移除）、振动（190 EBUSY）、GPS、HEIF 硬解、eSIM（设备无内置 eUICC）、
  **USB 配对 / AppSync / 越狱**。

### 5.2 2026-11-12 仓库整理（本次）

- 合并：`feat/usb-iosabi` → main（`a0a72be`）：SweetCPU / IOKitBSDInit 从源码编（与原 prebuilt 反汇编/符号一致）、
  kalloc.c + XnuPrelink sparse-index 导出、`X9NativeUsbProbe`（仅当 IOUSBDeviceFamily 已 prelink 时加载三件 USB kext 并打印类注册）。
- XNU 源码入库为 `x9/xnu/` 补丁 + `build-xnu.sh`，删除旧的 269 个只作记录的 `x9/xnu-patches/`。
- 删除：129 个本地分支（逐一核对其文件内容已在 main）、7 个 worktree、`.local/x9/work` 下约 390 个实验目录、`.local/analysis-*`、
  `.local/x9/logs`、android-* 抓取、旧参考快照、`x9/out` 只留 `x9-gpu280-purge-20261112`（最后一个完整 GPU 镜像 + 280 插件包）。
  删除前把散落的分区/固件原件全部移到 `.local/x9/backups/partitions/`。
- 外部依赖收进仓库：20H392 dmg（APFS clone）→ `.local/apple/`；相机 vendor 源码 → `.local/vendor/`；构建不再读 `~/HelioDarwin`。
  （`~/HelioDarwin` 本身未删，Mac 上 20H392 dmg 目前仍从那里挂载在 `/private/tmp/{os,sysos}20H392`。）
- 新增 `x9/build/boot-device.json`（从 gpu280 镜像 payload 逐字恢复的完整 43 参数）、`x9/build/scripts/build-usb-prelink.py`。

### 5.3 USB / AppSync / 越狱 当前交接

- **方向（NATIVE18 更正）**：放弃 NATIVE1–17 的逐调用 vtable 重映射路线。根因：X9 XNU 由 dxb 以 `-DXNU_TARGET_OS_OSX` 构建，
  `OSMetaClass.h` 因此开启 `APPLE_KEXT_VTABLE_PADDING=1` / `ALIGN_CONTAINERS=1`，而 iOS kext 是 0/0。去掉 `_RESERVED` 槽后父类
  vtable 与原件逐槽一致（OSObject 15、IOService 168、IOUserClient 186、IOCommand 15、IOCommandPool 21、IODMACommand 31、
  IOEventSource 27），实例大小相同。所以用 iOS ABI 的 XNU（0002）+ XnuPrelink 直接链接**未修改的 20H392 USB kext**，
  与 UniXNU QcomDwc3 的做法一致（Apple IOUSBDeviceFamily / AppleUSBDeviceMux / configd / lockdownd 原样运行）。
- **已完成（观察）**：`dxb-iosabi` 构建通过；`build-usb-prelink.py x9-usb-prelink2-20261112` 三步全部 exit 0（分支
  `feat/usb-prelink-shim`）。之前 shim 报 `bad segment address or size` 的唯一原因是 `__PRELINK_INFO` va `0xfffffe000a20d000`
  只有 4K 对齐；挪到 `0xfffffe000a210000` 后通过校验。镜像 **`x9/out/x9-usb-prelink2-20261112/boot.img`**（Image 68096000 B，
  boot 14176256 B；内核跨度 0xfffffe0007004000..0xfffffe000d1f4000，prelink 段为保持统一 slide 保留原 coalesced 尺寸，主要是零）。
- **静态核对（推断，未上机）**：XNU 8796 `arm_vm_init` 支持新式 `__PLK_*` 段；prelink 位于 xnu `__TEXT` 之上时 `segLOWEST` 仍是
  `__TEXT`，`end_kern`=最高段末尾=payload `kernel_end`；`__PRELINK_TEXT` 0x7c4000 < 0x3000000 断言。segmented8g 第一段连续
  RAM 0x100000000..0x13CC00000 足够容纳内核+ADT+boot_args。Image 68 MB < `--max-image-size 0x7b80000`；LK 实际装载地址未知，
  若与内核目标重叠 shim 会在复制前停下并打印 `payload or shim overlaps kernel destination`（不会写坏内存）。
  XnuPrelink 普通路径只需 sparse-index 补丁（已在 overlay kalloc.c）。
- **prelink2 上机（2026-11-12，用户）**：panic `assertion failed` @ `machine_routines.c:1797`，PC `0xfffffe00086adeb0`（用户读屏）。
  该行是 `ml_static_mfree` 的 `assert((vaddr & (PAGE_SIZE - 1)) == 0)`。之后 Android 下拉取：pstore 只有 `pmsg-ramoops-0`
  （shim 文本 + `x9m` 里程碑，多数 CRC 坏），UFS console sidecar 是 12:01 那次（插件 278）的旧日志——这次在挂根盘前就 panic，
  没写 console。原件在 `x9/out/x9-usb-prelink2-20261112/{android-pstore,disk-log}/`（gitignored）。
- **原因（推断，无 backtrace）**：20H392 kernelcache 的组件是**紧凑排列**的：6 个 KPI 接口 header 在 `0xfffffe000a400000` 起每
  0x80 B 一个，三件 USB kext 的 `__TEXT`/`__DATA`/`__DATA_CONST` 也都不是 16K 对齐（`/tmp/kextalign.py` 式解析 PRELINK_INFO 得到）。
  XNU 对每个 prelinked kext 的可执行 OSData 设 `osdata_phys_free`，释放（如 init 失败的 OSKext::free、linkedit jettison、
  DATA padding）就会对这些地址调用 `ml_static_mfree` → 断言；对齐的那个（kpi.bsd）还会把与邻居共享的页还给 VM。
  XNU 对 fileset KC 已因同样原因不释放组件内存。
- **修复**：`x9/xnu/0003-x9-sparse-prelink-kext-memory.patch`（只进 ios 树）：`OSKext.cpp` 新增 `x9_sparse_prelink_memory()`
  （存在 `__PRELINK_TEXT,__macho_list` 且地址落在 coalesced prelink 段内），`osdata_phys_free`、reloc 裁剪、linkedit jettison、
  DATA padding 四处遇到时不释放。`build-xnu.sh ios` 重建通过（OSKext.cpo 19:18、kernel 19:19），树 == tag+0001+0002+0003。
- **新镜像** **`x9/out/x9-usb-prelink3-20261112/boot.img`**（三步 exit 0，内核含 `x9_sparse_prelink_memory`）。未上机。
- **prelink3 上机（2026-11-12，用户）**：仍 panic。这次 Android 有 `console-ramoops-0`（存
  `x9/out/x9-usb-prelink3-20261112/android-pstore/`）：XNU 正常到 `X9 BSD 91: bsd_autoconf_ready`，1.232 s 在 **cpu 2**
  `machine_routines.c:1797 Assertion failed: (vaddr & (PAGE_SIZE - 1)) == 0`，**没有 backtrace**（之后只有
  `handshake structure not initialized`）。0.115 s 有 `X9 NATIVE USB: prelinked IOUSBDeviceFamily present; probe scheduled`，
  但 probe 线程自己的日志没出现。推断：IOBSD 发布后 probe 线程开始 load kext，那条路径上有未被 0003 覆盖的 unaligned free；
  已排除 kdebug（arm 上不 free）、libsa bootstrap（早期，且 OSX 宏下不 free PRELINK_INFO）。
- **诊断镜像**：`x9/xnu/0004-x9-diag-unaligned-static-mfree.patch`（ios 树，**诊断用，定位后应换成真正修复并删除**）：
  `ml_static_mfree` 遇未对齐地址改为 `OSReportWithBacktrace("X9 MFREE: ... lr=...; skipped")` 并跳过（泄漏，不错放），继续启动。
  **`x9/out/x9-usb-prelink4-20261112/boot.img`**（三步 exit 0，含 `X9 MFREE` 字符串）。上机后看 console 的 `X9 MFREE` 行的
  backtrace 与后续 `X9 NATIVE USB:` 行。
- **prelink4 上机（2026-11-12，用户拍屏）**：没有再出现对齐断言（0004 生效）。在 `X9 BSD 90: bsd_autoconf_enter` 后，cpu 2 上
  `Kernel data abort`，pc 0xfffffe0008450a68 = `OSKext::loadExecutable+0x118`（`str w9,[x10,#0xc]`，x10 = `this->kmod_info`），
  esr 0x96000045（写，translation fault），far 0xc。`kmod_info` 为 NULL，`isInterface()` 返回 false，所以是一个普通 prelinked kext。
  三个 USB kext 的 `_PrelinkKmodInfo` 都不为 0；kpi.* 是 0，但带 OSKernelResource，属于 interface。
  推断（未证实）：某个 kext 第一次 `loadExecutable` 失败，kc_type Unknown 分支把 `kmod_info` 置 NULL；或 removeKext/unload 后
  （非 fileset）置 NULL。之后依赖它的 kext 再加载它就写 NULL。第一次失败的原因被 panic 盖掉了，屏幕上看不到。
  pstore 大概率读不到，以后以用户拍屏为准，不要擅自重启手机去拉日志。
- **诊断 0005** `x9/xnu/0005-x9-diag-prelinked-kext-load.patch`（ios 树，诊断用）：prelinked 且 `kmod_info` 为 NULL 的
  非 interface kext，打印 `X9 KEXT: ... kmod_info is NULL` 后拒绝加载，不再 panic；`loadExecutable`、`load`、`start`
  失败时各打印一行 `X9 KEXT:`，会显示在屏幕上。镜像 **`x9/out/x9-usb-prelink5-20261112/boot.img`**（三步 exit 0）。
  下一步：用户拍下屏幕上的 `X9 KEXT:` 和 `X9 NATIVE USB:` 行。
- **prelink5 上机（2026-11-12，用户）**：**成功进系统**（能截图）。USB 日志通道（Mac 上 1209:0001 "X9 USB log"，
  `x9/tools/usb-live/usb_log.py`，只读）存到 `x9/out/x9-usb-prelink5-20261112/usb-log.txt`。启动日志里：
  `Kext com.apple.driver.usb.AppleUSBCommon module start pointer is outside of kext range (start 0xfffffe000b006ce0 -
  kext starts at 0xfffffe000a4f1dc0)` → start 失败 → removeKext 卸载失败但 kmod_info 已为 NULL → 0005 拒绝重载 →
  IOUSBDeviceFamily/AppleUSBDeviceMux 依赖失败，5 个类都 missing。（这就是 prelink4 那次 NULL kmod_info panic 的第一次失败。）
- **根因（已确认）**：start 0xb006ce0 本来就在该 kext 的 `__TEXT_EXEC` 0xb004000+0x3050 里。但非 fileset 内核的 `arm_vm_init`
  设 `vm_kernel_slid_top = vm_prelink_einfo`；我们之前把 `__PRELINK_INFO` 挪到 0xa210000，比所有 kext 段都低，导致
  `slidePrelinkedExecutable` 里 `ml_static_slide()` 对 kext 段返回 0，段 vmaddr 全变 0，范围检查就判越界。
  **修复**：`build-usb-prelink.py` 把 `__PRELINK_INFO` 放到最高段之后（16K 对齐，与 Apple legacy KC 一致）→ 0xfffffe000d1f4000。
  镜像 **`x9/out/x9-usb-prelink6-20261112/boot.img`**（三步 exit 0；Image 68096000 B）。未上机。
  预期：三个 kext 都能 load、类注册。但 Mac 仍看不到 iPhone，因为还没有 MTU3 controller（`X9Mtu3Xdci`），USB 口仍是 X9 USB log 设备。
  AppSync runtime 也还没 staging/部署。
- **用户反馈（prelink5）**：相册、设置的存储空间页面打不开。日志符号化（`x9/tools/dsc_symbolicate.py`，slide 0x1f6e4000）：
  MobileSlideShow 151.6 s 在主 runloop 遇到未捕获的 NSException（`objc_exception_rethrow` ← `CFRunLoopRunSpecific`）→ abort；
  Preferences 233 s 被 0x8badf00d 杀掉，卡在 `AirTrafficDevice -[ATClientController _loadClientsForDataclasses:]` 的 mach_msg 上
  （在等某个服务回复）；275 s 在 `SafariCore safari_mobileSafariContainerDirectoryURL` 的 dispatch_once 里抛异常 abort。
  另外：AegirPoster 47 次、syncdefaultsd 18 次、Safari 4 次、mediaserverd 2 次 abort。异常文本只在 iOS os_log 或
  CrashReporter .ips 里，需要 Android 下用 `pull_ios_logs.py` 读（等用户同意再做）。
- **prelink6 上机（2026-11-12，用户）**：进系统。USB 日志（`x9/out/x9-usb-prelink6-20261112/usb-log.txt`）：
  `load AppleUSBCommon / IOUSBDeviceFamily / AppleUSBDeviceMux -> 0x0`（三个都加载成功，没有 `X9 KEXT` 失败行）；
  类 `IOUSBDeviceController` registered size=504 super=IOService，`IOUSBDeviceInterface` 304/IOService，
  `IOUSBDeviceInterfaceUserClient` 440/IOUserClient，`AppleUSBDeviceMux` 464/IOService。`IOUSBDevice` 显示 missing：
  kext 里有这个字符串，但 20H392 可能没有叫这个名字的类（推断，探测列表是我写的猜测）。**原版 Apple USB 设备栈已经在 X9
  内核里活着。** Mac 侧仍只有 "X9 USB log"（还没有 controller）。CRASH 计数：AegirPoster 14、syncdefaultsd 10、
  driverkitd 1、MobilePhone 1。
- **X9Mtu3Xdci 开工（2026-11-12，分支 `feat/usb-mtu3-xdci`，worktree `.worktrees/usb-mtu3-xdci`）**：用户要求保留日志接口做组合设备。
  设计和进度在 `x9/usb/xdc/README.md`。已确认：UniXNU QcomDwc3 继承的是 `AppleUSBXDCI`（DWC3 IP），MTU3 不能这么用；
  模板改为 KDK 13.5 里带符号的 DWC2 驱动 `AppleSynopsysOTGDevice`，它直接继承 `IOUSBDeviceController`。
  `gen_usbdc_abi.py` 校验通过：KDK 的 vtable 去掉 `_RESERVED` 后是 232 槽，与 20H392 vtable（kc 0xfffffff006e11200，
  prelink 后 0xfffffe000c449200）槽数和 18 个纯虚位置完全一致。下一步：在 20H392 里定位非虚 API、ctor/dtor/gMetaClass；
  从 DWC2 反汇编确定返回类型、请求 tag 布局和 EP0 流程；生成 ABI 头文件和绑定；写驱动（MTU3/QMU 复用
  `x9_usb_device.c`，日志走 EP0 vendor 0x46/0x47）。
- **X9Mtu3Xdci 暂停（2026-11-12，用户：先不修 USB，转去修越狱环境，在手机上直接装 app）**。停在：`x9/usb/xdc/match_family.py`
  （KDK 符号 ↔ 20H392 IOUSBDeviceFamily 函数匹配：vtable 种子 + 调用序列对齐，capstone 反汇编 KC）匹配到 507 个、0 冲突；
  已得 `IOUSBDeviceController::abortEndpoint` = kc 0xfffffff005cf0770，`IOUSBDevicePipe::getDirection` = kc 0xfffffff005ce69cc。
  `inputSetupPacket/inputDataPacket/processSetupPacket/completeEndpointIO/readyForHost` 在 family 内部没有调用者，需要换方法
  （os_log 格式串引用、函数形状）；56 个 vtable 只有 7 个按长度唯一配上，可按槽的本地/外部模式继续配。
- **下一步（USB）**：写 `X9Mtu3Xdci : IOUSBDeviceController`，接 MTU3 device 控制器（参照 UniXNU QcomDwc3 的 controller
  子类 + linux mtu3 gadget + 现有 `x9_usb_device.c` 里已验证的 MTU3/QMU 代码），替换掉 X9 USB log gadget；之后 configd/lockdownd
  应自己配置 USB 设备 → Mac 识别 → 配对。0004/0005 诊断补丁在 controller 跑通后再删或换成正式修复。
- **上机步骤**：main/本分支 allowlist 是 280，设备上是 277，必须先（Android、用户给序列号）
  `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-gpu280-purge-20261112`，再由用户
  `fastboot boot x9/out/x9-usb-prelink6-20261112/boot.img`。
- **要看的**：shim 日志到 `X9: entering XNU`；XNU 早期是否在 `arm_vm_init`/`OSKext::readPrelinkedExtensions` panic；
  `X9 NATIVE USB:` 行里 AppleUSBCommon / IOUSBDeviceFamily / AppleUSBDeviceMux 的 load 结果与 IOUSBDeviceController、IOUSBDevice、
  IOUSBDeviceInterface(UserClient)、AppleUSBDeviceMux 的类注册/大小。若黑屏无日志先断电重启再判定（见记忆：黑屏先 power cycle）。
- **下一步**：上机结果后：写 `X9Mtu3Xdci : IOUSBDeviceController`（参照 UniXNU QcomDwc3 / linux-mtu3 / oppo-mt6993-usb）接 MTU3；
  Mac 识别 → Trust/配对/lockdown → `x9/usb/install.py` 装 IPA；AppSync runtime 经 mailbox 安装器部署；Procursus rootless 到 `/var/jb`，
  SSH 仅 ue0。每个设备写入步骤先征得用户同意。
- AppSync 已有：runtime v3（AppSync/ldid/appinst/jbctl 4 个签名 arm64 二进制）、内核 `x9_appsync_env.inc` 按哈希只对
  installd/SpringBoard/MI helper 注入 `DYLD_INSERT_LIBRARIES`、主机端 `x9/usb/install.py`。未 staging、未实机。

### 5.3a 手机上直接装 app（2026-11-12 起，分支 `feat/jb-installer`，worktree `.worktrees/jb-installer`）

- 用户决定：USB 暂停，先做"最小安装器"（自己写的 X9 安装器 app + AppSync），完整 Procursus/Sileo 以后再做。
- **内核**：`x9_appsync_enabled()`：编进了 runtime 清单（`X9_APPSYNC_RUNTIME_READY`）就默认开启，`appsync=0` 关闭
  （boot-args 已到 607B 上限）。注入和"Data 用户 app 不升 platform"共用这一个开关；runtime 文件哈希不符就不注入。
- **用户态**（`x9/appsync/`）：`installer/main.m` = `/Applications/X9Installer.app`（org.x9.installer，非容器化系统 app；
  接收「共享/打开方式」送来的 .ipa、+ 选文件，调用 `LSApplicationWorkspace installApplication`）；`jbctl.m` 新增
  `--register`（TrollStore uicache 式 `registerApplicationDictionary`，只接受 `/Applications/<Name>.app` 且 id 为 `org.x9.*`，
  entitlements 读 `share/<Name>.entitlements`）；`vendor/AppSync.m` 在 SpringBoard 里约 8 s 后若未注册就 spawn
  `x9-jbctl --register`。`build.py` 打包 app（图标、Info.plist、签名）并把 root 下全部文件写进 runtime.json。
  runtime v4：`.local/x9/appsync/runtime-20261112-v4`（3.4 MB）。
- **投递**：`package-plugins.py --base-package ../../x9/out/x9-gpu280-purge-20261112/plugin-package --extra-root
  .local/x9/appsync/runtime-20261112-v4/root --out x9/out/x9-jb-installer1-20261112` → 26 个文件、payload 56893440 B
  （上限 65011712）。原样复用设备上已装的 280（identity 8d414194… 与主 checkout 白名单一致）。`x9_update_dirs` 改为列出全部
  祖先目录。确认单卷：启动日志 `mount: data volume missing, but not required`，`/private/var` 就在根 HFS 上。
- **镜像**：`x9/out/x9-jbi1-20261112/boot.img`（build-usb-prelink.py，与 prelink6 同一流程，含 0004/0005 诊断补丁和
  USB kext；local.json 指向 v4）。
- **部署步骤（需用户同意，写设备）**：Android 下 `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-jb-installer1-20261112`
  （在 worktree 里运行），再 `fastboot boot x9/out/x9-jbi1-20261112/boot.img`。
- **风险**：新白名单把越狱文件也算进插件校验；开机安装失败（如目标存在且内容不同）→ Metal 禁用 → 黑屏/卡 logo，
  这时 boot prelink6 镜像恢复（但 mailbox 已是新 identity，prelink6 的白名单仍是旧的 280，插件已装好不需要 mailbox）。
- **上机要看的**（USB 日志 `usb_log.py`）：`X9 UPDATE: payload installed and verified`、`X9 APPSYNC: verified runtime
  environment appended`（installd/SpringBoard）、`[UniXNU AppSync] ... adapter active`、`X9Installer registration spawn=0 exit=0`
  或 `already registered`；主屏出现「X9 安装器」；用它装一个 IPA。os_log 里的 NSLog 行可能不在内核日志里。

### 5.3b 2026-11-12 相册崩溃 / 设置「储存空间」打不开（prelink6 启动，Android 只读诊断）
- 数据：CrashReporter → `x9/out/x9-usb-prelink6-20261112/ios-crash/`；统一日志最近 1 h → `.../ios-logd/`
  （`tracev3_strings.py` → `strings.txt`）。参考仓库浅克隆在 `x9/out/refs/`（gitignored）。
- **储存空间（观察）**：Preferences-202131 是 0x8BADF00D，主线程卡在 AirFair mach_msg（`ATClientController
  _loadClientsForDataclasses:` → `FPAirFair init`）；Preferences-203950 卡在 `lockdown_set_value`。日志里
  `user/501/com.apple.fairplayd.H2` 每隔几秒换一个 PID（372…847，`service inactive` ×216）＝ KeepAlive 循环，没有崩溃报告。
- **参考（观察）**：Droid-VM/UniXNU-droidvm `package/DroidVMBootArgs.c`：fairplayd 缺 FairPlayIOKit 就立即退出，但
  MachServices 仍注册，lockdownd 第一次 FairPlay 调用永远等待，accountsd 和问它的 app 跟着卡（Photos 空白）；他们
  `droidvm-disable-jobs=...fairplayd.A2...`。UniXNU-sheng `BootWatchdogPolicy.c` 同样禁 fairplayd.A2；hackios
  `userland/mkjobs.py`：fairplayd 要 `com_apple_driver_FairPlayIOKitUserClient`。
- **候选修复（未上机）**：boot-arg `x9_disable_jobs=com.apple.fairplayd.H2`（kern_exec.c 已有机制）。待验证：该机制只写
  系统域 `disabled.plist`，fairplayd.H2 在 `user/501` 域，可能还需要 `disabled.501.plist`（猜测）。
- **相册（观察）**：MobileSlideShow 是 PhotosUICore 的 NSAssert（`PXGCreateContext` 行 290，尺寸 ≥1；符号化成
  initWithContentsOfFile 是错的，返回地址在 handleFailureInFunction 后）。`-[PXGMetalTextureConverter
  _createTransparentTexture]` 用 `PXGCreateCGImageForTextureWithDrawBlock(0, 12, 1×1)`；色彩空间 12 = `kCGColorSpaceSRGB`，
  8bpc、`kCGBitmapByteOrder32Big|PremultipliedLast`，最普通的格式。`CGBitmapContextCreate` 返回 NULL → 图片为 NULL → 宽 0 → 断言。
  日志里 `CGBitmapContextCreateImage: invalid context 0x0` ×167、`CGContextGetCTM: invalid context` ×440，说明全系统都有
  bitmap context 创建失败，不止相册。ColorSync ICC 文件都在（decmpfs 压缩，不是空文件）。原因未知；参考仓库没有对应记录。
  下一步：设备上用 `CG_CONTEXT_SHOW_BACKTRACE=1` 或最小探针测 `CGBitmapContextCreate(NULL,1,1,8,4,sRGB,0x4001)`。
- Safari 设置页另一个崩溃（Preferences-202213）：`_safari_getMobileSafariContainerDirectoryURL` 拿到 nil 路径 → `fileURLWithPath:nil`。
- **2026-11-12 21:40 已做（分支 `fix/disable-fairplayd`，含 feat/jb-installer，提交 09472a4）**：内核默认把
  `com.apple.fairplayd.H2` 写进 `disabled.plist` 和 `disabled.501.plist`（原件各自备份为 `disabled.x9-orig.plist` /
  `disabled.501.x9-orig.plist`；`x9_fairplay=1` 保留该任务；boot-args 已满所以不走 boot-arg）。镜像
  `.worktrees/disable-fairplayd/x9/out/x9-jbi2-fp-20261112/boot.img`（14180352 B，白名单与 jbi1 相同）。用户同意后已
  staging mailbox `x9-jb-installer1-20261112`（56893440 B，逐段回读通过）。未上机。要看：USB 日志 `X9 JOBS: ... disabled.501.plist
  disable=1`、`X9 UPDATE`/`X9 APPSYNC` 行；统一日志里 fairplayd.H2 PID 不再变；设置 › 储存空间能打开；X9 安装器装 IPA。

### 5.4 其它未上机的候选

- **jbi2 上机（观察，USB 日志 `x9/out/x9-jbi2-fp-20261112/usb-log.txt`）**：mailbox 安装成功（`X9 UPDATE ... errno=0`），
  `X9 JOBS` 两个数据库都写入且回读一致；但 `X9 APPSYNC: runtime missing/changed; SpringBoard left unmodified`，主屏没有安装器。
  原因：`x9_bbfw_equal` 要求 mode 正好 0644，mailbox 按 0755 装 dylib（哈希一致）。修复：`x9_appsync_file_equal`
  （只拒绝组/其他可写，仍查 root:wheel、大小、SHA256，日志带路径/errno）。镜像 `x9/out/x9-jbi3-20261112/boot.img`，
  mailbox 不需要重写。未上机。
- **jbi3 上机（观察，`x9/out/x9-jbi3-20261112/usb-log.txt`）**：`X9 APPSYNC: verified runtime environment appended` 到 SpringBoard，
  `X9Installer registration spawn=0 exit=0`，主屏出现安装器。Safari 下载提示 "storage not available"；同时 `CRASH com.apple.Safari`
  ×9，符号化为 `_safari_getMobileSafariContainerDirectoryURL`（和 Settings Safari 页同一处）。反汇编：
  `LSApplicationRecord initWithBundleIdentifier:"com.apple.mobilesafari"` → `dataContainerURL` 为 nil → 退路字符串也是 nil
  → `fileURLWithPath:nil` 抛异常。即 LaunchServices 里 MobileSafari 没有数据容器（猜测：容器缺失或 LS 记录没有容器路径；
  需在 Android 下看 `/var/mobile/Containers/Data/Application/*/.com.apple.mobile_container_manager.metadata.plist`）。
- **jbi4（未上机，2026-11-12 22:10）**：Safari 容器在磁盘上存在（`Data/Application/3EA67E3B…`，metadata 完整，uid 501），
  MobileSafari.app 在根上 `/Applications/`（不在 cryptex）→ 判断 LS 记录缺 dataContainerURL（推测，未直接读 LS 库）。
  修复：`x9-jbctl --repair-container com.apple.mobilesafari`（记录无容器时 `registerApplication:` 重新登记），SpringBoard 里由
  AppSync 每次开机调用（有容器就什么都不做）。安装器加「网址下载」。runtime v5（local.json 已指向），包
  `x9/out/x9-jb-installer2-20261112`（56901632 B）已 staging（回读通过），镜像 `x9/out/x9-jbi4-20261112/boot.img`。
  要看：USB 日志 `x9-jbctl --repair-container ... exit=0`；Safari 下载/设置页不再崩。
- **jbi4 上机（观察，`x9/out/x9-jbi4-20261112/usb-log.txt`）**：`--repair-container ... exit=0` = LS 记录本来就有容器，上条推测错。
  仍 `CRASH com.apple.Safari`（进程名截断，SafeBrowsing 等服务）。反汇编 block：`getpid()` → `sandbox_container_path_for_pid(pid, buf, 1024)`，
  返回 0 就用 buf，非 0 才问 LS。内核 `sweet_amfi.c` 对没有 CFFIXED_USER_HOME 的进程返回「成功、buf 不动」（移植自 hackios
  kern_minimac.c，参考仓库同样如此）→ buf 未初始化 → `initWithUTF8String` nil → 崩。修复：未找到时返回 ENOENT（`x9_sbcont=0` 恢复旧行为）。
  镜像 `x9/out/x9-jbi5-20261112/boot.img`；白名单未变，mailbox 不用重写。未上机。要留意：其它依赖旧「成功」答案的进程是否出新崩溃。
- **jbi5 上机（观察）**：`CRASH com.apple.Safari` 消失（沙盒修复生效），但下载仍 "storage not available"。用户确认：
  文件 App「我的 iPhone」打不开；安装器「网址下载」能用。原因（之前 Android 只读扫描已见）：
  `group.com.apple.FileProvider.LocalStorage` 只有 Library、没有 `File Provider Storage`。修复：`x9-jbctl --ensure-local-storage`
  （SpringBoard 里以 mobile 建 `File Provider Storage` 和 `/Downloads`，已存在不动）。runtime v6（local.json 已指向），
  包 `x9/out/x9-jb-installer3-20261112`（2026-11-12 用户同意后已 staging，56901632 B，回读通过），镜像 `x9/out/x9-jbi6-20261112/boot.img`。
  注意：jbi6 白名单是 v6，必须先 staging installer3 才能用 jbi6；不 staging 就继续用 jbi5。
- **jbi6 上机（观察）**：`--ensure-local-storage exit=0`，磁盘上已有 `File Provider Storage/Downloads`，但「文件」里**根本没有**
  「我的 iPhone」，Safari 仍 "storage not available"。Android 只读排查（`x9/out/x9-jbi6-20261112/{ios-logd,fp}`）：
  fileproviderd 发现了 LocalStorage 扩展（EXConcreteExtension 列表里有）；`Application Support/FileProvider` 只有
  `LegacyEnabledStateHasBeenMigrated`（0 B）；`com.apple.FileProvider.Domains.plist` 只有空的 SpotlightDomains（排除「域被禁用」）；
  早期（时钟未设、1969 时间戳）fileproviderd 4 次崩在 `FPDDomainIndexer dropIndexForDomainRemoval`；日志 7 次
  `Persona List failed`（ENOENT/EPERM），但 config_personas 已开，不确定相关。**根因未知**；tracev3_strings 只有截断参数，
  需要真正的统一日志解码（mandiant/macos-UnifiedLogs，要先问用户）才能看 fileproviderd 自己的消息。
- **统一日志完整解码（2026-11-12 23:40，用户同意装 Rust + mandiant/macos-UnifiedLogs，克隆在 `x9/out/refs/`）**：
  `ios.logarchive` = diagnostics/* + uuidtext/* 的符号链接，`unifiedlog_iterator --mode log-archive --input ios.logarchive
  --output decoded.csv --format csv`（`x9/out/x9-jbi6-20261112/ios-logd/`，136 万行）。观察：
  - fileproviderd 15:04:17 `denying connection because the device is not set up` 只在开机最初（searchd），7 s 后正常发现 8 个插件 → 不是原因。
  - Files：`Could not set domain cache for domain <private> because storageURLs is empty`（×3）、`could not find a valid source`。
  - fileproviderd 每次查容器前 `Persona List failed ... ENOENT`；SpringBoard/小组件是 EPERM。
  - usermanagerd（USB 日志 UMD_INIT）：AKS sel 125/120 → 0xe00002c7（`AKS GETPrimary Identity`/`AKSLoadIdentity` 失败）；
    `APFSContainerGetBootDevice failed 49154` → `FAIL TO MOUNT DEFAULT SESSION`；内核 persona 99/199/200/1000/100 分配成功；
    `notify_register_check personageneration_changed: 8`。
  - 推测（未验证）：persona 列表 ENOENT → LocalStorage 域 storageURLs 为空 → Files 不显示「我的 iPhone」。ENOENT 的来源还没定位
    （UserManagement 里打印该串的函数是 objc 桩，未解析）。
- `x9/out/x9-gpu280-purge-20261112`：GPU 280（Volatile 资源丢弃，`x9v0` 关闭），需先 stage 280 插件包。未上机。
- 蜂窝 sim2bind / gpu-cs-audit / 279 等候选的输出已在整理中删除；源码都在 main，需要时重新构建。

### 5.5 2026-11-13 GPU 243 分析 / 通话短信参考（观察）

- **GPU 243**：每 job 平均 5.7 ms = wait 4.0 ms（71%）+ pre_cache 1.3 ms（24%，随 pre_bytes 线性约 12 GB/s；`RENDER IMPORT WC wc=0`）。
  `inflight_max=0`、`queue_full_waits=0`、`queue_avg_us=18`：内核 FIFO（async + x9sl）没有被等，是 backboardd 交 job 慢；一帧约 35 ms 里约 25 ms 不在 GPU 路径，GPU util 10–20%，开 `x9if` 不会提速。
  backboardd 轮换 3 个 framebuffer surface。下一步：带 `x9t`（前端 telemetry）启动测那 25 ms。注意 boot-device.json 已 603/607 字符，且 `build-usb-prelink.py` 不接受 `--boot-arg`。
- **Android 通话/短信 MIPC 参考**（卡2，NR SA，IMS 在 MD 内）：`.worktrees/audio-bounded-fdt/x9/out/android-callsms-20261113/`（`mipc-call-sms.txt` 抽取，号码已被 Android 打码；gitignored）。
  - 来电：CALL_SIP_IND 4904 → CALL_APPROVE_INCOMING_CMD 8901（0100 call id、8101 号码、0102 toa 129、0103 seq、0104 20、0105 0）/ RSP 8902（**0100 approve/u8、0102 call id/u32**、0103 seq/u32；原笔记把前两者写反，2026-11-15 已按 MD consumer 更正）→ CALL_STATUS_IND 4901 / CALL_EVENT_IND 4902 → CALL_ANSWER_REQ 0907（无 TLV）→ CALL_HANGUP_REQ 0905（0100 0、0101 call id）→ CALL_GET_FINISH_REASON 090f。
  - 短信：SMS_CFG 0601 → SMS_SEND 0605/0606；接收 SMS_EXT_INFO_IND 4609 + SMS_NEW_SMS_CMD 8601 → RSP 8602（与 X9Messages.inc 一致）。未抓到拨出（CALL_DIAL）。
- **iOS 现状**：WMS→MIPC 短信已有（243 SIM2 发送拿到 modem CNF）。QMI VOICE(0x09) 未在 X9Baseband 服务表中，通话空白；声明后每个 VOICE 请求都必须应答，否则 CommCenter ATCS_TIMEOUT 重置基带。

#### 2026-11-15 通话 / 短信进度复核（只读检查，无新构建 / 上机）

- **核对基线**：main `d62cd4f`；`X9Messages.inc` 最后代码提交为 `874695c`（2026-11-11）。检查源码、历史交接及 Mac 现存 313–319 日志；不操作设备、不发短信、不改 NV / 分区。319 的具体 boot.img / 源码提交未对齐，以下是该捕获日志的观察，不是本次 main 镜像验收。
- **短信已实现**：未修改的 iOS QMISMS/WMS → MTK MIPC：按 SIM 独立 modem-ready / host-ready、WMS 订阅与路由、原始 PDU 发送、GSM 来信 / 状态报告投递、原 command txid 的 ACK/NACK；另有 WMS31/22 的 SIM/ME 存储列表 / 读取，以及 WMS23 仅 MT 标记已读并读回验证。实现位于 `x9/kernel/src/iokit/Kernel/arm/X9Messages.inc`、`x9/kernel/src/bsd/dev/x9_mipc_messages.inc`。存储列表仍受 4096 B 单帧限制（最多 15 条），无分页。
- **最新短信运行观察**：`/Users/tosasitill/319.txt:7689,9972`，SIM1 / SIM2 分别在 30.831 / 33.425 s 获得真实 host-ready CNF；`:7846,7849` 两 SIM 的 MT/MWI 订阅启用。`:67556` 64.205 s SIM2 提交 token703、QMI txn27；`:69306` 68.420 s modem 接受发送。314 / 316 / 318 也有 SIM2 发送 CNF。**CNF 不是送达回执，未据此证明对端收到**；这些日志未找到来信、host ACK 或存储读回成功 marker，不能把“没有本轮 marker”解释为完全没有接收能力。
- **历史接收证据与验收缺口**：`git show 965e111 -- NOTES.md` 记录 220 在 76.559 s 收到真实 GSM 来信并准入 host 事件队列，token1 等 host ACK。尚缺同一样本的 Messages 显示 / 重开持久化、对应 token 的 ACK 物理 TX result0、对端实际收到 MO 的完整证据。WMS24 删除、内存满恢复、IMS-only 发送 / SIP、non-CS type4/8105 仍未实现；不能称完整短信可用或已验收。
- **通话仍未接入**：`X9Baseband.cpp` 的实际服务表没有 QMI VOICE `0x09`；319 在 15.776 s 也仅宣布 9 个服务（`:3807`）。目前只有 §5.5 的 Android 来电 / 接听 / 挂断信令参考，未捕获 CALL_DIAL；没有 iOS 拨号、来电状态机、接听 / 挂断到 MIPC 的实现及端到端运行证据。IMSA 只提供已核实的注册快照，不等于 IMS 语音服务、VoLTE / VoNR 已实现。
- **音频不是通话完成证明**：main 已有 audio46 的扬声器 / 麦克风共享 AFE、audio47 的采样时间戳修复，历史 304 / 305 已观察到同时启动；但通话专用 MD 语音上 / 下行路由未接入。319 开机 mic 测试仍 min=-6 max=4 rms=2（`:3612`），不能据此称真实录音或双向通话音频正常。也不能再引用旧音频评估的“物理 speaker backend 未实现”作为当前结论。
- **下一步**：先用准确匹配的当前镜像，逐 SIM 闭合一条 MO（对端确认）和一条 MT（Messages 显示 / 重开后仍在 / 相同 token 物理 ACK 成功）；再测存储导入、长短信和状态报告。通话另开功能分支，先补 Android CALL_DIAL 捕获，明确 iOS VOICE 初始化请求及每请求响应，再实现呼叫状态 / 接听 / 挂断与 MD 双向音频，分别验收信令和声音。本次无 CI run、无新 boot.img。

#### 2026-11-15 通话 iOS 桥（基础阶段已合入，完整通话未完成）

- 用户要求：照 Android 原件实现，不猜测、不盲目出探针镜像。本轮在 `.worktrees/call-ios-bridge`（起始基线 `583f132`）开发，主 checkout 当时的 `feat/als-power-init-ios` 由另一窗口使用，未切换/提交其改动。该窗口将 ALS 自己合回 main 后，本轮 rebase 到 `2fead1c` 再 ff 合入，未覆盖其后续交接。
- **已核实**：modem_a.img 的 md1rom 是 nanoMIPS（VA `0x90000000` → 文件 `0x200`）；md1_dbginfo LZMA 解压后可恢复函数边界。`mipc_call_approve_incoming_rsp` `0x9131f2bc..0x9131f340` 明确读 0100/u8 approve、0101/u32 cause、0102/u32 call id、0103/u32 sequence。CMD producer `0x9132568a..0x913257b6` 与这些字段相符，号码为 bounded NUL string。MD producer 的原始 mask 是高半字节（如 0x10/0x20）；Android `libmipc.so` CMD dispatch `0x13630..0x136a4` 保持该 mask，不能直接套 SMS/WDS 的低位 SIM 判断。
- **iOS 原件核实**：20H392 `libCommCenterMCommandDrivers.dylib` 的 `createAnswerIncomingCallReq` `0x1f1a05da8` 构造 VOICE22/tag1，`CallIdentifier` writer `0x1f19483e0` 为 u8；`createEndThisCallReq` `0x1f19a0048` 的单通话分支为 VOICE21/tag1。多通话和状态映射未闭合，不能把 MD defaults 当 Android 策略。
- **材料缺口**：旧 `android-callsms-20261113` 原始抓取及 `libmtkmipc-ril.so` 已在整理时删除，现存 `libmipc.so` 不能代替 RIL。已请求用户补回 RIL/原始日志；没有普通 CALL_DIAL 发包证据、Android 呼叫状态转换和 MD 语音上下行路由依据。
- **已写代码子集**：共用 `x9_call_wire.h` 进行严格 incoming CMD / RSP、iOS21/22 单通话动作编解码；`x9_mipc_calls.inc` 独立通话 inbox / 原 txid+mask response owner，接入原有唯一 MD tick writer，SMS ACK 仍优先。没有状态/mapping producer，也没有生产调用者提交通话 mutation；`X9Calls.inc` 对意外直达 VOICE 明确返回 unsupported。tombstone 无论 TX 成功/失败都保留至 boot（最多八条），不猜 call-end 的身份退休规则。详见 `docs/call-ios-bridge.md`。
- **离线验证**：LLVM22 ASan+UBSan 的 18 项宿主测试通过（编译实际 transport/codec，C11/C++17，人工向量，不用设备/Apple/vendor 输入）；最初 Xcode15 ASan 在当前 macOS 启动时崩溃，换现代 sanitizer runtime 后通过，不是忽略断言。`Builder.compile_one()` 重编 `X9Baseband.cpp.o` 与 `sweet_x9_gpu_dependencies.o` 均成功；后者首轮缺 gitignored `x9_mdota.inc`，复制主仓库现有生成头后重编成功。没有 full build/link/boot pack。`md_call_evidence.py` 只读恢复本版本十个 CALL 函数边界成功。
- **提交 / 保留产物**：代码 `e9f7b7c` 已 ff 合入 main 并 push origin；主 checkout 上再次从新 owner 状态运行 18 项测试通过。分析/编译产物已移动保留在**主 checkout** `x9/out/call-ios-bridge/`：`compile-one-final.log` 两对象均 True / FAILURES[]，`host-tests/unittest-main.log` 为 main 的 18 项通过记录，`md-call-evidence.json` 为本版符号来源信息；原 nanoMIPS decoder / MD 与 Apple 反汇编同目录。原件及派生数据不入 git。基础阶段树/分支合入后清理；补齐材料后从 main 新开功能分支，不复用旧探针路径。
- **VOICE 服务不发布，不新增默认开启参数，不自动 approve，不构建 boot.img、不上机、不发通话或短信、不写 NV/分区**。本阶段无 CI run（`gh run list --branch feat/call-ios-bridge` 返回空；当前改动不命中 Actions push 路径）。信令/声音均未设备验收，不能称通话接入完成。

#### 2026-11-15 Android 通话材料补抓 / 实测卡2拨出

- 用户切回 Android，要求先抓文件再指导场景。本轮独立 `feat/call-android-capture`，起点 `f2797a9`；只读 adb，唯一 online PLJ110，实测 `su -c id` 为 root。没有清 logcat、改 property、重启 RIL、拨号自动化或设备写入。主树由其他窗口使用。
- **补齐原件**：实际 `mtkfusionrild` PID1761 的 maps 加载 `/vendor/lib64/libmtkmipc-ril.so`，抓回 **4148424 B / SHA256 `19fb2bc0c2b5532d8b4f8460779159277ebfe54930eff0be2198d9e871f5bdba`**，恢复至既有 `.local/x9/modem/vendor-bin/x9b/libmtkmipc-ril.so`（没有覆盖不同原件）。RIL/audio/IMS/CCCI maps、cmdline、匹配 init rc、音频配置、radio/all logcat、dmesg、telephony/telecom/isub/audio 基线共124项全部 exit0。拨号后 HAL 新加载的 `lib_aurisys_opve.so`、`libaurisysdemo.so`、`libspeechparser_vendor.so` 也从更新 maps 按实际路径补抓。
- **真实拨出**：用户在卡2拨10086（非紧急号），18:42:37.684 `0901` tx044c mask2 / 72 B：`8100 number/NUL`、`108/u8=0`、`101/u32=2`、`102/u32=1`、`103/u32=0`、`104/u32=0`（这是本场景实包，不是通用默认参数）。37.687 `0902` tag0/u32=0，之后6条 `4901`；51.865 `0905` mode100/u32=0、id101/u32=1；51.878 `0906` result0；51.894 `090f {}` → 51.896 `0910`。后验 telephony 两 SIM 均 callstate0。**0902 本包没有 call id，不能凭 CNF 成功给 iOS 造 id。**
- **关键限制**：6条 `4901` header 声明512/520 B，但实际 AT 日志 DUMP 都只有486 B，尾部截断，不能作为完整状态向量。新的 RIL 原库可用于离线核对完整 framing / 状态；10086 测试未有双向音频人工确认，不等于通话音频验收。用户已明确**不方便来电测试，后续仅按拨出逻辑推进，不再要求补来电**。
- **采集工具 / 产物**：`x9/tools/record_android_calls.py` start/status/mark/stop，仅录现有 logcat，显式私有 session 的设备选择、无重启/清日志、单次有界且不覆盖旧样本。实测18:42:02启动，18:45:29按用户需求停止，`live-logcat.txt` **24708239 B**。私有目录已保留在**主 checkout** `x9/out/android-calls-20261115-183941/`，含124项 SHA256 manifest、private session、live状态/事件与补抓 manifest；只提交脚本/脱敏交接，原库/序列号/号码/日志均不入 git。
- 无 CI run、无编译/boot.img、无 iOS 上机。下一步新拨号功能分支：将已验证普通 VOICE20 输入与这个 Android profile 对齐；不盲目开启 VOICE，不猜 call id、CLIR、domain 或音频配置。

#### 2026-11-15 拨出 bridge codec（按用户要求跳过来电）

- 采集提交 **`87f1625`**（已 push `origin/feat/call-android-capture`）；拨号代码 **`9483481`**，在其上独立 `feat/call-dial-bridge`，主树由其他窗口推进至 `ff330e1`，未切换主树/提交其改动。
- **已实现**：`x9_call_wire.h` 的普通 iOS20H392 VOICE20/tag1数字解码（CallingNumber wire不带NUL，tag10/u8=0可选）→ 本次实测卡2六TLV的0901 encoder；原唯一MD writer新增严格DIAL白名单，不放行未知profile/CLIR/紧急/IP/video/暂停/USSD、多通话或错误padding。数字1..40位、原件RIL `requestDial` `0x381410` 的strlen<41限制；profile必须由调用者显式给出，**没有通用默认domain**。
- **重要重放修正**：Android AT DUMP里的整个8100 value六字节都是`*`（包括终止符位置），不是未脱敏号码报文。RIL `addMipcTlvStr` `0x1c0b10/0x1c0b60` 明确使用strlen/length+1。重放先正确拒绝星号；第一人工替换样本仍保留masked terminator，在offset25失败；把整个value明确替换为人工`12345 + NUL`、归零txid后，其他captured tag/len/scalar/padding与生产encoder全字节一致。详见私有 `capture-replay.log/json`；不是iOS实际发包或完整原包成功重放。
- **离线状态依据（尚未接成producer）**：RIL `mipcCallStateToRilType` `0x386180..38619c` 只原样传0..5；不能拿它解释4901/tag109 ECPI event。`handleCallProgressIndicationMessage` `0x37ca80..37cef0` 取100/101/102/103/105/109/10a/8104/810b，另经handleECPI。当前完整4901尾部/GET_STATUS数组/iOS CallList状态语义还需离线核对。0902 result0没有ID，不生成假的QMI20成功/CallIdentifier；100/u32=1前部已完整捕获且对应挂断ID1，但残帧不驱动生产状态。
- **验证**：最终**24项 LLVM22 ASan+UBSan 宿主测试通过**（实际transport/codec、C11+C++17），包含6项新增dial测试：golden/边界/同缓冲转换/显式profile、拒绝未知TLV、完整白名单、writer→fragmented/late CNF、失败不重发、无ID确认。`Builder.compile_one()` 的 **`X9Baseband.cpp.o` 与 `sweet_x9_gpu_dependencies.o` 均 True**，使用主树已有gitignored `x9_mdota.inc`，没有生成stub。无full compile/link/CI/boot.img/上机。
- **产物全部已迁移保留至主 checkout**：`x9/out/android-calls-20261115-183941/`（原件与设备日志）；`x9/out/call-dial-analysis/`（RIL/Apple反汇编、symbol、host-tests-final.log、capture-replay.log/json、人工派生fixture与原redacted DUMP分开）；`x9/out/call-dial-compile/compile-one-final.log`及`compile-results-final.json`；`x9/out/call-dial-host-tests/`（24项宿主编译/用例产物）。原件与派生文件均不进git。
- **明确未完成 / 下一步**：仅普通拨出的codec和受控transport子集，不是已可用通话。还没有iOS实时profile/state/ID producer；VOICE09仍不宣布、所有意外直达请求仍unsupported、无自动dial/approve、短信不变。下一步**只围绕拨出**离线完成完整状态→真实ID/QMI20响应→初始化/订阅，另核实`SpeechDriverOpenDSP`→SCP/Aurisys实际语音路由；不再要求用户来电测试，不拿10086当双向声音验收，不出盲测镜像。
- **合入 / 最终验证**：代码`9483481`、交接`7d20fa3`保留，合并当前主树后 **`757507a` 已ff合入main并push origin/main**（两个功能远端分支也已push）。主checkout再次从干净owner运行**24项测试通过**，日志`x9/out/call-dial-analysis/host-tests-main.log`；全部私有产物已迁移，采集worker已停止。功能worktree/本地分支随后清理。没有调度CI，改动不命中现有Actions的`xnu/patches/**`路径；无新镜像/上机结果。

#### 2026-11-15 拨出 state / ID / 异步确认补全（源码阶段，尚非可用通话）

- **范围/基线**：`feat/call-outbound-state` / `.worktrees/call-outbound-state`，基线main `4558c1f`。只围绕用户已有卡2拨10086材料离线补桥；没有设备操作、自动拨号、来电测试、property/logcat/RIL/NV/分区修改或boot.img。短信实现不变、音频窗口独立，不混入其改动。本条替代上方“尚无state/ID producer”的当前状态，不改写历史结果。
- **新增观察**：采集 `x9/out/android-calls-20261115-183941/live-logcat.txt` 的RIL解析日志实际保存 `130→2→132→6→6→133`；对应原DUMP前部完整100 ID1、101 direction0，初始mode0、后续（含END）mode20、IND mask2。六条DUMP尾部仍缺失，不补零、不据日志造完整帧；`isIbt`来自105 flags的bit16..17，不是101 direction。实际RIL原库与上一轮同SHA256。
- **静态依据**：RIL `handleImsCallInfoUpdate` `268db0..269dd0`：130经`assignImsCallId` `26ecc0..26ee58`绑定真实MD ID，2→内部state2，132/6→3，133→8/remove；`getRilCallState` `199ee0` 的 `125e20` 表是`{2,2,3,0,1,4,5}`。不能混同ECPI、内部state和RIL状态。Apple `CallEventStatus` `1f1939f8c` 跳表/字符串证明ORIGINATION1/ALERTING5/CONVERSATION3/END9，故采用130→1、2→5、132/6→3、133→9；成功dial响应 `1f1ac2a34` 读**tag10/u8真实CallIdentifier**。
- **完整wire子集**：新 `x9_call_state.h` 校验4901全帧和唯一已知tag，8104/810b bounded NUL，8106是binary408而非cstring；真实u32 ID只在1..255范围可交给Apple，不造ID/不截断。END留身份、迟到6不复活，未知/外来ID/多通话fail-closed。只实现实测IND2的MO/mode0→20，不推广CMD高mask或来电。
- **GET_STATUS更正/实现**：RIL `381354..381398` 的0909为空请求；MD `9131d574..d86c` **无条件输出result0/u32+count100/u32**，失败不当empty。11数组为10b/10c/10d/10e/10f/110/8111/112/113/8114/115；libmipc `12310/12420`、MD `9141b68a/9141b77c`证明len0/16B descriptor（reserved8B=0、base/u16、count/u16）+独立base+i TLV，非packed数组，count0无descriptor。完整parser限7 native记录，拒绝孤儿/重叠/重复ID。**没有完整真实090a样本、没有执行parser重放**；MD含IMS，RIL过滤20..39是CS消费策略。
- **异步iOS staging**：完整owned卡2empty snapshot才设count_known；查询TX记revision，穿越事件/mutation的旧CNF不更新快照。`x9c_dial_submit`保留原txn/msg、client generation/SIM、真实MD token和epoch，0902成功+下一MO epoch汇合才可生成18B成功响应。超时60s仅为本地host guard；删client/换SIM/超时仍保token、消费迟到CNF，不复活响应、不自动重发，号码buffer清零。锁内admission和writer选择均复核revision/count；发送前过期返回本地take=-3，不伪造MD CNF。guarded lease留到boot，**仅一次staging尝试，无后续action/release admission**，不是最终通话容量；唯一writer/SMS ACK优先不变，未声称MD ring在途状态/全部race已闭合。
- **domain说明更正**：`getDomain(bool,bool)`初值由IMS-request bool决定0/5，非紧急直接返回；requestDial的第三bool/w4来自IMS请求分支。先前“一切domain0都依赖实时网络”过宽；`getEccPreferredDomain`仍查询状态，iOS完整IMS/profile provider尚无，不从实测0泛化默认值。
- **编译里程碑**：首次因缺`kernel-objects/`目录失败，未进入源码；补目录后C对象通过，C++对象因共享state类型在匿名namespace与C ABI前置声明不一致失败。将header移到`X9Baseband.cpp`全局scope后第三轮 `Builder.compile_one()` **两个对象True**。host harness新增QMI/client/clock stub与9项人工用例源码；host `clang++ -c`、共享header C11 `-fsyntax-only -Wall -Wextra -Werror`均exit0。**遵守x9/AGENTS：本轮未运行测试脚本/套件，33项源码不等于33项通过；前轮24项通过不套用于本次修改。** 最终收尾编译/提交另记下方。
- **尚未接通/下一步**：`x9c_request`对全部VOICE仍unsupported，服务表无09；没有普通VOICE bootstrap/BindSubscription44/IND注册、完整运行profile、七字节CallList其它字段发布或SpeechDriverOpenDSP→SCP/Aurisys双向音频闭环。`x9c_dial_submit`无生产admission调用者，observer不自动查询/注册/approve。继续从实际consumer核实剩余字段/初始化，不出探针镜像，不要求用户来电或再拨号，不称iOS可拨。
- **私有产物/复建**：本功能树gitignored `x9/out/call-outbound-analysis/`（MD/libmipc/RIL/Apple反汇编、枚举/状态表、解析日志）与 `x9/out/call-outbound-compile/`（`compile-one-2.log`失败、`compile-one-3.log`两True、`host-compile.log`、`c-header-compile.log`）。只用现有 `.local`，`x9_mdota.inc`从主树已有生成头复制，没有新stub。用真实`Builder.compile_one()`依recipe选`X9Baseband.cpp.o`、`sweet_x9_gpu_dependencies.o`，先建out/kernel-objects目录；不调用compile/link/boot。`gh run list --limit 3 --json ...`返回`[]`，无CI run ID/调度。
- **最终源码编译**：`compile-one-final.log` / `compile-results-final.json` 两对象 **True、recipe compile exit0**；`host-compile-final.log` 的`clang++ -c` exit0、`c-header-compile-final.log`的C11严格语法编译exit0。只编译源码，没有执行新/旧用例、链接kernel、打包/二次检查镜像、上机或设备变更。本次生产代码+人工回归源码准备提交/合main；确切提交和产物迁移状态下次收尾记录。
- **提交/交付收尾**：代码与上方编译交接 **`cb1f261` 已push功能分支、ff合入main并push origin/main**；主树无其他窗口未提交变更，没有覆盖audio/GPU/BT worktree。私有分析/编译目录已完整移到**主checkout** `x9/out/call-outbound-analysis/` 和 `x9/out/call-outbound-compile/`，原始日志中工作树绝对路径保留，不回写原件；采集原件仍是 `x9/out/android-calls-20261115-183941/`。本收尾NOTES提交随功能分支再ff合入main/push后清理本地worktree/分支。没有main重跑套件或新编译、无CI/镜像/设备结果；真实初始化/profile/CallList/audio与parser执行仍待后续，不能当已可拨号版本。

#### 2026-11-15 真实来电立即保全 / 路由修复（本轮进行中）

- **用户最新目标**：补齐内核与 iOS 通话/短信，最后交付修复镜像；仍按 Android 实测，不盲测、不猜字段，不要求再补来电。功能树（已清理） `.worktrees/call-incoming-routing` / `fix/call-incoming-routing`，起点 main `e974321`；其他 audio/GPU/BT worktree 独立，未覆盖其改动。
- **采集观察**：刚发生真实呼入已立即只读抓 retained-all，再有界 live，19:29:52 停止本地 recorder；所有读操作exit0，不清日志/改property/重启RIL/设备reboot/flash/写分区。完整私有材料 **主checkout `x9/out/android-incoming-20261115-192820/`**：all 4312351B、radio 1142226B、live 1074421B，另有telephony/telecom/audio/audio_flinger/audio_policy/dmesg/maps、45条CALL frame metadata和201行语音窗口。all保住开头，radio开头已滚出，不以radio缺失否定all。
- **关键冲突已解释**：完整8901 mask22/tx2a9a/72B → Android8902 mask02/同tx/40B，approve1/id1/seq100。此前“整字节单bit / 原mask响应”会拒绝此真实CMD。已追RIL `obtainMipcCmd19f978..980` → `mipcSlotIdChangeToSlotId1ada10`（低4位、表129c50）→ incoming handler传slot/tx → `requestSetCallIndication382458..47c` → `obtainMipcData1c0814..824` → `slotIdChangeToMipcSlotId1ad9e0`（slot1映射2，表129c44）。**因此mask低位转换有原件完整依据，不是仅据实包猜mask&0xf。**
- **实测来电信令**：卡2，native id1，mt1：19:27:15.391 incoming8901；15.478 approve8902；15.483 ECPI0/mode20；23.258 ANSWER0907{}；23.263 ECPI132；23.269 6→6与0908/result0；31.338 HANGUP0905{mode0,id1}；31.357 ECPI133+0906；31.390 090f{} →31.399 0910/cause16。**4901仍只有486B、声明528/536B，号码也遮盖，不能补零/造完整wire；未证明双向音频。**
- **已改、编译通过**：incoming codec保留原mask，新增sim/response_mask；按同版RIL低1/2选本机slot，RSP用低1/2保留txid，拒绝高-only/非法低位/fanout/SIM3/4，不使用Android fallback误选卡1。owner原CMD身份和SMS ACK优先不变，尚不自动approve、不发布VOICE09。人工回归源码更新golden并新增route项，按规则只编译、不运行套件。反汇编/table解码已移至主checkout `x9/out/call-routing-analysis/`（不入git）。
- **本轮编译**：真实recipe `Builder.compile_one()` 的 `sweet_x9_gpu_dependencies.o` 和 `X9Baseband.cpp.o` 均True/exit0；host `clang++ -c` exit0；C11 header `-fsyntax-only -Wall -Wextra -Werror` exit0。日志已移至主checkout `x9/out/call-routing-compile/`。人工项源码合计34项含服务表守卫，未执行；没有full link/boot pack/CI/设备操作。
- **剩余/镜像边界**：初始化、完整运行profile、生产DIAL/ACTION、七字节CallList其余字段与MT状态、MD/SCP语音路由和短信完整验收未闭合；VOICE仍关闭，尚无本轮最终镜像/CI/run ID/上机结果，不能把编译子集称修好通话短信。

#### 2026-11-15 来电 native state / iOS observer 补齐（本轮，未发布服务）

- 路由修复 **`c570c4d` 已push功能分支、ff合main并push origin/main**；routing树/本地分支已删；上轮已合入的outbound树/本地分支也已清理，原件/分析/编译产物均在主checkout保留。新 `feat/call-mt-state` / `.worktrees/call-mt-state` 基于 `c570c4d`，未改audio/GPU/BT树。
- **新增依据**：同版RIL `2686ec..268784` 用完整8901 native ID加入IMS对象；`269884..269898` ECPI0根据其它active记录设incoming5或waiting6；实测本次incoming5（无多通话推广），后续 `269474..26947c` / `269910..269918` 设active3，`2695e0..2695f8` 设end8/remove。Apple事件表已解出INCOMING2/CONVERSATION3/END9，仍不直接混用RIL枚举。
- **实现**：独立 `x9cw_inbound_state` / begin/apply/id + MD锁内snapshot API + iOS observer。完整8901只建立pending ID/seq/tx/route，不能当接听/许可/显示；完整ECPI0才进入incoming2，132/6为conversation3，133为end9。只支持实测卡2/mode20/evoltesi0/direction1；重复outstanding CMD先去重，迟到0/132/6/133不复活END，未知ID/状态、多MT/同seq重用fail-closed；u32 ID越界不截断，不推断全局count。**MT独立epoch，不会完成MO DIAL join；MO仍blocked，尚无生产action/profile/CallList发布。**
- **编译**：真实recipe两对象 `sweet_x9_gpu_dependencies.o`、`X9Baseband.cpp.o` 均True/exit0；host第一次因漏include initializer_list失败，补头后 `clang++ -c` exit0；C11共享state header `-fsyntax-only -Wall -Wextra -Werror` exit0。日志在功能树 `x9/out/call-mt-compile/`，失败原日志保留。新增3项MT人工源码、补Python runner注册route项，合计37项含未发布服务守卫，**未运行任何套件**。
- **语音下一最早缺口（静态观察，不是完成）**：实测Android audio HAL原件为采集目录 `files/vendor/lib64/hw/audio.primary.mediatek.so`，不是不存在的audio.primary.mt6993.so。`SpeechDriverOpenDSP::ScpSpeechOn2474a0` 的 `247530..568` 先注册ADSP feature14、scene0 DMA回调/UL-DL各65536B，然后Aurisys构建；`247688..2476bc` 调 `AudioMessengerIPI` vtable+10发scene0/type0/ack1/idAD20、时间+on的param1与实际modem index的param2。同版 `AudioMessengerIPI::sendIpiMsg1dce80` 已定位；OPPO audio_speech_msg_id.h解释AD20/SPH_ON和MD→DSP3D00/CALL_DATA_READY。**现有媒体播放/录音通路不是这条voice任务/DMA/MD握手，不能照抄48kHz就宣称双向声音。** 原件派生asm保存在功能树 `x9/out/call-mt-analysis/`，不入git；本次呼入buffer没有额外完整SpeechDriver verbose，不以其缺失否定旧拨出audio证据。
- **完整内核编译/提交收尾**：源码`2096225`已push `origin/feat/call-mt-state`；`python3 -I x9/build/build.py kernel --name x9-call-mt-state-2096225-20261115` **exit0，98/98 overlay对象（3 prebuilt），link/strip成功**。kernel-only产物`x9/out/x9-call-mt-state-2096225-20261115/kernel.development.sweet`；console/exit原日志在`x9/out/call-mt-compile/`，构建日志在产物目录。未运行套件、打boot.img或构建后二次检查；没有CI run ID/设备结果。此阶段合main/push后，分析、编译和kernel目录迁到主checkout相同`x9/out/`位置，清理MT树/本地分支，下一功能从main另开树。
- **尚未交付最终镜像**：VOICE09未宣布，bootstrap/Bind44/IND注册、完整CallList/生产DIAL-ACTION生命周期、voice DMA/MD握手、短信MT显示/持久化/物理ACK/对端MO验收未完成；短信源码本轮未改。本轮没有新boot.img/CI run/设备拨号或刷写。下一步应从voice任务0的配置/DMA/3D00握手和Apple bootstrap消费者继续，不再要求重复来电，不给“通话已修好”镜像标签。

#### 2026-11-15 短信存储启动请求实质修复（WMS31 TLV / readiness 队列）

- **真实阻断/范围**：读取已有`~/319.txt:8400–8401`、`:8661–8663`，iOS实际发WMS31/len19，被返回unsupported0x5e。不能把尚未对齐源码提交的319当本次镜像验收，但当前main同样错把list的tag11当filter、tag12当mode。原20H392 `QMISMSCommandDriver::readStoredMessages` `1f196c298`构造31，`1f196c410..41c`明确tag11/Message_Mode=3GPP1，`1f196c4d0..594`明确tag10/RequestedTag=unread1；storage01/u8。RawRead22与ModifyTag23的mode仍在tag10（`1f197577c..784`、`1f1975930..938`），RawRead的tag11/IMS可选零不是筛选标签。未猜测WMS24格式，没有新增删除请求或设备存储写入。
- **已改生产路径**：共享freestanding `x9_sms_storage.h`从完整QMI请求解码31/22/23，验证唯一字段/大小/制式/storage/index及MT-read-only修改；list改用filter10/mode11，native607的flag转换和608过滤核对都读filter10。旧错误tag12与未知制式/重复/超范围继续拒绝，不造空列表。Apple启动list发生在卡2真实host-ready33.425s之前，故验证后可进入原有bounded per-SIM队列；仅poll看到该SIM真实ready CNF后才发607，control/offline仍拒绝；失client/超时不启动延迟mutation。短信ACK与唯一MDwriter不变。
- **编译/未验**：新增人工回归源码`sms_storage_host.cpp`，host第一次严格编译因aggregate漏完整initializer失败，修正后二次`clang++ -c -Wall -Wextra -Werror` exit0；共享header C11严格syntax exit0。完整`build.py kernel --name x9-sms-stored-tlvs-20261115` exit0，98/98 overlay、link/strip成功。遵守x9规则，未执行新/旧测试套件、没有boot.img或构建后二次检查，无CI run/设备操作。最终源码提交与固定提交构建另记下方。
- **固定提交/最终kernel-only构建**：源码`0e2777d`；`build.py kernel --name x9-sms-stored-final-20261115` **exit0，98/98 overlay、link/strip成功**；仅kernel（文件名中的final指此窄修复固定提交构建，不是通话短信最终镜像）。产物`x9/out/x9-sms-stored-final-20261115/kernel.development.sweet`，console/exit在`sms-stored-compile/`。最新header C11 syntax及host严格C++ compile均exit0，人工用例未执行。收尾提交随功能分支合main并push；本树两个kernel目录、分析及编译日志全部迁回主checkout相同`x9/out/`路径后清树。下一步仍为CALL生产初始化/动作/状态发布与voice音频，不要求用户补来电或刷这个kernel-only产物。
- **原件/限制**：私有分析/编译在功能树`x9/out/sms-delete-analysis/`（目录名称为最初离线删除研究，最终未开放删除）与`sms-stored-compile/`；Apple/RIL/MD asm只放out。WMS31修复不等于Messages真实来信/重开持久化/同token物理ACK或MO对端收到已验；VOICE09、生产通话生命周期和voice音频仍未闭合，**尚无通话短信最终修复镜像**。功能分支`fix/sms-stored-tlvs`，完成合main/push并保留out后清树，不覆盖audio/GPU/BT活动树。

#### 2026-11-15 短信初始化 / host deadline 修复（并行静态子项）

- **范围 / 原因**：用户要求多子代理加速后独立树 `.worktrees/sms-init-deadlines` / `fix/sms-init-deadlines`，起点main`55c3508`。只读源码确认`X9Messages.inc`全局CMD注册超时后迟到成功会清`init_hold`，逐SIM timeout仅记日志而迟到HOST_READY可置ready；普通jobs先take再判deadline也可能向首次超时poll回success。不是新设备观察，不扩展泛用短信/IMS策略。
- **生产修复**：全局及逐SIM在take前判`now>=deadline`并terminal hold；迟到原CNF仍drain，不重新推进注册/host-ready、不清hold。control或MIPC失联在job admission之前撤host-ready并hold已启动初始化，但保留原token，可消费已到完整结果；全局hold不会阻挡原逐SIMtoken drain，单SIM超时不hold另一SIM。后续CFG IND只更新真实modem readiness，不能解除host init hold。queued存储/SMSC额外要求control-ready。
- **普通job边界**：host deadline先返回一次NOT_READY；**不撤销modem副作用、不等于MO未发送/不等于送达失败**。已提交MO/CNF、ACK原token仍take，物理ACK成功仍正常退休对应CMD/incoming，超时txn不再回第二个success、不重发。存储TAG超时/失client不开始延迟mutation；已有明确写入后的hold规则保持。
- **编译 / 证据**：新增`sms_init_host.cpp`编译实际SMS owner与MD transport，8项人工源码边界（now==deadline、迟到CNF、单SIM隔离、offline drain、MO/ACK、queued control loss），**只编译未运行**。私有compile日志在树`x9/out/sms-init-deadlines-compile/`；最终host C++17严格`clang++ -c` **exit0**（`host-final.log/.exit`），真实recipe`X9Baseband.cpp.o` **true / exit0**（`recipe-final.log/.exit`、`recipe-results-final.json`），`git diff --check`无错误。无测试套件/设备操作/CI/boot pack，未修改audio/GPU/BT。
- **提交 / 集成交接**：生产源码`b0055a4`（`fix/sms-init-deadlines`，已无冲突rebase main`3b6717a`）；rebase后的实际源码再作host严格`-c` **exit0**、真实recipe`X9Baseband.cpp.o` **true / exit0**（`host-rebased.log/.exit`、`recipe-rebased.log/.exit`、`recipe-results-rebased.json`）。父代理统一合main/push/完整kernel构建，本子项不自行合并或pack；删除树前保留该compile目录到主checkout同名`x9/out/`。
- **独立inbox poison追加 / 静态确认**：父代理授权在同树追加SMS owner/API与共享transport两处SMS hunk。`x9mm_rx`在重复CMD改内容/多SIM歧义/16-slot overflow只置`x9_mm_poisoned`，而generic online/submit/CNF未看它；旧代码因此可在message_event/reply已不可用时仍占SMS注册/host-ready/queued存储/SMSC/MO token，并消费成功CNF。是源码证据，不是新设备观察。新锁内只读`x9_md_mipc_message_online()`只复制generic及SMS inbox可用性，不改CMD/tombstone/token；SMS init/ready输出/准入/queued改用它，CNF消费后再查一次，不伪造MD readiness丢失。
- **原子准入 / writer界限**：allocator锁内与writer前只拦实际SMS依赖集合：`601/605/607`、`30b{8601,8603}`、`307{4601,4a01}`；`a04` IMSA只读快照、CALL/data注册保持独立，IMSA query/background仍按generic在线。ACK优先选择保持；仅generic `state1`尚未TX候选因SMS poison转原有`state6`并擦未TX帧，`take==-3`明确本地拒绝，不是MD CNF。Messages init仅`-3`清token并terminal hold，其他负rc仍保留不确定原token；普通job `-3`答NOT_READY后退明确未TX操作，但TAG step2已有先前物理改tag时保持host hold。已经`state2`/selected/partial token、kind0 late CNF和physical ACK TX-done/reply_take均不变；不清SMS poison或原CMD、不重发。
- **追加编译 / 交接**：源码向量追加12项（共20项），含真实owner重复CMD触发poison、锁内准入集合、global/SIM的pre-TX与late-CNF、queued/ready、TAG mutation前后、physical ACK写入期间poison和未知token保留；全部**只编译未执行**。`x9/out/sms-inbox-compile/`的最终严格host `-c` **exit0**（`host-final.log/.exit`），header C11 syntax **exit0**（`header.log/.exit`）；最终真实recipe`X9Baseband.cpp.o` / `sweet_x9_gpu_dependencies.o`均**true / exit0**（`recipe-final.log/.exit`、`recipe-results-final.json`），`git diff --check`无错误。本子项不自行合main/push/kernel/boot/设备，父代理统一review、rebase集成与kernel-only。
- **固定源码 / 新main编译**：inbox生产`56ffe32`（原`8ab81aa`），deadline生产`635b2d4`（原`b0055a4`）；三提交已无冲突rebase实际main`17ac525`（含CALL元数据`ea1407f`及其他窗口sensor提交），两方NOTES保留。新基线host严格`-c`、header C11 syntax均**exit0**，真实recipe两对象均**true / exit0**（`host-rebased.log/.exit`、`header-rebased.log/.exit`、`recipe-rebased.log/.exit`、`recipe-results-rebased.json`），工作树待父代理review集成。
- **剩余**：SMS独立inbox poison的准入/ready矛盾已按上述静态路径修复，仍缺设备验收；WMS24/内存满恢复/IMS-only或non-CS短信尚未实现，Messages显示/重开持久化/同token物理ACK及MO对端收到仍缺设备验收，不能称通话短信最终镜像。删除树前保留`sms-init-deadlines-compile/`与`sms-inbox-compile/`到主checkout同名`x9/out/`。

#### 2026-11-15 未发布 MT / 原子 host queue admission（静态窄修复）

- **基线 / 确认缺口**：父代理授权独立树`.worktrees/sms-mt-admission` / `fix/sms-mt-admission`从已集成CALL/SMS的`a6f63b5`开始。源码确定`message_event`在MD锁内将真实CMD交给adapter（state2）后解锁；重复CMD内容变化可仅poison独立inbox，旧`x9m_deliver`仍会向host入队，queue-full/client absent后重试也不复核。这是可达源码路径，不是319–321中的新MT/设备复现；不抓新日志或要求用户拨号/来电。
- **锁序 / 阻塞边界**：静态闭包`bb_x9_sms_event→cbq_from_modem`只bounded table/header/copy；普通`bb_push→IOMallocData`在XNU `IOLib.cpp`最终使用`Z_WAITOK`，因此SMS改为锁外预分配。callback只做`MD mutex→bb_lock`的真实bounded queue入队、wakeup/thread_call排队，不malloc、不MD重入；这不是“完全nonblocking”承诺，两个mutex仍可等待。bridge唯一MD调用为无锁`sim_count`，poll在bb解锁后才`bb_send/del_client`；`thread_call_enter`只排队，`bb_complete`取帧解锁后才host IO completion，无同步反向MD路径。普通`bb_push`、CALL/data和其他窗口不改。
- **生产修复**：新受信同步`x9_md_mipc_message_publish(token,sim,callback,context)`仅返回0/1，AT readiness在入MD锁前读取；MD锁内重验generic online/poison、SMS poison、真实state2 token、原mask低SIM位精确路由及已有reply同款CS envelope（8601/type3、8603/status PDU），锁保持到真正host queue admission。整原mask（含真实高位如22）、CMD/PDU/token不修改、不ACK、不退休。control-ready由QMI单owner先检查，早读message_online仅避免不可用期间alloc，不替代原子授权。frame/packet锁外分配构造；bridge成功交queue所有权，失败caller锁外zero/free并保留incoming PDU/token，仍不默认ready/空成功/auto-ACK。
- **未改边界**：已delivered不会向新generation重新发布；replying/selected/partial/物理ACK TX-done及late-CNF drain不变。queue receipt仍不是WMS ACK、Messages持久化或对端收到。显式DISCARD仍使用原reply门禁，不新增ACK策略。
- **固定源码 / 编译**：生产与人工源码向量提交`97d3745`；新增13项（总33）包含真实RX重复CMD poison、event→alloc间poison、control/AT/transport loss、state/token/SIM/type拒绝、queue reject再准入/poison、OOM、高mask路由、已发布后poison/client复用；**全部只编译未执行**。host C++17严格`-c`最终**exit0**（第一次编译仅因未用旧`x9m_del` helper而exit1，追加client复用源码向量后解决）；header C11严格syntax **exit0**；真实recipe`X9Baseband.cpp.o` / `sweet_x9_gpu_dependencies.o`均**true / exit0**，现有非fatal recipe warnings保留。`git diff --check`无错误。日志在本树`x9/out/sms-mt-admission-compile/{host-final.log,host-final.exit,header-final.log,header-final.exit,recipe-final.log,recipe-final.exit,recipe-results-final.json}`。无测试执行、CI run、kernel/link/boot pack、产物二次检查或设备操作。
- **集成交接 / 剩余**：父代理统一review、rebase/合main、push与固定源码kernel-only；本子项不自行合/push/pack。删除树前保留上述compile目录到主checkout同名`x9/out/`。本修复仍缺设备验收，CALL生产初始化/动作/状态和voice audio、WMS24/内存满恢复/未实现非CS、Messages显示/重开持久化/同token物理ACK及MO对端收到仍未闭合，**没有通话短信最终修复boot.img**。
- **本轮集成实际结果**：上述生产提交从`97d3745`无冲突rebase到实际main`8bc5670`为**`f1aa29f`**，交接HEAD`009c947`。在固定`009c947`执行`python3 -I x9/build/build.py kernel --name x9-call-sms-owner-integrated-20261115`，正式收尾**exit0，98/98 overlay（3 prebuilt）、link/strip成功**；同时严格host C++17 `-c`编译`call_bridge_host.cpp`及`sms_init_host.cpp`均exit0，所有人工向量仍未执行。该内核集成main既有END metadata`47722ab`及sensor源码，不为后续docs重新编译。产物`x9/out/x9-call-sms-owner-integrated-20261115/kernel.development.sweet`；console/exit/固定HEAD及两host日志在`sms-mt-admission-compile/{kernel-integrated-console.log,kernel-integrated.exit,kernel-integrated-source-head.txt,call_bridge_host-integrated.log,call_bridge_host-integrated.exit,sms_init_host-integrated.log,sms_init_host-integrated.exit}`。仅kernel，没有boot pack/设备/构建后二次检查；`feat/call-end-reason`的gh查询为`[]`，SMS功能push后记录实际CI。

#### 2026-11-15 单普通 MO 挂断生产 owner / VOICE44 独立绑定

- **范围/基线**：用户继续要求完成通话短信，不新增设备场景。新`feat/call-end-owner` / `.worktrees/call-end-owner`起点main`af62d8b`；只沿用已有Android拨出/呼入原件，短信WMS31修复保留不重复。未拨号、接听、清日志、改property、重启RIL、reboot/flash、写NV/分区；audio/GPU/BT树未改。
- **新增原件依据**：Apple `registerForQMIIndications::$_11::operator()` `1f1ad1b30..1b4c`明确VOICE44/tag01/u8：internal subscription2→wire1；`192ad04..14`只写1B，`size=4`含3B TLV头，不是u32。普通`createEndThisCallReq::$_2::operator()` `19a1bf4..1cd8`只消费ResponseBase error，成功直接返回，不要求造response ID；其请求guard来自`19a00d4`的25000ms。MD `mipc_call_hangup_cnf9131d374..398`只写tag0/result u32。
- **已接生产分发**：`x9c_request`接明确VOICE44本地绑定和VOICE21/tag1非零真实u8 ID，不是新增unused staging。VOICE独立bound/sim/generation、无默认SIM。只有实测SIM2/mode20/同MO epoch先发owned0909{}，完整090a成功count1、同native ID、direction0/mode20/multiparty0及ACTIVE0/ALERTING3匹配ECPI state，才构造原Android0905{mode0,id}。MD admission与唯一writer各复核target epoch/revision/id/sim，原DIAL lease只能在mutation CNF已消费后走该guarded挂断；不允许空ANSWER/多通话/MT/初始mode0或猜profile。
- **确认/退休**：同token完整906才回原iOS transaction，MD失败/未知reply返回host error；CNF不造END/空CallList。删client/换generation/25s guard取消尚未writer-selected的905，selected/partial-TX原token继续等待迟到CNF、不回新client/不重发。实际END133 + mutation槽已消费之后仅一次readonly清理query；完整owned empty090a且epoch/revision仍有效、无mutation槽，才退休end/DIAL lease。stale/error清理query保持bounded hold，不循环探测；信令/声音未设备验收。
- **编译**：真实recipe `Builder.compile_one()`两轮`X9Baseband.cpp.o`、`sweet_x9_gpu_dependencies.o`均True/exit0；最新host`clang++ -std=c++17 -c`和共享header C11严格syntax均exit0。新增7项人工源代码，只编译未执行，包含完整挂断/过期query/发送前guard/删除及迟到CNF向量。私有原件派生asm在树`x9/out/call-end-analysis/`；原编译日志/exit在`x9/out/call-end-compile/`，不入git。固定提交完整kernel构建另记下方。
- **剩余/合入边界**：VOICE09仍不宣布；bootstrap/IND注册、生产DIAL策略、七字段CallList、MT动作及scene0/MD3D00语音DMA/AFE/iOS音频未闭合，**没有最终通话短信boot.img**。主checkout在本轮编译时已推进`f948f57`且另一个窗口正解决NOTES合并冲突（UU），不切换、不代解或覆盖其修改；本功能先独立提交/push，待main干净再rebase/合入。无CI run或设备结果，不要求用户补来电/刷kernel-only。
- **编译 / 并行合入续记**：初版`e259057`已push，固定提交kernel-only`x9-call-end-owner-20261115` exit0，98/98 overlay（3 prebuilt）、link/strip成功。另一窗口自行完成main合并至`82c72b4`、工作树干净后，本分支rebase为`a3dbdd3`，没有覆盖其修改；新基线kernel-only`x9-call-end-owner-a3dbdd3-20261115`也exit0，98/98/link/strip成功（新基线只新增ALS源码，不是产物二次检查/重建比较）。静态收尾再补相同client generation/transaction/ID的重复VOICE21去重，避免同transaction先BUSY后success/第二mutation；新增第8项人工源代码，仍只编译不运行。最终源码提交与固定kernel另记；`gh run list --branch feat/call-end-owner`返回`[]`，未调度CI，无上机/boot pack或构建后二次检查。
- **最终固定源码kernel-only**：代码`a3dbdd3` + 去重修复`6991dee`；最新host编译`host-compile-final.exit=0`，共享header最近一次C11严格syntax exit0，人工向量未执行。固定在`6991dee`的`python3 -I x9/build/build.py kernel --name x9-call-end-owned-20261115` **exit0，98/98 overlay（3 prebuilt）、link/strip成功**。产物`x9/out/x9-call-end-owned-20261115/kernel.development.sweet`；原console/exit在`x9/out/call-end-compile/kernel-build-owned-console.log`、`kernel-build-owned.exit`，其余两次固定源码构建日志保留。只是本挂断/绑定子项内核，不含VOICE09发布/完整可用拨号/通话音频；不打boot、不要求用户试刷。接下来ff合main/push，迁回三个kernel目录及`call-end-analysis`/`call-end-compile`后清理本地功能树/分支；其它活动树不动。
- **收尾已完成**：代码及编译交接`398b0ab`已ff合main，`origin/main` push成功（`82c72b4..398b0ab`）。三个kernel目录、`call-end-analysis`/`call-end-compile`已完整迁回主checkout相同`x9/out/`位置，原日志中的工作树绝对路径不改写；本地call-end树/分支已清理，audio/GPU/BT树保留。没有main重跑套件、boot pack/上机或构建后二次检查。下一功能从main新开树，先完成真正VOICE bootstrap/IND注册/CallList与运行DIAL profile，同时补scene0语音链，不能拿本kernel-only标记最终通话短信镜像。

#### 2026-11-15 CALL 初始化 / 注册 owner（生产子集，完整 bootstrap 尚未完成）

- **范围/起点**：用户要求继续初始化注册，`feat/call-bootstrap` / `.worktrees/call-bootstrap` 从 main`c01aae6` 开始；仅复用已保存的 Android/Apple/MD 原件，不操作手机、不再要求拨号/来电、不出探针镜像，不覆盖 audio/GPU/BT/SCP 活动树。
- **新核实原件**：Apple`1f1ad092c..0a80` 是 VOICE03/tag13/u8=0（关闭额外CallEventsNotify），`1f1ad0bec..0d44` 是03/tag14/u8=1（HandoverEventEnable），不是 voice-ready 或 CallStatus46 开关；writer`1f192a0c4/1f192a0e0`，两个response consumer`1f1ad0424/1f1ad0694`只检查Result错误。Android`37c458/470`→`1e2828/1e2c00`→libmipc`14228/14510`证明0307低位逐SIM、030b全局ff、tag100/u16消息ID。MD`91335654..91335a34`证明单ID成功Result0/u32，IND失败可额外tag100/u8=0。技术结论写现有`docs/call-ios-bridge.md`；私有窄asm/符号在`x9/out/call-bootstrap-analysis/`，不入git。
- **已接生产而非unused staging**：独立44绑定后的03显式disable请求触发全局CMD8901→逐SIM IND4901→IND4902→一次owned0909/完整090a；每步严格原token确认，保留不同client generation、逐SIM stage、registered与idle-baseline不同状态，无默认SIM、无NAS/WMS绑定借用。13=1/14=1明确unsupported，尤其handover未实现不回成功占位；不注册未消费的Android其余18项URC，不自动approve/answer，不从注册CNF造CallList/ID/voice-ready。换卡遇待答注册返回BUSY，删client仅清host请求，旧token继续drain；无waiter不自动下一步。host总25s/每物理步骤25s本地guard，物理timeout/transport loss终止本boot的该owner，迟到CNF不grant/advance/重发；SMS ACK优先/唯一MD writer保留。原泛用MIPC白名单没有整体封闭。
- **编译里程碑**：共享header C11 `-fsyntax-only -Wall -Wextra -Werror` exit0，host C++17 `-c` exit0；新增6项人工源码向量（codec/CNF/生产四步链/重复/超时/删除/unsupported），仅编译、未执行。真实recipe`Builder.compile_one()`的`X9Baseband.cpp.o`和`sweet_x9_gpu_dependencies.o`均True/exit0；日志/exit/result在`x9/out/call-bootstrap-compile/`。`gh run list --branch feat/call-bootstrap`返回`[]`，无CI/run ID。固定源码kernel-only构建与提交/合入另记。
- **未完成/下一步**：这只是实际MD注册子集，不是Apple完整bootstrap。VOICE09仍不宣布；handover14=1、CallList46七字段/号码/END原因发布、生产DIAL运行profile及admission、MT动作、scene0/MD3D00语音DMA/AFE/iOS音频和短信端到端验收仍未闭合，**没有最终通话短信boot.img**。不能把Result0或空snapshot称可拨号/双向通话；下步从真实handover/CallList consumer与运行profile继续，不要求用户补场景。
- **固定源码构建结果**：代码`4648b1c`；`python3 -I x9/build/build.py kernel --name x9-call-bootstrap-20261115` **exit0，98/98 overlay（3 prebuilt）、link/strip成功**。产物`x9/out/x9-call-bootstrap-20261115/kernel.development.sweet`；原console/exit在`call-bootstrap-compile/kernel-console.log`、`kernel.exit`，host/header/两对象编译结果也在同目录。不执行测试/套件、不pack boot、不上机、不做构建后解包/hash/反汇编/重建比较。接下来将代码及交接push/ff合main，把私有分析、编译和kernel目录迁回主checkout同名`x9/out/`后清理本地功能树/分支，其他树保留。
- **并行基线合入**：构建期间SCP窗口自行将静态LIST共享sequence修复合入main`aa0f151`；本分支原`4648b1c/dd7bfaf`已push后rebase main为`892a84c/ee28aad`，无冲突、未覆盖其它窗口。新源码基线单独构建`x9-call-bootstrap-scp321-20261115` **exit0，98/98 overlay（3 prebuilt）、link/strip成功**；这是新增SCP源码基线的编译，不是旧产物二次检查/比较。最新产物`x9/out/x9-call-bootstrap-scp321-20261115/kernel.development.sweet`，console/exit在`x9/out/call-bootstrap-rebased-compile/`；仍无boot.img/套件/上机。旧基线三个out目录已迁回main，最新两个目录合入后同样保留；功能分支推送使用自己已推dd7bfaf的expected lease，随后ff合main/push并清树。
- **收尾实际完成**：代码`892a84c`与交接`d0a2455`已ff合main，`origin/main` push成功（`aa0f151..d0a2455`）。两基线kernel目录及`call-bootstrap-analysis`、`call-bootstrap-compile`、`call-bootstrap-rebased-compile`均已迁回**主checkout**同名`x9/out/`，原日志绝对路径不改写；本地功能树/分支已清理，其它活动树不动。push后`gh run list --branch feat/call-bootstrap`仍`[]`。未在main重跑套件/重编/pack/上机；最新kernel只是注册子项，完整通话短信最终镜像仍未完成。

#### 2026-11-15 CALL handover 初始化续接（进行中，未交付最终镜像）

- **工作树 / 基线**：沿用 `.worktrees/call-status-publish`，分支重命名为 `feat/call-handover-init`，起点 `61d87af`；主树由其它窗口推进，合入前以实际 HEAD 为准。只读现有 Android/RIL/MD/Apple 原件；不操作设备、不运行测试套件、不碰 audio/GPU/BT 活动树。
- **原件观察**：MD `913258e6..91325a22` EVENT3 写4902/tag100=u32=3、tag115=u32 state；匹配RIL `37cfd4..37d020` 将0..3原值交 `notifySrvccState`，Android16 radio HAL 定义 STARTED0/COMPLETED1/FAILED2/CANCELED3。Apple20H392 lambda19 `1f1ad3864/38a0` 明确 indication54，consumer `1f1ac325c` 读tag1，`192ad18`为u8；`193a040`为START1/FAIL2/COMPLETE3/CANCEL4。因此映射 `{0,1,2,3}→{1,3,2,4}`，不是标准QMI编号猜测。真实10086材料没有SRVCC完成，不能称handover场景验收。
- **已接入源码，准备固定提交**：新freestanding `x9_call_handover.h`严格32B完整帧/唯一tag/u32/低SIM1或2/state0..3；iOS03/tag14=1真实订阅并发布54，不再unsupported占位。13=0与14=1两个独立transaction可并发，共享原CMD→IND→owned初始query链；不同options独立应用。接受enable时保存锁内RX cursor，待注册确认的状态最多8个，先Result后按顺序flush，generation/SIM/timeout/delete隔离，不回放订阅前queued事件。MD收到SIM2 handover时在writer选择前使旧DIAL/END基线失效、保持hold；**未实现交接后的真实call-ID/RAT/profile迁移**。
- **CallList原件分析**：七byte parser `1929b2c` 与consumer确认id/event/direction/technology位置，方向MO1/MT2、technology表NoService0/CDMA1/GSM2/UMTS3/LTE4/TDSCDMA5/Unknown6/UnknownTech2 7；并行复核已证明offset5为multiparty：consumer返回tree node，CallInfo=node+28，node+7d实为CallInfo+55；Base `1f039177c/178c`的`Is in Multi-Pary call: `和+55读取、`getLegacyFlags1f0390bec`交叉支持。此前把node+7d叫CallInfo+7d的记录需按本更正；call_type细分、offset6仍未闭合，不默认填0。`1929bfc`是CallConfigItem writer，不能借作CallList依据。未新增CallList发布。
- **编译边界**：host C++17 `clang++ -c` 与新header C11严格syntax最终均exit0（`host-final.exit` / `c-header-final.exit`）；真实recipe `Builder.compile_one()`的`X9Baseband.cpp.o` / `sweet_x9_gpu_dependencies.o`均True、exit0（`recipe-results.json` / `recipe-compile.exit`）。新增8项人工源码向量，**没有执行**；旧overflow向量改用完整RING6/u32，避免首个畸形progress就隔离而掩盖FIFO容量。静态复核另补CALL inbox poison时不能grant/flush注册，立即终止host等待、保持物理token drain。日志在树`x9/out/call-handover-compile/`，原件/派生asm在`x9/out/call-status-analysis/`。接下来固定源码kernel-only；尚无full kernel/CI/boot或设备结果。
- **仍未完成**：VOICE09仍不宣布；CallList46七字段/号码/END原因、生产DIAL运行profile与admission、MT动作、handover后ID迁移、通话专用audio及短信端到端验收未闭合，**无最终通话短信boot.img**。不要求用户再拨号/补来电，不把注册Result0当voice-ready。
- **固定源码构建**：生产代码及上述编译交接提交`d397605`。`python3 -I x9/build/build.py kernel --name x9-call-handover-init-20261115` **exit0，98/98 overlay（3 prebuilt）、link/strip成功**；仅有现存Wi-Fi cast-align等warning。产物`x9/out/x9-call-handover-init-20261115/kernel.development.sweet`，原console/exit在`call-handover-compile/kernel-console.log` / `kernel.exit`。构建后未做额外解包/hash/反汇编/重建比较。`gh run list --branch feat/call-handover-init`返回`[]`，本改动不触发现有workflow；无CI run ID、无boot pack/设备验收。接下来保留实际最新main、rebase后编译新源码基线再合入，不拿这个kernel-only作为最终通话短信镜像。
- **新main基线编译 / 并行授权**：用户明确要求多开子代理后并行复核CallList字段、DIAL运行profile和初始化/短信超时，不操作手机或运行套件。handover分支已从实际main`55c3508`rebase，源码`add7cf9`、构建开始HEAD`db24014`；新基线`python3 -I x9/build/build.py kernel --name x9-call-handover-rebased-20261115` **exit0、98/98/link/strip成功**（`kernel-rebased-console.log` / `kernel-rebased.exit`）。这是集成新main源码的编译，不是旧产物二次检查。子代理发现短信init/普通job迟到CNF可能先成功再判断deadline，独立`fix/sms-init-deadlines`树正在修复，尚未合入本内核。

#### 2026-11-15 CALL current 元数据纠错（并行分析发现，修复进行中）

- **工作树**：`fix/call-current-metadata` / `.worktrees/call-current-metadata` 从main`3b6717a`；SMS子项独立`fix/sms-init-deadlines`，不动audio/GPU/BT。用户要求只编译，所有人工向量未执行，未操作设备。
- **新原件证据**：MD `mipc_call_get_call_status_cnf`给10f的source record+17/u8→u32（NUMBER_TYPE），112为+99/u8→u32（CALL_RAT），113为+9c/u32（TYPE完整bitmap）。匹配RIL `385494..4a4`读113→w23，`3855a0/5bc`取bit0→RIL_Call.isMpty；10b/id、10c/direction、10d/mode、10e/state、110/toa既有映射未错。MD含IMS行，RIL过滤20..39只是CS consumer策略，不能据此说MD无IMS。
- **实际待修bug**：旧`x9cw_current_call.multiparty`错读10f，`priority`错读112，挂断target因此拿NUMBER_TYPE判断singlecall。现改名number_type/rat，保留113全u32，target看TYPE bit0；新增人工元数据向量使用distinct number_type2/rat6/TYPE0x100001，禁止NUMTYPE0放行多方或NUMTYPE2误拒普通数字。并行补证MD `9138fd0a..4c`取同session+3a0的TYPE低位，upper flags只合并bit4/8/12/16/20；CLCCS `913a29dc..e0`及CLCC `913a2a80..84`填同字段，mode20共用，支持在原子集上保守拒绝，不声称全部conference已实跑。
- **CallList证据追加**：tag1=count/u8+7B条目；END14=count/u8+{id/u8,reason/u16LE}3B条目（不复制memory padding）；号码presentation byte1→Base0基Available0/Restricted1/Unavailable2已核对。MIPC presentation来源为8114/detail_info[0]，不是105/107 word；当前parser验证后丢号码/detail，仍缺有界号码owner/转换，不新增空CallList假完成。
- **运行DIAL策略新结论**：key48已证为AP `ECC_PREFERRED_RAT`而不是MD/IMSA能力；RIL空current-call响应`26c1d8..26c218`重置该policy0；key48=0时getEccPreferredDomain所有RAT分支返回0。Apple普通Call_Type0、紧急9：Base PhoneNumberType1=Normal/2=Emergency/3=Emergency_Test。仍未接生产IMS准入，不能把普通voice0叫IMS bool或拿IMSA registered当voice capability。
- **编译/交付边界**：本元数据修复host C++17 -c / header C11严格syntax均exit0（`call-current-metadata-compile/host.exit` / `c-header.exit`）；真实recipe两对象均True/exit0（`recipe-results.json` / `recipe.exit`），新增1项人工向量、修改原target向量只编译未执行。准备提交/合入后与SMS新修复统一编译固定源码kernel-only；无本项单独kernel/boot/设备结果。SMS子代理已完成deadline修复`b0055a4/e71a2c8`，继续补独立SMS inbox poison的锁内admission与writer fence，不宣称当前集成已完成。VOICE09仍关闭，CallList/DIAL/voice音频和短信端到端验收未完成，无最终boot.img。

#### 2026-11-15 CALL 号码owner / RAT证据续接（仅编译，完整通话未完成）

- **已合入的纠错**：CALL metadata提交`ea1407f`已ff合main并push；handover代码`add7cf9`及交接`3b6717a`也已push。不是仍未提交的实验，旧段落“准备合入”按此更正；它们没有VOICE09/boot或设备验收结果。
- **本项工作树**：`.worktrees/call-number-owner` / `feat/call-number-owner`起点`ea1407f`，只改shared call metadata及人工源码向量/现有docs；SMS独占其初始化/准入，audio/GPU/BT窗口不动。主树由sensor窗口推进，合入以实际main为准。
- **号码owner已接原解析链**：progress8104/current8111的128B含NUL字符串与8106/8114 detail[0]原presentation有界复制，不持RX指针，整帧验证失败不改out；owned current快照按已有revision guard保存。新增valid位仅在成功解析时设置，zero-init不是Available。RIL388380已证native0/1/2..4→Apple0Available/1Restricted/2Unavailable，未知fail-closed不改输出；不把TYPE105/VIDEO_CAP107当presentation。仍无号码格式转换、progress identity merge或CallList发布，Restricted/Unavailable原号码不得由未来publisher外送。
- **offset6失败类已明确排除，不重复**：接收vector默认初始化会被`1929be0/be4`实际wire+6覆盖；相邻`1929bfc`是变长CallConfigItem，wire+6为其string长度，不是7B CallList字段。当前host CallList符号只有parse/resize/copy，无writer；QMIParserDynamic无vs/ALS metadata。ALS仍未证，不按标准/默认0填字段。
- **RAT决定性反例**：MD`913a0756`仅按mode范围生成CALL_RAT2（20..39），current112经session+39f/record+99取值。同一10086日志81634/81635行18:42:37.767与92175..92178行18:42:41.304/305明确voiceRAT20/NR_SA与VoNR supported；不能无条件2→AppleLTE4。RIL真实voiceRAT源为042f response8100/10B中networkType(+1/u16LE)，非PS/IMSA；本项未扩大query。窄asm在`calllist-remaining-analysis`/`dial-profile-remaining-analysis`，构建原件未二次检查。
- **编译结果**：host C++17 -c/header C11严格syntax均exit0，真实recipe`X9Baseband.cpp.o`/`sweet_x9_gpu_dependencies.o`均True/exit0；日志`x9/out/call-number-owner-compile/`。新增两个人工源码向量仅编译未执行。待SMS新源码集成后统一固定基线kernel-only，无本项boot/CI/设备结果。
- **严格边界**：VOICE09仍关闭；CallList offset6/真实technology/profile、iOS IMS准入、MT动作/handover身份迁移、通话专用audio和短信MT显示/持久化/物理ACK/MO对端验收未闭合，最终通话短信镜像仍未完成。不要求用户重复场景、不操作手机、不出探针镜像。
- **集成固定源码构建**：SMS deadline/inbox代码已集成，构建开始HEAD`4e7b84a`，`python3 -I x9/build/build.py kernel --name x9-call-sms-init-20261115` **exit0、98/98 overlay（3 prebuilt）、link/strip成功**；host新集成基线严格-c也exit0。产物`x9/out/x9-call-sms-init-20261115/kernel.development.sweet`，console/exit及确切HEAD在`call-number-owner-compile/{kernel-console.log,kernel.exit,kernel-source-head.txt}`；没有boot pack/上机/构建后二次检查，人工向量未执行。构建期间sensor窗口只新增NOTES一行`057e121`，SMS无冲突rebase为deadline`fc20e86`/inbox`38b3e9e`/交接`de1402c`并已ff main/push；本项随后rebase成`b3c4bec`，x9源码未因此变化，不为docs重复构建。`gh run list`的SMS与本功能均`[]`，无CI run ID。本kernel-only仍不是最终通话短信镜像。

#### 2026-11-15 IMS command context架构纠错 / 并行后续

- **已合入/push**：SMS deadline`fc20e86`、inbox`38b3e9e`和CALL号码owner`b3c4bec`，集成交接`a6f63b5`已ff main并push成功。kernel-only仍为构建开始`4e7b84a`的`x9-call-sms-init-20261115`（同上述x9源码），未pack boot/上机；没有为了docs重编/二次检查产物。
- **有意义的新原件证据**：Base CallInfo+44/u8是PhoneCallSubType：1Telephony/2Wifi/4VoLTE/8VoNR/16SOSCall，`isCallSubTypeCellularIMS1f039230c`只接4/8；独立于+40的普通号码类型。普通Eureka VOICE20 producer`1f1ac1f8c`未读取或透传+44，普通tag10=0不是IMS上下文，不能凭IMSA registered准入Android IMS_DIAL/cf33。
- **实际Apple路径**：根盘CommCenter`CallStatusDelegate::dial101017a94..101018318`取selected driver，经vtable+18 dispatch；typeinfo1015fa830→IMSCallCommandDriver、secondary1015fa390+18→100dcdbc8(this-=8)→100dcd7b0。独立subscriber/SIM call-control在100dcdcdc读取+44；后续100dcd114/async100dcf6e8不是Eureka sendDialReq。`IMSStartCall`导入ordinal58来自libIPTelephony，1b7d69208构造XPC{request=kCallStart,kCallDialedNumber,kCallOptions}，依赖真实client/connection链再sendXpcMessage1b7ccff18。最终接收端/SIP/MD桥及voice options仍未闭合，不猜context/default。
- **并行落实下一子项**：`feat/call-end-reason` / `.worktrees/call-end-reason`正在接成功完整ECPI133+presence+native16→Apple145的内部END metadata（Apple16实际Unknown/CSI1，145为NORMAL_CALL_CLEARING/CSI0）；无cause/非16/残帧/0910无ID不倒填。`fix/sms-mt-admission` / `.worktrees/sms-mt-admission`正在复核MD→host queue锁序后补undelivered MT原子准入；现确定缺口是SMS poison/control loss后仍可进入bb_x9_sms_event，不能仅一次getter声称全race闭合。两项未合入上述kernel，只编译、不操作设备。
- **文件与清理**：current-metadata与number-owner两个已合功能树的所有out已迁回主checkout同名`x9/out/`，树及本地分支已清理；编译原日志绝对路径未回写。原call-status树还被只读IMS接收端分析使用，旧SMS树保存review材料，暂不删。audio/GPU/BT等其它活动树未改。
- **完整目标未完成**：VOICE09仍关闭，没有最终通话短信boot.img；CallList offset6/完整身份生命周期、真实IMS桥/MT动作/handover迁移、通话专用audio、短信显示/重开持久化/物理ACK/对端收到均不能拿静态代码和kernel编译冒充验收。本文档子项无新增CI run/build/设备结果。

#### 2026-11-15 CALL END 原因空间闭合 / 内部metadata（仅编译，不发布END）

- **独立工作树**：`.worktrees/call-end-reason` / `feat/call-end-reason`从`a6f63b5`起步；仅shared call header、人工向量及现有docs/NOTES。父会话统一集成，不自行push或合main；audio/GPU/BT/SMS及registration/MT动作/profile不改，没有手机操作、探针镜像或新query。
- **原件语义证据**：MD IMS disconnect`910d30e6`与progress133分支`910d3366`均调用`imc_map_last_cause_to_ext_report911006ec`，CEER request`910f6678`也调用同mapper；CEER字符串table`929950d8[16]→9244a76c=CM_NORMAL_CALL_CLR`。真实完整0910在`x9/out/android-incoming-20261115-192820/retained-radio-logcat.txt:2977..2979`（19:27:31.399）含code16和同字符串；matching RIL`385848..5894`原样保留。0910无ID，只佐证原因空间，不倒填4901归属。
- **133/10a生产链闭合**：mapper输出→ECPI info+57/presence,+58/u16cause（`910d7f58..80`）→VDM ctx+f/+212（`921ecd2a`）→fill`921ecb6c`原值返回→END route`921e9758/9604`→encoder`9143dc36`的133→presence控制的common`9143da74`→L5 parser`91392d86..92db6`→session+4/+6到IND+231/+232（`91392ae0..aea`）→serializer`9132587e..894`仅presence1 LHU零扩展写10a/u32。无cause路径presence0；不拿内部备用31发布。完整细节见现有`docs/call-ios-bridge.md`。
- **Apple空间不可直接copy**：END14消费`1f1ac50a4`读u16，`1f19fd3c8`将native数值16解释为Unknown，`1f19fd830`对应CSI1；Apple145/0x91明确NORMAL_CALL_CLEARING，CSI table entry34→`1f19fd8e4`返回0。Apple0为OFFLINE，无cause不能默认0。
- **已接源码**：progress完整校验成功才`valid=1`；不门控旧手工对象的reducer，仅END原因转换依赖完整解析provenance。内部`end_reason{known,apple}`在MO/MT原identity/route/direction/state/revision准入成功tail保存，仅`133/mode20/has_cause/cause16`known145，其它/无cause未知。foreign/rejected/late133/revision overflow不能写原因；unknownEND不能late16补known；NEW130/新MT CMD begin清旧epoch。无CallList/END发布，无VOICE09能力声明。
- **编译事实**：host C++17 `clang++ -c` exit0，C11 header `-Wall -Wextra -Werror -fsyntax-only` exit0；真实recipe `Builder.compile_one()` 的`X9Baseband.cpp.o`及`sweet_x9_gpu_dependencies.o`均True，exit0。新增provenance/MO/MT三项人工向量只编译**未执行**。原日志/命令helper在树`x9/out/call-end-reason-compile/`：`host.log/.exit`、`c-header.log/.exit`、`recipe-console.log`、`recipe-results.json`、`recipe.exit`、`compile_recipe.py`；首次helper误取parent导致路径FileNotFound/exit1，保留`recipe-path-error.log/.exit`，修正为worktree ROOT后编译成功，不是源码错误。只复制主树现有gitignored local config和缺失MDOTA generated include，没有生成stub或读仓库外项目。
- **原件与限制**：旧call-status分析原件保留供另代理只读；本项所需22份窄asm/枚举已复制到新树`x9/out/call-end-reason-analysis/`，后续不依赖旧工作树构建。真实4901仍只486B logger前缀而declared512/520/528/536，**没有完整设备END frame/真机重放**，不能补零。无本项kernel link/CI run/boot/上机结果；最终通话短信镜像仍未完成，CallList offset6/technology、IMS准入、voice音频和短信端到端边界保持。
- **固定交付**：源码/上述编译交接原`277e875`已rebase实际main`baec47b`为**`47722ab`**，NOTES同时保留`fef6c31`的IMS架构纠错和上一集成kernel结果。rebase带入main既有proximity独立源码修复，未改变本项已编译的call host/header及两对象依赖源码，不为docs重新编译或二次检查旧产物。本树干净提交后交父会话统一集成；未push/合main，不把此局部metadata提交称最终镜像。

#### 2026-11-15 IMSStartCall接收端 / AP SIP终点与最新集成

- **END已收尾**：生产`47722ab`及交接`8bc5670`已ff main并push；`call-end-reason-analysis/`及`call-end-reason-compile/`已迁主checkout同名out并清本地树/分支。以上历史“未push/合main”不再是当前状态。最新统一kernel固定HEAD`009c947`，正式exit0，见SMS原子MT段；无boot/上机，不称最终镜像。
- **新的实际原件证据**：`IMSClientCreate1b7d67e4c..e68`与server instance`1b7b816b4..81730`共用`com.apple.ipTelephony`，实际CommCenter plist注册同MachService。`setHandlers1b7b62314..354`确实注册kCallStart→block`1b7b635ec`，读取kCallDialedNumber/kCallOptions；普通virtual+80→`ims::Call::dial1b7c41468`→async`1b7c41878/418cc`→`initializeNonEmergencyCall1b7b32708`，不是同名startCall wrapper。继续`IMSCallInfo::startCall/continueCall1b7b33110/1b7b28438/1b7b28de4`→`BambiClient::createNewCall1b7da99e0`→MO`1b7daa180`→handler/internalStart`1b7d7948c`→media创建；真实回调`1b7dad73c/1b7bc629c`调用SipSession::sendInvite。
- **AP终点闭合**：INVITE经SipDialog`1b7d0b438/1b7d0dea4`→SipTransactionLayer`1b7ce3ed0`→SipTransportLayer；IPSec原件address point`1d90a4d20`+38/+78/+80已由rebases定名sendMessage/bestTransport/sendData。`1b7d77aac`实际vcall+80→`1b7ac52a4`，`1b7ac5700`的socket+28→UDP secondary`1d90b7388`+28 thunk`1b7c3c2d8(this-=48)`→sendOverSocket`1b7c3c2c0`→ImsUdpSocket::send`1b7c3be68`→stub`1b7e0f0b8`；该dylib indirect-symbols表确认`_sendto`。因此Apple IMSStartCall主信令是**独立AP SIP流程**，不能把普通VOICE20/tag10=0当Android cf33/IMS_DIAL，亦不能由IMSA registered推导Apple SIP/call/media ready。这是静态路径，不证明设备当前选择此transport或已注册/拨成。
- **材料与清理**：原call-status树所有out无覆盖整体迁主checkout同名`x9/out/{dial-profile-remaining-analysis,calllist-remaining-analysis,call-status-analysis,call-handover-compile,x9-call-handover-init-20261115,x9-call-handover-rebased-20261115}`；原日志绝对路径不改写，旧树及本地feat/call-handover-init已清。原件不入git，其它audio/GPU/BT/camera/sensor窗口不动。
- **继续并行/完整边界**：三代理分别窄追CallList offset6实际consumer、Apple IMS provider/registration可替换边界、SMS剩余启动注册准入；不重复已排除猜测、不操作设备、不要求补场景、只编译不运行套件。VOICE09仍关闭；CallList完整身份生命周期、真正IMS→MTK桥、MT动作/handover身份迁移、通话专用audio、Messages显示/重开持久化/同token物理ACK/MO对端收到仍未闭合，**最终通话短信boot.img尚未完成**。

#### 2026-11-15 注册响应真实queue receipt / 下一集成基线

- **上一集成已收尾**：SMS MT生产`f1aa29f`、交接`009c947`及AP SIP文档`e1c0af1`已ff main并push成功（`8bc5670..e1c0af1`）；`gh run list --branch fix/sms-mt-admission`为`[]`，无CI run ID。SMS compile目录及`x9-call-sms-owner-integrated-20261115/`已无覆盖迁主checkout同名out，原日志绝对路径保留，本地SMS MT树/分支已清。没有为docs重编、测试执行、boot pack/上机或二次检查产物。
- **父会话新窄修复**：`fix/call-registration-admission` / `.worktrees/call-registration-admission`从实际main`e1c0af1`开始，生产**`4b2619b`**。旧`x9c_reg_answer→x9q_packet::send→coachz_baseband_qrtr`只确认port recognized，忽略bb_push的queue/OOM失败，随后仍grant registered/handover并丢弃retained54；分开入队还可能仅前半批成功，getter与队列写之间CALL poison未原子覆盖。这是源码确定缺口，不是本轮真机复现。
- **实现/边界**：Result03+最多8条retained54锁外预分配，新native publication API持MD锁检查online/独立CALL poison/floor并覆盖受信callback；callback复核single-owner control/generation/SIM与原25s deadline，再以MD→bb锁验证真实非release_pending VOICE client/service/node和整批queue容量。无分配/MD重入，整批all-or-none；失败锁外zero/free，queue/OOM只保留waiter/events重试local入队，不重发MD，成功才grant/clear。原未发布VOICE09、CallList/ID/profile、注册CNF、CALL/SMS mutation/物理writer均不改；已启用后的普通54路径仍未补queue-full持久重试，不能泛称所有handover发布race已闭合。
- **编译**：新增6项人工源码向量**只编译未执行**；host严格C++17 `-c` exit0，bridge.h C11严格syntax exit0；真实recipe两对象`X9Baseband.cpp.o`/`sweet_x9_gpu_dependencies.o`均True、正式exit0（现存warnings保留）。命令与日志在本树`x9/out/call-registration-admission-compile/{host.log,host.exit,header.log,header.exit,compile-recipe.py,recipe.log,recipe.exit,recipe-results.json}`。无本项单独kernel/CI/boot/设备，待SMS TAG pre-TX guard统一固定源码编译与合main/push。
- **SMS TAG真实发送前guard**：子代理生产`2db0d50`已无冲突cherry-pick并rebase为**`25e40ee`**；`git range-diff`确认两patch相等。TAG step1 indexed0607/103=0提交后仍state1时，旧host超时/删除不撤token，可能之后物理mark-read；新scoped API保存原slot/generation/64-bit monotonic deadline，allocator+唯一writer锁内检查期限，abandon匹配原token/slot/gen只state1→6/take=-3。selected/partial/late-CNF原owner继续drain，step2真实验证/hold不放开，其他generic提交者不变。9项新人工源码向量、SMS总42项只编译未运行；SMS/CALL兼容host -c、C11header、真实recipe两对象均exit0。私有`sms-startup-compile/`与`sms-startup-evidence/`已迁主checkout同名out；无新设备mark-read/存储验收。原SMS分支push/CI收尾另记。
- **新main与peer-review**：首次统一固定`d6320a3`的`x9-call-sms-publication-integrated-20261115`正式exit0、98/98/link/strip；两集成host -c均exit0。实际main随后推进`d044a61`，包含sensor源码而非仅docs，故无冲突rebase为CALL`c220b79`/交接`9b080bd`/SMS`25e40ee`；新源码基线`x9-call-sms-registration-main-20261115`也正式exit0、98/98/link/strip，不是对旧产物二次检查。只读peer-review发现callback判deadline后等待bb_lock可跨25s；修复**`051bc35`**将deadline传至actual helper，在bb锁拿到后、任何queue mutation前以同64-bit monotonic时钟再判，失败仍整批不入队。新增第7项源码向量仅编译，`host-deadline.exit=0`；peer再次核对实际route/capacity/ownership/锁序无具体阻断，不宣称运行验收。最新固定源码kernel编译结果另记，不复用旧candidate标签。
- **CallList新窄证据**：两consumer确实未消费offset6。first item`1f1ac37e0/37ec`仅读4→formatter；main loop`1f1ac4330..5608`仅读2/0/1/3/5、步长7。第二tag1 callback`1f1ac3934→5d30`仅读ID0，rawitem→emplace`1f1ad9a24`的x2在`9a3c`保存x22，`9abc`仅LDRB[x22]→node+20 key，CallInfo在node+28，constructor未带item指针；collision`5a94/98/5aa4/5ac4`只type2/state1。不指认为ALS/node+7e，不默认0。matching manifest20H392/d22ap与Mav17 archive9.00.02、qdsp6sw.mbn ELF32LE Hexagon原件已确认，handler字符串锚raw1c7d416/449/830，仅模块锚而非svc09→46 encoder。数据pointer refs和固定6/2/9 IDL候选未命中只排除该类；下一有据点是三个确定handler VA的Hexagon const-extender code xref→注册/dispatch→46 encoder，未宽扫或写codec。全部材料主checkout`x9/out/calllist-wire-six-analysis/`，无production/编译/设备结果。
- **IMS provider-ready新窄证据**：实际CCIMSClientFactory AP`101587cf0`+10/createCallClient10065c82c、+18/createSMSClient10065c9c8；root10065c6f8经Registry/SD manager→getIMSClientRef1f2e4c210按PersonalityID/controller获取client。更早sd::IMSSubscriberFactory AP1015494a8+38→100315998，controller create1f2e6803c/68118→ims::CreateNewClient1b7c43a00→manager create1b7c335b0，对象写+d8；StackDelegate注册后1f2e6829c直接handleClientReady1f2e68358，capability与EventInterface+90→root1007ab880→RestModule发布`/cc/props/ims_client_state`。这是AP对象可用，非网络注册；driver bootstrap100dd55f4观察该属性→100dcc390/100dcc570..588→同CC factory+10，仍是AP SIP。已找到真实抽象创建槽，但缺持续驱动MTK call/SMS provider的接口实现，不补ready常量/普通VOICE20字段，不据窄搜索说全系统无modem-managed provider；无production hook。材料主checkout`x9/out/ims-provider-boundary/`，两纯分析树/分支已清，原件不入git。
- **最新固定源码构建/CI**：固定**`051bc35eec95083ad72628a90003443a64a3393e`**执行`python3 -I x9/build/build.py kernel --name x9-call-sms-registration-deadline-20261115`，正式**exit0，98/98 overlay（3 prebuilt）、link/strip成功**。产物`x9/out/x9-call-sms-registration-deadline-20261115/kernel.development.sweet`；本树`call-sms-publication-compile/{kernel-deadline-console.log,kernel-deadline.exit,kernel-deadline-source-head.txt}`保留确切输入/结果，前两源码基线日志和kernel目录也保留。仅kernel，未pack boot、运行人工向量/套件、操作设备或二次检查；`fix/sms-startup-registration`原源码分支已push，gh查询`[]`，无CI run ID。main随后新增`8c7b7ac`仅NOTES一行，不为docs重编；合入时保留该交接。完整VOICE09/CallList/IMS provider桥/双向voice audio及短信端到端仍未完成，**没有最终通话短信boot.img**。
- **本轮合入已完成/当前下一步**：保留实际main`8c7b7ac`后无冲突rebase，源码成为CALL batch**`4e30917`**、SMS TAG**`07c591c`**、BB锁后deadline**`af5dbbf`**；只改docs的rebase未改变已编译x9源码。交接`09dd1c7`已ff main并push成功（`8c7b7ac..09dd1c7`），`gh run list --branch fix/call-registration-admission`为`[]`；无CI run ID。三个本轮kernel目录及`call-registration-admission-compile/`、`call-sms-publication-compile/`已完整迁主checkout同名out，私有原日志绝对路径不改写。SMS TAG原分支保留remote；本地树/分支已在range-diff确认与集成patch相等后清理，IMS/CallList纯分析树已清；当前CALL树/本地分支随本交接合入后清理，其他窗口不动。下一生产子项先补**已注册后普通54的bounded queue receipt/保序保留**，同时从上述Hexagon handler新锚定位CallList46原producer、沿真实CC factory创建槽恢复可实现的MTK provider接口，不重复ALS/default猜测。VOICE09仍关闭，最终通话短信boot.img未完成，不要求用户补场景或刷kernel-only。

#### 2026-11-15 live handover54：真实queue receipt与bounded保序

- **工作树/观察**：`fix/handover-live-publish` / `.worktrees/handover-live-publish`从干净main`ed71d17`开始。上一轮只修注册Result+retained54，已注册后的普通54仍用只返回port recognized的legacy发送，消费CALL inbox后queue/OOM可丢事件；这是确定源码缺口，没有新真机复现。
- **已接生产**：每client独立最多8条live54和最后RX sequence，保持SIM/generation/floor及重复状态原序；锁外预分配全部buffer，native publish持MD锁覆盖online/CALL poison/floor和受信callback，复核single-owner control/subscription/generation/SIM后以MD→bb锁验证真实VOICE route/service/node/non-release_pending及整批容量。注册/live共用all-or-none helper，成功才清owner，queue/OOM/partial alloc失败只重试本地入队，不重发MD。live事件没有虚构host deadline，注册Result仍在bb锁拿到后判原64-bit deadline。第9条/非单调sequence仅sticky-block该subscriber，已接受的enable waiter不能清owner；成功显式disable/delete/换SIM或generation的rebind才退休，不造ID/profile迁移。transport loss/poison撤订阅；原SMS/MD writer/VOICE09边界不变。
- **编译事实**：新增8项人工源码向量只编译未执行；`clang++ -std=c++17 -Wall -Wextra -Werror -c` exit0，handover header C11 strict syntax exit0；真实`Builder.compile_one()`的`X9Baseband.cpp.o`/`sweet_x9_gpu_dependencies.o`均True、exit0，既有warnings保留。只复制主树既有MDOTA generated include，没有stub。原命令helper/日志在本树`x9/out/handover-live-compile/{host.log,host.exit,header.log,header.exit,compile-recipe.py,recipe.log,recipe.exit,recipe-results.json}`。固定源码kernel/提交/合入结果另记，没有执行套件、pack boot或操作手机。
- **CallList新证据纠正**：Mav17 handler旧锚各指向完整C-string内20B，真实VA应为`c1ee2402/c1ee2435/c1ee281c`（PA`82ae2402/2435/281c`）。raw0即ELF，无MBN前缀，唯一PH15 raw181b000/VA c1a80000/PA82680000；同块NV路径有真实`c0e0b05c→c0e0b060`加载`c1ee23c1`对照，映射/LLVM Hexagon decoder成立。更正后完整字符串absolute extender、R/RW aligned u32引用及58个extended add(pc)候选最终目标均0；不能把名称存在称svc09→msg46 dispatch，旧零命中只排子串地址。邻接日志/二进制未证明service metadata，不猜IDL/offset6。新原件/精确脚本在CallList树`x9/out/calllist-modem-producer-analysis/`，未写codec/提交/设备操作。
- **并行当前边界**：IMS代理已证明legacy factory返回IMSCallClientInterface，legacy dial+28结果为bool非callid，正常driver另走typed CallHandle/model和CallEventDelegate；不能只替换factory冒充全桥。正在核对root CommCenter与提取libIPTelephony的vtable ABI差异，未混版本hook。call-audio代理证明OpenDSP+158为SpeechDriverNormal，sendIpcCmd最后返回0并非MD3D00 transport，真实scene0 callback只分DA00/DA10/DA11/DA12；iOS VirtualAudio真实`Baseband Voice`592B设备另有独立上/下行与CoreTelephony契约，正在只读闭合。不触audio/GPU/BT/camera/sensor其它窗口。
- **完整目标仍未完成**：本项不等于handover身份/RAT/profile迁移；CallList producer/offset6、真实IMS→MTK生产DIAL/MT动作、通话专用双向audio及短信显示/重开持久化/同token物理ACK/MO对端收到仍未验。VOICE09保持关闭，**无最终通话短信boot.img**，不要求用户重复10086/来电、不出探针镜像。继续收束真实provider/voice契约与可达生产子项。
- **固定编译/审阅**：生产及交接`2c266c9`，固定完整HEAD`2c266c9c517e77c43a4a19af05834096fadba35c`执行`python3 -I x9/build/build.py kernel --name x9-call-handover-live-owned-20261115`正式**exit0，98/98 overlay（3 prebuilt）、link/strip成功**；产物`x9/out/x9-call-handover-live-owned-20261115/kernel.development.sweet`，console/exit/源码HEAD在`handover-live-compile/{kernel-console.log,kernel.exit,kernel-source-head.txt}`。只读peer-review核对真实release_pending→delete→generation屏障、batch ownership、MD→bb锁序、overflow/disable、注册deadline，无具体阻断；不是运行验收。`gh run list --branch fix/handover-live-publish`为`[]`，无CI run ID，无boot pack/设备/二次检查。
- **新主线/下一集成**：编译期间main新增sensor生产`b6dad31/bfde8ca`及merge，不是仅docs；保留这些更改rebase后合入本项，下一含号码owner/SMS新修复的统一固定源码kernel再编译，不把旧kernel当新main全集成。IMS ABI冲突已由同版原件证明是分析分支读反：normal为`1007cb970→ims::Call+80`，`1007cbdcc+68`是EmergencyCall分支。root/OS20H392 CommCenter原件相同，root cryptex/sysos MAIN cache UUID同`8f970452-c9e3-3e88-b114-feb1864bc9e0`；不混版本ABI。VirtualAudio root/os的LC_UUID同`3196333F-5A48-373B-B427-0146DC1177FB`，独立voice契约原件有据，但未声称当前设备已接DSP。三代理继续号码epoch owner、typed IMS model和SMS owner，不重复已排除固件引用类。
- **live54合入/清理已完成**：保留新sensor main后无冲突rebase，生产为**`5ece97f`**、交接`be9610f`，已ff main并push origin/main及remote功能分支。`handover-live-compile/`和`x9-call-handover-live-owned-20261115/`完整迁主checkout同名out，原日志中的编译树绝对路径不改写；本地live54树/分支已清。纯分析CallList与call-audio树经代理确认无依赖后，分别完整迁主checkout`x9/out/calllist-modem-producer-analysis/`、`x9/out/call-audio-analysis/`，空分析树/本地分支已清，其它窗口不动。
- **当前统一树/待提交生产**：`.worktrees/call-sms-owner-integration` / `fix/call-sms-owner-integration`从实际main`49a041b`开始，先将真实typed IMS/UUID和独立Baseband Voice契约写现有`docs/call-ios-bridge.md`，没有hook或VOICE公告。`fix/call-identity-owner`正在补已准入完整progress→原MO/MT epoch的bounded raw number/detail/TOA/PAU owner；RIL显示有PAU优先与旧number保持，不能直接把progress raw号当最终显示，current号不倒填MT。`fix/sms-publication-owner-review`已发现另一个窄可达queue-loss：同token物理ACK结果后，WMS37 completion仍legacy void-send即标answered并擦owner，队列满/OOM可丢host结果；正准备仅ACK reply的真实receipt/保留，不重发MD ACK、不把它称Messages落库。两项尚未交付/合入当前kernel，统一编译结果后续更新。

#### 2026-11-15 通话raw身份 / WMS37 completion owner统一交付

- **固定源码与生产**：在上述统一树保留实际main **`43dd4b26`** 的sensor修改后rebase，raw身份来源`c7008018`（原集成3143ca5b）成为 **`bf96e0dc`**；短信来源`ec56e429`集成为 **`8c6a00a9`**。源码与原提交range-diff/实际diff核对，不改其它audio/GPU/BT/camera/sensor活动树。此前“待提交”以本段更新，文档契约提交`3d1edef5/2e3c1eda`，没有生产IMS hook。
- **raw身份准确边界**：完整4901 parser有界保存408B detail、PAU≤512B含NUL；仅原MO/MT reducer准入成功的commit附上同epoch/revision的raw number/presentation/TOA/detail/PAU。新MT CMD清旧owner，foreign/late/rejected/revision失败不改，legacy valid0不授parsed provenance；current独立owned-query不倒填MO/MT。这不是最终显示号码，不发CallList/VOICE09。RIL `26931c/269c44→26e000→270120` 的ECPI/PAU/旧号码融合受slot键178/179/180控制，不是固定PAU优先；真实writer `loadConfiguration21f020→libcarrierconfig.so getKeyCount/getValuesByMccMnc→slot setValue`已闭合，保存1761 maps证明库映射，但仓内缺库实体/三键live值。IMS显示TOA为含`+`的0/1、presentation为空1/非空0；current/CS才用native TOA/8114/detail0，不混codec，不公开restricted/unavailable/未知presentation原号码。
- **WMS37真实receipt**：原ACK job捕获独立local generation和真实BB/QMUX generation；BB锁内校验14B WMS37 terminal reply的原route/service5/node/used/non-release_pending/generation/queue容量，真正入队才answered。OOM/full保原req/slot/txn/首次result，仅重试本地回复；ack_done使物理reply_take仅一次，物理owner退休后不重发ACK/复投PDU。delete/lost-generation不回新client，仍drain旧physical token。未决定deadline在拿MD锁后再判原64-bit时间；及时physical success不因queue延期变timeout，先timeout不被late success覆写。same-slot/localgen/txn的原job消费identical及changed/malformed reuse，不再第二BAD/ARG或MD ACK；validator前scoped入口仅现存WMS37 owner，其它请求仍原校验。receipt只打印token/SIM/txn/result一次，明确not durable storage proof、无PDU/号码。
- **统一编译结果**：固定HEAD **`8c6a00a9003f5c6676ff464c201c6534c41e8d39`**，CALL与SMS host C++17 `-Wall -Wextra -Werror -c`、C11 header `-c`均 **exit0**；两真实recipe对象均 **True/exit0**。短信58项向量及通话新增3项均只编译未执行。执行 **`python3 -I x9/build/build.py kernel --name x9-call-sms-owners-20261115`** 正式 **exit0，98/98 overlay（3 prebuilt）、link/strip成功**。产物 `x9/out/x9-call-sms-owners-20261115/kernel.development.sweet`，源码/console/exit与recipe脚本保留在 `x9/out/call-sms-owner-compile/`；不是boot、不运行套件/设备、不二次解包/hash/反汇编。`gh run list --branch fix/call-sms-owner-integration --limit 3 --json databaseId,status,conclusion,headSha` 为`[]`，无CI run ID。
- **typed IMS新闭合与剩余**：normal创建实际ims::Call/Apple UUID/Personality client；CallInfo+40为PhoneNumberType、+44为PhoneCallSubType、+57为Is Listed As Emergency（不是video），真实CallConfig保opaque。delegate按client/name强shared_ptr owner与原queue/weak-lock，不能裸指针/global UUID表。typed hangup经UUID找到Info+80实际BambiCall，最终模型mutation/MD原因映射仍缺；ACTIVE门`1b7b30944`要求event+10等于实际BambiCall，且+140 MediaSession→+280→virtual+98非零。不能仅替Bambi factory或直接ACTIVE冒充媒体接通。原件材料 `x9/out/ims-call-provider-contract/`，目前代理继续其中client initialize、真实网络注册回调和destroy/unregister生命周期，初始化ready不冒充网络状态。
- **并行/完整目标**：复用三个代理，继续typed初始化/注册、typed最终状态consumer与内核NAS订阅reply owner；CallList新增真实CallType日志锚`c1ee2aa8/raw1c7daa8`的窄指针引用也为0，不再重复已排除扫描，不猜svc09→46/offset6。通话专用scene0/MD3D00/AFE/Aurisys与iOS双向voice仍未闭合，现存313–321没有同token MT→ACK→Messages存储/重开或MO对端收到样本。**VOICE09仍关闭，完整goal未完成、无“最终修好通话短信”boot.img**，不操作手机、不要求10086/来电、不出探针。

#### 2026-11-15 NAS03初始化订阅receipt / typed注册契约（本轮）

- **本轮树与范围**：`.worktrees/call-nas-init-integration` / `fix/call-nas-init-integration` 从实际main `30a0a313` 开始；typed初始化/注册/注销与HAL owner文档 **`696cc6cc`**，NAS03生产来源 **`1a5fd1c50f16c6d09832bff4131703c922e877b1`** 集成为 **`de8ddd4b`**。三个现有子代理分别只追typed最终状态、独立peer-review及NAS/SMS源码；不操作手机、不要求10086/来电、不出探针、不覆盖audio/GPU/BT/camera/sensor窗口。
- **NAS03生产修复**：原订阅flags/next在legacy void-send后直接生效，OOM/full可丢注册响应却启动后台查询。新pending32、每client独立last-completed cache保留slot/localgen/真实QMUXgen/txn/首次result，完整TLV准入后保存mask/values；真正host queue receipt后才提交CURRENT flags的原子patch/next。同client FIFO；exact completed duplicate只回原结果、不重apply，changed/malformed same-txn由原owner消费，无第二回复；合法新txn成功准入才清cache，ARG/BUSY不抹cache。DEL/gen变化退休，pending换SIM BUSY，成功换SIM清cache；UINT64_MAX准入BUSY防order回绕。disable receipt后旧background NAS24/4d/4f自身IND以当前localgen/SIM/对应flag gate，物理token照常drain，foreground与共享signal fanout不扩改。OOM/full仅重试host reply，不发MD命令、不赋网络ready、不虚构deadline；pool-full BUSY/ARG仍是原普通回复边界。
- **已完成统一构建**：固定源码 **`de8ddd4b4d261f654f74135d2663b1bb02dfe28e`**，CALL/SMS/NAS三个host严格C++17 `-Wall -Wextra -Werror -c` 均exit0，两真实recipe对象均True/exit0；执行 **`python3 -I x9/build/build.py kernel --name x9-call-sms-nas-init-20261115`** 正式 **exit0，98/98 overlay（3 prebuilt）、link/strip成功**。原console/exit/HEAD/recipe脚本在本树 `x9/out/call-nas-init-compile/`，kernel在 `x9/out/x9-call-sms-nas-init-20261115/`。所有人工向量只编译未执行；没有boot pack/上机/二次检查，不将kernel-only标为最终镜像。此基线尚未合main/push，CI查询与收尾另记。
- **独立peer发现待修**：新NAS03 owner双generation、BB release屏障、唯一QMI线程、FIFO/cache/重复txn未见另一具体阻断；相邻旧NAS6c仍直接 `c.signal=true;c.next=0`，可绕过tag19=0的已receipt关闭，与“6c仅threshold配置，tag19独立订阅”注释冲突。已交原源码代理依据现存Apple原件窄修；新生产提交将使用新的固定HEAD/唯一kernel名，不复用上述候选标签。BB通知只完成host read/Mach通知，host write/release排队到下一owner poll，不直接重入send/del，这是receipt到flags commit的串行调用链证据，不是运行race验收。
- **typed初始化/网络注册已静态区分**：真实per-personality Client由factory+38创建，controller+d8保存，StackDelegate为controller+40 alias/强shared_ptr/controller+18 queue；initialize使用model真实704B StackConfig、personality与CFDictionary，阶段f8=1/2不代表注册。独立RegistrationState为0/1/2/3 Deregistered/Registering/Registered/Deregistering；真实64B ActiveInfo含service mask/URI/stackId/expiry差值，服务mask经实际DataMode策略而非IMSA bool→ffff。真实模型锁内保存→root per-personality registered/mask/DataMode→observer map已闭合，后续CT/UI与MD-backed输入尚缺。失败20B Info保retry/SIPcode/reason，注销实际进入Bambi/SipStack并等uninitialized回调，factory deleter只有ret，release不等于delete；普通controller removeStackDelegate/销毁全链未闭合。材料 `x9/out/ims-call-provider-contract/` 收尾前仍由peer使用，不提前搬走。
- **voice owner纠正/完整边界**：Baseband Voice保存真实AudioObjectID，经HAL `stm#` / `inpt/outp`枚举和真实stream wrapper；device+240是CT singleton shared owner，不是MD DMA/media指针。CreateIOProcID helper可检查err/nonNULL，但其完整voice caller/硬件provider未闭合；静态材料已实际归档主checkout `x9/out/voice-provider-analysis/`，临时voice树已清。上一轮raw身份、WMS37及统一owner树/out收尾已完成。VOICE09仍关闭，CallList producer/offset6、真正IMS→MTK运行拨号、handover身份迁移、通话专用双向audio、Messages持久化/重开/MO对端收到仍未完成；**完整goal保持active，无最终通话短信boot.img**。
- **6c窄修已完成/peer收束**：Apple原件 `1f1ab58d0/58dc/58e8/58f4` 明确6c是threshold/delta/LQI，独立03 `6200/620c/6210/6238` 才tag19 SigInfoEvents订阅。生产来源 **`1e45a53341f37d87a808956d9a9ecf07d7d28ef1`** 集成为 **`a4ea2213`**，删除6c的signal/next隐式mutation，保原ACK但日志明确配置未应用/NAS03不变；没有加兼容stub或重写MD。独立peer末版无另一具体阻断，限定静态结论而非设备验收。与main `c9511461` 的无冲突rebase只带入sensor交接，NAS03成为 **`fbfb308d`**、typed docs `0c45b77b/fe32ef76`；range-diff确认NAS两原/集成production patch相等，原de8构建HEAD不改写。
- **末版实际构建**：固定源码 **`a4ea22137a7c42a5232038d24bd68a28a5541b88`**，最新NAS32组严格host `-std=c++17 -Wall -Wextra -Werror -pedantic -c` exit0；代理另仅编译out抽取的实际6c branch四组exit0、实际Baseband recipe exit0/34旧warnings，仓内host不依赖scratch/Apple。执行 **`python3 -I x9/build/build.py kernel --name x9-call-sms-nas-init-final-20261115`** 正式 **exit0，98/98 overlay（3 prebuilt）、link/strip成功**；日志/exit/HEAD在 `x9/out/call-nas-init-final-compile/`，kernel `x9/out/x9-call-sms-nas-init-final-20261115/kernel.development.sweet`。名称final只指本NAS子项收尾，不是完整通话短信最终镜像；仅kernel，无boot/设备/向量执行/二次检查。原统一CALL/SMS host与两recipe对象已exit0，未为后续纯文档重编。初次 `gh run list --branch fix/call-nas-init-integration ...` 为`[]`，无CI run ID；push/收尾另记。
- **typed权威状态已闭合/纠错**：原三层command delegate安装到driver+88/+90并沿weak CallController owner/真实UUID找到Call shared_ptr；UUID真实consumer **`100693d28`**，旧100694eac/100696fcc与旧END10068f434标签废止。`4Call` ctor分配0x68 FSM并由Call+338拥有；真实CallActive/CallEnded表由ctor填BSS，不因磁盘零指针判无handler。FSM state7/12或5、SRVCC Call+a4与CT CallState不混用：两层status wrapper **`10158af20→10158b148`** 进入nested弱owner，`101018528→101005398→101018718` 明确写 **Call+a0=0 Active**；END `101018e18→101018f54` 写 **Call+a0=6 Disconnected**，`101019424`再做同一状态维护，旧END CTState8撤销。真实libCommCenterBase table `20dad4e78`与最终store双证据；Call+2d4可写后提前分支，通知仅收束到owner+170 sender virtual+10，未冒充每次通知/UI/media成功。DisconnectInfo category→MD cause仍未闭合，不造映射。
- **分析实际归档/下一最早缺口**：三个代理均停写、tracked clean；其完整out已无覆盖迁主checkout **`x9/out/calllist-handler-closure-analysis/`、`ims-call-provider-contract/`、`nas03-peer-review/`、`init-registration-owner-review/`**，对应本地三树/分支已清。必要精确结论/旧错误警示/复读命令写入现有docs，新增私有closure-summary.md已消费后删除，不保留新过程报告，原asm/表/脚本不删。root窄voice direct-ref材料保留 `voice-io-owner-analysis/`：六个CreateIOProc helper直接BL caller及322cc8 owned record有据，但尚未建立完整Baseband Voice IOProc caller，不将旧voice-start-engine文件名当真实函数身份。下一步从已证typed Client/StackDelegate注册与销毁缺链建立可实现的MD-backed provider契约，补UUID↔epoch/native-ID与真实call/media生命周期；同时沿真实WMS消费者证明Messages存储/重开。不要重复零命中CallList handler/offset6猜测、再要10086/来电或把NAS本地receipt称网络ready；完整goal未完成。
- **本轮实际合入/push收尾**：生产 `fbfb308d/a4ea2213`、契约与统一交接 **`79506dce`** 已ff合main，**origin/main push成功 `c9511461..79506dce`**；功能remote `fix/call-nas-init-integration`也已push，push后gh查询仍`[]`，无CI run ID。root完整out已实际无覆盖迁主checkout **`call-nas-init-compile/`、`call-nas-init-final-compile/`、`voice-io-owner-analysis/`、`x9-call-sms-nas-init-20261115/`、`x9-call-sms-nas-init-final-20261115/`**，日志原工作树绝对路径不改写，固定build HEAD仍a4ea2213；没有为文档/合入重编、运行套件、pack boot、上机或二次检查。本条正常提交合main/push后只清本轮root树/本地分支，remote及其它audio/GPU/BT/camera/sensor树、旧镜像和`.local`原件/备份不动；完整goal保持active，VOICE09仍关闭，未交付最终通话短信镜像。

#### 2026-11-15 typed生产接入 / cleanup退休 / 短信持久化消费者（本轮）

- **本轮范围/并行**：root `.worktrees/call-provider-integration` / `feat/call-provider-integration` 从实际main **`a6854c83`** 开始；按用户要求复用三代理分别追typed Client生命周期、iOS短信消费者、MD通话专用DSP握手。只读原件/既有日志/源码，未操作手机、要求10086/来电、发探针或运行人工向量/测试套件；没有新增过程Markdown。现有 `NOTES.md` / `docs/call-ios-bridge.md`统一更新，未触其它audio/BT/camera/GPU/sensor源码窗口。
- **typed完整接口/真正拨号入口**：实际Client address point **`1d90b86d8`** 44槽无dial，legacy `101531058`不是这张表。`Call` ctor **`1b7c412a8`**直接copy ClientHandle+10 clientId→Call+28，不保存该client指针；正常dial的MainQueue block **`1b7c41878`**取IPTelephonyManager+90，并在 **`418cc`直接BL `IMSCallManager::initializeNonEmergencyCall1b7b32708`**。实际Cryptex cache完整140B与原件一致；只替Client factory/vtable不会转MD。per-personality Client/StackDelegate只是初始化/注册子边界，还须正常CallFactory+40、typed Call/UUID ingress/action/epoch/native-ID/state/media生产接入。当前x9c_dial_submit无生产admission caller，exec注入不覆盖CommCenter、构建无call用户态provider；未补空stub/ready常量。root原asm/44槽注释在 `x9/out/call-provider-integration/`，固定参考窄查未发现可直接复用typed provider，不泛称所有实现不存在。
- **cleanup关键纠错/退休边界**：此前 `+b0 removeStack / +c0 removeDefault`是手算offset错0x10，不是版本/提取ABI。原cachector76B、vtable352B、SD cleanup调用44B实证 **`+98 addStack / +c0 removeStack / +d0 removeDefault`**；**普通cleanup确实调用removeStackDelegate**，旧相反结论撤回。该调用只dispatch_async；nullptr分支擦wrapper weak/queue map后绕过SharedHolder stack强map，不撤销已捕获weak callback；samekey强map是emplace非替换。uninitialized仅阶段0，factory deleter仍ret；真Client dtor才同步销record/maps再异步invalidateStack，不以remove返回/weak.lock当队列fence，不称原件UAF。修订JSON/自动表/原cache切片已实际无覆盖归档主checkout **`x9/out/ims-client-lifecycle/`**；代理停止写入，raw与root新表经独立peer44槽核对相等。
- **通话专用voice纠错/缺输入**：HAL ioctl40046914填task-info每项8B；固定双核参考scene0→HIFI3_B(enum3)→physicalcore1，DT core-num2/mbox2,3/B-pool有据，但本次task-info[0]及DSP consumer未回读，不把参考路由/Aurisys core_type冒充runtime。REG_DMA两个65536B是AP↔DSP region，**不是已证MD PCM UL/DL环**；header128/entry384及ADD0/ADD1契约闭合，公共descriptor ABI可共用，不复用SmartPA coreA/scene/单环尺寸与owner。CCCI4 wrapper与另SPC stream AFxx mapper分开；仅两个窄MD range未见3D00 immediate，不泛称全MD无实现。X9Calls无通话audio生产调用，缺实际qualified core/pool owner、MD3D00 payload/同版DSP ACK、PCM地址/权限/codec节拍；无据不补generic audio。raw asm/切片/JSON已实际无覆盖归档 **`x9/out/call-voice-md-handshake/`**，代理及独立peer均已停写。
- **短信新消费者/保存决定分支已闭合**：WMS01 TransferRoute parser `1f192cfb8`与当前ack/u8、token/u32、format/u8、len/u16、bytes匹配，List `1f192d720` count/u32与index/u32,tag/u8亦匹配，未见具体codec mismatch。实际MsgPduProcessStatus **4=SmsSuccess、1=MemoryFull、2=ParsingFailed、9=CMASSuccess**，不把status或WMS37 receipt称持久化。SMSServiceSession→CTMessageCenter incomingMessage→payload写盘→didReceiveMessage→IMDServiceSession→_IMDStoreMessage→IMDMessageStore.storeMessage→IMDPersistence record create已闭合；DB server分支 `1a8ae3380→AddMessageRecord1a8b5467c→54740 AddRecord→54748 SaveStore1e75f7668→7698 transactionType1→facb0 Commit1e75ff1f8`，实际autocommit=0才ff228执行COMMIT并sqlite3_step。save result0/101转bool回AddMessageRecord，**3384据此决定null或真实RowID**；XPC completion失败null也调用，didStoreBlock的w1是store之前decision，均非commit receipt。无pending/already-autocommit等早分支保留，不以callback或字符串命中称当前成功。当前root Cryptex cache只读12×12B支岛＋32B CFString＋7B SQL，与旧采样逐字相等；旧sysos mount不再权威，原offset/byte/provenance在JSON/txt。全部raw/selector/事务边界已实际无覆盖归档 **`x9/out/sms-ios-consumer/`**；三个代理及peer均停写，无真实DB文件读取、设备、测试或编译。
- **准确构建/整体边界**：本轮没有生产行为修改，未编译、pack、二次检查或操作设备。同期main已推进 **`4087f82f`**，相对本轮基线只新增整包交接，源码未变；另窗口正常main候选 `x9/out/x9-main-full-a6854c83-20261115/boot.img` 的source仍a6854c83，不是本轮新产物或“最终修好通话短信”。NAS kernel source仍a4ea2213/exit0，不改写历史构建HEAD。VOICE09仍关闭，CallList producer/offset6、真正typed→MTK拨号/handover身份迁移、双向voice及Messages当前真实commit/重开/MO对端收到仍未完成，**完整goal active，不交付最终通话短信镜像**。下一最早生产缺口是上述多面typed ingress及真实MD/media owner，不重复已排除ABI/空handler/ready/profile/CCCI猜测。typed/voice契约 **`dfedf44c`** 已rebase保留同期main交接并push `origin/feat/call-provider-integration`；gh该分支查询`[]`，无CI run ID；本次SMS/docs完成后正常合main/push，仅清本轮四树/本地分支，原件/备份/其它窗口不动，归档/push结果另记。

- **本轮实际归档/合入收尾**：typed/voice **`dfedf44c`** 与短信存储契约 **`7f27d31a`** 已ff合main，**push成功 `4087f82f..7f27d31a`**；功能remote同步成功，push后gh仍`[]`，无CI run ID。四组完整原asm/切片/JSON/复读工具已实际无覆盖在主checkout **`x9/out/{call-provider-integration,ims-client-lifecycle,call-voice-md-handshake,sms-ios-consumer}/`**，不改写旧采样绝对路径/字节、不新增过程报告。三个代理均已停写，子树/本地分支已实际清理；此最后交接正常合main/push后只清root本轮树/本地与已完成feature remote，旧NAS remote、其它活动窗口、镜像、`.local`原件与分区备份不动。本轮是新的生产接入/生命周期/SQL决定分支证据与旧错误纠正，**不是生产功能修复/新镜像/物理验收**；完整goal保持active，不要求用户再补10086或来电。

#### 2026-11-15 注册真实discriminator修复 / CallFactory与handover身份消费者（本轮）

- **范围与并行**：从干净main **`be107d68`** 建root `feat/call-production-next`，复用三个代理及各自功能树分别追typed factory/action、IMSA/SMS初始化、MD/USIP voice；其中voice代理再独立复核root身份迁移。没有设备操作、要求10086/来电、探针、人工向量/套件或新过程Markdown；不触其它audio/BT/camera/GPU/sensor工作树。既有原件/帧静态分析与正式编译允许，构建后没有二次解包/hash/反汇编/比较。
- **本轮唯一生产修复**：代理原提交 **`ea37ce248c3b98d76b1a2da2613adf5aba66886e`**，root整合为 **`a608d83460ea1ee24f05e27d01143be52f46ac2f`**；`X9Messages.inc` 4a01必须tag100/u8=0才解析CIREGU101/bool。原RIL `290e18..290ecc`证type1 WFC/type2 RegInfo/type3 RegResp共用消息，不能混注册state。真实retained-radio line5157/5321是type2：104 account_id、8105 URI、106 expire_time，**255不是service mask**；type3另owner report_type2/account104/error107/reason108，core expiry=-1及code/reason。真实type2无101旧代码已忽略，本项是原dispatcher所证parser qualification缺口，不虚构已有运行错误state。未添加死metadata/猜mask、account→stackId、脱敏URI、expiry clock或ready/epoch。
- **typed工厂与导出新契约**：真CallFactory vptr **`101532938`** /正常+40 **`10019c234`**，0x60正常分配=control0x18+Call0x48，release真Call virtual destructor，不是Client ret-deleter。UUID owning输入CFRetain→CFUUIDCreateString；`100b93568`创建factory→controller+48/+50强pair→factory自身weak锁→driver+328/+330同强owner；Registry插CallControllerInterface，不是factory。正常`1007cb918 __dynamic_cast(ims::CallHandle→ims::Call)`成功才virtual+80，不仿裸vtable。当前根盘Cryptex原export trie实证C1 **`1b7c41390`**导出并B到C2`1b7c412a8`，Call RTTI/vtable也导出；Call D0/D1/D2没有export，不能以本地nm符号声明可动态导入析构。原Call dial仍直入IMSCallManager。此HEAD无call用户态provider builder/CommCenter loader/用户态callable桥，`x9c_dial_submit`仍无生产caller，request无VOICE20 admission；只加loading或真实C1不能称拨号转MD。18/13/25/6槽表与未解managerdelegate+10准确保留。
- **handover实质新身份消费者**：正确QMI库 `libCommCenterMCommandDrivers.dylib` 的outer `1f1ac3644`取**tag1f IsHandoverCallList**：count/u8+2B条目，ldrh>=100hex即item1非零，以item0键建bool map；不是七byteCallList offset6。独立**tag20 ParentCallInfo**条目3B，item1旧ID键/item0新ID值，thirdbyte未解；delegate+148→`1f1b2098c`→inner+1e8→root`1006965c4`，按真实Call+140选SimSlot，**ldrb Call+c4**查旧ID，命中后`101049ae8 str w1,[Call+c4]`写新u32 ID并原通知，缺键保留旧ID。SRVCC CallInfo copy+driver strong async→QMI wrapper+a0`1f1b20704`→root+d8`1006949ec`，QMI factory wrapper强owner→driver+78/+80安装有据，不混此前IMS三adapter。root真实UUID lookup/fallback谓词 **Call+c4==CallInfo+5c && w2!=0 && Call+2f0!=1**；w2=0跳过全部、2f0语义不猜。`10100a698(controller,&sharedCall,0)`不是CallInfo ID setter；无owner/no-calls/wrong-driver有清理/拒绝，不凭通知造Call。root当前cache14处有界原代码/表读取均与提取原件相等，独立peer四处8B表及branch/setter吻合；仍不是当前MD交接后映射producer、typed运行流或功能验收。早期scratch `final-srvcc-consumer.asm`等错误命名在JSON明确撤回，不据文件名给函数身份。
- **voice分层纠错/真实控制闭合**：HAL真open `/dev/usip`并GET config/hwif ioctl`c00844f1/f3`，参考copy u32不是8Btask-info。固定`mtk-usip.c` open发布 **AP AD50**，不是MD3D00：header16B+两u64`{USIP_AP_PA+30000hex,30000hex}`，BYPASS_ACK，main0及enh bit1时sub19；成功发送不是DSP回执。实际LK ID24 AP=f0d00000/MD=42d00000/size60000/flags2，当前内核skip映射/清零，ap_off=ffffffff不是NCoffset；范围同时含PHONECALL8000/SMARTPA28000，不借SmartPA coreA/ticket，不盲clear/map/ready。另真**MD2f20/sub25/128B配置→MD body→Spc_SpeechOn→SP_SpeechOn→af20**原件与Android拨出af20 result0闭合；仍不证明PCM/声音。最早缺同版AD50 DSPconsumer、实际8Btask-info/core与独立USIPlifetime，MD3D00/D300/ULDLPCM仍缺。
- **SMS初始化准确边界**：原emitter→callback：3a DCDisconnectTimer、39 RetryInterval、38 RetryPeriod；成功/失败日志不产ServiceReady，未证unsupported配置停止初始化。现存Android4601→601/602→SMSC query形状与现实现吻合，无MD timerwriter依据不补fakeACK；当前同token Messages commit/重开/MO对端仍无新增runtime，未编造终态修复。本次SMS第二次复读无新生产变化，不为此重编。
- **固定编译/产物**：代理actual `X9Baseband.cpp.o` recipe exit0/34既有warnings；root固定源码 **`a608d83460ea1ee24f05e27d01143be52f46ac2f`**执行`python3 -I x9/build/build.py kernel --name x9-call-ims-type-guard-20261115`，正式 **exit0，98/98 overlay（3 prebuilt），link/strip成功**。仅kernel，未bootpack/设备/套件；产物 `x9/out/x9-call-ims-type-guard-20261115/kernel.development.sweet`，源码HEAD/console/exit/JSON在`x9/out/call-production-next-compile/`。`gh run list --branch feat/call-production-next`为`[]`，无CI run ID。不把旧NAS/a6854c83正常main整包改称本轮产物。
- **当前完整边界/下一步**：本轮有实际parser窄修和新的工厂/ID迁移/voice控制证据，**完整goal仍active**。VOICE09继续关闭；typed→MD真实DIAL loading/admission/profile、七byteCallList offset6/完整身份producer、MDhandover后同epoch映射/CS动作、通话专用双向媒体及短信当前Messages持久化/重开/MO对端均未验，**无最终修好通话短信boot.img**。下一从已导出的真实C1/owned factory和真实ID setter设计完整可达用户态→内核接入，不重复以Client工厂、unsupported短信timer、AD50/2f20回执或空stub代替最终状态。原始材料停写后统一无覆盖归档主checkout，再合main/push并只清本轮四树；实际收尾另记。

- **本轮实际合入/归档收尾**：生产`a608d834`及契约交接 **`0eb47eb2ef37b1c374b5933d4a3c6aeb05b0f41b`** 已ff main，**push成功`be107d68..0eb47eb2`**，remote `feat/call-production-next`同步成功；push后gh仍`[]`，无CI run ID。六组目录已完整无覆盖迁主checkout **`x9/out/{call-typed-actions,call-init-production,call-md-voice-owner,call-handover-typed-owner,call-production-next-compile,x9-call-ims-type-guard-20261115}/`**，原私有日志绝对路径不改写；build HEAD仍a608d834，不为docs合入重编。三个代理均停写、tracked clean，三子树/本地分支已实际清理；原ea37ce24和a608d834整树diff为空才清原生产分支。本最后交接合main/push后清root本轮树/本地分支，feature remote、其它活动窗口、既有镜像、`.local`原件和分区备份不动。正式kernel只覆盖注册discriminator子项，VOICE09关闭，完整goal active；**没有最终通话短信boot.img，不要求刷kernel-only候选**。

#### 2026-11-15 typed拨号上下文 / UUID过滤 / 原broker与loading入口收束

- **范围**：基线`dfc67ca9`，root `feat/call-native-ingress`；按用户要求复用三个子代理分别在`call-typed-dial-context`、`call-md-profile-owner`、`call-runtime-loading`收束typed参数/Android策略/真正loading，loading代理再独立核对root ATCS入口。不操作手机、不要求10086或来电、不出探针、不运行套件/人工向量、不新增过程Markdown。**本轮无生产行为修改或编译，不能称通话/短信修好。**
- **typed真实输入及owner新纠正**：代理文档`0eb94bec15e253e856bfb150ed3df7c5f7773e0b`已cherry-pick为root **`0ae4ae0e`**。`Call::dial1b7c41468`五参真实calledAddress/CallConfig/additional/bool；MainQueue capture+20独立owned号码、+38独立0x13a0 options，UUID/clientId/两privacy byte/text/relay和全部owned配置必须保留，不能让stack指针逃逸或固定CLIR0。model+80/+88是真command delegate强owner，**不是typed CallHandle**；typed pair是model::dial形参，由原factory同control传入。delegate登记key **`ims.call.`+clientName**，不是callUUID；wrapper在client record内broadcast，最终`1007c4490→三层delegate→100693d28` weak-lock原CallController、扫Call vector并CFEqual UUID，match才copy CallInfo，失败不做ACTIVE同步/通知。8处当前cache小切片及真API export契约在`call-typed-dial-context/`，不把wrapper broadcast当定向lookup或void dial当准入成功。
- **Android真实生产策略**：同机cf33普通IMS→requestDial381410明确emergency0/video0/IMS1；六TLV实际为8100/string、108/u8、101/102/103/104/u32，101=2/102=1/103=0有producer依据。108需原owned Dial+8经Rtm key124/Rmm策略；key125比较**`true`**非carrier。104逐次依赖MCL72/31，普通cf33不走该ECC reset caller，不能默认0。Rtm构造临时copied request但实际requestToMcl发送原request引用，原ownedCLIR已被先改写；不误写成临时copy被发送。完整CallConfig/Apple privacy→策略映射仍缺，当前encoder无新已证错误，不加unused profile API。原log/13份numbered asm/`dial-profile-contract.json`及只读extract脚本在`call-md-profile-owner/`。
- **已有用户态→内核传输准确边界**：`libATCommandStudioDynamic`真导出`qmi::Client::createWithQueue19ecd6b20/19ece432c`，使用原 **`com.apple.commcenter.atcs.xpc`** broker；Client::send→State::send序列化QMIServiceMsg及原send_qmi_message/send_timeout_ms，shared QMux按service/cid保存ownedMessage。Router真实key=`sim`，samekey替换强pair，get缺key抛out_of_range，无默认卡。SendProxy借Client对象地址，只强制借用期间稳定；async另持State/capture，不猜所有callback仍借Client。原ABP type0 selector2/write→bb_host_write→cbq_from_host→bb_queue_tx→poll→x9bb_modem_send→x9q_send→x9c_request已在源码，channel open第二owner返回exclusive；**不能再泛称无用户态传输，也不能新增raw reader竞争。** 319:7405/7414/7417/7418/7435/7437只证明原QMUX/WMS执行，bootSHA未知，非新typed拨号运行。12处当前原cache切片相符；独立peer核对21条所选export/5处新owner原字节。ATCS至libIOABP全virtual边未全部新闭合，09仍未公告、20无生产caller；`native-ingress-contract.json`/原export/复读脚本在`call-native-ingress/`。
- **真正loading已证、非新增空loader**：当前root launchd cache/plist启动原CommCenter/_wireless，无call注入环境。原dyld5a→Security→DYLD_INSERT_LIBRARIES owner→getLoader→initializer list→imageBase+offset→BLR constructor已闭合，16处当前原cache相符，现sweet_amfi真ABI已返回ff，**无依据再改CS_RESTRICT/AMFI授权**。原CommandDriversFactory只vendor选固定M/K并返回强command-driver owner，不是任意插件目录/独立CallFactory入口。完整provider具备后才builder→签名manifest→PID1原子发布→精确exec注入，不先造空loading。此代理全部材料及`independent-atcs-review.txt`**实际已在主checkout `x9/out/call-runtime-loading/`**，不是其worktree，不重复搬/覆盖。
- **当前最早未完成与下一步**：根据新UUID/filter与true exports，将原Personality/client/有效注册stack初始化、完整typed配置语义、Android真实privacy/preferred-domain策略及broker/logical-client owner合成一个**可达生产provider**，随后才能接VOICE20 admission和UUID↔epoch/nativeID。不能让原Call仅继续进入未适配的AP SIP manager，不能用构造/加载成功、假ready或广播ACTIVE绕过实际CallInfo过滤。handover后ID/RAT/CS策略、CallList七byte offset6/producer、通话专用双向媒体及当前同token Messages commit/重开/MO对端验收仍缺；SMS无本轮新runtime，原IMS discriminator kernel source仍a608d834/exit0，未为证据重建。**完整goal active；没有最终修好通话短信boot.img。** 收尾正常提交合main/push，四组out无覆盖归档，只清本轮四树/本地分支，其他audio/BT/camera/hevc树、长期输入/原件/分区备份与旧候选不动，实际结果另记。

- **实际合入/归档收尾**：typed契约 **`0ae4ae0e`** 与broker/profile/loading交接 **`4bb5d7fa`** 已ff合main且 **push成功`dfc67ca9..4bb5d7fa`**，remote `feat/call-native-ingress`已同步；`gh run list --branch feat/call-native-ingress`返回`[]`，无CI run ID。root、typed、Android策略三整目录均已实际无覆盖迁主checkout **`x9/out/{call-native-ingress,call-typed-dial-context,call-md-profile-owner}/`**，loading及独立peer本来就在 **`x9/out/call-runtime-loading/`**，未重搬。原采样绝对路径/字节保留；脚本支持从归档位置复读固定原件，未为迁移重跑分析/构建。三代理均已停写，三个子worktree/本地分支已实际清理，原`0eb94bec`与整合`0ae4ae0e`全tracked树diff为空后才清原typed分支。本最后交接正常合main/push后仅移除root本轮树/本地分支；remote、其它活动树、旧kernel/boot、`.local`及备份不动。**本轮没有生产功能补丁、新镜像或通话/短信端到端验收，完整goal仍active。** 下一步按已证owned client/配置/原broker实现真实可达provider，而非重复查已闭合的登记key、dyld授权或六TLV默认值。

### 5.7 2026-11-13 音频：ADSP 自动 dsp-boot 镜像（分支 feat/audio-dsp-boot，worktree `.worktrees/audio-dsp-boot`）

- feat/audio-bounded-fdt 已 ff 合入 main（`b25f4ca`），worktree/分支已删；`android-callsms-20261113`、`shimtest`、fdt2/3/4 的 usb-log 移到本 worktree 的 `x9/out/`。
- **boot-args 腾空间（用户：修好的 bootarg 默认开启）**：`x9_submit_async`、`x9_gpu_park` 改为内核默认 1（各只有一处 `sweet_x9_arg`；PanVK 只查询内核能力），
  从 `boot-device.json` 去掉，571/607 B。`=0` 仍可关闭。
- **`x9_dsp_boot=<1..600>`**（仅 `x9_audio=2` 时）：X9Audio start 后起一个内核线程，延迟 N 秒执行**一次**与 `audioctl dsp-boot` 相同的路径
  （audioLock → `x9_adsp_mt6993_control(0)` → transport `x9_adsp_boot(0)`：power/reset/secure RELOAD/CORE_START0 → READY ≤2 s），不重试、不自动 stop，
  之后打印 `X9 AUDIO DSPBOOT: result=… board_err=… flags=…`、`votes …`、`smc …`、`dsp_err=… state=… ready=…` 四行。开机时打印 `X9 AUDIO DSPBOOT: armed|NOT armed`。
  设备没有 iOS shell，所以用这个代替 audioctl（用户选定）。
- `build-usb-prelink.py <name> [--boot-arg TOKEN ...]`：额外 token 同时传给 kernel 与 boot 两步；main 的 boot-device.json 不含音频参数。
- **镜像（未上机）**：`x9/out/x9-dspboot1-20261113/boot.img`（三步 exit 0；payload 含 `x9_audio=2 x9_dsp_boot=60`，597/607 B），**无需 mailbox**。
  这是第一次真正写 ADSP 硬件（AUDIO-3..11 代码从未上机）。可能的失败：SMC 错误/超时（会记录并保留 journal）、卡死或 panic → 重启用 fdt4/main 镜像即可，不写分区。
  验收：约 60 s 后四行 `X9 AUDIO DSPBOOT`；成功 = `result=0 … state=3(Online) ready>=1`；失败记录 result/error/guard/smc 返回值再分析。
  另外留意 GPU 默认值变化是否影响渲染（应与之前相同，因为之前就是显式开着的）。

- **dspboot1 上机（2026-11-13 17:05，用户，Mac `~/249.txt`，第二次收集覆盖了第一次只到 53 s 的版本）**（观察）：1.107 s `armed (board_init=0)`；61.108 s 执行：
  `result=16(EBUSY) flags=1(Registered) error=16`；`votes owned=0 ap=3ce0f all=3ce7f pd_busy=0`；`smc calls=0 resets=0 timeouts=0`；`dsp state=4(Quarantined) primary=16 ready=0`。
  系统继续运行到 176 s+。GPU 默认值改动正常（PARK ON、SubmitAsync ON、jobs failed=0）。
  **原因**：`power_on` 冷接管要求 TOP|INFRA 全局为 OFF；而 AP 投票（PD_EN）里 TOP(2)/INFRA(4)/AO(8) 在我们之前已经置位（推断：LK 留下的，内核没投过）。
  于是按设计拒绝（不 reset 可能在运行的外部系统），**没有任何写入**。
  下一步（待用户同意）：只读探测继承状态——电源域已经开着，读 ADSP cfg/mailbox/信号量寄存器是安全的——判断 core A/B 是否在跑固件、谁持有，再决定「热接管」策略。

- **只读探测 `x9_dsp_probe=<1..600>`**（仅 `x9_audio=2`；`x9_adsp_mt6993_inherited_report()`）：X9Audio start 时读一次（`start`），N 秒后再读一次（`delayed`）。
  不投票、不调用 SMC、不 reset，**不读 mailbox 和共享 DRAM**（前者可能读一下就清状态，后者可能被 EMI MPU 保护）。
  先读 HWV（PD_EN/DONE/ALL/BUSY），只有 TOP+INFRA 全局 ON、done、不 busy 才读 cfg `0x158`(sleep) `0x15c`(pending) `0x590/0x594`(DPSW) `0x64`(AUDIO sem)、
  cfg2 `0x50/0x54`(VLOGIC/SRAM ack)。这些都是本文件已在使用的寄存器。输出 `X9 ADSP PROBE start|delayed: hwv …` 和 `… cfg …`。
- **镜像（未上机）**：`x9/out/x9-dspprobe1-20261113/boot.img`（三步 exit 0；boot-args 以 `x9_audio=2 x9_dsp_probe=60` 结尾，不含 `x9_dsp_boot`），无需 mailbox。

- **dspprobe1 上机（2026-11-13 17:12，用户，Mac `~/250.txt`）**（观察）：start（1.1 s）与 delayed（61.1 s）**完全相同**：
  `hwv ap=3ce0f(TOP1 INFRA1 AO1) all=3ce7f done=ffffffff busy=0 owned=0`；
  `cfg sleep(0x158)=1240 pending(0x15c)=0 dpsw_req=0 dpsw_ack=1 sem(0x64)=0 vlogic_ack=0 sram_ack=7`。系统正常（121 s+）。
  推断（未证实，缺 MTK adsp v3 寄存器语义）：电源域由 LK 以 AP 身份投票打开，core 静止（没在跑固件）；DPSW ack=1、SRAM ack=7 像是 core 断电或 SRAM 休眠。
  注意 `qualify()` 把 `dpsw_ack&1` 当 EBUSY，热接管不能只放开 power_on 的冷检查，需要先弄清 0x158/0x594/cfg2 0x54 的含义。

- **参考源码（2026-11-13，用户提供）**：OPPO MT6993 开源内核 https://github.com/oppo-source/android_kernel_oppo_mt6993 与模块/设备树
  https://github.com/oppo-source/android_kernel_modules_and_devicetree_oppo_mt6993，已浅克隆到 `.local/references/oppo-kernel-mt6993`、`oppo-modules-dt-mt6993`（见 §2.2）。
  ADSP 驱动在 modules 仓库 `kernel/kernel_device_modules-6.12/drivers/misc/mediatek/adsp`。macOS 不区分大小写，kernel 仓库 `tools/memory-model/litmus-tests` 有几个文件冲突，不影响驱动代码。

- **250 的寄存器对照 OPPO `adsp/v3/adsp_reg.h`、`v3/adsp_platform.c`**：0x158=`ADSP_SLEEP_STATUS_REG`（bit0 `A_IS_WFI`、bit1 `B_IS_WFI`、bit2 `AXI_BUS_IS_IDLE`）；
  0x15c=`ADSP_DBG_PEND_CNT`（与 `axibus_idle_val` 比较判断总线空闲）；0x590/0x594=`ADSP_DPSW_REQ/ACK`（bit0）；cfg2 0x50/0x54=`DPSW_AD_VLOGIC_ON_ACK_STATUS`/`DPSW_AD_SRAM_ON_ACK_STATUS`
  （驱动只在 sysfs `dpsw_info` 里打印）。所以 250 的 `sleep=0x1240` 表示 A/B **都不在 WFI**，AXI idle 位也是 0；bit6/9/12 v3 里没定义。
  这和「core 静止」的推断不矛盾也不支持：core 一直被 reset 住时 WFI 位也可能是 0（推断）。
  可读的存活指标（只读）：`ADSP_CFGREG_RSV_RW_REG0/1`（0x8c/0x90，`adsp_mt_read_adsp_sys_status`，固件写入的系统状态），`ADSP_A_WDT_REG` 0x7c / `ADSP_B_WDT_REG` 0x68（bit31 WDT_EN）。

- **OPPO v3 补充**：`mt6993_adspsys_desc.axibus_idle_val = 0` → 250 的 `pending(0x15c)=0` 按驱动判断就是 **AXI 总线空闲**。
  v3 也注册了 `read_adsp_sys_status`（`v3/adsp_platform.c:154`）；`get_adsp_sys_status` 在 `has_system_l2sram()` 时读 0x8c/0x90，否则读共享内存
  `ADSP_SHAREDMEM_SYS_STATUS`，而 mt6993 core0 的 sharedmems 只有 TIMESYNC（推断：走寄存器）。`is_adsp_core_suspend` = WFI && (AXI idle 位 || pending==idle) && status==ADSP_SUSPEND。
- **dspprobe2（未上机）**：同 dspprobe1，cfg 那行之后多一行 `X9 ADSP PROBE …: fw sys_status A(0x8c)= B(0x90)= wdt A(0x7c)= en= B(0x68)= en= wfi A= B= axi_idle=`。
  镜像 `x9/out/x9-dspprobe2-20261113/boot.img`（三步 exit 0，boot-args 以 `x9_audio=2 x9_dsp_probe=60` 结尾），无需 mailbox。
  判读：sys_status 全 0 且两个 WDT 都没开 → 基本可以确定没有固件在跑，热接管有依据；有非零状态或 WDT_EN → 有固件在跑（或跑过），不能 reset。

- **dspprobe2 上机（2026-11-13 17:22，用户，Mac `~/251.txt`）**（观察）：start/delayed 相同；hwv/cfg 与 250 相同；
  `fw sys_status A(0x8c)=0 B(0x90)=0 wdt A=0 en=0 B=0 en=0 wfi A=0 B=0 axi_idle=0`。**ADSP 上没有固件在跑**，LK 只开了电源域。
- **OPPO v3 启动流程（`adsp_core.c` `adsp_system_bootup`、`v3/adsp_driver_v3.c`、`adsp_platform_driver.c`）**：probe 里 `switch_adsp_power(true)`
  =`clk_prepare_enable(CLK_PD_ADSP_TOP)`（HWV 投票，和 LK 已投的票叠加，不检查是否已开）→ `SYS_CLEAR(0xAD5BAD5B)` → 每个 core `initialize`
  （v3 只做软件注册：IPI/WDT irq/logger/misc，**不写硬件**）→ `CORE_START(cid)` → 等 READY IPI ≤2 s（core 0 和 core 1 都启动）。**开机不做 RELOAD**，
  RELOAD 只在 `adsp_reset`（CFG_RECOVERY_SUPPORT）里出现：要求 AXI 空闲 → SYS_CLEAR → clock mux → RELOAD → CORE_START。
  推断：固件由 LK 预先加载（mblock `adsp_image_reserved` 0xe4000000），LK 开着电源域留给内核，是正常交接。
  SMC：`MTK_SIP_KERNEL_ADSP_CONTROL`=SMC64 fast SIP 0x52B=0xc200052b；op INIT0 ENTER_LP1 LEAVE_LP2 SYS_CLEAR3 CORE_START4 CORE_STOP5 CFG_LATCH6 RELOAD7
  QUERY_STATE8 DUMP_PWR_CLK9 SET_SLP_PROT10 SET_CLOCK_MUX11 SET_SRAM_SLEEP_MODE12，与本驱动一致。DPSW 寄存器 Linux 只在 sysfs 打印，不作判断。
- **与本驱动的差异**：①power_on 要求 TOP|INFRA 冷关（Linux 不要求）；②`qualify()` 把 `dpsw_ack&1` 当 EBUSY（Linux 不判断，静止态就是 req=0/ack=1）；
  ③core_start 首次就 RELOAD（Linux 开机不 RELOAD）；④只启动 core A（Linux 两个都启动）。

- **热接管 dspboot2（用户选「照 Linux 开机流程」，未上机）**：`x9_adsp_mt6993.c`
  ①`adopt_inherited()`：power_on 发现 TOP|INFRA 已开时，只有满足 250/251 那样的空闲证据（CHAIN 已投/ON/done/不 busy，`owned_vote=0`，sys A/B=0，两个 WDT_EN=0，
  pending==0，无 DPSW req，AUDIO sem 空）才接管 LK 的 AP 投票，记在 `inherited_vote`。它和 `owned_vote` 一样算租约，但 `chain_off` 永不撤销它（不清 LK 的票）。
  不满足就照旧 EBUSY。打印 `X9 ADSP ADOPT: adopting|refused …`。
  ②`qualify()` 不再把 `dpsw_ack` 当 EBUSY（`dpsw_req` 仍拦截）。
  ③`core_start` 首次启动不 RELOAD（`reload_needed`，打印 `first launch of the LK-loaded image, no RELOAD`），之后的重启才 RELOAD。
  ④仍只启动 core A。流程：adopt → SYS_CLEAR → 清 mailbox 遗留 pending → CORE_START(0) → 等 READY ≤2 s；失败时 transport 自动 STOP/CLEAR/power_off（LK 的票保留）。
  镜像 `x9/out/x9-dspboot2-20261113/boot.img`（三步 exit 0，boot-args 以 `x9_audio=2 x9_dsp_boot=60` 结尾，597/607 B），无需 mailbox。
  验收：约 61 s `X9 ADSP ADOPT: adopting`、`X9 ADSP SMC: op=3 a0=0`、`no RELOAD`、`X9 ADSP SMC: op=4 a0=…`，然后 4 行 `X9 AUDIO DSPBOOT`：成功 = `result=0 … state=3 ready>=1`。
  失败可能：SMC 非 0（ATF 拒绝）、READY 超时（ETIMEDOUT=60；可能 core B 也需要启动，或固件需要别的初始化）、卡死/panic → 重启用 main 镜像。
  待补：`x9_audio_handoff_report` 缺原型（-Wmissing-prototypes）。

- **252**：用户误开了 dspprobe2（boot-args `x9_dsp_probe=60`），结果与 251 相同，没有写硬件。
- **dspboot2 上机（2026-11-13 17:47，用户，Mac `~/253.txt`）——ADSP core A 首次启动成功**（观察）：
  61.113 `X9 ADSP ADOPT: adopting LK AP votes ap=e; sys A=0 B=0 wdt A=0 B=0 pending=0 dpsw_req=0 sem=0` →
  `X9 ADSP SMC: op=3 a0=0`（SYS_CLEAR）→ `first launch … no RELOAD` → `X9 ADSP SMC: op=4 a0=0`（CORE_START 0）→ 61.128
  `DSPBOOT: result=0 flags=2f(Registered|Exclusive|Powered|Started|RegistersValid) error=0`，`smc calls=2 resets=1 timeouts=0`，
  **`dsp state=3(Online) gen=1 ready=1 recv=1`**，START→READY 约 15 ms。之后 `sleep(0x158)=11240`（多了 bit16，v3 未定义；推断与 core 运行有关），dpsw 0/1、sram 7 不变。
  系统继续正常运行到 74 s+，渲染正常；CRASH 只有 AegirPoster×9、driverkitd×1（与之前相同）。
  **下一步候选**：①core B 是否需要启动（Linux 两个都启动）；②IPI 消息往返（sent=0，还没发过请求）；③共享内存/EMI、AFE/I2S、PA，最终出声。
  boot-args 现在 597/607 B，再加音频参数前需要继续腾空间（或把已验证的 x9_audio/x9_dsp_boot 改默认）。

### 5.8 2026-11-13 音频：IPI 消息往返（分支 feat/audio-ipi，worktree `.worktrees/audio-ipi`）

- feat/audio-dsp-boot 已 ff 合入 main（`1a76d62`，已推送）。旧 worktree/分支的删除被权限拦下，待用户处理；
  `android-callsms-20261113`、`shimtest`、fdt 日志已移到本 worktree 的 `x9/out/`。
- **消息选择（OPPO `drivers/misc/mediatek/audio_ipi`）**：`AUD_CTL_MSG_A2D_IPI_TEST`=0xADC0（`audio_controller_msg_id.h`，内核驱动不用，推断给 HAL 测试）。
  scene `TASK_SCENE_AUDIO_CONTROLLER_HIFI3_A`=**42**：`audio_task.h` 里 0..41 固定，之后的 `#if` 全是车载配置（`CONFIG_MTK_ADSP_AUTO_*`、
  `CONFIG_SND_SOC_MTK_AUTO_AUDIO_DSP` 只出现在 `kernel/configs/auto*.config`），手机编译不启用（HIFI3_B=43，AUD_DAEMON_A=44）。
  layer：FROM_KERNEL=1、TO_DSP=2；MSG_ONLY=0；NEED_ACK=1、ACK_BACK=2。内核第一次和控制任务通信发的是 `DMA_INIT`（PAYLOAD、DIRECT_SEND），本 transport 只允许原生 IPI 路径发它。
- **实现**：`x9_dsp_boot` 流程里，READY（Online）后 0.5 s 发**一条** IPI_TEST（scene 42，MSG_ONLY，NEED_ACK，p1=0x58390001，p2=0），`x9_adsp_request` 超时 250 ms，
  doorbell 1 ms 内必须被消费；失败不重试（会话隔离）。只走 mailbox。打印 `X9 AUDIO IPI: test … result= reply …` 和 `X9 AUDIO IPI: dsp_err= … sent= recv= acks= …`。
  boot-args 只剩 10 B，所以没加新开关，直接并进 x9_dsp_boot（测试专用路径）。顺便给 `x9_audio_handoff_report` 补了原型。
- **镜像（未上机）**：`x9/out/x9-dspipi1-20261113/boot.img`（三步 exit 0，`x9_audio=2 x9_dsp_boot=60`），无需 mailbox。
  验收：约 61 s 和 253 一样 ADOPT→SMC 3→SMC 4→`state=3 ready=1`；约 61.6 s `X9 AUDIO IPI: … result=0 reply … ack=2 id=0xadc0`，`acks=1`。
  可能结果：ETIMEDOUT(60)（固件不处理/不 ACK 这条消息，或 scene 不对）；ACK 回来但 p1/p2 含义未知（只记录，不当 errno）。
- **功放阶段（同一测试路径，dspamp1 取代 dspipi1）**：盘点后确认真正的功放写入/出声链路未接线——`x9_dsp_playback_*`、`x9_speaker_afe`、
  `x9_tfa98xx`/`multimsg`/`re25` 都没有调用者（无 coordinator），TFA 容器/校准是厂商数据不能进 git；冷 `path_probe` 需要全局资源，ADSP 在线时
  必然 EBUSY（单 owner），且 LK 遗留的 TOP 票也会让它拒绝。所以本镜像推进到**第一次 I2C6 实际访问两颗 TFA98xx**：dsp-boot（+IPI，仅 Online 时）之后
  0.2 s 无论 DSP 结果都调一次 `x9_tfa_plj110_probe()`——借用已开时钟/已路由 pad，持 PMIF，PMIC 复位/供电快照前后比对，只读 0x34/0x35 的 reg03
  （revision，family 0x72/0x73/0x74 才算已知），恢复控制器 CSR；**不写任何 PA 寄存器、电源/复位或 PLL**（控制器 CSR 和寄存器地址字节会写）。
  打印 4 行 `X9 AUDIO PA:`（probe/flags/stage/rev、PMIC reset/supply 前后、时钟/bank/pin、i2c6 计数）。
- **镜像（未上机）**：`x9/out/x9-dspamp1-20261113/boot.img`（三步 exit 0，`x9_audio=2 x9_dsp_boot=60`）。
  验收：61 s DSPBOOT `state=3 ready=1` → 61.6 s IPI → 约 61.8 s `X9 AUDIO PA: probe=0 … stage=10 … rev=xx7x/xx7x read=3 family=3`。
  可能失败：stage=3 ENXIO(6)（I2C/DMA 时钟没开，不会去开）、stage=4 EBUSY(16)（总线被显示/充电器占用）、stage=6/7 EIO(5)+req=2（NACK，
  PA 处于复位/没上电——LK 不一定放开复位）、ENOTSUP(45)（读到值但不是已知 family）、stage=8 EBUSY（PMIC 前后不一致）。
  下一步按结果：NACK→需要原生 PA 复位/供电释放（写 GPIO/PMIC，要先问用户）；成功→AFE/I2S 时钟 + TFA 上电序列（写 PA，要先问用户）。
- **日志 254（dspamp1）**：DSP 同 253，61.110 ADOPT→SMC3→SMC4→61.125 `state=3 ready=1`，系统稳定到 76 s+。
  IPI：`result=100(EPROTO)`，`sent=1 recv=3 malformed=1 last_ipi=9 state=4`——请求已被消费，之后收到的 AUDIO_IPI(9) 被判畸形，会话隔离。
  解码通过才会走 EPROTO，所以是 `source!=2||target!=1`；OPPO `check_msg_format` 允许任何合法 layer，TO_HAL(0) 消息转 HAL——IPI_TEST 本是 HAL 测试消息，推断 DSP 回给 TO_HAL。
  PA：`probe=16 stage=2`（PMIC 准入），未碰 I2C。`reset=1,1,d,0,f,f`：GPIO10/11 **DIR=0（输入）**、DO=0、**DI 高**、MODE=0、AD 正常——LK 不驱动复位脚，
  线由外部保持在高（=未复位）；`supply=d0,1,3,80`：1.8V、EN=1，但 **HW_OP_EN0=0x80**（额外硬件投票）。旧准入要求“输出高”且 HW_OP_EN0=0，太严。
- **dspamp2**：①传输层：AUDIO_IPI 按 OPPO 语义——slot 比消息长可接受（截到 header+payload 再严格解码）；`FROM_DSP→TO_HAL` 合法，入事件队列不完成请求；
  每次启动前 16 条 AUDIO_IPI/畸形 RX 打 `X9 ADSP RX: <why> core= pending= ipi= len= hdr=…`（原始 16 字节头）。
  ②PA 准入：复位脚接受“输出高”或“输入且 DI 高”（任何驱动低都拒绝，flag 0x80）；HW_OP_EN0 非零记 flag 0x100，不再拒绝。仍只读 reg03，不写 PA/电源/复位。
  镜像（未上机）：`x9/out/x9-dspamp2-20261113/boot.img`（`x9_audio=2 x9_dsp_boot=60`）。
- **日志 255（dspamp2）**：DSP 同 253（61.125 READY）。
  IPI 原始回复 `X9 ADSP RX: audio ipi=9 len=16 hdr=212a8888 adc00200 58390001 00000064`：magic 8888、scene 42、**layer 0x21（src=1 KERNEL/tgt=2 DSP，未交换）**、
  MSG_ONLY、**ack=2 ACK_BACK**、id 0xadc0、p1 原样回显、**p2=0x64(100)**（含义未知）。即 **IPI 往返实际成功**：DSP 原样回显请求头只改 ack。
  我们要求 layer 互换所以判畸形（EPROTO）。OPPO `audio_ipi_queue.c:819` 匹配 ACK 只比 `task_scene`+`msg_id`。
  PA：准入通过（flags=0x183 = ResetInput+SupplyHWVote），`stage=3 ENXIO`：`gate=0x1e`——**imps_i2c6 时钟门控关着**（0x129d0000 STA 0xE00 bit2；
  I2C3/I2C10 也关）；APDMA 门控 `dma_gate=7c0402` bit26=0（开）。OPPO `clk-mt6993-peri.c` imps_cg_regs：STA 0xE00 / CLR 0xE04（开）/ SET 0xE08（关），I2C6=bit2，父时钟 `cksys_i2c_west_ck`。
- **dspamp3**：①`x9_dsp_ack_matches` 改为 OPPO 语义 scene+id；ACK_BACK 不检查 layer，其它 DSP 消息仍要 FROM_DSP→KERNEL/HAL。
  ②PA 探测：门控关着时写 CLR 0xE04=4 开 I2C6 门控（flag 0x200），探测+控制器 CSR 恢复干净后写 SET 0xE08=4 关回（flag 0x400 = 已关回）；
  总线卡住/未恢复则不关时钟。**这是本路径第一处新增硬件写（一个时钟门控位，可恢复）**；不动 mux/PLL/APDMA、PA、电源、复位。父 mux 关着仍会 ENXIO。
  镜像（未上机）：`x9/out/x9-dspamp3-20261113/boot.img`。
- **日志 256（dspamp3）**：**IPI 往返成功**：`result=0 reply scene=42 src=1 dst=2 ack=2 id=0xadc0 p1=0x58390001 p2=0x64`，`acks=1 malformed=0 state=3`。
  PA：flags=0x783（I2C6 门控开了也关回了，gate 1e→1a→1e），clock_check 过（parent=1 nominal=124.8 MHz），`stage=4 EBUSY`：bank0 `MASK=0x3f8`
  （LK 残留中断使能；START/STAT=0、FIFO 空）。修正 `79b3bf4`：仅当本次是我们开的门控时接受残留 MASK，记账清 0、结束恢复（dspamp4 已编，**不再上机**）。
- **用户决定（2026-11-13）：不再做探测镜像，直接推进出声。** 盘点：部件都在但没有总调度——ADSP 组合 owner `x9_adsp_mt6993_speaker_control`、
  时钟 `x9_speaker_mt6993_clock_acquire`、pads、AFE `x9_speaker_afe_*`、I2C6 native、`x9_tfa98xx` 容器解析，均无生产调用者。
  **阻塞：TFA 容器 `tfa98xx.cnt` 不在 Mac**（.local 和分区备份里都没有）；需要 Android root 下用 `x9/audio/collect-speaker.py --out .local/x9/work/speaker-<date>` 只读采集。
  计划顺序（照 OPPO）：AFE DL→I2S4 出 BCK/WS（先送零）→ PA 复位/供电确认 → I2C6 按容器 profile 初始化 TFA（PLL 锁 BCK）→ 解静音 → 内核低幅正弦 2 s → 静音拆除。
- **Android 采集（2026-11-13，`.local/x9/work/speaker-20261113`，complete=True，不入 git）**：`collect-speaker.py` 修了 adb 37 双重引号
  （adb 自己转义每个 argv，预先 `shlex.quote` 的 `-c` 参数让 KernelSU su 把 `'id -u'` 当程序名；改为单个 `su -c <quoted>` 参数）。
  - **实际功放 = 两颗 TFA98xx @I2C6 0x34/0x35**（`/soc/i2c@12990000/tfa98xx@34/35`，驱动 tfa98xx）；DT 里还有 `richtek,rt5512 @0x5c/0x5d`（未探到，备选料）。
    dmesg 183 s 一次播放：`tfa98xx_write_dsp(): [0x34] dsp_msg error: -1` ×2 → `tfa_dev_start success (0)` ×2 → unmute，4 s 后 mute。
    即保护算法消息走 ADSP 失败，**功放照样按容器启动出声**——AP 直写寄存器 + I2S 就是 Android 的实际工作方式。
  - **容器 `/odm/firmware/tfa98xx.cnt`（6118 B，`PM3_01`，customer OPPO/TFA，type `3a65_08`，CRC 0x90eb2cde）**；`/vendor` 无，无 reverse。
    **芯片 TFA9865 MN1A3（rev 0x3a65，family 0x65）**——我们 `x9_tfa_plj110` 只认 0x72/0x73/0x74，要改。
    每个设备：type29(`dscReserved1`)、type33(`dscRevid`)、46 个 bitfield、`TFA9865MN1A3_dummy.patch`(42 B)、8 个 profile：
    speaker / receiver / MMI_Test_Speaker / MMI_Rec / MMI_Test_HighGain / MMI_Test_LowGain / calib / calibrate.cal（后者带 7 个 `.msg` 算法参数文件，给 ADSP）。
    `speaker` profile：PWDN(0x0040)=1、0x5f54=0xe(增益)、0x7ce1=1、0x60e1=1、default 标记、PWDN=0、0x60e1=0。
  - 枚举来源：vendor `oplus/kernel/audio/codecs/tfa98xx_v6.14.2/inc/tfa98xx_parameters.h:607-611`（29..32 Reserved1-4，33 Revid）；
    `kernel_device_modules-6.12/sound/soc/codecs/tfa98xx` 是旧枚举（到 28）。我们的 `x9_tfa98xx.c` 按旧枚举把 ≥29 当非法 → 整个容器 EINVAL，要改。
    芯片 init：`tfa98xx_v6.14.2/src/tfa_init.c:1384` `case 0x13a65`（17 个固定寄存器写）。
  - Android 无 debugfs 挂载、无 `/dev/i2c-6`；只有驱动 sysfs `reg`/`rw`（要写选择器，未用）。

### 5.9 2026-11-13 音频：出声（TFA9865 + AFE DL24→I2S OUT4，进行中，分支 feat/audio-ipi）

- **用户决定：不再做探测镜像，直接做出声。** 现有租约框架（pa_reset/vdd/i2c6/pads/clock 各层都要“真实独占证明”回调，注释写明
  **没有默认提供者**，右声道供电“无法证明”；`x9_pa_native_revision_supported` 只有 0x72/73/74）按设计永远不会放行播放。
  决定：写**独立、有界的出声测试协调器**，复用已验证的组件：组合模式 `x9_adsp_mt6993_speaker_control(0,ticket)`（Speaker ticket 下启动 ADSP）
  → `x9_speaker_mt6993_clock_acquire`（AUDIO 票+VLP 时钟+APLL2+tuner/ENGEN2）→ `clock_pad_hold` + `x9_speaker_pads_acquire`（GPIO108-111 mode5）
  → AFE engine（CPU 生产者，memif10 DL24，`prepare_on_ticket`/`start`/`submit`/`stop`）；TFA 的 I2C 走 `x9_tfa_plj110` 已调通的控制器代码加会话读写。
- **TFA986x 启动序列（OPPO tfa98xx_v6.14.2，`tfa_dsp.c` tfaRunStartup/tfa_dev_start/tfa_dev_set_state，`tfa_init.c` tfa986x_specific）**：
  reg 8 位、值 16 位大端；bitfield = `reg<<8|pos<<4|(len-1)`。reg03 低字节 0x66 = tfa986x 通用（family 2，probus，mute=no-op）。
  1. `I2CR`(0x0010)=1：直接写 reg0=0x0002（复位全部 I2C 寄存器）。
  2. tfa986x_specific：PWDN(0x0000)=0、MANAOOSC(0x0460)=0，等 MANSTATE(0x1203)==1（≤50 次）；读 OTP 影子推 revid：
     `type_selection 0xf3b1` 非 0 时 rev=reg03-1，`disable_lpm 0xf090`=0 → MN 变体 rev|=1<<16，`lock_max_dcdc 0xf4f0` 非 0 再 -1；DEVREV(0x0687)&3 加 0x100000 倍数。
     容器 type `3a65_08` + 补丁名 `TFA9865MN1A3` → 期望 **revid 0x13a65**。
     解锁：写 0x0F=0x5A6B；读 0xFB；写 0xA0=值^0x005A；key2(0)：写 0x0F=0x5A6B、写 MTPK(reg 0xA1)=0x5A（advance_keys_handling=1，不再隐藏）。
     `case 0x13a65` 表（17 写）：00=f241 02=0628 50=c000 5a=5f4c 5b=74e2 5c=302b 5f=00a0 62=05c4 63=80d4 67=0066 68=0820 74=60f0 75=0e00 78=0001 7c=10f2 cf=2cd7 d7=1000 dd=0036。
     然后 PWDN=1，等 MANSTATE==0；MANAOOSC=1；（irq init，可省）。
  3. 恢复 AUDFS(0x0203)/FRACTDEL(0x0256) 原值；写设备列表寄存器项（遇第一个非寄存器项停：type29/33 跳过，46 个 bitfield）；写 profile `speaker` 中 default 标记前的项：
     DCA(0x0040)=1、TDMSPKG(0x5f54)=0xe、LNM(0x7ce1)=1、IPM(0x60e1)=1。
  4. INIT_CF：MANSCONF(0x0120)=1、PWDN=0，等 **CLKS(0x1100)=1**（需要 I2S BCK/WS 已在跑）。
  5. OPERATING：PWDN=0、MANSCONF=1、**AMPE(0x0030)=1**（probus 无 SBSL）。静音 = AMPE=0（mute_nodsp 是空操作）。
  停止：AMPE=0 → PWDN=1。
- **出声测试顺序**：Speaker ticket → ADSP 启动 → 时钟 → pads → AFE prepare/start（先送静音，BCK/WS 起来）→ 两颗 TFA 按上面 1-4 → 5 AMPE=1 →
  内核写 2 s 低幅（-30 dBFS 级）1 kHz 正弦 → AMPE=0、PWDN=1 → AFE stop → pads 释放 → pad 子租约 drop → clock release → speaker_control(2) → 释放 ticket。
- **实现（spk1）**：`x9/kernel/src/bsd/dev/x9_speaker_bringup.c`（`x9_speaker_bringup_run`），`x9_dsp_boot=N` 的线程现在跑它（不再单独 control(0)/IPI/PA 探测）。
  `x9_tfa_plj110.c` 加会话 `session_open/read16/write16/session_close`（同探测准入；PMIF 只在快照期间持有；写=CTRL 去 RS/DIR_CHANGE、LEN=3、TRANS=1、START=1）。
  容器：`x9/tools/tfa_container.py --src .local/x9/work/speaker-20261113/odm-tfa98xx.cnt` → `x9/kernel/generated/x9_tfa_container.inc`（gitignored；缺失=空桩，
  协调器在任何硬件访问前拒绝）；build.py 配置键 `tfa_container`。顺序：DMA 64 KiB 连续页 → Speaker ticket → I2C6 会话 → 两颗 TFA init（I2CR→…→PWDN=1，
  要求 revid==0x13a65）→ speaker_control(0) → clock_acquire → pads_acquire（mute 回调=两颗 AMPE=0 并回读）→ AFE register/prepare_on_ticket →
  start 960 帧静音 + 再排 ~100 ms 静音 → 每颗 MANSCONF=1、PWDN=0、等 CLKS → AMPE=1 → 1 kHz、-24 dBFS（>>4）+ profile TDMSPKG=0xe，2 s → mute →
  逆序拆除（AMPE=0/PWDN=1、afe stop、dsp halt、pads release、clock release、dsp release、会话关闭、ticket 释放）。日志前缀 `X9 SPK:`。
  镜像（未上机）：`x9/out/x9-spk1-20261113/boot.img`（`x9_audio=2 x9_dsp_boot=60`）。
- **日志 257（spk1）**：61.101 容器 6118 B、DMA bus 0x10dfb8000、ticket=1，然后 `i2c6 session -> 5`（EIO）stage=5（Timing）：某个定时 CSR 回读≠写入
  （探测从没走到 timing，首次暴露）；flags=0xb87，restore=0，**未碰任何 PA 寄存器**，ticket 正常释放，系统继续。`put()` 不记录是哪一个寄存器。
- **spk2**：定时阶段写入改为“回读不一致时打印 `X9 SPK: i2c6 timing reg= wrote= read= saved=` 并以实际值入日志（恢复仍精确）”，传输 CSR 仍严格；
  另打印 `i2c6 timing parent= div= total= hs= ls= ext=`。**用户要求不出声（2026-11-13）**：去掉 AMPE=1，功放停在 INIT_CF（上电、锁时钟、放大器关），
  2 s 期间 AFE 只送静音（验证 DMA 游标）。镜像：`x9/out/x9-spk2-20261113/boot.img`。
- **日志 258（spk2）**：**I2C6 写/读打通，两颗 PA 都 ACK**。定时：parent 124.8 MHz、div=3、total=426（≈97.7 kHz）；
  `reg=30(HS) wrote=0 read=8000`（bit15 只读恒 1）、`reg=48(CLOCK_DIV) wrote=202 read=2`（只实现低字节；LK 原值 4），均无害，按实际值入日志。
  继承状态两颗相同：`sysctrl=f261`（PWDN=1 断电）`flags10=7e8f stat11=0018 stat12=5000`。
  **reg03=0x9866**，`type_sel=1 lpm=0 dcdc=0 devrev=0` → OPPO 规则 **revid=0x19865**（不是按补丁名假设的 0x13a65）→ 写芯片表前拒绝（45）。
  共 4 次写（I2CR/PWDN/MANAOOSC），拆除 AMPE=0/PWDN=1 → 0，会话关闭 0，ticket 释放 0，系统正常。
  OPPO `tfa986x_specific` 的表只有 3a64/13a64/103a65/13a65/100b64/110b64/200b65/110b65 → 0x19865 走 **default（只警告、不写芯片表）**，Android 就这样启动成功。
- **spk3**：revid 0x13a65 写表；**0x19865 跳过芯片表**（与原厂 default 一致，打印 `OPPO default branch, no chip table`）；其它拒绝。仍不出声（AMPE 不置 1）。
  镜像：`x9/out/x9-spk3-20261113/boot.img`。
- **日志 259（spk3）**：**PA34 完整初始化成功**：revid 0x19865 default 分支 → `init result=0 device_items=46 profile_items=4`（61.120→61.172）。
  PA35 同样 46 个设备项写完，但 `profile_items=0` → ENOENT：容器里 0x35 的 profile 0 叫 **`speakerR`**（0x34 是 `speaker`；0x35 列表：
  speakerR / powerdown.standby / MMI_Test_SpeakerR / powerdown.standby / MMI_Test_HighGain / MMI_Test_LowGain / calib / calibrate.cal）。
  拆除：两颗 AMPE=0/PWDN=1 → 0，会话关闭 0（写 87 次），ticket 释放 0；DSP/时钟/pads/AFE 还没走到。
- **spk4**：profile 按**序号**选（OPPO/NXP 各设备同序号切换），序号 0，打印 `X9 SPK: PA3x profile 0 = <name>`。仍不出声。镜像：`x9/out/x9-spk4-20261113/boot.img`。

### 5.6 2026-11-13 音频：有界 FDT 收集恢复 ADSP/MIC 交接（分支 feat/audio-bounded-fdt）

- **改动**（`b84f08e`）：新 `x9/shim/fdt_collect.c`：第 1 遍完整校验并把 28 个音频节点的全部属性/直接子节点数收进静态表（上限 32 节点/384 属性，约 6 KiB BSS），
  第 2 遍一次核对全部 phandle；每 2048 token `wdt_kick()`，每遍 2 s 预算超时拒绝。`audio_handoff.c` 一次走 ADT 找三个槽，按特性（IRQ/I2S4/PA reset/LEFT VDD）单独撤销。
  `adsp_handoff.c`、三个 `.inc`、`mic_dma_handoff.c` 改为查表，所有原检查保留。loader 调 `fdt_audio_patch_adt`，build.py 重新链接。
- **验证（主机）**：新旧实现在 `.local/x9/modem/live.dtb` 上对照，基线 + 9 种篡改结果完全一致；全树扫描 124 次（ADSP 83 + MIC 41）→ 2 次。live.dtb 上 AUDIO IRQ 新旧都不通过（旧 dump 与 LK FDT 不同，待上机看）。
  单位编译 `-Werror` 零警告、ABS64=0，栈峰值 < 3 KB。测试在 `x9/out/shimtest/`（gitignored）。
- **镜像（已构建，未上机）**：`.worktrees/audio-bounded-fdt/x9/out/x9-audio-fdt2-20261113/boot.img`，`build-usb-prelink.py`（与 jbi6 同流程），main `63e4b67` + 本提交，
  local.json `appsync_runtime` 指向 v6（对应已 staging 的 installer3）。无新插件包，**无需 mailbox**。内核无 `x9_audio`/`x9_mic`，只被动复制布局。
- **验收**：USB/pstore 日志依次出现 `X9 AUDIO FDT: bounded collection pass 1/2`、`pass1 us`、`nodes present`、`pass1+2 us`、`watchdog kicks`、ADSP/MIC qualified 行 → boot_args/jump → Darwin。
  若停在 pass 1/2 之后，记录最后一行；回退用不含本提交的 main 镜像。
- **fdt2 上机（2026-11-13 16:40，用户，日志 Mac `~/246.txt` = `x9/out/x9-audio-fdt2-20261113/usb-log.txt`）**：**正常进系统**（85 s 后仍在跑 SpringBoard/渲染），
  有界收集没有卡住 shim（BOOT130 那种停顿没有再出现）。但 `X9 AUDIO FDT`/`X9 ADSP FDT`/`X9 MIC DMA FDT` 行**一条都没有**：USB 接收端在 shim 阶段只读到
  `x9 LK display: preserve ...` 为止的 12 行，之后 XNU 接管 gadget（`USB log stage changed; restarting cursor`），loader 里后面的 plog（SCP/AUDIO/CAMERA/Wi-Fi FDT）
  只进了 pstore/RAM。所以 ADSP/MIC 槽有没有填，这次日志看不出来（观察）。
- **fdt3（未上机）**：内核新增只读 `x9_audio_handoff_report()`（`x9_adsp_mt6993.c`，在 `kern_exec.c` SWEET 安装块开头调用），每次开机读 `/chosen` 的
  `x9-adsp-handoff`/`x9-adsp-audio-irq`/`x9-mic-dma-handoff`，用与消费者相同的校验打印一行
  `X9 AUDIO HANDOFF: adsp=<absent|badsize|zero|set> valid= flags=(layout,i2s4,pa_reset,left_vdd) reserve= alloc= irq=... intid= mic=... valid= flags= window=`；
  不碰硬件、不授予。镜像 `x9/out/x9-audio-fdt3-20261113/boot.img`（build-usb-prelink.py 三步 exit 0），无新插件包，**无需 mailbox**。
  预期（推断）：adsp/mic `set valid=1`；live.dtb 主机测试里 AUDIO IRQ 不通过，所以 irq 可能是 `zero`。若三个都是 zero，说明收集被拒，再看 pstore 或把拒绝原因带进 ADT。
- **fdt3 上机（2026-11-13，用户，Mac `~/247.txt`）**：进系统。`X9 AUDIO HANDOFF: adsp=set valid=1 flags=f(layout,i2s4,pa_reset,left_vdd)
  reserve=160000000+da0000 alloc=d95000 irq=zero mic=set valid=1 flags=7 window=1000000000`（观察）：**ADSP 布局 + 三项可选拓扑、MIC DMA 都在真机上通过**，只有 AUDIO IRQ 被拒。
- **IRQ 被拒原因（已确认，live.dts）**：`qualify_audio_irq` 要求 `/` 和 `/soc` 都**显式**有 `interrupt-parent = <GIC phandle>`；live FDT 里 `/soc` 没有这个属性
  （simple-bus，继承根的 `<0x01>`），GIC（phandle 1，gic-v3，4 cells，reg 正确）和 adsp-core0（SPI 0x216/0x218/0x21a level-high，names 正确）都符合。
  旧实现同样的检查，所以主机测试里新旧都不过。修复：根必须显式且等于 GIC；`/soc` 没有就按继承，有则必须一致；路径上仍禁止 interrupt-map。
  主机测试（`x9/out/shimtest`，新旧参照都打同一修复，加「根 interrupt-parent 指向 soc」「audioirq SPI 改 0x21c」两例）：baseline irq=1，全部 12 例 MATCH。
  （注意：shimtest 要用 Xcode 工具链 `-isysroot .../MacOSX14.0.sdk` 编，系统 cc 的 CLT MacOSX27 SDK 链接失败；还要加 `old/fdt_property.c`。）
- **fdt4（未上机）**：`x9/out/x9-audio-fdt4-20261113/boot.img`（三步 exit 0），= fdt3 + 上述修复，**无需 mailbox**。验收：`X9 AUDIO HANDOFF` 行 `irq=set valid=1 intid=570`，
  adsp/mic 不变。内核仍没开 `x9_audio`，IRQ 不会被武装。下一步（IRQ 通过后）：按 NOTES 旧交接决定 ADSP 上电/固件 RELOAD 路线（`x9_audio=…`），每个写硬件步骤先确认。
- **fdt4 上机（2026-11-13 16:53，用户，Mac `~/248.txt`）**：进系统，**三个槽全部通过**（观察）：
  `X9 AUDIO HANDOFF: adsp=set valid=1 flags=f irq=set valid=1 intid=570 mic=set valid=1 flags=7`。这次 shim 段也收到了：nodes present=0xfffffff（28/28），
  properties=0x102（258），**pass1 547 ms，pass1+2 1130 ms**，watchdog kicks 26；ADSP IRQ/布局/I2S4/PA reset/LEFT VDD、MIC DMA 各行都出现，之后正常 jump。
  CRASH 只有 AegirPoster×2、driverkitd×1（与之前相同）。注意：主机 1 ms，真机 shim（MMU/D-cache 关）约 1.1 s，每遍预算 2 s，pass1 余量约 3.6 倍；会加约 1.1 s 启动时间。
  **有界 FDT 收集功能完成。** 下一步待用户决定：合并到 main；ADSP 真上电（`x9_audio=2` + `audioctl dsp-boot`：power/reset/secure RELOAD/CORE_START0→READY，旧交接 AUDIO-3..11
  均未上机），boot-args 已 603/607 B，需要改为内核默认或腾空间。
- **2026-11-13 仓库整理**：fix/disable-fairplayd（含 jb-installer、usb-prelink-shim、photos-storage-logs）ff 合入 main，feat/usb-mtu3-xdci 合并（`63e4b67`）；
  删除这 5 个 worktree/分支及 diag/gpu-x9t。jbi6 等输出随 worktree 删除，可从 main 重建。

### 5.10 2026-11-13 日志 260（整机卡死）、GPU 发热、麦克风接入 mic1
- **日志 260**：spk4 走到 `PA34 rev=9866 revid=19865` 后整机停止（黑屏、按键无响应、USB 断）。之后只有解锁序列
  （写 0x0F、读 0xFB、写 0xA0/0x0F/0xA1），和 259 同一份代码，259 是通过的。GPU GOV/MODEM 日志也一起停了，所以是整机卡死，不是单线程卡住。
  巧合：259 的 MODEM `S2 listen 55007 ms` 结束于 61.125 s，正好落在 dsp-boot（x9_dsp_boot=60）的 PA 初始化里；
  260 卡在 61.123 s（推断，未证实）。mic1 改 `x9_dsp_boot=75` 错开，并在每次解锁访问前打一行 `X9 SPK: PA.. unlock ..`。
- **发热原因**：`boot-device.json` 默认 `x9_gpu_khz=1716000` + `x9_gpu_floor_khz=1716000`，GPU 下限=上限=1716 MHz/0.80 V，util 0% 也满频；
  80 °C 限频退到"下限"，也就是最高频，等于没有温控。255–260 全都这样，260 温度从 52 °C 涨到 72 °C，开机时电池已经 40.25 °C 且在充电。
  已删掉默认下限，回到代码默认的 900 MHz，上限仍是 1716 MHz：空闲时会降频，≥80 °C 时真的限到 900 MHz。
- **麦克风现状**：MIC-1…16 加最终链路的代码都在（MT6681 冷上电/MTKAIF/ADC/SRC/TX、AFE RX、VUL1、native DRAM ring），
  但从来没有调用方注册 `x9_mic_plj110_register_native_dma`，所以从来没录到过 PCM。
- **mic1 接入**：新增 `x9_mic_bringup.c`，用 `ml_io_map` 映射 AFE，提供 VUL1 drain（ON=0、CUR 静止、MON busy 位清零，连续 2×1 ms，20 ms 上限）。
  绑定 64 KiB native DMA 后，`x9_mic_kernel_capture()`（sweet_x9_microphone.c）走和 ioctl 相同的 START/commit/STOP 录 `x9_mic_boot` 毫秒，
  只打印统计（frames/min/max/mean/rms/zeros/前 8 个样本）和 board/codec/afe/analog/digital/dma 各级 stage/flags/errno。
  不存 PCM、不写盘、不放音。在 dspBootOnce 里麦克风先跑、扬声器后跑；可用 `x9_spk_boot=0` 跳过扬声器。
  镜像 `x9/out/x9-mic1-20261113/boot.img`（`x9_audio=2 x9_dsp_boot=75 x9_mic=2 x9_mic_boot=1000`）。
  看 `X9 MIC:` 行：bind→0、capture→0、rms 明显非 0 且 zeros 远小于 frames，就说明录到了真实声音。
- **日志 261（mic1）**：GPU 空闲时降到 858/572 MHz，温度 54–62 °C（下限改动生效）。麦克风：bind→0，capture→EBUSY(16)，
  卡在 board stage 1（BASELINE），`gpio9=700/700`：LK 把 codec-en GPIO9 留成输出高（codec 已上电），旧代码只接受冷启动，所以拒绝了。
  之后整机又卡死。USB 日志断在 76.115 s 麦克风报告的半行；从 Android 只读拉了 pstore（只有 shim 的 `pmsg-ramoops-0`，没有 console）
  和 UFS sidecar（`pull_disk_log.py --manifest`，seq 44，停在 75.41 s，比 USB 还早），原件在
  `x9/out/x9-mic1-20261113/android-261/`（gitignored）。结论：两次卡死的最后约 1 KiB console 都没发出去，看到的"最后一行"不是真正的卡死点。
  260 时 PA35 已经能初始化（259 修好），所以第一次走到 DSP 热接管/时钟/pads/AFE，这几步此前从没在真机上跑过，是头号嫌疑（推断）。
- **mic2**：扬声器每一步打印后 `IOSleep(250)`（`SPKLOG`），在 dsp boot、clock acquire、clock afe、pads、afe start 之前各加 begin 行，
  AFE 开始出流后不再等待。麦克风接受继承的 GPIO9 输出高：`codec_enable` 本身就先拉低 1 ms 再拉高（冷复位），restore 还原成高。
  镜像 `x9/out/x9-mic2-20261113/boot.img`（参数同 mic1）。
- **日志 262**：用户提前停了采集（74.46 s，没卡死），没有音频行。
- **日志 263（mic2）**：**没有卡死**，一直跑到 290 s。麦克风：GPIO9 冷复位放行后走到 board stage 3（BUS），
  `bus_fault_reg=8/errno 16`：SCP-I2C1 的 INTR_MASK 是 LK 残留的，`bank_idle` 拒绝后 `bus_stuck` 置位，restore 做不了，
  于是返回 EIO(5)，音频 lease 被扣住。扬声器 `ticket -> 16`，一步都没跑。扬声器没跑就没卡死，这进一步说明卡死在扬声器路径（推断）。
  另外：ticket 失败后扬声器仍打印 quiet run/tone 行，只是日志多余，没有动硬件。
- **mic3**：`bus_take` 只在 START/STAT/FIFO 都空闲时接受残留 MASK，用 `bus_csr_write(MASK,0)` 记入 journal，释放时还原成原值。
  SPKLOG 等待改为 600 ms，麦克风报告后等 2 s。镜像 `x9/out/x9-mic3-20261113/boot.img`（参数同 mic1）。
- **日志 264（mic3，卡死）**：逐步日志第一次定位到卡死点：`X9 SPK: clock acquire begin`（93.78 s）之后整机停住，
  也就是 `x9_speaker_mt6993_clock_acquire` → `take(true,false)` 内部。之前的每一步在真机上都过了：
  两个 PA 完整初始化（各 46 个 device 项 + 4 个 profile 项，profile 0 = speaker/speakerR，fb=ecd8/ecd4），
  **DSP 热接管首次成功**（`dsp boot -> 0`，`state=3 gen=1`）。
  麦克风：INTR_MASK=3ff 清零成功，走到 board stage 4（XO）EBUSY：BBCK5 已经开着（codec 由 LK 上电）。
- **mic4**：`take()` 的每一步前加 `X9 SPK CLK: <step>` + 600 ms，依次是 native board、map/AO、APLL、pll cold、smc 0、
  domain votes、mux、first AFE reads、AFE gates、smc 7、pll take、tuner、I2S、recheck、native mapping。
  麦克风只借用 xo&7==4（软件已开）的状态，不算自己拥有，restore 时不关；其他值拒绝并打印。报告里加了 xo/sleep/gate 前后值。
  镜像 `x9/out/x9-mic4-20261113/boot.img`（参数同 mic1）。
- **日志 265（mic4，卡死）**：最后一条是 `X9 SPK CLK: map done, AO snapshot`，`APLL capture` 没打出来，modem 日志还多打了 0.36 s。
  卡死点就在 `take()` 的 `ao_snapshot()`/冷态检查这一段：HWV 0x20708/0x13700/0x12900/0x1132c、foreign() 的
  0x1c040000+{0x30708..0x60708,0x70708..0x74708}、VLP 0x90/0x274、0x1131c/0x13704，以及 `x9_adsp_mt6993_speaker_validate`。
  ADSP 路径（log 264/265 成功）只读过 0x20708/0x1132c/0x13700/0x13704，所以 0x12900、foreign 那一组、VLP 是新的访问（推断）。
  麦克风：`xo=11`（BBCK5 在硬件/SRCLKEN 控制下，codec 一直开着），被拒。
- **mic5**：`rd/wr` 在 trace 窗口里（从 map done 到 APLL capture）先打印 `X9 SPK CLK rd|wr <bank>+<off> (<phys>)` 再访问，每次停 300 ms；
  speaker_validate 前也单独打一行。麦克风把 xo&7==1 也当作借用（不写也不撤）。镜像 `x9/out/x9-mic5-20261113/boot.img`。
- **日志 266（mic5，没卡死）**：XO 借用放行（`xo=11`），麦克风走到 board stage 5（CODEC）：第一次读 MT6681（reg 0x31 ACDIF_MON0，
  地址 0x6b）收到 NACK（STAT=3），清标志后 START 回不到 0，bank_idle 报 EBUSY(START)，`bus_stuck` 置位，restore 做不了，lease 被扣住。
  扬声器 `ticket -> 16` 没跑，所以没卡死，HWV 访问追踪也就没触发。
  OPPO 对照：`mfd/mt6681-core.c` 探测时不碰 codec-en（LK 已经开着），直接读 SWCID_H；只有 ID 重试失败才拉 EN_OFF/EN_ON，
  然后边轮询 ACDIF_MON0 边容忍读失败，最长 50 ms。`i2c-mt65xx.c` 遇到 ACKERR/HS_NACKERR 会调 `mtk_i2c_init_hw()`：
  SOFTRESET(0x50)=1，再全部重写配置。
- **mic6**：继承的 GPIO9 输出高直接沿用（不打复位脉冲，不算自己拥有，restore 保持原样）。NACK 后 SOFTRESET=1，
  再把 journal 里每个寄存器写回当前预期值（发过的写我们的值，没发过的写原值）。ACDIF_MON0 就绪轮询期间 EIO 重试到 50 ms。
  镜像 `x9/out/x9-mic6-20261113/boot.img`。
- **日志 267（mic6，没卡死）**：沿用 LK 已开的 codec-en 之后，**codec 第一次应答**（读 0x31，STAT=1 ACK）。但清完标志、写 START=0 后，
  `bank_idle` 仍读到 START 非 0，报 EBUSY(START)，`bus_stuck` 置位，后续传输 ENXIO，lease 又被扣住，扬声器没跑。
  推断是 START bit14（auto-restart 配置位）写 0 清不掉，bit0 那时已经是 0（传输前就检查过）。
- **mic7**：`bank_idle` 对 START 只看 bit0（传输进行中位）。前两次传输打印 `scp-i2c1 xfer reg= status= start after clear= fifo=`，
  确认 START 实际值。镜像 `x9/out/x9-mic7-20261113/boot.img`。
- **日志 268（mic7，卡死，第二次采集）**：
  - **麦克风**：START 读回 `4002`（bit0=0，推断成立），codec 读 0x31/0xb 都 ACK，`id=81 ready=1b`，MT6681 冷/活跃初始化全部写完。
    停在 codec stage 3（INIT）的 VAUD18：`LDO_VAUD18_MON(0x1b8a)=03`，只有 bandgap EN/STB，LDO 的 EN/STB（bit3/4）
    10 ms 内没起来，ETIMEDOUT(60)。这次 restore 干净，lease 已释放。
  - **扬声器卡死点精确定位**：trace 最后一行 `X9 SPK CLK rd hwv+40708 (1c080708)`，打印在访问之前。前一个
    `hwv+30708 (1c070708)` 读成功，读 0x1c080708 时整机停住。原因：`foreign()` 把每个 XPU 的 EN 当成
    ap_hwv(0x1c040000)+{0x20000+n*0x10000 | 0x70000+(n-5)*0x1000}+0x708；这套偏移其实属于 mm_hwv(0x31c00000)
    （DT `mm_hwv: syscon@31c00000`，相机驱动按 0x31c30000+base-0x30000 读）。麦克风的 `afe_foreign_votes()` 是同一个公式，
    走到 AFE 时也会卡死。
- **mic8**：两处 foreign 都改成看汇总寄存器 `PD_ALL(0x13700) & ~AP_EN(0x20708)`（扬声器 mask 7，麦克风 mask 1），
  这两个寄存器 ADSP 路径已经安全读过。VAUD18：照 OPPO `mt_vaud18_event` 先写 `OP_EN1_SET(0x1b8f)=0x40`（HW14，OPPO 从不清），
  等待放宽到 50 ms，超时打印 `X9 MIC VAUD18:` 寄存器。镜像 `x9/out/x9-mic8-20261113/boot.img`。
- **日志 269（mic8，没卡死）**：**整机卡死已修复**：foreign 改读汇总寄存器之后，扬声器时钟段走完冷态检查，
  正常失败并完整 teardown（PA 关、dsp halt、i2c 关），系统一直跑到 207 s。
  - 扬声器：cold check 拒绝（EBUSY）。DSPBOOT `ap=3ce0f all=3ce7f`：AUDIO bit0 已由 AP 投票开着，teardown 后（`owned=0`）依然开着，
    所以是 LK 留下的。trace 里打印的 `0` 是传进去的参数，不是读回值（已修）。
  - 麦克风：`X9 MIC VAUD18: con0=01 con2=1c mon=03 op_en0=01 op_en1=c0 op_cfg0=01 op_cfg1=00`。查 `mt6681-private.h`：
    MON bit0/1 是 `DA_VAUD18_B_EN/B_STB`（主级），bit3/4 是 `DA_VAUD18_L_EN/L_STB`（低功耗级），不是"带隙+LDO"。
    OPPO 的 regulator 路径从不轮询 MON。所以 `mon=03` 就表示 VAUD18 已开且稳定，旧判据 0x1b 是错的。
- **mic9**：VAUD18 只看 `(mon&3)==3`。扬声器在 combined 模式下沿用 LK 已开的 AUDIO domain
  （AP EN/PD_ALL/PM ACK bit0 全为 1、不 busy、没有外部投票），记为 borrowed，从不撤票；
  `native_restored_ao` 对 borrowed 要求它仍保持开着。trace 每次读后打印实际值。镜像 `x9/out/x9-mic9-20261113/boot.img`。
- **日志 270（mic9）**：日志在 82 s 结束（扬声器刚开 i2c6 会话，是否卡死待用户确认）。麦克风：VAUD18 通过，
  codec 上电完成（board flags=7 POWER_OK）；停在 AFE stage 1（BASELINE）EBUSY：`AP=3ce0f ACK=3ce7f VLP=2020301`，
  AUDIO 是 LK 留下的开着状态，两个 VLP mux 也开着（PDN bit7/bit15 清零，父时钟 sel=1/3）。
- **mic10**：麦克风 AFE 和扬声器时钟用同一套接管规则：AUDIO（AP EN/PD_ALL/ACK bit0 全 1、不 busy、无外部票）借用，
  并检查 SSYSPM PWR_ACK=1、PROTECT=0；每个 VLP mux 如果 PDN 清零且 FENC 置位，就按现状借用（扬声器把 expected 记成观测到的 sel），
  冷的照旧 take（parent 0），其他状态拒绝。借用的东西不改、不还原、不撤票。镜像 `x9/out/x9-mic10-20261113/boot.img`。


- **log 271 / mic11 (2026-11-13)**: mic adopted the LK-inherited AUDIO domain and both VLP muxes (borrowed), then timed out at stage 3 waiting on SPM AUDIO_SSYSPM_CON 0x903c (want PWR_ACK=1, BUS_PROTECT_EN/RDY=0; bit names confirmed in OPPO v6993/spm_reg.h). It was marked stuck, the lease stayed held and the speaker got ticket EBUSY. No hang, the system ran to 181 s. mic11 prints the raw 0x903c value with decoded bits. A refusal on the borrowed path is now non-stuck (it is a read-only observation; restore unwinds our own AO/infra votes and frees the lease).
- **log 272 / mic12 (2026-11-13)**: inherited AUDIO SSYSPM 0x903c=80f0100c, i.e. PWR_ACK=1 with BUS_PROTECT_EN=1 and RDY=1. LK leaves AUDIO powered but bus-isolated, so the AFE is unreachable. The mic refused cleanly (non-stuck, lease freed). The speaker then ran: TFA init OK for both PAs, DSP warm boot OK. It hung right after CLK native board, before map done (in 269 the same path passed). mic12: the mic cycles the sole AP vote (withdraw, then wait ACK/PD_ALL/PWR_ACK off, then cold HWV vote, which releases protection), as Linux genpd/AFE probe does, and only with no foreign or busy votes. The speaker refuses an inherited AUDIO domain that is still bus-protected and prints the native board result.
- **log 273 / mic13 (2026-11-13)**: the mic vote cycle worked. AUDIO went fully off (0x903c=f0100c) and HWV re-powered it (80f0100c), but bits 2/3 (BUS_PROTECT_EN/RDY) stayed 1 in both states. In HWV mode they are static configuration, not a blocking state; the 271/272 refusals were a misread. The re-power wait timed out on those bits, the mic retained the lease and the speaker got ticket EBUSY. No hang (ran to 225 s). mic13: PWR_ACK-only qualification on the mic and speaker (bits still logged); the vote cycle is removed and the inherited domain is borrowed when PWR_ACK=1. The mic will now proceed to its first AFE access (idle/gates/SMC 7/SMC 3).
- **log 274 / mic14 (2026-11-13)**: (273 was mic12; 274 is mic13.) The mic qualified the inherited AUDIO domain (PWR_ACK) and reached AFE stage 4. afe_access refused with EBUSY, reg=0: it required VLP 0x90&0x8383==0 (parent 0), but the borrowed muxes are aud_intbus=1 (vlp_clksq) and audio_h=3 (apll2) (OPPO clk-mt6993.c parent lists). mic14: a borrowed mux must keep exactly its observed parent; afe_access refusals are logged.
- **log 275 / mic15 (2026-11-13)**: the mic passed afe_access and did real AFE work: gates, SMC 7/3, engine, pads, then calibration. MTKAIF calibration phase 0 passed; phase 1 got EIO because TEST_MON DONE (bits 28/29) stays latched after TEST_ON=0. OPPO mt6993-mt6681.c never waits for it to retire: it raises TEST_ON again and polls. The mic restored cleanly (restore=0). The speaker then ran PA init and DSP warm boot OK, and the SoC hung inside the 600 ms sleep after CLK native board -> 0, before take(). 272 hung at the same point. Both had the mic AFE init SMC before the ADSP launch; 269 (no mic AFE init) ran the DSP 7 s without a hang. mic15: the calibration DONE pre-wait is observe-only (as OPPO). The speaker runs first by default (x9_spk_first, default 1; boot-args are at the 607-byte limit), and the mic runs 1 s after the speaker finishes.
- **log 276 / mic16 (2026-11-13)**: with the speaker first, nothing hung. That confirms the 272/275 hang needs the mic AFE init (SMC 0 plus AFE work) before the ADSP launch. The speaker passed native board, map, AO snapshot, the inherited AUDIO borrow (0x903c=80f0100c PWR_ACK ok) and adsp validate. pll cold refused with EBUSY reg=vlp+0x80 val=0x20303: LK leaves APLL2 running with aud_engen2 enabled on parent 3 and audio_h on parent 3 (apll2), which is exactly what pll_take would build. Clock release then returned ENXIO: the dl24_drained callback runs with no AFE pointer, because acquire failed before configured. The lease stayed held and the mic (run after) got EBUSY. mic16: borrowed APLL2 mode (full topology plus PCW 78fd5264/5 check, no PLL/mux writes, fmeter-only measurement of engen2 49.152M and audio_h 196.608M). Release skips the drain callback when the AFE was never handed out (!configured). The restored-AO check expects the initial ENGEN2 FENC.
- **log 277 / mic17 (2026-11-13)**: the SoC hung 0.35 s after the ADSP launch (dsp boot -> 0) with no mic run and no AP ADSP access (inside the SPKLOG sleep). Survey: every boot that launched the ADSP (253-256, 264, 265, 268, 272, 275, 277) died 0.4-25 s later; only 269 and 276, which halted the DSP after 7-10 s, survived. The 272/275 mic-AFE-init theory is withdrawn, and 264/265/268 also fit this pattern. OPPO adsp_system_bootup additionally does feature registration, after_bootup (SLB release loop, awake unlock), the SLB_INIT IPI handler, the WDT IRQ, the logger init message and the suspend flow; none of that is implemented here. mic17: speaker bring-up (which launches the DSP) is opt-in (x9_spk_boot default 0); the mic runs alone to exercise the calibration fix.
- **log 278 / mic18 (2026-11-13)**: no hang (the user stopped capture; mic only, no DSP launch), so the ADSP warm launch theory stands. MTKAIF calibration completed: 43 phases, chosen 3/4, cycles 3/3, AFE err=0. Analog reached the RC stage (HWCID 81/10) and failed closed on rc_tune_l=0x1f (AUDENC_PMU_CON32). OPPO mt6681.c toggles RG_AUDADCLPWRUP once on 0/0x1f, re-reads and continues regardless. mic18 does the same; digital requires PREPARED|READBACK (RC_OK stays diagnostic).
- **log 279 / mic19 (2026-11-13)**: no hang. Analog completed (stage 11, err 0; rc_tune_l stays 1f after the OPPO reset, continued). The digital DMA preflight refused with EBUSY at VUL1_MON0 (0x4db4): VUL1 is off (CON0=0x100048, CUR=0, no memif on) but MON0=0x100000. Its bit 20 mirrors CON0 MINLEN=1; OPPO gives MON0 no fields and our BUSY mask (0xf<<20) was a guess. mic19: VUL1_MON_ACTIVE discounts exactly the configured MAXLEN/MINLEN bits (CON0&0x330000) in preflight, start check, drain and quiesce.

### mic20 (2026-11-13) — static review vs OPPO after log 280

Log 280: digital stage 8 EIO on MTKAIFV4_MON0=0x200008 (bit21 RXIF search fail,
same on ADDA6 MON0). Calibration (43 phases, 3/4, cycles 3/3) and analog ok.

Statically checked against OPPO mt6993-dai-adda.c / mt6993-mt6681.c / afe-pcm.c:
- AUDIO_TOP gates (clk-mt6993-adsp.c): 0x0=CON0 (MTKAIFV4 bit25), 0x4 UL ADC,
  0x8 ETDM, 0xc ASRC, 0x10 F26M/APLL. No memif/VUL1 gate exists; ours opens
  bit25, UL0_ADC bit23, 0x10 bits0-1. OK.
- TX_CFG0 OUT_EN_SEL: OPPO sets it only in the playback hw_params; capture only
  raises AFE_ON via AUD_PAD_CLK. Ours (0x0851) matches capture. No change.
- Board DT: both aud-dat-miso0-on (GPIO102) and miso1-on (GPIO103) exist, so
  both need calibration and no SYNC_WORD disable. Matches.
- VUL1: regs, ON bit28, fs 0xa, NORMAL_MODE bit3, min/max 64B (3/3), S16
  stereo. Matches (WR_SIGN is never touched by OPPO; ours clears it, S16 only).
- UL0_SRC_CON1 FIFO_SOFT_RST: OPPO uses it only on the AP-DMIC route.
Fixed in mic20:
1. MTKAIF0/1_CFG0: full write 0x80010000 (OPPO regmap_write 0x10000 + CLKINV),
   not a masked update.
2. MTKAIF1_RX_CFG2 (0x1200) was never written. OPPO aligns ch3 (phase_cycle[2],
   never calibrated, so 0) with ch2, giving DELAY_DATA=MISO2(1) and
   cycle=phase_cycle[1]. Now written (0x3100 for cycle 3).
3. RXIF search-fail bit21 is no longer fatal in digital_afe_enable or
   signal_observe. OPPO only dumps MON0/MON1.
4. New prints: "X9 MIC MTKAIF:" (CFG/RXCFG2/DATA/TX/RX/PAD/UL/ENGEN/TOP0-4/MON)
   and "X9 MIC SIGNAL:" (MON first/last, UL raw, change counts).
Image: x9/out/x9-mic20-20261113/boot.img (same boot args as mic19).

### mic21 (2026-11-13) — log 281: DMA runs, data all zero

Log 281 (mic20): first time VUL1 DMA ran. CUR moved, 48160 frames in 1 s,
capture -> 0. All samples 0. AP UL0 shadow raw stays c0000000/60000000 (SRC on,
data 0). MTKAIFV4 MON0 and ADDA6 MON0 = 0x200008 (search fail) throughout.
MTKAIF dump: CFG 80010000/80010000, RXCFG2 6300000/6303100, TOP0=0 TOP1=555000.
The dma err=70 (ESTALE) is benign: the worker delivered one 16-frame chunk after
the 1 s window closed (x9_mic_deliver rejects once RUNNING is cleared).
Statically confirmed:
- live DT: audio-r-miso1-enable=<1>, accdet-efuse-check=<0>, so R_MISO1.
- AP pins 98/100/101/102/103 func1 match live DT.
- Codec AIFTX_Supply chain (CLKSQ+AUDENC, CKPDN AUDIF/AUD, SRAM, AUDIO_TOP0-3
  dbf/00/c1, AFE_ON, UL_DL AFE_ON, TX 0x29/0x2b, PAD 0x39, GPIO MODE4 MISO1)
  matches OPPO.
- Calibration uses codec dat_miso_loopback, so it proves the pad/line and clock,
  not codec TX.
Changed in mic21:
1. Codec UL_SRC_CON0_1: IIR_ON=1 (OPPO mt6681_set_ul_src), not bypass.
2. New x9_mic_mt6681_dump(): the OPPO final_dump register set ("X9 MIC CODEC:")
   plus 6 samples of codec UL_SRC_MON0 (0x256a-d) and codec MTKAIFV4_MON0
   0x2c9d/0x2c9e (bit6 TXIF_SDATA_OUT) / ADDA6 0x2ca3/4 ("X9 MIC CODEC MON[k]").
3. AP SIGNAL line now counts the RXIF_SDATA_IN (bit22) high samples.
How to read it: codec ULMON changing means the ADC works; AIF bit6 toggling
means codec TX is live; sdata_in_high>0 means the AP pad sees the line.

### spk22 (2026-11-13) — speaker without launching the ADSP (mic21 + speaker)

Why: every ADSP launch hung the SoC (logs 253-277). DL24 -> ETDM OUT4 -> TFA9865
is AP-programmed and does not need the DSP, so the board is now held "AP-only".
Statically checked against OPPO mt6993-dai-i2s.c / afe-pcm.c / afe-clk.c / reg.h:
- OUT4 CON0 0x1580 (I2S fmt0, bit_len 15, word_len 31, ch 2, relatch APLL 1),
  CON1 init_count 5 / init_point 6 / LRCK_RESET bit15, CON2 in2latch 6 (BCK
  < 11.2896M), CON4 FS 5 / clk src APLL bit6 / relatch_en 10 at bit24. Match.
- Cowork 0x1690 OUT4_SLAVE_SEL bits8-11 = ETDMIN4_MASTER 0. Match.
- Pad-top (OUT4 is_etdm_in_pad_top): AFE 0x98 bit5 CK_EN=1, bit20=0. Present in
  x9_speaker_afe.c config.
- DL24 BASE/CUR/END(+MSB) 0x46b0-0x46c4, CON0 0x46d0 (ON 28, MINLEN 20, MAXLEN
  16, FS 8, PBUF 5, MONO 4, NORMAL 3), routes CONN116_2/117_2 bits16/17
  (I_DL24_CH1/2 = 80/81-64, in mtk_i2sout4_ch1/ch2_mix). Match.
Found and fixed (would have failed for sure):
1. AUDIO_ENGEN_CON0 bit0 (AUDIO_26M_EN_ON, OPPO runtime_resume "enable AFE")
   was never set by the speaker; only bit3 (APLL2). Now 0x14 mask 9.
2. AFE_SPM_CONTROL_REQ: SRCCLKENA bit2 + DRAM request bits 3/6/7 (OPPO
   afe_dram_request; the mic's VUL1 needs the same) were never set. Now 0xcc.
   AFE_CBIP_CFG0 bit0 also set (resume). All in the native clock journal,
   restored in reverse; ENGEN off waits MON 0x7c==0 (as the mic).
3. TOP_CON2 0x8 reads 0 (log 281 mic dump): LK leaves ETDM clocks ungated, but
   native_tuner_take required bit17=1 -> EBUSY. 0 and 1 are both accepted now.
   An already-enabled APLL2 tuner is accepted only in exactly our image 0x375.
Changes:
- x9_adsp_mt6993: speaker_control op 3 = AP-only hold (power_on only: adopt LK
  votes, exclusive, borrowed ticket; never SYS_CLEAR/RELOAD/CORE_START), op 4 =
  release (withdraws only TOP/INFRA votes we set). clock_hold and halted
  qualification accept the AP-only hold. New speaker_online(ticket,gen),
  speaker_ap_only(), speaker_generation(ticket) (minted per op 3).
- Clock child, leaf holds and pads use speaker_online instead of transport
  Online. Pads accept each pin mode0 or already mode5 (DIR output only if all
  four are mode5). AFE baselines inherited O116/O117 inputs (our bank must be 0).
- Bringup: op 3 -> gen -> clock -> pads -> AFE -> PA INIT_CF -> silence; teardown
  uses op 4 (no halt). AMPE stays 0, silence only.
- SweetX9Audio defaults: x9_spk_boot=1, x9_spk_first=0 (mic first, 1 s gap,
  then speaker).
New log lines: "X9 ADSP AP-ONLY: hold", "X9 SPK: board gen=.. online",
"X9 SPK CLK: inherited spm/cbip/tuner/engen/top2/top4", "X9 SPK PLL: APLL2
inherited running", "X9 SPK PADS: inherited", "X9 SPK AFE: inherited O116".
Untested on hardware past log 276's pll cold: borrowed-APLL2 meter, smc 0/7,
tuner, pads, AFE start, PA CLKS.
Image: x9/out/x9-spk22-20261113/boot.img (same boot args as mic21).

### log 282 / spk23 (2026-11-14) — speaker first again

log 282 (spk22, `~/282.txt` on the Mac): the mic ran first and finished
(result=0, 80.5 s). The speaker then passed PA34/PA35 init, `ADSP AP-ONLY:
hold -> 0` (adopted LK votes inherited=e, so chain_on wrote nothing), `board
gen=40000001 online -> 0`, `clock acquire begin` and `CLK native board -> 0`
(97.894 s). After that the whole SoC hung: the 2 s GPU GOV and DISPLAY cadence
lines (due at 98.33/98.50) never came. `SPK PLL: APLL2 ... borrowed` never
printed, so take() never got past its map step. Remember that the last ~1 KiB
of console is lost on a hang (log 260), so the real stop is somewhere from
97.9 s to roughly the start of take().
Observation: this is the same apparent stop as 272 and 275, which were also
mic first. 282 did NOT launch the ADSP, so 272/275/282 are not explained by
the 277 launch hang. Inference (unproven): mic AFE work followed by the
speaker path hangs the SoC; 276 (speaker first) did not hang.
spk23: the only change is the default `x9_spk_first` 0 -> 1 (speaker first,
mic 1 s after it; `x9_spk_first=0` gives mic first). Same boot args as spk22.
If the speaker fails and keeps its lease, the mic gets EBUSY (as in 276).
Reading the log: no hang plus `SPK PLL: APLL2 inherited running ... borrowed`
means the order theory holds, and the speaker clock path gets its first test
past 276's pll cold (follow the spk22 checklist above). A hang at the same point
means the AP-only path hangs without the mic as well.

### log 283 / spk24 (2026-11-14) — static audit of the whole speaker path

log 283 (spk23, speaker first): no hang (as 276). The speaker passed PA init,
AP-only hold, native board, the AO snapshot, the AUDIO borrow (0x903c=80f0100c)
and adsp validate. Then `pll cold` failed with `clock acquire -> 16 reg=80
val=20303` (the same value as 276). `clock release -> 16` kept the lease and
the mic got EBUSY.
Audit against OPPO mt6993-afe-clk.c, clk-mt6993.c, clk-pll.c, clk-mux.c,
clk-fmeter-mt6993.c, pinctrl-mt6993.c and tfa98xx_v6.14.2 (tfa986x):
1. Borrowed-APLL2 check: CON0/CON1 bit31/CON3/TUNER bit0 and PCW_CON0/1 =
   0x78fd5264/5 are OPPO's own mt6993 apll_init/apll_enable values (an
   intermediate spk24 commit wrongly called them mt6991 leftovers; corrected
   here). The unproven part is postdiv: the check required CON1 bits24-26 = 0,
   but engen2 = apll2_d4 = 49.152M means apll2 = 196.608M, which needs /16
   (postdiv 4) with that PCW. LK also need not follow OPPO's enable sequence.
   Neither 276 nor 283 printed any APLL register.
2. On a borrow refusal pll_cold fell into the cold path, whose first check
   (vlp+0x80 bit15) must fail on LK's 0x20303, so it overwrote the real reason.
3. take() recorded s.borrowed_vote only at the domain-vote step, so a failure
   before it made native_restored_ao demand AUDIO off -> release EBUSY ->
   lease held -> mic EBUSY.
4. AFE 0x10 (AUDIO_TOP_CON4) bits 3/4/12 (CG_APLL2, CG_H208M, PDN_TUNER2) were
   required gated; LK's value was never logged, and the refusal set no
   last_register.
Verified OK (no change): VLP mux parents (engen2 idx3 = apll2_d4, audio_h idx3
= apll2_ck), FENC bits (0x270 b2, 0x274 b30/b31), CKSTA busy semantics (0x330
b2, 0x334 b29/30 must be 0), fmeter ids 30/33/34 with 49152/26000/196608 kHz,
SMC 0 = INIT (OPPO probe) and 7 = DOMAIN_SIDEBANDS (runtime resume), native
tuner list (SPM 0xcc, CBIP, TUNER 0x375 = OPPO 0x374|EN, ENGEN bits 0/3,
OUT4 CG), LK ENGEN_CON0 low bits are 0 (mic afe_idle passed in 282), I2S4
pads GPIO108-111 (mode 0x4b0, dir 0x30, IOC base 9 = 0x12860000, PU/PD/SMT/
IES bits 4/6/5/8 = 0x170, DRV), TFA986x MANSCONF 0x0120 / PWDN 0x0000 /
CLKS 0x1100 and OPPO's INIT_CF order.
spk24 fixes: borrow = FENC 0x34 bit4 + PCW_CON0 0x78fd5264 + the unchanged VLP
topology (CON0/CON1/CON3/TUNER/PCW_CON1 and the postdiv-derived rate are
printed, not required; the fmeter measures engen2/audio_h). The cold path is
unchanged. pll_cold prints `X9 SPK PLL: apll2 ...`, and a borrow refusal is
returned as-is (`X9 SPK PLL: borrow refused reg= val=`) when the engen2 mux is
enabled. The AUDIO borrow is recorded when it is adopted. AFE 0x10 is accepted
in any inherited state (journaled and restored) and printed with top0/engen/
spm (`X9 SPK CLK: inherited top4=`).
Reading the spk24 log: `SPK PLL: apll2 ...` gives the real APLL2 registers;
then `APLL2 inherited running ... borrowed`, `pll take`, `tuner take`
(`inherited spm=...`), `clock acquire -> 0`, `SPK PADS: inherited ...`,
`pads acquire -> 0`, `afe start (silence) -> 0`, `PA34/PA35 clocks -> 0`,
`tone ... played=` growing, then the mic runs (no EBUSY).

### log 284 / spk25 (2026-11-14) — fmeter HW mode, speaker-only image

log 284 (spk24): the borrowed APLL2 check passed. First real APLL2 readout:
`con0=3b10 con1=4000000 con3=80000002 fenc=3d00d731 tuner=110000
pcw=78fd5264/78fd5265 rate=196607999 vlp80=20303 270=fff7ffff 330=0`, so
postdiv is 4, which confirms that the old postdiv-0 requirement was wrong.
Then `clock acquire -> 16 reg=c val=1`: meter_idle refused VLP fmeter CON3
bit0 = 1. This time release worked (`clock release -> 0`, AP-only release 0,
ticket release 0). The mic then started (`native DMA bind -> 0`, 101.24 s)
and the SoC hung about 0.2-0.4 s later, inside the mic's IOSleep. The ~1 KiB
console tail was lost.
Hang pattern (observation): 282 (mic run, then speaker AP-only hold: hang
2.4 s later) and 284 (speaker hold+release, then mic: hang about 0.4 s
later). In both, both threads were sleeping. No hang when only one of them
touched hardware: 276/283 (mic EBUSY) and 278-281 (mic only). Ruled out
statically: SPI570 (armed only by the DSP PCM bind), mic afe_worker (polls
only while capturing), speaker AFE watchdog (starts at afe register, not
reached), SweetX9Audio worker (only while running), PMIF (global lock, no
retained state), and the DSPBOOT ffffffff fields (placeholders when not
held). Cause unknown.
Fmeter (OPPO clk-fmeter-mt6993.c fqmtr_cal): backup CON0-3, clear CON3 bit0
(HW mode), wait CON6 bit0, measure, restore CON0-3. So HW mode 1 is the
resting state. spk25: meter_hw_off/meter_hw_restore (CON3 cleared for the
cycle, restored in pll_restore); CON0 0x9110 and vlp+0x344 are no longer
required to be 0 (journaled from the post-HW-off image and restored);
`X9 SPK PLL: fmeter con0..con6` is printed. The meter programming itself
matches OPPO (CON0 0x0410_9000, CON1 load 0x3ff, CON2 div1, trigger bit4,
CON6 busy, ids 30/33/34).
spk25 is built WITHOUT x9_mic_boot (speaker only), so the speaker path runs
to the end apart from the mic interaction hang. Boot args: x9_audio=2
x9_dsp_boot=75 x9_mic=2.

### log 285 / spk26 (2026-11-14) — ETDM IN4 is the I2S4 clock master

log 285 (spk25, speaker only): no hang. The whole clock/pads/AFE path passed
(`clock acquire -> 0`, `pads acquire -> 0`, `afe prepare -> 0`, `afe start
(silence) -> 0`, DL24 consuming: played=496), then `PA34 clocks -> 60`:
CLKS (0x1100) stayed 0 for 50 ms (stat11=0018). Teardown/release all 0.
Cause (static, OPPO mt6993-dai-i2s.c): GPIO108/109 are I2SIN4_BCK/LRCK, so
the bit clock and word clock on the pads come from ETDM **IN4**, and spk25
never touched IN4. OPPO: `[DAI_I2SOUT4].share_i2s_id = MT6993_DAI_I2S_IN4`
by default (the live DT has no `i2sout4-share`), so mtk_dai_i2s_config(OUT4)
also runs the IN branch for IN4. The I2SOUT4 route powers `I2SIN4_GPIO`
(en_event: ch-1, ip_mode, DT sync=1, slave 0) and `I2SIN4_EN` (share
connect). DAPM powers supplies in register order, so IN4 EN (0x13c0) comes
before OUT4 EN (0x1580). HD_EN is NOPM (APLL only), MCLK is off (DT mclk 0),
and AUDIO_TOP_CON2 ETDM gates were already 0 (`top2=0`).
spk26 IN4 profile (x9_speaker_in4.h, all from mt6993-reg.h/dai-i2s.c):
CON0 0x13c0 sync=1 slave=0 fmt=0 bit_len=15 word_len=31 ch=1 relatch_dom=1;
CON1 init_count=5 init_point=3 (pad top, master) lrck_reset=1; CON2 clk_src
APLL=1, ck_en_auto=1, multi_ip=0; CON3 fs_timing=5; CON4 relatch_sel=10;
CON8 afifo off; CON9 out2latch=6; cowork 0x1690 OUT4_SLAVE_SEL[11:8]=0
(IN4_MASTER) + IN4_SLAVE_SEL[27:24]=8 (OUT4_MASTER); 0x98 PAD_TOP_CK_EN
bit5=1, OUT/IN BCK_NEG_EG_LATCH bits20/21=0.
AFE engine: journal is now 20 entries with named indices (IN4 13-19);
prepare refuses an inherited IN4 EN; start enables IN4, OUT4, then memif;
disable reverses (memif, OUT4, IN4); restore puts back every saved word.
New logs: `X9 SPK AFE: inherited in4 ... cowork= pad=` and `X9 SPK AFE:
enabled -> e in4= out4= in4mon= cowork= pad= memif=`. Bringup: the CLKS
wait is 200 ms and keeps feeding silence (a bare wait would drain the ring
and trip the AFE watchdog); on timeout it prints `PA.. no-clks` status.
Clock/pads validators do not check IN4/0x1690/0x98 (checked statically);
clock idle() requires 0x13c0 bit0=0 only at release, after AFE disable.
spk26 build: `x9/out/x9-spk26-20261114/boot.img` (speaker only, boot args
x9_audio=2 x9_dsp_boot=75 x9_mic=2). The speaker+mic hang is still open.

### log 286 (spk26, 2026-11-14) — PA clocks lock; full speaker path runs

First complete speaker run, no hang (mic not run), done result=0, every
release step 0. `X9 SPK AFE: inherited in4 9ff800/800984a5/410c062/1f80000/
100/40004000/1000 cowork=81b2a908 pad=0` (LK left IN4 configured, disabled;
cowork OUT4 sel 9 / IN4 sel 1), `enabled -> 0 in4=109f7803 out4=109f7801
in4mon=346f0220 cowork=88b2a008 pad=20`. `PA34 clocks -> 0`, `PA35 clocks
-> 0` within ~6 ms each. The DMA ran at the right rate: 97952 frames played
in about 2.04 s, tone loop result 0 (the PCM was all zero).
PA status after power-up (both PAs identical): stat11=0e89 means CLKS=1,
PLLS=1, AREFS=1, SWS=1, AMPS=1, LPMS=1, TDMERR=0, TDMLUTER=0. stat12=0929
means MANSTATE=9, AMPSTE=2, TDMSTAT=1. flags10=7eaf: NOCLK sticky (set
before the clocks came), no OCP/OTP/UVP/OVP, CLKOOR=0.
Observation: sysctrl went from f670 (285, no clocks) to **f678**, so AMPE
(0x0030) read back as 1 and the amplifier was engaged. Our code writes
register 0 only through pa_mute (AMPE=0, read back 0 at AFE acquire) and the
PWDN=0 RMW. Nothing touched the TFA between the CLKS wait and the `powered`
read. So the amplifier engaged on its own once the clocks arrived (guess:
the TFA986x manager with MANSCONF=1; OPPO sets AMPE itself in
TFA_STATE_OPERATING). spk26 therefore did NOT keep "AMPE stays 0": it played
digital silence through an engaged amplifier for about 2 s. Teardown still
wrote AMPE=0 PWDN=1. Next step: either force AMPE=0 again after CLKS and
read it back for a truly quiet run, or proceed to the low-level tone.

### spk27 (2026-11-14) — quiet 1 kHz tone, then the mic full flow

Static audit before combining speaker + mic again (the 282/284 hangs):
- Found: MTK_AUDIO_SMC_OP_INIT (op 0) is issued by OPPO only once, in
  mt6993_afe_pcm_dev_probe. The mic guarded it with a private
  afe_init_ready flag, but the speaker clock child issued `smc 0` on every
  acquire, so a full speaker run followed by the mic replayed the secure INIT.
  Fixed: x9_audio_afe_init_begin/end (x9_audio_resources.c) is one boot-wide
  state shared by both. A second user prints `... AFE init already done this
  boot, not replayed`. Ops 7 (DOMAIN_SIDEBANDS, every OPPO enable_clock) and
  3/4 (SRAM request/release, runtime resume/suspend) may repeat, as in OPPO.
- This does NOT explain 282/284 (both hangs come before any second smc 0:
  in 284 the speaker failed in pll cold, before its smc 0; in 282 the hang
  is in the IOSleep(600) right after `native board -> 0`, about 3 s before
  the speaker's smc 0). In both, every audio thread was sleeping when the SoC
  stopped. Background agents checked: mic afe_worker (idle unless active),
  speaker AFE watchdog (blocks unless running), SPI570 IRQ worker (DSP
  playback only, not on this path); mic DMA bind does no MMIO. Root cause is
  still unknown.
- Hand-off state checked for mic after speaker: the speaker release restores
  AFE 0x10/0x14/0x60/0x4380, IN4/OUT4 CON, cowork 0x1690 and pad 0x98 to the
  LK values, which is what mic afe_idle/afe_dram_take/afe_access require.
  VLP muxes stay borrowed by both, the pins are disjoint (mic 98/100-103,
  speaker 108-111), and the mic needs no APLL.
Tone (TFA986x rev 0x66 is a probus device: OPPO OPERATING and unmute set
AMPE=1, set_mute is tfa_set_mute_nodsp, so mute = AMPE=0): after CLKS both
PAs get AMPE=1 with readback (`X9 SPK: PA34 AMPE=1 -> 0`,
`PA.. amp-on ...`), then 1 kHz at -24 dB (>>4) for 2 s, mute (AMPE=0), stop.
`x9_spk_tone=0` keeps the silent run. Removed the unconditional 5x960 submit
after CLKS (it could overfill the ring and return EAGAIN).
spk27: `x9/out/x9-spk27-20261114/boot.img`, boot args x9_audio=2
x9_dsp_boot=75 x9_mic=2 x9_mic_boot=1000 (speaker first, mic 1 s after).
If it hangs again, the USB console loses about 1 KiB at the end, so also pull
the UFS sidecar `x9-bootlog-*/console.bin` for the tail.

### log 287 + Android regmap / spk28 + mic28 (2026-11-14) — mic ADC clock bug; speaker is the iOS output

log 287 (spk27): the 1 kHz tone played (user: too loud at -24 dBFS). Release
all 0. The mic then hung ~0.2 s after `native DMA bind -> 0`, same as 284.
Android reference (read only via adb root: debugfs regmap 15-006b during two
recordings; captures under x9/out/android-audio-ref/, not committed):
Android records in OPPO `mic_hifi_mode` with 4 PGAs (L=AIN2, R=AIN0, 3, 4),
GAIN_1P5=4/CARA=2, CON18/19=0x2a, RC read 0x08-0x09, UL_SRC_MON0=df ff a2 38
(ON_MON=1, COMB live). Codec digital (UL SRC, TOP, PAD, TX, CLKSQ) matches ours.
**Mic root cause (static, OPPO mt6681.c mt_adc_lr_clk_gen_event):** OPPO writes
`0x5 << RG_AUDADCLCLKSOURCE_SFT` under the COMBINED CLKGENMODE|CLKSOURCE mask,
i.e. GENMODE(bit5)=1 SOURCE(bits3-4)=1. Our port split it into two fields:
GENMODE=0 and SOURCE=5 masked to 1, so the ADC L/R clockgen never ran. That
matches every symptom: RC tune 0x1f even after the ADC reset (278-282), and
ULMON all zero INCLUDING UL_SRC_ON_MON although UL_SRC_ON=1 (the AMIC FIFO/UL
SRC clock is the ADC_L clock; OPPO: "amic fifo ch1/2 clk from ADC_L").
Fixed (both L and R), gain set to Android's 6 dB (GAIN_1P5=4, CARA 0x02), and
ANALOG()/DIGITAL_BYTE() now `_Static_assert` that every value fits its field
(the one runtime value, FFCAPVREF by hwcid, became an if/else). New dump line
`X9 MIC CODEC: PGA= ADC= CLKGEN= CLKMODE= RC= GAIN= CARA=` (expect CLKGEN=2a/2a,
RC not 00/1f, ULMON non-zero). Android's MTKAIF MON (0x2c9d) is 0 too, so the
AIF= field is not a liveness signal. Our normal (non-HiFi) OPPO branch is kept.

**Speaker -> iOS (spk28, user: no more beeps, connect to iOS, default on):**
x9_speaker_bringup.c backend mode (`x9_spk_backend`, default 1): boot holds
ticket, TFA init (ends PWDN=1), board AP-only hold, clocks, pads, AFE register
for the whole boot, then `x9_speaker_backend_register()` (called from
dspBootOnce AFTER dropping audioLock: register takes it). X9Audio callbacks:
start = AFE prepare_on_ticket + start(prefill, lead 1920 frames); position =
AFE cursor + a non-blocking PA state machine (MANSCONF=1/PWDN=0 per PA, CLKS
on both, AMPE=1 with readback, 200 ms limit); submit = fixed -18 dB (>>3)
headroom, then AFE copy (iOS volume is applied in the HAL on top); stop =
AMPE=0, PWDN=1, AFE stop. No boot tone (`x9_spk_tone` default 0, only in the
old one-shot path `x9_spk_backend=0`). `x9_speaker_hal` (PID1 HAL publish into
/private/var/x9/speaker/<id>/hal, union onto /System/Library/Audio/Plug-Ins/HAL)
now defaults to 1 with x9_audio=1/2 (user: "好的功能直接默认开启"); =0 disables.
While the backend holds the shared audio lease the mic would get EBUSY, so in
backend mode a boot with x9_mic_boot runs the mic ALONE and does not start the
speaker (boot-args had no room for x9_spk_boot=0, 599/607 B).
This also avoids the still-unexplained second-subsystem hang (282/284/287).
Images: spk28 `x9/out/x9-spk28-20261114/boot.img` (x9_audio=2 x9_dsp_boot=75
x9_mic=2): speaker backend; expect `X9 SPK: backend chain held`, `X9Audio
backend register lead=1920 -> 0`, and per playback `X9 SPK BACKEND: start`,
`PA clocks locked, AMPE=1`, `stop -> 0`. mic28 `x9/out/x9-mic28-20261114/boot.img`
(+ x9_mic_boot=1000): mic alone with the clockgen fix.
Note: an underrun (HAL stops feeding while Running) faults the service and it
stays Fault until reboot (existing service design).

### logs 288/289 / spk29 + mic29 (2026-11-14)

288 (spk28): the whole speaker chain held and `X9Audio backend register
lead=1920 -> 0` at 109 s; X9 SPEAKER HAL union visible at 2.39 s (created=4).
But no `X9 SPK BACKEND: start` ever: iOS never called X9AudioStart. User: no
sound. Kernel side is not reached; whether audiomxd loads the HAL / routes the
built-in speaker to it (the unported Sheng VirtualAudio/route pieces) is not
visible in the kernel log. spk29 adds `X9 AUDIO UC: open #n by <proc> type=`
(first 8 opens, all type1/2) and `X9 AUDIO UC: START|STOP -> e`.
289 (mic28, mic alone): CLKGEN now 2e/28 as intended, but RC still 1f and
ULMON 0 (UL_SRC_ON_MON=0). So the clockgen bug was real but not sufficient.
Found (static): OPPO's FIRST AIFTX supply SCP_REQ runs mt6681_sw_normal_mode:
if MTC_STS0!=7f / STS2!=1 / VCC_STS0 bit1=0 it forces SW normal mode
(TOP_CON2 3,1,200us,0,200us,1,2ms; then MTC_CTL1=8 if still bad), and on
power-down restores MTC_CTL1=0/TOP_CON2=3 if TOP_CON2==1. Never ported.
Android recording: TOP_CON2=03, STS0=7f, STS2=01, VCC=03. mic29 ports it in
power_on (before VAUD18) with `X9 MIC MODE:` before/after lines, and the
codec dump adds a non-zero sweep `X9 MIC REGS:` of the ranges compared with
the Android regmap, so the next log can be diffed register-for-register.

### logs 290/291 / audio30 (2026-11-14) — built-in Puffin route, HiFi mic, one image

290 (spk29): only mediaserverd type0 probes, never a type1 open or START: iOS
routing never picked the HAL device (it reported BuiltIn transport with an
unknown UID). User wants internal audio, not an external device.
Static RE of iOS 16 (20H392) `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin`:
DeviceFactory_Aspen (0x21ebc8) compares the HAL **device UID** (log `Creating
Device ID: %u with UID: %@`) against a CFString table: ... `PuffinInput`,
`PuffinOutput`, `Digital Mic`/`DigitalMic`/`Mic` ... (cfstrings 0x62e870/890/
8d0). `PuffinOutput` -> Device_Puffin (vtable 0x614e10), whose output port is
type 'pspk' (built-in speaker). No ProductID gate in the factory. vphone-cli
PR 555 uses the same names for its iPhone guest audio. The HAL speaker-
protection gate (0x2857fc, "HAL Speaker Protection is missing") only fails
when the route has 'pspk' AND predicate 0x29234c is true (product enum list
0x11/0x12/0x24/0x25/0x4a/0x4b/0x5e/0x5f + feature flag VirtualAudio/
siri_ducking_for_speaker); our identity is D22AP (iPhone10,3), assumed not in
that list (product enum mapping 0x5026c not decoded: unverified).
audio30 speaker: X9AudioPlugIn UID `PuffinOutput`, device always listed/alive
(StartIO returns NotRunning until the kernel is Ready), and the Sheng host
clock fix: the plugin decodes adrp+ldr of AudioGetHostClockFrequency and
calls new selector 21 (X9AudioSetHostClockSelector, writer type1 only); the
kernel rewrites the 24 MHz double on the shared-cache page via pmap_find_pa+
phystokv to the timebase frequency (1 GHz) and logs `X9 AUDIO CLOCK: host
clock addr= code= pa= seen= -> hz= r=`.
291 (mic29) vs Android recording regmap (scratchpad codec17 = during
recording, codec18 = after): ours ran OPPO's low-power branch; Android records
in mic_hifi_mode. RC tune read 1f (Android 09), UL MON 0 (Android df ff a2
38). audio30 mic = OPPO mic_hifi_mode for ADC_L: offset cal/trim 0, preamp
modes 0, FLASHIDDTEST 1, SPAREPGA1 0xc, IDAC 1, RINOHM 2, FFCAPVREF 1, RCTUNE
7, CLKSEL 1, DACMODE 0, LPEN 0, ADCLMODE 0, NMDAC cal on/3, EXTCAPP/EXTCAP on
+ preset pulse after PWRUP (all match codec17). Not changed: MTKAIF phase
(our scan 4, Android 3), CH2 mux, ADDA6 UL (Android runs 4 ADCs).
Coexistence: the 282/284/287 second-subsystem hang is still unexplained, so
audio30 registers the speaker backend lazily (x9_spk_lazy=1 default): the
boot runs the mic test first (x9_mic_boot=1000), then registers the backend
without touching hardware; the first iOS START does the full hold (`X9 SPK
BACKEND: first start, taking the speaker chain now (lazy hold)`). If playback
hangs right after that line, it is the known mic-then-speaker hang.
Build: `x9/out/x9-audio30-20261114-prelink`, boot args x9_audio=2
x9_dsp_boot=75 x9_mic=2 x9_mic_boot=1000.

### log 292 / audio31 (2026-11-14) — no hang; diagnostics for the HAL and a full codec diff

292 (audio30): no hang. Mic ran first (HiFi values applied, journal 76/76,
CLKGEN 2a/28) but RC still 1f, ULMON 0, 32760 captured frames all zero
(capture -> 22). Lazy speaker backend registered at 81.6 s with no hardware.
Speaker: only the usual mediaserverd type0 probes; no type1 open, no START,
no `X9 AUDIO CLOCK` (that runs only on the writer connect). The user did not
report playback results; the log ends at 147 s. Userland is invisible.
audio31 adds:
- Kernel selector 22 X9AudioHalLogSelector: the HAL queues lines and the
  500 ms timer flushes them as `X9 HAL: ...` (800 lines/boot cap). Lines:
  `init uid=PuffinOutput probe= ready=`, `kernel ready a -> b`, `add client
  N pid= bundle=`, `remove client`, `StartIO client N: why=<1 stopfail,2
  probe,3 not ready,4 running,5 gen,6 connect,7 snapshot,8 START err,9 not
  running after,10 timeline>`, `StopIO`, and `Q ...` = every distinct
  property access as `<obj>[S]:<selector>/<scope>` (`?` unknown property,
  `!` other error). Shows whether VirtualAudio queries the Puffin device.
- `X9 MIC DIFF:` reads every register Android's recording regmap has
  non-zero (pages 00/01/09/1b/1c/23-36, 1299 regs) and prints ours/android
  for mismatches, then `X9 MIC DIFF: done N/1299`. The table is the local
  gitignored x9/kernel/generated/x9_mic_android_ref.inc (generated from the
  Android capture; absent -> "no local Android reference table").
Build: `x9/out/x9-audio31-20261114/boot.img`, same boot args as audio30.

### log 293 / audio32 (2026-11-14) — static analysis only; BBCK5 clock fix

Log 293 (audio31) read statically. No probe image this round.
Mic:
- 123/1299 codec registers differ from the Android recording. The digital
  AIFTX/UL SRC config (UL_SRC_CON0, AUDIO_TOP0-3, CLKSQ 0x06, DCCLK1, CLKGEN
  0x2a) matches. But codec UL_SRC_MON0 ON_MON=0 (Android 1, COMB data live)
  and RC tune read 1f (Android 09). The AP-clocked MTKAIF calibration passes.
  So the codec analog/ADC 26M domain is dead while the digital side is clocked.
- OPPO mt6681_codec_init_reg -> mt6685_set_dcxo_mode(0) and the CLK_BUF DAPM
  supply -> mt6685_set_dcxo(true): MT6687 BBCK5 0x7d6 mode bits[1:0]=0
  (EN_M register control), EN_M bit2=1 (drivers/mfd/mt6685-audclk.c).
  Every iOS boot (logs 280-293) reads xo=11 = mode 01 (EN_BB pin), EN_M 0,
  and xo_take only borrowed it. That is the one clock input not covered by the
  codec register diff.
- Other differences: codec_init_reg never ran under iOS. SMT_CON0 lacked 0xf0
  (Schmitt on MOSI0/1, MISO0, CLK_MOSI) and DRV_CON2 was 0x88 (OPPO 0x89).
  GPIO_MODE3 was 0x11, but with audio_r_miso1_enable OPPO sets only CLK_MOSI
  (Android 0x10). Android records 4 mics (L=AIN2/MIC2, R=AIN0/MIC1, ADC3/4,
  ADDA6 UL). Our single L=AIN0 (MIC1, micbias0) is a valid OPPO route. ACCDET,
  VOW, NLE and interrupt registers are not capture-path registers.
Speaker:
- The speaker-protection gate 0x2857fc is not the blocker. MGGetProductType
  hashes IODeviceTree:/product product-id. Our D22AP product-id is table
  index 36 = 0x669eebc5, which maps to VirtualAudio enum 0x10 (0x5126c). The
  gate set is {0x11,0x12,0x24,0x25,0x4a,0x4b,0x5e,0x5f}, so the predicate is
  false.
- HAL trace: VirtualAudio enumerates PuffinOutput and reads streams, the
  volume control and 'lcdr' (dB range). No AddDeviceClient/StartIO follows.
  Still open: whether any playback was attempted in 293.
Changed in audio32:
1. x9_mic_plj110.c xo_take: xo mode 01 (EN_BB) -> write mode 0 + EN_M (OPPO).
   Restore already writes the original low 3 bits back. 4 stays borrowed.
2. Digital stage (journaled/restored): SMT_CON0 |= 0xf0, DRV_CON2 = 0x89,
   GPIO_MODE3 = 0x10.
Build: `x9/out/x9-audio32-20261114/boot.img`, same boot args as audio30.

### audio33 (2026-11-14) — HakiOS D22 speaker device model (supersedes audio32)

Reference: xm6661885/hakios-xnu-kernel-umi-fixes 05f7477 (HakiOS on Mi 10,
iOS 16.5, same D22AP identity), userland/src/audio_plugin.c (MIT). It is a
pure HAL plugin; mediaserverd is not patched for audio. What it says
VirtualAudio needs for the iPhone X speaker route:
- device UID "Speaker": stereo output plus a stereo input (the amplifier
  feedback; silent), with a volume control;
- a device "Hawking" with one silent input: the mandatory 'vhaw' companion
  of the D22 routing tables. "Without it everything stays on the null port".
  That matches log 293 (PuffinOutput enumerated, never a client);
- "Codec" with an input data source 'imic' for the built-in mic. Our
  microphone_plugin.c already does this.
All four UIDs exist in our 20H392 VirtualAudio (Hawking 0x596709 sits next
to PuffinInput/PuffinOutput in the factory table; Speaker 0x5b53ec; Codec;
Actuator). /Library/Audio/Tunings/D22 is present.
Changed in X9AudioPlugIn.c:
- UID/Name/ModelUID "Speaker" (was PuffinOutput);
- Speaker gets an input stream 4 (2ch, silent ReadInput);
- new device 6 "Hawking" + input stream 7 (1ch, silent), on the host clock
  from its first StartIO;
- CanBeDefault 1 in every scope; no 'accs'/'auto' answers, as in HakiOS.
The kernel speaker path is unchanged. The audio32 mic fixes are included.
Build: `x9/out/x9-audio33-20261114/boot.img`, same boot args as audio30.

### log 294 / audio34 (2026-11-14) — mic works; speaker routed but second-subsystem hang

Log 294 (audio33):
- Mic WORKS. BBCK5 xo=11 -> OPPO mode0+EN_M. RC tune 09 (= Android).
  Codec ULMON live (c0ffe707...). pcm min -135 max 108 rms 18, zeros
  5798/48176. Confirms the BBCK5 root cause (audio32/33).
- Speaker HAL: VirtualAudio now adds a client to Speaker and to Hawking, and
  iOS volume works. StartIO 31-74 s -> why=3 (blockers ff): the lazy backend
  only registers at 82 s, after the boot mic test (76 s). At 92.8 s the first
  type1 START -> lazy hold. PA34/PA35 init 0, board hold/online 0, then the
  SoC hangs about 0.3 s after `X9 SPK CLK: native board -> 0` (109.1 s).
  Same point as log 282 (speaker after mic). 284/287 are the mirror case
  (mic after speaker hangs at its board step). Root cause still open.
- Also: a lazy hold inside StartIO takes about 33 s (SPKLOG/CLKSTEP 600 ms
  pacing; 288: 76.1 -> 109.0 s), far too long for an IO start.
audio34 = boot args only: x9_audio=2 x9_dsp_boot=75 x9_spk_lazy=0 x9_mic=2,
no x9_mic_boot. The speaker chain is held at boot, as in 288 (no hang when
the speaker is the only subsystem), so StartIO is immediate. The mic stays
untouched until the coexistence hang is fixed.
Mi 10 repo survey: calls/SMS are "Not yet" there too. Its media injection is
for its camera pipeline. Vibration: its aw8692x driver (same Awinic family
as our AW8693XS, NOTES 5.1 "190 EBUSY") is the useful reference.
Build: `x9/out/x9-audio34-20261114/boot.img`.

### log 295 / audio35 (2026-11-14) — first real START; underrun killed it

Log 295 (audio34, boot-held speaker, no mic test): chain held at 109 s, no
hang. At 121.6 s the first type1 START: AFE in4/out4 enabled, PA clocks
locked, AMPE=1. 47 ms later the kernel worker stopped the backend itself.
Cause: START plays the 40 ms prefill immediately, but the HAL's StartIO
returns ~260 ms later and only then writes PCM. The queue drains to one
period -> positionBudgetExpired -> ETIMEDOUT -> Fault. Fault was sticky, so
every later StartIO got why=3 (error 60): no sound in any app.
audio35:
1. SweetX9Audio worker: if the HAL has < 1 period and the hardware queue is
   down to <= 3 periods, submit a period of silence (ring slot = device frame).
2. STOP that really stopped the backend clears an EPIPE/ETIMEDOUT stream
   fault back to Ready (logged "stream fault N cleared by STOP").
3. HAL DoIO accepts out_queued > written (kernel silence fill) and zeroes
   gaps from max(written, out_queued).
Coexistence hang (282/284/287/294): in both directions the second subsystem's
thread was asleep in an IOSleep log pause when the SoC died (294: within
600 ms after "native board"; 284: within 250 ms after "native DMA bind",
which only allocated/mapped memory). 284's speaker had failed and torn down
its board hold 1.4 s earlier. Every boot that had both the speaker's AP-only
board hold (power_on) and a mic run hung; boots with only one never did.
Investigation continues (async trigger, not the step that printed last).
Build: `x9/out/x9-audio35-20261114/boot.img` (audio34 boot args).

### audio36 (2026-11-14) — audio35 + vibration enabled by default

Vibration root cause, from existing logs: no boot since 190 ran the haptic
driver. `x9_haptic` defaults to 0 and boot-device.json never passed
`x9_haptic=2`, so 280..296 have no `X9 HAPTIC` line, no Actuator HAL install
and no /product/haptics closed-loop node (mkadt adds it only for the literal
`x9_haptic=2`). The post-190 reset/HAL fixes (7954777) were never booted.
Changes:
1. boot-device.json: `x9_haptic=2` default-on (cmdline now 607/607 bytes
   with the four audio args; no room left for more tokens).
2. Actuator HAL: ModelUID "Actuator" (== UID, as HakiOS D22 and as the
   Speaker fix in audio33) instead of "X9-AW8693XS-Actuator"; CanBeDefault
   answers 1 for output scope like HakiOS.
No hardware collision with audio: haptic uses I2C @0x12350000 (east gate
0x12650000 bit0), TFA uses 0x129d0000, mic uses PMIF/GPIO only. Camera shares
the east I2C parent mux; camera use may reparent it and make haptic EBUSY.
Expected log: `X9 HAPTIC: chip=93x1 ... native I2C5 ready`, else
`init rejected stage=N errno=E` names the failing step.
Coexistence hang: no idle poller exists in our code (mic afe_worker and
speaker watchdog act only while running; ADSP IRQ worker only when armed),
so the async actor is outside these threads (ADSP firmware after power_on,
or a hardware master). Still open.
Build: `x9/out/x9-audio36-20261114/boot.img` (supersedes audio35).

### log 296 / audio37 (2026-11-14) — order-dependent hang, pstore ring

296 (audio36): haptic init at 1.47 s rejected stage=12 errno=16 (I2C5 bank
busy), then the speaker hung after "CLK native board -> 0" with NO mic run.
Order table 282..296: the speaker passes whenever it is the first subsystem
initialized (283/284/285/286/287/288/295); it hangs at the same step when the
mic (282/294) or the haptic init (296) ran first; the mic hangs after DMA bind
when the speaker ran first (284/287). Not intermittent, not mic-specific:
the second subsystem dies. Haptic writes (GPIO30 reset DIR/DOUT, GPIO178/179
MWR mode, IOCFG idx6 0x12300000 PD/PU/IES/RSEL, I2C5 gate) all match OPPO
pinctrl-mt6993.c / mt6993.dts / oplus_misc reset-gpio 30.
Why no tail was ever captured: pstore console continued the previous boot's
size and never wrapped (sweet_log_reserve). 294 filled 2 MiB at 68 s; 295
and 296 wrote nothing. UFS sidecar lags (296 ended 91.4 s); USB loses ~1 KiB.
audio37:
1. pstore console is a ring (ramoops start/size), newest 2 MiB survive.
2. Speaker log pacing (CLKSTEP/SPKLOG/trace IOSleep) only with x9_spk_pace=1.
3. x9_dsp_boot=65 (60 overlapped the modem S2 timer in 260).
4. x9_haptic back off by default (296: its init alone kills the speaker).
Build: `x9/out/x9-audio37-20261114/boot.img`.
audio38 = audio37 code, coexistence test: x9_audio=2 x9_dsp_boot=65 x9_mic=2
x9_mic_boot=1000 (mic capture at 65 s, then lazy speaker on first StartIO;
same order as 282/294). After a hang the user boots Android; pull
/sys/fs/pstore/console-ramoops-0 for the exact last line.
Build: `x9/out/x9-audio38-20261114/boot.img`.

### audio39 (2026-11-14) — ROOT CAUSE of the coexistence hang: io_submap exhaustion

Static audit, no new test. xnu-8796 `osfmk/arm/io_map.c`: every post-KMEM
`ml_io_map()` is `kmem_alloc(io_submap, KMA_NOFAIL|KMA_PERMANENT)` from a
fixed **8 MiB** submap; exhaustion = `__kmem_failed_panic`. On X9 that panic
is invisible: the USB and UFS consoles are threads, which never run again.
Proof from existing logs (VM layout + Wi-Fi's 64 KiB NC map at ~21.8 s):
| log | io_submap offset of the Wi-Fi map |
| --- | --- |
| 294/295/297 | 0x694000 (6.58 MiB used) |
| 296 (haptic init at 1.5 s) | 0x744000 (+0xb0000 = haptic 0x80000+0x10000+pages) |
So ~1.36 MiB was left (0.66 MiB with haptic). Each audio subsystem's first
session maps its own copy of HWV 0x1c040000+0x75000 (480 KiB at 16 KiB pages)
plus AFE/VLP/GPIO/bus pages: speaker take() ~0.6 MiB (+ADSP HWV), mic
board_probe ~0.8 MiB. The first subsystem fits, the second runs out,
regardless of order. The hang windows match the second subsystem's first
`ml_io_map` exactly: speaker after "native board -> 0" = take()'s four maps;
mic after "native DMA bind" = board_probe's mic_map_once(); 297 = the
speaker backend registering ~2 s after the mic. 260 (61 s) likely the same.
Camera `cam_board_prepare` re-maps ~1.7 MiB on EVERY cold open (never
unmapped), so camera use would eventually exhaust any fixed size too.
audio39:
1. Overlay `x9/kernel/src/osfmk/arm/io_map.c` (replaces the frozen
   io_map.o; recipe.json + link-order.txt): IO_MAP_SIZE 64 MiB, and identical
   permanent mappings (same flags/prot, range covered) share one VA (512-entry
   table; permanent maps are never freed, so sharing is safe; unmappable maps
   untouched). Logs `X9 IOMAP: io_submap used N KiB of 65536 KiB` per MiB.
2. Haptic default-on in code (x9_haptic=2 default; boot-xnu.py ADT haptics
   follows unless x9_haptic!=2). Stage 12 now logs the busy bank/reg/value and,
   only after we ungated a gated I2C5 and only for bank 0 MASK with the bank
   otherwise idle, clears the stale LK INTR_MASK (as I2C6 256 / SCP-I2C1 263).
Args: x9_audio=2 x9_dsp_boot=65 x9_mic=2 x9_mic_boot=1000 (599/607 bytes):
mic at 65 s, then lazy speaker on first StartIO, haptic at boot — the exact
282/294/296/297 hang orders.
Build: `x9/out/x9-audio39-20261114/boot.img`.

### audio40 (2026-11-14) — log 298: no hang; speaker cut, mic silent, haptic STAT

Log 298 (audio39): no hang (`X9 IOMAP` ~7 MiB of 64). Three static fixes:
1. **Mic silent (MISO slot mux).** OPPO r_miso1 (`audio_r_miso1_enable=1`):
   MISO0 pad stays GPIO, so codec ADDA TX (2ch, `MUX_CFG` 0x2390) never leaves
   the chip; the MISO1 pad carries **ADDA6 TX 4ch** (`mt6681_mtkaif_tx_enable`
   tx2), slots = `MUX_CFG_M` (0x238f) ch1/ch2 then `MUX_CFG_H` (0x238e);
   AP takes UL ch1/ch2 from `ADDA6_MTKAIFV4_RX` (FOUR_CH=1, EN_SEL=1,
   `UL_CH1CH2_IN_EN_SEL`). `MUX_CFG_M` = 0x5a = UL2_CH1/UL2_CH2 (ADDA6 UL
   SRC + PGA3/4), which we never power; Android had data there only because
   it records 4 mics (348c/348e on, ADDA6 UL SRC 0x2588..a = 23/08/01).
   298: MUX=00/5a/80, MISO1 high 4/64, AP UL raw constant, PCM rms 1 while
   codec ULMON moved. Now `MUX_CFG_M` ch1=ch2=UL1_CH1 (our ADC_L/AIN0),
   `MUX_CFG`/`MUX_CFG_H` = Android's 08/88 selects. EN_SEL bits untouched.
2. **Speaker cut ~150 ms after every START.** Each START ended in
   `stream fault 60` (ETIMEDOUT) 40–312 ms in (gen 3 before the PA even
   locked) with audio still queued. Both layers capped the no-progress
   window at 10 ms (`MIN(budget,10ms)` in `x9_speaker_afe.c`, `safe` clamp
   in SweetX9Audio), polled by 2 ms kernel threads on a busy system (GPU,
   cell, PA I2C step inside the position callback). Now both budgets are the
   audio actually queued (AFE: pending−margin; service: pending−1 period),
   zero-CUR grace 50 ms, and the backend lead is 8 periods (80 ms, PREFILL_MAX)
   with silence fill keeping the queue at the lead (was >3 periods), so HAL
   frame 0 stays aligned. One fault-detail line per fault (AFE and service).
3. **Haptic stage 12:** after the MASK clear, bank 0 `INTR_STAT=1` (LK's
   unacked TRANSAC_COMP). Same preconditions (we ungated, START 0, bank 0
   only, other regs/banks idle): W1C it, re-check idle.
Args unchanged (x9_audio=2 x9_dsp_boot=65 x9_mic=2 x9_mic_boot=1000).
Build: `x9/out/x9-audio40-20261114/boot.img`.

### audio41 (2026-11-14) — log 299: speaker OK but late; mic TX2 idle; haptic stage 13

Log 299 (audio40): no stream faults any more (12 sessions, seconds each,
normal stops). Haptic passed stage 12 (stale STAT cleared). Fixes:
1. **Speaker only after ~73 s** = `x9_dsp_boot=65`: the mic boot test runs
   at 65 s and the lazy speaker backend registers after it (72.7 s); every
   StartIO before that got `blockers=ff`. Now `x9_dsp_boot=10` (mic test
   ~10–14 s, backend ~15 s, before mediaserverd's first StartIO ~33 s).
2. **Mic: TX2 shifted nothing.** MUX=08/40/88 took effect, ULMON moved, but
   the ADDA6 TX data-out monitor (0x2ca3/4) stayed 00 and MISO1 high 0/6.
   OPPO DAPM: MISO2/3 muxes (MUX_CFG_M) hang off `UL2_SRC_MUX` →
   `UL_SRC_34`, so an r_miso1 recording always runs `mt6681_set_ul34_src`
   (Android 0x2588..a = 23/08/01, ours 20/00/00). Now ADDA6 UL SRC is
   enabled in set_ul34_src order (voice 48k, IIR on, ON last), journaled
   and restored like UL1; slots still carry UL1_CH1.
3. **Haptic stage 13 EIO (htiming).** OPPO `mt6991_compat` (speed_ver 1,
   ltiming_adjust 1) writes HS=0, CLOCK_DIV=(d-1)<<8|(d-1), SDA 0, MIS 0,
   IO 3, DELAY_LEN 2, and never reads them back. This revision reads HS
   bit15 set and no upper DIV field (I2C6 identical since 257). Timing CSRs
   now journal the hardware value (logged), DELAY_LEN=2 as Android.
   Transfer CSRs stay strict. Bus stays ≤390 kHz (Android DT: 1 MHz FM+).
Args: x9_audio=2 x9_dsp_boot=10 x9_mic=2 x9_mic_boot=1000.
Build: `x9/out/x9-audio41-20261114/boot.img`.

### audio42 (2026-11-14) — log 300: speaker at 18 s; mic still silent; haptic stage 15

Log 300 (audio41): speaker backend registered at 17.9 s, HAL ready 18.7 s.
Mic still rms 2. **Correction:** Android's recording dump also reads the codec
MTKAIF monitors 0x2c9d..0x2ca4 = 00 and the AP `AFE_ADDA_UL0_SRC_MON0/1` =
c0000000/60000000 — neither "AIF6=00" nor "UL raw constant" means no data;
audio41's TX2 conclusion rested on that. ADDA6 UL SRC stays on (Android has it).
Android SoC dump vs ours, capture path:
| reg | Android | ours |
| --- | --- | --- |
| ADDA_MTKAIFV4_RX_CFG0 / ADDA6 | 31051 / 1053 | same |
| MTKAIF0/1_CFG0, RX_CFG0/2 | 80010000, 730, 6300000/6303100 | same |
| AUD_PAD_TOP_CFG0 | b9 | same |
| ADDA_UL0_SRC_CON0 | 60401 | same |
| **ADDA_UL0_SRC_CON1** | **7fff0412** | 7fff0402 |
| VUL CON0 | 100048 | same |
Bit4 = FIFO_SOFT_RST. OPPO's "ADDA_FIFO" supply is inverted and routed only
to "AP DMIC Capture"; MTKAIF AMIC never powers it, so it stays 1. We cleared
it ("release FIFO reset"). Now kept at 1 like OPPO/Android.
Android takes I_ADDA_UL_CH1..4 (CONN048..051 into CM1); our slot mux carries
ADC_L in every slot that can reach CH1.
Haptic stage 15 EIO: our hconfigure gated on its own strict readback of every
write and on SYSST fault bits; OPPO aw8693xs.c (i2c_w_bytes/w_bits, TMCFG
0x7d/0x00 lock writes) does neither. Now OPPO-style: only I2C errors fail;
one line `config as OPPO: sysst before/after, readback diffs=N reg=w/r`.
Not-standby (0x3f) still refuses; runtime fault checks unchanged.
Args: x9_audio=2 x9_dsp_boot=10 x9_mic=2 x9_mic_boot=1000.
Build: `x9/out/x9-audio42-20261114/boot.img`.

### audio43 (2026-11-14) — iOS recording blocked twice after the boot capture (log 301)

Log 301: haptic ok, speaker ok; boot mic capture rms 1, then nothing when the
user records. There is no `X9 MIC` line after boot at all. Static audit found two independent
blockers on the HAL path (X9Microphone StartIO -> X9_MIC_START -> afe_start):

1. **Stop race poisoned restart.** The boot capture's stop (sweet layer clears
   RUNNING, bumps generation, then waits on afe_mic_lock) raced the 5 ms
   worker: its `x9_mic_deliver` got ESTALE and the worker recorded it as a DMA
   fault (`codec err=70`, `dma err=70` in 301). `capture_restart_ready()`
   then refused (EIO), so every later START failed. The kernel marked the
   mic FAULT and the HAL latched io_fault, both silently. Fix: ESTALE from deliver
   = consumer stop in progress -> EAGAIN (afe_stop does the drain).
2. **Speaker kept the shared lease until reboot** (lazy hold at the first
   X9Audio start, 31.75 s in 301), so the mic admission got EBUSY. Fix: the
   mic asks an idle speaker backend (`x9_speaker_backend_yield`: no AFE
   stream, PA idle) to run the one-shot teardown and hand the lease back.
   The speaker re-takes the chain on its next start: the AFE engine registers
   once per boot, the TFA I2C6 session reopens after a clean close, and the
   ADSP re-adopts the LK votes (release only withdraws owned votes; 301 owned=0).
   The clocks/pads need a new generation, which comes from the ADSP serial.
   A playing speaker refuses. The mic START then returns EBUSY, which is now
   non-sticky in the kernel (READY kept, last_errno 0) and in the HAL (no
   io_fault). A speaker START refused because the mic records is EBUSY and is
   cleared by STOP like EPIPE/ETIMEDOUT.
3. Analog as Android recording (record-regmap 3488=83, 3489=5e): PGA L =
   AIN2 (OPPO mt_pga_l_event: DEVICE_MIC2), CON1[3:0] RG_AUDPREAMPLGAIN=0xe
   (OPPO mt6681_adc_init on every power-up).

New lines: `X9 MIC: START -> 0` / `START refused`, `X9 MIC: lease busy`,
`X9 SPK BACKEND: yield chain to mic`, HAL stderr `X9 MIC HAL: START`.
Expected: recording works while nothing plays. The first sound after
recording re-takes the speaker (~140 ms lazy hold). Playback during a
recording is refused for now (exclusive lease; the Android-style shared AFE
is the next step).

### audio44 (2026-11-14) — second mic session refused by its own leftover START bits (log 302)

302: the audio43 yield worked (speaker teardown all 0, ticket released at
44.76 s). The mic START then stopped at board stage 3 (bus): `bus_fault_errno=16
reg=0x24` (scp-i2c1 START). The boot capture restores LK's INTR_MASK=3ff, and
after our own transfers START reads 0x4002 (log 267: bit14 auto-restart config
plus bit1 stay set after START=0). The stale-mask admission compared the whole
START register, so it refused (bank_idle already checked only bit0). That pre-write
refusal also set bus_stuck, so restore() returned EIO (restore=5). The mic
kept the shared lease, and every speaker start after that got EBUSY ("speaker
dead"). The speaker teardown was not the cause.
Fix: the admission checks START bit0/STAT/FIFO only, and refusals before
bus_owned (nothing written) use bus_refuse(), which does not mark the bank stuck.
Audit of the rest of the second session: the journal bzero resets codec.touched,
and GPIO9/XO/wake/routes are restored to their found state (same adopt paths
as the first session). The speaker clock release restores 0x14/0x60/VLP muxes
before afe_take. The speaker re-hold path has still not run on hardware.

### audio45 (2026-11-14) — second mic session: latched VUL1 MON0 and a terminal failed START (log 303)

303: the speaker works, including re-taking its chain after the mic (122 s
lazy hold -> 0). The mic's second session passed bus, codec, AFE cal and
analog (AIN2), then got EBUSY at digital stage 2: the VUL1 baseline saw
MON0=0x300000 with CON0=0x100048. In every log (298-303), MON0 after a clean
stop reads 0x300000 (burst lengths of the last run stay latched), while before
the first run it reads 0x100000 (Android idle dump: 0x100000). The idle macro
only discounted the length bits that the current CON0 also had set. Fix:
bits 16-17/20-21 are never activity.
The immediate retry returned EALREADY (37) -> FAULT: restart_ready accepted only a
completed capture. Now a failed START whose board_finish fully unwound (finish_errno 0,
board RESTORED, nothing retained/owned/pending, DMA never started) is
re-armable. Retained or uncertain unwinds stay terminal.

### audio46 (2026-11-14) — speaker and mic run together on one AFE (log 304)

304: the mic itself works now (12x `X9 MIC: START -> 0`), but iOS recording and
Voice Memos playback start input AND output together. With the exclusive
lease, the second side got EBUSY (`lazy hold -> 16`), iOS aborted the session,
and the mic stopped, in a loop. That is why both "speaker" and "mic" looked dead.
Taking turns (audio43 yield) can never satisfy a PlayAndRecord session.
Model now (as Android, one shared AFE, one owner per shared bit):
- lease: the mic has its own slot beside the speaker; exclusive only vs ADSP and a
  second mic.
- before a mic START (lazy backend registered, i.e. not the boot capture)
  the speaker chain is taken (PA PWDN=1) and kept for the boot again; the
  mic never tears it down (yield no longer called).
- mic afe_take in shared mode borrows the speaker base and writes nothing
  shared: requires TOP_CON4[1:0]=0, 0x60 bit2 + DRAM bits, CBIP bit0,
  ENGEN[3:0]=9; no sidebands/engine/0x60 writes or restores; DRAM request
  borrowed. afe_idle expects ENGEN=9 and skips the DL24 memif (idx 12); mic DMA
  admission ignores DL24 while the base is held. The mic still owns its SRAM smc 3/4,
  UL/RX/MTKAIF journal, ENGEN bit1 (ADDA), VUL1, codec, scp-i2c1, pins.
- speaker native_engen_idle masks bit1 (mic ADDA). native_tuner_verify
  already masked 0x14 to 9. Speaker 0x98 writes bit5 only (the mic checks bit0).
- the codec register dump runs once per boot (it added ~1.5 s to every START).
- MTKAIF calibration wrote AFE 0x98 (AUD_TOP_CFG_VLP_RG) as `(old&~0xffff)|4`,
  clearing bit5 RG_I2S4_PAD_TOP_CK_EN (the speaker's I2S4 pad clock) and the
  I2S4/5 latch bits. With a shared AFE that would cut a running speaker. All
  calibration writes to 0x98 are now RMW on TEST_TYPE/SW_RESET/TEST_ON
  (bits 2:0) only, including the restore and the phase loop.

## audio47 (log 305): mic stream died ~1.3 s after START with the speaker running

- 305: the speaker and mic ran together (shared AFE, `START -> 0 gen=3`). At 170.551 VUL1 reported `errno=22`, the worker stopped DMA and the mic went FAULT.
- Cause: `poll_locked` timestamps each chunk by working back from `now` by the bytes still pending. When a poll ran late and picked up a DMA burst, its first chunk came out at or before the previous poll's last sample. `x9_mic_deliver` requires strictly increasing time and returned EINVAL.
- Fix: `delivered_time` is reset at arm. Each chunk's time is clamped to `max(t, delivered_time+1)` and then to at most `now`. It is recorded only after a successful deliver.
- Still open: one stream fault keeps the mic FAULT until the next boot. EOVERFLOW (worker delayed 100 ms or more) and ENOBUFS (ring of 32768 frames is full) also still end the stream.
- Also seen: the boot self-test PCM was min=-5 max=4 rms=1. If recordings now play back silent, look at the analog gain/route next.

### 5.11 2026-11-15 蓝牙状态核对：bootstrap 已实跑，ROM / 固件 / 原生 iOS 链路尚未接通

- **范围/提交**：只读核对 main `d62cd4f`、源码和已有日志，交接分支 `feat/bluetooth-status-20261115`。蓝牙实现仍是 `c9b3e46`（2026-11-10），三个核心文件与该提交无差异；`efad465` 是同实现的另一历史提交。归档 `refs/archive/wip-worktrees/20261112/bluetooth`=`baac0bb` 不是 main 祖先，不能当成后续已交付功能。此次只更新 NOTES，没有编译、测试套件、新 boot、CI 或设备操作；`gh run list --limit 5` 返回空，无本轮 CI run ID。
- **实机观察（最新 Mac `~/319.txt`，313–316、318 同样）**：`:398–400`（1.123–1.131 s）打印 `firmware metadata 774585 bytes/11 sections; probe=0`、`passive BT_RST=0 PMIC_EN=0 cg0=00000000 cg2=00000000; firmware-ready=0`、`X9BluetoothBootstrap registered; bootstrap ABI v1, not AppleBT, not radio-ready`。证明注册/只读快照已执行，不证明 UART 收发；这些是启动早期快照，不能据此判定 Wi-Fi 上电后 BT 仍未上电或 hub 已可用。`:6947`（28.842 s）SpringBoard 的 `IONameMatch=bluetooth` 返回 `none`。
- **内核已实现/未实现**：`x9/kernel/src/bsd/dev/x9_bluetooth.c/.h` 与 `SweetX9Bluetooth.cpp` 已加入 recipe/link-order/IOResources personality。已有 UART3→UARTHUB 的有界轮询 H4、8 槽 TX/RX、分片/超长帧/超时处理、自有同步 IOUserClient ABI v1。`x9/build/boot-device.json` 显式带 `x9_bt=1`，但不带 probe/raw/discovery，所以实际停在 PASSIVE。`x9_bt_probe=1` 的两条 WMT 查询（query_uart=2、chip_id=9）仅可接管已上电/时钟开启/空闲的 AP/CMM，不会补齐 SSPM/hub/时钟初始化；没有本次或既有交接的 XNU ROM_READY 证据。固件小端 v2 节表已索引（774585 B/11 节），**仅元数据，没有嵌入/下载固件**；patch/ILM、connfem、校准、RAM code、低功耗 ownership 与 HCI Reset 均未完成，`firmware_ready` 始终为 0。
- **iOS 已实现/未实现**：`x9/bluetooth/btctl.c` 是自有 ABI 的状态/节表/WMT 诊断工具，历史 BT-1 交接记录 iOS16 arm64 编译+adhoc 签名与 boot 打包 exit 0，但未安装/运行；当时 `x9/out/x9-bluetooth1`、`.local/x9/bluetooth/client2/btctl` 现在均不存在，旧绝对路径复建命令不再适用。当前工具构建用新的 `x9/out/<name>/client`，不要照旧 README 在 `.local` 新建实验目录。原始 AppleBluetoothModule/AppleBT shared-memory/pipe ABI 和原生 UART 传输选择条件未实现；没有 `/dev/cu.bluetooth` / `/dev/uart.bluetooth` 对接，也没有原始 bluetoothd 扫描/配对或 A2DP/HFP/BLE 验收。`x9_bt_discovery=1` 仅改服务名，不补齐 ABI。
- **daemon 证据边界**：319 的 `:4176–4177` 有 `bluetoothuserd` 的 AKS 调用，证明该进程实跑，不等于 bluetoothd 控制器就绪。319 未见 bluetoothd 自身的成功启动/打开 transport 日志。已挂载的本地根盘克隆 `/private/tmp/x9root` 保留原始 bluetoothd、bluetoothuserd 和 launch plist；bluetoothd plist 为 RunAtLoad=true、UserName=mobile，克隆 disabled.plist 没有两者的禁用项。克隆是辅助证据，不能替代设备当前进程/launchd 状态。当前源码的默认 disabled-job 策略也不禁用 bluetoothd。
- **下一步/复核**：先按 OPPO 已知路径补齐并证明 UART3/UARTHUB/SSPM 前置→完整 WMT 回复且 chip=6653→真正固件/校准/HCI Reset；iOS 侧研究 20H392 原始 bluetoothd 的传输选择与 ABI。之后接入原始 daemon，依次验真实扫描、配对、BLE，最后单独验蓝牙音频；不能用现有扬声器/麦克风工作代替 BT 音频证明。日志复核：`rg -n 'X9 BT:|X9Bluetooth|bluetoothuserd|bluetoothd|IONameMatch.*bluetooth' ~/319.txt`。现阶段结论：**内核 bootstrap 已编译并实跑，iOS 自有诊断接口已写；硬件 ROM 链路和原生系统蓝牙都未验收，蓝牙仍不可用。**

### 5.12 2026-11-15 蓝牙 native 实现交接（进行中）

- 用户要求完整 iOS 桥，所有硬件初始化依据 Android，不出探针镜像。工作树 `.worktrees/bluetooth-native` / `feat/bluetooth-native`；主树正在另一会话做 ALS，不切换主树分支、不修改共用 XNU 源树。当前没有新 boot/CI/设备操作。
- **已提交并push**：取证交接 `9628afc`；下载执行器/串口/iOS H4桥实现 `489e13a`。分支已在origin，功能未闭环所以保留worktree，不合入main、不发布候选镜像。
- 已恢复参考 UniXNU `e893717` 的 pinned `drivers/QcaBluetooth` submodule `986d6319d1d2819b27ed2c9786776f10bf63cc76`。可复用的是传输前端与 native 路由方法，不是 Qualcomm 硬件初始化或 iPadOS27 offsets。
- Android 权威源：`oppo-modules-dt-mt6993/vendor/mediatek/kernel_modules/connectivity/bt/linux_v2/uart/btmtktty.c` 的 `btmtk_sp_pre_open`，及 `btmtk_main.c` 的 `btmtk_load_rom_patch_connac3`。已确认 115200 → query UART/chip → 24M → raw ff wake → CONNXO → 按 `{0,2,3,0x80,0x90,0x10000}` 的首个匹配 bin type 下载、type3 后 connfem、MT6653 最后 bin index3。DMA 在 UART 上是原始固件流（1988 B 单元），不是 H4；WMT fallback 为 ACL phase1/2/3。section spec 必须保留完整52 B，announce header10 B；status0需要下载/1其他下载者/2已就绪。
- MT6993 UARTHUB 的 `SSPM_DRIVER_EN=1`，Android 不执行非SSPM分支的手工 PLL/mux 初始化。必须继续追 SSPM/default-config/host-request/awake 的真实前置，不能把共享 Wi-Fi 供电或 bypass 写入等同完整 BT 初始化。
- iOS 原件只读来源 `/private/tmp/x9root/usr/sbin/bluetoothd`（20H392 arm64，SHA256 `8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f`，UUID `AA9424E8-0648-34E2-B3CF-E6C793B9D471`）。分析输出在本树 `x9/out/bluetooth-native/`。`transport-encoding`/`transport-speed` 从 DeviceTree plane 读取 CFData4B小端；`0x100677370` jump table已解码，encoding1确实是H4。
- **重要纠正**：20H392 的 `POSIX` 表 `0x100844148` 的 open `0x100066bdc` 实际打开 `com.apple.uart.sk.bluetooth` kernel control，随后走 Skywalk，不是 `/dev/cu.bluetooth`。串口字符串是启动存在性检查，不能证明可直接使用 BSD TTY。原始 H4 framing可以复用，但必须适配底层传输表/收包回调或 UART.SK；不能照 macOS qbt_tty 宣称原生可用。
- chipset0 字符串存在但注册 `0x100166c74` 拒绝0；controller object `+0x260` 经 `0x100283fa4` 注册。vendor init `0x100166a04` 对type1..1999走Broadcom，2000..3999走Apple；type0需专门适配。generic callback `0x100166e48` 的 `0x1008507b8` 保存不可删除。原生能力须来自 HCI 查询，不发布 BCM 能力常量。
- **实现里程碑**：新增 `x9_bt_download.c/.h`（Android完整52B section spec、type/index顺序、status0/1/2、有界raw DMA与WMT ACL phase1/2/3、必需connfem回调）；生成器现在输出 gitignored `x9_bt_payload.inc` 的774585B真实固件字节，`x9_bt_download_builtin`可取用。新增 `x9_bt_serial.c/.h`（mobile的cu.bluetooth、64-bit termios、阻塞读写/select、16KiB FIFO、TX peek/commit generation隔离、失败/关闭唤醒），在现有service注册阶段初始化，`kern.x9_bt_boot`保持ENXIO。两个对象已加入recipe/link-order。
- **iOS桥实现**：新增 `x9/bluetooth/X9H4Transport.c`，constructor校验20H392 UUID/arm64及dyld已解析POSIX slots，仅替换open `0x100844168`，不修改磁盘Apple原件。open检查完整board sysctl结果后打开cu.bluetooth，复用原件 `0x100067908` RxLoop、`0x100067610` TX、`0x1000674d0` close；H4 buffer在创建RX线程前初始化。原始thread/cond wrapper已逐条静态核对。未修改chipset/vendor分支；不把桥编译成功宣称daemon已接通。
- **确切构建结果**：本树执行 `python3 -I x9/bluetooth/build.py --kernel-objects --out x9/out/bluetooth-native/client` exit0，btctl与X9H4Transport.dylib完成iOS16 arm64编译/adhoc签名；三个BSD overlay对象编译通过。日志 `x9/out/bluetooth-native/compile.log`，对象在 `client/kernel/kernel-objects/`。早期单文件编译暴露thread_terminate声明缺失、FIFO宏冲突与私有头尚未生成，均已修复；最终只有常规C99声明兼容警告与dylib constructor提示，无error。没有完整内核链接、boot打包、测试套件、构建后解包/hash/反汇编、CI或设备操作。`client/build.json`明确stageable=false。
- **仍未完成，不可上机宣告蓝牙可用**：下载执行器尚未接到完整board worker；SSPM/connv3/32K/host-request与24M/XO/connfem/post-fwdl/ownership仍须按Android完成，UART HCI流尚未接到serial API。原生chipset0/vendor ops及真实能力回调仍待实现，launch/BlueTool启动路径未交付。因此默认firmware_ready=0、serial ENXIO仍是真实状态；扫描/配对/BLE/A2DP/HFP无验证。下一步先落地真实Android硬件前置与完整WMT会话，再接原生vendor适配，不能启用旧ROM探针替代完整初始化。
- **续做里程碑（2026-11-15）**：新增 `x9/tools/bluetooth_board.py`，从私有 production `sku/hw-0` 编码171 B BT connfem WMT ACL；已核对 `connfem_api.h` 的 pinmap 实际为3 B `(Antsel,FEM pin,polarity)`，不是Wi-Fi交换顺序/补齐4 B。七个flags缺失即拒绝，FEM/pin/truth/layout有界，生成头 `x9_bt_board.inc` 已gitignore；总构建与蓝牙单对象构建均接入生成器。
- 新增 `x9_bt_session.c/.h` 并加入recipe/link-order：完整串行会话 `pre_on → shared power → 100ms reset settle → RX/32K → 115200/query2/CTSRTS/query9 → 24M → raw ff → XO → builtin下载器（含必需connfem回调）→ query0x22真实FW flags/maxbaud → post-download hub config → raw wake/ownership验证`。WMT长度/opcode/status严格检查，真实chip必须0x6653；XO25 B必须由已存在的实机校准提供，reserved35 B清零、平台word在尾4 B。未注册wake callback不启用wakeup-enhance；所有步骤保留确切失败stage。不复制厂商失败默认chip/assert/忽略XO与connfem错误。
- **供电证据纠正**：`connv3_hw_pwr_on(curr_status!=0)`只执行antenna power，不重启shared PMIC；BT的pre_power_on仍要调用。`mt6993_pinctrl.c` 的ops没有 `pinctrl_ext_32k_ctrl`，manager对此返回0即Android真实no-op，不是需要移植的其他平台PMIC寄存器写。UARTHUB 24M generic配置函数是TODO，AP UART的8250驱动对12M/24M使用固定sample7/point3/fracdb,1；CMM24M必须追SSPM初始化，不能套旧probe的AP/CMM双写计算。
- **本里程碑确切构建**：`python3 -I x9/bluetooth/build.py --kernel-objects --out x9/out/bluetooth-native/session-build` exit0；171 B production board header生成、四个BSD对象（含session）和原H4 dylib/btctl编译签名通过。日志 `x9/out/bluetooth-native/session-compile.log`。未跑测试套件、未完整链接/打包boot、未二次验证产物、未CI/设备操作。**session是已实现的协议层，不是已完成的硬件backend**：七个board ops必须真实实现；当前worker仍不调用它，firmware_ready仍0、serial仍ENXIO。SSPM启动/共享radio仲裁/硬件ownership、iOS vendor与能力适配、启动整合和场景验证仍未闭环。
- **本轮协议提交已push**：`1aaa4a8`。继续实现 `X9Vendor16.c`：仅20H392 UUID/transport slots已校验、显式 `X9_BT_VENDOR=MT6653`、真实完整 `kern.x9_bt_boot=0` 才安装。独立核对本版两种vendor backend均复制0x750 B ops，vendor callbacks另有bridge；base/derived constructor分别清type-dependent默认enum，注册范围仅由1..3999扩至0..3999。type0初始化清已确认的vendor ops/hooks，null ops由原件返回NOT_IMPLEMENTED 0xe11；保留0x100850788 context与0x1008507b8 generic callback，非0仍通过PC-independent prologue trampoline调用原件。supported-commands callback原先 `MOV w0,#0` 无条件吞掉真实HCI状态，改NOP保留原状态。全部目标指令先核对、全部COW text页权限先取得才修改，权限失败不修改指令；不写Apple原件文件。
- **native构建确切结果**：`python3 -I x9/bluetooth/build.py --out x9/out/bluetooth-native/vendor16-build` exit0，btctl/dylib编译签名通过，日志 `x9/out/bluetooth-native/vendor16-compile.log`。最初iOS SDK明确不支持mach_vm.h，改用该SDK已提供的64-bit `vm_allocate/vm_protect/vm_deallocate`（静态断言地址8 B）；这是编译修复，不是运行证据。没有测试/CI/boot/上机；**未证明COW executable保护与native流程在设备上成功，也未完成model capability getters**。build.json继续stageable=false；不修改launch/BlueTool、不用type0路由编译通过宣告扫描/配对/BLE/音频完成。
- 会话补充前置：`x9_bt_download_preflight_builtin()` 在任何pre_on/供电操作前校验完整builtin容器，缺固件、缺production connfem或缺实机校准即返回错误。**最终组合构建** `python3 -I x9/bluetooth/build.py --kernel-objects --out x9/out/bluetooth-native/native-session-final` exit0（4 BSD对象、btctl、包含type0代码的dylib）；日志 `x9/out/bluetooth-native/native-session-final-compile.log`。仅常规C99兼容/constructor警告，无error；不二次检验产物。最终仍无kernel链接、CI run ID、boot镜像、设备操作或功能验证；主树保持main且clean。下一项先依据当前LK/SSPM真实移交确认UARTHUB CMM24M配置与APB/PLL request-ready路径，再实现共享radio硬件board ops并接worker，随后native真实model能力与启动编排。不能将本轮协议/路由代码等同三项用户目标全部完成。
- **当前代码提交已push**：protocol/SKU `1aaa4a8`，20H392 vendor routing与会话preflight `7196eb2`，均在 `origin/feat/bluetooth-native`。功能未闭环，保留worktree，不合main；最终package明确不可stage。此交接之后只补记录提交，不重建、不上机。
- **硬件 backend 续做里程碑（2026-11-15，仍未闭环）**：新增 `x9_conn_power.c/.h`，Wi-Fi/BT共用有锁lease、power-done、last-release；首radio按MT6993的30ms→PMIC_EN→20ms→faultb/common pinctrl执行，第二radio不重启PMIC、不把live combo UART短暂切回GPIO。未登记但已高的PMIC返回EBUSY，不猜它是Wi-Fi；vsel/antenna/32K保持该平台真实no-op。Wi-Fi原真实radio loop持有lease，返回失败/诊断只释放Wi-Fi，不再直接关GPIO251；WF_RST仍由Wi-Fi单独管理。GPIO SET/CLR避免与BT_RST同word竞态。未实跑新的共享供电生命周期。
- 新增 `x9_bt_hardware.c/.h`：UARTHUB APB检查在先，SSPM default SRAM实际offset **0x1e4**（base0x00113c00、写0x10001）；multihost RX/TX请求后最多50ms等DEV0 ready，再检查PLL/SPM/AP UART clocks，不能要求PLL先开才发request。严格拒绝busy MD/ADSP/AP、残留XOFF，不强行清运行中radio；idle才执行Android AP-only FIFO隔离和bypass。AP UART115200与24M配置按8250实现，**CMM不套AP sample/point/frac，不写非SSPM PLL路径**。host-awake的状态bit11、SET bit2、CLR bit4已独立核对，原交接按状态位写SET的解读不可用。
- `x9_bluetooth.c`已替换旧ROM worker，接 `x9_bt_session_run`/下载器/production connfem/真实XO参数；UART3 SPI0x148+32真实IRQ收包→32KiB ring→worker完整WMT帧/opcode匹配，无关包有界保留，partial TX/drain有界；完整成功分支才串口HCI流和ready。**不是Linux APDMA移植，也未上机证实IRQ/吞吐**。SLEH hook同步overlay和 `x9/xnu/0006-x9-bt-uart-irq.patch`，build-xnu的patch集合已接006；base独立构建有weak/noinline false fallback，overlay提供strong实现。没有修改/重建共享 `.local/dxb/xnu` 或 `.local/dxb-iosabi/xnu`；两棵旧源树暂未应用006，未来该分支完整XNU重建须按patch集合更新，不在其它会话工作时改它们。
- **实际硬阻塞保留**：SSPM怎样配置CMM24M及当前LK是否完成SSPM移交仍没有闭环证据；`post_download`明确返回ENOTSUP，而不是猜寄存器/把default SRAM或AP24M当CMM成功。后续host-awake feedback/ownership验证代码因此尚不可达。移除 `x9_bt_probe/raw` 与btctl query入口，bootstrap selectors2/3返回unsupported，避免与native抢ACK。当前 `boot-device.json`不加 `x9_bt_init=1`，保持x9_bt=1的passive注册；**不生成部分初始化/探针镜像，不发布runtime**。`kern.x9_bt_stage`记录返回stage，不是ready，boot结果仍默认ENXIO。
- **本里程碑确切编译**：`python3 -I x9/bluetooth/build.py --kernel-objects --out x9/out/bluetooth-native/hardware-backend-final` exit0，6个BSD对象（common power/hardware/download/session/serial/worker）、sleh与2个IOKit owner对象、btctl和20H392 H4/vendor dylib编译签名通过。原始日志 `x9/out/bluetooth-native/hardware-backend-final-compile.log`；初次缺sched_prim声明和Wi-Fi正常生成头已修复，最终只有既有C99/override/cast-align/constructor warnings。未跑测试套件、未完整kernel链接/CI（无run ID）/boot打包/产物二次检验/设备操作。package继续stageable=false。native model能力、LE feature真实error传播、launch/BlueTool和扫描/配对/BLE/A2DP/HFP仍需完成；不要用本轮对象编译宣称三项目标完成。
- **硬件里程碑已commit/push：`a8a904c`**（feat/bluetooth-native）。继续iOS侧确认20H392 `initCmdReadLeLocalSupportedFeaturesCB`：x19在error/logging分支被覆盖成日志句柄，末尾MOV w0,0吞掉真实失败，不能像supported-commands尾部那样简单NOP。新增三处受UUID/原始opcode保护的进程内edit（共4条指令）：入口把真实status保存到原函数已保存但未使用的x20、保持CBZ成功分支、错误日志用x20、tail用x20恢复真实status；原8 B LE mask在0x1008638c0+0x24的存储完全不改，local-features原status链也不改。COW页数组按edit数分配，无固定6页容量隐患。没有发布能力常量，也不把这项error传播修复称为model getter已完成。
- **iOS续做确切编译**：`python3 -I x9/bluetooth/build.py --out x9/out/bluetooth-native/le-feature-status` exit0，btctl与含LE status修复的20H392 H4/vendor dylib编译/adhoc签名通过；日志 `x9/out/bluetooth-native/le-feature-status-compile.log`，只有既有constructor提示。不二次检查产物、不测试/CI/boot/stage/上机。上一条硬件final对象编译仍为当前内核编译结果；shared XNU树没有修改。下一项仍先取证SSPM endpoint配置/当前LK移交（本地现有备份范围未找到独立SSPM firmware输入），据证据实现CMM routing/ownership而非移除ENOTSUP；同时把native model getters绑定到真实supported commands/local+LE features，再做launch/BlueTool启动和完整场景验证。当前三项目标**尚未全部完成**，分支/worktree继续保留，package不可stage。
- **续做基线确认**：`7f7b4d5e71dd703bd8921d1b6ae5728e51719ed8` 已在 `origin/feat/bluetooth-native`（`git ls-remote`确认），不是未push状态。
- **20H392真实能力实现（仅静态/编译，未实跑）**：新增 `X9Capabilities16.c/.h`，独立核对descriptor `0x1008448e0`（ID<0xfd、24 B stride、opcode+2、bitmap encoding+4）与真实bitmap `0x100863938`；local/LE status与8 B masks由原生回调捕获，status0但NULL参数改为原生invalid-parameters 0x67。10个model getters绑定ext adv/scan、periodic adv/sync、PAST sender/recipient、enhanced connection、spec adv、coded及adv count；2M使用本版本实际lower predicate `0x1000f9d38`（读取LE bit8），不是iOS27 model/cache，额外检查2030/2032。保留原生LE mask存储不改。真实203b以descriptor ID0xd1走原生submit `0x10010ca98`，回调 `(status,sets)`；上限5来自原件7*count/__memset_chk容量35，不冒充控制器能力。回包前不advance，也不暴露部分flags：getter读release/acquire的原子快照。provider原dispatch_once先正常调用，再同步其cache bytes及lower standard HCI selectors；Apple vendor scan gate始终false。
- **重置/广播续修（未实跑）**：HCI Reset callback `0x10010bbd8`入口存x19、tail传真实status，保留原件日志与特殊0x26d处理；reset同时清model snapshot/provider缓存/lower selectors，pending203b保留旧generation等待完成，拒绝旧回包推进新epoch。同epoch重复publisher不重提交/advance。原件broadcaster constructor `0x100509670..6c4`在查询前缓存legacy mode1于实例+0x88；start `0x10050a18c`会在 `0x10050a230..254`使用，已增加原prologue trampoline，在native GAP init前按发布能力刷新，不猜iOS27的+0x90。UUID/opcode与所有COW页预检保留；本轮代码**不等于设备上COW/回调时序已验证**。
- **共享供电/8250修复**：新增shared_done按最后lease结束才清，避免首个ready radio退出、另一个仍booting时重放shared UART mode；AP NONE flow-control补齐Android `ESCAPE_DAT=0xdb`（offset0x40）与escape disabled，保存/回滚数组同步扩大。不实现未知production SW/HW FC策略；worker仍flow0，与源码uart_launcher默认一致，但缺真实启动flags证据。
- **SSPM新原件证据**：已有 `firmware-originals/gps-runtime-surface38-20261110/bl2_ext-original.bin` 的0xaf58调用loader0x3c0ec，随后检查 `app_load_sspm()==NO_ERROR`。loader在局部+0x3c120/124构造 `sspm` 名称，0x3c26c..280将其与 `tinysys-sspm` 交给0x8f568；内层magic0x58901690，ID0/1/99必需、ID3可选CPM，ID1内容复制到0x1ce00000，0x1cf00020/24为共享RAM/mode，0x1cf00000=1 release。现有live.dts将0x9fcf0000/0x300000保留为SSPM-reserved、0x9fbe0000/0x110000保留为sspm_ap-shared；不在XNU allocatable RAM范围内。**这仅证明BL2具备加载路径，仍未证明当前LK成功移交及CMM24M配置**；不把 `spmfw` 字符串当SSPM固件来源。scratch解码 `x9/out/bluetooth-native/sspm-loader*.dis`；重放 `python3 x9/out/bluetooth-native/read_bl2.py 3c0ec 0x1a8` / `3c28c 0x4a0` / `af40 0x65`，原件与scratch不入git。
- **当前源码最终组合编译**：`python3 -I x9/bluetooth/build.py --kernel-objects --out x9/out/bluetooth-native/capabilities-reset-final` exit0；6 BSD对象、UART IRQ hook、2 IOKit owner对象、btctl及含能力/Reset/broadcaster代码的20H392 dylib编译/adhoc签名通过。日志 `x9/out/bluetooth-native/capabilities-reset-final-compile.log`；只有既有C99/override/cast-align/constructor warnings。之前 `capability-publication` 编译发生在原子snapshot改动之前，不能当最终源码结果。未二次核对构建产物、未测试、未完整kernel链接/CI（无run ID）/boot/stage/设备操作，共用XNU源树不改。**post_download仍ENOTSUP、boot-device不加x9_bt_init、stageable=false；扫描/配对/BLE/蓝牙音频未验证，三项目标仍未完成。** 下一项在真实SSPM endpoint/移交证据之外，补齐20H392启动先后顺序及BlueTool的真实XPC契约，不能照参考吞EPERM或合成manufacturer/firmware成功字段。
- **启动桥续做（2026-11-15）**：继续在 `.worktrees/bluetooth-native` / `feat/bluetooth-native`，已确认远端包含 `862344a68752267ad060a049d2d9a62985bdeee7`（真实能力/Reset/broadcaster/shared_done/escape DAT）。新增 `X9Bootstrap16.c/.h`、`X9BluetoothLaunch.c`、`X9BlueTool.c`、`X9XPC16.h`：只读GET_STATUS ABI1的READY/error0/firmware_ready1/chip6653/baud24M和serial boot0必须全部一致；service未注册/真实INITIALIZING才重试，PASSIVE/FAILED的ENXIO/EINPROGRESS也立即原样失败，wait保留同一个read-only userclient并在所有出口close。launcher mobile501，60s门槛通过且库/原daemon可访问后才设H4:POSIX、MT6653和DYLD_INSERT_LIBRARIES并exec原件，避免fwdl期间constructor拒装后永远缺vendor hooks。内核将serial结果置0后才发布READY，消除原先READY+serial ENXIO的发布窗口；设备未跑到该分支。
- **BlueTool原件契约（静态证据，不是实跑）**：20H392 bluetoothd 0x1001d5804构造 `kMsgId/kMsgArgs`，0x10027227c..290发送 `RunBuiltinScript` + args `script`；0x100272d48/dc0/e38三个脚本是boot/init/deepsleep，0x100272574..630读取result(int64)/firmware(string)/manufacturer(int64)，result10/13有特殊含义。新listener `com.apple.BlueTool`，mobile501、`-R`；boot/init仅读完整kernel状态，不发power/HCI、不吞EPERM；失败result1，精确BSD errno为x9_errno，**不合成firmware/manufacturer**。deepsleep明确ENOTSUP，实际firmware-own/clock-release/resume未做。iPhoneOS app SDK缺xpc.h：minimal opaque C声明来自Xcode MacOSX14 SDK，所用符号在原始20H392 BlueTool imports中存在，不定义libxpc内部布局。原件code-sign显示bluetoothd seatbelt-profiles=BTServer/platform-application，BlueTool=BlueTool；不能认为设置DYLD环境或exec就证明注入/沙盒访问已成功。
- **启动配置构建**：`build.py --launchd-source` 只接受显式仓库内两件原始20H392 job输入，先校验Label/mobile/原ProgramArguments/MachServices，再只override `Program` 到 `/usr/libexec/X9BluetoothLaunch` / `/usr/libexec/X9BlueTool`；原arguments、MachServices、KeepAlive、events及其它字段不改。原件来自已挂载root的两件准确路径，副本 `x9/out/bluetooth-native/launchd20h392-input/`；输出原件/派生件分开于 `launchd-originals/` / `launchd-candidates/`，不入git、不安装、不写root。原Apple bluetoothd和BlueTool文件不改，build.json仍stageable=false。
- **进一步必需LE status取证/修复**：原始20H392 `initCmdWriteLeSupportCB` 0x10010c328、`initCmdReadLeBufferSizeCB` 0x10010c450均强制MOV w0,0，且合法成功路径保留x0，改为NOP保留真实status。`ReadLeSupportedStatesCB` 0x10010c458、`ReadLeFilterAcceptListSizeCB` 0x10010c480、`ReadLePAListSizeCB` 0x10010c5d8的成功setter会破坏x0，新增success-only trampoline（首个CBNZ在status0下不可达，用NOP替换；其余prologue逐词校验），失败清capabilities并以各自原step label推进真实错误。SupportedStates setter0x10018f0b4加载8 B指针，NULL+status0→0x67；Filter setter0x1001a9cb0存w0、PA setter0x10008fbe4保留size scalar，**零大小有效，不按NULL处理**。现31个edits/capacity32，额外trampoline位于+256/+288/+320，icache范围352 B。原件解码 `native-init-tail.dis` / `native-le-init-submits.dis` / `native-init-tail-callers.txt`；重放 `read_original.py dis 10010c1c0 0x690`、`dis 10010b0e0 0x8e0`。可选CIS/subrating/default-subrating callbacks仍有原生容错路径，未猜着全改；不能称其它初始化全核对完。
- **Android完整open新增遗漏纠正**：`btmtktty.c:1293..1545` 只到pre_open；`btmtk_main.c:btmtk_fops_open` 之后仍调用 `btmtk_send_init_cmds`（4135..4191），其中UART tty/serdev calibration callbacks实际no-op，接着WMT function power-on，再phase1 WMT/vendor配置；只有DVT分支跳过，不能套生产。新增session FUNCTION/CONTROLLER_CONFIG阶段和function_powered检查，opcode6命令/7 B ACK header来自 `chip/btmtk_chip_common.c:31..32`；仅完整8 B ACK的result0成功、2=busy可50ms退避（1s/20次有界），timeout/解析/partial TX不重发，busy耗尽返回EBUSY，不沿用Android可能留下power_state未设置却返回0的路径。**实际有效phase1/vendor配置或其为空的证据尚缺**，controller_config当前ENOENT；cfg文件loader在USE_DEVICE_NODE==0条件内，不能据示例bt.cfg或缺文件就判定生产命令为空，后续还要对应Android生产用户侧初始化。ownership之后不再直接DONE。首要SSPM post_download仍先ENOTSUP，这些新阶段目前不可达。
- **确切编译版本/最终边界**：上轮 `launch-bluetool16-final` exit0早于wait/launcher最后改动；本轮 `launch-bluetool16-current` exit0覆盖这些改动，`launch-native-status-final` exit0覆盖ready次序/LE回调/启动候选，均不是最后function阶段源码结果。**当前组合** `python3 -I x9/bluetooth/build.py --kernel-objects --launchd-source x9/out/bluetooth-native/launchd20h392-input --out x9/out/bluetooth-native/launch-function-gates-final` exit0，日志 `x9/out/bluetooth-native/launch-function-gates-final-compile.log`；6 BSD对象、UART IRQ hook、2 IOKit owner对象、btctl/dylib/两个启动二进制编译/adhoc签名及两件候选job生成通过，只有既有warnings。未二次检查产物、未测试套件、未完整kernel链接/CI（无run ID）/boot/stage/设备操作，共用XNU树没改。**仍不加x9_bt_init、不交付探针或部分初始化镜像、不合main**。下一项必须得到真实SSPM CMM24M/移交及Android有效初始化输入（含实际UART FC参数），再补配置与sleep/resume，随后才能整合交付并按扫描→配对→BLE→A2DP/HFP/SCO验收；当前三项目标没有全部完成。
- **本次实现提交/推送已确认**：`74648bb9e545625ad253a7177b8d5b9b5352fa82`（`feat(bluetooth): gate native startup on complete Android controller initialization`），`git push origin feat/bluetooth-native` 成功，`git ls-remote origin refs/heads/feat/bluetooth-native` 已读到同一hash。代码与README/交接已提交；分支/worktree保留等待上述原始输入，主树main未切换。
- **本轮续做（2026-11-15，native-optional-fc）**：从 `f057f3f` 继续，只改 `.worktrees/bluetooth-native` / `feat/bluetooth-native`。追完 `uart_daemon/uart_launcher.c:main`：`-f` 是用户选择的Android `UART_FC` enum，`HCIUARTSETBAUD`保存到cif配置；node==1下`HCIUARTGETBAUD/SETWAKEUP/LOADPATCH`不执行node==0的命令/下载逻辑，生产下载在pre_open。`btmtk_main.c:3543..3915/6069` 的cfg loader只编入node==0；不能误要求node==1提供一件并不由该kernel读取的`bt.cfg`。**真实HAL/vendor初始化与有效FC选择仍无原始输入，不据缺文件推断为空。** 内核移除 `flow_control=0` 的launcher默认假设：完整worker现在显式要求 `x9_bt_fc`（0/1/2/3=NONE/MTK SW/Linux/HW）；缺失ENOENT、越界EINVAL、未实现非NONE ENOTSUP均在供电前返回。不添加任何该参数到boot-device，不能选0当生产值。
- **新增Android原件证据（仅静态，不是当前运行结果）**：从已有 `firmware-originals/vcp-real-android-active-b/vendor_boot_b.img` 的v4第一片（type1、45738222 B）解码LZ4 legacy/newc，1047项中窄提取原始 `uarthub_drv.ko`（542968 B）、modules.load和两件Bluetooth HAL manifest到 `x9/out/bluetooth-native/vendor-original-selected/`；没有修改原备份。原件 `uarthub_config_baud_rate_mt6993` 在 `.text+0x13c90..9c` 比较24M后直接到 `.text+0x13e24` 返回0，**没有配置写入**；`uarthub_uarthub_init_mt6993`在 `.text+0x1182c..40`写SSPM SRAM+0x1e4，和源码已有ABI一致。它不证明当前LK已移交SSPM或CMM24M已配置，post_download仍ENOTSUP。manifest实际声明AIDL `android.hardware.bluetooth/IBluetoothHci/default`；此片未找到btmtk module/uart_launcher/init rc，有效服务参数或配置不在此次证据范围内。重放（只读原件）`python3 x9/out/bluetooth-native/read_vendor_original.py`；索引 `vendor-original-surface.txt`，原件符号解码 `uarthub-original-baud.dis`，不入git。
- **iOS原件续做**：20H392 `initCmdLeSetHostFeatureLeCisCB`/`...LeSubratingCB`/`...DefaultSubratingCB`（`0x10010c600/610/620`）完整16 B只有label→强制status0→advance，没有cache/payload setter。原始submit `0x10010b654/750/84c`已经按真实LE feature及unsupported descriptor `0x274`在提交前跳过；保持这些路径不变，新增完成桥仅将**实际已提交命令**的原始status传到sequencer，失败清能力。原件descriptor `0xf3→0x2074`、`0xfa→0x207d`、`0x5d→0x0c31`。HostFlowControl callback `0x10010c6ac`对0x2cd的日志分支会吞错/泄漏logger返回值；桥保留入口status与原step label `0x1006ee068`，错误走sequencer原日志，不冒充成功。四个wrapper无合成payload/缓存，全部原指令preflight后COW写入；patch容量40、实际35项，原trampoline352 B不扩展。原件解码 `native-le-optional-submits.dis` 与既有 `native-init-tail.dis`，重放 `python3 x9/out/bluetooth-native/read_original.py dis 10010b580 0x810` / `dis 10010c600 0x120`；不分析新构建产物。
- **本轮当前组合编译**：`python3 -I x9/bluetooth/build.py --kernel-objects --launchd-source x9/out/bluetooth-native/launchd20h392-input --out x9/out/bluetooth-native/native-optional-fc-final` **exit0**，日志 `x9/out/bluetooth-native/native-optional-fc-final-compile.log`；btctl、20H392 H4/vendor dylib、launcher/BlueTool编译并adhoc签名，两件launchd候选生成，6个BSD对象、UART IRQ hook、2个IOKit owner对象编译通过，只有既有warnings。未运行测试套件、未二次核对新产物、未完整kernel链接/CI（无run ID）/boot/stage/设备操作，共用XNU源树没有修改。**仍不可stage，不合main；扫描/配对/BLE/A2DP/HFP/SCO无实机验证，三项目标未完成。** 后续首先取得原始SSPM/CMM24M移交资料，以及Android成功启用蓝牙的真实HAL/vendor配置、服务启动参数与日志；据实际FC实现对应AP IRQ/escape backend，再接真实sleep/firmware-own/resume，最后启动与场景验收。不能继续用默认参数或部分初始化镜像代替这些输入。
- **本轮提交/远端已确认**：实现与上述交接在 `9897aad219668bebe55ad7d5dd498f84f736b178`（`fix(bluetooth): preserve optional init errors and require explicit Android flow config`）；push exit0，`git ls-remote origin refs/heads/feat/bluetooth-native` 已返回同一完整hash。分支/worktree保留，main与设备均未操作。
- **Android 已开蓝牙的实机输入（2026-11-15，续自 `0376ccb`）**：只读采集于本树 `x9/out/bluetooth-native/android-active-20261115-195016/`，指针 `current-android-capture.txt`。实际为 Android17/MT6993，HAL 加载 `libbluetooth_mtk.so` / `libbt-vendor.so`；活进程参数 **`uart_launcher -f 1 -p /dev/ttyS3`**，tty START=0x11、STOP=0x13。stty显示的38400是termios旧编码，不证明当前硬件baud。真实固件774585 B与固定 `.local/x9/wifi-fw` 原始输入逐字节相同；没有换用开源参考版本。原日志以有效 `*-original-v2.txt` 为准，早期非v2部分是shell引用错误输出，不能当正常日志。未关闭蓝牙、发HCI、写debug寄存器或分区。
- **SW流控实现/编译**：显式enum1→WMT byte0x80；AP EFR=0x1a、XON1/2=0x11、XOFF1/2/INB_WORD=0x13、escape DAT=db/EN=1。UART硬件处理转义/流控，不再软件二次编码H4或raw下载。只保留PIO RDI/RLSI，屏蔽无handler的XOFF/RTS/CTS IRQ；`host_uart`由worker wrapper持RX/IRQ锁，board ops不覆盖它。回滚保存/恢复完整XON/XOFF/INB bank。Linux/HW仍ENOTSUP，缺fc仍ENOENT，不选择NONE绕过。**24M PIO吞吐与SW流控尚未上机验证。** iOS203b拒绝成功回复中>0xf0的畸形数量，分开保存/记录真实controller sets与native容量5，不把本次返回16硬编码进桥。
- **真实HCI证据**：原 `originals/android-initialization.cfa` 经 `python3 -I x9/tools/bluetooth_hci_facts.py <原件>` 解码至 `controller-facts.json`；164 records、0 dropped/truncated、一个Reset epoch、没有读错误或未完成读。version0x0e/revision0x1117/manufacturer0x0046/LMP subversion0x2608，LE features bytes `fffdffffbe450182`，ext features页0/1/2，adv sets16；BR ACL4096/10、SCO244/4、LE ACL251/32、ISO1504/24。这是Android运行证据，不是iOS功能验收。新增解码器仅输出控制器字段，不输出地址/名字/配对密钥。
- **SSPM已取得原件，但移交仍未闭环**：active `sspm_b` 完整2MiB，`bluetooth_sspm.py`索引/分离ID0/1/99（header/data/bytes：0x200/0x218/0x38424，0x3863c/0x38654/0x29f66，0x625bc/0x625d4/0x110）；与BL2 inner24 B/alignment4/magic58901690匹配。解码在 `sspm-rv32-original-v2.dis`，重放 `python3 x9/out/bluetooth-native/read_sspm_original.py 4bfc 0x9c`。ID1有RV32/RVC厂商指令：不能用Capstone skipdata按2 B越过未知4 B指令推分支。找到CMM16060000/dev0/hub映射和sample7/point4/fracdb,1/escape设置，但路径中有12M常量及未知指令，不能推定CMM当前24M，也不能复制AP参数。`post_download`继续ENOTSUP，不盲写 `/proc/driver/uarthub_dbg`（read为空，实际操作靠write）。
- **Vendor原件缺项精确化**：MT6653真实preload表24项，构造器/流程解码 `vendor-preload-original.dis` / `vendor-init-thread.dis` / `vendor-fw-cfg.dis`。已读btinit6192 B，含完整6136 B NVRAM成员，但动态符号真实size **6448 B**，尾部256 B未取得。配置**优先 `/vendor/odm/etc/bt/BT_FW.cfg`**，已取得的 `/vendor/firmware/BT_FW.cfg`只为fallback。优先文件、countrycode/country.nv.switch/sspdebug/pip/protocol1_rat_config五项属性读取前ADB断开；断线0 B与cat错误文件已标`.failed`，不得当有效输入。scratch `emulate_vendor_original.py`在原件下执行了21项构造器，停在countrycode读取；输出 `vendor-emulated-private.json`不是实机回包或生产构建输入，没有猜country/skip条件，`controller_config`继续ENOENT。
- **下一轮只读补采入口**：新增 `python3 -I x9/tools/bluetooth_capture_android.py --out x9/out/bluetooth-native/<全新名>`，自动发现当前HAL/launcher PID，取得优先/fallback配置及真实属性，解析当前ELF导出对象大小与当前maps，不复用旧PID/base。原库最后PT_LOAD的filesz<memsz，btinit跨file-backed结束进入anonymous `.bss`；因此采完整maps，只核对已知符号范围+PT_LOAD并读该6448 B对象，不dump整个进程。ELF表边界、shell假成功、timeout/断线均保留失败与已有原件，不冒充有效库/固件。新增三件Python工具已做源码编译；采集器尚未online运行，本轮 `adb devices`仍空，**没有补得缺失输入**。
- **当前组合确切编译**：`python3 -I x9/bluetooth/build.py --kernel-objects --launchd-source x9/out/bluetooth-native/launchd20h392-input --out x9/out/bluetooth-native/android-swfc-20261115-final` **exit0**；日志 `x9/out/bluetooth-native/android-swfc-20261115-final-compile.log`。6 BSD对象、UART IRQ hook、2 IOKit owner对象、btctl、20H392 H4/vendor dylib、launcher/BlueTool及两件launchd候选完成编译/签名（只有既有warnings）。没有测试套件、新产物二次解包/hash/反汇编、完整kernel链接、CI run ID、boot/stage或实机验收；工具收尾不改变这次C编译结果。main与共用XNU源码不动，不加init/fc到boot-device，stageable=false，分支不合main。先补真实有效vendor配置，再继续SSPM endpoint/当前LK移交、firmware-own/sleep/resume，门禁闭环后才整合与扫描→配对→BLE→A2DP/HFP/SCO验证；**三项目标未完成。**
- **上述里程碑提交/远端确认**：`43efd696f9ae91a33d29f1c6da408d2b061c42cd`，push exit0且ls-remote返回同一hash。其后的补采/移交改动另作下一提交，不能用此hash代表后续源码。
- **ADB恢复后的缺项补采（同日，覆盖上一条“尚未online/缺输入”）**：`android-active-20261115-supplement-1`因Toybox ps显示basename而提前失败，原件保留；修正为basename/path均接受、另核对当前 `/proc/PID/exe`。`...-supplement-2/`取得实际优先1245 B BT_FW.cfg、fallback1104 B、5个属性、全部库、启动dmesg及SSPM。前四个属性（countrycode/country.nv.switch/sspdebug/pip）**实测为空**，RAT=`N/Lf/Lt/W/G`，state=1；不是猜默认。优先cfg末行实际启用 **`VENDOR_CMD: 0x37 0xFC 0x03 0x0D 0x00 0x01`**（A2DP dynamic min power），与fallback不同；旧“追加命令可能为空”的假设撤销。HAL/vendor/BT固件原件与第一采集逐字节相同。
- **完整btinit已取得**：supplement-2的dd返回6448 B对象+81 B统计文本混入stdout，总6529 B被严格size检查正确标为`.failed`，没有当有效blob。采集器改为设备端`dd ... 2>/dev/null`，仅抑制dd统计，仍检查确切符号size；全新窄补采 `x9/out/bluetooth-native/android-active-20261115-vendor-object/`重新发现PID、核对exe、重新读取原库/maps，原 `btinit-active-private.bin` **6448 B**。此次完整映射包含其匿名`.bss`尾部。未重复整个分区采集、未写设备。三件工具源码编译通过；采集器两处live行为修复已在源码，不再说工具从未online使用。
- **GORM有效选择的离线分析（不是HCI抓包/已移植生产代码）**：scratch `emulate_vendor_current_original.py`在原库+完整btinit及真实properties下遍历全部24项：17条构造命令、2个GORM property skip（SSPdebug及country-NV）、5个构造器内部无命令路径；顺序/skip由原 `GORM_FW_Init_Thread 27ff0..28130`分支核对。输出 `vendor-emulated-current-private.json`及opcode/length-only `vendor-emulated-current-summary.txt`。该JSON没有作为构建输入，没有“approved”标记；17条还需review/编码，加优先cfg实际FC37尾命令。country-NV为空时真实分支跳过country构造器，不能人为给country参数。`controller_config`仍ENOENT，但缺口已是**序列移植/嵌入**，不是缺运行输入。
- **SSPM关键运行证据/受限移交实现（覆盖旧固定ENOTSUP说明）**：supplement-2的启动原dmesg **11.067400..469、15.504009..124**记录实测hub208000kHz、mux2；debug源码的顺序确为dev0/dev1/dev2/CMM/AP。dev0与CMM真实FEATURE1/highspeed3/sample7/point4/DLL1/fracdb,1/EFRa/XON11/XOFF13/escape1/db/DMA3，AP是point3，不能混写。此前静态代码12M常量是在104M配置下，同sample在运行208M下不可据源码注释判成12M。`post_download`现在要求当前SSPM request-ready+资源/clock checks、208M mux、CRC enable、两端完整profile匹配（额外DLH0证明完整divisor1），只支持真实SW、拒绝inband；不写CMM任何baud/flow寄存器、不写非SSPM PLL。匹配并再证明所有host idle后按Android清bypass、启用MD/ADSP FIFO和host-awake反馈，并回读实际结果；任一profile不符ENOTSUP。保留其它host的hub IRQ mask，AP driver-own bit3由既有轮询处理，不擅自安装MD IRQ handler。**这是依据Android实跑寄存器的实现，不是XNU当前启动已完成移交的证据。** SRAM default/静态firmware存在仍不足以成功。
- **普通HCI vendor回包路径**：worker独占的 `hci_exchange`匹配完整16-bit HCI opcode与0e/0f，不再错用WMT E4的byte opcode；保留无关HCI包/partial TX错误/超时，不开放用户探针。新增 `x9_bt_session_vendor_command`只接受完整H4 vendor命令（排除WMT FC6F）、核对完整reply length/opcode、保留status，Complete/status0才成功，Command Status仅accepted不能假报完成。原 `GORMevt_HCE_Common_Complete 28a90..28b18`的Status分支忽略status byte，未复制该缺陷；原 `process_cmd_rsp`的5s等待支持此有界timeout。原件解码 `vendor-response-original.dis` / `vendor-process-response-original.dis`，没有在设备发这些命令。
- **本次最后组合编译/停止边界**：早一步 `vendor-hci-separation-20261115` exit0不包含后来的SSPM profile实现。**当前最终** `python3 -I x9/bluetooth/build.py --kernel-objects --launchd-source x9/out/bluetooth-native/launchd20h392-input --out x9/out/bluetooth-native/sspm-observed-handoff-20261115` **exit0**；日志 `x9/out/bluetooth-native/sspm-observed-handoff-20261115-compile.log`。6 BSD对象、UART IRQ hook、2 IOKit owner对象、btctl、20H392 H4/vendor dylib、launcher/BlueTool与两件job候选完成编译/签名，仅既有warnings。没有测试、新产物二次检查、完整kernel链接/CI run ID、boot/stage、XNU上机或扫描/配对/BLE/A2DP/HFP/SCO验证。boot-device仍不加init/fc，stageable=false；main/共用XNU树没改，ADB仅只读。下一项review/编码17条实际构造命令及FC37追加，补controller preflight、firmware-own/sleep/resume；再整合完整候选由用户上机，首先证明当前SSPM/profile/ownership与原生注入/初始化，不能把Android证据或对象编译等同iOS验收。**三项目标仍未全部完成。**
- **补采/SSPM/HCI分离提交已push并确认**：`75a36629181111378709b40da263540742ae90a4`，push exit0，ls-remote同一hash。所有新增原件/私有校准/对象/地址/离线构造命令均在gitignored `x9/out`；仅源码、README和本交接入git。没有新boot镜像，分支/worktree保留，不合main。
- **正式vendor序列移植（续自aaf88c8，2026-11-15）**：新`x9/tools/bluetooth_vendor.py`只用Python标准库，输入固定§2.2的原ELF/6448 B btinit/properties/优先BT_FW.cfg；原件从前三次有效补采copy，未覆盖原始scratch，未用emulation JSON当生产输入。校验原ELF SHA/平台6993/chip6653/type4、24项表顺序、完整properties、所有有效cfg字段、16个TX limits与对象解析结果一致；不认识的属性/可选表分支拒绝，不猜默认。原指令确认BDADDR反序（1b190..1b1c0）；radio signed-byte的22/52 B短长分支（20304/2045c）；24e80/25140功率表输出是96+6+96+6交错，不是192+12；20660的32 B literal被packet+31的0x028005e0覆盖/扩到35 B。当前生成17条原构造命令+确切7 B H4 FC37 append。独立离线原件差分17条逐字节一致（**只作原件佐证，不是HCI抓包或XNU验收**），无设备HCI发送。
- **controller接入/供电前门禁**：移除hardware固定ENOENT controller_config，session直接调用生产序列；下载/board/vendor全部输入与所有packet预检在pre_on/供电前。只有function-on后每条实际Command Complete/status0才累加vendor_completed；失败记录constructor/opcode/status/errno，不打印校准，不跳过、不重发模糊TX。最后FC37不可缺失。常规kernel构建与组合对象构建均调用生成器；显式init要求完整原件。`.local`共享symlink的repo-local检查修正；prepare-local只认共享主树维护链接，不把插件链接改向临时worktree。`prepare-local.py --check` exit0，没有改共享链接/XNU树。
- **此里程碑确切编译/边界**：`vendor-port-20261115-current`失败在.local路径检查（worktree symlink），`vendor-port-20261115-fixed`失败在新生成器C数组的多余右括号；均已按编译日志修正，不把早期失败说成成功。**最后组合**`python3 -I x9/bluetooth/build.py --kernel-objects --launchd-source x9/out/bluetooth-native/launchd20h392-input --out x9/out/bluetooth-native/vendor-port-20261115-complete` exit0；日志`x9/out/bluetooth-native/vendor-port-20261115-complete-compile.log`。6 BSD对象、UART IRQ hook、2 IOKit owner、btctl、20H392 dylib、launcher/BlueTool及两件job候选编译/签名通过。未运行测试套件、未二次检查新构建产物、未完整kernel链接/CI（无run ID）/boot/stage/设备重启。只读`adb devices`确认online1/offline0，没有关闭Android蓝牙。boot-device仍不加init/fc、stageable=false，三项目标仍未全部完成，持续目标保持active。
- **下一步已核对的ownership原顺序，尚未实现生命周期**：生产Android日志17.577982起反复打印`btmtk_uart_driver_own`的CMM/DEV0_BT_AWAKE，证明hw-mechanism路径实跑。源码`btmtktty.c:3205..3515`固件own为10 B`01 6f fc 06 01 03 02 00 01 01`、允许/拒绝sleep回包；hw-mech成功后clear host-awake再clear TX/RX requests。resume为请求→SSPM ready→set host-awake→仅BT_AWAKE0时rawff→有界poll→10 B`01 6f fc 06 01 03 02 00 03 01`及完整10 B ACK→set RX request。pre_open的rawff唤醒本身就是原流程，**不要无证据添加无条件fw-own-clear或重复唤醒**。必须先接sole-worker生命周期与native H4队列/分片、IRQ暂停/唤醒再接BlueTool deepsleep；还不能把睡眠仅换成boot-status检查、保留IRQ访问已gated UART，或靠删除ENOTSUP出镜像。完整kernel link/启动整合和扫描/配对/BLE/A2DP/HFP/SCO实机验收仍待做。
- **正式vendor里程碑提交已push/确认**：`b9856602d5a6302572e07849d1eb5f01e1fe072a`，push exit0、ls-remote同一hash。仅该hash包含vendor移植，不代表以下native队列后续改动。
- **native帧/队列里程碑（同日，续b985660）**：runtime不再直接RX原始1024 B chunk/任意TX partial chunk：RX使用已有H4解析器、先处理exchange保留pending队列，完整frame原子入serial FIFO；暂满保留whole frame，IRQ ring真实溢出仍失败。TX按完整command/ACL/SCO/ISO claim，队列保留到真实UART drain完成；finish同锁发布错误与队列/唤醒，不把drain失败冒充成功。wire上的旧claim完成前close不能reopen；新增write调用锁，避免uiomove时不同writer的512 B块交错。独立8192 B worker frame cap覆盖实际4096 B ACL+5 B H4 header与1504 B ISO；metadata ABI4096不改，incoming/TX buffer静态存储，避免8KiB kernel stack frame。半帧无后续字节也检查1s超时。`btmtktty.c:319..328`及`btmtk_main.c:1179..1235`的node1原raw_filter只在帧边界处理0xff/0x00；不删除payload中的这两值，不套node0-only0xf0规则。当前仍是driver-own常驻，**这些改动不是自动sleep/wake实现或实机吞吐证据**。
- **native当前确切组合编译**：`native-wire-20261115-current` exit0早于raw_filter/write-lock最后改动；**最后**`python3 -I x9/bluetooth/build.py --kernel-objects --launchd-source x9/out/bluetooth-native/launchd20h392-input --out x9/out/bluetooth-native/native-wire-20261115-final` exit0，日志`x9/out/bluetooth-native/native-wire-20261115-final-compile.log`。覆盖vendor+native全部当前源码，6 BSD对象、UART IRQ hook、2 IOKit owner、btctl、20H392 dylib、launcher/BlueTool与候选job编译/签名通过。没有测试套件、新产物二次分析、完整kernel link/CI（无run ID）/boot/stage/扫描或配对验证；设备保持Android蓝牙开启，main与共用XNU树没动。下一项实际实现firmware-own允许/拒绝ACK、sole-worker显式sleep/resume、serial admission与IRQ暂停/恢复，再接BlueTool脚本。尤其host wakeup bitmap源码比较各dev的RX/TX低2位是否**恰为TX-only2**，不是OR所有requests；hw-mech释放先host-awake、再TX/RX request，USB_RX_SEL在请求清除前处理。未有真实wake callback时不能启用FW wake-enhance或开放自动idle睡眠丢掉活动连接。完整目标仍active，不加init/fc到boot-device、不交付部分初始化镜像。
- **native实现提交/远端确认**：`a39d8de26d83f2524404370dba2344a3424c29e0`，push exit0，ls-remote同一hash，tracked tree clean；这条交接记录另作docs提交。仍留在`feat/bluetooth-native`/蓝牙worktree，不合main。
- **并行 ownership/iOS/构建整合（2026-11-15，续3b00153）**：用户明确要求多代理；各子任务独立worktree，提交已逐项cherry-pick回蓝牙树：`2e901c9` Android严格FW-own/driver-own，`7aa0d7a`+`54163bc` BlueTool/Bootstrap/launcher/H4 reopen真实power桥，`9b2646d`隔离XNU/root离线payload构建，`86abd6b`完整init输入与fc1/duplicate构建门禁，`60e9971`wake保留profile纯读/unsafe cleanup，`d181a0f`隔离树重用及严格make结果。未向main合入蓝牙、未设备写入。子分支未push，最终只push主蓝牙分支。
- **worker明确生命周期已接入**：`kern.x9_bt_power`同步队列（0 awake、1 sleep、2 initializing/transition/unavailable）只由sole UART worker执行，root/mobile可写（ANYBODY仅绕过sysctl默认root检查，handler仍核uid0/501）。closed/empty/no-wire-claim admission与serial open/claim同锁；FW拒绝sleep=EBUSY保持driver-own，不把timeout/parser/TX错误当awake。完整ACK/drain后先mask自有IER/GIC再clear HOST_AWAKE/TX/USB_RX_SEL/RX；不释放供电lease。resume先TX request/SSPM resources/当前208M dev0+CMM profile与AP正常bank只读保留门禁，再自有IRQ恢复，不clear pending/FIFO或重绑/重写baud。AP隐藏DLL/DLH/EFR/XON/INB没有纯读retention证明，不能称完整bank验收；不切bank盲恢复。live FW或资源unsafe的cleanup返回真实EBUSY/readyerrno保留lease，不reset/关PMIC假称干净baseline。
- **rawFF同ACK问题已按原件修复**：Android `btmtktty.c:792..835/1220/3429/3472`的MT6653 rawFF与driver-own-clear期望完全相同10 B ACK `04 e4 07 02 03 03 00 00 03 01`。不等event的原send并不抑制回复。worker在rawFF前收排已有完整帧，rawFF只有DEV0_BT_AWAKE0才发一次；轮询后先独立消费该完整ACK再提交own-clear，缺失/重复/畸形/旧own ACK失败不重发，原生无关帧保留pending；不以固定延迟或awake bit当新事务ACK。无外部wake callback，仍明确closed-transport-only，不启auto-idle/wake-enhance/活动连接系统suspend。
- **组合编译范围**：`power-lifecycle-20261115-current` exit0在rawFF barrier、retention与READ-cancel之后修改之前；`power-lifecycle-20261115-barrier` exit0含rawFF barrier，但早于retention/READ-cancel后改动，不能当当前final。日志均在 `x9/out/bluetooth-native/<同名>-compile.log`。iOS原close先_close再等待RxLoop；BSD close先fileproc_drain后d_close，阻塞read不能靠d_close唤醒。现增加私有READ-only cancel ioctl供原生close桥在stop flag之后唤醒reader，TX不丢；iOS close slot wrapper正在完成，尚未宣称close生命周期闭环。
- **完整XNU隔离重建**：首次 `x9/xnu/build-xnu.sh macos --isolated x9/out/bluetooth-native/xnu-lifecycle-20261115/dxb`日志 `xnu-lifecycle-20261115-build.log`：LD/STRIP/DSYMUTIL/CTF实际运行，随后lldbmacros六个INSTALLPY因`env: python: No such file or directory`失败；旧脚本虽然打印ready/exit0，**不认作严格完整install成功**。已加隔离python wrapper、真实PIPESTATUS及fresh link门禁，用`--isolated-existing`在同scratch重跑，结果待写后续条目。共享 `.local/dxb*/xnu`未编辑，原失败日志保留。尚无完整overlay kernel link/boot、root安装、iOS扫描/配对/BLE/A2DP/HFP/SCO；未执行测试套件或新产物二次hash/解包/反汇编。ADB只读devices确认online1/offline0，Android蓝牙未关闭。持续目标仍active。

### 5.12 2026-11-15 ALS 上电/初始化/native HID：代码与候选编译完成，未上机

- **功能提交**：`96d3a79`（`x9 als: add bounded secure SCP startup, sensorhub init and native HID bridge`），已push到`origin/feat/als-power-init-ios`；距离会话可cherry-pick这个提交（已含shared wrapper，勿重复加），type8/控制队列仍需它自己的独立后续提交。
- **分支/证据**：`feat/als-power-init-ios`，基线 main `583f132`。319 `:410` 是 `physical SCP transport NOT enabled`，`:3688–3689` 已由 backboardd 打开 X9AmbientLight；`IONameMatch=als` 另有未匹配请求。不能据此说有真实 lux 或自动亮度。此轮读取的是现有 iOS 日志、`.local/x9/modem/live.dts` 和 OPPO 开源树；**没有新抓 Android sensorservice/dmesg，没有当前 Android 上电实跑日志**，没有 ADB/设备写入、启动或测试套件。
- **硬件/安卓路径**：live DT `light_1` / `als_panel_P_7_1` = **TCS3720，SCP I2C0 addr0x39**；rear CCT 是 TCS3449，不拿后置色温传感器当 LIGHT。由 `sensor/2.0/sensorhub` 控制，不是 sensor/1.0 ALSPS 或 AP 直连 I2C。scp/rv secure-dump 的 region-info 使用 SMC `c2000528/op10`；RESOURCE_REQ=`c2000232/op1`、26M|INFRA=3，DRAM_REQ=`c2000528/op12`，RESET_RELEASE=`op3`。实际 DT 没有 READY_0/channel9，只有 READY_1/channel22（mbox0）；sensor 控制/ACK/notify 为29/30/31/32（mbox1，64-bit slots）。VLP provider 是 **`/clock-controller@1c200000`**，不是 top 1c210000；SCP mux b0/b8、UPDATE1 bit9按 clk-mt6993。
- **已写的链**：`x9_als_scp.inc` 唯一线程 owner：512 B v2 shim handoff资格校验→TFA 已加载固件 region 范围→secure资源请求→LK-held core 的26M/release（不强制复位运行中的SCP）→真实 channel22 READY（TCM size4MiB）→确认GPR reserve与DT一致→awake握手→host READY→真实sensor READY→normal/super FIFO headers→SHARE_MEMORY ACK→TIMESYNC→CRC8/CRC32 LIST（只取type5且非零唯一gain）→ENABLE的delay/latency及匹配ACK→normal36/super76记录→既有lux解码→native HID type12。命令100ms、boot/READY5s有界；WDT/重复READY/坏FIFO/控制超时停止发布并保留errno，不自动重试/复位。普通坏样本/重复时间戳丢弃。真实样本超5s无效。
- **iOS 桥**：仍用 Sensor/AmbientLight usage20/41、integer lux和mach_absolute_time，新增服务名`als`、stage/errno/layout/power-request/firmware/sensor/gain/physical-ready registry属性。ReportInterval在20ms–1s内排队给同一worker，只有真实ENABLE ACK后更新应用值。`x9_als=1`仍仅外部MTK ingress，2仍显式诊断；**3才是native启动实验**，默认boot-device保持1。无伪造lux、True Tone、默认gain，也没有跳过BLControl等待。
- **协作约定**：距离传感器独立worktree `.worktrees/proximity-power-init-ios` / `b0d8096`，本轮不修改它。公开幂等`x9_als_start_shared_transport()`供prox3使用：ALS0可创建、ALS1可升级/复用，ALS2拒绝；同boot只起一次worker，错误不可通过重复调用清除；共享调用pin owner，ALS HID stop不停止其他sensor的transport。**type8 LIST/DATA分流、异步请求回调尚由距离会话后续扩展**，不要再建一个SCP boot线程。
- **构建里程碑**：prepare-local --check通过；sweet_x9_als.o、sweet_x9_barometer.o、SweetHID.cpo单文件交叉编译通过（首次缺声明已修复）；`build.py all --boot-config x9/out/x9-als1-20261115/boot-input.json --name x9-als1-20261115` exit0，97/97 overlay，Image30855168 B，shim-boot14045184 B。原始编译日志在该目录`single-compile*.log` / `full-build.log` / `build.log`，不入git。加入shared接口后的最终候选另记下方。`gh run list --limit 3`返回空，本轮无CI run ID；只改overlay/shim，没有重编XNU对象树。
- **最终构建**：含幂等shared API的`python3 -I x9/build/build.py all --boot-config x9/out/x9-als1-20261115/boot-input.json --name x9-als2-20261115` **exit0**，候选`x9/out/x9-als2-20261115/boot.img`，Image30855168 B / shim-boot14045184 B；该目录`full-build.log` / `build.log`。未上机、未二次解包/hash/反汇编，只有构建内置检查。
- **未验/限制**：此代码没有下载/修改SCP固件，仅使用LK/TFA已加载的签名原件；还没有ULPOSC校准同步、RTOS feature/DVFS和正常低功耗交接，26M/INFRA/DRAM票与映射保留到重启，停止不声称已断电。DT的gain-cali/面板补偿/LCD参数、固件实际READY、GPR窗口与lux随遮挡变化都待运行验证；不能用ACK称光感/距离可用。**候选基于main的GPU280 allowlist，而319实跑GPU283**；不能当作319的单变量替换。当前GPU worktree需cherry-pick功能提交后按其已stage的283基线重建，或先解决280插件资产，才验iOS自动亮度；本轮没有新plugin-package/mailbox操作。
- **最小复建/日志定位**：从boot-device拷贝JSON到`x9/out/<新名字>/boot-input.json`，仅把`x9_als=1`替换为3，然后`python3 -I x9/build/build.py all --boot-config x9/out/<新名字>/boot-input.json --name <新名字>`。用户手动临时fastboot boot；遮挡/照亮各数秒，保留原始日志。`rg -n 'X9 SCP FDT|X9 ALS|IONameMatch.*als|open X9AmbientLight' ~/N.txt`，预期真实LIST/ENABLE→stage11→`dispatch seq=... lux=... source=MTK`，无数据时应NULL/invalid而不是0lux。Android侧如需新基线先确认设备状态，再运行只读`capture_android_sensors.py`；默认已改为gitignored `x9/out/sensor-baseline-<time>`，不再在.local创建实验目录。

#### 320 实机核对（2026-11-15）— HID 接入成功，SCP 停在 region-info，光感尚未可用

- **观察**：原始日志 `~/320.txt`，摘录 `x9/out/als-log320.txt`，均不入git。`:393–395`（1.144–1.152 s）native worker进入stage1/2，HID注册为`als`；`:612–613`（2.117–2.120 s）首个`SMC c2000528 op=10 arg=0 -> ffffffffea000000`后即`exit stage=2 errno=45 firmware=0 sensor=0 packets=0 records=0`。`:3625`（14.438 s）backboardd打开X9AmbientLight返回0；`:10899/10901`（35.832 s）SpringBoard的`IONameMatch=als`匹配X9AmbientLight。**iOS服务注册/打开已实跑，但没有READY、ENABLE或真实lux，不能称光感或自动亮度成功。**
- **源码解释/推断**：`errno45=ENOTSUP`由`x9_als_scp.inc`中getter的`v > 0xffffffffULL`分支生成，并非日志证明固件拒绝SMC。返回值与固件reserve起点`0xea000000`的32位符号扩展一致；安卓`scp_helper.c:2262–2276`把`scp_do_get_region_info()`的64位返回赋给`uint32_t`数组，结构字段也是32位。本实现提前拒绝了该表示，因此**未进入stage3的26M/INFRA/DRAM上电请求**。此结论仅定位首个阻塞，不证明其余region字段或后续初始化正确。
- **下一步**：针对GET_REGION_INFO窄修复32位字段的零扩展/合法符号扩展解析，保留错误码、结构大小及reserve范围检查；不要把其他SMC一律截断。下一候选应先读完全部8个字段，再验证POWER/READY/LIST/ENABLE/DATA和遮挡下lux变化；继续注意GPU280/283启动基线，不能仅此修复就宣称上电成功。
- **本轮范围**：只读320和安卓源码，交接分支`feat/als-log320-status`（基线`4793856`）；仅更新NOTES并提交合入main/push。没有驱动修复、编译、新boot、测试套件、CI run或设备操作；320的确切镜像/GPU基线本轮未核定。复核命令沿用上方，将`~/N.txt`换成`~/320.txt`。
- **用户确认/修复归属（2026-11-15）**：用户要求“一切按安卓的数据来”。已向距离会话`01a1deb1-fb7e-7b23-8cb4-56b4b2c63fda`发消息，由它在独立worktree统一处理op10的32位zero/sign extension窄修复，并保留SMCCC错误、reserve/结构范围检查；本会话不重复修改共享worker。距离会话报告type8 LIST/ENABLE/DISABLE ACK/36B DATA已接完且整包通过，**本会话尚未核验对应收尾提交、产物路径及GPU基线，仍待其回传**。优先依据安卓实测/流量、live DT及对应源码；缺少实测的校准、增益、时序不补猜测值，ACK不代替真实光照/近远DATA验收。本次仅协作交接，无新增编译、CI或设备操作。

#### 320 光感单变量修复 — Android u32 region-info

- **实现/基线**：距离会话的独立修复`6eee23a`仅改op10专用getter，已在`fix/als-region-info-320`（worktree `.worktrees/als-region-info-320`）cherry-pick；NOTES冲突只保留本会话光感交接，不引入未合入的type8功能说明。候选固定在`2fead1c`（与已构建ALS2相同驱动/GPU基线），不带距离功能，不包含其他会话同期合入main的通话更新`e9f7b7c`，便于与320差分。
- **Android依据/改动边界**：`scp/rv/scp_helper.c:2262–2276`逐项存入`uint32_t`，`scp_helper.h:180`结构字段同宽；`mtk_sip_svc.h:9–13`定义-1/-2/-3/-4。仅GET_REGION_INFO接受u32零扩展/合法符号扩展，拒绝其它形状并保留四个错误码，原始64位SMC日志不变。完整8字段读取后的结构大小/对齐、loader/firmware/DRAM在DT reserve内检查不变；不改其它SMC、时钟/复位/READY、增益或协议，不伪造样本。
- **启动基线纠正**：320 `:1065`明确`active plugin=X9GeneralMetal280 from verified allowlist`，`:3692` backboardd加载`X9GeneralMetal280`/`X9PanVK280`。**320实跑GPU280，319实跑283；不能把319的限制套用到320。** 本ALS-only候选继续280，无新插件包；若设备仍为320插件状态则无需mailbox，否则不能假定当前插件未变化。
- **准备/待办**：共享`.local`软链仍指向主checkout；worktree运行`prepare-local.py --check`因`MaliMetal/X9/General`指向main而拒绝，未重指共享软链以免干扰其他会话。随后在主checkout核验固定输入；kernel/boot构建从本worktree直接取维护源码，不构建插件。下一步完整编译，结果另记；尚无修复后的实机READY/ENABLE/DATA证据。
- **最终构建（2026-11-15）**：上述局部cherry-pick为`e7cc23b`（源提交`6eee23a`）。主checkout `python3 x9/build/prepare-local.py --check`通过，`gh run list --limit 3`为空，无CI run ID。在ALS worktree执行`python3 -I x9/build/build.py all --boot-config x9/out/x9-als-region320-20261115/boot-input.json --name x9-als-region320-20261115` **exit0，97/97 overlay（3 prebuilt）**，Image30855168 B / shim-boot14045184 B。输入逐字复制原ALS1的ALS3配置；未新增prox、GPU参数。整目录已移到主checkout **`x9/out/x9-als-region320-20261115/`**（`boot.img`、`boot-input.json`、`full-build.log`、`build.log`等），日志中的原worktree绝对路径是构建时位置。未重编XNU对象树、未构建插件/运行mailbox、未跑额外测试套件、未二次解包/hash/反汇编、未操作设备。
- **交付/最小验证**：此ALS-only候选固定`e7cc23b`，即使main随后合入距离/通话更新，也不要把它标成最新main全功能镜像。从主目录由用户手动`fastboot boot x9/out/x9-als-region320-20261115/boot.img`，保持320的GPU280插件状态，照亮/遮挡各数秒。下一日志先核对8次`SMC c2000528 op=10 arg=0..7`均读完、是否进入stage3，再核对真实READY/LIST/ENABLE及`dispatch seq=... lux=... source=MTK`；缺少任一环节继续定位最早失败，不宣称物理光感/自动亮度成功。复核`rg -n 'X9 ALS|IONameMatch.*als|open X9AmbientLight' ~/N.txt`。距离会话独立双传感器候选不混用于本单变量验证。

### 5.13 2026-11-15 距离传感器：共享 SCP 初始化/控制与原生 iOS 桥已编译，物理链未验收

- 分支 `feat/proximity-power-init-ios`，worktree `.worktrees/proximity-power-init-ios`；基线 `583f132`。距离 ingress/HID=`b0d8096`、coalesce/stop ACK/只读捕获=`036221a`（均已push）；已merge共享上电依赖`96d3a79`及交接`4793856`，没有覆盖主目录其他会话。**代码已接共享worker，但没有物理距离 DATA/自动熄屏验收**。
- **实机静态事实**：`.local/x9/modem/live.dts:3675` 前置 proximity 是 SCP I2C0 `0x39` / EINT24，`sensor_name=9`、`ps_power_config=0xa`，对应 panel `device_name=tcs3720`。公开 OPPO `oplus_sensor_24081.dtsi:97` 模板为 TCS3760 / sensor_name21，不可覆盖实机事实或照搬传感器寄存器；校准/供电由原 SCP 固件负责。
- **现存运行日志**：`~/319.txt:410` 只见 ALS HID registered、physical SCP NOT enabled；313–316/318/319 未找到 proximity ingress/ACK/DATA。本轮无 Android sensorservice/near-far 原始捕获，不把缺 marker 当硬件不存在。Android `sensor/2.0/core/hf_sensor_type.h` 定义 PROXIMITY8；`sensorhub/transceiver.c:321` 原样传 `value[0..2]`（不是 ALS lux），`share_memory.h` 为 40B info / 36B data。
- **原始 iOS16 20H392 的精确接入**：未修改 backboardd（根盘克隆）ObjC+反汇编显示 `BKHIDUISensorConfiguratorFactory::_proximityServiceWrapper` `0x100046f0c` 的 CFNumber 匹配值是 DeviceUsagePage=0xff00 / DeviceUsage=8；block `0x100047134` 检查 Placement=1（top）。`BKHIDUIDiscreteProximityServiceWrapper::applyUIMode` `0x10002bfd4` 写 DetectionMode0..5；`BKHIDProximityEventProcessor::processEvent` `0x10002cd48` 读 field0xe0000，`0x10002cdd8` 提 Receiver mask bit6作为 objectWithinProximity。Sensor20/12 是另一个 human-presence/biometric 路径，不能冒充离散 receiver proximity。
- **实现**：`sweet_x9_proximity.c` / `X9Proximity.h` / `hid/X9Proximity.inc` + recipe/link-order/personality。`x9_prox=1` 外部 MTK ingress、2 标记诊断、3 共享 SCP native；`x9s` bit3 相当于 native3。HID先注册 identity供 iOS 写 DetectionMode，但未有有效 DATA 时 copyEvent=NULL；mask near=0x40 / far=0，不伪造 FAR/厘米。PS是 on-change，不能像 ALS 在5s后因无变化作废；DISABLE/reset/fault作废。保留 value[1] steps与 value[2] 辅助值，不拿ADC设自造阈值。
- **待验证推断**：value[0] oneshot采用 Android 二值约定0=near/1=far，但 sensor2.0 AP源码只证明原样透传，尚缺该设备HAL/真机遮挡数据证实极性；真实事件接通后先复核0/1与遮挡关系。`debug.x9_prox_test_near`只在显式mode2允许root写（1=near/0=far/-1 invalid，与Android oneshot极性相反），不能当物理距离成功。
- **构建结果**：本轮 `sweet_x9_proximity.o` / `SweetHID.cpo` / `KernelConfigTables.cpo` 单文件交叉编译 exit0（原有warning）；日志 `.worktrees/proximity-power-init-ios/x9/out/proximity-compile/{build.log,cxx-driver.log}`。原始分析/反汇编放该worktree `x9/out/proximity-analysis/`，不进git。未跑测试脚本/套件、未上机；`gh run list --limit 5`为空，无CI run ID。
- **共享链已实现**：prox3调用幂等`x9_als_start_shared_transport()`，同一SCP owner负责上电/READY→SHARE_MEMORY→TIMESYNC→完整CRC LIST（唯一type8的40B info缓存，HID晚附着无需新LIST）→type8 ENABLE/DISABLE及matched ACK→normal36/super76的36B前缀分流。DetectionMode0保持OFF、1..5请求ENABLE；在ACK飞行中coalesce至最新期望。stop后仍完成已排队DISABLE，超时quarantine不能被迟到ACK、diagnostic/external重启清除；不自动重试/复位。type8缺席只使proximity ENODEV，不终止有效LIGHT。LIGHT按ALS HID demand开关，proximity-only启动不会偷偷开LIGHT，ALS stop不关闭共享owner；停用后的在途DATA丢弃而非造成共享fault。
- **最终编译里程碑**：共享适配的单文件日志`x9/out/proximity-compile/shared-driver.log`；`build.py all --boot-config x9/out/x9-proximity1-20261115/boot-input.json --name x9-proximity1-20261115`及收尾后`... --name x9-proximity2-20261115`均**exit0，98/98 overlay**，Image30875648 B / shim-boot14053376 B，最终候选为本worktree `x9/out/x9-proximity2-20261115/boot.img`，`full-build.log`/`build.log`同目录。boot输入由boot-device仅将als1换3、追加prox3；没有plugin构建/mailbox/设备操作/测试套件/构建后二次检查。只改overlay，不需重编XNU对象树，无CI run ID。
- **只读/状态工具**：`x9/tools/capture_android_proximity.py`仅在明确X9_SERIAL下读取sensorservice/DT/过滤日志/SCP status，不启用传感器、不写节点，本轮未执行。`x9/proximity/proxctl.c`/`build.py`经iPhoneOS SDK交叉编译及ad-hoc签名exit0（首次SDK标记SetCFProperties unavailable已按既有USB工具改为20H392 IOKit runtime dlsym；未改ABI）。产物`x9/out/proximity-compile/client/proxctl`；`--status`、`--watch 1..60`只读，root `--mode 0..5`经正常registry属性提交，成功不是ACK/DATA证据；未安装进根盘/设备。
- **320阻塞及修复**：`~/320.txt:612–613`的GET_REGION_INFO op10返回`0xffffffffea000000`曾被getter拒为errno45/stage2，尚未发POWER/READY。已按Android uint32字段在`6eee23a`限定修复zero/sign-extension及SIP错误处理（细节见下），并编译进proximity3/4；不重测未修的proximity1/2。仍没有修复后的真实上电/距离DATA结果。
- **未验/启动基线**：SCP仅用LK/TFA已加载原固件；ULPOSC/DVFS/DT与工厂校准下发/完整低功耗尚未验，26M/INFRA/DRAM票保留至reboot，不声称停止即物理断电。319实跑GPU283，但**新320:1065已明确active plugin=X9GeneralMetal280 from verified allowlist，:3692 backboardd加载X9GeneralMetal280/X9PanVK280**；最终候选沿用GPU280，无新plugin-package，设备若仍保持320的280资产则无需mailbox。proximity4开ALS3+prox3且含最新main call代码，不是320的单变量差分；ALS会话另做仅op10修复的ALS-only候选。本轮不stage。
- **下一步/验收**：op10窄修复已编译；优先ALS-only对320差分，联合距离候选由用户手动临时boot。`rg -n 'X9 SCP FDT|X9 ALS SCP|X9 PROX|open X9Proximity' ~/N.txt`，要求真实READY→LIST type8→iOS DetectionMode→ENABLE ACK→覆盖/移开两方向DATA及mask→mode0 DISABLE ACK。先核实oneshot极性，再单独验iOS原生遮挡熄屏/恢复；电话未闭环，不能预称通话成功。

#### 320 的共享 GET_REGION_INFO 窄修复（距离会话统一实现）

- type8共享控制/FIFO/iOS桥提交`a0df27c`（已push）；已merge`66f7f2a`的原始320证据，保持ALS与proximity共用一份worker，不覆盖其他会话。
- Android依据：`scp/rv/scp_helper.c:2262–2276`的`uint32_t`数组、`scp_helper.h:180`的32位字段；`include/linux/soc/mediatek/mtk_sip_svc.h:9–13`的SIP错误0/-1/-2/-3/-4。仅`c2000528/op10`新增`als_get_region_word()`，接受高32位为0，或低32位bit31置位且高32位全1的合法符号扩展；非此形状EOVERFLOW，-1/-2/-3/-4分别ENOTSUP/EINVAL/ERANGE/EPERM，不当地址。`status.last_smc`和日志保留原始64位；其它SMC仍用原完整返回判断。
- 完整8字段读取后的56..72B/4B对齐结构检查，以及loader/firmware/DRAM的DT firmware reserve范围检查原样保留；此修复不伪造READY、不改固件、不发额外reset。320实际`0xffffffffea000000`现在能进入32位地址分支；**尚无修复后的实机结果，不代表后续POWER/READY/LIST/DATA已通过**。
- **修复提交/构建**：`6eee23a`已push；完整proximity3（src6eee23a）exit0，Image30875648 B / shim-boot14053376 B。再merge当前main的`e9f7b7c`（保留call会话工作）得到`b4aa2ef`，最终`python3 -I x9/build/build.py all --boot-config x9/out/x9-proximity4-20261115/boot-input.json --name x9-proximity4-20261115` **exit0，98/98 overlay**，Image30875648 B / shim-boot14057472 B；没有测试套件、构建后二次检查、设备操作或新CI。
- **最终交接/产物**：功能已合main/push；距离worktree的候选、原始分析与编译日志整体移回主仓库gitignored `x9/out/`后清理该worktree/本地功能分支。最终候选`x9/out/x9-proximity4-20261115/boot.img`，源码基线`b4aa2ef`（a0df27c+6eee23a+main e9f7b7c），输入/日志为该目录`boot-input.json`、`full-build.log`、`build.log`。`x9/out/proximity-analysis/`、`x9/out/proximity-compile/`、proximity1/2/3保留原始文件，旧日志中的worktree绝对路径是构建当时路径，不回写原件。
- **iOS工具复建/人工验证**：`python3 -I x9/proximity/build.py --out x9/out/proximity-client-<新名字>`；既有签名client位于主仓库`x9/out/proximity-compile/client/proxctl`，未安装。由用户自行部署后`proxctl --status`/`--watch 20`看状态，必要时root `--mode 1`提交开、`--mode 0`提交关；不要与backboardd的自动模式切换并发干预，更不能用提交成功/diagnostic当物理DATA。覆盖/移开验证SCP oneshot0/1极性与HID mask0x40/0，之后才验原生熄屏/恢复。
- **协作差分候选**：ALS会话独立worktree`als-region-info-320`取`2fead1c`+cherry-pick6eee23a，不带本轮type8和call，做ALS-only/GPU280/原ALS3输入，名称`x9-als-region320-20261115`；其确切编译/上机结果由该会话另记，不能混成距离候选已实跑。

#### 2026-11-15 用户要求合入main并删除光感/距离分支

- main `f2797a9`已含`96d3a79`（ALS）、`a0df27c`（type8）、`6eee23a`（op10）、`e7cc23b`/`05d13f0`（ALS-only候选交接），逐项`git merge-base --is-ancestor`通过；无尚待合入的这两项功能。本地光感/距离功能分支及worktree已在上轮清理，用户本轮要求后再删除远端`feat/als-power-init-ios`（96d3a79）、`feat/proximity-power-init-ios`（8694bfd），`git push origin --delete`成功。ALS/proximity候选与原始构建日志仍保留在gitignored `x9/out/`。
- 远端配置仅跟踪main，因此不能用`git branch -r`当远端总数；本轮用`git ls-remote --heads origin`读取实际GitHub分支。只清理用户指定的两条传感器分支，不动audio、GPU、Bluetooth及call活动分支/worktree。本次仅git/交接维护，没有构建、CI、设备操作或上机新结果。

#### 321 实机核对（2026-11-15）— region-info 已过，共享链停在 LIST，两个传感器仍未可用

- **原始证据/基线**：只读分析 `~/321.txt`（17730行，日志覆盖启动至81.650s），传感器摘录保存在主仓库 gitignored `x9/out/sensors-log321.txt`，原始日志/摘录不入git。`:406–407`明确 proximity mode3、ALS native SCP；`:986`明确 `active plugin=X9GeneralMetal280 from verified allowlist`。这与联合 proximity4 的启动基线一致，但日志没有唯一镜像ID，不能仅凭这些marker断言确切boot文件。分析时main基线`af62d8b`，共享worker及stage定义与候选`b4aa2ef`逐文件无差异。
- **已修复/已观察**：`:618–625`（2.087–2.106s）GET_REGION_INFO op10的8字段全部读完，包括 `ffffffffea000000` / `ffffffffea002000` / `ffffffffea700000` / `ffffffffeae49800`；`:626–628`进入stage3，26M/INFRA资源请求与DRAM_REQ均返回0。`:700` RESET_RELEASE返回0，`:723–725`（2.426–2.431s）出现真实 `READY channel22 image=400000`、firmware DRAM reserve与DT一致并进入stage6。`:731/736`分别是SHARE_MEMORY（type0 cmd11 seq1）及TIMESYNC（type0 cmd10 seq2）的匹配ACK，均 `-> 0 ret=0`。因此320的u32符号扩展阻塞已实跑消失，SCP启动和前两条控制命令已经走通；不是只注册了HID。
- **当前阻塞/结论**：`:737`（2.464s）进入stage9；`:823`（2.793s）`exit stage=9 errno=60 firmware=1 sensor=1 packets=2 records=0`；`:846` proximity收到 `transport errno=60; no synthetic screen state`。stage9按`X9ALS.h`是LIST，`errno60=ETIMEDOUT`；`als_list()`只有发送notify成功但100ms期限内没有设置`als_reply_received`时走该超时分支。**没有被接受的LIST回复，没有进入ENABLE/STREAM，记录数为0；光感/距离及自动亮度/遮挡熄屏仍未修好。** 日志不含原始notify包和reject原因，不能区分固件未回、回包晚到、CRC/头字段/seq匹配被拒，不能据此宣布硬件或固件故障。
- **iOS桥边界**：`:3340`（12.365s）backboardd打开X9AmbientLight返回0；`:3350–3351`（12.406–12.408s）打开X9Proximity返回0且UserClient open=1；`:11906/11908` SpringBoard的`IONameMatch=als`匹配成功。这些发生在共享worker退出之后，仅证明服务匹配/打开，不证明有lux、近远DATA或系统自动效果；全日志未见LIGHT/proximity LIST、ENABLE ACK或真实传感器dispatch。
- **下一步**：只针对最早未通过的LIST入口，对照Android sensor/2.0 的共享TX/RX结构、CRC32覆盖、notify command9的字段与sequence语义和实际回复时序；下一次最小观测记录发包header/CRC、IN32原始28B及拒绝原因、相关IRQ/TX/RX状态和超时时间。不先改增益/极性、放宽CRC或伪造样本，也不重跑已确认通过的op10修复作为新结论。LIST真实通过后才继续LIGHT/type8 ENABLE与遮挡DATA验收。
- **本轮范围/交接**：分支`feat/sensors-log321-status`仅更新NOTES；没有驱动修改、编译、新boot.img、CI run、测试套件、mailbox/ADB或其他设备操作。本轮文档提交合main/push后清理本地交接分支，既有候选镜像保持原样。

#### 321 LIST 窄取证（2026-11-15）— Android 格式核对完成，候选只增加观测

> **后续用户已取消探针路线**（见下一节）：保留本节为历史构建记录，不再建议启动本节的定位镜像。

- **分支/固定输入**：`fix/scp-list321`，worktree `.worktrees/scp-list321`，源码从联合镜像`b4aa2ef`起步，不混入后续call/SMS功能；NOTES恢复为`f948f57`的当前交接。boot输入将逐字复制主仓库`x9/out/x9-proximity4-20261115/boot-input.json`，保持ALS3+prox3/GPU280，不增加boot token。本轮没有新物理运行数据，321确切boot文件仍不能由日志唯一识别。
- **Android协议核对**：本地OPPO `sensor/2.0/sensorhub/share_memory.h`、`share_memory.c:444–700`、`sensor_list.c:135–148`、`sensor_comm.c:127–144`及tiny_crc8/32；common channel0/list command0、SENSOR_TYPE_INVALID0、notify command9/length12、TX id9/RX id10、首尾DEADBEEF、共享header12B+length4B、tx_len0/rx_len4096/length4096、CRC32从header覆盖16B（回复再加rx_len），均与现有LIST代码一致。CRC8=poly0x8c/init0覆盖外层前4B；CRC32=reflected0xedb88320/init/xoroutffffffff亦一致。Android仅配置normal/super FIFO，v2 TX/RX不在SHARE_MEMORY配置中，与现实现相同；默认reserve表缺省size/alignment均0，id9之前的缺省项未发现额外偏移。UniXNU/hackios参考未找到可直接移植的该sensorhub v2 LIST实现。
- **仍有证据缺口，不作修复猜测**：Android`share_buffer_seq_comm`接收outer seq/type/cmd的异常判定是`&&`，本实现必须outer seq匹配；这是源码差异，但321没有原始IN32，不能据此放宽。Android inner share_buffer_header.sequence不参与校验，不要把inner0与outer3当作错误。321从stage9到exit约329ms，现有wait在sleep后先检查deadline、可能不再poll；只能怀疑调度超期时有回包未消费，尚无pending IRQ/原始包佐证。不改超时长度、CRC/序号、共享内存地址或重试策略。
- **实现/已编译**：`x9_als_scp.inc`只在LIST等待期间缓存最多4个IN32、2个IN30及原因/相对时间、poll次数与最大间隔，等待结束后才打印（避免serial输出挤占100ms窗口）；记录OUT31原包、预期outer seq、起止IRQ/OUT31、未消费pending IN32、TX/RX首24B/尾magic/资格范围内地址。保持既有W1C、发送、成功条件与100ms deadline，不在超时后消费包/补poll。`reason1=CRC,2=length,3=READY,4=LIST accepted,5=LIST seq mismatch,6=other`。新增只读离线解码器`x9/tools/decode_scp_list.py`输出JSONL并复算外层CRC8，不声称仅头部能验证整段CRC32。
- **准备结果/当前待办**：主checkout `python3 x9/build/prepare-local.py --check`通过；worktree经原Builder模板单文件`compile_one(sweet_x9_als.o)` exit0（28个warning），日志`x9/out/scp-list321-compile/single-compile.log`/`build.log`。`gh run list --limit 3`为空，无CI run ID。下一步固定代码提交后完整kernel+boot打包，结果另记。遵守x9约定，不运行测试脚本/套件，不做构建后二次解包/hash/反汇编；没有设备、mailbox、ADB或分区操作。
- **固定提交/最终构建**：源码`e235820`（b4aa2ef+LIST取证；NOTES同步不影响代码）。`python3 -I x9/build/build.py all --boot-config x9/out/x9-scp-list321-20261115/boot-input.json --name x9-scp-list321-20261115` **exit0，98/98 overlay（3 prebuilt）**，Image30875648 B / shim-boot14061568 B。离线解码器仅Python syntax compile通过，未执行测试。只改overlay/工具，没有XNU对象树重建、插件构建、CI run或硬件操作，未二次检查产物。候选与单文件日志迁回主checkout gitignored **`x9/out/x9-scp-list321-20261115/`**、**`x9/out/scp-list321-compile/`**；`boot-input.json`、`full-build.log`、`build.log`保留，旧worktree绝对路径不回写。
- **交付/下一次最小验证**：这是**定位镜像，不是LIST或传感器已修复镜像**。用户手动 `fastboot boot x9/out/x9-scp-list321-20261115/boot.img`；boot完成后留原始`~/N.txt`即可先查启动LIST（无需为LIST试验拨电话/遮挡；若后续意外走通再验真实光照/近远数据）。GPU仍280，设备若保持321插件状态则无需mailbox。先 `rg -n 'X9 ALS LIST:|X9 ALS SCP:|X9 PROX' ~/N.txt`，原包可用 `python3 x9/tools/decode_scp_list.py ~/N.txt > x9/out/list-N.jsonl` 解码。`reason5`证实outer seq拒绝，`reason1/2`证实CRC/长度拒绝，`pending=1`加合法LIST原包仅证实超时快照时有未消费回复（不证明deadline前到达）；RX已写而IN32无接受也需对照原包，不能把头部变化当整段CRC通过。根据第一处实际差异再修，不把诊断候选当成功验收。功能合main/push后清理交接worktree/本地分支，候选固定提交不随main更新。

#### 321 LIST 静态续查（2026-11-15）— 备份固件支持 VALUE 序号回显，发送侧统一序号；未构建/上机

- **用户当前规则/镜像回答**：本轮要求“不要出探针镜像，多静态分析”。取消上一节探针启动建议，本轮不编译、不打包新 boot、不操作设备。最新已有的**非探针双传感器候选**仍为主仓库 `x9/out/x9-proximity4-20261115/boot.img`（源码 `b4aa2ef`，ALS3+prox3/GPU280）；后出的 `x9-scp-list321-20261115` 只是旧定位包，不推荐。321 没有唯一镜像ID，不能声称已证实其确切文件。321 的 region-info/READY/SHARE_MEMORY/TIMESYNC 已过，但 LIST 超时、records=0，物理光感/距离仍未验收。
- **分支/输入来源**：`fix/scp-list-static321`，工作树 `.worktrees/scp-list-static321`，基线 main `82c72b4`。只读原件 `.local/x9/backups/partitions/firmware-originals/gps-baro-vendor5-android-files/scp_b.bin`（16 MiB，2026-11-10 备份），没有修改/下载/替换固件。其 MTK 容器 `tinysys-scp-RV55_A` 在分区 offset0，512 B header 后 payload 长 `0x426c38`；DRAM 容器在 `0x472000`，payload 从 `0x472200` 起长 `0x7496c0`，长度与321 op10 field5相同。**该长度一致不是当前固件唯一版本证明**。以下地址均为提取出的 SCP payload base0，不是分区偏移或 AP 地址。
- **工具可靠性边界**：r2 6.1.8 的普通 RV32 解码把该固件的部分压缩访存/双寄存器移动误报为 `fld/fsd`，并将某些未知32位指令拆成2B，不能直接照抄其伪代码。公开 Andes binutils `dd22be9a0ef80fde64e65e57d25f988a1ea1460f` 与 Zc 文档用于核对 `cm.mvsa01` 的 mask/寄存器；其 ratified Zcb byte-load opcode **不匹配本固件**。本轮从多处同型 packet 字节复制和 CRC8 table lookup 交叉恢复了所需的 RV55 byte-load/store；派生 `rv55_slice.py` 明标 `[recovered RV55]`，未知指令保持 unknown，不宣称全 ISA 已解码或外部 opcode 文档已证实所有压缩指令。
- **窄数据流/静态证据**：notify handler `0x8698a` 保存 outer四字节到队列对象+8..11，`0x869f2/0x869f6/0x869fa` 将 incoming `notify.value`（n+8）复制到对象+28；`0x86ace/0x86ad8` 把该 VALUE header（对象+28）作为 a1 传入 `0x886d2`。`0x886da` 的 `a6ad` 匹配公开 `cm.mvsa01 s3,s1`，所以 s1 保存 VALUE header，而非 outer header。回复构造 `0x88824` 的 `8e20` 恢复为 `lbu a1,0(s1)`，紧接标准 `0x88828:2308b102 = sb a1,48(sp)`，写入 reply.outer.sequence；type 来自 VALUE+1，标准 `0x88836` 写 command9/length12。发送函数 `0x87788/0x8778e` 再将传入 byte0复制到 outgoing+8，`0x877e4..0x877ee` 仅复算前四字节CRC8，没有重新分配序号。这条链**支持备份固件从 VALUE.sequence 回显 outer.sequence**，不是 Android AP 的弱 `&&` 规则证明。`0x8883e:aca0` 恢复为从 VALUE+2读 halfword，会覆盖 a1；不能误把先前的0x0c09当作内层channel/command/sub_command，否则会得出错误的第二个协议问题。
- **现实现差异/推断**：旧 `als_list()` 清零 shared header，TX memory.sequence=0、notify.value.sequence=0，而 `als_notify()` 独立递增 outer.sequence；321 SHARE_MEMORY seq1、TIMESYNC seq2，因此下一 LIST outer应为3。按上述固件数据流，回复 outer会是0，被当前 `n[0]==als_expected_sequence` 拒绝，随后 errno60。这与 packets=2/records=0相符，**但321未捕获第二包原文，不能把它声称为已观测的 seq0 LIST 回包，亦不能排除调度/其他拒绝原因**。上一节“inner0/outer3不要当作错误”只是缺固件证据时的约束，现有静态数据流支持发送侧修复，而非放宽接收侧。
- **发送侧窄修复**：源码提交 `4a19ace`。`als_notify()` 接收显式 sequence，不再内部独立取号；HOST_READY 仍取一个序号。LIST 在构造前由唯一 worker 取一次号，写入 TX memory.header.sequence，再据此计算 CRC32、复制完整12B到 notify.value，outer使用同一号并计算原CRC8。旧 LIST 的外层3保持3，只将两份内层0同步为3；CRC/magic、channel/command/sub-command、容量、type/增益、接收严格匹配、100ms期限、poll/W1C/重试策略及原有缓冲观测不变。不增加对 response 内层 sequence 的新要求（Android不校验，固件回复构造亦未见显式赋值）。不改 DT 校准、近远极性、ENABLE 或真实样本。
- **复查/派生产物**：工作树分析目录收尾时整体迁回主仓库 gitignored `x9/out/scp-list-static321/`，保留 `scp.bin`、`scp-dram.bin`、字符串/xref、`sequence-path.decoded.txt`、局部 disasm、`rv55_slice.py` 和公开 opcode参考；原件和派生物分开，均不提交固件/设备日志。原件提取可用 Python `b=Path(<上述原件>).read_bytes(); Path(<out>/scp.bin).write_bytes(b[0x200:0x200+0x426c38])`。复查 `r2 -n -q -a riscv -b 32 -e scr.color=false -c 'pd 30 @ 0x86ac8; pd 28 @ 0x88824; pd 14 @ 0x87788' x9/out/scp-list-static321/scp.bin`，注意前述错误解码边界；有限解码器（本机现有 Capstone5，仅分析依赖，不参与构建）`python3 x9/out/scp-list-static321/rv55_slice.py x9/out/scp-list-static321/scp.bin 0x88824 0x88864` 输出带来源标签的局部结果。
- **未完成/交接边界**：本轮只有静态分析与发送侧源码改动，**没有编译结果、新镜像、CI run ID、上机结果、测试套件、mailbox/ADB或分区操作**。尚不能称 LIST、真实 lux/near-far、自动亮度/遮挡熄屏已修好；没有本轮可交付的“修复后镜像”。下一步若用户要求功能镜像再正常编译/打包，不再制作或推荐纯探针；后续验证需真实 LIST type5/type8、ENABLE ACK及光照/遮挡DATA，不能以 ACK 或 HID打开代替验收。
- **合并/收尾里程碑**：`4a19ace`（源码）与 `3a78903`（静态交接）已合入 main `846ec20` 并 push 成功；合并保留同期 call 会话的 `c01aae6`，没有改它的代码。静态产物已实际迁回上述主仓库 `x9/out/scp-list-static321/`；本交接补记提交后正常合 main/push，清理本轮工作树/本地分支，不删除其他会话工作树、已有镜像或任何 `.local` 输入。

#### 2026-11-15 GPU 分支收尾 — 用户停止闪烁调查，仅合入优化

- **用户决定**：停止关机滑块文字闪烁调查，不再继续追因或制作诊断镜像；仅提取有用优化到 main，然后删除 `feat/gpu-wait` 及其 worktree。闪烁 root cause 仍未找到，不能把本轮合入说成闪烁修复。IA 格式重算、guarded VS no_psiz 残留 store 两个潜在/已确认静态缺陷均未修复，后者与闪烁的因果仍未证实。
- **范围/提交清点**：原 GPU HEAD `20c1019f487e153529e06a5f2d332bd1619719b9`，main 集成基线 `61d87af`。`git log --reverse main..feat/gpu-wait` 共 **30** 个提交：**6 个优化/行为改进 + 1 个配套安全修复**，其余 **7 个纯诊断/离线工具提交 + 16 个 NOTES 提交**不合入。使用临时集成分支 `feat/gpu-perf-merge` 按原顺序提取代码，不把落后于 main 的 GPU 整棵源码树覆盖回来。
- **保留的来源**：`3e2cd34`（fence 每次 poll acquire、100us 等待、GPU 完成唤醒 present、GUI 双缓冲、按下时 4ms 触摸）；`8f49c45` 的 UI boost 部分；`0d01209`（CPU 时钟探测保持中断开启，避免 308 的 interrupts-disabled panic）；`7539c74` 的 MT6993 1+3+4 拓扑及 steering 默认关闭修正；`ba95b18`（厂商 FDVFS per-CPU 请求/LUT v2）；`45157a7`（85–95°C 渐进 GPU 限频、CPU boost 90°C 关/85°C 以下恢复）；`d95d14d`（灭屏忽略触摸/交换触发的 boost、GPU 最低 OPP、FDVFS 低请求）。这些是已有实现的提取，不代表本轮取得新性能实测。
- **混合提交也做裁剪**：不带 `7539c74` 的纯 MAP/UNMAP 拒绝原因日志；不带 `8f49c45` 的全局 audio/radio/camera 默认表、mic 默认模式及相关打包器改动，保持 main 既有设备配置。GPU per-job timestamps 的默认开启 hunk 不带入，原有显式 `x9gt=1` 机制保留；默认 `x9g=61`（1|4|8|16|32），bit2 保留为空位，64/128 steering 仍默认关闭，`x9g=0` 可关闭这些性能项。低频 governor 自检/状态日志及其配套安全修复保留。
- **没有合入**：`565cbd1` IOSurface 探针、`379f609`/`48e753a` label readback、`bc50c6c` label-input capture、`673c82e`/`45ef53f`/`86d8fa9` 离线 PanVK/FAU 工具，以及这条分支的 16 个调查交接提交。`x9/metal/`、插件 allowlist、`boot-device.json` 均不改，main 仍是 **GPU280**；保留同期 call/SCP/sensor 功能，未合入 GPU281/282/283 诊断插件，也不更改 `.local` 构建树/链接点。
- **静态集成核对**：7 次原提交代码 patch 的 `git apply --index --3way` 全部干净应用；裁剪后代码仅 **7 个文件、694 insertions/108 deletions**，`git diff --cached --check` 通过。没有编译、测试套件、CI run、新 boot.img、mailbox、ADB/fastboot 或手机操作；优化组合在最新 main 上仍未重新构建/上机。本轮中断前的 guarded VS 32-case 有条件静态数据流结果留在下面的 ignored 分析目录，不是 GPU 执行或 root cause。
- **产物已经保留**：GPU worktree 原 `x9/out/` 的所有已有镜像、插件包、构建日志及 `label-static-20261115/` 整体迁回主 checkout 的 gitignored **`x9/out/`**，没有删除镜像/日志或改写旧绝对路径；同名 `dsc-20H392-textmap.json` 放到 `x9/out/gpu-wait-archive-20261115/`，未覆盖 main 原件。原 worktree local build config 和 15 个 ignored generated include 也归档于该目录。Apple 字节/设备输入均不入 git。
- **可恢复历史**：删除分支前已创建完整本地 `x9/out/gpu-wait-archive-20261115/history.bundle`，包含原 GPU 分支及全部调查提交；同目录保留 7 份来源 patch 与最终 `optimization-only.patch`。如以后确实要恢复，`git fetch x9/out/gpu-wait-archive-20261115/history.bundle refs/heads/feat/gpu-wait:refs/heads/archive/gpu-wait`。不自动继续闪烁调查或启动旧诊断镜像。
- **实际交付/清理完成**：优化集成提交 **`45d4df7`** 已 fast-forward 到 main；`git push origin main` 成功（`61d87af..45d4df7`）。随后 `git push origin --delete feat/gpu-wait` 成功，本地 GPU worktree、`feat/gpu-wait` 与临时 `feat/gpu-perf-merge` 均已删除。`git ls-remote --heads origin refs/heads/main refs/heads/feat/gpu-wait` 只返回 main `45d4df79a4792e7203221b3a2674b1b7dc891727`，GPU 远端分支已不存在；主工作树干净，其他活动分支/worktree 未动。本条是 NOTES-only 收尾补记，仍没有新增构建或设备结果。

#### 2026-11-15 双传感器完整链静态续修 — 功能源码/单文件编译通过，不出探针或新镜像

- **范围/当前结论**：用户要求“不要出探针镜像，多静态分析”；沿用 `fix/sensors-complete321` / `.worktrees/sensors-complete321`，起点 main `aa0f151`。继续核对 `~/321.txt:723–737/823`：READY、SHARE_MEMORY seq1、TIMESYNC seq2 已通过，但 LIST stage9 errno60、records0；没有新实机结果，两个物理传感器仍未验收。最新已有的非探针双传感器候选仍是 `x9/out/x9-proximity4-20261115/boot.img`（`b4aa2ef`，ALS3+prox3/GPU280），**不包含后来 `4a19ace` 的 LIST 序号修复，也不包含本轮修复**；旧 `x9-scp-list321-20261115` 探针不推荐。本轮没有新 boot.img。
- **代码里程碑**：`804b39b` 修复 TIMESYNC 月份、ALS cadence/coalesce、completion-first wait、样本错误隔离，并移除旧 LIST-only 原包/IRQ快照探针；`1fcf342` 补齐 TIMESYNC 同窗采样。`26b6701` 将同期 main `5e4ce29` 合进功能树，保留 call 初始化与 GPU 优化；同期 main 没有改本轮三份传感器编译单元或 ALS HID include。前一轮 `4a19ace` 的单次取号→TX memory/VALUE/outer统一序号仍保留，不新增 response.inner.sequence 校验。
- **TIMESYNC 明确协议差异**：Android `sensor/2.0/sensorhub/timesync.c:timesync_comm_with_nolock()` 写 `tm_mon+1`，旧实现把转换用的0-based month直接发出；改为 month+1（wire 1..12）。Android 在 `local_irq_save` 内采样4种时钟，旧实现可被调度切开 calendar/CNTVCT/host timestamp；现用 XNU `clock_get_calendar_absolute_and_microtime()` 得到配对 calendar/absolute ticks，在同一个短 IRQ-disabled 窗口读 CNTVCT，恢复原 IRQ 状态后转ns/编码。XNU `clock.c:582–641` 证明该API配对采样、使用clock spinlock且自行保存/恢复spl；`link-order.txt:50`已有clock.o。仍发送 UTC，不猜 Android `sys_tz` 或用户态时区；suspend/低功耗时钟域尚未闭合。
- **ALS iOS请求→真实ACK**：旧 `x9_als_set_report_interval_us()` 在未ENABLE或存在未应用cadence时返回EBUSY，丢掉初始化/在途ACK期间的合法请求；现保存/coalesce最新desired cadence（范围20ms..1s不变），worker为唯一控制owner。初始ENABLE的interval与LIGHT demand一同在lock下snapshot，消除global无锁读取。`status.report_interval_us`仍只在native matching ENABLE ACK后提交；HID poll采用applied值，setter成功不等于物理应用。`X9AmbientLight::start`显式初始化timer/ingress成员，失败/free路径不依赖分配器清零。
- **100ms等待语义**：control和LIST恢复调度后先poll completion，未收到且deadline已过才返回ETIMEDOUT，不再先看deadline而漏掉已pending的合法回包；不新增重发、延长后续等待、放宽CRC/seq/type/cmd或重置SCP。**mailbox没有arrival timestamp**，因此不能区分deadline前到达与线程被延迟期间到达的matching reply；这项修复是completion优先的等待语义，不是硬100ms到达时间证明。321的329ms调度间隔仅支持这一静态缺陷，不能声称已观察到deadline前pending ACK。
- **每sensor拒收≠共享transport故障**：静态链 `prox expire_locked()` 设置本地ETIMEDOUT/清pending → `x9_prox_native_data()` 返回state.error → 旧 `als_drain()`把非白名单错误传回worker → `als_fault()`清LIGHT/control，证实本地type8 quarantine可以杀有效ALS。现完整复制的36B DATA只走相应sensor admission/decoder及拒收计数，不据其错误终止共享owner；已停用/缺席/隔离sensor的旧DATA照常消费并归还rp。`als_poll`的WDT/重复READY/坏notify位置、FIFO头/rp结构检查及 `als_control`真实发送/ACK错误仍传播并终止共享链，**没有吞掉真实控制超时**。近远极性、gain、DATA格式、on-change有效期和HID receiver mask均未改。
- **继续静态查到的校准入口/边界**：Android `hf_sensor_io.h` 的 `HF_MANAGER_SENSOR_CONFIG_CALI` 经 `hf_manager.c:1078` 和 `transceiver_config()` 发送 `SENS_COMM_CTRL_CONFIG_CMD=4`；type8/95/116的config缓存特殊预留48B，**这不是这些传感器的确定wire长度或结构定义**。`transceiver_custom_data()` 另走common share-buffer `CUSTOM_DATA_CMD=2`，custom_cmd v2走channel1；现有代码没有还原设备专用config/custom载荷。live DT明确TCS3720、need_gain_cali、panel gold/decision table、PS供电/offset参数，但不能把DT直接拼成CONFIG或套用24081 TCS3760寄存器。备份SCP tail含TCS3720 probe、PS/CCT calibration和寄存器配置字符串；新 `tcs-*-candidates/pointers` 只记录未验证LUI/ADDI候选（可能跨branch/寄存器覆盖），不当作字段或可达调用证明。raw payload字符串offset比旧分区级 `scp-strings.txt` 小0x200；部分tail字符串出现候选VA bias `-0x2a2000`，**尚未证明bank/overlay映射，更不能推导实际当前SCP地址**。下一静态动作是按完整指令边界验证真实driver registration/CONFIG dispatcher及载荷长度，再定位实机校准原始输入，不能以字符串替代数据流。
- **编译/产物**：主checkout `python3 x9/build/prepare-local.py --check`通过；`gh run list --limit 3`返回空，无新CI run。仅使用正常Builder模板 `compile_one` 顺序交叉编译 `sweet_x9_proximity.o`、`sweet_x9_als.o`、`SweetHID.cpo`，全部exit0，分别13/27/15 warnings；同窗采样改动后仅重新编译改动的ALS对象exit0/27 warnings。日志/depfiles/objects为 `x9/out/sensors-complete321-compile/{single-compile.log,timesync-compile.log,build.log,kernel-objects/}`；有限静态结果为 `x9/out/sensors-complete321-static/`，收尾迁回主checkout对应gitignored路径。复编可从 `x9/build/build.py` 导入Builder/load_config并按上述object name调用compile_one，原命令完整保留于build.log。**未完整kernel link或boot pack**，未构建/替换固件、插件或XNU对象树，未跑测试脚本/套件或产物二次检查，没有ADB/mailbox/fastboot/设备/分区操作；源码编译绿色不代表真实LIST、lux、near/far或自动亮度/遮挡熄屏成功，完整目标仍未完成。
- **实际合入/收尾**：源码 `804b39b`、`1fcf342` 与交接 `c7bd2c0` 已 fast-forward 合到 main，`git push origin main`成功（`5e4ce29..c7bd2c0`），保留同期GPU/call代码。上列两个ignored编译/静态目录已实际迁回主checkout `x9/out/`；仅删除本轮 `.worktrees/sensors-complete321` 和本地 `fix/sensors-complete321`（未推送该远端分支），其他audio/Bluetooth/call/HEVC工作树未动。此条为NOTES-only最终补记，仍无新镜像或物理验收。

#### 2026-11-15 相机 native source 配置串接（并行分支）

- `feat/camera-source` / `.worktrees/camera-source`，源码 **`1cc799d`**：新增 `x9_camera_native_source.inc/.h`，供同TU的native configure在已持有control mutex、sensor已verified preview且capture/stream尚未attempted时调用 `cam_native_source_prepare_locked(epoch, &status)`。实际串接原 receiver→SMI clocks/A0/shared→MAC→analog→双fresh CSI fmeter/timing→rate admission→CTLE→OUTMUX/CAMSV dormant IRQ worker；每段之后重新读取同sensor的0100/0104及供电guard，不重复sensor表、不开PHY source/stream/DMA、不注册空engine或发布READY。分段首错保留原journal交唯一native power-close，没有新诊断ioctl/镜像。
- rate依据是当前私有live DT `cdphy-dvfs-step0..3` 与OPPO MT6993父表：parent9/12/14/15分别312/364/416/499MHz（不是源码旧comment356）。两份fresh计数各须匹配本次实际parent的nominal±2 counter ticks，并保持原MM时钟journal；主/前preview均落step0范围。status的 `required_vcore_uv=575/600/650/725mV` **只是要求，不是实测/申请成功**。没有据meter成功造持续PLL/VCORE/DVFS/MMQoS/SMI DMA grant；原缺口仍保留。
- 单文件编译 **exit0 / 106 warnings**：正常Builder `bsd-c-4` 模板编译ignored wrapper（原camera TU后include新文件，唯一新增unused caller warning待主线程实际接线）；原命令、对象/depfile、日志在 `x9/out/camera-source-compile/{single-compile.log,build.log,camera-native-source.c,kernel-objects/}`。没有测试套件、完整link/boot、新CI run ID、硬件/MMIO/SMC实跑、mailbox或分区操作。当前尚无实际native capture/host注册、SMMU attach/完整DMA退休与repeat，也未得到iOS物理预览结果；主线程继续集成caller。
- 静态rate下一步：OPPO `mtk-dvfsrc.c` 的MT6993 ops以AP SW_REQ `+18[6:4]` 请求VCORE，并以CURRENT_GEAR `+6ac[10:8]`验收；`mt6993_data.mem_res_req_en=true`，请求等待前后调用VCOREFS `MEM_RS_REQ/REL`。`mtk-dvfsrc-regulator.c` 最低selector0对应575000µV；DT最小575000/always-on只是配置，不证明本次EL1 firmware启用状态或实际电压。不能跳过firmware gate直接RMW、以selector/聚合gear冒充实测供电或持续lease。

#### 2026-11-15 相机 source rate 供应者续接 — 复用已运行的UFS单调VCORE floor

- 新证据更正上一段的“VCORE供应者缺失”：`~/321.txt:1286` 有实际VCOREFS INIT a0=0；`:1291`有 UFS AP SW_REQ `00009000→00009020`、current selector5与`floor_held=1`。main现有 `sweet_x9_vcore_ufs_check()` 在同DVFSRC mutex内检查已初始化控制器、保留的单调floor、AP selector与actualgear，boot生命周期内无降压/释放API。**不把先前DT-min/always-on推断替代这条运行证据**。
- 源码 **`23b129b`**：native source rate复用这个只读真实供应者，并借本次MM父表的MAINPLL2 `0x10043000` 或UNIVPLL2 `0x10043100`，要求FENC bit4及PCW/postdiv原值不变、两份freshCSI计数匹配该实际PLL分频；所有映射按两PLL固定缓存，不每次冷open重新映射。step3是正常UNIVPLL2 2496MHz/5=**499.2MHz**，DT整MHz499不能直接当±2ticks目标。新增 `cam_native_source_rate_guard_locked(epoch)` 给native host/check调用（原receiver/MM持有、thread context，不能IRQ spin内调用），保留AP VCORE要求与currentgear校验；不写DVFS/PLL、不重新SMC、不以高gear代替持续APfloor，不称PMIC实测电压。
- 修订代码最后单文件编译 **exit0 / 106 warnings**，日志 `x9/out/camera-source-compile/rate-final-compile.log`（先前`rate-compile.log`108 warnings是修正两个新warning前的中间编译）。没有测试套件、完整link/boot、CI run或设备操作。仍不更改继承的SENINF parent（若26MHz/未知值则真实ERANGE），也没有持续CAMSV processing clock、CAM AL/VCP MMDVFS或SMI/MMQoS带宽供应者；不能据CSI/UFS floor就给全链rate/SMI DMA grant。相机native capture caller、重复RAW请求/完整退休及真实iOS预览继续由主线程集成。

#### 2026-11-15 native SENINF 实际父时钟 take/check/restore（source并行树）

- 在 `feat/camera-source` 新增 `x9_camera_native_clock.inc/.h` 并接入 `x9_camera_mm.inc`：native config已验证sensor standby后请求唯一当前epoch，MM在receive实际引用与CAMSV sibling admission后、**任何MM HWV slot及PHY meter前**，借正常锁定UNIVPLL2和已有UFS单调floor将单SENINF0/3父改为9（312MHz）；普通mode3诊断完全不改父。每次修改均检查实际secure token、十XPU mux EN、aggregate/STA、CKSTA idle与port FENC off；只对选中低nibble CLR/SET及UPDATE17/20，保留PDN/reserved/其它端口，未写共享PLL、AL表、电压或VCP状态。
- 独立原/预期/未决写/UPDATE journal：确认后更新MM cfg[2]并维持PCW/FENC/floor guard；unknown值或已确认值回退不修复、不重放UPDATE。唯一原MM close在SENINF slot8 unvote后、CAMTM/底层domain仍持有时恢复原父；partial take在slot8尚未seen时同样恢复。新foreign vote或守卫失败保留原资源，不强制下电；status.retained的slot8同时覆盖父journal。没有把cold mux检查称固件lease/ABA保护。
- 正常Builder `bsd-c-4` 单文件交编 **exit0/106 warnings**，原命令/对象/depfile在 `x9/out/camera-source-compile/{clock-compile.log,build.log,kernel-objects/}`；没有测试脚本/套件、完整link/boot、CI run ID、新镜像、设备MMIO/SMC运行或分区操作。静态实现并不证明设备真的已改父/312MHz或物理预览；native_source双fresh meter仍是本次硬件验收。下一动作继续实现CAM处理频率和MMQoS供应者。
- CAM供应者重要纠正：vendor kleaf `mgk_64.bzl:1494/1496` 对mt6993实际选 `mmdvfs-mt6993` + `mtk-mmdvfs-v5`，不是旧v3。它虽沿用live DT `mediatek,mtk-mmdvfs-mux`兼容名，CAM `clk_set_rate(user-cam)` 实际进入 `mmdvfs_mt6993_dfs_vote_by_xpu()`：VMMRC `0x31d00000`、user CAM是XPU0、pos0、CLR/SET/UPDATE `+110c/+1110/+1114`，不是VCP IPI或generic MUX_PWR AP SET。这仍是源码/构建配置证据，尚无当前XNU运行的VMMRC take/frequency验收；继续定位实际初始化/level与MMQoS路径，不据旧v3另启动VCP。

#### 2026-11-15 相机主集成续接 — 已导出 normal map 的 prepare 失败不得撤底层资源

- 主集成树 `feat/camera-integration` / `.worktrees/camera-integration` 已到 `6a5f52a`（native SENINF parent9 take/restore）；当前增加 mandatory engine `retirement_guard`，sensor-session/receiver/MM 的 close 在改变 close journal 或撤供电前询问实际 native owner。MM-only close 用 epoch0 查询当前 owner，不引用尚未定义的 sensor 对象。即使 prepare 在 core 记录 capture=true 前失败，已安装/导出的 normal translation 仍必须由实际 controller 证明退休，不能靠清 capture 标志/close成功假设释放。
- 完整 **kernel-only** 构建固定源码 `fedb847`：`python3 -I x9/build/build.py kernel --boot-config x9/build/boot-device.json --name x9-camera-integration-20261115-kernel` **exit0，98/98 overlay，3 prebuilt，link/strip成功**。日志与内核在本树 ignored `x9/out/x9-camera-integration-20261115-kernel/{full-build.log,build.log,kernel.development.sweet}`；不含后来 `6a5f52a` 或本节 guard/未挑的 repeat。没有 boot pack、产物二次解包/hash/反汇编、测试套件或设备操作。`gh run list --limit 3`此前返回空，无CI run ID；SDK torch/camera 两工具正常编译成功，不代表 iOS native vendor/真实帧已执行。
- 并行代码待集成：native repeat `9561287`、first capture `9a73463`、CAM MMDVFS v5 RC/fresh processing meter `bdac8f6`。capture admit仍明确ENOTSUP、getter不注册；主线程接真实frame-bound AE，capture代理接下一请求循环，native代理补独立late DMA_DONE ledger及完整producer/map退休，source代理续接MMQoS。当前没有新的真实RAW、相机READY或iOS连续预览证据，goal仍active；不出过程探针、不制造帧、不自行boot/flash。
#### 2026-11-15 CAM AL 真实VMMRC频率供应者与双fresh processing meter（source并行树）

- 新增 `x9_camera_native_rate.inc`，native-only在实际MRAW domain take后、MM CAM AL slot6消费前，走mt6993 vendor v5 `user-cam` 对VMMRC XPU0/pos0请求 **level2=416MHz**。不对CAM CLK_CFG直接SET、不伪造generic MUX_PWR enable、不启动VCP/重写RC initialization、PMIC或AVS/AL map。要求CAM当前cold parent0、AP PRE/vote/target/current0且其它12XPU CAM vote0；这只是冷基线准入，不据零值称已初始化成功。单field CLR/SET/UPDATE保持本XPU其它mux和foreign字段，posted request已提前记账，原UPDATE只poll/readback、不重发。请求后必须见真实RO AP vote/target/current2、RC current level至少2且有效、actual CAM parent10/CKSTA；PCW/FENC与UFS单调floor持续guard。RC level/电压ACK仍不是PMIC实测电压。
- 唯一MM close在CAM AL slot6退休后、下层domain还持有时恢复本次原RC vote0与original parent，partial take也由同receipt处理；未消费UPDATE、unknown PRE/parent、外部CAM新owner或缺失供应者保持MM/RC/原sensor资源。status.retained的slot6覆盖未见HWV slot但已发RC request；不盲降VMM/不清全bank/不修复shared owner。此实现要求冷baseline，**不是全系统VMMRC初始化器或跨XPU租约**，当前LK是否已经配置RC map仍待首次真实执行的RO/target/current与meter结果决定。
- 对 `x9_camera_phy_clock.inc` 的唯一meter receipt做小范围slot/id参数化：原CSI18/21逻辑和count[2]不变，native source在CSI rate admission后用同exclusive journal测 **CAM CKGEN_CK2 ID14/FENC18** 两次，要求各fresh BUSY→DONE、parent10/normal UNIVPLL2÷6≈416MHz与±2counter ticks匹配，保存 `cam_clock_count[2]/cam_clock_hz/cam_parent/cam_rc_level`。未决CAM meter仍属于原PHY retirement guard，没有另开一个可相互覆盖的meter owner；native `cam_native_source_rate_guard_locked()` 同时验证真实CAM供应者及FENC，不能只据CSI grant capture。
- 单文件正常交编 **exit0/106 warnings**，最终日志 `x9/out/camera-source-compile/processing-final-compile.log`（前一编译为`processing-compile.log`）；没有测试套件、完整link/boot、CI run或设备操作、新镜像。source/编译成功不等于实际VMMRC变更、fresh416MHz或iOS预览；完整host/DMA/SID/RAW/退休由主线程和native代理继续。下一动作真实MMQoS/SMI bandwidth供应者，vendor `apply_sv_qos()`不只ICC，还调用CAM BWR channel/total QoS与CAMSV FIFO thresholds；不能把RC频率作为bandwidth租约。

#### 2026-11-15 native 帧边界AE实际接线（主集成）

- 已挑代码 `9561287→7035323`、`9a73463→97a87ab`、`bdac8f6→fcba9f8`，保留 `4ab2e0e` 的export-retirement guard。新增 `x9_camera_native_ae.inc/.h`，TU实际按IRQ→AE→native capture→native sensor顺序编入。seed对照当前真正VERIFIED config、两份fresh AP实际0100/0104与0340/0202/0204读回，不以expected table初始化applied。queue只coalesce/取号，不改旧immutable request元数据。
- next要求control+IRQ mutex、无spin，重复校验actual completed+worker/RAW retired+PARKED双bank零credit边界；固定owned AP helper只准0104及六个曝光/帧长/gain字节，generic table writer仍禁0104。hold/write/release均在可能posted START前记账；任一不确定错误保留首错及partial group，不盲retry/release。release后两份实际controls匹配，继续观察真实0005至少三次forward变化（拒OFF ff/倒退/半wrap歧义），再次读回和boundary通过后才提交applied供next request。两秒有界等待全程不持IRQ spin，不用估算SOF、不把settling packets发布RAW。OPPO两sensor实际声明ae_effective_frame2/frame_time_delay_frame3；这是保守帧间排空，不是硬件实测AE验收。
- 正常Builder `compile_one(sweet_x9_camera.o)` **True/exit0，122 warnings**（包括尚未调用AE next的unused warning）；日志/objects/depfile本树ignored `x9/out/camera-integration-ae-compile/{single-compile.log,build.log,kernel-objects/}`。无测试套件、full link/boot、CI run或设备操作/产物二次检查。capture admission仍ENOTSUP、不注册，专用next controller及full退休日志尚待代理完成，iOS连续真实预览仍未完成。
#### 2026-11-15 原生A0静态最坏MMQoS带宽供应者（source并行树）

- **实际输入而非旧generic VMMRC猜测**：当前live DT `mmqos-state=0x13f42d` 是MMPC_V2(bit17)+MMUP(bit20)，没有VMMRC_ENABLE(bit8)。MMPC在 `0x31ae0000`；AP `+ff0` 是24个独立word、mask `+10b0`；L19/P0链接common0/port5/channel1，SRT-write index5、HRT-write index7。vendor MMPC握手临时mask0会影响所有AP预算，因此只允许**整个24word AP表冷零**（mask原0或0x0fffffff），保持其它22word且guard拒绝任何新APwriter；绝不在继承GPU/display/AP带宽下临时清全APmask。硬件/BWR/其它subsystem预算不写。
- 新 `x9_camera_native_bw.inc` 接入source的rate stage，实读传感器0342/0343 LL=back9696/front3768、0340/0341 FL>=7546/7642，依已verified preview PLL vendor pclk2196000000/864000000及 `cam_pipeline_plan` 的actual RAW10 stride/bytes计算最坏bound。AE可延长FL不低于该minimum；用minimum FL保留最大average，peak是stride*pclk/LL，不用人为30fps也不把整数line_ns当测量。静态计算back average472078369/peak1159603961 B/s、front300861888/939210192 B/s；10/7margin再16MB/s向上取整，AP units为back43/104、front27/84。当前LK是否配置MMPC执行表仍待真实request/current运行证据，零值只作准入、不冒充供应者成功。
- AP两word按原/预期/pending独立journal写，必须实际 **TOTAL_CURRENT `+d8c+4*index >= floor`** 才验收；这是最终MMPC budget面，不只中间APwrite。随后L19/P0按MT6993 bw_ratio9、w-hrt-ostdl=OSTDL_2(DT3)设置peak bound OSTD=back5/front4，保留其它port，并将真实HRT permission的DISABLE_ULTRA bit0/NONSEC bit3清0。A0原snapshot与唯一原close继续恢复default及entry；source rate guard同时做BW-current和actual A0 readback。静态最坏APfloor替代这一路动态CAM BWR上报，不启动或覆写共享BWR、不开CQ/STG/其它图像端口、FIFO/IRQ/DMA；每项预算/权限成功仍不是SID/DMA租约或PMIC/DRAMclock测量。
- 退休接回**唯一原SMI close**：原retirement guard先确认未决MMPC目标，不重发write；实际producer/port inactive与已有native DMA退休约束通过后，先还原本次AP表/mask，再还原A0/default和SMI shared/clocks。未消费目标、unknown值、new APwriter或budget丢失保存原SMI/MM/sensor依赖。主线程 `4ab2e0e` 的exported native DMA pre-close guard必须保留；本提交不修改MM close入口，不覆盖该并行修复。
- **供应者close不是DMA grant**：SMI port idle只准许预算面撤回，完整DMA/SMMU退休仍由上游mandatory engine实际retire-stage scope守卫授权；普通live capture的`cam_native_source_rate_guard_locked()`继续要求BW held+complete，不能借close路径放宽。成功AP表/mask恢复后BW receipt清held，`cam_native_bw_guard()`返回0，允许唯一SMI close继续原A0/shared/clocks恢复；顺序必须是AP budget→A0，不提前撤去consumer/底层供应者。
- 正常Builder `bsd-c-4` wrapper交编 **exit0/106 warnings**，最终日志 `x9/out/camera-source-compile/bandwidth-final-compile.log`（前次`bandwidth-compile.log`）；不运行测试套件，不完整link/boot，不出新镜像，没有CI run ID、设备操作或分区写入。当前仍无物理RAW/iOS预览证据，主线程集成后需以本次真实SENINF/RC/CSI+CAM meter/MMPCcurrent/A0、host映射与连续RAW completion/完整退休为验收，不能从这些静态提交宣布相机已可预览。

#### 2026-11-15 相机continuous/MMPC/full-stream retirement联合编译里程碑

- 主集成已挑 `de196de→41c3e34`（MMPC预算+A0HRT/OSTD）、`0f565c5→b7955d0`（专用continuous控制线程）及 `2ee4ce4→4fe3f39`（实际streamed producer/normal bank退休）；另 `182c2dc`修正AE counter仅用fullbyte forward差，不推定modulo255。core STOP在清STREAMING/递增generation后、source-off前实际调用short `cam_native_capture_stop_locked(epoch)`；即使off失败不再排下一请求，不拿此标记作硬件drain。
- 控制线程只响应actual completed→RAW_IDLE→workerbusy清零的PARKED边界；control+IRQ下AE/旧authority后two-phase publish替换下一immutable request，再由真实no-credit SOF IAR发布RCNT。新late CHANNEL DMA_DONE只绑定已实际DONE/FBC/CPU退休的前一credit独立单次receipt，不生成新frame；channel-only IAR不发RCNT。
- 独立fullstream退役代码核实际AP OFF/hold0/0005ff、旧spentFBC/RAW_IDLE、worker/GIC、DMAstopACK/restore receipt、A0 external/internal monitors+LARB STAT、COMM33 OSTD/idle，再SID80 BBM/targeted CFGI/CD/ASID TLBI+实际SYNC，恢复SID80原abort/bypass policy、CR0/ACK0、自己已消费完的disabled queue cursors及完整normal priors。所有backing仍pin，普通DMAcheck在retireattempt后拒绝，retirement_check只有normal完整恢复才准oldteardown scope；不把零寄存器/IRQclose/SYNC单独当全链drain。
- 正常联合Builder camera TU **exit0/127warnings**（continuous/BW/corehook，`x9/out/camera-integration-continuous-compile/single-compile.log`），加入retire后 **exit0/135warnings**（`x9/out/camera-integration-retire-compile/{single-compile.log,build.log,kernel-objects/}`）。`gh run list --limit 3 --json databaseId,status,conclusion,headSha`返回空，无CI run ID；没有full link/boot/测试套件、构建后复检或设备操作。远端已push到686781b，随后源码本次继续push。
- **尚未闭合/继续任务**：capture.off仍ENOTSUP，尚未串接实际retire-stage授权到supplierclose；normal未issue/prepareerror退休及front-switch新namespace/slot复用待补。静态发现第二req ADDR/FH/DB写被旧cam_sv_post要求creditretired+closeattempted拒绝，native代理正加真实PREPARING专用权限，不能泛开EN/RCNT。另minor0提前open可抢走opened阻断异步注册，source代理做mode2注册完成前拒open，不影响minor1status/mode3。没有真实RAW/iOS预览，新provider仍不注册、admit保持明确未完成。
- **streamed producer/SMMU实际退休源码（2026-11-15）**：`2ee4ce4`新增 `x9_camera_native_retire.inc`，`cam_native_retire_locked(epoch,smmu)`由实际 `cam_irq_stop_locked`结束之后调用（不重复stop；caller持control，内部IRQ）；`cam_native_retire_irq_locked`供两锁已持有者。旧request独立DMA authority只在BBM前admission调用；之后pure阶段guard复读真实sensor0100/0104/0005、source/gate/receiver journal、spentFBC两bank、RAW_IDLE/slot RETIRED、worker/GIC、DMA halt ACK+restore receipt、L19P0外/内outstanding+LARBSTAT，以及COMM33 DEBUG_S OSTD mask0x7ffe000和DEBUG_MISC bit0实际idle，没有循环旧translated d.check或由callerbool授予drain。SID80实际BBM→CFGI/CD/SYNC→ASID1TLBI/SYNC→prior abort/bypass STE/SYNC；保存17word/3base coldnormal完整快照，实际CR0/ACK0后才restore自己已真实消费且disabled的CMDQ PROD/CONS0、bases/CFG/CR1/2，GBPA/secure/其他SID政策不变，两份actual完整prior回读才记restored。所有exportedbacking继续pin；`x9_camera_native_smmu_retirement_check`只给完整restore之后的oldrequest teardown scope，绝不是新DEVICE/credit/DMA grant。
- **STOP queued history与编译**：repeat STOP先真实等待已issued request的worker→CPU RETIRED/PARKED，再暂enable自有GIC消费masked no-credit SOF/CHANNEL的actual IAR/EOI，源已OFF，随后真实PENDING/ACTIVE空与maskedidle；不ICPENDR/ICACTIVER或blindR-C。READY未issue取消、partial startup/unstarted路径仍不属于streamed OFF/DONE判据。正常Builder单文件camera.o最终exit0/**130warnings**，日志 `x9/out/native-smmu-compile/retire-fixed-compile.log`；首版枚举wordcount/cache符号5编译错误已修，失败原日志 `retire-compile.log`保留。未完整link/boot、测试套件/二次产物检查、CI或设备操作，无真实DMA/RAW/iOS预览结果；本条源码分阶段闭合不是硬件全drain验收。

#### 相机 native 最窄 unstarted 失败收尾与 repeat SVpost（2026-11-15，子代理续交接）

- `5380447` 修复 continuous 第二 request 的 SVpost：只在 PREPARING、旧真实 DONE/FBC/CPU boundary、新真实 ARMED/RAW_DEVICE 下允许同 IOVA/gen/sensor 的 ADDR_LO/HI/FH 与普通 DB_EN down/up；不清旧 ACCEPTED/close 历史，不开放 FBC/credit/DMA/VF/CMOS/CQ。单文件正常编译 exit0/132warnings，`x9/out/native-smmu-compile/repeat-post-scope-compile.log`。
- 本次增加 `cam_native_unstarted_retire_locked(epoch,m)`（调用方已持 control）及 public wrapper。只 export/cache 失败且第一 normal setup store 未 issued、RAW 仍 PRIVATE、没有 request/credit/source 才可；native_source_prepare 已创建 dormant IRQ worker 时先核 exact epoch/EMPTY slot/no issued，走真实 inactive `cam_irq_close_locked`，不要求不可达的初始 !prepared。随后真实 AP0100/0104/0005 OFF/hold0/FF 与两完整 prior bank 快照，保留原错误与全部 pin backing。
- 该路径不是 streamed OFF/DONE，不覆盖 partial normal startup、CMDQ timeout、READY DEVICE 未 RCNT、已 started 的 arm 前错误。`retirement_check` 对该证明独立核 namespace/prior，不能因原 private cache 错误而伪造正常 DMA 身份，也不抹原 errno。
- 正常 Builder `compile_one(sweet_x9_camera.o)` exit0/**134warnings**，日志 `x9/out/native-smmu-compile/unstarted-dormant-compile.log`；前一版相同134warnings日志 `unstarted-retire-compile.log` 保留。没有完整link/boot、CI run、测试脚本/套件、二次产物检查或设备操作；真实帧/iOS预览尚未验收。下一步真实 physical-close 后 epoch recycle 与 fresh MM reopen，旧 backing/receipt 继续保留，不假清 namespace。

#### 2026-11-15 相机最后生命周期集成进行中（主线程接续）

- 主集成 `1d052c6` 已包含真实 streamed capture.off 与 scoped old-request teardown，`71aaaad` 是第二请求 PREPARING ADDR/FH/DB 的窄权限；不再沿旧必定 EBUSY 路径重跑。新增已挑 `289aa7e→a271622`（never-exposed export 错误先真实关闭同 epoch dormant IRQ，再核 AP OFF/hold0/FF、RAW_PRIVATE/no setup issued 与完整原normal bank）及 `f4ced38→a00e075`（capture guard实际调用该proof、运行时EL/16KiB/chosen/GIC admission preflight）。原错误/backing保留；partial setup、queue timeout、issued credit不能用unstarted清理。
- 联合正常camera TU复编（启动于1d052c6阶段）**exit0/137warnings**，日志本树 `x9/out/camera-integration-lifecycle-compile/{single-compile.log,build.log,kernel-objects/}`；随后新增unstarted代码尚待最终联合复编，不把代理自树编译当作主树结果。`gh run list --limit 3 --json databaseId,status,conclusion,headSha`为空，无CI run ID；无full boot/测试套件/构建后二次检查/设备操作。
- 当前未提交接线：native_sensor仅在原sensor→RX/SV/OUT/PHY/MAC/SMI/MM真正physicalclose成功后调用capture recycle wrapper；mode2 init一次普通kernel thread真实engine admission及两sensor power/ID/close，失败不retry，不伪造READY/RAW。capture/native代理正在完成旧epoch receipt/archive与fresh-MM namespace/RET-slot reuse，admit仍ENOTSUP等待闭合，未将boot默认mode1改2。
- 注册时序已纠正：admit只runtimeEL/页粒度/此bootIRQ拓扑及无owner，不读未持MM票的CAMSV/SMMU；注册只power/ID/close，不configure/MM，故不等待UFS floor，不新增跨UFS/GPU的timer或fake事件。configure/prepare仍必须实际UFS单调floor与CSI/CAM/MMPC/MM token通过。最新运行证据仍`~/321.txt:487 mode=1 platform=0`，真实RAW/iOS连续预览目标仍active，不能由源码/编译/ID注册宣布完成。
#### 相机 native full-close epoch recycle / fresh MM reopen（2026-11-15，子代理实现待父集成）

- 新 `x9_camera_native_epoch.inc`（从 native_retire 尾 include）提供 control-held `cam_native_epoch_recycle_locked(old_epoch,oldm)`：只在 sensor 真正 physical-close success 的 EMPTY/epoch0/session_activefalse、RX/MMclosed、全部 SV/MAC/PHY/OUT/SMI/A0/shared supplier heldfalse、真实 prior SMMU restored receipt、RAW_IDLE+oldslotRET（或最窄 unstarted PRIVATE+EMPTY）与 GIC enable/pending/active空后，归档完整旧 IRQ/FBC/repeat/source/streamclose/DMAstop/SV/gate/sensor/RX/MM/suppliers journals。此时旧MMtoken已释放，绝不再 validate 该 token，也不读已关电的 camera/SMMU CSR。完整 restore/close 的实际硬件观察仍保留在 receipt，不把 EMPTY flag 当新 DMA grant。
- 只有上述真实收尾成功后重建旧 per-epoch IRQ/FBC/repeat/source/streamclose/DMAstop；保留永久 thread/maps、monotonic IRQ last_epoch、两个 slot（所选仍RET）、全部原 backing。`cam_native_epoch_recycled_check_locked(old_epoch,oldm)`只给匹配 heap history 的旧收尾历史scope，不能 arm/cache/MMIO；capture wrapper须同步保存完整 immutable ctx/request/view及单调 capture/sequence history。
- 新 `cam_native_reopen_locked(new_epoch,oldm,fresh_MM_token,iova,bytes,&newm,&view)`只 exact new dormant prepared IRQ/noowns/credit/source/RAWarm、真实AP0100/0104/0005 OFF/hold0/FF两样本与**fresh MM票**下完整 old prior normal bank两快照后允许 namespace replacement。旧 m 通过 newm.native_previous 继续 pin；新未export allocation失败会恢复 old namespace，不假NULL。首次 normal start/普通 `x9_camera_smmu_prepare`仍不能替换已有 namespace。
- 所选旧RETslot必须按其实际 backing找到较早完整 retired/recycled receipt（支持 back→front→back）；核完整slot snapshot/RAW_IDLE/rawmatch后归档 tombstone并真正重建软件 EMPTY，未变old RAW ownership也未释放旧页，不把 old RET 单独当权限。不是每次 STOP清slot；只有全physicalclose+freshcold reopen才允许。新启动 export/cache失败仍保留新旧 pin backing与原 errno。
- **有限约束**：最多 **8 exported epoch lifetimes** 留存 pin；第9次 reopen在 namespace/slot mutation、allocation/export或controller store之前 ENOSPC，供新epoch noexport普通物理powerunwind。不是无限稳定循环/内存回收实现；partial startup、CMDQ timeout、READY DEVICE未RCNT仍terminal retained，不宣称它们可clean关闭。
- 普通 Builder camera.o 最终 **exit0/139warnings**，日志 `x9/out/native-smmu-compile/epoch-reopen-apoff-compile.log`。首版完整receipt超过kalloc_type 16KiB产生2错误，已改 kernel_map wired整页receipt（失败日志 `epoch-reopen-compile.log`保留；中间正常 `epoch-reopen-fixed-compile.log`）；没有测试脚本/套件、完整link/boot、CI run、二次产物检查或设备操作。当前仅代码/普通编译证明；没有真实SOF/DONE/RAW帧、fullclose/reopen运行结果或iOS真实预览验收。

#### 相机 unstarted gate sentinel 可达性补修（2026-11-15）

- 窄源码审阅发现 `cam_sv_gate_init()` 的 `pending=X9_CAM_SV_GATE_STEPS`（8）才表示没有 pending，上一 unstarted guard误用 `cam_sv_gate.pending` 布尔值将合法 dormant失败close全部拒绝。现只要求 `pending==STEPS` 且 `issued==0`；不允许任何 gate-issued/RAWDEVICE/credit/source/setup 放宽。
- 普通 Builder camera.o **exit0/139warnings**，`x9/out/native-smmu-compile/unstarted-gate-sentinel-compile.log`；未运行测试套件、二次产物检查、完整link/boot、CI或设备操作。补修仅依据实际初始化与守卫数据流，不是已观察到设备 unstartedclose 成功。
- 重复 close/phase 审阅未新增递归：BBM前一次 oldDMA admission，BBM后 producer guard无DMAcallback；oldretirementcheck只在实际 MM票尚持时供 supplier teardown，epochrecycle 在物理close后不读gatedCSR。**READY DEVICE未RCNT的STOP仍ENOTSUP/retain**，continuous控制器可能经常处于此窗口，所以本轮fullclose/reopen接口不能当任意时刻可靠相机关闭/前后切换验收；独立never-issued取消receipt尚未实现，不得以READY→PARKED或假DONE代替。
- **确定输入条件**：集成树`d5d05b02`的tracked `x9/build/boot-device.json:52`仍`x9_camera=1`；321也是mode1，不触native注册、PID1安装或camera exec注入。真实路径须mode2，PID1在native UFS可写根上通过`x9_camera_install.inc`安装Builder当次生成的hash路径，精确`/usr/sbin/mediaserverd`exec才注入；camera不是独立Metal插件、不需camera mailbox。既有torch mode3 helper把camera组合在同一个DYLD key中，其他caller-owned DYLD链不覆盖。旧21:25集成构建只证明当时桥编译，不能代替最终native集成重建；rootclone缺camera库也不能证明设备当前根盘部署。
- **READY→RAW→真实handler边界**：`camera_status.h`以只读minor1查询，要求status version1/mode2/READY且无FAULT/RETAINED、supported_mask精确6与两真实chipID；`media_bridge.m`仅mediaserverd、精确20H392 CMCapture UUID与vendor原IMP/caller gates。设备create再检READY；`camera_device.c:2022+`真实open minor0、完整HAKIOS_SENSOR write后起worker，读取并严格验120B `RWFRAME2`+实际RAW10/sequence/controls/SOF时间，实际graph池取PB、ISP渲染、metadata附着和最终短锁admission后`:1463`调用PixelBufferHandler；没有合成帧或提前READY。最终handler回调不是屏幕验收。
- **不能由旧comments宣称graph不Start**：20H392原件`.local/apple/dsc-20H392/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture`证明`-[BWMultiStreamCameraSourceNode start:] 0x1a1f38514`在`0x1a1f387ac`真实`_setStreamOutputsEnabled`；仅该调用成功、stream.streaming=false且suspended=false时，`0x1a1f387d4`发objc `start`。`BWFigVideoCaptureStream start 0x1a1fd9584`加载self+8的wrapper并tailcall start；`BWFigCaptureStream start 0x1a1f44168`在`44254`取真实vtable，`44258`取+0x10 derived、`4425c`取derived+8、`44268`blr Start，完全匹配当前`stream_class{version2,start,stop,add_attachments}`槽。真实Start成功才wrapper+0x40 streaming置1。当前RequestStreams只`stream_begin(...,false)`采集，graph Start才true是正确分层，不修改为虚true。上述是原件**静态可达及必要分支**，不是此轮设备实际调用证明。
- **handler协议吻合**：原件`_registerStreamOutputHandlers 0x1a1f684fc`用栈block并于`6859c`调`_mscsn_setStreamOutputBufferHandler 0x1a1fb1830`；后者`1864`真实objc copy、`1894/18fc`构造`{outputID:{handlerKey:copiedBlock}}`两层dictionary、`191c`通过stream setProperty交给驱动。只读原SystemOS已挂DSC的GOT chain解析确认`1dacd8290→VideoOutputHandlers`、`1dacd8960→PixelBufferHandler`、`1dacd82a0→VideoOutputsEnabled`，后者由`numberWithBool`产生NSNumber；SecondaryScaler/StillImage输出ID同当前代码。primary handler invoke `1a1ef6ba4`取x0=block/x1=PB/x2=24B CMTime地址，并实际将PB+复制CMTime交`_multiStreamCameraSourceNode_sendBufferToBufferServicingThread`。当前`:706–784 outputs_set`全事务校dictionary、Block_copy、每output revision、无callback中替换，snapshot再次copy、最终检查epoch/revision；`:1195+`必须真实CVPixelBufferPool `BufferPool`，形状验证后才deliver。必需属性`VideoOutputConfigurations/VideoOutputsEnabled`可读写、`VideoOutputHandlers`WriteOnly已声明，读取回原保留配置；未发现要补假Start/另一camera plugin path的确定缺口。
- **复查材料/范围**：局部原件反汇编、symbol与GOT结果在本树gitignored `x9/out/camera-ios-static/{source-start,stream-start,video-stream-start,request-control,register-handlers,handler-set,handler-invoke,source-enable,output-config,raw-got-keys}.txt`；`cm_dis.py`仅读原件，用capstone按VM segment读取，原件不改、不入git。optimized共享stub/GOT部分不在独立extracted image里，key值因此只读已有`/private/tmp/sysos20H392/System/Library/Caches/com.apple.dyld`原SystemOS映射，没下载、挂新盘或作为构建输入；原cache pointer低36位还原仅用于这几个已知GOT key。没有构建/测试/设备操作/额外检查新产物，也没有屏幕证据。继续最早确定阻塞：历史`fcc6225a`移除了camera shim对象/caller而mkadt仅零槽，必须用新bounded camera collection恢复实际当次IRQ/CSI metadata，不恢复旧无界per-field walker或GPS。

#### 2026-11-15 修复更早相机零handoff：单有界collection恢复IRQ+CSI metadata（source树）

- **确定缺口/历史依据**：`fcc6225a`因当时shim启动卡住，从`x9/shim/build.py SOURCES`及loader移除了camera/GNSS两旧walkers；现main已恢复audio bounded，但camera仍无objects/caller，`mkadt.py`只有`x9-camera-irq-layout`64B与`x9-camera-calibration`32B零槽。于是新native admit的真实IRQ topology查chosen必失败，PHY校准也没有当次数据。这是当前源码/构建输入确定缺口，不以算法到屏幕路径替代它，也不恢复旧per-field全树扫描或GPS。
- **实现**：新`camera_handoff.c/camera_fdt.h`，一个bounded完整ADT walk定位两个原槽（任一misplaced/duplicate/flagged/错误size拒绝，验证到末尾才清零）；静态12node/~6KiB `fdt_collect`一次收完整所有属性，候选最多GIC+CSI0/3共3phandle以独立feature bit注册，最多一次第二pass检查全树唯一且owner正确。所有完整FDT END/trailing验证与每2048token watchdog/每pass2s预算复用**已实跑audio**的`fdt_collect.c`，没修改它或audio调用；camera wrapper另校完成pass期限，短树没有checkpoint也不能越预算后publish。ADT walk自身同样有节点/深度/总bytes/token watchdog及2s上限，不把BSS表放64KiB栈。
- **所有原资格保留**：IRQ必须unique Root/Soc/CAMSV/GIC，GICv3/4cells/reg/controllers/status与唯一phandle、root显式parent/Soc与SV继承或显式同parent、路径禁止interrupt-map/mask/extended、Root/Soc2cells与空ranges；CAMSV0/cammux0、10个outer/inner banks的精确命名/地址/大小、三组SPI0x26d/273/279 levelhigh+affinity0及最终64B ABI。CSI仍要求top/efuse和两个消费者+exact provider cells启用/compatible/csi-port/nvmem-cell-names/nvmem-cells唯一phandle/真实reg1d0/1dc+4；chosen/chosen@0唯一且不从malformed/ambiguous回退，atag,devinfo为原LE count<=400、长度恰好、仅取两原words。没读efuseMMIO、没打印devinfo/设备标识、没添加固定值；IRQ与校准独立发布，缺chosen@0或cal被拒不自动撤IRQ，某phandle拒只撤相应feature。整棵树格式/预算拒则两槽保持零。
- **真正构建接线**：`build.py`链接新aggregator及两个改为纯collection查表的camera qualifiers；loader仅调用一次`fdt_camera_patch_adt`，非fatalrefusal保持零；GNSS仍excluded，不恢复GPS。旧`fdt_camera_irq_patch_adt/fdt_camera_calibration_patch_adt`独立full-tree入口及camera内`fdt_property/fdt_capture_nodes/fdt_phandle_unique`调用全部移除。只复制当前bootIRQ/CSI出处，不armIRQ、不访问CAM/GIC/SMMU/efuseMMIO、不发硬件grant、不改变mode1/2输入或native READY条件。
- **编译/验收边界**：正常`toolchain.CFLAGS --target=aarch64-none-elf -O2 -Wall -Wextra -Werror`编`camera_handoff.c/camera_irq_handoff.c/camera_calibration.c/loader.c`四个单对象最终**exit0/零warning**，日志/命令/对象在本树gitignored `x9/out/camera-shim-compile/final-compile.log`（初版compile.log也保留）；loader局部编译使用diag0/USB0/UFSroot0/native0，最终主线程正常完整构建将编实际配置。没有测试套件/基线篡改运行、完整link/新Image/boot/CI/设备/分区操作，没有额外hash/解包/反汇编编译产物。新camera collection尚未实机验证耗时/slots；audio248的双pass1130ms、watchdog26只能证明共享collector原路线运行，不能当本camera handoff成功。下一动作主线程cherry-pick并正常构建mode2候选，首次日志先验当次IRQ/CAL槽资格及native register真实ID/close，再到MM/SMMU/连续RAW/graphStart/真实handler和物理屏幕。

#### 2026-11-15 相机运行入口及更早zero-handoff阻断已接线（主集成）

- 已集成 `a888c88→6d8f9aa`（actual physical-close后archive完整epoch，fresh MM/AP OFF/prior bank后才换namespace/选中RET-slot）、`b119ca8→200441a3`（unstarted gate的no-pending实际sentinel=8，不错当bool），`c360d41→ac6dc756`（capture receipt/history、reopen与recycle wrapper）及 `68a05f6→60ba75e`（新bounded camera IRQ/CSI双槽收集及真正shim对象/caller）。后者修复现行源码确定的更早问题：旧loader移除camera，mkadt只zero槽，不能靠允许admit或硬编码IRQ/校准跨过它。所有静态原件graph/handler协议结论见前节，仍无本轮运行/屏幕证据。
- 主线程已接 `cam_native_sensor_power(off)`：只有原sensor/RX/SV/OUT/PHY/MAC/SMI/MM真正close成功才调用 `cam_native_capture_recycle_locked(epoch)`，ID-only probe无capture则no-op，任何错误保留旧epoch/首错。mode2 init仅启动一次普通kernelthread→真实engine admission→两sensor power/ID/close；getter没有register副作用，失败不自动retry；日志明确只有physical ID admission，不声称RAW/预览。注册不等待UFS floor，prepare/configure仍严核实际floor/供应者。
- 联合正常Builder camera TU最新 **exit0/145warnings**，含root physicalclose/init与上述native/capture代码，日志 `x9/out/camera-integration-reopen-init-fixed-compile/{single-compile.log,build.log,kernel-objects/}`。首编因未声明 `thread_terminate` 失败1error/145warnings，已按既有touch/keys的真实声明修复；失败原件保留 `x9/out/camera-integration-reopen-init-compile/`。`gh run list --limit 3 --json databaseId,status,conclusion,headSha`仍为空，无CI run；未full kernel/boot、新Image、测试套件、构建后二次检查或设备操作。
- **仍active的具体必补分支**：continuous controller常在repeat READY（new RAW_DEVICE、尚未RCNT）；旧STOP只认current actualDONE/RAW_IDLE，不能把new未issue身份标DONE/PARKED。native正在实现独立old真实spentDONE+new never-issued取消双identity receipt及CANCELLED/RAW_CANCELLED，capture/source代理同步scope/review。因此admit仍ENOTSUP，boot-device仍mode1，不出已知必失败的过程镜像。八个exportedepoch pin上限之后ENOSPC、partialnormalsetup/queueerror/partialPREPARING保守retain的约束保留，不能声称任意关闭/切换可靠。
#### 321 校准/原固件续查（2026-11-15）— 三代理静态并行；两处 host 边角修复编译通过

- **用户规则/状态**：本轮明确要求多开子代理加速，三个子任务分别追 RV55 指令/参数 getter、TCS overlay/校准 callback、HF command 转换；主线程追原 LK 参数 producer 并修代码。功能树 `fix/sensors-calibration321` / `.worktrees/sensors-calibration321`，基线 main `55c3508`。仍不出探针、不操作设备。重新只读核对 `~/321.txt:723–737/823/846`：READY、SHARE_MEMORY seq1、TIMESYNC seq2 成功，LIST stage9 errno60/records0；没有新 LIST/ENABLE/lux/near-far 实测。最新已有非探针联合候选仍为 `x9/out/x9-proximity4-20261115/boot.img`（`b4aa2ef`，ALS3+prox3/GPU280），不含后续 LIST 或本轮修复；旧 LIST 探针不推荐。
- **源码里程碑 `8682208`**：PROX 已有真实 DATA、ACK enabled1 时，iOS 只改 cadence 的 ENABLE/coalesced ENABLE 不再无条件清 `valid`，避免 on-change 无新 edge 时永久丢失已知 level；DISABLE、OFF→ON、错误、disconnect 仍失效，ACK 仍不制造 DATA。固件重复 ENABLE 是否补发 current DATA 未知，不能称此项是321的实测根因。共享 SCP 启动复用若 owner 已 `active=0` 正在 DISABLE/退出，则返回 ENXIO且不 pin；不复活线程、不重启/重置 SCP，原错误优先返回。
- **编译边界**：按现有 Builder `compile_one` 正常顺序交叉编译 `sweet_x9_proximity.o` / `sweet_x9_als.o`，均 exit0（13/27 warnings）；日志、objects、depfiles 位于功能树 gitignored `x9/out/sensors-calibration321-compile/{single-compile.log,build.log,kernel-objects/}`。没有 full kernel link、boot pack、新固件/插件、CI run、测试脚本/套件、产物二次检查或 ADB/mailbox/fastboot/分区操作，目标仍未完成。
- **参数入口（原 SCP 静态证据）**：原 payload `0xcbf90` 请求 shared memory id27，经 `0x612e6` 的 MMIO metadata/table 查表返回地址；`0xcbfca` 复制 blob `0x5608` 到 `0x2d3434`，再将 blob+`0x4ac8` 的 `0xb40` 复制到 `0x228918`。`0xcc00e` 校验 LE magic `0x37847857`、按 category stride `0x4fc` 查设备参数。live DT id27 是 `0x100000`；当前 shim 只构造 metadata，ALS share 只初始化2/8/9/10，**未证明321 id27内容来自LK/已正确填写**。321 region struct_size60不能据当前72B头文件推定 `sensor_data_addr`；原 SCP raw0x3c word实际为0x40，是地址/结构边界候选，不是0x400000。
- **现代原 LK producer 命中（继续追，非实机证明）**：只读原备份 `.local/x9/backups/partitions/firmware-originals/gps-secure-runtime21-20261110/lk_b.bin`；raw `0x1b2660` 含唯一 magic/template；ARM64 raw `0xb74dc/0xb74e0` 以 `0x4fc` 索引 template，`0xb749c` part stride `0xd4`，`0xb93b8–0xb93c4` 将完整 `0x5608` 复制到 LK global `0x20e388` 指向的地址。与现代 SCP consumer 尺寸相符，但 producer 地址到 SCP id27 的交接尚未闭合，不能直接新增 DT 拼包。搜索 magic 命中的第三方 NovaLK `platform/mt6765/mt_scp.c` 是旧 `0x1134/0x1cc/0x98` schema、非OPPO权威源码，只作名称参考，不移植。
- **有限 RV55 解码**：三个独立 assert/queue/overlay 路径交叉恢复 opcode0x67 的 B-format offset、compare-zero 和小正数立即数 unsigned branch；Zba/Zbb 用公开掩码。`b832/b836` 的 rlist3 在公开 Zcmp 下 reserved，保留 unknown16，不猜push寄存器。分析原件/派生物均不入git；scratch `x9/out/sensors-calibration321/` 包含 `isa-agent/rv55_slice_v2.py`、精确分支证据、LK/SCP局部 decoded，收尾需迁回主checkout。
- **原 LK producer→id27 已静态闭合**：raw `0x4652c` 为 `scp_sharedram_alloc`；`0x46614` 将 `reserve-memory-scp_share` 分配结果放入sp+0x28，`0x46624` 用id27调用 `0xbde94` 取global `0x218430[id*8]` 的offset，`0x46628/0x4662c` 把reserved base+offset27传给setter `0xb9478`，`0xb948c` 写global `0x20e388`，就是上述blob复制目标。LK `0xbe154–0xbe188` 对非零memory条目累计size/应用alignment/写offset并通过secure op9登记，与SCP getter的31项table对应；**当时尚未闭合动态dump0来源；本轮续查更正为LK本地五字段前缀和＋SAP DT值，不是secure查询（详见下节）**。原blob template `0x5608` 只有magic四字节非零，其余由运行时DT producer填入。不能把原件template当实机校准数据，也不需要基于旧struct猜一个AP初始化包。
- **现代字段/TCS init**：blob `0x5608 = 4 +15*0x4fc +0xb40`，category `0x4fc =4 +6*part0xd4`；common part为u32 `+0 sensor_name/+4 i2c_num/+8 direction`、u8 `+c i2c_addr`、u32 `+10 eint_num`，`+14`起专用payload。LK property读取/REV存储和SCP getter双方独立匹配：`cc00e`返回full part，`cc0a0`返回part+14；category4/5对应LIGHT/PS，不是wire type5/8。TCS raw `0x4242aa` 调 `cc00e(4,9)`，缺失直接return -2；probe reg0x92期待0x82，最多3次，失败-1。PS payload `cc0a0(5,9)` 缺失有默认块；ALS payload `cc0a0(4,9)` 的NULL默认块不能在此前同一blob/common成功时当作正常可达缺参路径。**默认块、probe recipe不等于321实际注册或参数正确**。
- **AP CONFIG/type完整静态链**：AP CONFIG command4经 `0x89b26→0x892c6→0x8972c/0x89840→0x912dc→0x91366` 转成内部command12，以APpayload复制出的新包排到class3；`0x91b5c/0x91bf2` 调ALSPS obj+4=`0x11c9f0`，`0x11ca14` 对内部12转 `0x11cf18`。原固件正反translate表 `0x20919c/0x20925c` 明确 internal6↔wire8，DATA `0x8a474/0x8a476` 转后写wire header；原件表还明确internal5↔wire5、internal28→wire95、internal62→wire116。因此**X9使用LIGHT5/PROX8正确，不能因callback内部PS6就改wire为6**；入口到逆向translate的精确调用切片在chain-agent内。
- **设备专用CONFIG载荷已分流，数值仍缺**：内部12只认type6/28/62，没有type5直接CONFIG分支。PS callback ops+44=`0x19d51c`/raw`0x423664`固定copy24B：six-word `[hp_offset, unused_here, hp_delta, lp_offset, unused_here, lp_delta]`，四有效字段截u16，名称由原log参数绑定。CCT/internal28（wire95）chipkind2路径传12 words/48B到ops+20=`0x19d920`/raw`0x423a68`，first4 channel scales int/1000.0、index6 clock int/1000000.0，零channel默认1.0；clock有效范围仍有未知压缩运算，不猜。extra gain/internal62（wire116）传8 words/32B到ops+60=`0x19dfca`/raw`0x424112`，global byte`0x224d99`非零才copy。独立factory internal21的type5/start1锁硬件gain512/stop解锁，**不是LIST gain或正常校准值**。48B通用cache不是PS wire48，不发送全零CONFIG/DT猜包，不把factory512套作lux转换gain。
- **阶段产物/下一步**：所有原件/局部decoded/有限decoder保持gitignored `x9/out/sensors-calibration321/`：`lk-sensor-evidence.txt`、`lk-memory27-handoff.decoded.txt`、`lk-sensor-builder.decoded.txt`、`isa-agent/blob-consumer-evidence.txt`、`overlay-agent/tcs-ps-config-evidence.txt`、`overlay-agent/tcs-cct-config-evidence.txt`、`chain-agent/`。LK复查用既有Capstone对原raw的 `0x4652c..0x46630/0xb9478..0xb9494/0xb74d4..0xb7618/0xb93b0..0xb93dc` 局部反汇编；SCP复查用 `python3 x9/out/sensors-calibration321/isa-agent/rv55_slice_v2.py x9/out/scp-list-static321/scp.bin 0xcc00e 0xcc0fc`。继续静态核定LK secure dump0/offset27与shim实际布局，完成TCS参数字段到live DT的有据映射；实机blob/工厂校准值、真实LIST/DATA及SCP↔host时钟仍未验，不宣称321或完整目标已修好。本轮不再展开校准旁支或新增探针。
- **实际合入/收尾**：三代理已结束，最后一处入口也闭合：`0x879b6/0x879bc/0x879c0` 将AP wire type经 `0x8a2e2` 转换并写回，`0x879dc/0x879ec` 调 `0x286e94` 槽，其已注册callback就是 `0x89b26`；wire8→internal6→CONFIG4→内部12→PS callback不再是猜测。源码 `8682208`、交接 `90749d5/529c819` 与同期call main `ea1407f` 无冲突整合为 `17ac525`，已ff合main并 `git push origin main` 成功（`ea1407f..17ac525`）；同期call没有改本轮两个sensor编译单元，不重复编译或检验旧产物。静态与编译两个目录已实际迁回主checkout **`x9/out/sensors-calibration321/`、`x9/out/sensors-calibration321-compile/`**，原件保持 `.local`，旧日志中的worktree绝对路径不回写。此最终NOTES补记随后正常合main/push，仅清理本轮功能树/本地分支；其他工作树/旧候选/分区备份不动。仍无新boot、CI或硬件结果，完整goal保持未完成。


#### 2026-11-15 双传感器静态续查 — LK布局条件一致，PROX cadence期间真实edge拒收已修

- **范围/状态**：用户要求多开子代理、继续静态、不出探针；复用三个代理追LK布局、TCS参数、host控制/DATA链。功能树 `fix/sensors-layout321` / `.worktrees/sensors-layout321`，起点main `057e121`。重新只读 `~/321.txt:723–737/823/846`：READY、SHARE_MEMORY seq1、TIMESYNC seq2已过，LIST stage9 errno60/records0；没有新物理运行数据。最新已有非探针联合候选仍为 `x9/out/x9-proximity4-20261115/boot.img`（`b4aa2ef`，ALS3+prox3/GPU280），不含后续LIST和本轮源码修复，旧LIST探针不推荐。
- **真实源码修复 `a5d6305`**：此前 `8682208` 仅在cadence/coalesced ENABLE保留已有valid，但 `x9_prox_native_data()` 仍遇pending无条件EAGAIN。worker `als_prox_service()` 完成ACK后可立即coalesce下一ENABLE；同轮 `als_drain()` 把真实edge拒收且推进rp，对on-change传感器可能永久保留旧near/far。现仅在已有真实ENABLE ACK (`state.enabled=1`)、最新desired仍ENABLE、in-flight若有也是ENABLE时放行DATA；初次ENABLE、requested OFF、pending DISABLE、expire/error/disconnect仍拒绝。type8/oneshot/时间单调校验未变，ACK不更新样本/不制造DATA。此为控制流证明，不是321已实测edge或LIST根因。
- **正常编译**：用本树原Builder `compile_one(sweet_x9_proximity.o)` 返回True/exit0，13 warnings；日志 `x9/out/sensors-layout321-compile/{single-compile.log,build.log}`。只编该变更对象；没有full kernel link、boot pack、新镜像/固件/插件、XNU对象树重建、测试脚本/套件、构建后复检或设备操作。`gh run list --limit 3 --json databaseId,status,conclusion,headSha`返回空，本轮无CI run ID。
- **LK dump0来源更正**：原LK raw `0x46748` 返回本地global `0x1f4558[type]` 前缀；`0x46764(type,value)` 初始化SRAM并按selector1..5累加。`0xbdfbc..0xbe040` 喂五个DT字段，`0xbe05c` 取type5总和写global `0x21853c`；`0xbeccc` 仅读global `0x218560`（SAP secure-dump-size）。所以 `0xbe1f4..0xbe208` 的id0是本地五字段sum＋SAP，并非secure查询来源；op7/op9登记不能误称查询。
- **原始DTB条件计算**：直接解析 `.local/x9/modem/live.dtb` 的BE原字节（不用DTS的转义字符串猜数）。五项为 SRAM `0x400000`、cache `0xc4000`、reg `0x3c00`、tbuf `0x400`、DRAM `0x750000`，SAP `0x24000`，dump0=`0xc3c000`。原LK31项与Android/shim30项的活跃offset均一致：id2=`0xd06700`、8=`0xfc3700`、9=`0xfd3700`、10=`0xfd5700`、27=`0x1769300`（base `0xe8000000` → id27 `0xe9769300`）。LK跳过零size项，shim为其写当前offset；本DTalignment全0，不影响非零项。尾id30=`0x10000`只造成allocated `0x189a300` vs `0x188a300`，不能用它解释LIST TX/RX或改id27。**这证明保存DT下的公式一致，不证明321运行时table/blob字节**。代理早期把cache转义误读为`0xc40`产生的数值已撤销，最终产物从原DTB重算。
- **地址域边界**：SCP原payload raw `0x3c=0x40`、`0x40=0x34`、`0x4c=0x74`、`0x74=0xfc`，与getter `B0000040/44/48/4c` 的metadata/table形状对应；不得把这些SCP地址等同AP `als_core0` GPR偏移。公开 `scp_reg.h` 的core0+0x4c明确是EXPECTED_FREQ_REG，不是table；本轮没有新增该地址读取或猜AP TCM映射。
- **静态产物/未完**：本轮分析在gitignored `x9/out/sensors-layout321/`，LK `lk-agent/compare_live_layout.py` 直接解析DTB，结果 `live-memory-layout-comparison.{json,txt}`，窄decoded保留raw/VA标注。TCS schema/live-DT选择、通知时间→真实样本年龄映射仍继续核定；不发全零CONFIG、不猜工厂值、不改wire PS8。新源码仍未被full link/上机，真实LIST、ENABLE、lux/near-far及自动亮度/遮挡效果未验收，完整goal未完成。

- **TCS schema/panel路径已收束**：原LK `ba49c` 的私有属性vector精确映射LIGHT/PS payload；front `light_1/proximity_1` 的common sensor_name9/I2C0/addr0x39来自front，不来自panel节点。panel匹配为tolower后的substring，`P_7_2`在`P_7`前；选中P_7只把指定参数覆盖front。panel为空/未匹配/overlay失败仍到 `b9870→b7414`，不能单凭panel失败解释category4/name9空白。缺node跳过part留template；node存在但私有属性缺失时u32=-1/u8=ff，common缺属性则不写。PS sensor-reg count在payload+0c是cell count8，+0e起4对u16；TCS `count>>1`匹配。PS非NULL的+20..78无条件copy，非NULL不代表所有缺参可默认；ALS ir_ratio/coef_ratio有signed fallback，decision_index=max(v,1)，其他未解select不猜值。窄证据 `overlay-agent/lk-light-ps-evidence.txt` / `lk-light-ps-schema-tables.txt` 保留重放命令，无CONFIG/原固件修改。
- **样本时间仍待闭合，不实施猜测mapper**：host `als_poll()` 丢弃notify n+12 scp_timestamp/n+20 archcounter，只用wp；ALS/PS host_time是receipt time，ALS 5s检查目前不是measurement age。Android正常/超FIFOnotify都先配对host boot_ns/CNTVCT、扣传输延迟再offset映射DATA。原SCP `877ba→120a2` 写relative CSRtime，`877c2→12076` 写raw CSRtime；120a2返回路径减epoch，无可见scale。PS producer962c2也读120a2，DATA8a430/8a526复制输入timestamp；PS中间队列与ALS producer仍核定。321行6与762证明AP CNTFRQ=1GHz，**不证明AP CNTVCT与RV CSRtime同counter epoch**。下一步先闭合source getter/中间路由/共同timer contract，再加runtime频率、checked64/128 arithmetic、measurement/receipt分离及epoch边界；不硬编码13MHz，不把scp_ns cast为mach ticks，不给on-change PROX加5s过期。方案与exact slices `chain-agent/time-mapping-evidence-and-next.txt`；这不是321 LIST timeout的实测根因。
- **同期main整合**：源码 `a5d6305` 与交接 `4a2d952` 在功能树通过merge `46586eb` 整合实际main `a6f63b5`（保留同期call/SMS修改）；这些同期提交没有改三个sensor编译单元，不因docs/其他功能重复编译。最新call/SMS `x9/out/x9-call-sms-init-20261115/`只有kernel-only构建，不是新的可启动联合boot。仍没有包含全部后续sensor修复的新boot.img；新源码及这条交接正常合main/push后仅清本轮树，目标仍active。

- **新增time producer静态闭合/收尾**：ALS internal5 normal两路 `11ea3a..11ea6a`、`11f774..11f7a4` 与PS internal6 `962a2..962e0` 都以 `120a2` 写packet+8/+c，和notify.scp_timestamp确为同getter；`91698→8b182` 将packet指针存message+c，handler892c6的command6分支→89dae再serializer原样copy时间。但posted message+8=3/+9=action0如何映射到handler6仍缺桥，不能把已匹配片段称完整producer→FIFO证明；AP/RV同live timer epoch仍未证。精确下一地址与重放在 `time-agent/time-domain-closure.txt`。三代理已停止写入。
- **实际交付/归档**：源码 `a5d6305`、交接 `4a2d952/bb76e8c` 经 `b069685` 整合最新main（保留同期call/SMS及其文档），已ff合main并push成功 `fef6c31..b069685`；没有重编同期未变sensor源码。分析与单对象编译产物已无覆盖迁回主checkout gitignored **`x9/out/sensors-layout321/`、`x9/out/sensors-layout321-compile/`**（chain-agent原已在主目录，其余lk/overlay/time合并到同目录；原件不动，旧worktree绝对路径不回写）。本NOTES补记随后正常合main/push，仅清理本轮worktree/本地分支；其他工作树/旧镜像/长期输入均不动。没有新boot/CI/上机结果，完整goal未完成。静态复查可从主目录运行 `python3 x9/out/sensors-layout321/lk-agent/compare_live_layout.py .local/x9/modem/live.dtb --out x9/out/sensors-layout321/lk-agent`；时间getter复查用既有有限decoder对原SCP `0x11e9f0..0x11ead0/0x961f0..0x96450`，不需设备或探针。

#### 2026-11-15 双传感器功能候选与异步 DATA 静态闭合（本轮）

- **范围/基线**：用户继续要求静态分析、不要探针，并明确授权多个子代理。功能树 `fix/sensors-time321` / `.worktrees/sensors-time321` 的实际源码基线 **`baec47b`**；三个代理分别追异步 DATA dispatcher、AP/RV counter 契约、host ingress/control。主目录同期已进到 `8bc5670`（call END metadata），**本候选不包含这两个后来的 call 提交**。未改 sensor 源码、未新增诊断代码/token、未操作设备。
- **最新正常双传感器候选已实际构建**：先 `python3 x9/build/prepare-local.py --check` 成功，再在功能树执行 `python3 -I x9/build/build.py all --boot-config x9/out/x9-sensors-functional321-20261115/boot-input.json --name x9-sensors-functional321-20261115`，真实 **exit 0**（完整 kernel link + boot pack），内置输出 Image **30912512 B**、boot **14094336 B**。`boot-input.json` 完整沿用旧 proximity4 的设备配置：**ALS3 + PROX3 / GPU280**；没有新插件包，无需 mailbox（设备仍保持321的GPU280时）。包含 `4a19ace/804b39b/1fcf342/8682208/a5d6305` 的已合真实修复。`source-head.txt` 已按功能树真实HEAD纠正为 `baec47b`，不是同期主目录的 `8bc5670`。产物当前在功能树 `x9/out/x9-sensors-functional321-20261115/`，收尾迁回主目录同名路径后再清树；这是正常功能候选，不是旧 LIST 探针。没有 CI run、测试脚本/套件、构建后二次解包/hash/反汇编/对比或硬件验收。
- **异步缺桥更正、原固件 DATA 链已闭合**：旧交接误将 handler 的 action6 分支视为唯一 DATA 入口，**不需要 action0→6 remap**。原固件 `892c6→89378→893da(beqz action0)→894ca→89dae` 可直接入 DATA。`919a2→8b49a` 注册 operation3 prehandler `91b5c`；`891c4` 写 descriptor callback `892c6`，`891f8→908a2` 将 descriptor 放入 client+8 并入 manager list；`91b98` 取 message+c 原 packet，`91cda/91ce4/91ce6/91cec` 按 callback/action0/packet 原样调用。因此 ALS5/PS6 的 `120a2→packet+8/c→915b6→8b182→async8c274→91b5c→892c6→89dae→8a430/8a526→FIFO87c84` 在已启用 subscriber DATA bit0 gate 下静态闭合，时间戳未改写。证据与精确 replay：`x9/out/sensors-time321/dispatch-agent/dispatch-closure-evidence.txt`。这不是321已到达 ENABLE/DATA 的证明。
- **host 审查边界**：本轮未找到另一个与 LIST/ENABLE 强相关、尚未修复的确定控制流缺陷，不重复 `a5d6305`。`als_poll()` 仍只保存 wp、丢弃notify时间对，ALS/PROX host_time 是 receipt time；ALS5s检查不是 measurement freshness。FIFO限128条意味着未来 mapper 需保留每个 normal/super notify boundary 的时间上下文，不能把最新全局 offset 任意套到旧 backlog。PROX on-change 不加 ALS5s过期。无 AP/RV epoch 证据时不实现猜测 mapper；N/C 是先后两次读取，不能称同瞬间精确 pair。host证据 `x9/out/sensors-time321/host-agent/host-time-design-and-build-input.txt`。
- **实机结论仍未变化**：只读 `~/321.txt:723–737/823/846` 仍是 READY、SHARE_MEMORY seq1、TIMESYNC seq2成功，LIST **stage9 errno60 / records0**，PROX跟随transport失败；没有新的 LIST/ENABLE/lux/near-far、自动亮度或遮挡熄屏验收。**不能称321已修好或完整goal完成**。下一静态动作收束AP/RV计时器路由/epoch及LIST最终审查；新正常候选可由用户手动临时boot验证，不自行ADB/fastboot/reboot/flash/写分区。
- **计时器静态收束**：`321.txt:3` 的boot EL=1确实来自入口 `CurrentEL`，跳过shim的EL2 `CNTVOFF_EL2=0`；本轮没有证明继承offset为零。SCP `66828→12076` 采样raw CSRtime，`66846..6685c`重载同pair，`6685e→121fa`写epoch `251e00/04`；原字符串称 `ostimer_init_cycle ... cycles`。因此relative时间减的是SCP自身ostimer初始化count，不是AP TIMESYNC写入的epoch；不能从AP CNTFRQ=1GHz、DT timer1GHz或GPU duration recipe断言RV同源/同rate/同origin。GPU原驱动还实际校准 `TIMESTAMP_OFFSET`，更不能拿它证明zero epoch。证据与replay **`x9/out/sensors-time321/timer-agent/hardware-counter-contract.txt`**；host mapper继续不实现，不把rawwire time直接cast成mach ticks。头文件仅补注释标明legacy `scp_time_ns` 为未转换wire time、ALS5s为receipt-age、PROX level不过期，ABI不变。
- **额外有据LIST相关性加固 `fb8657b`**：host此前只匹配outer completion，现 `als_list()`还要求 **RX h[0]==本次sequence** 再接受CRC-valid LIST；旧/不相关header返回EBADMSG，不能安装sensor。最终专用路径验证：`87bea→88ce6` 只在buffer+14+非负offset写40B record（有capacity检查），`87c72→88bb4` finalizer仅写buffer+c rx_len、buffer+0 CRC，memory.sequence在buffer+4不变。因此h[0]相关性有据。这是静态协议相关性加固，**不是321 stage9 timeout的已观测根因或物理验收**；不改地址/长度/CRC算法/等待/近远极性/校准，不吞控制错误。没有扩大notify.value n[0]校验；LIST专用确有回显，而common `886d2` 的VALUE.seq未见初始化，不能把两条路径混同（见最终更正）。
- **最新推荐非探针功能候选为r2**：在 `fb8657b` 源码上使用同一完整ALS3+PROX3/GPU280输入重新正常 `build.py all`，唯一name **`x9-sensors-functional321-r2-20261115`**，真实 **exit0**；内置Image **30912512 B**、boot **14090240 B**。改动ALS编译27 warnings，PROX13、SweetHID15；完整link/pack完成，不额外运行测试或产物复检。两次正常构建目录已经**实际迁回主checkout**，不覆盖旧镜像：**`x9/out/x9-sensors-functional321-r2-20261115/boot.img`** 是最新含全部上述sensor修复的正常候选，前一次 `x9-sensors-functional321-20261115`保留；原`x9-proximity4-20261115`不再是最新。每个目录保存 `boot-input.json/source-head.txt/build.log/full-build.log`；没有新plugin-package、CI run（`gh run list --limit 3 --json databaseId,status,conclusion,headSha`为空）或设备结果。复编在该源码/新唯一name下使用同目录boot-input；用户验证用 `fastboot boot x9/out/x9-sensors-functional321-r2-20261115/boot.img`，重点先看真实LIST LIGHT/PROX与ENABLE ACK，再看lux/near-far及自动效果。代码构建绿色不等于完整goal完成。
- **LIST专用路径最终更正，撤销common-path泛化**：`8a17c` notify callback的 `8a180/8a182` 先把outer.seq复制到VALUE.seq，`8a192/8a194` 读VALUE.command高nibble；LIST cmd0经 `8a1a0/8a1c2/8a1d2→8a26e/8a25e→8a274→87ba0` 走专用异步路线。其finalizer `88c20` **从memory.header.seq（buffer+4）** 读序号，`88c34`写outer template、`88c38`也写notify VALUE.seq，再经 `87788`发回。因此未修host的memory.seq0/outer3可导致LIST回复outer0；入口VALUE虽已被固件改成outer3，也不能修正memory0。`4a19ace` 将memory/两个notify header统一为3仍是正确修复，但**不能再以common `88824` 从VALUE0回显解释LIST**。此前common `886d2` 的VALUE.seq未知不能泛化成LIST专用n[0]未知。源码仅更正两段注释为真实专用路径，h[0]检查与r2行为不变，**不因纯注释/交接变化再编译或复检**。精确切片/replay `x9/out/sensors-time321/dispatch-agent/list-final-audit.txt`；仍没有321原始LIST回包来证明实际outer0或物理成功。
- **实际合入/归档收尾**：本轮代码 `fb8657b`、注释/交接 `d044a61` 经 `dac5c2a` 保留同期main `e1c0af1` 的call END/SMS admission修改，已ff合main并 **push成功 `e1c0af1..d044a61`**；同期这些文件不在r2的源码 `fb8657b` 中，不把集成HEAD误写成已构建HEAD，不因非sensor/纯注释变化重复编译。三个代理均已收束/停止写入，静态分析目录已经**实际无覆盖迁回主checkout `x9/out/sensors-time321/`**（dispatch/host/timer、prepare-local日志）；两次正常候选早已实际迁回主目录，原固件/Apple/设备日志仍gitignored。此最终NOTES补记正常合main/push后仅移除本轮 `.worktrees/sensors-time321` 和本地 `fix/sensors-time321`，其他工作树、旧镜像、分区备份与`.local`输入不动。仍无新的物理验收，完整goal保持未完成；下一有效验证是用户临时boot上述r2后的实际LIST/ENABLE/DATA及自动效果，不再重复已排除的布局/schema/common-path猜测或出探针。

#### 2026-11-15 HID 生命周期静态审查与真实 ENOMEM 修复（本轮）

- **范围/基线**：用户要求继续静态分析、不出探针，并授权多代理；本轮 `fix/sensors-hid321` / `.worktrees/sensors-hid321` 基于 **`8c7b7ac`**。三个代理分别核对原20H392 ALS消费者、PROX消费者及HID生命周期；只读原Apple/SCP/已有日志，不重复r2产物检查。最新推荐正常镜像仍为 **`x9/out/x9-sensors-functional321-r2-20261115/boot.img`（源码fb8657b）**，未包含本轮改动。321的LIST stage9/errno60、records0结论不变，r2尚无新实机日志，不能称光感/距离/自动效果已验收。
- **确定源码缺陷与修复**：SweetHID基类start原先在retain前将borrowed workloop存入free会release的字段，其他allocation失败时引用不平衡；现在获取非NULL后立即retain。handleClose原先compact数组allocation失败会留下原登记，而userclient已进入Close，导致恢复内存后reopen遭ExclusiveAccess且再close不通知owner；现在先在_clientLock内无分配取消登记，再做COW compact，OOM只留inactive tombstone，open/isOpen忽略并清理它。dispatch仍retain数组snapshot、不改旧count；共享wrapper字段读写同锁，局部retain target后锁外callback/release，无新增持久owner cycle或锁内回调。未新增实例字段/ABI变化。这是**确定ENOMEM控制流修复，不是321的已观测根因**，不声称必然UAF。
- **独立复核/边界**：正常ALS/PROX userclient open/close与poll共用provider workloop gate；当前原件context=NULL。close不是in-flight callback的drain barrier，已捕获callback可完成一次；该限定路径由局部retain、原Close/inactive检查和stop移除gate保障，不扩展为所有opaque context保证。复核 `x9/out/sensors-hid321/host-agent/{hid-lifecycle-audit.txt,hid-enomem-patch-review.txt}`（收尾迁回主目录）。ALS/PROX event allocation失败本来会保留sequence并重试，不重复改它。
- **编译里程碑**：仅在本worktree调用现有 `Builder.compile_one(SweetHID.cpo)`，**exit0 / result=True / 15 warnings**，没有link/boot打包、测试脚本/套件、CI或设备操作。exact replay脚本 `x9/out/sensors-hid321-compile/compile-hid.py`，日志 `single-compile.log/build.log`；未二次解包/hash/反汇编或复检构建产物。仅overlay修改，不重编XNU树。
- **ALS消费审查**：原BLControl匹配20/41、copy type12、整数level c0000及mach ticks符合现实现；ColorSpace0及缺省Placement/Orientation/ALSSensorType没有确定fatal分支。默认ReportInterval .05/.10/.40s在现接受范围；0/超界producer与原shared-island callee仍未权威闭合，**不添加猜测ReportInterval0补丁**。证据/replay `x9/out/sensors-hid321/als-consumer-contract.txt`，原Apple产物gitignored。
- **仍在推进**：PROX静态发现条件性ON→ON模式切换漏稳定near：UI1(wire1)/UI2(wire4)→UI3(wire2)清消费者filtered/object状态，而provider同enabled/cadence不发control且同sequence不派发。不是321根因（该日志没有DetectionMode/PROX DATA）。正独立核查原timestamp重派能否被消费者接受及pending/fault/OFF边界，尚未改PROX；不刷新时间或伪造FAR/ACK。完整goal仍未完成。
- **PROX re-arm已闭合并实现**：已接受的不同非零DetectionMode切换latch `fReplay=true`；同mode/cadence不清它，OFF与无有效snapshot清它。下一poll仅在真实backend sample可用且新sequence或replay请求时派发，event allocation成功才提交sequence/清flag，保留原sample的timestamp/sequence/near/mask与provenance；不强制另发ENABLE，不造DATA/FAR，不复活OFF/reset/fault。三个代理独立闭合消费者/host边界，实际diff复核通过 `host-agent/prox-replay-actual-diff-review.txt`。这是稳定遮挡切入active时的**条件性状态同步修复，不是321 timeout修复的实测结果**。
- **旧timestamp过滤的证据与限制**：IOKit `___IOHIDEventSystemIsTimeToDispatchEvent 18e5535f0` 只有service+c4及该client cache含ReportInterval时才比较event timestamp/deadline；c4由 `__IOHIDServiceSetReportIntervalForClient 18e574c44` 遍历cache OR得来，**不是registry ReportInterval存在即启过滤**。backboardd明确两个ReportInterval writer `100007388/10007adf8` 分别匹配gyro ff00/9、orientation ff00/3，不匹配PROX ff00/8；已识别PROX wrapper/普通Foundation callback/processor没有另一个必然拒旧T的age过滤。证据 `prox-agent/{proximity-contract-evidence.txt,reportinterval-refs.txt}` 与 `als-agent/prox-replay-time-review.txt`；不保证未来其它client/filter未设interval，启cache时同旧T可能被拒，不能靠等更久规避，更不能刷新T冒充新测量。异步setter后下一poll**不是绝对sensorModeDidChange完成barrier**，host异步fault可能允许已取快照完成一次，dispatch也非client队列交付证明。
- **最终单对象编译**：因X9Proximity.inc真实改变，重新仅 `compile_one(SweetHID.cpo)` **exit0 / True / 15 warnings**，日志 `x9/out/sensors-hid321-compile/replay-single-compile.log`；原HID修复编译日志单独保留。没有全kernel link/boot、CI run、测试脚本/套件、上机或产物复检。HID生命周期源码提交 **`b6dad31`**，已用 **`6d7f300`** 正常合入同期main `ed71d17` call/SMS实现及交接，无改写其他功能、无重编未变单元。本轮两项新修改均不在r2镜像，最新正常已构建候选名称不变。
- **异步顺序最后收束**：`applyUIMode 10002a698` 后在同线程同步调用observer `10002a700`，但实际property setter由Foundation `1bef4a594` dispatch_async至service `_workQueue`；未证明该queue与event callback/controller串行。processor+8锁只保证互斥，**先replay再通知清filtered的反序尚未排除**。本补丁只补齐provider缺失的re-arm请求，不能称ON→ON端到端竞态完全解决；不添加无据delay/重复重派或刷新T。详见 `prox-agent/replay-timestamp-order-evidence.txt`。另明确interval cache存在时Rclient或Rservice为0返回false，**不是零interval绕过**。
- **实际交付/归档**：真实修复 **`b6dad31/bfde8ca`** 已正常ff合main并 **push成功 `ed71d17..bfde8ca`**。三代理均停止写入；原Apple/SCP与设备数据不入git。本轮分析/两次单对象编译目录已实际无覆盖迁回主checkout **`x9/out/sensors-hid321/`、`x9/out/sensors-hid321-compile/`**，日志中的原worktree绝对路径保持构建时位置；从main复编只变更单元用 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 x9/out/sensors-hid321-compile/compile-hid.py`，新修改时用唯一scratch目录，不出探针。此最终交接正常提交合main/push后只清本轮 `.worktrees/sensors-hid321` 和 `fix/sensors-hid321`；其它工作树、长期输入、镜像、分区备份不动。**仍未生成含本轮HID/replay的新boot，最新正常已构建镜像仍r2/fb8657b**，无CI/测试/上机新结果。完整goal仍未完成，下一有决定性的证据是用户手动boot r2的新LIST/ENABLE/真实DATA日志；provider re-arm的顺序和最终自动效果仍待真实场景验收。

#### 2026-11-15 PROX 晚订阅/重开状态同步与正常全功能候选（本轮）

- **上一轮分类/当前基线**：上一goal轮实际交付HID ENOMEM与ON→ON replay `b6dad31/bfde8ca`，不是空转；最终交接 `4dd83b49` 已经 `49a041b3` 合入main/push，旧 `sensors-hid321` 树/分支实际清理。本轮启动主目录 tracked干净，未发现Mac `322/323/324.txt`，无新物理验证。新分支 **`fix/sensors-open321`** / `.worktrees/sensors-open321` 基于 **`49a041b3`**；复用三个代理做独立晚订阅审查，不重复LIST、计时器、r2产物检查。
- **新窄缺口与Apple证据**：不能假设首次单consumer（open→ENABLE）第一次DATA发生在订阅前；但已启用PROX后UserClient selector close只取消登记、不DISABLE，mode/valid保持，原poll仍消费sequence。再open/新clientopen及相同DetectionMode都没有replay，稳定near可能直到下一edge才主动送达。原20H392 plugin `initWithService 1354` 调 `selector0 13f8` 后只mapMemory/notification setup，activate亦无copyEvent；snapshot API是独立方法。BKfactory/wrapper初始化无copyEvent14；matching wrapper `servicesDidMatch 10006077c` 可重apply当前activeMode，但仍是同DetectionMode setter，不触发上一轮different-mode latch。纯UC reopen没有BK重新match保证。这是**明确provider/原Apple订阅流程的条件性缺口，不是321根因或实机已漏near的观察**。
- **真实修复**：X9Proximity覆盖已有4-param virtual `open(IOService*,IOOptionBits,void*,Action)`；先调用base，只有登记成功后在_hidGate内latch非零mode的fReplay，下一poll只重派仍valid真实snapshot，不改timestamp/sequence、不强制ENABLE、不伪FAR。base已在 `IOService.cpp:3507` unlockForArbitration后返回再取W，不引入自身仲裁→workloop反向。UC selector本来持W且open返回后才_state=Open，下一poll避开同步callback被丢；gate缺失/removed不改写已成功open的bool。OFF/无效sample/ENOMEM重试仍沿上一轮有据规则，不新增字段/基类vtable。
- **早入队/晚通知端口边界已闭合**：实际UC队列为IOHIDEventServiceQueue，不是裸IODataQueue；其 `setNotificationPort:229–234` 调super后若head!=tail立即sendDataAvailableNotification，UC gated注册走virtual调用。故新poll若早于plugin port安装而已入队，晚安装会补通知，不添加猜测delay。无send反馈/queue-full及旧T per-client interval风险、wrapper mode异步通知顺序仍保留，不将provider replay等同最终自动熄屏验收。
- **单对象编译**：主目录固定输入 `prepare-local.py --check`通过；实际diff独立review通过，worktree仅 `Builder.compile_one(SweetHID.cpo)` **exit0/True/15warnings**。exact脚本/日志 `x9/out/sensors-open321-compile/{compile-hid.py,single-compile.log,build.log}`，未link/打boot/运行测试/CI/设备或二次检查产物。静态证据 `x9/out/sensors-open321/{host-agent,apple-agent,prox-agent}/`，收尾无覆盖迁回main。下一动作在本真实源码上构建一次包含全部sensor修复的**正常功能r3**，输入保持ALS3＋PROX3／GPU280，不出探针或新插件；结果另记，当前还不能说r3已构建。
- **最新正常候选r3已实际构建**：在真实源码 **`36083f1`** 上，完整沿用r2的ALS3＋PROX3/GPU280输入 `boot-input.json`，执行 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --boot-config x9/out/x9-sensors-functional321-r3-20261115/boot-input.json --name x9-sensors-functional321-r3-20261115`，**完整kernel link＋boot pack exit0**。内置输出 **Image30928896 B / shim-boot14094336 B**。候选 `x9/out/x9-sensors-functional321-r3-20261115/boot.img`，包含r2之前的LIST/layout/控制/DATA修复、HID ENOMEM、ON→ON replay及本轮成功open rearm；无新增探针token、伪DATA、固件patch或plugin-package（GPU280保持，设备仍为321状态则无需mailbox）。构建源码基于49a041b3，后续同期main变化不在该候选，不拿集成HEAD替换source-head。
- **记录/验收边界**：上述目录保留 `source-head.txt/boot-input.json/full-build.log/build.log`；`gh run list --limit3 --json databaseId,status,conclusion,headSha`返回空，无CI run ID。没有测试脚本/套件、构建后二次解包/hash/反汇编/复建对比、设备/分区/mailbox操作。**r3取代r2作为最新正常双传感器候选**，r2与更早镜像全部保留；没有r3的实际LIST/ENABLE/真实lux/near-far、自动亮度或遮挡熄屏验收，完整goal仍未完成。用户手动临时 `fastboot boot x9/out/x9-sensors-functional321-r3-20261115/boot.img` 后保留新日志；首先检查 `X9 ALS` 的stage9 LIST LIGHT/PROX与ENABLE ACK，不越过最早失败，再验照亮/遮挡的真实DATA和自动效果。已有效near时关闭/重开订阅、或从passive/monitoring切active的稳定遮挡场景还需真实验收，不能用构建成功代替它们。
- **实际合入/归档收尾**：本轮真实open修复 **`36083f1`** 与构建交接 **`d7252694`** 已ff合main且 **push成功 `49a041b3..d7252694`**。三个代理均停止写入，`sensors-open321`静态分析、`sensors-open321-compile`单对象记录及最新正常 **`x9-sensors-functional321-r3-20261115`** 整目录均已实际无覆盖迁到主checkout `x9/out/`；其中source-head仍36083f1，不重写构建时绝对路径，不动原件/旧镜像/长期输入。此最终NOTES正常合main/push后仅移除本轮 `.worktrees/sensors-open321` 与 `fix/sensors-open321`；其他树不动。原Apple打开/queue通知证据与重放 `apple-agent/apple-open-snapshot-evidence.txt`、`host-agent/derived-open-actual-diff-and-port-review.txt`、`prox-agent/prox-subscription-evidence.txt`；精确source/candidate/build日志均已归档。没有新实机结果，完整goal未完成，下一决定性证据是用户boot上述r3后实际LIST/ENABLE/DATA/自动效果，不重复已闭合的静态fuzz类或造探针。

#### 相机 READY never-issued 双身份取消（2026-11-15，子代理代码闭合待集成/硬件验收）

- 新 `x9_camera_native_cancel.inc` 与 repeat `cam_repeat_cancel` 保存双身份：**PARKED→PREPARING前**在CAMSV/S1锁下核oldslot真实 RETIRED/RAW_IDLE/rawmatch，永久 snapshot old完整slot/FBC/DONE/version/gen/id/SOF/CPU boundary/sourceON receipt，另存new immutable request及 submitted/completed counters。newarm之后不再拿oldslot做rawmatch，也不将old DONE/capture_id重映射为new。
- core STOP仍先真实AP0100 OFF（current newREADY request仍ARMED/RAW_DEVICE、newFBC.request=currentnew，newDONE/SOF/countaccepted保持false），再capture.off。repeat关admission后只 READY且submitted未增/old实际spent receipt，真实OFF/ACK/FF、both FBC CNT0且RCNT/WCNT/reserved不变，才建立独立NO_CREDIT admission。已有sole IAR/EOI owner实际消费masked no-credit SOF/CHANNEL，再实际GIC PENDING/ACTIVE/worker空；不ICPENDR，不伪造DONE或把READY叫PARKED。
- NO_CREDIT只允许same当前newunused身份下reverse source/TG/receive controls（此时slot仍ARMED/RAW_DEVICE）。normal DMAhalt actual ACK+restore receipts、sourceOFF、controlsOFF、GIC/worker空、L19 ext/internal monitors/STAT与COMM33真实OSTD+idle两样本后，thread-only per-page cache CPU sync（**不读RAW字节、不delivery、不在hardIRQ/spin下cache/睡眠锁**）取消该never-issued request：**追加** `X9_CAMSV_CANCELLED` / `CAM_RAW_CANCELLED` / `CAM_REPEAT_CANCELLED`，保持旧enum值和RAWFRAME2 ABI。cancel后再核完整bus/source guard，任何失败terminal retain。
- `cam_repeat_cancel_complete_locked(epoch)`只在IRQ **close** 的实际GIC idle且DMAhalt之后调用；begin仍原EMPTY/RETIRED，不调用cancel，不放宽CANCELLED。普通 `x9_camera_camsv_arm` 在owner callback前拒CANCELLED，RAWdevice/finish也拒；仅fullphysicalclose归档+freshMM完整coldbank证明可重建所选软件slot，原取消backing继续pin。
- `cam_repeat_cancelled_check_locked(epoch,request,backing)`只短IRQspin+S1，能在control/IRQ/CAMSV已持场景调用，无CAMSV重锁、SMMU递归、ownercallback/MMIO/旧MM票检查。它独立核exact当前newcancel与old真实DONE/unused counters/slotCANCELLED/RAW_CANCELLED/no worker；是teardown/history scope，不是DMA/arm/cache grant。oldcreditretire/producer/fullSMMUretire/epochrecycle/reopen已接显式取消receipt分支，newFBC DONE/SOF/countaccepted仍false。capture代理必须同步LOCAL_CANCELLED_STOPPED→normalnative_smmu_check、FULL_CANCELLED_TRANSLATION_RETIRED→normalretirement_check两scope，不能拿新request冒旧DONE。
- 普通 Builder `compile_one(sweet_x9_camera.o)`最终 **exit0/146warnings**，`x9/out/native-smmu-compile/ready-cancel-fixed-compile.log`；首版同146warnings `ready-cancel-compile.log`保留。父只读review发现误替换同时插入begin已修；source代理只读review未发现额外确定bug，重点核APOFF currentnew身份、二次busguard无source递归、取消状态拒新arm与fullclose归档。未跑测试脚本/套件、二次产物检查、完整link/boot、CI或设备操作；**没有实际READYcancel/fullclose/reopen或真实iOS帧/预览结果**。partial PREPARING、RCNT已POSTED/uncertain仍非此取消路径；POSTED只实际DONE，其他错误保守retain。

#### 2026-11-15 相机最后 READY 取消作用域联合闭合，启用正常 mode2 功能候选（主集成）

- 底层 `bfcbf87→276fc68` 与 capture `66c94658→286f1158` 已集成：保留实际 old DONE 与 exact new never-issued cancellation 双身份；LOCAL_CANCELLED 只核仍有真实 MM 票的 live normal translation，FULL_CANCELLED 才核 restored prior。CANCELLED 不当 DONE/RAW_IDLE，不重放 credit，不虚置 graph_running；actual physical-close 后保存历史，fresh MM/cold bank 后才重建所选槽。source 对最终 SHA 只读窄复核未发现确定新增阻断，不等于设备取消或预览验收。
- 联合实际 tracked `sweet_x9_camera.o`（含主线程 init/physicalclose、全部底层、最终 capture 作用域）正常 Builder `compile_one=True`、**exit0/153warnings**，日志本树 `x9/out/camera-integration-final-cancel-compile/{single-compile.log,build.log,kernel-objects/}`。未运行测试套件/额外产物检查。主 checkout `prepare-local.py --check`成功；worktree 因共享 `.local` maintained-source 软链仍指main而报告stale，不是缺私有输入，未为单树更改共享链接。`gh run list --limit 3 --json databaseId,status,conclusion,headSha`为空，无CI run ID。
- 本树 boot-device 仅将 camera1 改为 **camera2**（其它token原样），admit 已返回实际 runtime 检查结果；这只许可 EL1/16KiB/当次合法 FDT/no-owner 的冷 sensor ID+actualclose 注册，READY 仍须两真实ID成功。下一动作合入构建前最新 main，然后一次正常完整 kernel/boot 构建，不出过程探针。新 bounded camera IRQ/CSI handoff 仍待首次实机；没有本轮新RAW、graph Start、PixelBufferHandler或屏幕证据，goal仍未完成。
- 约束仍保留：最多8 exported epoch lifetimes/backing永远pin，第9次ENOSPC在新mutation前拒绝；partial PREPARING、uncertain queue/setup保守retain。新 epoch 在fresh-MM attach前controller-thread创建失败可能过度retain，不能以清旧句柄假授权。没有设备/reboot/flash/分区/mailbox操作。
#### 2026-11-15 PS 极性与用户态队列契约静态收束（本轮）

- **范围/基线**：`fix/sensors-contract321` / `.worktrees/sensors-contract321` 基于 **`43dd4b26`**；用户明确授权多代理，三个代理分别核对原SCP数值链、service/session队列、原backboardd MIG mode调用线程。仅使用原SCP、原Apple输入和现有日志；不运行设备、测试脚本/套件、编译或构建产物复检。本轮只有 `sweet_x9_proximity.c` 的极性注释纠偏与本交接，没有行为补丁，也不为纯注释重建镜像。
- **PS数值链新证据**：正常producer `962b8` 把PS byte转float，`962c0` bytes `88e8` 经公共RV32 C.FSW字段核实为 `fa0→packet+0x10`。serializer `8a498/8a4b4` word `48955533` 经公共Zbs核实为 `bext a0,a0,s1`；mask `0x00fc0030/0x0e` 的bit6均为0，所以PS6实际走 `8a4ec→fcvt.w.s→8a4f4 wire+0xc`，**byte→float→wire没有取反**。正常producer的ops+1c非零分支 `96202` 明确写byte0，并在同分支实际调用原日志字符串 **`keep far status in high ir or strong lux`**（`14e321`）；不是孤立字符串命中。这是当前host `oneshot==0 → near` 假设的具体反向疑点，不能再用通用Android约定当作本设备证明。
- **极性限制/代码处理**：当前设备的driver binding、部分RV55 selector（normal `9519`、rear `02f9765b`）和driver状态字段write `11aedc` 仍未权威解码；不能仅由高反射直觉、字符串标签或诊断sysctl输出确定物理极性。源码仅将旧“Android binary PS convention”声明改为**当前未认证host假设**，保留 `latest.near=oneshot==0`、原DATA接纳/错误/OFF/replay行为；不猜测翻转、不修改SCP、校准或阈值。下一真实验收必须同时记录**原始oneshot与实际遮挡/移开关系**，不能只读host已解释的near。证据 **`x9/out/sensors-contract321/ps-agent/ps-polarity-evidence.txt`** 与 `normal-ps-serializer.txt`、`super-ps-serializer.txt`、`bext-public-decode.txt`、`ps-report-branch-and-payload.txt`；原payload地址不含分区头0x200，不把overlay bias用于main-text地址。
- **三条队列/调用线程已区分**：Foundation service getter将新建serial queue缓存于service+0x18，helper为原 `BSDispatchQueueCreateSerial`；IOKit独立创建 **`IOHIDSession - SessionEventDispatch`** serial queue/source（session+0x60/+0x68），ordinary event callback直接沿当前上下文进入Foundation及primary processor。原 **BKHIDSystem MIG 6000083 / 0x5b8dd3**（subsystem `1000dc9e8`、routine index83/entry `1000dd708`）经 `10008e438→10000e578→applySensorMode→sensorModeDidChange:` 同步通知；其server helper `100023ee8` 新建独立dispatch workloop，在 `100023f78` 传给 `dispatch_mach_create_f`，不是物理事件receiver queue。**processor+8锁只能保证互斥，不能保证observer reset先于异步property→kernel replay→event；下一poll不是完成barrier。**这闭合了此前queue身份疑点，但未证明实机竞态发生，更不是321 LIST timeout的观测根因；不加无据delay、重复replay或刷新旧timestamp。精确证据 **`consumer-agent/consumer-thread-evidence.txt`、`mig-mode-thread-slices.txt`、`mig-mode-table-proof.txt`** 与 **`queue-agent/queue-identity-order-evidence.txt`**（后者单独报告的caller未知限制由前者补齐，勿将局部报告当作组合最终结论）。
- **原cache来源纠偏/ALS单点闭合**：发现旧 `/private/tmp/sysos20H392` mount确来自Helio输入，该采样不作为本轮权威证据；代理以固定 **`.local/apple/ios16-20H392/SystemOS.dmg`** 新只读挂载重核12B支岛，实证 `1c2d1d24c→18dd49968 BSDispatchQueueCreateSerial`、`1c2d1d4c8→18e59e820 IOHIDEventSystemSetCallback`、`1c2d1d498→18e59e574 IOHIDEventSystemOpen`、`1c2d1d348→186e6102c CFRunLoopGetMain`，不是uniform-delta猜测。额外单ALS支岛 **`1afb95b70→18e55f484 _IOHIDServiceClientSetProperty`** 已由原字节解析；只解决callee名称，不证明真实0/超界ReportInterval producer，不新增猜测补丁。新挂载自身 `/dev/disk8` 已detach，`consumer-agent/local-sysos-detach.txt` 为 `"disk8" ejected.`，主线程核对mount表无本树挂载；旧外部mount未动。原cache/Apple/固件/设备数据均不入git。
- **最新镜像/实机边界**：最新正常双传感器候选仍 **`x9/out/x9-sensors-functional321-r3-20261115/boot.img`**，源码 **`36083f186609f01b13f151e1e146d06f08b5da33`**、ALS3＋PROX3/GPU280，完整build此前exit0，Image30928896 B/boot14094336 B。本轮再次只读 `~/321.txt:723–737/823/846`，仍READY、SHARE_MEMORY seq1、TIMESYNC seq2成功，LIST **stage9 errno60 / records0**，PROX跟随transport失败；未发现 `322…329.txt`。r3没有上机验收，**不能称321修好、真实lux/near-far或自动亮度/遮挡熄屏成功**；无本轮CI run、构建/测试/新boot/plugin-package/mailbox。同期main已进展至 `18a469da`（call/SMS），这些变化不在r3源码，不将集成HEAD替换source-head。
- **重放/下一步**：所有本轮证据在gitignored **`x9/out/sensors-contract321/`**，收尾无覆盖迁回主checkout；报告保留原采样路径。主目录可用 `python3 x9/out/sensors-calibration321/isa-agent/rv55_slice_v2.py x9/out/scp-list-static321/scp.bin 0x9616a 0x962c2 > x9/out/<唯一名>/producer.txt` 重读原producer（先建唯一scratch目录），公共BEXT/C.FSW字段按PS报告独立核对。queue脚本使用固定 `.local` 与主目录既有切片；cache重放在新只读mount后运行 `consumer-agent/cache-static.py`，按attach plist detach本次新device，不照搬旧disk编号。设备验证仅由用户手动临时 **`fastboot boot x9/out/x9-sensors-functional321-r3-20261115/boot.img`**：先LIST/ENABLE最早阶段，再真实lux和raw oneshot/遮挡关系，最后原生自动效果及mode/open稳定level场景；设备仍GPU280则无需mailbox。完整goal未完成，不重复已排除路径或出探针。
- **实际提交/归档收尾**：注释与静态交接 **`2b1793df`** 经 **`48851038`** 正常合入同期main `18a469da`，已ff合main且 **push成功 `18a469da..48851038`**；没有改写其它功能。三个代理均已完成并停止写入，mount表确认本树无挂载后，整个分析目录已**实际无覆盖迁回主checkout `x9/out/sensors-contract321/`**，原采样路径/attach/detach记录保留；最新r3及旧镜像均未改动。此最终交接正常提交合main/push后只移除本轮 `.worktrees/sensors-contract321` 和 `fix/sensors-contract321`，其它树、`.local`长期输入、原件与分区备份不动。本轮是新数值/队列证据与未认证注释纠偏，**不是额外功能修复/新镜像或物理验收**；完整goal仍未完成，下一决定性输入是用户手动boot r3的新日志。

### 5.14 2026-11-15 蓝牙完整候选整合 — 当前继续中

- 保留前文蓝牙§5.12与main传感器§5.12历史记录，当前蓝牙handoff写本节。`de7ae79`将main `3b6717a`合入蓝牙功能树，保留同期call/双传感器/GPU，未把未验收蓝牙合入main；NOTES只合并双方记录，recipe/link-order正常合并。本次merge不改变已有XNU patches，隔离树输入仍匹配。
- **严格XNU base build已完成**：`x9/xnu/build-xnu.sh macos --isolated-existing x9/out/bluetooth-native/xnu-lifecycle-20261115/dxb` exit0，日志 `x9/out/bluetooth-native/xnu-lifecycle-20261115-strict-build.log:2227`真实LD，后续DSYM/CTF/INSTALL全部成功，make/tee返回值实际0。先前缺python的非严格日志保留，不用早期ready作成功证据。对象树/私有config在同目录dxb/与local.json，共享.local/XNU不动。
- **原生close生命周期已闭合源码**：`d0648b0`前置原已知RX stopflag→READ-only cancel ioctl→unchanged nativeclose，四组原stop/exit handshake指令preflight；原RX stop路径只置exited/cond_signal后返回，不递归close/销毁transport。cancel失败恢复stop并返回真实nativefailure不进入可能挂的close。serial先前READ-only cancel实现与此对接，后续又修select取消可读及空TX队列仍核boot错误，防worker故障清队列导致TIOCDRAIN假成功。无HCI/硬件状态副作用。
- **完整overlay kernel首次link已成功**：`python3 -I x9/build/build.py kernel --config x9/out/bluetooth-native/xnu-lifecycle-20261115/local.json --boot-config x9/build/boot-device.json --boot-arg x9_bt_init=1 --boot-arg x9_bt_fc=1 --name bluetooth-native/complete-kernel-20261115 -j 8` exit0（含READ-only cancel，但早于后续select/drain修复），日志 `x9/out/bluetooth-native/complete-kernel-20261115-console.log`；103/103 overlay objects编译（3 prebuilt），与完整固定tag+patches XNU tree正式link+strip。产物同名目录 `kernel.development.sweet`，不是当前最终boot。**同目录重跑失败**在audio/build-microphone.py“output bundle already exists”，日志 `complete-kernel-20261115-final-console.log`；没有把此次失败说成功，后续完整候选必须全新输出目录，不能反复覆盖生成器bundle。
- 当前userland+单对象组合 `power-close-20261115-final`日志在本树相应compile.log，结果另补；其启动早于serial select/drain末改动，不能覆盖该末改动。正式全候选入口正在添加：同一次orchestration生成完整kernel、4 binaries、2 Program-only jobs、原件分离root payload与一次boot.img；built_complete与runtime_verified分开。尚无boot镜像、未安装root/重启/flash/写分区；所有检查仅编译/构建日志，不跑测试套件或二次分析新产物。Android蓝牙保持开启。真实XNU SSPM/profile/vendor ACK/ownership、native COW/init、扫描/配对/BLE/A2DP/HFP/SCO仍未实机验收；目标仍active。
- **完整初始化/启动整合候选现已实际构建成功（覆盖上条“尚无boot”）**：源码 `1acedab9de3863737e790263870c0e1203711230`；`python3 -I x9/bluetooth/candidate.py --name bt-native-full-20261115 --config x9/out/bluetooth-native/xnu-lifecycle-20261115/local.json --boot-config x9/build/boot-device.json --boot-arg x9_bt_init=1 --boot-arg x9_bt_fc=1 --launchd-source x9/out/bluetooth-native/launchd20h392-input --jobs 8` exit0。`x9/out/bt-native-full-20261115/candidate-build.log:13522` 103/103 overlay编译（3 prebuilt）/正式link+strip，kernel、四件userland/two jobs、root payload、boot四阶段均exit0；主console副本 `x9/out/bluetooth-native/full-candidate-20261115-console.log`。内置packer报告 Image31772672 B、boot14888960 B，低于20MiB guard，未override，未二次检查新镜像。产物 `x9/out/bt-native-full-20261115/{boot/boot.img,kernel/kernel.development.sweet,runtime/,root-payload/root-payload.json,inputs/,manifest.json}`；这是**完整构建候选，不是全部功能验收镜像**。manifest built_complete/candidate_ready=true，但 runtime_verified/stageable/root_installed=false。boot不安装root桥/jobs；未设备boot/stage/写入，root步骤仍需逐项用户确认。
- **音频真实原件新缺口，旧候选不含后续SCO修复**：原20H392 type0 support table全0；`"shci"`→source2→vtable+a78→`100267008`→ID1 admission返回2，`1002844bc`不写路由状态，上层未检查该返回继续，尚未证明阻断初始启动。ID1实际签名是 `_BCM_WriteVoiceConfig(VSCVoiceRouting,CB)`，不是仅凭名字就能迁的PCM寄存器操作；ID8是WBS(enable,handle,CB)，ID3e是SCO buffer控制，全部缺真实MTK等价映射。Android实际FC72 `01 72 fc 04 49 00 80 00` 已在17条vendor序列中，不是标准0c26，也不证明HCI/I2S路线；`libbt-vendor.so` op2 SCO_CFG仅log→0、op8 SET_AUDIO_STATE unknown→-1，不能当route成功证据。原件解码保留 `x9/out/bluetooth-native/ios-power-handoff/`，不移植BCM私有HCI、不重复补发FC72、不fake success。
- **活动Android SCO wire已证并移植**：已有supplement-2 `modules-original.txt`确证 `btmtk_uart_unify Live/ref2`；同次1067720 B模块原件的TX hook注册链 `cap_init8a68→common_register18d0c→bdev16850` 未被6653/66xx注册覆盖，`send_frameb8a4/b8cc→txcommon1825c/183e8..18438`对所有type3转换，无VTS/runtime开关。RX rodata16218（type3/hlen3/loff2/lsize1/maxlen255）→recv_sco3854→rxworkc864..c8ac反向转换。wire为 `03 AA (LL+2) (LL+1) BB payload`，native为 `03 AA BB LL payload`。worker已在唯一UART边界编码/解码、保留high handle/status、RX双length关系与整帧严格校验；pending/serial均canonical native；TX仅整wire成功+真实drain后finish原native长度，不把额外1 B算进FIFO退队。payload251为原声明buffer预算下安全上限，Linux skb分配会round-up，**不是实测LL252/253必拒收**；控制器原始SCO244覆盖该范围，254/255回绕明确拒绝。只读review通过，原件解码/脚本在 `x9/out/bluetooth-native/android-audio-original-handoff/`。runtime fatal清理现在打印真实hardware_stop errno，不冒充FW retained时恢复干净baseline。此源码尚未重新完整链接/打包；scan/pair/BLE/audio验收均未做，目标active。

- **SCO移植当前源码编译**：按正常Builder `compile_one` 使用隔离private local.json顺序编译 `x9_bt_serial.o`、`x9_bluetooth.o`，最后 `sco-wire-20261115-final-v2` exit0（含runtime fatal真实cleanup日志）；`x9/out/bluetooth-native/sco-wire-20261115-final-compile-v2.log` / 同名前缀`final-v2-objects/{build.log,kernel-objects/}`，仅既有warnings。candidate/build/package三件Python源码编译通过，新增显式audio_verified=false门禁。不跑套件、不二次分析新产物、不改设备；上条旧完整候选仍不含此次修复。首个final脚本shell使用zsh只读status变量失败，日志保留，不拿该shell exit1当成功。

- **本轮源码合并/推送**：SCO与候选交接提交 `fa26c0e`，其push结果以实际工具返回为准；随后合并main `8bc5670`，保留最新call/SMS/prox cadence修复。只有NOTES冲突，双方记录保留，当前蓝牙§5.14置末。main这段未改XNU patches，隔离base不需再重建；未把蓝牙反合main。

- **SCO修复+最新main完整候选已实构建成功**：源码 `e7bf442`（蓝牙SCO `fa26c0e` + main `8bc5670`）；重放同上candidate命令，只改 `--name bt-native-sco-full-20261115`。command exit0；`x9/out/bt-native-sco-full-20261115/candidate-build.log:13531` 103/103 overlay（3 prebuilt）/正式link+strip，:13536/13541/13544/13591 kernel/userland-jobs/root-payload/boot四阶段exit0；:13587内置Image31789056 B、boot14893056 B，低于20MiB，不override、不二次检查新镜像。主日志副本 `x9/out/bluetooth-native/sco-full-candidate-20261115-console.log`。当前完整初始化/启动整合候选为本蓝牙树 `x9/out/bt-native-sco-full-20261115/boot/boot.img`，配套同目录runtime与root-payload；旧 `bt-native-full-20261115` 不含SCO，不当最新使用。audio_verified/runtime_verified=false，built_complete仅构建成功，不是完整功能验收。未root安装/boot/flash/写设备；已向用户明确询问四binary+两Program-only job专用iOS根盘安装许可，尚待回答，不默认已有许可。
- **原iOS私有音频unsupported不会自动等同死锁**：ID3e startup `3c88e4→25a8b8` type0 false，直接跳 `3c8978`，未提交不等CB；XPC `501764`同gate→`50183c/6534d0` not supported直接return。HFP WBS setup `3fd4f8→281e98` type0 false→`3fd520`继续标准流程，teardown同gate，正常HFP不发ID8；sample-rate/mic `408c68/40a60c`若走ID8则丢return继续。shci也丢route返回继续，但controller路由状态未成功更新。未证明这些缺私有CB导致wait，亦未证明实际音频route工作；原件解码追加到ios-power-handoff。硬件代理窄只读运行核对现Android audio.service-aidl.mediatek的maps真正加载bluetooth.audio-impl/session两件依赖，正在追可达provider，不根据名字猜HFP能力；未触蓝牙状态/连接/HCI。
- `fa26c0e` 已实际push origin蓝牙分支成功（9698016..fa26c0e）；`gh run list --limit 3`返回空，本轮完整构建是本地正式构建，无CI run ID。功能分支保留，main未操作。
- **专用root安装执行器已合入，未执行**：`63e2f0a`（子代理原b57a136）新增 `x9/bluetooth/stage.py` / 操作参考STAGING.md；`c176dba`（原5fdf1f2）补持久化与空间边界。默认离线plan无ADB；apply/rollback要求真实用户许可后显式确认参数+serial+私有state。读取当前live base||extension并逐part原rawhash核，私有APFSclone→macOS原生HFS处理journal/catalog→先四binary再两Program-only job→detach后最多128MiB blockdiff，复用pinfile rawwriteat旧SHAcompare/fsync/readback；每写重核pin/inode/FIEMAP/动态userdata路由，写前/后全root preflight与全hash。root SystemVersion20H392/bluetoothd原SHA+UUID/两原jobs与BlueTool原bytes/四absent目标在任何设备写之前全部核对。blocks.old/new、目录项、snapshot、updated与冻结manifest先fsync，ledger再发布；保守本机空间至少totalroot+3×128MiB+16MiB。失败先等在途pinfile结束，再只对精确old/new4KiB块compare回滚；未知/torn/concurrent块拒绝盲写，rollback-incomplete不冒充成功。安装只写专用iOS根盘范围，receipt固定audio/runtime=false；不改构建manifest、不动boot/modem/persist/备份、不flash/reboot。只源码compile/diffcheck，**未执行plan/apply/rollback、未安装、无安装验收**，用户许可仍未回复。
- **已为安装准备仓库内Apple原始输入副本**：仅本机 `cp -p` 从既有20H392原件挂载 `/private/tmp/x9root/usr/sbin/{bluetoothd,BlueTool}` 到本蓝牙树 `x9/out/bluetooth-native/apple-originals20h392-stage/`，未碰挂载root内容/设备，不入git。后续安装输入可取 `--candidate x9/out/bt-native-sco-full-20261115 --payload x9/out/bt-native-sco-full-20261115/root-payload --root-manifest .local/x9/root-extension/20261110-16g/root-manifest.json --apple-originals x9/out/bluetooth-native/apple-originals20h392-stage`；真实许可后才执行写模式，stage本身还会核对currentlive原件，不拿本机原root克隆当设备baseline。当前候选固定e7bf442，后来安装器/docs未改变该内核或用户态C源码，不因工具文档重复构建镜像。

- **音频原件最后窄链收束（不猜vendor参数）**：stock iOS `conn+0x116` 是linkType的HDR flag，`0f8250`先清零，仅F0/F1置一；普通SCO0/eSCO2走 `1055e0→ID1a→0428` / accept `ID1c→0429`，descriptor无标准043d/043e，legacy参数没有DataPath。profile+12非零提供VS，否则10089c读84f528；0ff2f8/default67c6fc/844868的真实值0060，10136c以原step/status gate提交标准0c26，其callback10154c更新全局，不能把VS0060当HCI路由证据。SCO readbuffer解析 `ID93/1005→typeA112268→113ca8→10c4a8→11fa44` 保留真实244/4并开启SCO admission；标准H4发送经11e73c→10a478→0e6b58→685cc→0e6938→67610→661134 write，无须新Skywalk桥。extended SCO标志84f96a仅ID3e成功callback设置，type0正常gate跳过；同offset的85596a是另一global不能混淆。全部37件原件decoded已在 `x9/out/bluetooth-native/ios-power-handoff/`，重放reader：`python3 x9/out/bluetooth-native/read_original.py dis 1000f7e1c` / `dis 100105364` / `dis 10010125c`。仍需真Synchronous Connection Complete、双向SCO及声音；不fake ID1/WBS成功、不向0428猜塞043D参数。
- **Android当前audio依赖已窄追至标准043D，非iOS验收**：真实audio.service-aidl.mediatek maps加载audio-impl/session（bluetoothHAL maps不加载），当前com.android.bluetooth maps加载19080632 B libbluetooth_jni.so，原.gnu_debugdata解出3381528 B mini-ELF恢复真实函数符号。确切原件路径调用读 `/vendor/etc/aidl/hfp/hfp_codec_capabilities.xml`（3499B）：CVSD_IO16000、MSBC_IO32000、全部dataPath1/useControllerCodec=true；LC3_IO64000原codec实际MSBC，不擅自更正，也不当currentactive session。窄config链 node+14/+18/+1c→create625374/37c→params+34/+35→BTM_SetEScoMode b36620/default11fe648→BTM_CreateSco b32700→b32f54 btm_send b332f0→b33ae0→serializer ba9cc0，opcode043D/plen59，两个dataPath落payload50/51，尾部标准btu_hcif_send_cmd，无该分支MTKvendoropcode。选择仍依赖runtime/managed/offload/property，**未观察到当前HFP session/043D实际执行，不可映射成iOS legacy0428/shci route1/WBS success**。3件原ELF+XML/maps/minidebugdata/精确decoded及重放reader保留 `x9/out/bluetooth-native/android-audio-handoff/current-audio/`，不入git。ADB仅沿确认活进程maps/原件调用链只读，未HCI、连接/reset、chmod/write，蓝牙状态保持。
- **收尾状态/下一次从这里继续**：安装器+README/交接 `c202888` 已push origin `feat/bluetooth-native`成功（edf7705..c202888）。当前最新完整boot仍为源码e7bf442的 `x9/out/bt-native-sco-full-20261115/boot/boot.img`，配套runtime/root-payload已构建，不是全部功能验收；未root安装，仍待用户对此次四binary+两Program-only job专用iOS根盘安装明确许可。随后执行安全stage与真实receipt核，用户手动boot并保存新原始日志；先查SSPM/profile/vendor所有ACK/DONE、kern.x9_bt_boot=0/power0、native injection/H4open/capabilities，再扫描→配对→BLE→A2DP→HFP/SCO。Audio vendor真实映射、活动连接suspend/FWwake callback与实机吞吐尚未完成；closed-transport-only不可包装为完整system suspend。目标保持active，不合未验收蓝牙进main，不自行boot/flash/写设备。

- **继续整合：两处确定原生协议缺陷已修**：子代理 `093717b` 合为 `16d085a8`，纠正扫描 lower setter：`19a7e8` 实际写 `85a646`（standard extended scan），`19a83c` 写 `85a645`（Apple new VSC scan）。现前者取真实 ext_scan、后者0；原 provider slot1b8/1c8 与此一致，原 `199fac/19abc4` 才走标准2041/2042，避免此前选入已清空的vendor ops返回0xe11。子代理 `979a2fb` 合为 `fc759aaf`，原 `10b9ac:11001009→2a0003e9` 去掉 HostBufferSize ACL payload 多声明的4B。原H4 RX固定对象0x804、包预算2032含ACL头，实际payload2028；serial_open的4096是TX staging容量，不是RX容量。0c33 descriptor5e格式2122原样marshal，completion真实保留status，无需额外callback hook。两子代理正常编译exit0，未套件/设备/新镜像；原件decoded及日志保留本树 `x9/out/bluetooth-native/ios-power-handoff/` 与扫描代理原目录。
- **新的准确门禁（旧候选不能当最终）**：原默认HCI config `67684e` 的+7/+8/+9为0，现已证启动注册 `109724` caller `f35f4` 传NULL不覆盖；0c31这条路径发送0，不能说降低0c33已保证controller只回2028以下。正在窄审原生ACL credits/HostCompletedPackets与现成flowpolicy闭环，不猜扩大parser或开启未接通的flow。扫描代理另核2041 variablemarshaller长度与实际TX收尾。旧 `bt-native-sco-full-20261115` 不含上述修复，暂不重复生成候选。
- **launchd实际缓存前门已证，安装器待同步补齐**：20H392 normal启动 `rd=ufsroot0` 非restore，`19de4` mustcache=true；`19c08→19f68→1a064/41340` 将缓存LaunchDaemons入registry，`011a34→195f4→197f8/413ac` 按job路径取cached字典到definition+50，`02b4fc/02b588` 使用它，不按独立plist mtime覆盖。因此只改两份LaunchDaemons文件不足。子代理正实现仅追加两Program/job父容器的binary-plist cache派生，保留其它原objects/offsets（Siri原same-length guard依赖它），并加入第7文件 `/System/Library/xpc/launchd.plist` 精确比较/备份。Siri size gate只会接纳原大小与有据派生几何，不宽放未知cache。root修改仍未获本次明确许可；后续确认范围必须含该cache。
- **同期main整合 `6c0db6b`**：正常合入实际main `be9610f6`，保留最新call/SMS/handover及HID/PROX修复；仅NOTES冲突，双方完整保留，活动蓝牙交接仍在末尾。本段main未改XNU patches，隔离base不需重建；最终新候选会正常重编全overlay。DYLD原AMFI0x5a/0xff允许INSERT的链与mobile501 devfs0600、MAC vnode open/ioctl默认非NULL hook检查均已源代码/原件核定，仍无实际注入/open成功日志。目标active，不反合未验收蓝牙进main，不自行root写入/boot。
- **权威launchd cache源码修复已整合**：`ab028785`（子代理原 `f7aeb08`）新增stdlib append-only helper及build/candidate/package/stage第七cache文件。原几何 `796447;4/2/22269/0/707339`，候选有据内存构造几何 `798777;4/2/22275/22269/709645`；只追加4dict+2Program strings，完整原object region/IDs/offsets保留，派生字典只有两Program变更。`45d198b` 内核Siri只允许这两精确size及32Btrailer几何，仍分类全部32原string guards才写；原read helper拒shortread，日志记录实际size。正常 `kern_exec.o` 单对象compile exit0/143 warnings（日志 `x9/out/bluetooth-native/launch-cache-kernel-20261115/compile.log`），五Python源码已由代理compile，无suite/新候选/设备。cache原件副本仅本机cp至既有 `x9/out/bluetooth-native/launchd20h392-input/launchd.plist`，原挂载root不变；build必须新增 `--launchd-cache-source` 同路径，stage必须精确live比较不拿这份副本当live成功证据。原件DYLD/launchd/access证据在本树 `x9/out/bt-launchd-access-proof.txt` 等。
- **BLE length疑点已明确排除**：原2041实际cursor为3+5n payload；`115c9c` queue丢临时4B H4头，`685cc` 重建真正plen，单PHY实际wire12/params8。**不要改 `10db04 ADD#4`**。200d/2043标准queue/SupportedCommands门禁与LE01/0a/0d原eventdecoder已静态闭合，无新确定阻断；证据/原件/compile在 `x9/out/bluetooth-native/ble-native-handoff/`。配对SSP/SC门禁正独立窄核，仍非扫描配对实机验收。
- **当前Android仅只读核状态**：本机 `adb get-state` 返回device，`adb shell settings get global bluetooth_on` 返回1；未变更设置/连接/发HCI/安装/写根盘，serial不输出入git。此前 `2a2cc8a` 交接及两修复已实际push成功 `b9387a38..2a2cc8ae`；gh run list为空，无CI run ID。等待ACL flow完整原生credit/error闭环源码，确认全部改动后再全新名字一次完整构建，旧候选不能作最终使用。
- **标准host ACL流控源码已合**：`717022fa`（子代理原 `a294c885`）在原HCI Init注册前clone完整12B config，只开BR/LE ACL归还（+7/+9=1），SCO+8=0；保持原0c33 payload2028/ACL count1，按iOS真实SupportedCommands核0c31/33/35，只有真实0c31 status0且config pointer/fields匹配才置active。六entry hooks复用原ID5f/特殊marshaller/command-credit bypass，保留blocked/count门禁、成功enqueue才清tally；活BR/LE零长度ACL补credit，unknown/disconnected继续原drop。enqueue、async transport、标准0e/0f的真实0c35错误均转换/保留真实status并走原0902c0 native fatal，不fake Complete、不加pending/timer、不重发可能上线的credit。32处只读instruction contracts+12B默认config+3descriptor prefixes均在COW/text修改前核，42项/容量44、tramp/icache512B。父复读原config/credit/event归档后整合；子正常userland compile exit0，证据在 `x9/out/bluetooth-native/ios-power-handoff/host-acl-flow/`，仍非实机流控验收。
- **本轮同期main与最终硬件窄复核**：`efe01feb` 合入main `30a0a313`，保留call/SMS identity/receipt及PROX晚open/replay与未认证极性注释；仅NOTES冲突，两边记录完整保留且本节置末，无XNU patches变化，不重建隔离base。三代理并行只读复核初始化、iOS flow及完整构建入口。硬件代理沿真实SSPM/UARTHUB请求/时钟、共享供电、Android absent/no-op ext32k、115200 query→24M/fc1→XO→下载器/171B connfem ACK→当前208M/dev0/CMM纯读→routing/feedback→ownership/function-on→production vendor→DONE/READY，未发现新确定缺口；closed/empty-only sleep/resume与hidden-bank retention/active-link/system suspend/FW wake未验收边界不扩大。原配对0c56/0c7a与标准请求回复链已窄核，无新的确定修补项，UI/key persistence仍待设备。
- **最后窄review与构建冻结**：iOS代理确认原全局pthread mutex对象84b020真实覆盖Init→vendorinit与RX→packet/ACL/0c31/Reset，以及timerdispatch（88ea0/88f90 lock/unlock，Init08906c…089118，RX67ad0…67ae4，timer89424…8942c），不是猜单线程；plain active及同tally/submit在已确认native路径串行，无凭空atomic/新锁。ABI/config pointer lifetime/六prologue及0c35错误metadata/0e/0f parser均复核，无新确定缺口。构建代理确认隔离local.json/boot-device的x9_bt=1与附加init1/fc1、四阶段、七文件cache及flags一致，无确定构建阻断；这不替代即将运行的正常source-preflight或设备验收。README已从“flow仍整合”改为实际实现，Vendor注释明确协商不等于controller遵守。下一步全新 `bt-native-cache-flow-full-20261115` 一次正常candidate（完整命令见README），不跑套件、不二次分析新产物；本次root安装仍未获得用户明确许可，范围须包含四binary、两独立jobs和权威cache七文件。
- **最新完整cache+flow候选已实际构建成功，覆盖前面旧候选状态**：冻结源码 **`4c855d0a4f7021c5b6c4071f93ba697046338930`**（含717022fa及main30a0a313）；按README完整candidate命令，name **`bt-native-cache-flow-full-20261115`**，显式隔离local.json/完整boot-device/init1/fc1/两原jobs+cache，jobs8。父命令实际 **exit0**；主console `x9/out/bluetooth-native/cache-flow-full-candidate-20261115-console.log`，内建日志 `x9/out/bt-native-cache-flow-full-20261115/candidate-build.log`：:13540全103/103 overlay（3 prebuilt），:13545/:13550/:13553/:13600全kernel/userland-jobs-cache/root-payload/boot四阶段 **exit0**；source-only隔离preflight在正常编排内成功，正式link+strip完成。:13596内置packer **Image31789056 B、boot14905344 B**，保留20MiB guard、不override；未额外hash/解包/反汇编新产物、未跑测试套件。产物本功能树 `x9/out/bt-native-cache-flow-full-20261115/{boot/boot.img,kernel/kernel.development.sweet,runtime/,root-payload/root-payload.json,inputs/,manifest.json}`。新候选包含scan gates、HostBufferSize/host ACL flow、权威cache/Siri及先前SCO wire全部修复；旧bt-native-sco-full/原bt-native-full保留但不当最新交付。
- **最新候选验收边界与下一步**：这是完整**编译打包候选**，不是蓝牙全部功能通过；candidate ready/built complete只描述构建，runtime/audio/stageable/root installed仍false。未设备boot/flash/reboot/root apply/rollback/安装plan；本轮本地正式构建，无CI run ID。**本次七文件根盘写许可仍未得到明确答复**，不得当作以前允许userdata的授权：四binary（btctl、X9H4Transport.dylib、X9BluetoothLaunch、X9BlueTool）、两独立Program-only jobs、`/System/Library/xpc/launchd.plist`及必要HFS元数据，须先确认才安全stage（输入APPLE原件路径/私有root manifest沿前文，candidate改为最新名字）。Android保持运行直至安装/恢复真实receipt成功，用户随后手动boot本功能树新boot并保存新原日志；先验SSPM/profile/vendor所有ACK/DONE、boot0/power0、原件注入/H4/caps与实际0c33/31/35/ACL最大RX，再scan→pair/UI/key persistence→BLE→A2DP→HFP/SCO。真实音频route/WBS vendor等价、active-link suspend/FW wake及本次iOS所有场景仍未完成，不能把传输实现称为这些验收。目标active，不反合未验收蓝牙进main。
- **本轮实际push**：冻结提交及flow/main整合已成功推至origin `feat/bluetooth-native`，范围 `e85f1f52..4c855d0a`。下一个纯交接提交不改变本候选source-head，不能把交接HEAD冒充镜像源码或因纯文档重复构建；完工前保留功能树、所有旧候选/原件、`.local`及分区备份。
- **当前停止推进条件（等明确用户确认）**：交接 `6231a844` 已实际push origin成功 `4c855d0a..6231a844`；工作树tracked干净、三代理均terminal completed，完整candidate构建进程已exit0，不存在待完成构建/代理任务。后续继续轮只读复核ADB仍device及安装器的明确许可门禁，用户尚未回复本次七文件/HFS元数据root写确认，无新iOS运行证据；不能把自动“continue goal”当作破坏性写许可，也不重复镜像/原件审查冒充进展。目标未完成，当前blocked等待该明确确认；得到确认后恢复安全stage→真实receipt→用户手动boot→按前述全场景验收。当前候选source-head仍4c855d0a，本段纯交接不修改镜像/用户态或设备，不反合main、不删除功能树/产物/备份。

#### 2026-11-15 蓝牙恢复推进：用户允许定点写，禁止全盘读取/备份

- **当前许可覆盖前面的阻塞记录**：用户明确回复“允许 不要进行全盘读/备份操作”。仅安装四自有 binary、两 Program-only jobs、权威 launchd cache 七文件及必要 HFS 元数据；不执行旧 stage.py 的 whole-root snapshot/clone/rawhash，不新增完整 root/分区备份，不自行 boot/flash/reboot。此许可并非扫描/配对/BLE/音频已通过。
- **最新完整候选不重建**：仍使用本功能树 `x9/out/bt-native-cache-flow-full-20261115/boot/boot.img`，冻结镜像源码4c855d0a；后面的安装器/文档提交不变更该镜像。103/103 overlay、四阶段exit0及内置Image31789056B/boot14905344B结果沿前段，不重复hash/解包/反汇编、不跑suite/新CI。
- **已实际执行 metadata-only preflight**：私有 `x9/out/bt-root-targeted-preflight-20261115/preflight.log` exit0；只读root first64KiB，grown几何34359738368B、device/pin/immutable/FIEMAP/route/UUID/extension保护通过；BBFW仅metadata，payload SHA明确未验证。没有设备写/安装/mount/boot。源码6f607e73（子01212d3）新增显式mutable-root限定模式，默认行为不变。
- **定点安装器源码**：e4221a91（子13baf7c）新增私有macFUSE单rawfile RPC；696e9a6f（子3002f23c）新增bounded HFSX BinaryCompare节点lookup/exact overflow与七record UID/GID-only修正；2745e5c6（子37dccbed）新增真实clean internal journal checksum/start==end有界检查。新stage_targeted.py不创建wholeimage，只缓存实际必要4KiB块，所有native HFS writes先localCOW；detach后修owner，冻结readset+净变化块，重验读依赖才compare-write。所有原件/log/cache/前像在gitignored x9/out，serial不入git。
- **三代理窄审修复**：partial attach按原生hdiutil info的精确image-path追踪；native detach失败真实保持前台RPC/COW而不raise退出；FUSE卸载未证成功前不close。原hdiutil mountOptions拼法无据，改有据-nomount/-noautofsck后diskutil mount noatime,noowners，再核真实MountPoint。existing三目标cp-p保xattrs/ACL/resourcefork/flags，截断替换后定点比较。512MiB正常读费用含pinfile内部OLD/NEW；独立恢复额度、冻结后禁止新hydrate、完整readset/ledger/hash/OLD绑定、回滚复验全部必要读依赖。源码compile()通过，无测试套件；实际macFUSE/HFS挂载和安装结果尚待正常工具执行，不能把本段当安装receipt。
#### 2026-11-15 r3 实机验收阻塞审计（已 blocked，目标未完成）

- **审计1/分类**：上一轮 `2b1793df` 的PS数值与MIG队列证据改变后续验证重点，归类为progress；本轮只重核当前状态，归类为**no progress**，不把重复交接当作功能进展。主main=`30a0a313`、tracked干净；三个sensor代理工具状态均completed，没有活跃构建/设备采样句柄可供verified wait。按用户已约定的Mac数字日志目录被动查看最新文件，最大编号仍 **321**（1781932 B，mtime_ns=1794744150821963383）；没有r3运行证据。未重新读取/修改镜像或重跑LIST/ISA/queue已收束路径，没有行为补丁/编译/测试/设备操作。
- **同一阻塞条件/待输入**：已交付正常r3，但原始321只到LIST stage9 errno60/records0，不能证明修复已执行、更不能校验DATA物理极性/自动效果。当前没有新的可确认缺陷或独立证据支持继续改行为；不能以猜测翻转near、重复replay/delay、刷新时间或新探针替代实机验证。需要用户手动临时boot **`x9/out/x9-sensors-functional321-r3-20261115/boot.img`** 后提供新原始日志，先LIST/ENABLE，成功后才验lux、raw oneshot与遮挡关系及最终原生效果。连续三次无进展阻塞审计后，goal已由工具实际标记 **blocked**，**不是complete或用户请求paused**。临时交接树 **`.worktrees/sensors-verification321` / `fix/sensors-verification321`** 仅此NOTES待收尾；不动其它工作树/输入/旧候选。
- **审计2/无进展复核**：再次读取main和本交接树状态，main仍 `30a0a313`、本树上一审计提交 **`995d7e9`** 已push，两个tracked工作区均干净。最新数字日志仍321、size/mtime_ns与审计1完全相同；三个sensor代理再次确认为completed，不存在可供verified wait的live句柄。上一turn仅验证/记录同一缺失输入，分类no progress；本turn没有新的执行结果、源码缺陷或可继续的有据行为修改，因此仍是相同r3实机验证阻塞。没有重新启动agent、CI/构建/测试、探针或设备操作；只补记本审计，不把此NOTES提交当作目标进展。
- **审计3/工具状态已收束**：最新被动状态与前两次相同：main `30a0a313`、本树 **`50099af8`** 均tracked干净，最新日志仍321且1781932 B/mtime_ns=1794744150821963383不变，三个sensor代理均completed。上一turn分类no progress；本轮仍没有新的r3执行/DATA证据或可继续的有据行为修改，满足同一阻塞连续三turn且不能在缺失输入下推进的条件。实际调用 `update_goal(status="blocked")` 已返回 objective=`修复完整代码`、status=`blocked`（不是缩小目标或以交接提交冒充完成）。仅收尾这三次审计的NOTES，正常合main/push后删除本轮交接树/分支；真实代码、最新r3、GPU280插件、原件、旧镜像与分区备份均不动。用户提供新日志后恢复目标，从最早失败阶段继续；不要在未获新证据时自动重跑旧审查或不停提交状态说明。
- **实际交接合入**：审计提交 **`995d7e97/50099af8/136214b7`** 已ff合main并 **push成功 `30a0a313..136214b7`**，仅NOTES有差异；不把这些状态记录当作功能进展。此最后归档交接正常合main/push后仅删除本轮 `.worktrees/sensors-verification321`、本地及远端 `fix/sensors-verification321`；三次记录保留在main历史。最新正常候选仍r3/source36083f18，无新CI/build/boot/插件/设备验收，goal实际状态blocked，等待用户新日志而非假装有live process。

#### 2026-11-15 用户要求最新 main 整包镜像（已编译，未上机）

- **范围/准确源码**：用户要求“编译一个最新的main镜像，应包含传感器相机等”。在当时tracked干净的最新main **`a6854c830f7e66f34ad929978132351ad418468c`** 上创建 `.worktrees/main-image-20261115` / `feat/main-image-20261115` 固定源码快照；没有合入未完成的其它功能分支或改变源码。本镜像包含该main已有的光感/距离LIST、控制/DATA、HID生命周期/mode/open replay修复，现有相机控制/接收/RAW/iOS桥代码，以及最新NAS/call/SMS、GPU等集成；构建源码HEAD不以之后的交接commit替换。
- **启用项/边界**：沿用完整 `x9/build/boot-device.json`，只将 `x9_als=1`替换为 **`x9_als=3`**，并唯一追加 **`x9_prox=3`**；boot line为556 B，其它完整UFS/Wi-Fi/蜂窝/GPU参数保留。相机维持main **`x9_camera=1`**（节点/只读状态模式），实际相机代码与自有iOS桥均已编译/内嵌，但不启mode2注入或mode3硬件probe；原构建明确报告 `explicit cold probe/standby, no capture adapter; install only with x9_camera=2`。**相机采集后端尚未完成，不能声称此镜像已能拍照。**未追加音频启动自测、诊断或probe token，也不更改main其它功能默认模式。
- **输入/构建**：`.local`共享固定主目录输入，复制既有 `x9/build/local.json`，没有仓库外私有输入。worktree的 `prepare-local.py --check` 首次exit2仅因共享maintained-source symlink指向主checkout而非新树；未改共享symlink，改在权威主checkout执行同一check，**exit0**。两个输出分别保留 `prepare-local.log/prepare-local-main.log`。正常执行 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --boot-config x9/out/x9-main-full-a6854c83-20261115/boot-input.json --name x9-main-full-a6854c83-20261115`，**真实exit0，98/98 overlay objects / 3 prebuilt、完整kernel link与boot pack成功**；仅现有warnings，无新增测试脚本/套件或构建产物二次解包/hash/反汇编/复建对比。没有重建未变XNU本体、无本轮CI run（本地构建）。
- **最新正常产物/归档**：内置构建输出 **Image30928896 B / boot14106624 B**；最新正常main整包候选为 **`x9/out/x9-main-full-a6854c83-20261115/boot.img`**，相机桥构建输出136192 B。整个产物目录已**实际无覆盖迁回主checkout `x9/out/`**，保留 `source-head.txt/source-status.txt/boot-input.json/build-command.txt/build-exit-code.txt/full-build.log/build.log`、相机/音频/HAL等构建输入记录及上述prepare日志；原构建时worktree绝对路径不回写。此前sensor r3/r2、其它镜像、原件及分区备份全部保留，不用新main镜像覆盖旧候选。
- **插件/设备/验证**：main allowlist保持 **GPU280**，未调用 `--revision`、没有新plugin-package/mailbox/设备/分区操作；设备若仍为321的GPU280基线，**无需mailbox**。用户手动 `fastboot boot x9/out/x9-main-full-a6854c83-20261115/boot.img`，先看ALS LIST/ENABLE和PROX transport，再核真实lux、raw oneshot/遮挡关系及原生自动效果；相机mode1的节点/状态不是拍摄成功。此请求的**整包编译交付成功**，但原“修复完整代码”goal的物理链/功能验收仍缺新日志，不因build成功complete或恢复goal。此交接正常合main/push后仅清理本轮build worktree/本地分支，产物已在主目录，不动其它活动树。
- **实际合入收尾**：构建交接 **`5882b34d`** 已ff合main并 **push成功 `a6854c83..5882b34d`**；差异仅NOTES，镜像源码仍a6854c83，不为纯交接再编译。完整正常候选及日志已在主目录，最后归档交接合main/push后只删除本轮 `.worktrees/main-image-20261115` / `feat/main-image-20261115`，无需删任何输出目录或改共享`.local`链接。构建请求已交付，真实传感器/相机拍摄仍未验收。

#### 2026-11-15 蓝牙根盘改为 iOS 首次启动由内核 PID1 安装（x9_bt_root）

- **动机**：用户要求不要在 Android 侧全读/全写 32G 根盘。新增 `x9/kernel/src/bsd/kern/x9_bluetooth_root.inc`，与 torch/camera/HAL 安装器同处 `load_init_program`（原 launchd 读 cache 前）。七文件约1.03 MB 由 `x9/bluetooth/root_header.py` 嵌入内核（gitignored `generated/x9_bt_root.inc`）。`x9_bt_root=1` 安装，`=2` 恢复，默认0不动。
- **安全边界**：先核 `/usr/sbin/bluetoothd`、`BlueTool` 与构建原件 SHA 一致、嵌入数据 SHA；七目标全部分类后任何未知内容整体不写。新文件 temp→fsync→校验→link（不覆盖）；两 jobs+cache 必须是 Apple 原件，先硬链接 `<path>.x9-orig` 再 rename 原子替换，cache 最后。owner 由内核以0:0创建，不需要 Android 侧 catalog 修正、macFUSE/COW。恢复反序（cache 先回原件，再删四自有文件）。日志 `X9 BT ROOT:` 是唯一 receipt。
- **源码**：`05e8da8b`（功能）+ `49695140`（合入 main `4087f82f`：NAS/call 修复及笔记；传感器/相机代码此前已在本分支）。只做了 kern_exec.c 单文件编译验证与正常完整构建，无测试套件。
- **构建**：boot-config 取 main 整包镜像 `x9-main-full-a6854c83-20261115/boot-input.json`（ALS3+PROX3，相机为内核默认）副本，附加 `x9_bt_init=1 x9_bt_fc=1 x9_bt_root=1`。首次 `bt-native-rootboot-20261115` 内核成功但 boot 打包因 PATH 选到无 lz4 的 Homebrew python3.15 失败（保留 failure.json）；改用 `/Library/Frameworks/.../3.13/bin/python3` 重建 **`bt-native-rootboot2-20261115`** exit0：103/103 overlay，Image 32837632 B，boot 15306752 B（<20MiB）。产物 `x9/out/bt-native-rootboot2-20261115/boot/boot.img`（安装）与 `boot-rollback/boot.img`（同内核，x9_bt_root=2），控制台 `x9/out/bluetooth-native/rootboot2-candidate-20261115-console.log`。
- **未做/下一步**：未上机、未写设备。用户手动 `fastboot boot .../boot/boot.img` 后先看 `X9 BT ROOT:`（Apple 原件核对、七文件 install errno、installed 7/7），再按原顺序验 SSPM/vendor ACK、H4、扫描→配对→BLE→A2DP→HFP/SCO。若 launchd 起不来，用 `boot-rollback/boot.img` 恢复原件。旧 `stage_targeted.py` 保留为备用，未执行。
- **log 322（rootboot2 首次上机，2026-11-15 23:15）**：内核安装成功：1.0–3.9 s 七文件 errno=0、`installed 7/7`。进桌面后约 39 s 整机卡死，USB console 断在 38.99 s；UFS 盘日志（`.local/x9/logs/322-disk.*`，seq24）到 37.75 s，两者都停 → 内核级挂死，不是单纯用户态卡 UI。IOMAP 7.8/64 MiB，无 panic 类迹象。最后活动是蜂窝 v4v6 call MIPC TX；但 321 同样请求后正常，蜂窝本身不是已证原因。
- **322 与 313–321 的唯一 BT 差异**：这是第一次启用 native 初始化。`X9 BT: initializer failed stage=1(PRE_ON) errno=16 cleanup=16`（1.135 s）。cleanup=16 且无 retained 打印，说明 pre_on 已越过 SSPM_DEFAULT/BT PRE pins/STA0/CON2 多主/DEV0_SET 写入后才失败；cleanup 时共享 hub（0x40/0x80 忙位）仍忙，CON2 与 endpoint FIFO 快照没有恢复。pre_on 的 AP UART3/endpoint 空闲检查放在共享状态写入之后，失败也不打印寄存器值。**这只是线索，尚未静态证明它导致 39 s 挂死。**
- **确定缺陷（与挂死无直接证明）**：板级会话不可用时 X9BluetoothLaunch 返回 1、不 exec bluetoothd；KeepAlive 每 10 s 重启（13.6/23.6/33.6 s），13 个 Bluetooth MachServices 无人服务。设备根盘现在已装新 cache，任何镜像都会走这个循环，直到用 rollback 恢复。另外内核安装器会拒绝替换内容不同的旧自有文件，以后升级 payload 必须先 rollback 或另加版本升级规则。
- **log 322 静态审计（对照 OPPO `btmtk_proj_sp.c:btmtk_pre_power_on_handler`、`uarthub/common/platform/6993/mt6993.c`）**：
  - Android pre-on：`enable_bypass_mode(0)` → `dev0_set_rx_request` → `btmtk_wakeup_uarthub`（等 ready）→ `reset_flow_ctrl`（无 XOFF 时 no-op）→ `fifo_ctrl(1)`（“disable ADSP,MD when fw dl”）→ `enable_bypass_mode(1)` → `bt_on_count_inc`。**不要求 MD/ADSP 空闲**；`reset_to_ap_enable_only` 只拒绝三个 host STA 同为 0x300/0x0，并按读到的 dev1/dev2 FIFOE 恢复。
  - 我们的 `pre_on`/`endpoints_idle`/`shared_idle` 额外要求 DEV1/DEV2 STA 的 0x1d0f 全零（sw_rx/tx、hw_rx/tx_busy、intfhub_busy bit8、rx_data_req、host_awake、bt_awake）。这是 Android 没有的检查；`MD_CHANNEL_EN=1`，MD 本来就可能占用。322 的 EBUSY 就出在这类检查上（stop 返回 EBUSY 且没有 retained 打印，说明 `requested` 已置位），但拒绝时没打印寄存器值，无法确定是哪一处。
  - cleanup 在共享忙时跳过 CON2 和 endpoint FIFO 恢复。如果失败点在 isolation（全部 FCR 写 0xC0，只重开 AP/CMM）之后，MD/ADSP FIFO 会保持关闭整个 boot；Android ap_only=0 路径按原 FIFOE 无条件恢复。
  - 已排除：蜂窝（321/322 两次 call 的 CNF 序列相同，321 第二次后仍正常）、IOMAP（7.8/64 MiB）、UFS lock 统计（同类累计值）、boot 打包尺寸（load 0x80000000 到 dtb 0x87c80000，Image 31.3 MiB）。
  - **39 s 挂死的直接机制仍未静态证明。** 可修的确定缺陷：pre_on 增加了非 Android 检查且检查在写入之后；cleanup 不恢复 MD/ADSP FIFOE；拒绝不打印寄存器；launcher 失败不回退 stock bluetoothd；内核安装器不能升级自有文件。

#### 2026-11-15 按 Android 重写 BT pre_on/post_download/cleanup，launcher 回退，安装器支持升级

- **pre_on**（`x9_bt_hardware.c`）按 `btmtk_pre_power_on_handler` 顺序：PRE_ON pins → bypass(0)+清 dev0 IRQ → dev0 RX request → TX request 轮询 ready 50×1ms → reset_flow_ctrl（仅 XOFF 时；恢复流程未实现则带寄存器拒绝）→ 保存 AP UART3 → `mtk8250_clear_fifo` 等价清 AP FIFO（不再因 RX 残字节失败）→ fifo_ctrl(1)=feedback(0)+reset_to_ap_enable_only(1)（含 Android 的三 host 同为 0x300/0 拒绝）→ bypass(1)（mask+clear dev0 IRQ）→ bt_on_count_inc → TX pins。删除 Android 没有的 `shared_idle`/`endpoints_idle`（DEV1/DEV2 0x1d0f 全零）要求。pre_on 不再置 bt_on_sta。
- **post_download**：去掉两次 endpoints_idle；bypass(0) → fifo_ctrl(0)（MD/ADSP FIFO 0xC1、feedback=1，无条件）。verify_ownership 成功后 `assert_bit_ctrl(0)`。dev0 IRQ mask 恢复为启动值（XNU 无 hub IRQ handler，不照 Android 解除屏蔽）。
- **cleanup**：不再以“共享忙”为条件。只要 APB 可读就恢复启动时的 CON2 bypass、STA0 feedback/bt_on_sta、dev0 IRQ mask；资源仍 ready 时按原 FIFOE 恢复 MD/ADSP FIFO 及 AP UART3 寄存器；再按 release_uarthub 顺序清 DEV0 请求。每次拒绝/cleanup 打印 dev0/dev1/dev2/CON2/STA0（ready 时含 FCR_RD/DEBUG_8/AP IER/LSR）。
- **launcher**：板级会话不可用、runtime 缺失、环境设置或 bridge exec 失败时，清掉注入变量后 exec stock bluetoothd，MachServices 始终有人服务。
- **安装器升级**：`root_header.py/candidate.py --previous-payload`；内核按每文件的旧自有版本 SHA 原子升级，restore 也认旧版本。
- 只做了 kern_exec.c、x9_bt_hardware.c 单文件编译（测试头含伪造旧版本以覆盖代码生成）。**修复不保证解决 322 的 39 s 挂死，其机制仍未证明。**
- **构建 rootboot3**：源码 `ee685ffa`，同 rootboot2 输入 + `--previous-payload x9/out/bt-native-rootboot2-20261115/root-payload`，exit0，103/103 overlay，Image 32837632 B / boot 15306752 B。payload 只有 `X9BluetoothLaunch` 变化（其余六个与 rootboot2 字节相同），嵌入 1 个可升级旧版本。产物 `x9/out/bt-native-rootboot3-20261115/boot/boot.img`（x9_bt_init=1 fc=1 root=1）与 `boot-rollback/boot.img`（root=2，不带 init/fc，蓝牙被动）。预期日志：`X9 BT ROOT: upgrade /usr/libexec/X9BluetoothLaunch errno=0`、`installed 1/7`；随后 `X9 BT:` 不再出现 stage=1 EBUSY，若仍拒绝会打印 dev0/dev1/dev2/CON2/STA0。
- **日志 323 / rootboot4**：323 无卡死（162 s），安装器升级 1/7 成功；蓝牙首次越过 pre_on，停在 `stage=4 errno=16`。ROM 阶段唯一 EBUSY 来源是首次 query 时 `irq_bind` 的 AP UART3 空闲检查（GIC/IER/DMA_EN/TEMT/DR）。首次绑定在本会话首个 TX 之前，FIFO 里的字节不可能是回包（上电/pinmux 时 RX 下拉可锁存 break/垃圾；安卓 tty 路径也有 dirty-data 容忍）→ 有界丢弃 ≤64 字节并打印，持续出数据或不 idle 仍拒绝；每种拒绝都打印寄存器值。音频/麦克风/振动从 320 起消失：board-default 表（8f49c45，从未进 main）缺失而 boot 配置依赖默认开 x9_audio/x9_dsp_boot/x9_mic_boot；已移植表 + x9_boot_defaults.py + build.py/boot-xnu.py + mic fallback=2。手电内核/桥接在 319 与 323 一致，差别是 mediaserverd 在 323 未打开任何音频设备（推测随音频恢复，未证实）。相机两者均 mode=1，拍摄只在未合并的 camera-integration（mode 2）。源码 `f9eba4a0`，构建 `bt-native-rootboot4-20261115` exit0：103/103 overlay，Image 32837632 B / boot 15306752 B，kernel/audio（mic HAL）已嵌入，payload 7 文件 + 1 个可升级旧版本。main 同样缺 board-default 表。
- **手电/振动静态审计（2026-11-16，未刷机）**：手电内核侧每次都正常（319/323 均 `mode=3 ... ready`、`bridge install errno=0`、`mediaserverd bridge environment appended`），319 与 323 完全一致。控制中心 `FlashlightModule.bundle` 的 `UIRequiredDeviceCapabilities=[camera-flash, arm64]`，磁贴是否出现只取决于 MG `camera-flash`，与桥无关（桥坏也会显示磁贴，只是不亮）。HakiOS loader 注释：`/product/camera "flash"` = MG camera-flash，`/product/haptics closed-loop` = DeviceSupportsClosedLoopHaptics（无则 iOS 找老式振动马达，不用 Actuator）。两节点均在 11-10 加入 mkadt（5e06897b / 21df75d9），haptics 节点到 audio36（11-14）才真正进镜像；上一次 MG 缓存重建是 11-08（x9_mgwipe），缓存只校验 build+UUID（常量）→ 两个答案很可能一直是旧的 false。振动内核侧 313–323 每次都 `native I2C5 ready`、HAL 安装，但从无任何播放活动；桥和 Actuator HAL 只写 stderr（不进控制台）。修改 `ceba10a2`：`x9_mgwipe=2` 按 epoch 标记 `/private/var/x9/mg/refresh-flash-haptics-20261110` 只重建一次（两次 unlink 成功后才写标记）；内核新增 `X9 TORCH: writer open by ...`、`X9 HAPTIC: writer open #N` / `START #N` 日志。构建 `bt-native-rootboot5-20261116`（不删缓存）与 `bt-native-rootboot5-mg-20261116`（+x9_mgwipe=2，删除 MG 缓存需用户自行选择），均 exit0，boot 15306752 B，含 rootboot4 的音频默认值与蓝牙修复。
- **日志 324 / rootboot5-mg**：MG 缓存重建成功（`X9 MGWIPE: cache errno=0`、epoch marker created=1），手电磁贴恢复（用户确认）。**振动**首次真正被 iOS 使用：`writer open #1 by mediaserverd`、`START #1 errno=0`，343 ms 后 `stop errno=60` + `fault errno=5`（fail_locked 先 stop 后打印，原始错误是 EIO；I2C 未 stuck，否则 stop 会是 EIO）→ EIO 只可能来自 SYSST(0x01)&0xe6 保护位。OPPO `rtp_work_routine/rtp_play/aw8693xs_stop` 播放路径从不读 SYSST 保护位（只在 IRQ 里读锁存 SYSINT，OPLUS 版只打日志）；OPPO 停止等待 100×2 ms 后强制 SYSCTRL3 bit5 standby，RTP 模式开 auto-brake，我们只等 25 ms；GO 后 OPPO 2 ms + 200×2 ms、GLBRD5&0x0f==8，我们 5 ms。三处全部按 OPPO 改，SYSST 只记录/打印。**蓝牙**仍 `stage=4 errno=16`，新增打印给出 `irq_bind GIC refused ctlr=12 group=0`：NS 视角（两安全态）GICD_CTLR bit0 是 RES0（pe_fiq 写 ARE_NS|G1A|G1 后读回 0x12），而 pe_fiq.c 已记录本 GIC 从 NS 读 IGROUPR 恒为 0 但中断照常投递；安卓 8250_mtk 以普通 NS IRQ 使用 SPI328。改为要求 ARE_NS+EnableGrp1A、去掉 group 读回（resume 同），route/priority/enable 读回保留并在拒绝时打印值。**同样的 `(ctlr&0x11)==0x11` + group 检查也在 x9_adsp_audio_irq.c / x9_camera_irq.inc**（当前扬声器为 AP-only、相机 mode1，未触发），尚未改。源码 `bf70834a`，构建 `bt-native-rootboot6-20261116` exit0，boot 15310848 B，boot args 与 rootboot5 相同（去掉已完成的 x9_mgwipe=2），root payload 7 文件与 rootboot5 字节相同（仅 root-payload.json 元数据不同）。
- **日志 325 / rootboot6**：蓝牙 `irq_bind SPI328 route=100 prio=a0 enabled`，首次越过 ROM→24M→XO→**固件下载**→FEATURES→ROUTING，停在 `stage=10(OWN) errno=60`。OWN 两步都比安卓严：安卓 `btmtk_uart_load_rom_patch` 下载后 0xFF 失败回退 `btmtk_uart_driver_own_cmd`；`uarthub_host_awake_sta_ctrl_mt6993` 在 FAIL/CANCEL 时都返回 0（仅 CMM_BT_AWAKE=1 且 host-awake 原为 0 时轮询 11us+20×50us），`btmtk_uart_open_done` 若 STA0 bit6 BT_AWAKE=0 发不等事件的 0xFF。我们要求 1 ms 内 host-awake+CMM+DEV0 BT_AWAKE+TX-done 否则 ETIMEDOUT。按安卓改（power_done 先、SET_HOST_AWK 打印寄存器、可选 0xFF 500 ms 并吞掉迟到事件、assert_bit_ctrl(0)）。未改：安卓 fifo_ctrl(0) 在 0xFF 之后，我们仍在 post_download 内（之前）。**振动**：无故障，START 多次，81 s `SYSST=50 at feed state=08`（BST_OVPS+FF_AES，处于 RTP_GO），但用户无震感；boost 配置（PLAYCFG1 BOOST+0x11=8V、PLAYCFG3 RTP+brake）与 OPPO DT 一致，根因未证明，加每流聚合行（accepted/submitted/peak/go/state_or）。**光感**：322–325 LIST 只有 type8 `tcs3720 ps`，从无 LIGHT；OPPO 24087 DT light_1 同为 TCS3720@0x39（need_gain_cali=1、als_panel P_7）；加 LIST 每条记录打印。**距感**：325 iOS 只请求 DetectionMode=0，未进入需要距感的场景（通话贴耳），未测。源码 `5fff0b03`，构建 `bt-native-rootboot7-20261116` exit0，boot 15310848 B，boot args 同 rootboot5/6，root payload 7 文件与 rootboot5 相同。
- **日志 326 / rootboot7**：**蓝牙内核会话首次完整成功**：`SET_HOST_AWK CANCEL ... cmm=0 bt_awake=0` → `DEV0_BT_AWAKE=0, sent 0xFF`（无回包，同安卓不需要）→ `Android vendor configuration completed 18 commands` → `complete firmware session chip=6653 baud=24000000`。但用户态 X9BluetoothLaunch 每 10 s 被 launchd 重启（每轮两次 bootstrap open = wait + resume 的 status 读回），从未 `native tty open`，无 CRASH 行（launchd SIGKILL 也会打 CRASH，见 driverkitd），说明是正常 exit；launchd 任务 stderr 是 /dev/null，`sweetnull=` tee 需加 boot arg 但 boot line 已 593/608 B。改：kern_exit.c 现有 X9 EXIT 观察表加 `bluetoothd`、`X9BluetoothLaunc`（p_comm 16 字符），看 exec 是否到达 bluetoothd 及退出码/原因。**振动**：用户感到“像扬声器发出/太轻”。`stream gen=4 accepted=11712 submitted=0`（转换输出全零，hgo 未调用）；另一流进入 RTP_GO 有 BST_OVPS。每个触觉流前 150–230 ms 有扬声器 START（iOS 音频+触觉配对事件）。HakiOS hakios.haptic.c 在同一 D22 Actuator 流上实测 tap 峰值 1000..33000，按比例跟随“多数只有安卓增益的十分之一或更低，感觉不到”，其 gain_for：≥1/3 满幅全驱动，最弱 5/8，≤256 静音。我们 RETUNE 按包络比例且上限 96/127 → 改为 HakiOS 映射、满 ±127，芯片 PLAYCFG2 仍 0x60（OPPO 效果最高 0x80）。核对：12K WAVDAT 寄存器与数据一致（OPPO 初始化 24K 但其 RTP 文件也是 24K），f0 173 Hz 对 OPPO f0_pre 1700，SYSCTRL7 PRO3_GAIN 0.5 仅低电量第三级保护生效，BST_OVP OPPO 不特殊处理。**光感**：完整 LIST 无 type5 LIGHT；前置 TCS3720 只以 `SENSOR_TYPE_FRONT_CCT=95`（hf_sensor_type.h）和 OPLUS `pwm_rgby`(116) 出现，后置 `tcs3449 als`(98)/cct/flicker/spectral；lux 应由 OPPO 闭源 HAL 用屏幕补偿计算。未实现，需用户决定来源。源码 `c7c1236e`，构建 `bt-native-rootboot8-20261116` exit0，boot args 同前。
- **日志 327 / rootboot8**：用户：振动有了但“散散的不线性”，光感选方案 2，蓝牙仍无。**蓝牙根因**：X9 EXIT 显示 launcher exec 进入 bluetoothd（同 pid、p_comm 变化；本 XNU 的 exec 会为旧映像打一条 exit），bluetoothd 随后 status 0 退出（首次 2.7 s，之后约 60 ms），10 s KeepAlive 循环。20H392 main `0x1000266bc`：取本机地址的顺序是 env `BT_DEVICE_ADDRESS` → `IOServiceNameMatching("bluetooth"/"marconi-bt")` + 递归 IODeviceTree `local-mac-address` → boot-args `wlan.factory`；全零 → `0x10002702c` `errx(0,"No bluetooth on this device.")`。ADT 原来没有 bluetooth 节点。修：x9_bt_init=1 时 mkadt 发布 `/arm-io/bluetooth`（SweetPlatformExpert 为 arm-io 子节点建 IOPlatformDevice nub），属性 local-mac-address `02:58:39:42:54:01`（占位，控制器真实 BD_ADDR 未读）、transport-encoding 1(H4)、transport-speed 24000000（`0x1000e751c`/`0x1000e7684` 同样的查找）。**振动**：Android 实机（KernelSU root）f0=1729、f0_data=234（F0 trim 只作用于芯片 CONT/RAM 驱动频率，不影响我们合成的 RTP 正弦）；读 `/sys/class/leds/vibrator/f0_save` 时手机重启，**不要再读 aw_haptic sysfs**。OPPO `aw8693xs.c` vmax_map：vmax 固定 0x11，level 0..2400 线性映射到 gain 0..0x80，波形本身保持满幅。我们上一版把 HakiOS 的 5/8 下限逐样本套用（HakiOS 是按事件选增益），每次 tap 的尾巴都被抬高 → 散、不线性；另有 3 次谐波（LRA 上表现为嗡嗡声），以及包络在 iOS 载波两倍频上的纹波与 f0 正弦拍频。改：纯 f0 正弦，与 iOS 包络成线性（1/3 满幅即满驱动），峰值保持 6 ms 后约 2 ms 释放，芯片 gain 0x80（OPPO 最大档）。worker：GO 前预填 30 ms（原 10 ms，327 某 pattern go=28，FIFO 中途耗尽→刹车再重启）；1 s 连续上限只对 scene 生效（OPPO RTP 无此限制）。**光感**：Android dumpsys sensorservice：`tcs3720 pwm_rgb`（SCP type 116，LIST gain=1）value[0]=427，value[4..7]=R/G/B/C，同时 OPLUS Fusion Light（HAL 虚拟传感器，带面板补偿）报 474 lux。改为 type 116 value[0] 作前置 lux（不再找 type 5）。源码 `6c09148d`，构建 `bt-native-rootboot9-20261116` exit0，boot args 同 rootboot8，boot/x9.adt 已含 bluetooth 节点。
- **日志 328 / rootboot9**：用户：振动太轻，亮度传感器和蓝牙仍不行；途中又要求“先做线性马达映射，按原厂的来”。**振动**：stream peak=126（满幅），underruns 归零，go 次数正常；太轻是因为 ×3 线性映射下 iOS 常见 tap（峰值 3000–5000）只有 9–46% 驱动。OPPO 对流式触觉 PCM 走 RichTap stream 模式（haptic_hv.c `aw_stream_mode`：boost=max_boost_vol 8.414 V、加载 OSC 校准、PCM 原样送 RTP；`aw_set_gain` 上限 HAPTIC_GAIN_LIMIT=128=0x80），不做逐样本或逐事件整形。改为严格线性，只定 iOS→OPPO 比例 ×6（约 5500 的 tap 到满驱，1000 为 18%），纯 f0 正弦，gain 0x80（`3497eb52`；中间按事件 HakiOS 增益的版本 `496f7a80` 已被取代）。**光感**：内核 `sample valid`，持续派发 lux=497，不过期。CoreBrightness 有“ALS is not supported on this device”的判断，引用 MG `ambient-light-sensor`；libMobileGestalt 查 `IODeviceTree:/arm-io/aop/iop-aop-nub/als`（D22 的 AOP ALS）及 `als-colorCfg`，我们 ADT 没有这条路径。改：x9_als=3 时 mkadt 发布 aop/iop-aop-nub/als（只有路径，不声明 color）；x9_mgwipe=2 的 epoch 改为 `refresh-als-20261116`，缓存只重建一次。CoreBrightness 的 HID 匹配属性（Built-In/Placement）未静态证明。**蓝牙**：bluetoothd 通过 MAC 检查，改为 status 1 退出（首次 4.1 s，之后 60–240 ms），从未打开 cu.bluetooth，也没有“bootstrap client opened by bluetoothd”。launcher 每轮打开两次（wait+resume 成功）后 exec 桥。status 1 来自 bluetoothd 的 `0x100090734`（"Bluetooth error - restarting"，_exit(1)），调用者之一是 `0x1000f36f0` "HCI initialization failed"。日志中没有 `CODE SIGNING` W+X 行，说明 vendor 的 vm_protect 没执行到，AMFI 0x5a 返回 0xff，SWEET sandbox 只有 priv_grant。可能是桥 constructor 在 UUID/slot/close 契约检查处拒绝，或 open 前失败，但所有输出都写 stderr(/dev/null)，静态无法区分。新增 `kern.x9_bt_note`（root/mobile、清洗、最多 400 行，打印 `X9 BT USER:`）；launcher/H4/vendor/capabilities/BlueTool 的所有 fprintf(stderr) 改为同时写入该 sysctl，桥另报 constructor 进入、路由安装、每次 POSIX.open。构建 `bt-native-rootboot10-20261116`（593 B）与 `bt-native-rootboot10-mg-20261116`（+x9_mgwipe=2，605 B），均 exit0，ADT 含 aop ALS 路径与 bluetooth 节点。
- **日志 329 / rootboot10-mg**：**光感已工作**（用户确认）：MG `ambient-light-sensor` 由 ADT `/arm-io/aop/iop-aop-nub/als` 决定，加节点并重建 MG 缓存（epoch refresh-als-20261116，日志 `X9 MGWIPE: epoch marker created=1`）后 CoreBrightness 接受 X9AmbientLight 的 lux。**蓝牙**：`X9 BT USER` 显示 launcher "board awake; executing stock bluetoothd with H4/vendor bridge"，随后 bluetoothd "X9 H4: constructor entered"，之后没有任何 note，exit 1。constructor 之后仅剩的静默返回在 X9Vendor16：匿名 vm_allocate → vm_protect(R|X) 做 trampoline（hook 数 11 ≤ 13 不是原因）。另外本内核是 macOS 配置（XNU_TARGET_OS_OSX），vm_map_protect 对普通 arm64 map 拒绝 W+X，原来的 RWX|COPY 本来也会失败。UniXNU QcaBluetooth 是离线 patch 并重签 bluetoothd（tools/patch-ios27.py），不是运行时改。改（`de6bd737`）：trampoline 放进 dylib 自己 `__TEXT,__x9tramp`（16 KiB 对齐、恰好一页），所有页按调试器下断点方式改：R|W|COPY → 写 → R|X，每个失败点都写 note。如果仍失败，下一步改为 UniXNU 式离线 patch 原件。**振动**（用户：仍太轻、散、有延迟）：多个 stream accepted>0 但 submitted=0（输入包络低于驱动阈值），tap 只有 10–70 ms（173 Hz 下 2–12 个周期），LRA 来不及起振、结束后自由振铃。改：每个事件前两个周期按事件自身电平 2 倍超驱，结束时一个周期反相刹车（从最后电平线性降到 0），两者都随事件缩放，仍是线性；预填 30→20 ms；stream 摘要加 `in_peak`（输入峰值），条数 16→48。构建 `bt-native-rootboot11-20261116` exit0（boot args 593 B，不再需要 mgwipe）。
- **日志 330 / rootboot11**（用户：卡死，设置里出现蓝牙选项）：串口 `X9 BT USER` 在 "constructor entered" 之后什么都没有，于是按用户要求进安卓读硬盘日志：`pull_ios_logs.py --manifest .local/x9/root-extension/20261110-16g/root-manifest.json --paths CrashReporter diagnostics`（脚本仍会自动带上 uuidtext），再用 `x9/out/refs/mandiant_macos-UnifiedLogs/examples/target/release/unifiedlog_iterator -m log-archive` 解码（macOS `log show` 读不了缺元数据的归档）。结果在 `.local/x9/logs/330-disk/`（330-unified.jsonl、bt-330.txt）。**蓝牙根因（已证实）**：bluetoothd 用原生 `BT driver`（H4/POSIX，说明 hook 没装上），启动时先去钥匙串读/建 LE identity root：securityd 用 AKS sel 10（class 11，bag -6）包钥匙，SweetKeyStore 回 e00002c7 → SecItemAdd -25291 → `Failed to set new identity root!`，readRootKeys（0x100594fac）返回 130 → 0x100599104 cbnz → `Bluetooth error - restarting reason=130`，每 10 s 重复；整个开机过程 wrap 失败 139 次，所有钥匙串用户都受影响。修（`fabf4d4e`）：sel 10/11 照 HakiOS MiniAppleKeyStore 做软件 wrap（固定可逆混淆 + 8 字节标记/class 头，没有 SEP，不是真保护），descriptor 输入也处理；x9_bt_note 同时写 os_log，让 note 也进持久化日志。**卡死**：持久化日志停在 06:07:04，卡死在约 7 s 后（串口 70.98 s mediaserverd SIGABRT，71.8 s 中断），没落盘，没有 330 的 crash report；旧 mediaserverd .ips 是 dispatch 队列里未捕获的 C++ 异常（std::terminate），以前的启动也出现过但没卡死。根因未证实，本轮不动。构建 `bt-native-rootboot12-20261116` exit0。
- **振动重做（2026-11-16，未刷机，rootboot13）**：用户：有延迟、很多场景不适配/适配错，映射仍有问题。静态读 20H392 `Library/Audio/Tunings/D221/Haptics`（OS.dmg 只读挂载）：hapticengineconfig.plist 的 transient 用预制、满幅（32767）驱动脉冲 MicroTap（约半周期、单向）、MiniTap（1 周期）、FullTap（约 2 周期），80–300 Hz 由 sharpness 选；continuous 是带包络的正弦。所以 Actuator PCM 已经是给马达的成品波形，正好对应 OPPO RichTap stream（`aw_stream_mode`：PCM 原样进 RTP）。327–330 的包络 + 173 Hz 重合成 + 6 ms hold + kick/brake 把 7 ms 的 MicroTap 拖成约 30 ms 的嗡，丢掉 sharpness，×6 又把每个 tap 削顶（330 in_peak 17695–32509）。改（`66dd5f8c`）：转换只做 48k→12k 抽取，线性 32767→127，不加包络、不重调、不加 kick/brake、不去直流（用 D221 WAV 验证：MicroTap230 6.8 ms 单向、MiniTap150 1 周期、FullTap80 33 ms、强度 1/4 → 峰值 32，延迟约 1 ms）。HAL 插件提前 20 ms 放包，让内核 20 ms 预填在呈现时刻前完成，而不是从呈现时刻才开始。运行中最后一个信号后继续送 200 ms 静音（原 20 ms 就停机），连续按键不再每次重新 GO + boost 起振。构建 `bt-native-rootboot13-20261116` exit0（含 rootboot12 的 AKS 修复；HAL identity eaeb987a…）。


#### 2026-11-15 相机最终正常 mode2 功能镜像已构建；下一阻塞为首次真实上机（主集成）

- **实际源码/构建**：`feat/camera-integration` 构建前合入 main `48851038`（含同期call/SMS/sensor修改）为 **`4825d329c2a9bf031addf919bcdbc24569ffcf30`**；执行 `python3 -I x9/build/build.py all --boot-config x9/build/boot-device.json --name x9-camera-native-preview-r1-20261115`，完整 overlay/kernel link/相机iOS bridge/shim/boot pack **exit0**，**98/98 overlay（3 prebuilt）**。内置输出 **Image31035392 B / shim-boot14168064 B**。含新bounded IRQ/CSI collection、mode2一次真实native注册、正常持续RAW/AE/实际停止、独立READY取消与fullclose/fresh-MM reopen，不是旧kernel-only或过程探针。
- **权威候选/记录**：`/Users/tosasitill/ios_plj110/.worktrees/camera-integration/x9/out/x9-camera-native-preview-r1-20261115/boot.img`；同目录 `boot-input.json`、`source-head.txt`、`full-build.log`、`build.log` 是当次输入/源码/原始构建记录。没有构建后二次解包/hash/反汇编/复建对比、测试套件、设备/reboot/flash/分区操作。没有新plugin-package或camera mailbox；GPU仍280，设备若仍保持321资产则无需mailbox。没有CI run ID，最后gh run list为空。
- **下一次最小权威验证**：由用户在主checkout执行 `fastboot boot .worktrees/camera-integration/x9/out/x9-camera-native-preview-r1-20261115/boot.img`。进入SpringBoard后打开原生相机，移动镜头看连续真实后置预览；若成功再切前置、关闭重开一次（不要连续耗尽8个exportedepoch）。保存新完整 `~/N.txt` 和屏幕/现象；首先核当次 `X9 CAMERA FDT/IRQ/CAL` metadata、mode2 native注册/两真实chipID/physicalclose，再到实际MM/CSI/CAM/MMPC/SMMU、SOF/DONE/RAW、CMCapture graph Start、PixelBufferHandler，最终以屏幕连续真实图像验收。前级成功不能代替屏幕，失败返回最早实际失败分支，不先猜iOS后级。
- **当前结果仍未验收**：没有这份镜像的boot/真实RAW/handler/屏幕证据；最新已有设备证据仍321的mode1/platform0。目标“iOS相机真实连续预览”保持未完成，工作树与分支保留，不合main宣布完成/不删树。代码/构建已可复现，但缺用户临时boot和新日志，不能继续用静态推断代替运行证据。仍有8 exportedepoch上限及uncertainstartup保守retain限制。

#### 相机上机证据等待审计（2026-11-15）

- 镜像交付及随后两次goal续轮的同一阻塞未变化：没有用户启动mode2候选后的新日志/屏幕证据。最新数字日志仍 `321.txt`（1781932 B，mtime 2026-11-15 20:02:30），其相机行仍mode1/platform0；精确USB日志/pull工具进程查询无正在采集的匹配进程，不能将等待会话当成live运行。未执行设备命令，不复建/复检候选，不修改驱动以替代运行验证。
- 功能树干净、最终正常候选与源码记录保留；完整预览goal因上述外部证据缺口置blocked，**不是complete**。解除条件：用户手动boot上节候选并给新完整日志和实际Camera预览现象；收到后从当次最早失败/成功分支继续，不能以旧mode1日志或编译绿色验收。该审计仅更新交接并push，不合main/删相机树或动其他会话。

- **日志 331（r7，首次 x9_camera=2 实机）→ r8b**：`X9 CAMERA NATIVE REGISTER: errno=45 IDs=0000/0000`，主摄冷上电在读 ID 前 ENOTSUP，status FAULT，mediaserverd bridge 不发布相机，Camera.app（142 s）黑屏。静态定位：`cam_board_prepare` 把四个 route pin 一律按 mode1 判 AP，但 mt6993-pinfunc 中 GPIO162/163 **mode1=SCP_SCL2/SDA2、mode2=AP SCL12/SDA12**（i2c@16200000：WL28681C+RT5133）；live DT 把另一组 SDA12 引脚 GPIO187 用作 WL28681C reset 组 GPIO，故 i2c12 必在 162/163 mode2。LK 交接 mode2 → ROUTE blocker → ENOTSUP（无 AP CLOCK/RAIL/PIN 日志、恢复干净，与 331 吻合）；旧 take 还会把总线切给 SCP。修复 **`01c9af6c`**：route pin 按 pin 携带 AP 功能号（sensor SCL/SDA=1，162/163=2），`cam_pin_write` 允许 MODE≤15；native power-on 失败也打印 `cam_sensor_cold_log`。同轮核对无误：GPIO/MODE/DIN 布局、UNIVPLL/MAINPLL 地址与 24 MHz 判据、CAMTG4 parent7/HWV v1 XPU0 偏移、I2C4/I2C12 base/DMA/IMPE·IMPC gate 位与 i2c_east/peri 父时钟、mt_i2c_regs_v2 偏移、MT6661 DOVDD/AFVDD 寄存器、WL 通道 4/7/2 与 hhmain 电压表。**未消除风险**：LK 含 RT5133 代码并引用 i2c12 base，若其留下的 TIMING 超出≤100 kHz 标准档或 CONTROL 含 DMA/ASYNC 位，bus 阶段仍会拒绝；届时 `X9 CAMERA AP TIMING` 与 COLD 日志会给出实际寄存器。镜像：`x9/out/x9-camera-route-r8b-20261116/boot.img`（与 r7 同命令/参数）。
- **日志 332（r8b）→ r9**：route 已过（162/163 入口实测 mode2，验证上轮修复）。新首错 `PROBE stage=2 blockers=2 errno=16`：DOVDD LN60_7 入口 sel=d0/EN0/DA0，PMIF 无 fault，`cam_rail_snapshot` 唯一剩余 EBUSY 为关闭 rail 上 HW_OP_EN0≠0。厂商 mt6661-regulator 只把 HW_OP_EN0 bit1 当 HW1 normal-mode 选择（ln60_7 无 allow-set-load，不会写），imgsensor 只靠 RG EN 上电。修复 **`8b0889fe`**：关闭 rail 的 HW_OP/EINT 作为借用配置接受，不写、全程仍须不变；新增 `X9 CAMERA RAIL ENTRY` 记录每路入口 sel/en/da/hw_op/eint。同轮核对：HWV v1 预轮询 DONE、CAMTG4 入口 parent7/PDN0、WL28681C ID0x0D/ENABLE reg3/VOUT reg3+n。注册循环任一镜头失败即整体失败，UW/长焦未上机，**r9 去掉 `x9_camera_lenses=1`**，只验主摄+前摄。镜像 `x9/out/x9-camera-rail-r9-20261116/boot.img`。

## Camera log 333 (r9) -> r11 (2026-11-16)
- 333: rails pass (DOVDD/AFVDD hw_op=80 borrowed). New fail PROBE stage=9 blockers=1 EBUSY in
  cam_clock_take: CAMTG4 entry cfg 07 (parent7, PDN0), ap vote 0, FENC/ALL not logged.
- Vendor MUX_GENERIC_HWV enable = HWCCF vote only, no FENC-off precondition. Now a live/voted
  CAMTG is borrowed (parent must be 7), our vote + post-vote 24 MHz checks unchanged.
- Front MCLK fixed to CAMTG0 (live DT clock 19, GPIO72 CMMCLK0), was CAMTG1.
- I2C take now like mtk_i2c_init_hw: W1C stale STAT, FIFO clear, MULTI/CONTROL borrowed,
  non-standard inherited timing reprogrammed; only START bit0/APDMA count as busy.
- Later stages follow Android: wake GPIO171 input-high no longer refused; reset/MCLK pins
  forced to rst-low/mclk-off states; inherited-on WL camera channels cleared first
  (regulator_late_cleanup equivalent).
- New logs: X9 CAMERA MCLK ENTRY, AP ENTRY, WL ENTRY.
- Image: x9/out/x9-camera-android-r11-20261116/boot.img (r10 superseded, not flashed).

## Camera log 334 (r11) -> r12 (2026-11-16)
- 334: MCLK passes (CAMTG4 fenc=1 borrowed, vote OK), I2C4 timing reinit OK (97.9 kHz).
  Fail: PROBE stage=3 blockers=8 EIO on I2C12 (gate 1) timing reinit; entry timing=20
  low=20 div=004 hs=8000 io=93 multi=4380 = LK ~378 kHz, DT clock-frequency 400 kHz.
  One reprogram write did not read back (which one not logged).
- Fix: WL/RT5133 bus borrows the inherited LS/FS timing when HS is off and rate <= DT
  400 kHz (Android runs it at 400 kHz, never reads back). Failed CSR writes now log
  X9 CAMERA AP WRITE; borrow logs X9 CAMERA AP TIMING BORROW.
- Image: x9/out/x9-camera-i2c12-r12-20261116/boot.img

## Camera log 335 (r12) -> r13 (2026-11-16)
- 335: every power stage passed (WL ID ok, WL 4a on, DOVDD/AFVDD on, VS vote, CAMTG4 24 MHz,
  MCLK pin208 CMMCLK4 4 mA, reset171 high). Fail: PROBE stage=6 errno=6, one ID read NACK
  (sensor status 3 = COMP|ACKERR). Power-off then failed at COLD RESTORE step=5 (WAKE) EBUSY.
- Statically re-checked vs vendor: module is hhmain (README Android log), 0x1a, ID regs
  0016/0017 = 0x9186, pw_seq order/voltages/delays, WL map (AVDD LDO4 179, AVDD1 LDO7 54,
  DVDD NLDO2 144), VS voter SID3 0x178a bit2, pins 171/208/174/175. Front hhfront likewise.
- Difference 1: vendor get_imgsensor_id retries NACK (2 addrs x 5); we read once. r13 retries
  up to 10 x (1 ms gap) on clean NACK only. Logs X9 CAMERA ID.
- Difference 2: WL ENABLE keeps non-LDO bit7 (prior c8 = ours|0x80); wake restore needed the
  whole byte 0, so power-off always EBUSY. Vendor selects WL reset_high once and never lowers
  it (GPIO203 is also RT5133 enable): keep high, retire journal. Logs X9 CAMERA WAKE.
- Image: x9/out/x9-camera-id-r13-20261116/boot.img

## Camera log 336 (r13) -> r14 (2026-11-16)
- 336: ID retry ran 10x, all NACK (status 3). Power-off now completes (WAKE kept high, enable=80).
  Phone hung silently at 23.45 s mid WiFi NVRAM download (330-335 all finished WiFi); no
  CrashReporter/panic report on disk for this boot; no static camera link found (camera rails/VS
  are camera-only per DT). Watch for recurrence.
- Android live state (adb root, debugfs, camera streaming): CAMTG4 24 MHz on; WL LDO2/4/7
  1.160/2.804/1.804 V on (WL reg03=ee); LN60_7 1.8 V, LDC0_3 3.0 V; GPIO171 out-high, 208 mode1,
  174/175 mode1 pull-up rsel7 (same idle/streaming). Identical to iOS r13 state.
- i2c4 is shared with SCP AOV (aov sensor-i2c includes i2c4) and enters with MULTI=4380.
  Vendor mtk_i2c_init_hw (apdma_sync) soft-resets controller + APDMA on every runtime resume
  and after each ACK error; we never reset. r14: vendor reset sequence at sensor-bus take
  (START=0, W1C, DMA WARM/CLR/HANDSHAKE|HARD, SOFTRESET HANDSHAKE|SOFT, TX_MEM_ADDR=1000,
  DELAY_LEN=2). Logs X9 CAMERA AP RESET.
- Image: x9/out/x9-camera-i2creset-r14-20261116/boot.img

## Camera log 337 (r14) -> r15 (2026-11-16)
- 337: no hang (ran to 52 s). Vendor init_hw reset ran but changed nothing (MULTI 4380, TIMING 20
  unchanged); ID still NACK x10. MCLK FENC=1 at entry explained: LK leaves CAMTGs on; Android
  only shows N when idle because clk_disable_unused turned it off. Offsets all match clk-mt6993.c.
- Android live pad state (pin string = mode dir out in drv2 smt ies pullen pullup + rsel) for
  174/175 and 162/163: IES1 SMT1 PU1 PD0 RSEL7. Never read/set by us; DT has no pinctrl for
  174/175 and LK only uses i2c12 -> sensor pads may lack input/pull-up on iOS = NACK.
- r15: cam_sensor_pads in route take logs i2c12 reference pads + sensor pads (X9 CAMERA PAD) and
  sets back 174/175, front 172/173 to IES1 SMT1 PU1 PD0 RSEL7 (bank5 0x120e0000 SET/CLR).
- Image: x9/out/x9-camera-pad-r15-20261116/boot.img

## Camera log 338 (r15) -> r16 diagnostic (2026-11-17)
- 338: sensor pads were already IES1 SMT1 PU1 PD0 RSEL7 at entry (same as Android, same as i2c12):
  pads ruled out. ID still NACK x10. All statically checkable state matches Android live state.
- User approved ONE read-only diagnostic image: after the ID NACKs, dump i2c4 debug CSRs
  (DEBUGSTAT e4, DEBUGCTRL e8, DMA_FSM ec, MCU_INTR 40, other channels) and address the other
  devices on the bus: EEPROM 0x50, AF 0x0c, alt 0x10, sensor 0x1a with 1-byte reg. Logs
  X9 CAMERA DIAG CSR/CH/PROBE. ACK from EEPROM/AF => sensor problem; NACK all => bus.
- Image: x9/out/x9-camera-diag-r16-20261117/boot.img

## Camera log 339 (r16 diag) -> r17 (2026-11-17)
- 339 diag: on i2c4, EEPROM 0x50 ACK (byte 05) and AF 0x0c ACK; sensor 0x1a NACK (2-byte and
  1-byte reg), 0x10 NACK. Bus/controller/pads/timing proven good; the sensor itself is silent.
  Froze ~2 s after Camera.app opened (37/38 did not): blamed on the diag (other-channel CSR reads
  stalled boot 1.3 s). Diag removed in r17.
- Android live diff camera off->on (adb debugfs regmap): SID3 178a 00->04 (VS voter, iOS does it),
  1787 08->00 (another voter, not sensor0), DOVDD 2047/2062 and AFVDD 1f07/1f14 (iOS does it),
  WL 03 80->ee (extra LDO3/LDO6 = front/other sensors, opened together by the Oplus app),
  CAMTG3/4/5 on (4 = sensor0 per DT clocks <VLP 23>). Sensor0 rails/codes/order/pins
  (0xd001, 0xab), univpll (CON1 0x180000 = 2496 MHz), GPIO offsets all match Android.
- Only remaining Android write iOS never did: Oplus wl28681c-regulator.c probe
  regmap_write(0x11, 0x07) "disable UVP". Android WL regs: 01=03 02=7f 10=7f 11=07 12=01 13=05.
  UVP trip on a WL output under sensor inrush explains "EEPROM/AF ACK, sensor silent".
- r17: write WL 0x11=0x07 after WL ID check, before any channel enable (X9 CAMERA WL UVP);
  read-only X9 CAMERA WL STATUS (03,0b-0f,11,12) right before the ID read.
- Image: x9/out/x9-camera-uvp-r17-20261117/boot.img

## Camera log 340 (r17) -> r18 fmeter diagnostic (2026-11-17)
- 340: WL reg11 was 06 (UVP on) -> 07; WL status en=ca, 0b-0f=00. ID still NACK x10. No freeze
  (ran 75 s): the 339 freeze was the r16 diag. GPIO203 was output LOW at iOS entry (WL freshly reset).
- Re-checked vs vendor: hhmain pw_seq, get_imgsensor_id (0x0016/17, 0x9186, addr 0x34/0x35),
  i2c4 DT (plain mt6991-i2c, 1 MHz), CAMTG4 is MUX_GENERIC_HWV (HWV grp6 SET bit7 -> PDN bit7=0;
  iOS CFG=07 so PDN already 0), set_mclk = clk_prepare_enable+set_parent. RT5133 unused by camera.
  Nothing left to compare statically except whether MCLK really toggles.
- r18 (user-approved diagnostic): vendor fqmtr_cal on VLP CKMTR (0x1c201000; dbgmux 0x1c200344)
  measures ref 26M (id1), CAMTG4 (id25), CAMTG3 (id24) right before the ID read
  (X9 CAMERA FMETER); WL STATUS also prints regs 01/02/10/13 (Android 03 7f 7f 05).
- Image: x9/out/x9-camera-fmeter-r18-20261117/boot.img

## Camera log 341 (r18 fmeter) (2026-11-17)
- FMETER: ref26m=26000 (meter valid), camtg4=24019 kHz, camtg3=24019 kHz -> MCLK source runs.
- WL STATUS full match with Android: en=ca 0b-0f=00 11=07 12=01 01=03 02=7f 10=7f 13=05.
- ID still NACK x10. Verified also: LN60_7 VOSEL 0x24a[7:4] (0xd -> 1.8 V), LDC0_3 0x1e61/0x1f07;
  pmic_delayus=0 (no maximum-pmic-delay) so adaptor_pmic_ctrl is a no-op; no MIPI switch.
- Freeze: NOT camera kernel. At 40.043 s (0.1 s after Camera.app open) GPU MMU fault
  (CSF BOOT fault mmu=00030002 AS1status=01d003cb AS1addr=3dfcb01c0) -> RENDER submission failed,
  no reset/retry -> backboardd DeviceLost x23k, UI dead, kernel alive to 149 s. Only in 341.
  GPU issue for the gpu-wait line of work.
- Web search (user request): nothing beyond what iOS already does (MCLK before ID, rails ->
  INCK -> XCLR, >=0.6-10 ms after XCLR). MT8189 "CMMCLK gating" thread = just pinmux name.
- Still unverified: MCLK at the PAD (fmeter measures inside VLP), BUCK5 (WL NLDO input) actually on.
- No new image this round.

## Android BUCK5 / MCLK pad check -> r19 (2026-11-17)
- Android live (adb regmap 0-03, camera off and on identical): MT6661 SID3 BUCK5
  EN 0x1687=01, DA 0x16c5=03, VOSEL 0x244=be. DT mt6661_3_buck5 = regulator-always-on.
  178a 00->04 (sensor0 vs-voter) and 1787 08->88 only stack on the SW enable.
- MCLK pad: GPIO208 DIN sampled via debugfs pins: off 0x200 zeros; on 83 ones/117 zeros (toggling).
  GPIO171 reset 1 when on.
- iOS never enables BUCK5 in SW (only votes). BUCK5 = WL28681C NLDO (DVDD) input per the vs-voter.
- r19: cam_vs_vote(on) reads BUCK5; if EN bit0 clear, set VOSEL be (only while off), EN|=1, wait DA,
  never disable (always-on like Android). Logs X9 CAMERA BUCK5. Fmeter line kept.
- Image: x9/out/x9-camera-buck5-r19-20261117/boot.img

## Camera log 342 (r19) -> r20 SID3 dump (2026-11-17)
- 342: BUCK5 already on in iOS: en=01 vosel=be, but DA 0x16c5=13 vs Android 03 (bit4 extra,
  undecoded in vendor headers; likely a HW low-power/mode status). BUCK5-off hypothesis disproven.
  ID still NACK; fmeter camtg4=23968 kHz. No freeze.
- r20 (read-only): dump SID3 0x1680-0x16cf and 0x1780-0x17cf after the vote (X9 CAMERA SID3 xxxx)
  to diff against Android (have 1680-168f, 16c0-16cf, 1780-179f captured).
- Image: x9/out/x9-camera-sid3dump-r20-20261117/boot.img

## Camera log 343 (r20 SID3 dump) (2026-11-17)
- SID3 dump: 1680-168f, 16c0-16cf, 1780-179f byte-identical to Android (16c5=03 in the dump;
  the earlier single-read 13 was transient). PMIC/BUCK5/voter side fully ruled out.
  (Android 1690-16bf, 17a0-17cf not captured; iOS: 1690 00 a6 00 00 06 17 87 84 07 0a 00 ba ...)
- ID still NACK; fmeter camtg4=24019.
- Silent hang at 26.56 s (log cut mid-line, no desktop, USB dropped) during modem bring-up;
  camera code finished at 3.3 s, Camera.app not opened. 336 (WiFi), 339 (GPU), 343 (modem) all
  stop at different places -> invisible panic pattern; need disk panic logs (pull_ios_logs from Android).
- No new image.

## Android full camera off/on diff (safe regmaps) -> r21 (2026-11-17)
- Safe snapshot (SPMI 0-03/0-06, WL 12-0035, pins, clk, regulator_summary; NOT all regmaps:
  globbing every debugfs regmap rebooted the phone). Regulator consumers for 4-001a on open:
  dvdd=wl ldo2 1160, avdd=wl ldo4 2804, avdd1=wl ldo7 1804, dovdd=ln60_7 1800, afvdd=ldc0_3 3000
  = exactly iOS. Extra pins on open: SCP_SCL4/SDA4_OIS (218/219), SCP_SPI2 (164-167),
  CMMCLK3/5 (192/209), CONN (232-234): not sensor0. kmsg do_hw_power_on order = pw_seq.
- Remaining Android difference: I2C speed (DT i2c4 clock-frequency 1 MHz; iOS retimed to 98 kHz).
- Found in iOS: sensor/ldo bus mapped ALL channel banks (i2c4 0x80000 + dma 0x30000, i2c2 same,
  i2c12 0x40000+0x10000 = ~1.7 MB io_submap) and cam_bus_other_idle read channels 1..7 of the
  AOV/SCP-shared i2c4 on EVERY transfer (r16 diag: reading ch3 stalled 1.3 s). Suspected cause of
  the silent hangs (336/339/343, io_submap exhaustion pattern) and of a disturbed bus.
- r21: banks=1/dma_banks=1, 0x1000 maps (vendor uses channel 0 only); sensor bus max_hz=1 MHz so the
  inherited ~378 kHz LS timing is borrowed (like i2c12); removed fmeter, SID3 dump, WL status dump.
- Image: x9/out/x9-camera-ch0-r21b-20261117/boot.img

## Camera log 344 (r21) -> r22 (2026-11-17)
- 344: no hang (ran to 46 s, Camera.app opened at 35 s, no DeviceLost). i2c4 borrowed inherited
  timing (hz=378182). ID still NACK x10 -> SCL rate is not the cause.
- Checked: oplus ois_power@33 is only an SCP OIS power/pinctrl helper; vendor i2c-mt65xx uses
  ch_offset_i2c=0 without ch-offset-i2c (no multi-host shadow path) -> AP channel 0 is right.
- Remaining Android difference = timing/state: Android sensor starts from a long full power-off
  (WL default-on rails cleaned up by late_cleanup long before first open); iOS cleared WL
  default-on rails and re-powered after only 150 ms.
- r22: WL entry clear settle 150 ms -> 1 s; after 10 NACKs keep power and poll 0x1a ID every
  10 ms for 300 ms (X9 CAMERA ID LATE). Only 0x1a reads added.
- Image: x9/out/x9-camera-settle-r22-20261117/boot.img

## Camera log 345 (r22) -> r23 (2026-11-17)
- 345: no hang (2nd clean run after ch0-only). WL entry was 0x80 (COLD WL prior=c8 is the journal
  value before the last channel enable), so no inherited rails: the 1 s settle never applied and the
  sensor already started from full power-off like Android. Late polling 30x10 ms: still NACK.
  Timing/state hypothesis disproven. AOV DT node is only an SCP capability declaration.
- r23: register loop no longer stops at the back sensor's clean NACK (only when power-off was clean);
  it probes the front sensor (WL LDO1/LDO3, same DOVDD, i2c2). Front ACK -> rear-specific;
  front NACK too -> shared WL input / DOVDD. Logs X9 CAMERA REGISTER + front X9 CAMERA ID.
- Image: x9/out/x9-camera-front-r23-20261117/boot.img

## Camera log 346 (r23) -> r23b (2026-11-17)
- 346: front returned ENXIO in 2 ms without power-on: cam_native_sensor.error is sticky from the
  back NACK. r23b clears a clean ENXIO left by a different, already-closed sensor before power-on.
- Image: x9/out/x9-camera-front-r23b-20261117/boot.img

## Android camera bring-up flow vs iOS (static audit, 2026-11-17)
Source: mtkcam imgsensor src-isp8s adaptor-drv.c/adaptor-hw.c/adaptor-i2c.c + oplus hhmainmipiraw_Sensor.c.
1. imgsensor_probe -> adaptor_hw_init -> search_sensor (DT sensor-names hhmain, cjmain; both 0x34/0x35 = 7-bit 0x1a, ID 0x16/0x17).
2. try_probe_subdrv_entry: adaptor_cam_pmic_on (needs base-supply: absent in DT -> no-op), init_ctx,
   adaptor_hw_power_on, get_id, power_off.
3. do_hw_power_on: vs-voter SET (178a mask 4), adaptor_pmic_ctrl (maximum-pmic-delay absent -> return),
   pw_seq: MCLK clk_prepare_enable+set_parent 24M (pin still mclk_off), RST0 1 ms, AVDD 2.804 3 ms, AVDD1
   1.804 3 ms, AFVDD 3.0 3 ms, DVDD 1.16 4 ms, DOVDD 1.8 3 ms, pinctrl mclk_4mA (GPIO208 mode1, drive 1) 6 ms,
   RST1 (GPIO171 out high) 4 ms. No subdrv power_on op, I2C (not I3C, no pre-cfg-addr).
4. get_id: adaptor_i2c_rd_u8 = 2-msg i2c_transfer (2-byte reg write, repeated start, 1-byte read), 5 tries per addr.
5. i2c-mt65xx: ch_offset_i2c=0 (no ch-offset-i2c in DT), 1 MHz DT clock.
iOS matches every step (order, values, delays, pins, transfer format; EEPROM 0x50 / AF 0x0c ACK with the same
transfer code). Static audit finds no missing step. Signature (bus + EEPROM + AF ACK, sensor NACK) is the Sony
"no INCK at pad or XCLR low" pattern; iOS MCLK was only proven at the mux (fmeter), never at pad 208.

## r24 pad check (user-approved diagnostic, 2026-11-17)
- Read-only DIN sampling before the ID read: GPIO208 (MCLK) 4096 samples, ones/edges; GPIO171 (XCLR) 64 samples; plus MODE. Log X9 CAMERA PAD.
- Image: x9/out/x9-camera-pad-r24-20261117/boot.img

## Camera log 347 (r24 pad check) (2026-11-17)
- X9 CAMERA PAD: mclk=208 mode=1 din_ones=1576/4096 edges=3150; reset=171 mode=0 din_high=64/64.
  MCLK toggles at the pad and XCLR is high, as in Android. Pad layer ruled out. ID still NACK (10 + 30 late).
- Front still not probed (REGISTER sensor=2 NACK 2 ms later, no power-on): r23b sticky-error clear not
  sufficient; another gate blocks it (side issue, not pursued).
- WL28681C DT node has no vin supplies (inputs hard-wired); vendor driver only writes 0x11=07; codes
  179/54/144 match its linear ranges. Regs 0x14-0x1e never compared with Android.
- MT6661 BUCK5 range 0-1.2 V (5 mV): vosel be = 0.95 V; DA 0x16c5 only bit0 (EN) defined in vendor header;
  bit4 (iOS 13 vs Android 03) undocumented.
- WL full compare with saved Android capture (0x00-0x1e, camera off/on): identical to iOS except enable
  ee (Android) vs ca (iOS); extra LDO3 (0x706 = avdd of the sensors at DT 9065/9503) and LDO6 (0x723 = avdd
  of the sensor at 9573) are other cameras the OPPO app powers together. 0x14-0x1e = 00 both. WL ruled out.
- Android debugfs now Permission denied (SELinux Enforcing, ksu) even after mount; BUCK5 OP_EN 16a4-16a6
  not captured. Static audit has no further lead.

## Android BUCK5 live compare + r25 front probe (2026-11-17)
- Android SID3 (permissive, restored to Enforcing after) camera off/on: 0244=be 1687=01 16a0-af and 16c0-cf
  identical off/on except 16c1-16c4 (be e1 on), 16c5=03, 178a 00->04. iOS r20 dump (343): 16c0-cf identical
  to Android-on (16c5=03; the 13 in X9 CAMERA BUCK5 is a transient). Only 16a7 (A 02 / iOS 00) and 16ad
  (A 3d / iOS 3f) differ: BUCK5 HW op-mode config, same with camera off and on in Android -> not camera.
  BUCK5 (0.95 V) cannot feed any sensor rail. PMIC ruled out.
- r25: r23b required cam_native_sensor.sensor!=0, but a clean close zeroes it; clear a clean ENXIO whenever
  no session is open and it is not this sensor's own error. Front (LDO1/LDO3 + DOVDD, i2c2) now really probes.
- Image: x9/out/x9-camera-front-r25-20261117/boot.img

## Camera log 348 (r25) -> r26 (2026-11-17)
- 348: back NACK unchanged (pad ok). Front failed EIO in 3 ms: cam_sensor_session_fault latched cam_probe_latched on the clean ENXIO, and open refuses while latched.
- r26: after a successful restore, a clean ENXIO unlatches (cam_resources_latched still blocks).
- Image: x9/out/x9-camera-front-r26-20261117/boot.img

## Camera log 349 (r26) -> r27: WL reset_high group GPIO187/202 (2026-11-17)
- 349: front really probed for the first time: MCLK pin 72 toggles (edges 3150), reset 170 high, WL 0x85,
  DOVDD DA verified, i2c2 borrowed timing -> front NACK x10 + 30 late, exactly like the back.
  Two different sensors on two buses fail together -> shared cause (DOVDD proven by DA wait; WL regs = Android).
- Android oplus wl28681c probe selects pinctrl "reset_high" = state_reset_high_a2: pinmux <0xcb00 0xbb00 0xca00>
  = GPIO203 + GPIO187 + GPIO202, mode0 output-high; "reset_low" drives only 203, so 187/202 stay high for the
  whole boot. No other DT node uses 187/202. iOS only ever drove 203 (code comment "never 187/202") -> missing.
- r27: before the WL ID check, drive 187/202 mode0, DOUT=1, DIR=1, keep high (no restore, as Android). Logs
  X9 CAMERA WL GROUP with prior/after mode/dir/dout/din.
- Image: x9/out/x9-camera-wlgroup-r27-20261117/boot.img

## Camera log 350 (r27): SENSOR IDs READ (2026-11-17)
- ROOT CAUSE of the long ID NACK (logs 331-349): GPIO187 of the Android WL28681C pinctrl reset_high group
  (203/187/202) was never driven. iOS entry: 187 mode0 input low (dir0 dout0 din0); 202 already high.
  After 187 output-high: back `ID: sensor=1 slave=1a attempts=1 id=9186`, front `sensor=2 slave=10 attempts=1
  id=0615`, NATIVE REGISTER errno=0 IDs=9186/0615. Repeated at 12.7 s (id 9186).
- Next blocker: Camera.app start at 16.3 s: `NATIVE CONFIG sensor=1 stage=1 errno=5`, `MM: flags=19
  stage/slot=1/0 errno=5 blockers=1ff smc=c2000514/0`, start errno=5 (MM/camsys stage, before CSI/DMA).

## MM stage failure analysis after log 350 (static, 2026-11-17)
- Failing call: x9_mm_power_acquire -> SMC 0xc2000514 x1=0x803 (smmu-secure PM_GET, MM_SMMU). a0 came back
  0xc2000514 = x0 untouched = "not supported" (pkvm_smmu_hyp.c treats a0==sip_id as "smc fail because not
  support"). Code matches smmu-secure.c exactly (cmd = id | type<<8 | sec<<11), so the path itself is wrong.
- Android on MT6993: MM SMMU plat flags SMMU_SEC_EN|SMMU_HYP_EN (pKVM owns it). smi-dbg has hwccf-support,
  so mtk_smi_pm_ops is NOT registered. pKVM EL2 uses SMC 0xC2000831 HYP_SMMU_PM_GET(0)/PUT(1), smmu_id MM=0,
  which only takes a keep-on semaphore and returns nonzero if MM is off; it does not power MM.
- MM power on Android = HWCCF: mminfra-util (DT mminfra-mtcmos-voter 0x37 = HW_CCF_BACKUP_GRP_0, data bits
  MM_0=1 MM_1=2 MM_AO=0 MM_2=3, ack regs 0x31b5009c..a8 mask 0xc0000000) votes MM HWV 0x31c00000:
  XPU0 B1 SET 0x20724 / CLR 0x20728, DONE 0x1136c, STA 0x1135c, EN 0x11358. Camera nodes have no
  power-domains; seninf-core is hwccf-apply (clock votes trigger MTCMOS via HWCCF).
- Next: replace the unsupported 0x514 acquisition with the Android HWCCF mminfra path (needs care: MM infra
  shared with display/GPU).

## 2026-11-20 相机 MMUP(VCP core1) 启动：362 结论与 restore 候选
- **观测(362.txt)**：`stage=4 errno=60 lr=14e4 nest=39879` 与 361 完全相同；`0x1_06c39000` 处 `nz=39202`，首字 `8009c000 80011d60`，与 `tinysys-mmup-RV33_A.bin[0x2000]`(`c1350513 65a170ef`) 不符，即 LK 放的 MMUP 镜像已被覆盖。扣掉 `0x1_06c00000..0x1_06e00000`（fdt_ram.c/pmap.c）无效或不充分（写入者未证实，疑似 XNU 早期 first_avail 分配不走 hole 检查，仅推断）。
- **静态推演**：LK `app_load_vcp` 把 vcp_b.img 的 mmup 段读入 LK 堆 buffer(`vcp_loader_phy`)，op7 把 buffer 基址交 TF-A；SRAM 头 `+08=0x2000,+0c=pack(src),+10=0x48c00`；MMUP boot 代码(0x3a8)复位后从 DRAM `src` 拷 0x46c00B 到自己 SRAM。SRAM 头 0x0..0x2000 由 TF-A 直接写，`0x2000..0x48c00` 依赖该 DRAM。
- **修复(未上机)**：`x9/tools/mmup_image.py` 从 `.local/.../vcp_b.img` 提取 `tinysys-mmup-RV33_A`（段头 +0x200 起的 body，丢弃前 0x2000）生成 gitignored `generated/x9_mmup_image.inc`；`x9_mm_power.c:mmup_restore_loader()` 在 `MMUP_OP_RESET_RELEASE` 前按 SRAM 头(0x31837000)解出 src，校验头、拒绝 XNU 管理页，写入并回读校验，打印 `X9 MM MMUP LOADER: restored ... verify_bad=N`。
- **候选**：`x9/out/x9-camera-mmup-restore2-20261120/boot.img`（15237120 B）。上机看 `X9 MM MMUP LOADER` 行（restored/refuse/header mismatch）及 `VCP BOOT` stage 是否越过 4。
- 未解：vcp core0 的 op2 缓冲(`x20+0x200000`, `+0x800000` 第二段)是否同样被覆盖；若 MMUP 通过则再查。

### 363 结果与更正（2026-11-20）
- **观测**：`X9 MM MMUP LOADER: restored 289792 B to 106c39000 verify_bad=0` 后，MMUP 失败完全不变（`stage=4 errno=60 lr=14e4 nest=3af0d`，TBUF 仍 `19c->4a14` 后为垃圾）。`PROG fpga=0 mask=0`，而镜像静态值 `230bc=1 231e4=ffffffff`，即 SRAM 0x2000.. 仍未得到镜像。
- **更正（推断）**：SRAM 头 `+04=0x06c37001` 是 36 位打包的 **IOVA** `0x1_06c37000`（落在 mm_smmu_hfrp_protected dma-range `0x1_0000_0000..0x1_4000_0000`），不是物理地址。362/363 的 `0x106c39000` 物理探针和 restore 都无意义（restore 调用已撤除）。
- **静态**：LK `0x53d4c` 的 `0x200e8` 恒返回 0 → 走 else 分支分配(align 2MiB, limit `0xC0000000`)，与 `mblock-51-me_vcp_reserved 0xbd200000` 吻合：加载缓冲很可能在 `0xbd200000`，no-map，不在 XNU 内存里，内容应完好。MMUP 通过 TF-A 建立的保护 SMMU(SID 0xa1, mm_smmu@0x30800000) 把 IOVA 译到该 PA；读到 0 说明翻译/总线路径在 reset release 时不可用（SMMU 时钟/MM 电源域未开，或翻译态丢失）。Android `reset_vcp()` 在 release 前没有额外 SMMU 操作，依赖 LK/TF-A 留下的状态与 MM/SMMU 时钟已就绪。
- **下一步（未做）**：确认 reset release 前 MM 电源域/SMMU 时钟状态（`DIAG5 bus=a1/21` 是 bus tracker）；对比 Android 启动顺序里 MM_2/HWCCF 与 VCP 的先后；必要时在 MMUP release 前先拉起 MM 电源。
- **续（静态，2026-11-20）**：Android `x9/out/call-owned-runtime/dmesg.txt` 显示每次系统 resume 都重新 `RESET_RELEASE` MMUP 并 `core id 1 ready`（内核已运行、无 LK 参与），说明 MMUP 加载镜像留在内核不会回收的保留 DRAM（与 `me_vcp_reserved 0xbd200000` 吻合）且 TF-A/SMMU 翻译态长期有效；Android `reset_vcp()` 前无任何额外 SMMU/MM 动作。本机差异只能在“SMMU/总线翻译态被改动”“MM_2/mminfra 路径时钟”“0xbd200000 内容”三者中。当前 VCP boot 发生在首次 `x9_mm_power_acquire`、先于 MM_2 vote（`mm_vote_take` 本身依赖 MMUP 的 hwccf，存在先后环），仅凭现有日志无法再缩小。现成的一次性判定镜像 `x9/out/x9-camera-vcp-loader2-20261120/boot.img`（含 `X9 MM VCP LOADER2` 读 `0xbd200000` 的 `sig@`），未被使用；用户暂不要探针镜像，故未上机。

## 2026-11-20 camera: 364 finding + VCP DRAM carve-out
- 观察: 364.txt 在 ~20.3s 止于 vm_page_find_contiguous(num=132, low=458751) found 0 pages (vcp_alloc_reserved 的 kmem_alloc_contig 在碎片化系统失败)。
- 改动: shim 在 live range 2 末尾预留 4MiB (0x15FC00000..0x160000000, X9_VCP_DRAM_LO/HI), vcp_alloc_reserved 改用 ml_io_map 映射。
- 镜像 x9/out/x9-camera-vcp-carveout-20261120/boot.img 已构建, 未上机。
- 推断: MMUP 仍可能 stage=4 errno=60 (SRAM 拷贝读到零, 与分配无关)。下一步看 365 日志。

## 2026-11-20 365 结果
- 观察: 365(carveout 镜像)日志止于 20.300 SENSOR CAL phase=mode 且行被截断; 对应 364 里 20.324 出现 vm_page_find_contiguous 的位置。无 "carve-out"/"X9 MM VCP BOOT" 行 -> 在 vcp_alloc_reserved 内 hang/panic。
- 推断(未证实): ml_io_map 是 Device 映射, bzero(dc zva/stp)在 Device 内存 fault。
- 修复: 改用 volatile 32-bit 逐字写, 映射前加 "mapping carve-out" 日志。镜像 x9-camera-vcp-carveout2-20261120/boot.img。待 366。
