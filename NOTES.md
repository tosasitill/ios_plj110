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
- 设备上最后一次 stage 的是 **277**（2026-11-11 19:41）。main 的 allowlist 是 **280**：从 main 构建的镜像上机前必须先 stage
  `x9/out/x9-gpu280-purge-20261112`（含 280 plugin-package）。

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
  - 来电：CALL_SIP_IND 4904 → CALL_APPROVE_INCOMING_CMD 8901（0100 call id、8101 号码、0102 toa 129、0103 seq、0104 20、0105 0）/ RSP 8902（0100 id、0102 1 接受、0103 seq）→ CALL_STATUS_IND 4901 / CALL_EVENT_IND 4902 → CALL_ANSWER_REQ 0907（无 TLV）→ CALL_HANGUP_REQ 0905（0100 0、0101 call id）→ CALL_GET_FINISH_REASON 090f。
  - 短信：SMS_CFG 0601 → SMS_SEND 0605/0606；接收 SMS_EXT_INFO_IND 4609 + SMS_NEW_SMS_CMD 8601 → RSP 8602（与 X9Messages.inc 一致）。未抓到拨出（CALL_DIAL）。
- **iOS 现状**：WMS→MIPC 短信已有（243 SIM2 发送拿到 modem CNF）。QMI VOICE(0x09) 未在 X9Baseband 服务表中，通话空白；声明后每个 VOICE 请求都必须应答，否则 CommCenter ATCS_TIMEOUT 重置基带。

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
