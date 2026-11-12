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

### 5.4 其它未上机的候选

- `x9/out/x9-gpu280-purge-20261112`：GPU 280（Volatile 资源丢弃，`x9v0` 关闭），需先 stage 280 插件包。未上机。
- 蜂窝 sim2bind / gpu-cs-audit / 279 等候选的输出已在整理中删除；源码都在 main，需要时重新构建。
