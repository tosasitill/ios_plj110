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

### HEIF/HEVC 硬解续接（2026-11-15，活动分支 `feat/hevc-hw`）

- **完整目标不变**：完成 HEIF/HEVC 原生硬解全部代码；不能把旧的独立 IDR 前端、接口齐全或单对象编译通过当成完成。只在代码齐全后出唯一最终 boot 镜像；本轮没有手机、ADB、fastboot、mailbox、MMIO/SMC 或分区操作。
- **实际活动树/恢复基线**：`.worktrees/hevc-hw`，`.local -> ../../.local`，从 main `55c3508` 继续。旧 HEIF 工作树已不存在，但 `0ea7546` 的源码对象仍可读；恢复其中视频独立新增文件，手工将 recipe/link-order、UFS 初始化、构建器、shim 和 ADT 的视频 hunks 接入当前 main。旧三方 merge 对这五个集成文件均产生冲突，已恢复本轮操作前的 HEAD 版本，再逐处锚定插入；没有将旧整树或旧 NOTES 覆盖回来。main 的 `x9_mm_power.c/.h` 原样保留且只链接一次，现有 camera/audio/SCP/call/GPU 改动保留。
- **恢复后编译（本次新证据）**：真实 Builder、关闭 ccache，仅编 `sweet_x9_vdec.o`、`x9_vcp_bus.o`、`x9_vcp_wire.o`、`x9_mm_smmu.o`，总 exit0；日志/结果在 `x9/out/hevc-restored-compile-20261115/`，实际 helper/外层 stdout/exit 在 `x9/out/hevc-resume-20261115/`。iOS VT dylib 和 vdecctl 经 `x9/hevc/build.py` 的 `-Wall -Wextra -Werror` 编译/链接/签名 exit0，保存 `x9/out/hevc-restored-frontend-20261115/`。改动 shim 的 `vcp_handoff.c`/`loader.c` 按正式 toolchain CFLAGS 单对象编译及内置 ABS64 gate exit0，保存 `x9/out/hevc-restored-shim-20261115/`。没有额外测试套件、全内核链接、CI run、boot 镜像或成功产物二次验证。
- **旧实现边界仍成立**：默认 `x9_vdec=0`；没有真实平台注册者，READY/profiles 仍0。VT 注册仍显式要求 `X9IndependentHEVC`，仅 Main/Main10/Main-compatible profile3 独立 IDR。原生 bus/DMA/control/SHM 代码恢复不等于物理硬解通路；普通 VT/CMPhoto 选择、视频跨帧/DPB、HEIF alpha、线性输出和退休均尚未完成。
- **下一实际缺口**：共享 normal MM/SMMUv3 controller/storage/ASID/CMDQ owner（保留 camera SID80/ASID1 和 display SID90）；cold firmware/clock bootstrap 与真实最高层 create/install/shared/bind/start；BS 路由/VSI 写入、WAITISR/c006、硬件 completion/ring/CPU cache 生命周期、线性 NV12/P010（P01H/HYFBC 不是 P010）；VT→CMPhoto/ImageIO、grid/方向/ICC/alpha 与完整 HEVC 视频。旧 helper 的 `.local/x9/work/...` 路径不可直接复用；临时产物只放本工作树的 gitignored `x9/out/`。目标保持 active，未称完成。
- **恢复已交付**：上述恢复/集成提交 `dbaa27b` 已 push 到 `origin/feat/hevc-hw`，没有合 main。原有四个编译对象通过不证明有真实 bus/平台注册者；不重跑未改源码的成功对象。
- **新增输出代码（本次）**：`vt_image_metadata.c/.h` 已进入 dylib 构建和真实 `pixel_buffer`→`emit` 路径。按当前 SDK 的 CM→CV 键映射传递 ICC、色彩三要素、gamma、chroma siting、pixel aspect、mastering display 和 content light；先检查所有类型/比例再设 attachment，未知色彩字符串原样保留，ICC 与 nclx 不自行决定优先级。不重做已在内核裁好的 clean aperture，不伪造 BT.709、不色调映射或丢掉 10-bit 精度。当前格式描述必须实际含这些字段；容器/样本未提供的 metadata 不会凭空生成。新源码经完整 VT dylib/client 编译/链接/签名 exit0（`-Wall -Wextra -Werror`），原始日志/exit 在 `x9/out/hevc-metadata-20261115/`；没有硬件 decode/CMPhoto/ColorSync 运行结果，普通会话的注册门仍未放开。
- **原厂输入恢复/新静态证据**：从既有 `.local/x9/backups/partitions/firmware-originals/vcp-real-android-active-b/vendor_boot_b.img` 按 v4 ramdisk table 只解 type1/boot 片段，LZ4 legacy→newc，取 VCP/SMMU/mbox/clock 相关 KO；没有抓设备或触碰 recovery 片段。helper、来源记录、原件 KO、raw disassembly 在 `x9/out/hevc-vendor-source-20261115/`（全部 ignored）。`arm_smmu_v3.ko` SHA256 `5626de268a9242170c9b3d1a9109c7af7db8e79ecaf418e9ed0e13f4ad79e20d` 与旧门吻合；此次还恢复 `pkvm_smmu.ko`，SHA256 `d77ff428fcf8b2e7c0931f051b9e33033174b9f674540e665a3da4b5dafff546`。它的实际 `.init.text+0x8c` 重定位调用 `__pkvm_load_el2_module`，成功分支后 `+0xac` 才调用 `smmu_setup_hvc`，`+0x12c` 执行动态 ID 的 HVC 初始化。对应源码最终 `smmu_host_hvc` 注册 memory-share handler。故旧 SMC 取 HVC ID 不是自足的 normal owner/bootstrap；不能省掉 EL2 module、VM/memory/device 注册，也不能把 on-disk `+0x24` 的 alternatives 分支当当前运行策略。没有 captured loaded note、runtime alternatives 或 XNU 环境 handler 可用性证据。boot ramdisk 此子集没有 decoder KO，后续还需定向恢复真实 decoder 输入；原有固件/LK 输入仍在备份中。

#### 2026-11-15 Session / 共享 SMMU / VT 跨帧续写（当前交接）

- **工作树/固定源码**：仍为 `.worktrees/hevc-hw` / `feat/hevc-hw`，从 `98b1f71` 保留的未提交草稿继续完成集成。新增 `3a8c01c`（clone-fd session + AU parser）、`47b44c5`（MM singleton / camera桥）、`8fda515`（VT persistent session）、`df48be6`（camera入场前先验证codec队列记录）。没有将旧树覆盖main；尚未合main或清理此活动树。前面“VT仅独立IDR”的描述是恢复基线，不再代表当前前端源码，但真实提供者仍缺席。
- **Kernel session真实入口**：`/dev/x9-vdec` 使用devfs clone minor；每fd拥有一个native codec，fork/dup按XNU specfs的最后close销毁，不以PID猜寿命。原v1 IDR ioctl保留；新增CAPABILITIES/CREATE/SUBMIT/RECEIVE/DRAIN/DESTROY与strict generation、唯一token、max_pending、held output和payload copyout重试。明确generic BSD ioctl最终**结构**copyout不在driver事务内，不能承诺该步骤失败可重复退休。session_ops提供者负责真实codec DPB和display-order输出；host仅记录输入身份，不做软件reference/POC排序。AU gate新增base-layer非IDR与IRAP、SPS DPB/reorder字段；仍限定单VPS/SPS/PPS、Main/Main10/Main-compatible profile3、4:2:0、4096维度，并非所有HEVC profile/container功能完成。
- **MM实际适配代码**：`x9_mm_smmu_owner.c/.h`已进recipe/link-order；只adopt真实已安装的normal storage，逐16K页pmap核对、保存完整L1/首L2、pins共享power，a0/b0分配非camera的ASID。只允许target STE的BBM与准确CFGI/CD/TLBI/SYNC四条batch；不写STRTAB/CMDQ/CR0全局配置、不SMC/HVC探测或接管。display90及其它STE按实际ledger保留。两份whole-domain lease不是每VT frame分配，释放后保留cookie tombstone，不在同boot换epoch重新授权。camera可选paired `stream_guard/complete`使用同锁/ledger；仅新operation边界接受owner已验证的empty completed queue，pending/fault不rebase；入场前仍验证preceding codec queue record，camera操作deadline/失败也通知singleton quarantine。旧无callback host的sole-producer策略不变。**这是installed-storage适配，不是已完成cold NS bootstrap**；没有现实authority注册者或自动adopt调用。
- **VT已接实际dispatch**：`vt_video_sessions.inc`进入实际vtable decode/finish。start创建native session，AU submit后按原token关联VT frame与该帧immutable CM description，重排输出使用原帧ICC/color/HDR，不借用当前输入metadata。DoNotOutput仍解码/维持引用但不交付pixels；DROPPED需真实退休。只有EnableTemporal允许延迟，否则同步drain；DRAIN契约必须保留DPB/parameter sets，不得以每帧重建IDR替代。RECEIVE合计5s、DRAIN另有5s限，不按每输出再乘5s。接受后的错误显式完成原token，不让VT duct重复发当前帧；recursive mutex允许同线程callback invalidate，拒绝emission中的嵌套decode，CM对象自retain跨callback寿命。普通VT仍被RequiredSpecification排除；当前显式key改为`X9NativeHEVC`，DecoderID=`org.ios-sweet.x9.vcp.hvc1`，注册还必须匹配真实session caps。不能用此前仅IDR provider冒充视频会话。
- **本次确切编译**：`sweet_x9_vdec.o`在`hevc-sessions-compile-20261115`通过；该首轮camera因本树缺ignored sensor include失败，未进入camera源码编译。用正式`x9/camera/build.py:sensor_tables`及固定`.local/vendor`生成include后，`hevc-owner-compile-20261115`的owner/camera两对象均exit0（20/107 warnings）。最终修改的owner按`df48be6`再编译exit0/20 warnings，日志/helper/result在 **`x9/out/hevc-owner-boundary-final-20261115/`**。最终VT dylib/client全编译、链接、ad-hoc签名exit0，`-Wall -Wextra -Werror`，在 **`x9/out/hevc-session-final-20261115/frontend/`**，原console/exit为同目录上级`frontend.log/frontend.exit`（源码`8fda515`）。未重跑未改动的成功对象；没有测试套件、full kernel link、boot pack、CI run（`gh run list --limit 3`为空）、构建后二次解包/hash/反汇编，或手机/MMIO/SMC/ADB/mailbox/分区操作。编译成功不证明VT/runtime或物理DMA行为。
- **下一具体缺口/完整目标active**：真实NS/controller/firmware/clock owner与最高层create→install→shared→bind→start尚缺，不能把callback契约/adapter当注册者。当前`x9_vdec_control`仍是一份output/一次START的独立实例，尚未实现持续session_ops；需要在真实decoder/VSI/BS路由证据下补连续submit、WAITISR/c006、coherent completion/ring dequeue、input/DPB/output cache生命周期、线性NV12/P010和退休。P01H/HYFBC不能改标签当P010。之后接普通VT/CMPhoto/ImageIO进程路由及HEIF grid/方向/ICC/alpha全链；单sets/4K gate也仍需按真实capability扩展。此轮无新boot.img，唯一最终功能镜像仍须等**完整代码齐全**，不出探针、不称HEVC/HEIF已可用。

#### 2026-11-15 Native SHM cache续接（当前里程碑）

- **活动树不变**：`.worktrees/hevc-hw` / `feat/hevc-hw`，接`e71e552`；没有修改main或共享`.local`布局。
- **新观察**：原厂`vcp_helper.c:vcp_alloc_iova`明确取`mtk_mm-uncached`，`mtk_system_heap.c:system_heap_do_vmap`对uncached使用`pgprot_writecombine(PAGE_KERNEL)`，不是前面secure dump的`ioremap_wc`分支。恢复的原件`vcp_v2.ko`有`mtk_mm-uncached` / `vcp_uncache`，实际`vcp_reserve_memory_ioremap`内有heap分配→`dma_buf_vmap_unlocked`→attach/map调用；定向原件反汇编保存`x9/out/hevc-vendor-source-20261115/vcp-normal-shm.asm`。这不证明当前loaded pairing、冷启动或DMA authority。
- **实质改动**：global SHM完整slice在CPU_PRIVATE、arena未install时，通过XNU的`pmap_set_cache_attributes`转`VM_WIMG_WCOMB`（Normal NC，MAIR0x44），预核对每16K页PA和DEFAULT属性，保存恢复义务再变更；使用受管pmap API同时更新physical aperture/PV mappings，不建WB/NC别名、不改live页/PTE。每次SHM观察检查NC属性/当前CPU MAIR，改用NC loads+外域barrier，不再clean firmware更新的VSI cacheline；仍不承诺multiword snapshot原子性或退役。DEVICE仍不能free；私有失败/真实whole-domain退休后free先验证DEFAULT/WCOMB，再恢复DEFAULT，异常保留handle。普通BS/FB/页表仍原有WB/cache协议。
- **确切编译**：首轮`hevc-shared-nc-20261115`因export headers缺两条kernel-private pmap声明失败（6 errors），补与XNU原型一致的extern后，最终`x9/out/hevc-shared-nc-final-20261115/compile-native.py`只编修改的`sweet_x9_vdec.o`，exit0、32 warnings；原日志/exit/result保存在该目录。没有测试套件、成功产物二次hash/反汇编、full kernel link、CI run或boot，也没有设备操作。
- **下一步仍是代码而不是探针**：用这个真实NC storage owner补AP-owned VSI精确字段写入与native ring消费；main/non-ACP BS地址不能假设相等，原厂non-ACP缺席分支为0也不证明main路径已授权。持续session backend、cold NS/bootstrap、WAITISR/c006、DPB与线性输出、普通VT/CMPhoto/HEIF全链仍未完成，目标active。
- **NC里程碑已交付**：提交`a219ead`已push到`origin/feat/hevc-hw`。

#### 2026-11-15 Native VSI精确写入（最新交接）

- **代码变化**：新增内部`vshared_live_window`，只返回已保留的DEVICE global slice的真实NC CPU window，重新核对PA/NC/本CPU MAIR/power；不导出通用live写接口或猜测firmware→CPU指针。INIT在slot唯一绑定后只写捕获的AP `ipi_blocked(+0x1228)=0`并保留`slot_initialized`，失败不丢binding。START在真实bus整call reservation内publish BS、记录真实handle/IOVA后，从live NC观察解码模式，再调用新的`hardware.qualify_start`核验实际firmware/ABI、main normal codec route（ACP关闭且无独立non-ACP attach）、layout/clock/IRQ。原来无映射的`hardware.prepare_start`已替换；没有现有调用者需要迁移，也未添加成功空实现。
- **真实publisher**：资格核验成功后由native代码执行aligned volatile NC scalar stores，仅`bs_iova(+0xe80)`、缺席`bs_non_acp_iova(+0xe90)=0`、`bs_token(+0xf18)`、`timestamp(+0xf48)`和`dynamic_low_latency(+0x1004)`；`dsb oshst`在a001发送前。只支持实际`INPUT_DRIVEN_PUT_FRM=2`，输出来自之前真实a005；其它模式失败关闭，绝不让equal snapshots充当排他授权。FW的FB、error、mode、plane_count等字段不写、不复制整slot、不clean邻居cacheline。新增`vsi_attempted/vsi_staged`仅表示写入，不表示b001/pixels/退役；`decode_start`内核接口增加raw timestamp及0..255 latency参数，以后session backend需实际传入。
- **本次确切编译**：中间`hevc-vsi-store-20261115`exit0/36 warnings（有4个新增volatile/alignment warnings）；改成显式volatile snapshot loads和明确aligned scalar访问、补slot8B对齐gate后，最终`x9/out/hevc-vsi-store-final-20261115/compile-native.py`只编修改的`sweet_x9_vdec.o`，exit0/32 warnings。原stdout/exit/result在该目录。未重跑未改前端、owner或camera；无测试套件、产物二次检查、full kernel link、CI run、boot或设备操作。
- **后续边界仍明确**：上述native START路径没有现实qualify/provider注册者，因此READY仍不能发布。control仍单output/一次START，不是持续session_ops。原厂FB共享transfer更新read与派生count，BS consumer更新read/count；NC解决cacheline覆盖但不证明producer排他，不能用两次相等snapshot直接消费/退役。下一步要补真实ring dequeuer、token/DPB/input/output lifetime和WAITISR/c006，以及cold NS/firmware/clock最高层；普通VT/CMPhoto/HEIF、grid/方向/alpha与线性NV12/P010仍未齐全。无新boot，目标active，不称完成。
- **VSI里程碑已交付**：提交`316abfb`已push到`origin/feat/hevc-hw`。
- **Ring观察窄修复**：fixed ABI/kind/slot-size下，immutable ring parser的`EINVAL`仅是count与(read/write) distance不一致。捕获的FW publisher分开写write-index/count，即使两份NC snapshot相等也可能停在中间；`vcontrol_recheck`现在将这一项视为`EAGAIN`，保留真实notification与pending，不修metadata、不消费、不称completion。ERANGE/满环EOVERFLOW仍失败关闭，offline parser契约不变。仅修改native源码再编译，`x9/out/hevc-ring-observation-final-20261115/compile-native.py` exit0/32 warnings，stdout/exit/result同目录；无其他构建、测试或设备动作。
- **停止时恢复基线**：上述ring修复`41de9e6`已push；本次代码依次为`a219ead`、`316abfb`、`41de9e6`。仍在活动工作树，未merge main；没有运行中的编译/设备任务。下一具体工作是native ring消费与持续session/DPB/completion（不能以观察结果授予退役），并补现实cold NS/firmware/clock注册者。所有前面未完成项仍保留，目标active。

#### 2026-11-15 FREE_BS 满环 ABI 修正（当前交接）

- **新静态证据**：只读既有备份 `firmware-originals/vcp-real-android-active-b/vcp_b.img`，提取其 RV55 DRAM payload（2057906 B，link base `0x200000`）。FB publisher `0x30a074..0x30a13c` 的 full gate 是 `((write+1)&63)==read`、最大63项、count由distance派生；新定位 FREE_BS publisher `0x30b264..0x30b30e` 则在 `0x30b2ae/0x30b2b6` 以 shared count==64判满，stride8，token写入后先更新write-index、再写shared count+1。因此 **BS read==write/count64是FULL，不是empty**。原厂 `vdec_vcp_if.c:vdec_get_bs` 按count0判空，read-index++及count--，与FB不是同一个消费协议。此前“所有shared ring count64都是EOVERFLOW”描述只适用于FB，已由此证据纠正。
- **实际代码**：wire immutable parser允许BS full并读head，FB count64仍EOVERFLOW，其他count/distance不一致仍EINVAL；不增加live写入/消费。`vcp_artifacts.py`在原完整payload身份门内记录BS发布指令spans、独立满环/count语义，并把旧泛称键限定为 `shared_fb_count64_not_host_full_ring`。**BS count是FW++/AP--跨端RMW**，NC映射、两次相等snapshot、b001或AP bus reservation都尚不能证明producer exclusion；不得借此开始持续消费或退休。
- **可复查产物**：当前工作树 `x9/out/hevc-ring-producer-20261115/{rv55-dram.bin,source.json,fb-publisher-mapped-r2.asm,bs-publisher-r2.asm,output-profile-bs.json}`。raw文件要用 `r2 -a riscv -b 32 -m 0x200000 -e scr.color=0 -qc 'pd 105 @ 0x30b23a' <rv55-dram.bin>`；首轮仅设bin.baddr得到ffff的输出无效，未作固件证据。custom opcodes保留unknown，不推断完整wrap/控制流。本轮重跑定向静态 `rv55_output_evidence(dram,rv55_init_evidence(dram))`，profile_match=true、27 spans，输出新文件，未覆盖旧source.json。
- **确切编译/下一步**：仅修改wire对象按正常Builder交叉编译，`x9/out/hevc-bs-ring-abi-20261115/{compile-wire.py,stdout.log,compile.exit,result.json}`，exit0/2 warnings；diff check通过。没有测试套件、CI run、full kernel link、新boot或设备操作，也未二次检查成功构建产物。下一动作是追 `0x30b264` 调用者与返回/异步时序，确认真实producer排他边界，再分别实现BS/FB durable host journal和live消费；持续session backend、cold NS/firmware/clock注册者、DPB/completion、线性输出和普通VT/CMPhoto/HEIF仍未完成，目标active。
- **ABI里程碑交付**：上述代码与交接提交 `b8e8b1d`，已push到 `origin/feat/hevc-hw`。

#### 2026-11-15 START 后异步 bus/native service（最新交接）

- **新增真实路径**：`x9_vcp_bus_service()` 复用 `call_begin/check/end`，整batch保留OUT0/IN1与bus寿命，先drain owned RX，再在软件队列空时进行一次物理poll。按相同strict parser处理b005/e000并调用真实notification；其它包走现有SHM/alloc/clock/WAITISR/VP reverse dispatcher及物理reply。不发送新a001/a005、不等待新的控制ACK；stale ACK/未知token或message失败关闭并保留last_rx，c006仍ENOTSUP。只有已经建立的service/成功INIT才能入场，不以pump重新bootstrap。每batch最多64包，单个1..3000ms absolute deadline覆盖所有callback/reply；空RX立即正常返回，不idle spin到timeout，达到包数上限时剩余软件包/physical latch留给下一batch。
- **Native确实接入，不是空handler**：`x9_vdec_control_service()` 使用当前bound实例、既有native SHM/alloc/VP及实际hardware clock/wait handler，持有control lock并调用上述bus路径，b001返回后仍可服务。新增native `recheck` 在同一reservation尾部（包括空batch）重看retained transient ring observation，不伪造notification、不清pending当完成；累积真实batch/packet计数。reservation前EBUSY不毒化decode；非EBUSY硬错误及reservation后的错误保留fault/resources。**尚无持续session owner循环调用者或session_ops provider**，不能把此有限pump称完整跨帧会话；没有自动启动worker/发布READY。
- **继续静态追BS**：在原完整DRAM身份门下验证 `0x306bfa`（JAL）和 `0x30afb6`（C.JAL）到 `0x30b264` 的局部指令边界。前一路 `0x306be8..0x306c0a` 是query→publish→release→jump `0x305fa4`，后者构造并发送b001；另一路 `0x30af9a..0x30afc6` 是对象比较→query→publish→release→loop。上层还找到 `0x30b71a→0x30a432` 等标准call，但尚未证明全部custom/indirect/其它core调用根或固件线程排他，**b001仍不是FREE_BS consumer RMW授权**。`vcp_artifacts.py`新增这两段指令/调用边并明确AP count--来自stock参考源码，不能说是captured loaded AP行为。
- **复查产物**：`x9/out/hevc-bs-callers-20261115/{source.json,find-callers.py,find-target-callers.py,candidates.json,producer-callers.asm,ack-and-operation.asm,operation-prologue.asm,operation-callers.asm,output-profile-final.json}`；后者profile_match=true/29 spans。原件未修改；另从同一容器offset512提取184320B的`rv55-tcm.bin`供后续定向xref。scanner只找halfword-aligned标准JAL/C.JAL/C.J/AUIPC+JALR和pointer候选，不能把全文件scan当完整call graph；保留unknown ISA，不采用首轮错地址string输出作语义证据。可复查 `python3 -B x9/out/hevc-bs-callers-20261115/find-target-callers.py 0x30b694` 及 `r2 -a riscv -b32 -m 0x200000 -e scr.color=0 -qc 'pd 140 @ 0x30b610' x9/out/hevc-ring-producer-20261115/rv55-dram.bin`。
- **确切编译/范围**：正常Builder只编改动的 `x9_vcp_bus.o` / `sweet_x9_vdec.o`，`x9/out/hevc-async-service-20261115/` exit0，2/32 warnings。随后仅native错误记账小改动，`x9/out/hevc-async-service-final-20261115/compile-native.py`再编sweet对象exit0/32 warnings，stdout/exit/result同目录。无测试套件、XNU对象树重建、full kernel link、CI run ID、新boot或设备操作；不对成功编译产物二次解包/hash/反汇编。未合main，没有后台构建/手机任务。下一具体工作仍为BS真实producer排他/FB独立消费→durable token journal→continuous session/DPB/cache/completion；cold NS/firmware/clock最高层、线性NV12/P010及普通VT/CMPhoto/HEIF全链继续保留为未完成，目标active。
- **恢复提交**：异步bus/native service及上述静态证据为 `6c5dc38`，已经push到 `origin/feat/hevc-hw`；ABI修正前置为 `b8e8b1d`。本条为NOTES-only交付补记，不改变编译源码/结果，没有新增镜像或物理验收。

#### 2026-11-15 多参数集真实输入续接（最新交接）

- **活动树/源码**：仍为 `.worktrees/hevc-hw` / `feat/hevc-hw`，恢复基线 `e09797c`。审核并完成上一轮未提交的 parser 草稿，源码提交 **`b7dd6b0`**。未改 main、共享 `.local` 链接或长期输入，未启动子代理。
- **输入链实质变化**：hvcC 不再限制每类恰好一个 VPS/SPS/PPS；按真实 type/ID 建最多16+16+64个条目的 catalog，逐项保留原 offset/bytes 和依赖。相同 ID/相同 bytes 去重，同 ID/不同 bytes 拒绝；按 VPS→SPS→PPS 稳定排序，检查所有 PPS→SPS→VPS 引用存在后才选一条代表链，移除了草稿中缺失引用时先解析无关 first SPS 的 fallback。独立 IDR 和 session-create Annex-B 都输出**全部**保留集；AU 可使用任一保留 PPS，但同一 picture 的 slice 必须同 PPS，in-band PS 必须逐字节匹配保留集。`vdecctl` 的真实 `CMVideoFormatDescriptionCreateFromHEVCParameterSets` 调用同步传全部 catalog，不再静默丢回三份；VT 与 kernel session 原有实际 parser 调用自动使用新逻辑。
- **边界/栈**：所有 SPS 目前仍须有相同 coded/visible geometry、crop、depth、profile、level、layer count/decoder profile，DPB/reorder取各SPS最大值并进入原capability gate。尺寸切换、Rext/alpha、多profile仍未完成，4096是现实现 gate 而非硬件能力证明。legacy ps spans/IDs仅表示一条真实关联链，不表示只剩一组。catalog entry为12B，临时SPS使用独立64B `sps_info`，不在每轮分配整个catalog config；所有窄ID转换在明确范围检查后进行。catalog/helper检查计数、type/ID/reference、reserved、跨度、NAL长度及Annex-B总长；没有用解析成功授予物理硬解或READY。
- **确切编译**：首轮完整 iOS dylib/client编译、链接、ad-hoc签名exit0，在 `x9/out/hevc-multisets-20261115/`；首轮正常Builder `compile_one(sweet_x9_vdec.o)` exit0/38 warnings，在 `x9/out/hevc-multisets-kernel-20261115/`。修正三处已范围校验的窄整数转换告警后，最终完整frontend **exit0（`-Wall -Wextra -Werror`）**，`x9/out/hevc-multisets-final-20261115/{frontend/,frontend.log,frontend.exit}`；最终仅sweet对象 **exit0/result=True/35 warnings**，`x9/out/hevc-multisets-kernel-final-20261115/{compile-native.py,stdout.log,compile.exit,result.json,build.log}`。parser直接include在sweet源码中，recipe没有独立parser对象，故没有另编猜测对象。最终日志为31个declaration-after-statement和4个既有session ioctl的cast-align告警，无窄转换告警；没有另开frame-size分析或产物复检。原日志不覆盖，不做成功对象二次反汇编/hash/产物对比。
- **最小复编**：在活动树用新唯一输出名执行 `python3 -I x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/<新名>/frontend`；kernel用上列最终 `compile-native.py` 模板，将脚本内out改为新唯一目录再运行 `python3 -I <新目录>/compile-native.py`，关闭ccache、按正式recipe选sweet对象。没有测试脚本/套件、full kernel link、XNU树重建、新boot或硬件decode验证；`gh run list --limit 3 --json databaseId,status,conclusion,headSha`返回空，无CI run ID。
- **上一轮BS静态调查补记/仍不授权消费**：原DRAM `0x30a432` 操作函数候选的标准call包括 `0x307964/0x30812c/0x308392/0x30b71a`；`0x30b694`内`0x30b71a→0x30a432`，其callers含`0x306a2a/0x30b176`。`0x30b0ac`是pending-operation bit dispatcher，bit2分支到`0x30b694`；`0x246d46`是转TCM `0xc31c/0xc40a`的tail wrapper，所见全局计数/cfg+0x6c修改**不能称BS mutex**。旧候选 **`0x308342`不是函数入口，而是AUIPC/JALR对的第二条**，对它scan无caller不构成callgraph结论。stock `ipi_blocked`仅AP job admission/stream-stop gate，`ctx->buf_lock`也未证明锁住FW publisher，不能以二者开始count--。静态窗口保留 `x9/out/hevc-bs-exclusion-20261115/{dispatch-root-windows.asm,serialization-candidates.asm,tcm-dispatch-and-helper.asm}`；原件未改，unknown RV55 custom指令不猜。复查DRAM用 `r2 -a riscv -b32 -m 0x200000 -e scr.color=0 -qc 'pd 140 @ 0x30b610' x9/out/hevc-ring-producer-20261115/rv55-dram.bin`；TCM raw映射base0，输入 `x9/out/hevc-bs-callers-20261115/rv55-tcm.bin`。
- **恢复点/未完成**：本里程碑只移除单参数集输入限制，**完整HEVC/HEIF目标保持active**。BS producer排他、FB独立消费/durable token journal、per-buffer completion/cache/退休和c006真实free/ACK、持续native session_ops provider、cold NS/controller/firmware/clock最高层注册者、DMA容量/完整DPB、线性NV12/P010及普通VT/CMPhoto/ImageIO→HEIF grid/方向/ICC/alpha仍待完成。普通VT gate/READY/profiles不放开，未合main。源码`b7dd6b0`及交接`0b07577`已push成功到`origin/feat/hevc-hw`（`e09797c..0b07577`）；本条告警分类更正随后作为NOTES-only提交正常push，不重编未变源码。没有后台编译、CI、设备或子代理任务；不生成探针或最终boot，唯一最终镜像仍等完整代码齐全。

#### 2026-11-15 Native 多缓冲 journal / 发送前 RX 基线（当前交接）

- **活动树/本轮范围**：继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，基线 `74e359c`。审核并完成恢复的五文件草稿；没有修改main、共享`.local`输入或启动子代理。完整HEVC/HEIF目标仍active，READY/profiles及普通VT gate不放开，未生成探针/最终boot。
- **RX相关性更正与实改**：此前control发送后才drain旧RX，同实例旧b001可能被新START误匹配，旧交接中泛称“不接受stale ACK”过强。现在START/a005/已初始化service的QUERY均在whole-call reservation内、prepare/send之前先drain软件RX并物理poll确认empty；已排队b000/b001/b100返回EPROTO，真实b005/e000和reverse请求由统一dispatcher实际处理。最多64包未取得empty baseline则EOVERFLOW、不prepare或发送新请求；同一个absolute deadline。**baseline后才到达的delayed duplicate ACK仍不可辨别**（协议无picture sequence），实际FW仍须遵守串行命令一条回复；RX empty不是FW/BS/DMA排他或退休证明。
- **真实多record资源寿命**：每个control动态保留64个input及64个output journal（实现界限，不是硬件/DPB容量）。每次成功control之后可新a005/START，每次用fresh token/index/slices；旧DEVICE backing全部保留，不因b001释放。全部output暂要求相同plane count/capacity，未实现尺寸切换。partial allocation/publication/VSI写入、真实handle/extents、阶段、result、call log与原errno均入journal；失败/不确定call停止新admission。qualify_start前复核所有旧input/output handles仍DEVICE；discard仍须STOPPED及两域RETIRED，遍历保留的全部slices。新增get_input/get_output只返回历史host snapshots；resources.output仅为**最近offered FB，不是当前input最终选用/display的FB**。
- **ring观察连接实际journal**：head token必须匹配本实例prepared record，否则ESTALE；不把token cast为指针。FREE_BS/FREE_FB/DISPLAY_FB raw snapshot与observed位留到该record，**不改read/count、不授予CPU访问、退休或pixels**。count/distance transient按ring独立解析，BS的这一暂态不阻断可解析FB，最新status带ring_valid_bitmap；whole-slot双snapshot仍保守要求相等，变化保留pending。输出prepare只检查COMMON；START/VP只比较两份已证据化DECODE_FIELDS，避免无关异步ring变化阻断准备；相等仍不是原子/ownership证明。
- **确切编译与审核**：中间 `x9/out/hevc-multibuffer-20261115/` 两对象exit0；最终bus/native `x9/out/hevc-control-journal-final-20261115/{compile-service.py,stdout.log,compile.exit,result.json,build.log}` exit0，两项True，bus2/sweet41 warnings（总39 declaration-after-statement＋4旧ioctl cast-align）。之后只改native journal errno/错误优先级，最终仅sweet对象 `x9/out/hevc-control-journal-native-final-20261115/{compile-native.py,stdout.log,compile.exit,result.json,build.log}` exit0/True/41warnings。当前最终diff静态审核及git diff --check通过，未因恢复审核重复编译未变源码。无测试脚本/套件、full kernel link、XNU重建、成功产物二次解包/hash/反汇编、设备或分区操作；`gh run list --limit 3 --json databaseId,status,conclusion,headSha`为空，无CI run ID。
- **FB静态证据补记**：RV55 publisher候选 `0x30a06e` 的标准calls为 `0x307a16/0x309876/0x3099ec`，后两处spans加入vcp_artifacts身份门。定向结果 `x9/out/hevc-fb-consumer-20261115/{publisher-callers.asm,publisher-candidates.json,output-profile-fb.json,r2.log}`，profile_match=true/31spans。原件不动，复查：`r2 -a riscv -b32 -m 0x200000 -e scr.color=0 -qc 'pd 64 @ 0x3079e0; pd 140 @ 0x309806; pd 145 @ 0x309946' x9/out/hevc-ring-producer-20261115/rv55-dram.bin`；最后window起点可能在前指令内部，只采用后续已确认标准边界，unknown custom保持未知。**完整producer根、FW→AP发布顺序/barrier及安全live消费边界未证实**，BS跨端count RMW排他也未闭合。
- **下一具体工作/完成边界**：当前仍没有session_ops provider或持续service caller；多fresh缓冲不是完整持续硬解。下一步实际补FB安全live消费/BS producer排他→durable dequeue与input/DPB/output completion、cache handoff、per-buffer退休及c006真实free/ACK；再接持续session、cold NS/controller/firmware/clock最高层初始化/注册、DMA容量/完整DPB与线性NV12/P010、普通VT/CMPhoto/ImageIO及HEIF grid/方向/ICC/alpha。当前未合main；没有后台构建、CI、设备或子代理任务，唯一最终boot继续等全部代码齐全。

#### 2026-11-15 逐ring双snapshot与异步flag静态收束（最新交接）

- **前一里程碑已交付**：多record journal / RX baseline代码及交接提交 **`d8f4a527`** 已正常push成功到`origin/feat/hevc-hw`（`74e359c6..d8f4a527`）。本轮接着发现前述“独立解析”仍被whole-slot相等检查前置阻断，不能泛称所有BS变化都不影响FB；本条修复覆盖这个剩余范围，活动树/长期输入不变。
- **确定观察路径修复**：RING_FIELDS仍取两份真实NC immutable slot及相等/无fault的COMMON，但不再比较全部slot。每个BS/FREE_FB/DISPLAY_FB各自解析两份metadata/head，只有该ring相等时更新valid_bitmap及token匹配journal；count/distance transient或两份不同仅令该ring pending，继续检查其它ring。ERANGE/FB非法full仍失败关闭，BS合法64仍沿原ABI。不比较empty ring的stale entry或head以后的未读slot；parser清零padding，host结构比较确定。结束再核absolute deadline。**这是减少观察饥饿，不是原子snapshot、read/count消费、cache交接、硬件completion或退休证明**；START/VP的DECODE_FIELDS资格检查未放宽。
- **新静态字段证据**：`0x30978c..0x309798` 是标准 `lui a2,1; li a3,1; addi a2,2; add a2,s8; sb a3,0(a2)`，之后 `0x30979a→0x309896` 走e000 sender，即发送前instance+`0x1002`字节置1。原厂 `vdec_ipi_msg.h` 该邻接字段名为output_async_handling，`vdec_vcp_if.c:864`的e000分支在FB transfer前清0；当前固定输入没有捕获的decoder KO，不把source清0提升成已验证AP指令或配对runtime。`vcp_artifacts.py`仅给已有完整payload身份门增加这项raw offset/value与显式未证明边界；**不能以flag置1/清0当BS/FB producer exclusion，更不自行清live flag或造消费成功**。
- **原始/派生证据与确切编译**：新只读窗口 `x9/out/hevc-fb-consumer-20261115/{async-handling.asm,async-handling-r2.log}`；复查 `r2 -a riscv -b32 -m 0x200000 -e scr.color=0 -qc 'pd 160 @ 0x3096d4' x9/out/hevc-ring-producer-20261115/rv55-dram.bin`，unknown custom及其被r2拆出的假指令仍不赋语义。新定向静态profile `x9/out/hevc-per-ring-observation-20261115/output-profile.json` 为profile_match=true/31 spans，没有覆盖旧结果。仅真正修改的sweet对象用正式Builder.compile_one、关闭ccache编译，**exit0 / True / 41warnings**，精确 `x9/out/hevc-per-ring-observation-20261115/{compile-native.py,stdout.log,compile.exit,result.json,build.log,source.diff}`。diff check通过，没有重编未变bus/wire/frontend，也没有测试套件/full link/CI/boot/产物复检/设备操作。
- **当前恢复点/下一动作**：逐ring修复与上述字段证据提交 **`9feae058`** 已正常push成功（`d8f4a527..9feae058`）；此交付补记仅改NOTES，不重编源码。没有后台任务或子代理。继续从真实FW的output notification flag读取/producer根与BS操作退出时序收束安全live transfer，不用field equality或b001授予退休。持续session_ops provider/caller、per-buffer DMA/cache/DPB寿命、c006实际free/ACK、cold NS/controller/firmware/clock最高层注册、线性NV12/P010与普通VT/CMPhoto/ImageIO及HEIF grid/方向/ICC/alpha仍未完成。READY/profiles/普通VT gate不放开；未合main，无新boot，完整目标active。

#### 2026-11-15 HEIF coded item → CoreMedia 输入链（当前里程碑）

- **活动树/基线**：继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，接已push的 **`8f5fe8fe`**。审核并完成六文件未提交草稿，不修改main、共享`.local`布局或长期输入，不启动子代理。完整HEVC/HEIF目标保持active；READY/profiles/普通VT gate不放开，无新boot或探针。
- **实际输入链**：新增 `heif_items.c/.h`，以borrowed immutable文件解析ftyp/meta/pitm/iinf/infe/iloc/iref/iprp/ipco/ipma/idat，支持64-bit box、infe2/3、pitm0/1、iloc0/1/2、iref0/1和窄/宽ipma。真实拼接method0文件及method1 idat的base+extent，复制前验证全部extent/容量与禁止覆盖原文件；保留有序dimg/essential属性以及auxl枚举。hvc1/grid/iden的ispe、grid descriptor及child数检查已经实际实现；未知essential、protected/external/method2、implicit zero-length extent仍明确拒绝。file256MiB/items256/properties512/associations32/extents32/references1024/item data8MiB/item metadata4MiB均是实现界限，不是硬件上限。
- **真实CM所有权/metadata**：导出 `X9HEIFCopyItemSample(file,item_id,sample,info)`，id0选primary，只选择coded hvc1；读取item后走现有hvcC/独立IDR gate，将全部参数集传CM建format，再按type/ID/raw NAL与length framing核对CM产生的catalog。ispe/SPS/CM尺寸、pixi channel/depth必须一致；AU拷进CM-owned block，info复制有序原属性和辅助IDs，释放文件不让sample悬挂。ICC和nclx都保留，不猜ColorSync优先级；CICP通过SDK exact mapping，未知值留raw，gamma4/5显式2.2/2.8，不造BT709默认。方向/clap/auxiliary只是保留，未应用；Rext单色alpha仍不支持。
- **CLI/完成边界**：`heif-info file.heic`只读primary/dimg/auxl（32层active-stack cycle检查）；`decode-heif-item file.heic item-id-or-0 NEW.yuv`实际调用上述CM入口，再复用指定DecoderID/RequireHardware/UsingHardware和真实callback线性pixel gate，输出明确是**选中的raw coded tile，不是composed HEIF image**。不注册假硬件、不软件fallback。最后审核补齐config/IDR gate失败的OSStatus，避免预建sample成功后误报0。build manifest记录coded-item input=true、complete-image compositor=false。
- **确切编译/静态结果**：首次完整frontend `x9/out/heif-item-input-20261115/` exit0，首次Clang analyzer `x9/out/heif-item-static-20261115/` exit0/无诊断。catalog roundtrip、metadata容量与gamma后的完整iOS dylib/client编译/链接/ad-hoc签名 **exit0 / -Wall -Wextra -Werror无诊断**，`x9/out/heif-item-input-final-20261115/{frontend/,frontend.log,frontend.exit}`；同目录analyzer.log空/analyzer.exit0，分析parser/CM源码而非运行测试。最终CLI一行OSStatus修复仅重编受影响client，复用上述未变dylib，**exit0/无诊断**，`x9/out/heif-item-client-final-20261115/{compile-client.py,client.log,client.exit,vdecctl}`。单client目录不含dylib，若要使用需与上列dylib同目录；没有自动部署。git diff --check通过；此前gh run list返回空，本里程碑无CI run ID、kernel变化/编译、full link、新boot、测试套件、成功产物复检或设备/分区操作。
- **复编/下一具体工作**：完整frontend用 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/<新唯一名>/frontend`。继续HEIF完整图的cycle/geometry、grid/iden composition及属性顺序下clap/irot/imir，再补ICC/CICP最终处理与aux/alpha；method2/更多合法输入尚未实现，不能称完整HEIF。Native ring安全live消费/producer排他、durable dequeue、cache/DPB/per-buffer退休、c006真实free/ACK、持续session_ops provider/caller、cold NS/controller/firmware/clock最高层、DMA容量与真实线性/压缩转换、普通VT/CMPhoto/ImageIO进程路由仍未完成；唯一最终boot继续等全部代码齐全，未合main。

- **输入链已交付**：上述七文件（六代码文件＋NOTES）提交 **`802773d9`**，已正常push成功 `8f5fe8fe..802773d9` 到`origin/feat/hevc-hw`。最终CLI复编包括最后一行错误状态修复；没有用前一完整frontend构建冒充这一行已编译。

#### 2026-11-15 HEIF 派生图geometry与packed raster compositor（当前交接）

- **范围/基线**：继续同一活动树，从 **`802773d9`** 实际新增 `heif_compositor.c/.h` 并接入frontend/CLI；没有修改native kernel、main或固定`.local`输入，不启动子代理。完整目标active；READY/profiles/普通VT gate关闭，不生成最终boot或探针。
- **真实图检查**：`x9_heif_plan_create` 先验证整个可达dimg/auxl图，再允许pixel callback；active-state cycle与32层路径上限，重复DAG子树仍按其完整height计入新路径，不能用seen/cache绕过上限。grid以child应用自身transform后的尺寸检查所有tile一致、row-major数量与覆盖（最后一行/列必须相交，不留空洞、不允许多余整行列）；iden基底ispe必须等于其唯一child最终geometry。每个节点按ipma属性原顺序处理clap/irot/imir，crop为精确整数rational origin/extent；signed offset与所有uint32 denominator用有界int64算术，不截断、浮点近似或猜测。分数size/origin明确ENOTSUP，超界crop拒绝。原metadata通过新 `x9_heif_span_data` 向**同一document**借用bounded span，不接收另一未经核对的file pointer。
- **真实pixel代码，不是空接口**：`x9_heif_compose_packed` 已实际执行coded source callback→cached raster→grid行拷贝/最后边界裁切、iden copy以及顺序crop/逆向索引quarter-turn/mirror。重复dimg每次placement都保留，但同item每次composition只取source一次；各transform新buffer后释放旧buffer，任一错误丢弃全部partial/cached输出。支持RGBA8/16LE/half/float与gray8/16LE的原样packed sample搬运，不压缩解码、不隐式色彩转换。全部cached/intermediate buffer共享256MiB预算，逻辑dimension16384/items256/depth32均为实现界限。公开头是**内部backend core**，不冒充ImageIO/CMPhoto入口或硬件认证；source必须提供caller显式选择的同一working color/component/alpha约定，out为zero-initialized malloc ownership，close释放。
- **aux/颜色/真实caller边界**：plan保留并验证aux图，但compose只要发现任一reachable辅助关系，就在第一次source callback前ENOTSUP；没有丢弃alpha/gain/depth后返回完整图。**native VT→working packed raster adapter仍未实现**；当前没有实际hardware callback调用此pixel compositor，也没有ICC/CICP的最终working-space选择、fractional clap resampler或alpha合并。因此这只是完成derived geometry与真实packed composition算法，不能称HEIF硬解端到端完成。`heif-info`现在实际先走plan，再打印每节点ordered最终尺寸；仍明确没有decode/transform pixels/aux merge或hardware success。manifest新增derived_geometry/packed_raster_compositor=true，但native_raster_adapter/auxiliary_merge/complete_image_compositor=false。
- **确切编译/静态结果**：首次完整frontend `x9/out/heif-derived-raster-20261115/{frontend/,frontend.log,frontend.exit}` exit0。最终源码审查补齐cached subtree depth、同document span、已有owned out保留及live预算/placement gate后，完整iOS dylib/client编译/链接/ad-hoc签名 **exit0 / -Wall -Wextra -Werror无诊断**，`x9/out/heif-derived-raster-final-20261115/{frontend/,frontend.log,frontend.exit}`；同目录 `analyze-source.py/analyzer.log/analyzer.exit` 对本轮改变的items/compositor/CLI做Clang源码静态分析，**exit0/无diagnostic**（日志含准确命令），不运行任何测试脚本/套件或实际sample/pixel案例。git diff --check通过；无本轮CI run ID、kernel编译/full link、成功产物二次解包/hash/反汇编、设备/MMIO/分区操作。
- **下一具体动作**：在本core上补真实native VT coded-item callback和显式统一working-space raster转换/metadata交付，不能拿现有raw YUV tile接口假定已经是RGBA或把P01H/HYFBC改标签。再补fractional clap/更多合法property、Rext单色alpha与aux全链及method2输入；普通VT/CMPhoto/ImageIO入口仍要接。Native安全live ring消费/producer排他、durable dequeue/cache/DPB/per-buffer退休、c006真实free/ACK、持续session_ops provider/caller、cold NS/controller/firmware/clock最高层初始化注册、DMA容量与真实线性/压缩转换继续保留为完整目标缺口。复编仍用上一条完整frontend命令并取新唯一输出名；未合main，无后台任务，唯一最终boot继续等全部代码齐全。
- **实际源码交付**：本轮派生图/core与交接提交 **`6dc00531`** 已正常push成功 `802773d9..6dc00531` 到`origin/feat/hevc-hw`。完整最终frontend/analyzer对应此源码；此追加只改NOTES，不重编成功源码或复检产物。当前无后台构建、设备任务或子代理，保留活动功能树用于继续未完成的完整目标。

#### 2026-11-15 真实 VT picture → HEIF native raster/显式 ICC 接入（当前交接）

- **上一轮分类/当前基线**：上一轮 `802773d9/6dc00531` 实际改变输入/geometry/composition代码，属于progress，不是状态复述。本轮继续活动树 `.worktrees/hevc-hw` / `feat/hevc-hw`，开始时 **`6d5cbedf`** tracked干净。没有改main、共享`.local -> ../../.local`或长期输入，未启动子代理。完整HEVC/HEIF目标active；READY/profiles和普通VT gate不放开，无boot/探针。
- **复用真实VT picture路径**：新增 `vt_picture.c` / `X9HEVCCopyHardwarePicture`，原raw CLI与HEIF callback都实际调用它。先检查ready单sample、HEVC format/hvcC完整catalog/visible尺寸与独立IDR gate，再按真实注册capability走**指定DecoderID＋EnableHardware＋RequireHardware**；缺失symbol/注册失败不选择其它codec或软件fallback。UsingHardware在decode前/后均须真实CFBoolean true。flags0的SDK同步callback契约、wait/invalidate teardown、source ref身份、sticky callback错误/次数/FrameDropped及retain pixel已经实现；只返回一帧匹配depth/偶数geometry/plane shape/stride的线性NV12/P010，不把HYFBC/P01H当线性。旧CLI的错误覆盖式callback被移除，复用此路径。
- **已接实际compositor调用者**：新增 `heif_native.c/.h` / `X9HEIFCopyHardwareImage(file,id,working_rgb_icc,policy,pixel,info)`。先解析/plan整个graph，metadata总量16MiB（实现界限），逐coded item建CM sample/验证color，所有preflight结束才允许首帧硬解；发现任何reachable aux立即失败，不静默忽略alpha/gain/depth。真实callback建sample→上述native picture→read-lock decoder owner→独立planar CV view→**native VTPixelTransferSession**按显式目标ICC转RGBA IEEE-half LE→填入packed compositor目标。view只拥有attachments，转换不改VT callback原pixel；保留原plane owner到transfer完成，精确range与pointer/stride-span gate，释放view/target/transfer后才unlock/release decoder。core实际执行grid/iden与整数clap/irot/imir，不再是无native source的接口。
- **颜色/metadata策略与限制**：新增共享 `heif_metadata.c/.h`，CM与raster共用bounded span、ordered raw properties、dimg/aux IDs和ICC/nclx exact SDK mapping/gamma4/5；原CM完整catalog roundtrip不变。source policy每次显式选 `icc`（有ICC时用其RGB解释，matrix/range仍来自nclx）或 `nclx`；target必为合法RGB ICC，不造sRGB/BT709默认，native transfer后还核对实际ICC attachment。container derived colr可为缺失tile colr提供相同解释；父子或重复item的不同effective context明确ENOTSUP，不在item-only缓存下偷换色彩。未知RGB CICP在显式ICC覆盖时可保留raw不用，未知matrix不能猜；**真正SPS VUI解析/colr与bitstream consolidation仍未实现**，所以missing/unspecified container source字段不信CM可能生成的default、不做bitstream fallback。混合derived/leaf color语义、HDR/tone-map/色度siting实际效果与更多合法metadata仍未验收，不能称完整ColorSync/HEIF完成。
- **真实输出所有权**：API返回owned half-float CVPixelBuffer和必需info（Items的所有ordered properties/derived/aux IDs、effective colr、最终尺寸/PixelFormat、WorkingRGBICCProfile、source policy、DecoderID、实际native frame数）。原input/working ICC用`CFDataCreate`真正copy bytes，**不以可能retain immutable NoCopy CFData的CreateCopy冒充owned backing**；working profile可随pixel/info逃逸但不会引用CLI随后free的原ICC bytes。最终pixel malloc backing交由CV release callback一次释放；任意preflight/decode/transfer/composition/metadata失败都不返回partial输出。源ICC/YUV metadata不误贴到已转换working RGB上；没有软件熵解码fallback。
- **CLI实际入口/重放**：新增 `vdecctl decode-heif file.heic item-id-or-0 working-rgb.icc icc|nclx NEW.rgba16f NEW.info.plist`，实际调用上述API；输出tight RGBA half little-endian及mandatory binary plist（不是HEIC/PNG重编码）。两路径必须new，O_EXCL/0600、write-all、fsync/close/unlock成功后才打印成功；出错只移除本次创建文件，不覆盖旧文件。原 `decode-vt/decode-heif-item/heif-info/status` 保留。**此命令本轮没有运行**，物理provider仍缺席；只能证明当前编译源码已接真实调用，不是手机图片已成功。manifest native_raster_adapter/explicit_working_rgb_icc=true，但physical_provider/hardware_tested/automatic_injection/bitstream_color_resolver/auxiliary_merge/complete_image_compositor=false。
- **确切编译/静态结果**：初版完整frontend `x9/out/heif-native-raster-20261115/` exit0（当时尚无最终新CLI与owned ICC修正）；完整最终首版 `x9/out/heif-native-raster-final-20261115/` exit0。首轮源码分析 `x9/out/heif-native-raster-static-20261115/{analyze-source.py,analyzer.log,analyzer.exit}` exit0，picture/metadata/CM/CLI无诊断，但native有一条cross-TU noErr＋NULL decoded的nullability warning，**不把exit0称无告警**。实际加sample/info/pixel success非NULL defensive gate后，最终完整iOS dylib/client编译/链接/ad-hoc签名 **exit0 / -Wall -Wextra -Werror无诊断**，`x9/out/heif-native-raster-final2-20261115/{frontend/,frontend.log,frontend.exit}`；只重新分析改变的native/CLI，`x9/out/heif-native-raster-static-final-20261115/{analyze-source.py,analyzer.log,analyzer.exit}` **exit0/无diagnostic**。其余源码的首轮无诊断结论仍有效，不重复分析未变单元。git diff --check通过，`gh run list --branch feat/hevc-hw --limit3 --json databaseId,status,conclusion,headSha`返回`[]`；无CI run ID、kernel改变/编译/full link、测试脚本/套件、成功产物二次解包/hash/反汇编/重建对比或设备/MMIO/分区操作。
- **参考证据层级/下一动作**：本地UniXNU/hackios未找到HEIF/HEVC decode实现（只有photo启动脚本）；本轮只读SDK VT flags0 callback/Wait/Invalidate、planar CV view与pixel-transfer ICC文档。补取公开libheif master `libheif/image-items/{grid.cc,image_item.cc}` 到gitignored `x9/out/heif-native-raster-source-20261115/{libheif-grid.cc,libheif-image-item.cc}`，`curl --fail --location --silent --show-error --max-time25 https://raw.githubusercontent.com/strukturag/libheif/master/libheif/image-items/<name>`可重取；grid复制tile原component并clone首tile metadata、随后image_item:1558–1578处理container colr，是**公开参考源码，不是实际Apple/MTK runtime或固定build依赖**。不能据此猜完整mixed color语义、normalization或tone-map。继续补真实SPS VUI/CM与container colr consolidation（当前SPS parser确实停在DPB ordering）、fractional clap/更多合法properties、Rext单色alpha/aux与method2；普通VT/CMPhoto/ImageIO入口继续缺席。Native safe live ring消费/producer排他、durable dequeue/DPB/cache/per-buffer退休/c006、持续session_ops provider/caller、cold NS/controller/firmware/clock最高层及DMA容量/真实压缩转换仍未完成。复编用此前完整frontend命令并取新唯一输出名；目标不缩小，唯一最终boot仍等全部代码齐全，未合main。
- **实际源码交付/停止状态**：上述十文件源码与交接提交 **`430f7d2b`** 已正常push成功 `6d5cbedf..430f7d2b` 到`origin/feat/hevc-hw`。最终frontend final2及native/CLI final analyzer对应此代码；本条追加只改NOTES，不重复编译未变源码或复检产物。编译/analyzer/gh工具句柄均已terminal，无后台任务/设备操作/子代理；保留活动功能树继续完整目标，不能把新CLI存在或green compile当作HEIF实机验收。

#### 2026-11-15 SPS 尾部／sample 实际 PPS→SPS VUI 与颜色 consolidation（当前交接）

- **范围/源码交付**：继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，从已push且tracked干净的 **`0f712d1b`** 开始；实现九文件源码提交 **`f6d69bc5`**，交接提交`4528fa0c`，两者已正常push成功`0f712d1b..4528fa0c`到`origin/feat/hevc-hw`。这是实际parser和CM/native调用链改变，不是重新报告状态。完整目标仍active；不改main、固定`.local`、ioctl ABI或catalog entry大小，不启动子代理，READY/profiles/普通VT gate仍关闭，无boot/探针。
- **已补实际执行的SPS尾部**：`hevc_bitstream.c:sps()` 不再停在DPB ordering。支持范围内实际读完整coding/transform block语法、scaling lists（含size3预测delta与signed DC/coeff界限）、AMP/SAO/PCM、最多64组short RPS、最多32个long-term SPS refs、temporal MVP/smoothing、VUI/HRD/restrictions、extension header和严格`rbsp_stop_one_bit/align0/end`。拒绝截断SPS、非零尾随字节、未支持的Rext/multilayer/3D/SCC/unknown extensions；不是整个VPS/PPS或slice entropy validator。UE扩到31个leading zeros，覆盖`UINT32_MAX-1`，不做shift32；bounded UE出错不让超界值进入下一段算术。SPS TemporalId/nesting与MinCb整除、CTB/TB/PCM关系实际检查。
- **RPS/VUI所有权与选择**：SPS predicted RPS只依赖上一组，用两个15-ref小数组保留排序真实delta POC；`used_by_curr_pic/use_delta`选择计数与shift-to-zero后derived计数分开，不按flag数猜后续bits。没有为96个catalog entry增添VUI表/大kernel栈。新增小型`x9_hevc_vui`，presence与规范inference分开：CICP2未指定、range limited、chroma type0不变成BT709；保留原始CICP/SAR/overscan/chroma/field flags/display window/timing/HRD presence/restrictions及所选IDs。`x9_hevc_sample_vui`先通过整个one-picture AU gate，再按该picture实际PPS→SPS读取，检查hvcC长度和所选SPS IDs；不是代表SPS/CM-generated default。SAR0或未知idc保留，不造比例；default display window仅保留，不自动再裁一次conformance/clap。
- **已接CM与native，不留空helper**：内部`x9_heif_copy_item_sample`实际从原始hvcC/AU取VUI；公开`X9HEIFCopyItemSample`包装它，增加owned `BitstreamVUI/ResolvedColr`，继续原完整catalog roundtrip与IDR gate。共享`heif_metadata.c`一个CICP/gamma4/5 mapper：容器非2字段逐字段优先，只有缺失/CICP2回退到所选SPS，未知非2 code保留raw、不以NULL mapper偷回退；CM/native共用resolver。range来自容器nclx或SPS真实/规范inference。native preflight取得typed VUI后才确定source ICC/nclx解释，**不读可能合成默认值的CM extensions作为颜色证据**；source CV view明确覆盖top/bottom chroma location，实际YUV format/range仍必须匹配。任一field_seq或top/bottom不同location在首帧硬解前ENOTSUP，不假装场重建；所有reachable sources仍先preflight再decode。输出各item额外保存真实VUI与实际`ResolvedSourceColor`，ordered properties/EffectiveColr继续保留。manifest bitstream_color_resolver=true，但mixed_derived_color_contexts/auxiliary_merge/complete_image_compositor/physical_provider/hardware_tested/automatic_injection仍false。
- **确切编译/分析结果**：首版完整frontend `x9/out/hevc-selected-vui-20261115/{frontend/,frontend.log,frontend.exit}` **exit0，无诊断**。首kernel `x9/out/hevc-selected-vui-kernel-20261115/{compile-native.py,compile.log,compile.exit,result.json}` **exit0，但73 warnings**，含新uint8 narrowing；根据真实日志为bounded flag/code读取加显式转换，并补SPS NAL gate、保留Extended SAR零值。最终完整iOS dylib/client编译/链接/ad-hoc签名 **exit0 / -Wall -Wextra -Werror无诊断**：`x9/out/hevc-selected-vui-final-20261115/{frontend/,frontend.log,frontend.exit}`。只编受影响`sweet_x9_vdec.o`的最终kernel对象 **exit0，47 warnings（43 declaration-after-statement + 4 cast-align，无implicit narrowing/error）**：`x9/out/hevc-selected-vui-kernel-final-20261115/{compile-native.py,compile.log,compile.exit,result.json,kernel-objects/}`；不把kernel exit0说成零warning。四个改变TU的Clang源码analyzer `x9/out/hevc-selected-vui-static-20261115/{analyze-source.py,analyzer.log,analyzer.exit}` **exit0/无diagnostic**；最终parser改变后仅再分析该TU，`x9/out/hevc-selected-vui-static-final-20261115/` **exit0/无diagnostic**，其余三TU未再改变。
- **准确复编入口/边界**：完整frontend：`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/<新唯一名>/frontend`。kernel单对象继续用上述最终`compile-native.py`（import正式Builder/load_config，禁ccache、只选`sweet_x9_vdec.o`），改唯一out名再运行。`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit3 --json databaseId,status,conclusion,headSha`返回`[]`，无CI run ID。没有运行测试脚本/套件、sample parser案例/实机解码、full kernel link、成功产物二次解包/hash/反汇编/重建比较或设备/ADB/fastboot/mailbox/MMIO/分区操作。所有构建与analyzer工具已terminal，无后台任务。
- **参考层级/下一具体动作**：公开FFmpeg master `libavcodec/cbs_h265_syntax_template.c` 与 `libavcodec/hevc/ps.c`已在前一轮取到gitignored `x9/out/hevc-vui-reference-20261115/`；本轮实际对照HRD/VUI/RPS/scaling/SPS尾部，属于语法参考，不是当前Apple/MTK runtime或长期build依赖。此前「SPS停在DPB、bitstream resolver缺席」已被本提交取代。下一步继续补`heif_items.c`的`iloc construction_method=2` item-offset/`iref iloc`依赖读取（严格cycle/extent/depth界限、实际CM sample调用），然后补mixed derived/tile color语义与fractional clap working-space resampling，不把任意非线性ICC raster直接当线性像素插值。Rext单色alpha/aux全链、更多合法properties、普通VT/CMPhoto/ImageIO自动进程路由继续缺席；native FB/BS安全live消费/producer排他/durable dequeue、cache/DPB/per-buffer退休/c006真实free ACK、持续session_ops provider/caller、cold NS/controller/firmware/clock最高层初始化注册及DMA容量/真实HYFBC转换也未完成。HDR/tone-map/色度siting实际效果没有runtime验收。本轮无最终boot、未合main，完整目标不缩小。

#### 2026-11-15 HEIF method2 item-offset 实际读取链（当前交接）

- **基线/交付**：上一goal turn实际交付SPS/VUI代码，属于progress。本轮从 **`3ba918fb`** tracked干净的 `.worktrees/hevc-hw` / `feat/hevc-hw` 继续；七文件实现提交 **`2735bfb7`** 已正常push成功 `3ba918fb..2735bfb7` 到`origin/feat/hevc-hw`。未改main、固定`.local`或kernel源码，不启动子代理。完整HEVC/HEIF目标active，不以新输入路径或green compile替代全目标；READY/profiles/普通VT gate仍关闭，无boot/探针。
- **新增实际method2行为**：`heif_items.c`不再把所有method2直接ENOTSUP；保留每item的index_size以及`iref/iloc` group边界。`extent_index`为所选唯一group内1-based序号，index_size0才infer1，encoded0/越界拒绝，不按item ID/全局reference序号猜。引用顺序/重复to IDs保留，数据源不限hvc1 image type；这些是byte sources，不当作dimg/aux、继承源image properties或额外硬解源image。显式length的base_offset+extent_offset作用于**引用item完整逻辑字节序列**，能跨其多个method0/1/2 extents精确取range，而非只引用首extent。method2 length0在base/offset0时取完整source，非零base/offset＋隐式length仍明确ENOTSUP，未猜remaining-length/忽略offset；method0/1隐式length、protected/external/unknown method、歧义重复iloc groups仍不支持。
- **真实两阶段read，不是空resolver**：每次read独立heap DAG cache，先检查全部selected dependency items的所有extents、索引、bounds、logical-size uint64溢出、active cycle与32层路径；已cache subtree仍贡献完整height，合法重复DAG不会像全局seen链那样误报cycle/漏计深度。再递归intersect所选range，构造精确file spans、合并相邻连续span，所有验证和fragmentation预算结束后才copy。**不分配完整source data**：即使引用source逻辑size超过8MiB，只要所选输出≤8MiB、图/算术合法即可读取小slice。每item仍≤32 extents、全document≤256 items/1024 references；新增≤1024 reference groups（empty group也计）、32 data-depth、65536 final spans和2097152 intersection visits，均是明确实现界限，不是硬件容量。query与copy走同一plan/预算；range不clamp，失败不写partial output。继续拒绝destination覆盖borrowed file bytes，并新增覆盖opaque document tables的拒绝；cache/spans所有错误路径释放，document/input不被修改，可并发read。
- **实际CM/native/CLI接线**：原`X9HEIFCopyItemSample`的query→allocate→`x9_heif_read_item`→IDR/catalog/PPS→SPS gate现在实际使用新reader，raw tile/native HEIF callback也通过同一CM入口，不另造method2“成功”接口。`get_image`与grid payload read同样走新reader。新增ordered `ItemOffsetIDs`与`ConstructionMethod/LocationPresent`到owned CM/native info；raw properties/dimg/aux/VUI/color metadata继续保留。`vdecctl heif-info`显示location/method与1-based ordered iloc byte-source IDs，但不递归把byte source当image child，也不打印硬解成功。manifest新增`heif_item_offset_input=true`；physical_provider/hardware_tested/automatic_injection/auxiliary_merge/complete_image_compositor仍false。**本轮没有运行CLI、sample/data案例或硬解**。
- **参考与不确定性**：公开参考只在gitignored `x9/out/heif-item-offset-reference-20261115/`：libheif master `libheif/box.cc`、GPAC master `src/isomedia/meta.c`、Nokia HEIF master `srcs/{reader/heifreaderimpl.cpp,common/itemlocationbox.cpp}`及该repo GitHub tree JSON，分别保存为`libheif-box.cc/gpac-meta.c/nokia-reader.cpp/nokia-iloc.cpp/nokia-tree.json`。实际读到libheif/GPAC extractor **仍不支持method2**，不能拿它们证明现成decoder；GPAC writer建立ordered iloc refs，Nokia reader提供index-size0/infer1、recursive item-byte序列及length0完整source的参考。Nokia的共享pastReferences不pop及method2未使用base_offset等实现细节不能作为我们DAG/base策略的正确性依据；本实现独立写active/cache/range逻辑，不移植其whole-source分配/不完整检查。尚未取得规范/实际样本证据解决“非零offset＋length0”语义，因此保留明确ENOTSUP而非自称支持。公开源码/搜索HTML不是Apple/MTK runtime证据或固定build依赖。
- **确切编译/分析**：初版完整frontend `x9/out/heif-item-offset-20261115/{frontend/,frontend.log,frontend.exit}` **exit0/无诊断**；CLI显示与隐式length明确gate以随后最终编译为准，不用初版产物替代最终源码。最终全部iOS dylib/client编译/链接/ad-hoc签名 **exit0 / -Wall -Wextra -Werror无诊断**：`x9/out/heif-item-offset-final-20261115/{frontend/,frontend.log,frontend.exit}`。仅改变的items/metadata/CLI做Clang源码分析，`x9/out/heif-item-offset-static-20261115/{analyze-source.py,analyzer.log,analyzer.exit}` **exit0/无diagnostic**。`git diff --check`通过；push后`gh run list --branch feat/hevc-hw --limit3 --json databaseId,status,conclusion,headSha`返回`[]`，无CI run ID。复编仍用上一条完整frontend命令并选新唯一输出名；无kernel编译/full link、测试脚本/套件、成功产物二次解包/hash/反汇编/重建比较、设备/ADB/fastboot/mailbox/MMIO/分区操作。上述curl/build/analyzer/gh工具已terminal，无后台任务。
- **下一具体动作（推进物理链，不继续仅扩输入接口）**：本轮只读当前`x9/kernel/src/bsd/dev/x9_vdec_control.inc/.h`再次确认：`vcontrol_match_ring/vcontrol_recheck`仍只是token-matched head observation；journals的free_bs/free_fb/display_fb_observed不能转CPU ownership、dequeue或产像素，`control_service`无ring消费/per-buffer retirement，最高hardware owner与session_ops provider仍缺席。下一轮先对照已捕获MT6993 vendor/firmware ring producer及free/dequeue路径，补**独立可证producer排他＋durable head dequeue**，不能把稳定双快照、b001/b005或c->lock当FW writer停机证明；再接input/DPB/output cache/per-buffer退休、c006真实free/ACK和持续session_ops provider/service caller。cold NS/controller/firmware/clock最高层初始化注册、DMA容量/完整DPB与真实线性NV12/P010或HYFBC转换仍必做。HEIF remaining gaps（mixed derived/tile colors、fractional clap working-space resampling、更多合法properties/隐式length、Rext单色alpha/aux、普通VT/CMPhoto/ImageIO自动进程路由、HDR/tone-map/色度siting实效）继续保留，不能缩小目标。无最终boot、未合main，唯一最终boot继续等全部代码齐全。

#### 2026-11-15 native DMA 容量与按需页账本（当前交接）

- **上一轮分类/基线**：上一里程碑 `2735bfb7/6c17ae46` 的method2 reader为真实源码进展。本轮继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，从 `6c17ae46` tracked干净开始；本轮后端源码提交 **`9c02d2de`**，不是仅状态记录或扩大HEIF输入接口。完整goal保持active，不修改main、共享`.local`、长期输入；没有子代理，READY/profile/普通VT gate仍关，无最终boot或探针。
- **物理排他复核/有据转向**：复用既有RV55 producer/caller切片与固定vendor源码。`.../drivers/media/platform/mtk-vcodec/vdec/vdec_vcp_if.c:137–157` 的锁只保护AP destination列表；实际先copy head，再写src read，最后重算src count；`864–874`的output_async_handling清0也不证明FW排他。`.../drivers/misc/mediatek/vcp/rv/vcp_helper.h:61–74` semaphore枚举只有clock/PTP/I2C/touch/APDMA/sensor/awake，没有decoder ring项；`vcp_helper.c:946–985`的trigger_vcp_halt实际走 **EE coredump / force WFI ISR**，不能当作有据可恢复ring mutex。以上是当前stock源码/捕获静态证据，不是loaded decoder KO/runtime验证，也未排除所有可能FW私有协议。本轮没有新weak scan、FW patch或设备/MMIO操作；不把b001、双快照、ipi_blocked或AP锁升级为BS跨端RMW授权。物理ring仍是observation-only。
- **真实DMA代码**：`x9_mm_smmu.h/.c`将完整单arena映射/检查实现界限从16MiB改为 **512MiB**，scatter界限256→ **4096**，最大tree页16→ **260**（最坏L1+2L2+257L3）。新增并**实际调用** `x9_mm_smmu_stage1_geometry`，先检同SID完整aperture、4K对齐/无overflow和36-bit输入，按起点/end计算实际table count；native create先做该gate，再分配wired backing。builder也使用同一geometry，private/installed tree全部零洞/地址/权限/存储排除检查保留；mapping walk预算随真实bytes计算，不再卡4097次。HFRP/codec原SID与aperture、同一epoch/ASID/native parent、controller lease、ACK/TLBI/SYNC和cache要求未放宽。
- **按需存储/复杂度**：native `x9_vdec_dma.inc`将raw PAs、sorted native PA→CPU-page index、spans、table descriptors、pool PAs及bitmap移到**按actual pages/geometry的可变typed heap存储**；table backing只分配实际4K树所需的16K CPU-rounded容量，小arena不预留最大tree，大arena不膨胀固定kalloc owner或kernel栈。长度先记再分配，任何私有partial failure逐项free，已发布/uncertain backing仍只允许whole-domain retirement释放。私有index用非递归heapsort，不改ordered scatter，线性strict-index/真实pmap backing核对替换raw-page二次方去重；PA resolver改binary predecessor lookup，不将FW token/PA cast为pointer。generic tree验证按递增IOVA用单scatter cursor，不每leaf重扫旧spans；controller `owner_private`直接访问CD/tree scalar，移除随260页扩大到4KiB以上的临时range栈数组。
- **准确能力边界**：512MiB/4096 spans/260 tables/256 live slices都是**实现界限，不是12GiB设备容量或固件DPB能力声明**。物理fragmentation超过4096 spans会明确ENOSPC；没有声称支持任意全碎片512MiB。源码现在可由原native create→slice alloc→publish→controller install路径请求大arena，但本轮**没有实际分配/安装大arena或解码、没有指定DPB数/真实HYFBC/linear layout，也没有改变默认启动/provider注册**。完整DPB布局/数量/codec scratch预算仍需真实最高owner与SPS/firmware协商；单纯扩大arena不能代替这些工作。
- **确切编译/源码分析**：仅编受影响的 `x9_mm_smmu.o/x9_mm_smmu_owner.o/sweet_x9_vdec.o`，正式Builder+固定`local.json`（launcher空），**exit0 / 三个True**，日志分别 **4/20/54 warnings**；前两项均declaration-after-statement，sweet为50 declaration-after-statement+4 cast-align，无error/narrowing。准确脚本/输入命令/日志/result保留 `x9/out/hevc-dma-capacity-20261115/{compile-native.py,compile.log,compile.exit,build.log,result.json,kernel-objects/}`。随后从这三条真实compiler命令只移除object/dependency输出并切 `--analyze`，同kernel headers/macros/source做Clang源码分析，**exit0 / 无analyzer diagnostic**；仅压制上述两类style warnings，不压制analyzer检查，`x9/out/hevc-dma-capacity-static-20261115/{analyze-source.py,analyzer.log,analyzer.exit}`。没有测试脚本/套件、frontend重建、full kernel link、成功产物二次解包/hash/反汇编/重建对比或设备/分区操作；compile/analyzer会话均已terminal。
- **下一具体动作**：回到真实consumer/owner协议，补**独立可证producer排他＋先durable journal后head/count不可逆store**，未知/重复/epoch失配token与uncertain写失败必须保留/quarantine。BS count跨端RMW仍不能普通decrement；FB出队也不能仅以AP锁与count hint冒充FW排他。然后input/DPB/output cache与逐buffer退休、c006真实free/ACK、持续session_ops provider/service caller；cold NS/controller/firmware/clock最高层初始化/注册与真实layout/DPB预算仍缺。HEIF mixed-derived color、fractional clap working-space resampling、更多properties/implicit lengths、Rext单色alpha/aux、普通VT/CMPhoto/ImageIO自动入口及HDR/chroma-siting真实效果仍保留。完整目标未完成，继续保留功能树，未合main；唯一最终boot仍等全部代码齐全。

- **实际交付收尾**：后端源码 **`9c02d2de`** 与交接 **`a575f777`** 已正常push成功 `6c17ae46..a575f777` 到`origin/feat/hevc-hw`；`gh run list --branch feat/hevc-hw --limit3 --json databaseId,status,conclusion,headSha` 返回 **`[]`**，无CI run ID。这一补记只改NOTES，不重编成功源码/复检产物。当前无后台构建、设备任务或代理；保留活动树继续完整目标，不把本次容量交付当作ring消费/DPB/硬解验收。

#### 2026-11-16 control-owned 持续 native service pump（最新交接）

- **基线/实际交付**：接 `a2feb0a0` 与上一turn保留的四文件草稿，仍在 `.worktrees/hevc-hw` / `feat/hevc-hw`；本轮审核、修正、编译并提交实际后端代码 **`73b0f5d0`**，已正常push `a2feb0a0..73b0f5d0` 到`origin/feat/hevc-hw`（最终目录`push-code.log/.exit`，exit0）。不改main或固定`.local`、不启动子代理。完整HEVC/HEIF goal保持active：新增持续service调用者不是session_ops provider、ring消费或完整硬解完成，READY/profile/普通VT gate仍关；无boot。
- **确切接线**：`x9_vdec_control_create`真实分配`THREAD_CALL_PRIORITY_KERNEL + THREAD_CALL_OPTIONS_ONCE`的XNU thread call；只有物理a000/b000、完整slot绑定和native NC `ipi_blocked=0`全部成功的INIT才自动enabled/arm。worker与manual `control_service`共用`vcontrol_service_locked`及同一control锁、现有bus whole-call reservation，调用真实`x9_vcp_bus_service`处理reverse/async并重查pending observations。每批仍最多64 packets/3000ms原deadline；idle10ms，满批/pending或前reservation争用1ms再调，不把这些调度值当硬件deadline/producer fence。仅`EBUSY + log.epoch=0 + 无control fault`重试；reserved/handler/protocol/timeout失败保留并停调度，STOPPING/STOPPED/QUARANTINED停止，不永久重试EBUSY。计数溢出fail closed。status新增wakeups/contentions/enabled/joined/errno/bus phase/closing，以及最后实际reserved或非争用失败batch的完整`service_call`/last_rx；争用不覆盖已有packet证据。
- **真实host寿命**：bus新增最多32个boot-unique retained-client token，`retain_shared`使用原RUNNING/exact published extent/epoch/guard检查，不放宽authority；`release_shared`只移除精确当前token，拒绝旧/重复。control在发布前取得hold并保留至join+discard；create失败释放从未arm的worker及hold。有任一hold时`bus_discard`即使STOPPED/交易间隙也EBUSY，避免worker在API间隙访问已释放bus。release不是停FW、DMA退休或power释放。header明确外部opaque-handle调用者仍须串行化discard寿命，private mutex/worker join不代替外部引用。
- **关闭/释放顺序**：新增不可逆`x9_vdec_control_quiesce`：持control锁关闭admission/arm，**放锁后**`thread_call_cancel_wait`，再记joined；允许对retained对象重复调用，不允许从pump/hardware/bus callback或持callback需要的owner/shared/bus锁调用。应先quiesce，再bus stop/whole-domain retirement；discard也先join，之后仍要求INIT尝试后的物理STOPPED及两个真实RETIRED domains、成功slice cleanup，最后release bus hold/shared-memory refs与worker。失败discard保持closed和依赖retained。这里只join CPU回调，不声称FW/IRQ/AXI/DMA drain、ring dequeue、per-buffer retirement或c006 free。
- **XNU源码依据/边界**：实际固定`.local/dxb/xnu/osfmk/kern/thread_call.h:78,116–125`说明ONCE self-resubmit延至callback返回，普通delayed self-rearm可能并发；`thread_call.c:1343–1348`清RESCHEDULE，`1413–1460`的ONCE cancel_wait同时取消pending并wait in-flight，`1098–1135`的free拒绝pending/RESCHEDULE。实现依据当前XNU代码，不只header旧文字；未改XNU源码或重建其对象树。不同control的业务身份/async demux仍需最高session owner统筹，本轮没有实现bus-wide多会话分派；不能把bus whole-call排他当FW ring writer互斥。
- **确切编译与修正**：首编`x9/out/hevc-service-pump-20261116/{compile-native.py,compile.log,compile.exit,result.json,build.log,kernel-objects/}` **exit1**：`x9_vcp_bus.o`成功/2 declaration-after-statement warnings，sweet有54 warnings及1 error（`kMillisecondScale`未声明，属IOKit常量，不是kernel clock声明）。改成`clock_interval_to_deadline(ms, UINT32_C(1000000), &at)`，不引入IOKit依赖；之后仅复编受影响`sweet_x9_vdec.o`，`x9/out/hevc-service-pump-final-20261116/` **exit0 / True / 54 warnings（50 declaration-after-statement + 4 cast-align，无error/narrowing）**，保留同名helper/log/exit/result/build.log/objects，未重复未变且已成功的bus对象。最终只补opaque lifetime说明注释，不改可执行源码，不重编成功产物。
- **源码分析/复现入口**：从首编记录中两条真实compiler命令取cwd/flags，移除`-c/-MD/-MF/-o`输出，改`--analyze`处理当前bus/sweet源码；仅压制上述两类style warnings，不压analyzer。`x9/out/hevc-service-pump-static-20261116/{analyze-source.py,analyzer.log,analyzer.exit}` **exit0 / 无diagnostic**，不把SAST当并发/硬件验证。复编使用正式Builder+固定`x9/build/local.json`、launcher空；复制最终`compile-native.py`改唯一out名再用`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I <helper>`。`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0 / `[]`**，记录在最终目录`gh-runs.json/.exit`，无CI run ID。没有测试脚本/套件、frontend/full kernel link、成功产物二次解包/hash/反汇编/重建对比、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作；所有compile/analyzer/gh工具已terminal，无后台任务。
- **下一具体动作/仍缺**：本轮将“service无持续调用者”替换为实际INIT接线的native pump，**不能**将其扩大为完整session backend。继续从已捕获MT6993 producer/consumer协议补独立可证producer排他、durable journal→head/count dequeue和unknown/重复/epoch失配token quarantine；BS跨端count RMW与FB出队仍observation-only，AP锁/双snapshot/b001/b005/slot blocked/EE force WFI均不授予排他。再接真实input/DPB/output cache与逐buffer退休、c006 free/ACK、持续session_ops provider及多会话身份分派。cold NS/controller/firmware/clock最高层初始化/注册、完整DPB/layout/scratch预算、真实linear NV12/P010或HYFBC转换仍缺；HEIF mixed-derived colors、fractional clap working-space resampling、更多合法properties/implicit lengths、Rext单色alpha/aux、普通VT/CMPhoto/ImageIO自动入口与HDR/tone-map/chroma-siting实效继续保留。唯一最终boot等全部代码齐全；功能树保留、不合main。

#### 2026-11-16 fractional clap 线性工作空间实际输出链（最新交接）

- **上一轮分类/基线/交付**：上一turn的native service pump源码和交接 `73b0f5d0/de4aeca4` 为实际progress。本轮从tracked干净的 `de4aeca4` 继续同一 `.worktrees/hevc-hw` / `feat/hevc-hw`；六文件代码 **`9fd8a80f`** 已正常push `de4aeca4..9fd8a80f` 到`origin/feat/hevc-hw`，原stdout/exit在最终目录`push-code.log/.exit`（exit0）。未改main、固定`.local`或kernel，未启动子代理，完整目标active；新pixel-compositor代码不是物理ring/DPB完成，READY/profile/普通VT gate不变，无boot。
- **物理证据/有据转向**：定向复用已有 `x9/out/hevc-ring-producer-20261115/{bs-publisher-r2.asm,fb-publisher-mapped-r2.asm}` 与 `hevc-fb-consumer-20261115/{publisher-callers.asm,async-handling.asm}`，以及固定vendor `vdec_vcp_if.c:137–157,864–874`。BS `0x30b2f0..0x30b306` 仍为shared count load/add1/store、write_idx先写；FB `0x30a128..0x30a138`先write_idx后load read_idx/派生count。这些旧captured静态切片/stock AP destination锁没有新增FW排他证据；不重复弱扫描、不造dequeue stores、不把NC/双snapshot/ACK/pump当writer fence。本轮转向另一必需的**实际HEIF输出渲染**缺口，不以扩输入parser/成功空callback替代ring工作。
- **真实 rational aperture 代码**：新增显式 `X9_HEIF_CLAP_LINEAR_CEIL` policy 与 `plan_create_with_policy`，旧 `plan_create` 仍EXACT_SAMPLES、拒绝fractional尺寸/原点。完整uint32分母/有符号offset先在`__int128`做精确bounds与整除判定（origin公共分母可65位、分子可79位，不能假称int64足够），越界/零尺寸/零分母拒绝；通过后才转换采样坐标为double。新policy对**每个ordered clap**取ceil(width/height)栅格，pixel centers覆盖精确aperture，后续grid/iden/mirror/rotation按这个实际栅格执行，原raw属性保持。ceil是调用者选择的渲染策略，**不是HEIF规范强制取整或规范尺寸推断**。plan汇总全部reachable节点是否需插值，不只root；任一aux仍在首source callback前拒绝，不被丢弃。
- **实际 linear-light compositor，不是ICC像素直接插值**：新增 `x9_heif_compose_linear_float`，只接受调用者明确提供的统一linear-light、premultiplied RGBA IEEE binary32 little-endian；普通`compose_packed`即使FLOAT format也拒绝需插值的plan。coded raster先检查finite RGB/alpha[0,1]，允许负/extended RGB，不clip、不tone-map；bilinear用double中间值、pixel-center逆映射，edge extension仅在已精确验证的源support内，所有索引始终在源界限内。NaN/Inf/float溢出拒绝，不返回partial raster。DAG cache/ordered placement/递归界限/失败释放沿原真实renderer执行，整数crop与纯几何仍精确copy，不无故重采样。
- **native/CLI 已真实调用**：新export `X9HEIFCopyHardwareImageWithOptions(... source_policy, clap_policy, raster_budget, ...)` 经同一CM sample/选中SPS preflight与 `X9HEVCCopyHardwarePicture` 的真实DecoderID/UsingHardware gate取coded pixels。仅plan实际需插值时，CoreGraphics `CGColorSpaceCreateExtendedLinearized` 从caller working ICC创建matrix-based extended-linear RGB，核对RGB/3 components/extended/output支持，再取真实ICC；LUT/non-matrix或无ICC明确ENOTSUP且发生在首硬解前，**不偷回退sRGB**。每个coded item通过真实VTPixelTransfer目标ICC变成RGBA-float→上述linear compositor→**一次**真实VT transfer回caller原working ICC/RGBA-half。每次transfer检查其返回ICC attachment，并read-lock输出检查finite/opaque alpha1（当前无alpha源），不手填alpha；这是执行/格式gate，**不是pixel-transfer或HDR实际效果证明**。新旧输出均有owned pixels/ICC/info，partial CV adoption/transfer/metadata失败逐项释放。原`X9HEIFCopyHardwareImage`显式传EXACT/256MiB，兼容路径保留。
- **预算是实际caller输入，不是硬件容量**：发现float root+half export共存需24 bytes/pixel，旧256MiB会直接卡4K fractional输出，故新增native与core的显式非零heap budget，core<=SIZE_MAX，native再限INT64_MAX用于owned metadata表示；新增`compose_packed_with_budget`让新options的exact路径也真正使用同一预算，legacy packed仍256MiB。ALL cached/intermediate raster按实际live bytes计账；root float+half已知峰值在首硬解前检查，export也复核，真实calloc失败仍传播。VT内部/driver DMA/DPB存储不属于这个host heap账本，没有称已预留内存/支持任意12GiB图片。CLI `decode-heif`支持 `[exact|linear-ceil [raster-MiB]]`，exact默认256MiB、linear-ceil默认1024MiB，可显式调预算；返回half buffer的尺寸/stride/budget/地址边界也检查。info保存`CleanAperturePolicy/RasterBudgetBytes/LinearLightResampling/LinearRGBICCProfile/CleanApertureSampling`及每item原有ordered properties/VUI/colr。manifest标记linear resampler/explicit heap budget，但complete compositor/physical provider/hardware tested/mixed-derived contexts/auxiliary merge/automatic injection仍false。
- **确切编译/分析**：初版完整iOS dylib/client编译、链接、ad-hoc签名 `x9/out/heif-clap-linear-20261116/{frontend/,frontend.log,frontend.exit}` **exit0/无诊断**。补CoreGraphics/已知export预算preflight后，`heif-clap-linear-final-20261116/` **exit0/无诊断**；三个改变TU的源码analyzer `heif-clap-linear-static-20261116/{analyze-source.py,analyzer.log,analyzer.exit}` **exit0/无diagnostic**。根据上述实际4K容量审查再加入caller budget；**最终全部源码**以 `x9/out/heif-clap-linear-budget-final-20261116/{frontend/,frontend.log,frontend.exit}` **exit0 / -Wall -Wextra -Werror无诊断**为准，不拿pre-budget产物替代当前API。最终 `heif-clap-linear-budget-static-20261116/` 对compositor/native/CLI三个TU按同iOS target/SDK/macros做Clang源码分析 **exit0/无diagnostic**，未压制analyzer。`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0 / `[]`**，最终目录`gh-runs.json/.exit`，无CI run ID。没有kernel编译/full kernel link、测试脚本/套件、sample/CLI/实际像素或色彩运行、成功产物二次解包/hash/反汇编/重建比较、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作；上述build/analyzer/push/gh工具已terminal。
- **准确入口/继续工作**：重编用 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/<新唯一名>/frontend`。新CLI调用形式（**本轮未执行**）：`vdecctl heif-info file.heic linear-ceil`；`vdecctl decode-heif file.heic 0 working-rgb.icc icc linear-ceil 1024 NEW.rgba16f NEW.info.plist`，native物理provider仍缺，不能说命令当前会成功产像素。fractional/clap的matrix working ICC显式ceil路径已接实际renderer，**LUT/non-matrix working ICC的fractional渲染仍需明确的另选linear intermediate与真实颜色转换，不缩小完整目标**；普通入口如何选择rasterization、mixed-derived/tile color上下文、Rext单色alpha/aux、更多合法properties/implicit lengths和HDR/tone-map/chroma-siting实效继续缺席。继续补可证producer排他/durable ring dequeue、input/DPB/output cache/per-buffer退休/c006真实free ACK、持续session_ops与bus-wide多实例分派；cold NS/controller/firmware/clock最高层初始化/注册、完整DPB/layout/scratch协商与真实linear NV12/P010或HYFBC转换必做。普通VT/CMPhoto/ImageIO自动路由仍关；唯一最终boot仍等**全部代码齐全**，不合main、保留功能树。

#### 2026-11-16 bus-wide 多实例 reverse/async 分派（最新交接）

- **本轮分类/基线/交付**：上一turn的显式linear clap renderer/budget为progress；本轮从tracked干净的`bf966173`继续同一`.worktrees/hevc-hw` / `feat/hevc-hw`，四文件native代码 **`0ab50d37`** 已push `bf966173..0ab50d37` 到`origin/feat/hevc-hw`（最终编译目录`push-code.log/.exit`，exit0）。未改main、固定`.local`或XNU补丁，未启动子代理。完整goal保持active；本轮实际补bus mux，不是ring消费、session_ops provider或物理硬解完成，READY/profile/普通VT gate仍关，没有boot。
- **真实注册表/身份**：每个control create取得原host lifetime hold、增加shared/memory refs、完整初始化cookie/lock/ops后，调用新的`x9_vcp_bus_register_instance`冻结`init + 全部defined ops fields`；失败完整回滚refs/mutex/worker/hold。call_begin必须命中同一注册身份与handler，不再接受未注册/替换的ops。host hold和FW AP instance token分开保存；token永不cast为pointer。每bus最多32个native注册记录（**host ledger策略，不是已证明的FW容量**），移除后保留identity/packet tombstone，不复用注册slot/wire token。首次INIT/QUERY真正request attempt前先记exposed；未exposed的foreign route失败关闭。没有用注册代替FW INIT/bootstrap/READY。
- **所有实际RX入口接同一分派**：INIT/QUERY/START的等待循环、RX baseline、a005等待和持续service统一走`dispatch_packet`。普通reverse及e000用snapshot offset16的完整64-bit AP instance token选择注册对象，再由原typed parser检查service/长度/字段及captured context例外；b005是outer service2，不能强制service1或只按context分派。foreign c001–c005/c007/c008/c009与b005/e000进入正确control的native/hardware handlers。只有当前caller的合法b005可写a005的out并结束等待；foreign b005只dispatch，不作ACK/本buffer acceptance。预期control ACK只接受当前实例，foreign/stale ACK仍EPROTO/保留packet；service已建立时新INIT也先走真实empty RX baseline。c005/type8仍走当前caller的bus-global SHM sentinel parser/一次实际reply，不把UINT64_MAX当普通实例；duplicate type8失败，c006仍ENOTSUP。未改任何wire typed parser或制造free/ACK。
- **锁与生命周期**：bus whole-call reservation固定registry/cookie至reply/最终journal写入，注册/移除均拒绝任何活动call；registered hold不能release，discard还检查retained_routes。foreign `route_enter`在bus mutex外以deadline-aware try-lock（10us pause、最多300000次且受原absolute deadline约束）获取recipient control，当前caller不重复锁；成功enter才调用一次leave，leave记录最终handler/transport错误并放锁。public hardware、bus-owner和lower DMA callbacks明确不得reenter任意control或持lower锁等待control/pump；源码现有call入口在争用时先返回、不并行跑hardware callbacks，**不是实际provider锁图或实机并发验证**。quiesce只关闭自身admission/join自身CPU pump，closing后foreign live-FW handlers仍可执行，不能移除route；owner须stop/drain后退役domains。discard在原STOPPED/两域RETIRED检查后、任何slice cleanup/ref释放之前unregister；活动foreign call使其EBUSY保留对象，不free cookie。后续cleanup失败可按retained closed对象续清，route tombstone保留。原每call<=3000ms/WAITISR<=500ms不延长。
- **完整packet/reply证据与争用窄回滚**：每route保存最新owned72B RX/reply、epoch/recipient token/sequence、foreign/enter attempted/entered/notification、reply encoded/attempted/physically consumed、最终errno及累计计数；失败enter/旧ACK也留在registry，unknown身份留caller last_rx。新增bus instance status及`x9_vdec_control_get_route`只读入口；foreign leave同时保存control的foreign journal/错误。reply consumed只表示物理transport，不授予DMA/像素/退休。修正check_shared→call_begin之间的合法跨实例争用：**只在EBUSY且log.epoch=0**时INIT恢复fresh、output/START恢复纯host attempt/前一状态且不留假buffer record；无prepare/alloc/NC store/send，burned unique output token不复用。reserved错误不回滚、不重试；QUERY现在连reserved RX baseline错误也保留control fault。
- **确切编译/分析**：首版两对象`x9/out/hevc-instance-router-20261116/`exit0，bus3 warnings（含新unused parameter）、sweet54 warnings；补reservation前回滚/最终journal invariant、去unused后，**最终四文件源码**按正式Builder、固定`x9/build/local.json`且launcher空，只编`x9_vcp_bus.o`与`sweet_x9_vdec.o`，`x9/out/hevc-instance-router-final-20261116/{compile-native.py,stdout.log,build.log,compile.exit,result.json,kernel-objects/}` **exit0 / 两项True / 2与54 warnings**（原kernel style/cast-align，非零warning不能说无诊断）。首次analyzer helper误指不存在的compile.log，`hevc-instance-router-static-20261116/`exit1于读日志、未启动clang；保留失败原log，改指最终真实stdout两条compiler命令后，`x9/out/hevc-instance-router-static-final-20261116/{analyze-source.py,analyze.log,analyze.exit}` **两TU --analyze exit0/无diagnostic**，不压制analyzer。没有测试脚本/套件、frontend重编、full kernel link、成功产物二次解包/hash/反汇编/重建比较或设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作。
- **复编/CI与下一步**：复制最终`compile-native.py`到新的`x9/out/<唯一名>/`，修改其中out名，再用`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I <helper>`；仅两改动对象，不重编未改且成功的wire/frontend。源码analyzer从本次stdout取实际flags/cwd，移除输出flags后--analyze；不能拿早期pre-rollback源码编译当最终证据。`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha`返回`[]`，无CI run ID。下一实际工作仍是可证producer exclusion/durable BS和FB分别dequeue→per-buffer cache/DMA/DPB退休/c006真实free ACK→持续session_ops provider；本轮没有新增物理ring证据，不重跑已排除的双snapshot/NC/b001/b005/slot-blocked/WFI排他猜测。cold NS/controller/firmware/clock最高owner、完整DPB/layout/scratch协商、真实linear NV12/P010或HYFBC转换仍缺；HEIF mixed-derived colors、fractional LUT ICC另选linear intermediate、Rext单色alpha/aux、更多合法properties/implicit lengths、普通VT/CMPhoto/ImageIO自动路由及HDR/tone-map/chroma-siting实效继续未完成。唯一最终boot仍等**全部代码齐全**，保留功能树、不合main；编译/analyzer/push任务均已结束，无后台设备/编译任务。

#### 2026-11-16 c006 间接 free 调用链与 RV32 独立分帧（最新交接）

- **范围/交付分类**：继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，基线为已push的 `d7a1eda3`；本轮两文件工具/证据代码提交 **`3a260037`**。这是离线RE工具与新静态调用边收束，**没有新增native free/ACK或物理硬解功能**。未改main、XNU补丁、kernel/frontend或固定 `.local` 输入，未启动子代理；完整goal保持active，不合main、不生成boot，READY/profile/普通VT gate继续关闭。
- **恢复并收紧真实分帧工具**：新增 `x9/tools/rv32_slice.py`，先按RISC-V encoded-length bits决定2/4/6/8/长指令边界，再逐exact PC调用read-only `r2 -n -a riscv -b32 -m <base> ... pdj 1`，不沿r2的invalid fallback size前进。原始bytes来自显式普通文件；拒绝FIFO/device、超界/奇数地址/partial final instruction、保留的>=192-bit length，最多32768B/4096条、subprocess argv无shell且timeout60s；检查输入inode/size/mtime/ctime变化及同长度decoder bytes。新JSON/同stem `.asm` 均exclusive-create，旧证据不覆盖。非标准JALR funct3、RV32C高shift编码、long/invalid/长度不符标unresolved；`engine_accepted`仅表示匹配长度的engine mnemonic，**不证明vendor ISA语义、入口或完整CFG**。不是运行测试或固件。
- **决定性错误解码与注册链**：`0x246a44`的原始 `13565520` 是4B unresolved编码，r2旧输出的 `0x246a46 jal 0x246aea` 落在指令内部，不能作调用边。真正注册是 `0x30584a/0x305858` materialize `0x308c1a`，`0x305866`存56(sp)，block始于24(sp)，即free callback相对+32；`0x305898..0x3058a2`传service1/block到 `0x242f00`。installer选择 `0x32c0f0 + service*96` 并复制96B，因此service1 free cell为 **`0x32c170`**。通用free wrapper `0x24695a`从88B object的+64取service，最终 `0x246ae4..0x246af4`加载+80/+84的64-bit AP token、a2=sp24的32B mem object、从table+32读取a3并 **`jalr a3`**。这条静态registered-target边依赖真实table内容，未证明runtime注册/覆盖排他或所有wrapper custom分支。
- **cleanup/deinit新增窄边**：`0x306db6..0x306dce`检查instance+`0x198c`，将instance+`0x1990`/size88传 `0x24695a`；返回后不检查free结果就改软件字段并jump `0x3061aa`（该join构造b002）。上层a002在 `0x305d6c`确有标准branch到 `0x306106`，但 `0x306170` 等vendor分支仍未知，**a002→cleanup完整路径未证实**。cleanup前 `0x306cfc→0x2707d2` / `0x306d2a→0x270874` 已独立分帧；前者在 `0x270802`以object+80 context调用object+44 callback、随后清+84软件flag，后者从object+640复制88B后 `0x2708a0→0x24695a`。原logging bytes支持名称 `eVDecDrvDeInit` / `eVDecDrvRelease`，**名称/清flag/返回值不证明codec实际deinit target或IRQ/AXI/DMA drain**。另一个 `0x311dd6`标准free caller仅确认窄边，不扩成硬件退休。旧26个raw caller候选未全验证，不作完整callgraph。
- **静态profile与私有证据**：`vcp_artifacts.py:rv55_mem_wait_evidence`在原完整DRAM INIT身份门内增加上述raw spans/metadata，共 **28 spans**；新增runtime_free/DMA_retirement/cache_handoff flags始终false，原allocation/IRQ/mapping flags亦不开放。只读输入仍是 `x9/out/hevc-ring-producer-20261115/rv55-dram.bin`（base0x200000，2057906B，SHA256门 `200294bca18891fe4cc8b8cc93489b586305f1acb80aeea176566f1726392c3f`）；原sender/request资料在 `x9/out/hevc-c006-retirement-20261116/`，新exact-PC资料在 **`x9/out/hevc-c006-chain-framed-20261116/`**（registration、table-install、instance-dispatch、native-free-call、free-wrapper、codec-callback/codec-object-free JSON/asm）。instance-dispatch实际1891条/107 unresolved，wrapper143条/13 unresolved；不把大window当完整CFG。该目录一次较大second-caller slice因partial final instruction拒绝、无输出，后续仅有效narrow10条。原件/派生/失败log均保留，未提交任何固件或设备文件。
- **确切执行/复查**：最终 **`x9/out/hevc-c006-evidence-final-20261116/`** 的 `syntax.log/.exit`：两个Python源码syntax compile exit0（不是测试套件）；`framing.log/.exit`及`free-wrapper.json/.asm`：最终工具实读wrapper **exit0/143条/13 unresolved**。初版 `extract-profile.py`误取parents[3]、在import不存在的`tools/vcp_artifacts.py`时exit1，未生成profile；失败`profile.log/.exit`保留。修正helper **`extract-profile-corrected.py` / `profile-corrected.log/.exit`**实读上述固件，**exit0/profile_match=true/28 spans/runtime_free=false/runtime_dma_retirement=false**，产物`mem-wait-profile.json`。复查用 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/tools/rv32_slice.py x9/out/hevc-ring-producer-20261115/rv55-dram.bin --base 0x200000 --start 0x24695a --bytes 0x19c --r2 /opt/homebrew/bin/r2 --out x9/out/<新唯一名>/free-wrapper.json`；profile helper复制到新同级out目录再执行，避免覆盖旧结果。`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，记录`gh-runs.json/.exit`，无CI run ID。没有kernel/frontend重编、full link、测试脚本/套件、成功产物复检、设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作；无后台任务。
- **下一具体动作/完整目标不缩小**：从已确认 `0x2707fe` 的codec object+44 callback开始追真实初始化来源与selected HEVC codec deinit target，补全相关vendor分支及free发生时的FW/HW访问退出条件；不重复LLVM `+c,+xandesperf`已不匹配的弱feature尝试，不用c006请求、软件deinit/flag、WAITISR或transport返回授予DEVICE→CPU/free/d006。BS/FB独立producer排他/durable dequeue、input/DPB/output cache与per-buffer退休、持续session_ops provider、cold NS/controller/firmware/clock最高owner、DPB/layout/scratch协商和真实linear NV12/P010或HYFBC转换仍缺。HEIF mixed-derived colors、fractional LUT ICC显式linear intermediate、Rext单色alpha/aux、更多合法properties/implicit lengths、普通VT/CMPhoto/ImageIO自动路由及HDR/tone-map/chroma-siting实效也未完成；唯一最终boot继续等**全部代码齐全**。

#### 2026-11-16 H265 codec ops 初始化→destructor→多对象 free（最新交接）

- **分类/基线/提交**：上一goal turn为progress（排除指令内部假调用并确认c006注册间接边），本轮从实际tracked干净的 `1b28c9f6` 继续同一功能树，证据代码 **`021c936c`**。新增的是已分帧的codec ops来源、H265-labelled destructor及真实多对象free调用边；**没有新增native free/ACK、DMA退休或硬解功能**，不能将本轮交付缩成完整目标。未改main/kernel/frontend/XNU补丁/固定 `.local`，未启动子代理；完整goal继续active，未合main、无boot，READY/profile/普通VT gate仍关。
- **+44 callback来源已定位，不再仅停在间接调用**：原先限定API区间的standard SW/C.SW +44候选为空，原因不是callback缺席，而是批量写ops。真实native create **`0x307faa→0x26ff2c`**，随后 `0x27000a/0x270014/0x270022`将codec object+16作为a1传 **`0x2233ec`**。selector的 `0x223468..0x223476` materialize **`0x3356b0`**，`0x223490..0x22349a`将64B ops块传TCM copy helper **`0x701c`**（不是DRAM `0x20501c`）；因deinit读取object+44，对应table+28。该完整固定DRAM中的table实际+28值为 **`0x2cd5e6`**。table是data，不当指令；selected candidate→table仍有vendor分支，**未证明runtime选择/安装，也不把copy意图当实际执行**。
- **条件下的真实H265 destructor与free循环**：`0x2cd5e6` wrapper标准路径检查context非NULL/word0 magic `0xabba4321`，以sp12中context地址在 **`0x2cd5fe→0x2d2648`** 调用；之后直接li a0=1，不检查destructor结果。原log literal `0x2d6532`为 `Vdec_Drv_H265_destroy`，只用来解释这条已实际定位的static edge，不当运行或硬件状态。destructor从context+368取得子对象，在 **`0x2d2692→0x2f7d46`** 调用initial cleanup；该helper首loop为7个88B object（base+`0x1b38`、presence+56/60、field+72非零跳过），**`0x2f7dac→0x24695a`** 然后清相应软件presence。destructor随后确有32项SPS/256项PPS cleanup标准loop，以及3个88B object（base+`0x1840`）的 **`0x2d2772→0x24695a`**、清软件字段；尾部 `0x2d27fc→0x2ccb20` / `0x2d2800`清handle。**7/3是这两个FW软件loop的界限，不是DPB/硬件容量；object具体用途/访问者、整个destructor CFG及DMA/IRQ drain均未证明**。generic deinit此前 `0x2707ea→0x222b70` 目标确为单ret，不能当物理drain；其它上层helpers仍未闭合。
- **vendor分支只记录新假设，不冒充已解ISA**：`0x223400`/`0x223410` 的opcode67/funct3=1字段分别含literal2/7；用标准B immediate布局计算candidate targets为 `0x223468`/`0x22345a`，都邻接上述table block。`0x306170`同布局候选目标为 `0x306c0a`。这使后续能定向验证，而非继续错误JALR framing；**尚未证明它们是beqi、literal为比较立即数或taken edge**，engine仍unresolved，profile逐条标 `vendor_semantics_or_taken_edge_proven=false`，没有据此将a002→cleanup或HEVC candidate选择标proven。已排除的LLVM `+c,+xandesperf`不重试。
- **本轮私有证据与profile**：原件仍为上条DRAM/TCM输入，没有覆盖/改动。新 **`x9/out/hevc-codec-destructor-20261116/`** 保留create-path（184条/11 unresolved）、codec-ops-select（54/14）、table-deinit-wrapper（39/2）、codec-destructor（149/8）、initial-cleanup（122/11）、native-create-call（16/0）的独立分帧JSON/asm及只读r2窗口。`ops-tables-and-branch-fields.json`保存四个64B候选table与明确未证明branch fields，原标准caller候选不是完整callgraph。`vcp_artifacts.py:rv55_mem_wait_evidence`在同一个完整DRAM INIT长度/SHA256门下新增12段，**现40 instruction spans**；另保留64B table data、conditional deinit target、loop元数据及三个branch-layout candidates。runtime free/DMA retirement/cache handoff、selected table与完整控制流flags仍false，不生成boot/执行FW，也不提交固件/原始日志。
- **确切执行/复查/CI**：最终 **`x9/out/hevc-codec-destructor-final-20261116/`** 的 `extract-profile.py`沿正确repo内相对布局实读原件，`profile.log/.exit` **exit0/profile_match=true/40 spans/runtime_free=false/runtime_dma_retirement=false**，产物`mem-wait-profile.json`。`syntax.log/.exit`对唯一改变的Python源码syntax compile **exit0**，不是测试套件；未重编未变的kernel/frontend，未做成功构建产物二次检查。复查仍用 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/tools/rv32_slice.py x9/out/hevc-ring-producer-20261115/rv55-dram.bin --base 0x200000 --start 0x2d2648 --bytes 0x1c6 --r2 /opt/homebrew/bin/r2 --out x9/out/<新唯一名>/codec-destructor.json`；profile helper复制到新同级out目录再运行，禁止覆盖旧结果。`git diff --check`通过，`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，无CI run ID。无full link、XNU rebuild、测试脚本/套件、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作，无后台任务。
- **下一具体动作/仍未完成**：不要把H265_destroy/free循环存在当退休授权。先从cleanup前已知 **`0x306c8a→0x23ef84`（内call `0x23ef56`）**、**`0x306cf0→0x30b4e6`（tail `0x23ac86`，内部 `0x23ac3a`/`0x2259f6`/`0x26fec4`）**以及 **`0x306cf6→0x23bc28`** 闭合真实stop/release/排他行为；本轮只读窗口在`outer-pre-free-raw.asm`，不据名称、计数/锁或field相等作DMA/FW退出判断。接着分别补BS/FB独立producer排他与durable dequeue、per-buffer cache/DPB退休/c006 free ACK及持续session_ops。cold NS/controller/firmware/clock最高owner、DPB/layout/scratch协商、真实linear NV12/P010或HYFBC转换；HEIF mixed-derived colors、fractional LUT ICC显式linear intermediate、Rext单色alpha/aux、更多properties/implicit lengths、普通VT/CMPhoto/ImageIO自动路由和HDR/tone-map/chroma-siting实效仍全部保留。唯一最终boot继续等**全部代码齐全**，本轮不是硬解完成。

#### 2026-11-16 pre-free DVFS/job 与条件硬件 disable（最新交接）

- **分类/基线/交付**：从 tracked 干净、已 push 的 `853fae81` 继续同一 `.worktrees/hevc-hw` / `feat/hevc-hw`，证据代码提交 **`84e4318e`** 已实际 push `853fae81..84e4318e` 到 `origin/feat/hevc-hw`（最终目录 `push-code.log/.exit`，exit0）。本轮为 progress：新闭合软件链表/任务队列的实际 mutation，定位条件硬件 disable 与 counted-release 的新静态边；**无 native dequeue/free/ACK 或物理 DMA 退休实现**。未改 main、kernel/frontend、XNU 补丁或固定 `.local`，未启动子代理。完整 goal 仍 active，READY/profile/普通 VT gate 未开放，不合 main，不出 boot。
- **两类 helper 不再误当同步 stop**：`0x23ef84→0x23ef56→0x23ec6a` 以 context+`0x12e0` 为 key 查链表，成功后清 context+`0x1006` halfword、修改 node+92/+96 的相邻链接并清节点软件链接。原 literal `0x23f7e0` 是 `[VDVFS] ...remove from list`，只解释已定位的 static mutation。`0x23ac3a` 更新 `0x382a00[index]` byte/`0x382a10` counter；`0x23ac74` 的 vendor 算术仍 unresolved，不能连减法语义都默认已解。`0x23bc28` likewise 仅见两个候选软件 counter 的减法和 zero store，vendor 选择/地址计算不完整，**链表摘除、计数/锁/清字段均非 producer fence 或 hardware drain**。
- **type2 trigger 已追到实际提交队列**：`0x23ac96→0x23ad20` 传 a2=2，job+4/type、+16/context stores 已分帧；`0x23ac9e→0x23ae48` 以 queue **`0x382c10`** 经 `0x23ae8e→0x241ff4` 提交，后者在 `0x2420de/0x2420e2` 链入节点、`0x2420ec` 增 queue+20 count，随后 unlock/return0。原 literal `eVDecTheadsTrigger` 用于命名，不是执行或退出证据。这里是 task creation/submission，**没有证明 worker consumption/join、任何 BS/FB producer 排他或 buffer 退休**；不能把队列返回0直接充当同步 drain。
- **不是纯软件 helper：条件 disable 的真实窄路径已定位**：`0x30b4e6` 在 object+`0x1390` 存在、全局 option byte **`0x329730`** 与 indexed table **`0x3e1830`** 非零时，`0x30b54e→0x26fec4` 传 id5/data1；callee 从 object+108 取实际子对象，tail **`0x2490be`**，原 name literal 为 `eHalDisableHWById`。它构造 stack descriptor（+4 literal480、+12 literal1000、+20 object field380、+24 zero byte、+28 data），但 **`0x2490e8` 的 vendor encoding 会影响随后存 +16 的 selector；不能假定 selector=1，也不猜 480/1000 的字段角色**。`0x24910c→0x2258e2` 进入实际局部 register helper（conditional read/bit-clear、masked store），再 `0x249112→0x24897c` dispatcher。helper 返回 dispatcher 结果，**外层不检查就 `0x30b558` 清 option byte**；该路线本身可被 flags 跳过，不能成为通用 free/退役许可。raw MMIO base 常量不移植到 native，也不执行任何寄存器操作。
- **下一条真正硬件边与未解范围**：dispatcher 中静态 candidate **`0x248e84→0x224aa0`**、内部 **`0x224b46→0x224308`**、其 **`0x22435e` / `0x224376`** 相邻 standard AUIPC/JALR pairs 都计算为 TCM **`0x20f8`**，不是 DRAM 同偏移；candidate 的完整 dispatcher 分支仍未知。真实 TCM slice 显示 nonNULL object 下 calls `0x1d94`，仅其返回0后 calls `0x1f90`，并有 -25 fallback/间接 callback 及错误处理；**整个 TCM helper 的实际 power/clock/DMA 语义仍未闭合**。下一优先追 `0x1d94`/`0x1f90` 的对象/typed callback 来源与实际释放 target，再证明 dispatcher selector 与退出条件；不据 count decrement、局部 register write、c002/status0、destroy 名称或 transport 成功授权 DEVICE→CPU/free/d006。
- **确切材料/执行**：新只读资料 **`x9/out/hevc-pre-free-stop-20261116/`**；独立分帧 list-detach42/5、list-find52/7、software-vote24/3、thread-trigger49/4、job-create-enqueue125/13、software-release28/5、hardware-disable-descriptor57/7、disable-local-registers73/6、local-register-access35/8、hardware-dispatch538/70、task-queue-submit101/7、disable-candidate69/13、counted-release70/6、TCM-release54/4（条数/unresolved）。所有原 raw r2 窗口及派生 JSON/asm 保留，不提交固件/设备/原日志。DRAM 原身份门不变；TCM full declared payload 184320B/SHA256 `f59d646ba74fab1c40500139c40d67c97a0e8ab96c1569f738a0c7403cc49508` 实读匹配。第一次 span extraction 发现 `0x248820` 在4B `csrw` 内部而 exit1、无 JSON，修正 end 为 `0x248822`，没有沿 r2 fallback 造指令。最终 **`x9/out/hevc-pre-free-stop-final2-20261116/`** 的 `syntax.log/.exit`（唯一变更 Python 源码 syntax compile）、`extract-profile.py` / `profile.log/.exit`、`extract-targets.py` / `targets.log/.exit` 全部 **exit0**；profile_match=true、**63 spans**，新增23段边界全部由 exact-PC JSON 覆盖，两个 TCM target 均20f8，runtime free/DMA retirement/cache handoff/producer exclusion/hardware disable flags 全 false。早期 `...stop-final-20261116/` 的63-span profile 是修正边界/最终新增 flags 之前的中间版本，不当最终证据。复提 profile/targets helper 到新的同级 out 目录再执行，JSON exclusive-create，不覆盖旧材料。
- **构建/CI/完整余项**：`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，原结果在中间 final 的 `gh-runs.*`，无 CI run ID。没有 kernel/frontend 编译、full link、测试脚本/套件、成功构建产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作或后台任务。BS/FB 独立 producer exclusion/durable dequeue、input/DPB/output cache 与逐 buffer 退休/c006实际 ACK、持续 session_ops、cold NS/controller/firmware/clock 最高 owner、DPB/layout/scratch 与真实 linear NV12/P010/HYFBC 转换仍缺；HEIF mixed-derived colors、fractional LUT ICC linear intermediate、Rext 单色 alpha/aux、合法 properties/implicit lengths、普通 VT/CMPhoto/ImageIO 路由及 HDR/tone-map/chroma-siting 实效亦未完成。唯一最终 boot 继续等**全部代码齐全**。

#### 2026-11-16 fractional LUT working ICC 的显式 matrix intermediate（最新交接）

- **基线/实际代码**：继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，从已push且tracked干净的 **`37bd0167`** 开始；上一turn的pre-free软件mutation/条件disable证据为progress。本轮四文件实际frontend代码 **`330243c9`** 已push成功 `37bd0167..330243c9` 到`origin/feat/hevc-hw`（最终目录push-code.log/.exit，exit0），不改main、共享`.local -> ../../.local`或kernel，不启动子代理。完整HEVC/HEIF goal仍active；这是一个独立生产渲染缺口的接线，不是完整物理provider/硬解成功，READY/profiles/普通VT gate不放开，无boot/探针。
- **真实调用链/ABI**：新增export **`X9HEIFCopyHardwareImageWithIntermediateICC(file,id,workingICC,intermediateICC,sourcePolicy,clapPolicy,budget,...)`**；旧WithOptions原ABI委托新函数并传NULL，旧exact入口/256MiB不变。nonNULL intermediate要求LINEAR_CEIL且为有效RGB ICC；真实owned CFData与CGColorSpace保留，CoreGraphics `CGColorSpaceCreateExtendedLinearized`从**显式选中base**派生，核对RGB/3components/extended/output及实际ICC。不把suppliedICC直接当linear、不选默认sRGB、不在invalid intermediate失败后回退workingICC。最终workingICC独立核output资格，**不再要求它能被matrix-linearized**；LUT是否被实际VT支持仍由转换返回值/ICC检查gate，不能宣称任意LUT均可用。
- **实际像素路径/不制造无用API**：CLI的owned intermediate输入已真实调用新export。需插值的plan：每个真实DecoderID/UsingHardware coded frame直接通过VTPixelTransfer进入**派生linear ICC/RGBA-float**，已有linear compositor执行ordered fractional clap，最后**一次**真实VT transfer回原workingICC/RGBA-half；并非先在nonlinear suppliedICC插值或只改attachment。无插值的LINEAR_CEIL图也预先资格检查显式intermediate，但丢弃其派生空间，仍直接在workingICC精确copy，不无故多转两次。所有这些CoreGraphics/profile资格与已知root float+half heap peak检查发生在首硬解前；VT内部存储/实际profile支持及动态allocation失败不由此预先认证。aux/mixed-derived colr等原拒绝不放宽，finite/opaque alpha1、所有真实transfer返回ICC与owned输出/失败释放检查保留。
- **可重放接口/元数据**：CLI（本轮**未执行**）新增 `vdecctl decode-heif file.heic 0 final-lut-rgb.icc icc linear-ceil 1024 --linear-intermediate matrix-rgb.icc NEW.rgba16f NEW.info.plist`；也可省预算保留linear-ceil默认1024MiB。旧exact/linear-ceil语法均保留，unknown/重复flag、exact+intermediate、缺flag值或非法预算在文件读取/硬解前拒绝。info保留原WorkingRGBICCProfile、实际LinearRGBICCProfile，新增 **RequestedResamplingIntermediateRGBICCProfile / ExplicitResamplingIntermediateUsed / LinearizationBaseSelection / LinearizationBaseRGBICCProfile**，区分请求/是否使用/实际base；integral图不虚称linear插值。manifest取消“matrix working ICC必需”，另记显式matrix intermediate代码可用与LUT runtime_verified=false；physical_provider/hardware_tested/complete compositor/automatic injection等仍false。
- **确切编译/源码审查**：当前四文件源码以 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/heif-intermediate-icc-20261116/frontend` 编译、链接dylib/CLI、ad-hoc签名，**exit0 / -Wall -Wextra -Werror无诊断**。记录 `x9/out/heif-intermediate-icc-20261116/{frontend.log,frontend.exit,source-head.txt}`，source-head=330243c9；没有构建后二次解包/hash/反汇编/复建比较。两个实际改动C TU按同iOS16 target/SDK源码analyzer，`x9/out/heif-intermediate-icc-static-20261116/{analyze-source.py,analyzer.log,analyzer.exit}` **exit0/无diagnostic**，未压制analyzer，未复析未改compositor。人工source diff检查NULL/显式/无插值分支、所有release路径、CLI选项边界、最终ICC与base隔离；`git diff --check`通过。未运行测试脚本/套件、sample/CLI/像素或色彩实效验证、kernel/full link、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作。复编必须改为新唯一out名，不重复已成功且未变的产物。
- **CI/工具收尾**：`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0 / `[]`**，最终目录gh-runs.json/.stderr/.exit，无CI run ID。frontend/analyzer/gh/代码push均已terminal，当前无后台任务/设备任务或子代理。交接只写本NOTES；未新增报告/timeline，原料与派生资料均在本树gitignored x9/out/。
- **本轮收束的TCM负边界（非native行为补丁）**：上turn未交接的只读材料 **`x9/out/hevc-tcm-release-owner-20261116/`** 已核对，原独立分帧unprepare67/4、release-count40/7、disable68/6、scalar-load8/3（条数/unresolved）。TCM20f8调用1d94和1f90，二者都先调1e42，调用前分别设a1=1/3；object+28/+29的byte decrement及局部非零计数分支可见，但选择counter的vendor分支与**1eb6最终return算术未解**。1d94:1da0先置s1=0，1da2非零gate跳1e1c的mv a0,s1返回序列；1f90:1f9e置s3=0，1fa0非零gate跳2040的mv a0,s3返回序列（vendor epilogue仍未解）。这些分支绕过实际callback；**外层helper返回0并不证明callback执行过**，不能把counted clock release成功当DMA退出。条件callback候选仍为object+24→block+24（1dd2）与block+8（1fd0），另有lookup/parent遍历；取值helper1449c四个lbu后3条combine vendor编码未解，不假称atomic/u32完整语义，更无runtime安装target来源。
- **窗口/原件区分**：本轮新增 **disable-main-return-window.json/.asm**（start1f90/bytesb8/end2048，**70/7**）补主返回epilogue，原disable68/6不覆盖；**错误branch到2048的尾部仍不在新窗中，不称完整函数/CFG**。原TCM177d5字符串`%s disable unprepare fail %d`只支持generic clock helper解释、不证明DMA drain。captured DRAM3296ac/3296d4/329704的64B窗口全零只说明image初始/BSS，不等于runtime table为空；clock-data-windows.json/203a0数据亦非已安装callback指针。没有修改固件或提交设备/Apple/原日志，未改vcp_artifacts63-span profile/任何runtime退休flags。
- **仍缺/下一真正工作**：fractional final LUT的显式matrix intermediate生产链已接，但**实际色彩/LUT/HDR支持尚未验证**，不将此代码扩大为完整HEIF。继续补BS/FB独立producer排他与durable dequeue；input/DPB/output cache、per-buffer退休/c006真实free ACK；持续session_ops provider、cold NS/controller/firmware/clock最高owner、DPB/layout/scratch与linear NV12/P010/HYFBC真实转换。HEIF mixed-derived颜色、Rext单色alpha/aux、合法properties/implicit lengths、普通VT/CMPhoto/ImageIO自动路由和HDR/tone-map/chroma-siting实效仍未完成。针对TCM先取得实际callback安装/owner与退出target，不能重复count/双snapshot/b001/b005/软件flag猜测或造free/d006。唯一最终boot仍等全部代码齐全；功能树保留，不合main。

#### 2026-11-16 VDEC/VENC clock 角色纠偏与 TCM domain 注册（最新交接）

- **上一turn/当前交付**：上一turn的显式matrix intermediate真实frontend代码 `330243c9` 与交接 `af4fab2b` 为progress。本轮从tracked干净的af4fab2b继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，新证据代码 **`8b6316aa`** 已push成功 `af4fab2b..8b6316aa` 到origin/feat/hevc-hw（最终目录push-code.log/.exit，exit0）；不改main/kernel/frontend/XNU补丁/固定`.local`，不启动子代理。完整goal保持active，不将静态注册/clock vote包装成native退休或硬解成功；READY/profiles/普通VT gate不变，无boot。
- **改变下一动作的实际角色证据**：此前条件disable dispatcher的 `0x248e84→0x224aa0→0x224308→TCM20f8`仍只是candidate，完整selector分支未闭。现在实际 `0x224b40` materialize的字符串 **`0x226d3e = venc_clk`**，对应DRAM初始化 **`0x223b0e`** 中lookup IDs **4/7/9/11/13**，结果写入 **3296ac/3296b0/3296b4/3296b8/3296cc**；软件index table3296d4。捕获TCM对象的这些ID实际name均为**ven1_venc / ven_c0_p1_venc / ven2_venc / ven_c1_p1_venc / ven_mdp_venc_adab_ctrl**。这是实际参数/数据一致的**VENC角色证据**，不是单独命中字符串；不能继续把224aa0默认当VDEC stop或DMA fence，更不能据其count递减放开free。
- **真实VDEC独立初始化/候选**：DRAM **223a34** 中标准AUIPC/JALR分别以IDs **2/0/1** 调TCM **19a4**，结果store **3296a0/3296a4/3296a8**；捕获对象name分别 **vde2_vdec_cken / vde1_lat_cken / vde1_vdec_cken**。另一函数 **2244ba** 的 **224570/22459e** 调counted helper224308，实际参数name为 **226c43 vdec_clk / 226c36 vdec_soc_clk**。其index/vendor算术、完整进入条件与runtime target仍未闭；本轮standard JAL/C.J/AUIPC-JALR对2244ba的限定候选扫描为空，**不等于没有间接调用或证明dead code**，不重复无新证据的同类扫描。下一追正确VDEC owner/worker/安装边，不再扩大VENC candidate。
- **TCM安装来源已不再只停BSS零值**：新定位initializer候选 **9154**：**917e→22d4** 以entries14/index0/storage **24688** 创建vector候选；loop从**20018**取14个**59B packed对象**，每步stride59，读取ID低9bits，**91a0 vendor-index计算仍未解**，不能称vector已正确装好。**91ba→2322** 的真实注册参数是domain0、name **18901 mm_hwccf**、该vector block、register-base31400000；2322的标准mul12/基址 **20c10**，**234c/234e**实际store name/vector到table+0/+4，+8另有mapping结果。真实注册代码存在，**但cold boot调用、运行时执行、表覆盖排他及lookup正确返回均未证明**，不把函数或table初始值当已安装owner。
- **实际captured对象/ops与callback负边界**：14个对象都在+24保存 **18100**，+8是上述实际name ptr，+28/+29初始counter bytes均0（只是image初始数据，不是runtime计数）。18100的68B表实际 **+24=38f0、+8=0、+64=3d06**；generic1d94/1f90分别取block+24/+8。**38f0**真实读取packed字段，末尾 **3976 tail→2d64**，a2=1；完整clock vote/硬件退出CFG仍缺，本轮只保存2d2a..2f64的182/19窄窗口，不称完整。**3d06**的实际name **17fb0 v1_hwccf_cg_voter_timeout_dump**，读取寄存器（921c是标准lw/ret）并以1712a记录`%s hwccf reg...`/`%s subsys reg...`；这是-25 fallback的diagnostic候选，**不能当修复/disable/producer exclusion或DMA drain**。取值1449c的vendor combine与generic计数/branch/epilogue仍未解；helper返回0、captured disable ptr0、注册store本身均不授予退休。
- **已接现有artifact入口，不是未调用空API**：`vcp_artifacts.py`新增 **rv55_clock_install_evidence(tcm,dram,init)**，并由真实 `firmware()` 提取路径输出 **service_clock_install_evidence**。原DRAM完整INIT长度/SHA256门与完整TCM184320B/SHA256门共同限制本解释；20段新raw instruction evidence、14对象/68B ops data、VDEC/VENC初始化/名字/存储和callback边界可重复提取。runtime_registration/callback_target/decoder_stop/producer_exclusion/DMA_retirement及scalar vendor语义flags全false。原mem_wait **63 spans不变**，仅在旧candidate增加实际venc_clk与“不是已证VDEC stop”注解；未新增native dequeue/free/ACK或制造DMA owner。
- **确切分帧/最早不确定stage已纠正**：私有新资料 **x9/out/hevc-clock-table-install-20261116/**，manager-prefix-aligned349/36、manager-lookup-init241/18、manager-init-entry-aligned40/1、tcm-registration-entry43/5、tcm-domain-register-entry23/2、tcm-clock-lookup26/6、tcm-vector-create29/6、prepare-unprepare92/11、fallback51/6加return3/2、map-and-access35/4、vdec-release-wrapper124/19（条数/unresolved）。prefix初次end2247fe在4B指令内而拒绝，改224800；init-entry初次end223aae在223aac ADDI内而拒绝，改223ab0，失败stdout保留，无伪JSON。早期tcm-registration-caller起点9130与tcm-domain-register起点22e0实际在前一4B指令中，**两者不是权威分帧窗口**；已从真实调用/前ret确定9154、2322重新独立取entry，旧件不覆盖，最终profile framing明确跳过两旧窗。不是沿r2 fallback猜边界，不把engine acceptance当vendor语义/完整CFG。
- **最终可重放/执行结果**：**x9/out/hevc-clock-install-final-20261116/** 的syntax.log/.exit：唯一变更Python源码syntax compile **exit0**；**extract-profile-exact-entries.py / profile-exact.log/.exit** **exit0/profile_match=true/new20 spans全部独立exact-PC raw bytes与边界覆盖**，产物 **clock-install-profile-exact.json / mem-wait-profile-exact.json / span-framing-exact.json**，source-head.txt=8b6316a。初版extract-profile.py/profile.log保留，但以排除两旧mid-instruction窗口后的exact-entries为准。重提需将helper复制到新同级out目录，JSON exclusive-create，不覆盖旧件。`git diff --check`通过；没有kernel/frontend编译/full link、测试脚本/套件、成功构建产物二次检查或设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作；原固件/日志/Apple输入不入git。
- **CI/收尾状态**：`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，最终目录gh-runs.json/.stderr/.exit，无CI run ID。本轮r2、候选/最终profile提取、syntax、gh、代码push均已terminal，无后台构建/设备任务或子代理。只写本NOTES交接，不新增报告/timeline；所有原料和派生资料仍在本树gitignored x9/out/。
- **完整余项/下一行动**：正确VDEC clock owner的进入/安装与2d64真实硬件vote退出仍需闭合；独立FW BS/FB producer排他/durable dequeue、input/DPB/output cache与逐buffer退休/c006真实free ACK、持续session_ops provider、cold NS/controller/firmware/clock最高owner、DPB/layout/scratch/linear NV12/P010/HYFBC实际转换均未完成。HEIF mixed-derived色彩、Rext单色alpha/aux、合法properties/implicit lengths、普通VT/CMPhoto/ImageIO路由及LUT/HDR/tone-map/chroma-siting实效仍缺；上一轮显式intermediate不重编。不要拿VENC名、captured ops、注册store、count/返回0或clock diagnostic绕过这些不变量；唯一最终boot仍等全部代码齐全。功能树保留，不合main。

#### 2026-11-16 Clock-voter register 轮询与 ASSERT/ECALL 错误出口（最新交接）

- **活动树/交付**：继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，从 `8411563d` 干净基线接上轮未交付的离线切片；新证据代码 **`15d082b6`** 已push（`8411563d..15d082b6`，push-code.log/.exit=0）。只改 `x9/tools/vcp_artifacts.py` 与本交接，不改main/native/frontend/XNU补丁/固定`.local`，不启动子代理。完整HEVC/HEIF goal active，READY/profiles/free/普通VT门保持关闭；没有boot或物理硬解成功结果。
- **错误出口的新实证**：2d64 voter 的 `33f6=-25`、`338c=-30` 及 `2e80=-22` / `2e98=-29` 汇入 **33f8**；`3402→1712a` 实际格式 **17a23 `HWCCF vote fail, error code: %x`**，之后 **3406 `csrci mstatus,8`**、**3414→10266**，实际参数 **179f8 `../common/drivers/hwccf/v5/src/hwccf_api.c` / line699**。10266入口先关MIE，经1028c或10300记录实际 **`[ASSERT] file: %s, line: %u` / `[ASSERT] task: %s, file: %s, line: %u`**，共同10362记录Module；**10366 a0=50、10374 tail→114d0、114d6 a7=a0、114da ECALL**，wrapper后有114de ret。**trap handler/disposition未核实，不能称必然不返回，也不能把此错误当普通可恢复errno或保证-25 fallback会执行**。上一轮3d06仍只证明register-read/log diagnostic，不是恢复/disable/drain。
- **实际write/poll/成功status边界**：`9204` 保存mstatus，`9208 csrci mstatus,8`，`9210` vendor fence未解，**9214 sw a1,0(a0)** 后恢复保存的mstatus；mask8是 **bit3/MIE，不是bit8**。2d64输入在2d6c把a3保存为s0 mask；**2f42/2f9c→9204** 实际a1=s0，随后921c读值并mask轮询。ordinary index<=51与special52/53/54路径分离；观察literal627/626/14，但有opaque vendor limit/test branches，不据数字生成完整timeout CFG。最终 **3156→921c、315c masked compare→31a0、31a4→161ae、31a8 s1=0、31aa→3418、3418 a0=s1、3424 ret**；vendor地址/bit角色与epilogue仍未解，不能把zero status或clock readback当producer排他或buffer退休。ret后3426..3434还有out-of-line跳转、3436才见下一prologue，不称已闭完整函数CFG。
- **15ed4已核实到实际counter等待，时间单位仍未知**：入口 **15ed6 factor26、15edc→12a3a**，**15ee8** 从 **29b6c** 取counter pointer，**15ef0/15ef2** 重复读取+8，**15f08** 按差值继续轮询；乘法/组合/差值有vendor op未解，未核timer calibration。voter调用时a0=16仅是literal，**不标成16µs、不用循环计数×16制造硬件退出期限**；等待不是DMA drain。
- **证据代码/可重放**：现有 `firmware()` 实际输出的 `service_clock_install_evidence` 在原full TCM184320B SHA及full DRAM INIT门内扩展 **22新span / 合计42span**，新增vote_register_sequence / vote_delay / vote_error_frontier与3个未证明flags；原mem_wait63spans不变，所有runtime_registration/callback_target/decoder_stop/producer_exclusion/DMA_retirement仍false。新增窗口在 **x9/out/hevc-clock-vote-exit-20261116/**：clock-vote-tail415/32、return26/6、fatal-entry115/17、delay-entry88/17、fatal-dispatch116/7、assert-prelude29/3、delay-multiply27/2（instructions/unresolved），各helper stdout/exit0。这些都是只读原件离线切片，不是运行固件或成功构建产物二次检查。
- **最终结果/约束**：**x9/out/hevc-clock-vote-final-20261116/** 的syntax.log/.exit **0**；**extract-profile-exact-entries.py / profile-exact.log/.exit 0**，profile_match=true、**全部42span独立exact-PC/raw/boundary覆盖**，输出clock-install-profile-exact.json / mem-wait-profile-exact.json / span-framing-exact.json，source-head.txt=15d082b6。helper继续排除前轮9130/22e0的mid-instruction旧窗；重提先复制helper到新同级out目录，JSON exclusive-create。`git diff --check`通过；gh-runs.json=`[]` / exit0，无CI run ID。未kernel/frontend编译/full link、测试脚本/套件、boot pack、ADB/fastboot/mailbox/MMIO/SMC/设备或分区操作；固件/Apple/原始日志不入git。
- **下一具体动作/完整目标未缩小**：继续追正确VDEC初始化223a34、release2244ba的owner/worker/安装来源，或TCM9154 cold caller；不重复已空的2244ba标准call扫描、不将VENC候选回退成VDEC stop。clock-voter更完整的register/ASSERT证据仍没有独立BS/FB producer排他、durable dequeue、逐buffer retirement/c006 free ACK或cold NS controller authority；持续session_ops、cache/DPB/layout/scratch/linear NV12/P010/HYFBC、HEIF mixed-derived/alpha合法properties与implicit lengths、普通VT/CMPhoto/ImageIO路由及LUT/HDR/tone-map/chroma-siting实效全部仍在完整必做清单。唯一最终boot继续等全部代码齐全，不合main。

#### 2026-11-16 Startup hart 分流接到 registry / 独立 VDEC 初始化（停止时最新交接）

- **第二里程碑/固定源码**：本turn继续窄追两个此前未扫描的init目标9154/223a34，再按新caller追一层；新证据代码 **`8cfdecb8`** 已push（`15d082b6..8cfdecb8`，`x9/out/hevc-clock-startup-final-20261116/push-code.log/.exit=0`）。本turn两次代码提交15d082b6 / 8cfdecb8，活动树/分支不变；只改现有artifact入口与NOTES，未增加native owner或放开任何READY/free gate。
- **真实static registry调用来源**：TCM **11030**（前ret1102e）内 **1107a→9154**；再上一层startup候选 **fdc6**（前retfdc4）读取 **fdca mhartid**、保存/重新读取stack值，**fdd8 beqz→fdfe**，hart0侧 **fe3e→11030**。所以9154已不再只有孤立注册函数：有startup hart0→platform init→clock domain initializer的直接static调用链。**startup实际执行、vector vendor index、跨hart可见性和runtime lookup仍未证明**，initializer_runtime_called/runtime_registration flags仍false，不把image调用边当现实installed authority。
- **独立VDEC实际进入边/顺序不能混淆**：同一个startup在非零hart路径 **fdf4 AUIPC / fdf8 JALR→DRAM316948**；但此前 **fddc** 是opaque vendor selector，尚未证明它只选hart1/实际执行。316948（前ret316946）中 **31696a→223a34**，之后316970→227eb0；初始化223a34的lookup IDs2/0/1仍是实际VDEC对象。调用clock init前，**316962 / 316968** 分别经 cells **3389e8 / 3389f0** 指向的对象+8间接调用；**对象身份、填充writer、实际callback与同步含义未知**。hart0 registry链和非零hart VDEC链是分支分流，不是连续先注册再lookup；不能凭纸面顺序授权clock owner或读到正确vector。
- **来源/可重放**：新私有目录 **x9/out/hevc-vdec-clock-install-entry-20261116/**，find-init-candidates-spaced.py与find-init-parent-candidates.py只扫描上述新目标的标准JAL/C.J/AUIPC-JALR，候选仍须entry独立分帧；没有重跑已空2244ba扫描。startup-init-caller51/4、tcm-init-owner-entry-aligned74/7、vdec-install-entry76/6、dram-subsystem-init-entry71/10，r2/extractor exit0。最后一个DRAM窗口3262ee以后是字符串/data，**不当code CFG**，本次profile没有取此data区域。初始find-init-candidates.py机械替换缺空格产生Python SyntaxWarning，改用新spaced文件重复确认相同候选；初次TCM窗口误用0x100而非已算出的0x102，在末条1112e的4B内拒绝，失败日志/exit1保留、没有伪JSON；新aligned窗口按0x102成功，旧件不覆盖。
- **最终代码证据/执行结果**：`service_clock_install_evidence`又加 **5span，最终合计47span**，新增startup_hart_split及静态caller字段；新 `startup_hart_ordering_and_vdec_owner_proven=False`，所有runtime退休/注册、vendor mask/time/trap flags仍false。**x9/out/hevc-clock-startup-final-20261116/** 的syntax.log/.exit=0、extract-profile-exact-entries.py / profile-exact.log/.exit=0，profile_match=true，**全部47span独立exact-PC/raw/boundary覆盖**，clock-install-profile-exact.json / mem-wait-profile-exact.json / span-framing-exact.json、source-head.txt=8cfdecb8；mem_wait63spans未变。重提复制helper到新同级out目录，JSON exclusive-create。没有测试套件、kernel/frontend编译/full link、boot、成功构建产物二次检查或设备/MMIO/SMC/ADB/fastboot/mailbox/分区操作；前轮成功frontend未变，不复编。
- **CI/停止时下一实际stage/完整goal active**：最终 `gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0 / `[]`，gh-runs.json/.stderr/.exit在startup-final目录，无CI run ID。优先定向追 **3389e8/3389f0对象的安装writer与+8 callback**，以及startup非零hart selector/registry-ready同步；有了这条新真实入口后再验证正确VDEC release2244ba的owner/worker，不重跑旧空call类。clock-voter ASSERT/ECALL仍不能当可恢复-25、clock readback不能当DMA fence。cold NS/controller authority、独立BS/FB producer排他/durable dequeue、cache/DPB/output逐buffer退休/c006 ACK、持续session_ops provider、layout/scratch/linear输出、HEIF mixed-derived颜色/alpha/合法properties/implicit lengths、普通VT/CMPhoto/ImageIO及LUT/HDR实效全部仍未完成。唯一最终boot仍等全部代码齐全；不合main，不改`.local`，本turn所有r2/extraction/git任务已terminal，无后台编译/设备任务或子代理。

#### 2026-11-16 Pre-init 软件 service 对象身份与 task-delete 边界（最新交接）

- **上一turn/当前状态**：上一turn为progress（新源码15d082b6 / 8cfdecb8、交接6e1b7c24改变了下一static动作）。本turn从tracked干净的6e1b7c24继续 `.worktrees/hevc-hw` / `feat/hevc-hw`；证据代码 **`9020f634`** 已push（`6e1b7c24..9020f634`，最终目录push-code.log/.exit=0）。仅改 `vcp_artifacts.py` 与本交接；没有native/kernel/frontend/XNU改动、main合入、`.local`修改或子代理。完整目标active，READY/profiles/free/普通VT门仍不放开，无boot。
- **纠正“cells尚待安装”的最早stage**：直接读取捕获DRAM，**3389e8=20b88、3389ec=20bb8、3389f0=20ba0**，不是这三处BSS全零。三个连续 **24B / magic5a5a5a5a** TCM descriptor 的+4实际name为 **1c3b0 vdec_service / 1c950 venc_service / 1c3bd vdec_res_srv**；+8分别 **114e0 / 1192c / 11624**。startup316962 / 316968调用的是第一个与第三个对象+8，所以现在其**captured身份/初始化callback已闭合**。这些descriptor本身曾在旧service-dispatch提取器中出现，本次新证据是从真实startup cells接到它们；不能把旧descriptor或此次image pointer值当runtime覆盖排他/实际已安装证明。
- **+8不是clock owner/barrier**：114e0在1150e/11512注册service1/116f2，实际log **`vdec_service_init`**；创建两个 **entries2 / record72B** 的软件queue，handle cells **1f870 / 1f878**，以descriptor20b88创建任务。11624注册service2/118ac、创建entries2/72B queue1f874与descriptor20ba0任务。generic30501e真实调用descriptor+12，两个task body为 **327ea6 / 328024**。补齐service1 **327ef6→queue1f870、327f06→TCM5e08**，a2=-1，outer service/length在stack+0/+4、payload+8；**327fc2→305c4c** 然后回327ef6，与既有service2+16/3280d2路径并行。vendor queue选择/dispatch前置判定仍未解，不能称排他或完整worker CFG。startup最早的 **fdd2→16344** 已核：本体只有 **16344 csrw 0x9c7,a0 / 16348 ret**，下一函数1634a开始；**没有registry-ready等待/屏障证据**，CSR语义也不自造。
- **真实exit路线不是producer/DMA退休**：descriptor+20是实际named **327fc8 `vdec_service_exit` / 3280d8 resource exit**，但其最高层invoker仍未闭。327fca→305a8a先对 **338a10/338a14** 的core/res task handles调用24710e，327fd6再对 **29750** 的service task handle调用24710e。24710e的实际name **247f78 `eVideoThreadExit`**；从handle取值，**247148→TCM753c**（实际log `vTaskDelete Done`），随后 **247160 a0=0 / 247164 ret**，没有依据task-completion/DMA drain核验返回值，也没有在wrapper写回handle=NULL；不要把该日志当状态mutation。753c读取mhartid，出现scheduler list修改/cleanup/return或yield分支；vendor index/完整选择仍未知，**不能把task delete或scheduler对象free当codec input/DPB/output退休**，更不能替代c006 ACK。native现有quiesce/stop合同仍明确要求真实halt/drain，未发现可据此放开的退休通路。
- **实际代码入口/未证明flags**：真实 `firmware()` 的 **service_dispatch_evidence** 从12扩展到 **25span**，新增descriptor name/初始化/exit/member cells、service1队列/worker、named_service_exit与两个task-exit runtime flags（全false）。**service_clock_install_evidence** 加16344窄span变 **48span**，接captured pre-init names/callbacks并明确只是software services，不证明跨hart registry ordering。原mem_wait仍63span；所有runtime dispatch/registration/ready/producer-exclusion/DMA-retirement仍false，没有native dequeue/free/成功空provider或新hardware owner。
- **原始/最终资料与确切验证**：新原始资料 **x9/out/hevc-vdec-preinit-objects-20261116/**（captured-preinit-objects.json、ranges与独立r2窗口）：res-init88/7、service1-entry98/9、service1-exit27/4、resource-entry60/4、resource-exit52/8、exit-prelude139/23、thread-exit137/13、task-delete157/28、initial-hart-aligned65/6、旧service callback45/7、generic-task/registrar32/5、task-create29/2（instructions/unresolved，helper exit0）。thread-exit窗口2471f2以后为strings，profile仅取24710e..247166，不当data为CFG；initial-hart的初次0xd0误用在16412的4B内拒绝，失败exit1/log保留且无JSON，按已算出的0xd2重新取aligned成功，最终只取真实6B小函数。**x9/out/hevc-service-preinit-final-20261116/** 的syntax.log/.exit=0、**extract-profile-exact-entries.py / profile-exact.log/.exit=0**，两个profile均match，**全部48+25span独立exact-PC/raw/boundary覆盖**且captured cell pointer与descriptor地址一致；clock-install-profile-exact.json / service-dispatch-profile-exact.json / mem-wait-profile-exact.json / span-framing-exact.json，source-head.txt=9020f634。重提复制helper到新同级out目录，JSON exclusive-create；继续排除旧mid-instruction9130/22e0窗。
- **CI/下一具体动作/停止约束**：`git diff --check`通过；最终 `gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0 / `[]`，gh-runs.json/.stderr/.exit在final目录，无CI run ID。当前startup之前两个callback的captured身份已确定，**不再重复追“它们是clock ready屏障”**。下一沿正确VDEC223a34传入203a0/203a3等packed描述符、TCM15208/150dc的实际421c/4056注册候选边，先核字段/注册/callback语义（是否IRQ及lat/core角色仍待核），再核真实owner与registry同步；或核正确VDEC2244ba的间接注册来源，不重跑旧空standard caller类。cold NS/controller/bootstrap authority、独立BS/FB producer排他/durable dequeue、input/DPB/output cache及逐buffer退休/c006 ACK、持续session_ops、DPB/layout/scratch/linear NV12/P010/HYFBC转换、HEIF mixed-derived颜色/alpha/合法properties/implicit lengths、普通VT/CMPhoto/ImageIO与LUT/HDR/tone-map/chroma-siting实效均仍必做。没有测试脚本/套件、kernel/frontend/full link/boot、成功构建产物二次检查或设备/MMIO/SMC/ADB/fastboot/mailbox/分区操作；所有任务terminal，无后台句柄或子代理，功能树保留不合main。

#### 2026-11-16 VDEC packed IRQ 注册→分发→ACK / shared semaphore 边界（最新交接）

- **活动基线/代码里程碑**：从tracked干净的`5e41311b`继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，新证据代码 **`1f17976`**。新增 `rv55_vdec_irq_evidence()` 并接入实际 `firmware()` 的 **service_vdec_irq_evidence**，不是未调用接口；没有修改native/kernel/frontend/XNU、main、共享`.local`或使用子代理。完整HEVC/HEIF目标active，READY/profiles/普通VT/DEVICE→CPU/free门仍不开放，未出boot。
- **packed注册/真实table consumer**：VDEC223a34以 **203a0=`27 0c 01` / callback15208 / context0** 在223a6c调用 **421c**；以 **203a3=`28 0c 01` / callback150dc / context0** 在223a90调用同helper；第三个 **2039d=`26 0c 01` / callback223a30**（实际仅ret）。421c读三个byte，424c的vendor index算术仍未解，**4264/4268**分别写callback/context至基址 **20fa0** 的entry+0/+4；426c→3fb0→3eaa拆字段并配置寄存器，失败路径4270显式清callback，没有显式清context。后续dispatch候选4288经 **42a2→43f8** 取source，**42b6**标准mul40定位table，**42c2/42ca**取callback/context，**4304 a0=context / 4306 jalr callback**。43f8读2120b18c/2120b01c候选寄存器，source/index及hart gate、registry执行/覆盖排他仍未解，不称实际已安装ISR。
- **不能把control调用直接当IRQ已启用**：223a76/223a9a/223ac0调用 **4056** 前只恢复a0 descriptor，**没有显式设置a1**（在421c返回之后）；4056非零a1走register OR、零走AND-NOT，mask=`1<<(field0&31)`，bank=`field0>>5`，base literal2120b040，4082 vendor地址算术未知。3eaa的field1参与routing地址`<<4`，field2控制另一register OR/AND，**byte1=12/byte2=1的priority/affinity/触发类型等语义不自造**；vendor fence和跨hart同步亦未证明。
- **LAT/core角色由原始日志闭合，AP地址参数不是RV55直接指针**：15208实际format **`DEC LAT ISR` / `wait lat irq fail` / `vdec_lat_misc_addr`**，150dc实际 **`DEC ISR` / `wait core irq fail` / `vdec_misc_addr`**。分别以 **36010000 / 36025000**、共享 **36000000** 调 **10fdc**（a1=0），读取共享+0/mask16与local+164，local bit-test/gate含vendor编码。10fdc有captured **1c1e0 / 16B records** 的region/attribute比较、region maskf8000000、offset mask07ffffff及候选output OR offset；本轮只保留7个完整records，**不称完整table/运行时映射结果，更不把AP-address参数授权为XNU MMIO或直接RV55地址**。
- **ACK和signal不是逐buffer退休**：LAT **15294→1517e**，core **15168→15052**；ACK均对映射local+164先OR17、再AND(-17)，各短段保存mstatus/关MIE bit3/恢复；随后对 **36098000** 映射结果+12发 **LAT1 / core16** 再0脉冲。vendor fence/真实运行未证。条件尾路 **152a4 / 15178→14fda**，分别传shared handle cells **1f1b4 / 1f1b0** 和a1=1；14fda读handle，ISR分支 **14fe8→6d36**，thread分支1502c→6788，另有conditional yield57da。原始日志为`Semaphor reaches maximum count!`/`sem handle is NULL`，6d36读queue+56/+60检查limit、**6da6**递增+56，另有6df6→8378 waiter操作/vendor wakeup。VDEC init223a46/223a52→246fb2创建两对象，参数entries1/item-size0/type literal3，**246fce**存handle；captured cells全零仅是image初值，初始queue计数/实际创建、wait消费/reset寿命未知。**shared hardware semaphore既不是instance token，也不是BS/DPB/output buffer身份；ACK、count递增、yield均不授权DMA drain、c006/d006或退休。**
- **独立分帧/实际入口可重放**：raw窗口在 **x9/out/hevc-vdec-register-callbacks-20261116/**，继承上一turn4窗register132/20、control72/6、LAT65/10、core69/9；本turn新增config55/8、source49/6、core-ACK39/6、LAT-ACK39/6、signal42/5、mapping30/4、field-config77/12、ISR-give93/10、thread-give83/6、create119/10（instructions/unresolved）。extract-entry/helper/sync-config/semaphore-create helpers、JSON/asm/log/exit、callback-strings.json与descriptor-map-data.json均保留且ignored。首轮profile把6d92 span结束误置6daa（6da8的4B指令内部），在 **hevc-vdec-irq-final-20261116/profile-exact.log/.exit=1** 被独立boundary拒绝；改到6dac，不覆盖失败证据。中间framed-final目录exit0，最终源码资料为 **x9/out/hevc-vdec-irq-source-final-20261116/**：syntax.log/.exit=0、extract-profile-exact-entries.py / profile-exact.log/.exit=0，**全部32新spans + 原clock48 / service25独立exact-PC/raw/boundary覆盖**，mem-wait63不变；vdec-irq-profile-exact.json / clock-install-profile-exact.json / service-dispatch-profile-exact.json / span-framing-exact.json。**extract-firmware-entry.py**从固定备份vcp_b.img经真实`firmware()`提取，firmware-entry.log/.exit=0 / firmware-entry-vdec-irq.json，profile_match=true/32spans，全部runtime flags=false；source-head.txt固定1f17976。重提将helper复制到新同级out目录，exclusive-create，不覆盖原件。
- **CI/下一具体stage/停止边界**：`git diff --check`通过；最终`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0 / `[]`，gh-runs.json/.stderr/.exit在source-final目录，无CI run ID。下一追 **1f1b0/1f1b4对应wait consumer、消费/reset和owner寿命**，再把真实IRQ/status与持续codec instance关联；或从正确VDEC2244ba核间接release/IRQ stop，不重复旧空standard caller扫描，也不再猜packed字段含义/把4056纸面调用当enable。cold NS/controller/firmware/clock最高authority、独立BS/FB producer排他/durable dequeue、input/DPB/output cache与逐buffer退休/c006真实free ACK、持续session_ops、DPB/layout/scratch及linear NV12/P010/HYFBC转换、HEIF mixed-derived颜色/Rext单色alpha与aux/合法properties及implicit lengths、普通VT/CMPhoto/ImageIO自动路由和LUT/HDR/tone-map/chroma-siting实效全部仍必做。没有测试脚本/套件、native编译/full kernel link/boot、构建成功产物二次检查、设备/MMIO/SMC/ADB/fastboot/mailbox/分区操作；功能树不合main，目标不缩小，不称硬解可用。

#### 2026-11-16 VDEC WaitISR 的真实 count 消费与 pending 清理（最新交接）

- **进展分类/活动状态**：上一goal turn为progress（1f179761 / 5a984b6f接通静态IRQ callback链并改变下一动作）。本turn从tracked干净的 **5a984b6f**继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，新证据代码 **153770a**；未改main、native/kernel/frontend/XNU或`.local`，没有子代理。完整目标active，未放开READY/普通VT/free，未出boot。
- **真正的消费，不再停在signal**：新定向cell/call候选引出 **246b82**，实际日志名 **2479fb `WaitISR`**。它读argument+0的instance，再读instance+460 context；argument+4是selector、+8是duration，**246b90**显式清argument+16 status。VDEC路径以基址 **1f1b0**、VENC另一候选以 **1f1b8** 定位cell，但 **246c24 / 246c4a** vendor index算术和完整selector仍未解。两条真实local call **246c2a / 246c50→246cee**：从cell读handle，**246cfa mul / 246cfe divu** 对duration先做u32 wrapping乘1000再除1000，**246d06→TCM68e2**。不能对任意输入把这两步约成恒等式、猜tick单位或当c007已成功。68e2 **6934/6936**读count+56/非零gate，**6938/693a**减1/store，候选blocking **69e4→82ee**，**69fc/69fe**重读非零回6938，实际返回序列6a1a..6a28仍有vendor epilogue/timeout gates。这是软件count消费，不证明token或buffer身份。
- **初始化reset的真实mutation**：246fb2的static-create目标现已核为 **TCM660a**，storage参数 **1f1d0 / 1f224**。660a有84B literal、**66a6→+60 capacity / 66aa→+64 item bytes**，**66ae→6592**且a1=1；6592在 **65c8 sw zero,56(s0)**，初始化wait lists+16/+36、+68/+69=255，66b0写type3到+80。这是captured初始化代码，不是live句柄/reset已发生证明，也不是DMA reset或退出。之前`initial_queue_count_proven`仍false；新queue_initialization明确只有静态zero-store证据。
- **disable IRQ后的pending清理与owner边界**：实际 **2253c4** 的format **22689e `disable VDEC %s IRQ`**；非零a0选LAT descriptor203a0/queue cell1f1b4，零选core203a3/1f1b0。**2253d4 / 225442→TCM403c→3fd2**，后者实际register R/W含vendor bit op/fence，不能复用4056的OR/AND语义或称硬件mask已生效。随后 **225402 / 22546e→6f44** 在paired helpers5864/58cc间读queue+56；只在查询count非零的标准branch之后，分别 **225410 / 22547e→68e2**，传duration=ffffffff。每个count query后只有一个显式take call，**不是6592 whole-queue reset**；查询与take是两次操作，vendor结果/完整CFG/cross-hart exclusion仍未知。2254a4尾到223886，不扩成codec stop。WaitISR后 **246c5a→243534** 处理context/selector，另有条件 **246cae** 从shared **1f1cc** 写argument+16；当轮两者用途/来源未闭合。**后续纠正见下一交接：243534是计时停止路径；已定位1f1cc writer属于VENC区域，不是VDEC LAT/core status来源。**count消费、pending清理、状态复制均不授权input/DPB/output cache handoff、DMA退休或c006/d006。
- **现有入口/精确输出**：`firmware()`实际 **service_vdec_irq_evidence** 新增19span、合计 **51span**；新增runtime consumption/reset/instance-association flags全false，其它IRQ/producer/DMA flags仍false。原clock48/service25/mem-wait63不变。新私有输入/输出在 **x9/out/hevc-vdec-wait-consumer-20261116/**：cell窗口49/2、83/9、68/12；queue-take194/13、count22/3、static-create80/13；timed68/8、IRQ-control107/9、driver-WaitISR146/12；reset40/3（instructions/unresolved，所有slice log/exit0）。两份候选scanner只记录halfword原始候选、**不是完整framing/CFG**；解释采用后续独立exact-PC窗口，旧空VDEC release standard caller类没有重跑。最终 **x9/out/hevc-vdec-wait-final-20261116/**：syntax.log/.exit=0、extract-profile-exact-entries.py / profile-exact.log/.exit=0，全部 **51+48+25span** exact-PC/raw/boundary覆盖；extract-firmware-entry.py / firmware-entry.log/.exit=0，从固定备份vcp_b.img经真实firmware入口提取51span。JSON、span-framing-exact.json、source-head.txt固定153770a。重提复制helper到新同级out目录、exclusive-create，不覆盖原件。
- **两项离线负证据，不据此补猜实现**：implicit长度参考在 **x9/out/heif-implicit-extents-20261116/**（固定libheif v1.19.8 `libheif/box.cc`、GPAC v2.4.0 `src/isomedia/meta.c`、HTTP headers、下载exit0及source-identity.json）；libheif将length0读成0/不copy，GPAC识别并拒绝抽取self-reference且不支持method2，**均不足以证明method0/1的合法implicit长度应读到source末尾**。当前heif_items ENOTSUP门保持，不按源码注释或peer的未实现路径扩功能。另 **x9/out/hevc-vendor-opcode-source-20261116/** 捕获Andes binutils master `include/opcode/riscv-opc.h`及headers/SHA/source身份（master可变、非build input）：新窄mask比较实际422c raw67360504对NDS_BEQC/BNEC、prologue0590对CM_PUSH/POP/NEXEC_IT全部不匹配，**不把这个公开variant注入RV55 decoder**，也不重复旧LLVM弱feature尝试；vendor selector仍未解。这些外部参考/原件/派生文件全部ignored，不进git，没有新增Markdown报告。
- **CI/约束/完整目标不缩小**：`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0 / `[]`，gh-runs.json/.stderr/.exit在wait-final目录，无CI run ID。所有shell/r2/gh任务已terminal；无测试脚本/套件、native编译/full link/boot、成功构建产物二次检查、设备/MMIO/SMC/ADB/fastboot/mailbox/分区操作。下一从 **243534与1f1cc writer** 追status/instance owner，而不是重做signal/count或拿WaitISR软件成功当buffer completion；cold NS/controller/firmware/clock最高authority、独立BS/FB producer排他/durable dequeue、cache与逐buffer退休/c006 ACK、持续session_ops、DPB/layout/scratch/linear NV12/P010及HYFBC真实转换、HEIF mixed-derived颜色/Rext单色alpha/aux/合法properties和implicit lengths、普通VT/CMPhoto/ImageIO自动路由及LUT/HDR/tone-map/chroma-siting实效全部仍未完成。功能树保留不合main，唯一最终boot仍等全部代码齐全。

#### 2026-11-16 WaitISR 计时/VENC status 纠偏与 VDEC 前后 WLA 控制（最新交接）

- **活动树/里程碑**：从tracked干净的`9f9d7ecd`续接 `.worktrees/hevc-hw` / `feat/hevc-hw`。第一代码里程碑 **`9b6a055`** 已提交：真实`firmware()`的`service_vdec_irq_evidence`由51扩到61 spans并纠正旧status-query命名。第二代码里程碑 **`dd530a7c`** 加入WLA定向证据，最终67-span独立提取已通过，两项代码里程碑都已push（9f9d7ecd..dd530a7c）；仅改`vcp_artifacts.py`与NOTES，没有native/kernel/frontend/XNU/main或`.local`改动、子代理、boot或设备动作，完整HEVC/HEIF目标仍active。
- **回到最早不确定点纠正用途**：**2433ac**向`32c1b0 + context.word0*48 + selector*16`起始8B record调用2463c4，并在`32c210 + context.word0*3 + selector`写flag1；**243534**向record+8取时间、**2435a6→2435d6**求差，后者真实name **244dac `u4VideoGetTimeDiff`**、literal1000000与overflow logs。非零差值在 **2435c0** 间接调用`32c0e0 + category*8`的callback，参数为context+16 word/selector/差值，随后 **2435cc**清flag。callback pair在 **242f28/242f2e**由242f00的a2+4/+0写入；最终callback身份/副作用、vendor category/selector、时间源15876和算术helper12bc8语义仍未证明。**这是静态计时begin/stop链，不是直接硬件status query，更不是DMA completion。** 原字段`post_wait_query_*`已替换为`post_wait_profiling_stop_*`，不继续追伪status来源。
- **1f1cc实际writer不属于VDEC LAT/core**：新定向field scan引出 **TCM155cc**；**155d4→10fdc** 映射参数`38020000`，**155d8**取映射结果+92，**155de**写shared`1f1cc`；另映射38020060、155f4→154b4，末尾 **1563e→14fda**传queue cell **1f1b8/a1=1**。新真实注册entry **223b0e**创建从1f1b8起5个shared句柄，**223b66→421c**传descriptor **20388=`2d 04 01`** / callback155cc / context0，再223b70→403c。与WaitISR的VENC区域cell基址1f1b8相接；**246ca6**明确只在context.word0为0时才 **246cae**把1f1cc复制到argument+16。已定位的是另一codec的shared status writer，不是VDEC1f1b0/1f1b4的buffer身份；其他writer/runtime覆盖、当前注册/硬件源和per-instance owner仍未闭，不称VENC runtime IRQ成功或VDEC退休。
- **显式enable路径纠偏，不能称pending清理最终保持masked**：raw AUIPC/JALR精算 **2238f0/223922→TCM3fb0（不是403c）**，随后 **2238f8/22392a li a1,1**、**2238fc/22392e→TCM4056**。该helper **223886** 的真实name **2267bf `mtk_enable_irq`**，log22680c `enable VDEC %s IRQ`，VDEC直接descriptor203a0/203a3；其他分支table227e9c的vendor index未知。旧initializer223a76/223a9a没有显式a1的结论仍成立，但**不能推广到所有control callers**。2253c4 pending清理的common tail2254a4就是此enable helper；完整selector/最终mask/runtime enable仍未证，不能把该tail当codec stop、producer排他或DMA drain。
- **WaitISR前后调用不是profiling/IRQ ACK**：精算 **246c14→TCMc40a / 246c3a→TCMc31c**，传`(selector-1)!=0`与context+8 word。完整窗显示history的word0前写0/后写1、word1为context+8，基址 **24df8 / 24e00**、index&7、vendor category算术仍未知；shared **24df4** 前有decrement store **c49c**，后increment **c3fa**与7fffffff saturation compare。真实source string **1a449 `drivers/RV55_A/mt6993/wla/wla_plat.c`**，logs含wla_history_idx/reload_ref_cnt和mminfra-off。两者以 **24bdc** 调160ec，尾到161ae；160ec→16052，16170→1613c且 **16168**显式清对象word0，另有per-hart计数/vendor guards与55ba恢复tail。对literal **2120a000+108** 的bit0 clear/set配合保存/恢复mstatus，vendor fence/硬件含义未解。**这是WLA history/reload控制，reference count不是pending IRQ或buffer count；短段word清零、WLA register toggle不证明BS/FB producer排他、codec持续lock、DMA drain或free。**
- **私有证据/可重放与第一里程碑结果**：新原始窗/候选/精算脚本 **`x9/out/hevc-vdec-status-owner-20261116/`**，包含旧误命名的post-wait-status-query窗口（名称仅为历史文件名，实际用途按上文纠正）、新的timing/status provenance、registration及call targets。VENC writer37/2、timestamp37/4、profiling begin32/6、callback writer66/4、status注册31/1（instructions/unresolved）；所有slice exit0。曾失败的inline非法hex没有生成JSON，后续`resolve-status-owner-calls.py`成功精算且不覆盖旧窗口。**`x9/out/hevc-vdec-profiling-final-20261116/`**的syntax/profile-exact/firmware-entry exit均0、独立 **61+48+25** spans exact-PC/raw/boundary一致，mem-wait仍63；source-head.txt=9b6a055，真实固定备份vcp_b.img经firmware入口match61 spans。新WLA窗 **`x9/out/hevc-vdec-wla-wait-20261116/`**：完整pre81/10、post73/8、paired-enter窗口70/11、paired-leave72/8，slice均exit0；后两窗口含相邻helper，profile只取各真实entry/tail窄段，不把整个窗口称单function CFG。复提复制最终helper到新同级out目录、exclusive-create，不覆盖原件。
- **最终准确执行结果/复提入口**：**`x9/out/hevc-vdec-wla-final-20261116/`** 的syntax.log/.exit=0、extract-profile-exact-entries.py / profile-exact.log/.exit=0，**全部67+48+25 spans独立exact-PC/raw/boundary一致**，clock/service/mem-wait仍48/25/63。extract-firmware-entry.py / firmware-entry.log/.exit=0，以固定原始vcp_b.img经真实`firmware()`输出match67 spans；JSON/span-framing-exact.json及 **source-head.txt=dd530a7c** 完整保留，全部runtime flags=false。`git diff --check`通过；push-code.log/.exit=0，最终`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0 / `[]`（gh-runs.json/.stderr/.exit），无CI run ID。重提可在本工作树 `mkdir x9/out/<新唯一名>`，复制本目录的两个extract-*.py到新目录，再分别用 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/out/<新唯一名>/extract-profile-exact-entries.py` 和 `.../extract-firmware-entry.py`；helpers按新同级路径定位仓库并exclusive-create输出，不能在旧目录覆盖。最窄writer窗口命令：`python3 -I x9/tools/rv32_slice.py x9/out/hevc-bs-callers-20261115/rv55-tcm.bin --base 0 --start 0x155cc --bytes 0x74 --r2 /opt/homebrew/bin/r2 --out x9/out/<新唯一名>/writer.json`。额外保存WaitISR原始labels247a8f=`VENC` / 247a94=`VDEC`（vendor string选择仍未知），captured live DT的venc@38000000/mt6993 node没有38020000 reg，**不据相邻MT6991的同地址DT授权AP映射**；captured-dt-venc-node.log与wait-isr-codec-labels.json仅为角色佐证。所有工具terminal、无后台句柄或子代理；最新代码不因NOTES-only收尾而复提或复编。
- **当前未完成/下一实际stage**：下一追VDEC真正instance/context创建及caller的selector、持续HEVC调用到WaitISR返回后VSI/DPB/output的mutation，或cold shared controller/firmware authority；**不再把243534、1f1cc、WLA counter当VDEC completion，不重跑旧空2244ba standard caller类或公开弱opcode feature**。所有runtime/owner/producer-exclusion/DMA-retirement flags仍false，READY/普通VT/free门不放开。cold NS/controller/firmware/clock最高authority、BS/FB独立producer排他与durable dequeue、input/DPB/output cache及逐buffer退休/c006真实ACK、持续session_ops、DPB/layout/scratch/linear NV12/P010/HYFBC真实转换、HEIF mixed-derived颜色/Rext单色alpha/aux/合法properties与implicit lengths、普通VT/CMPhoto/ImageIO自动入口和LUT/HDR/tone-map/chroma-siting实效全部仍必做，唯一最终boot等全部代码齐全。没有测试脚本/套件、native/frontend编译/full kernel link、新boot、成功构建产物二次检查、设备/MMIO/SMC/ADB/fastboot/mailbox/分区操作；不合main、不改.local、不缩小目标。

#### 2026-11-16 两条 WaitISR 路线的实际注册与 local caller（续接补记）

- **恢复基线**：活动树仍为 `.worktrees/hevc-hw` / `feat/hevc-hw`，HEAD/origin `13fd10a2`、tracked clean。上一turn留下的拟议 per-item 色彩 patch 整次失败，未应用源码、未编译；以下是已保存的离线新证据，不是 native provider 进展。本补记不重提旧67-span profile，不操作设备，不改 `.local` 或 main，不启动子代理。
- **注册的 service1 wait callback 是 remote 路线**：DRAM `3057d2..305a8a` 的真实初始化先在 `305812` 清 stack+24 的96B block，`305832/305842` 将 `30854a` 写 block+0（pfnWaitISR），`305836/305848` 将 `3086c2` 写 block+8（pfnLockHW）。`3058a2→242f00` 的参数为 service1/block；后者实际复制96B至 `32c0f0 + service*96`，wait cell为 `32c150`，与此前 free offset32/cell32c170一致。`30854a..3086c2` 是已知 service1/c007/40B reverse-wait sender（selector+16/id+24），**不是本地 semaphore take**。generic VAL 的真实 indirect wait consumer/完整进入分支仍未定位，不声称该 route 当前实跑或本地 wait 不可达。
- **local `246b82` 的两个直接 caller另行保留**：标准 call 候选为 `220660` / `249550`。`22063a` 从 instance/selector/duration 构造88B argument，+12=1/+16=0，wait后读instance+380、tail `220720→220020`（a1=244/a2=0）并返回 saved result；`220728 ret` 后是字符串，邻近 `VENC_GetHWRegBase` 仅支持 VENC 候选角色，不当 VDEC completion。`249526` 日志为 `HAL_CMD_SET_CMD_QUEUE: WAIT_ISR_CMD+ %d`；`249550→246b82` 后取instance+456，包含 `3602f000/3600f000` 映射参数与status bit16、reset候选 `249604→2491fc`，`24963c` 间接call对象+76（a0=对象+80/a1=selector/a2=1/a3=0）。**该窗没有完整epilogue，callback来源和dispatcher caller仍未知，status/reset/返回0都不是DMA drain**。raw pointer与LUI247000+ADDI候选为空仅限定scanner结果，不证明完整CFG/不可达。
- **可重放资料与下一阶段**：`x9/out/hevc-vdec-instance-wait-20261116/` 保留 scripts/ranges/candidates/JSON/asm/log/exit；独立分帧 registration207/13、copy-writer40/2、remote-wait124/9、local-instance133/21、local-wrapper114/10（instructions/unresolved），slice exit0。仅这些新窗口已保存，未改 `vcp_artifacts.py` 或runtime flags。复查新唯一输出：`python3 -I x9/tools/rv32_slice.py x9/out/hevc-ring-producer-20261115/rv55-dram.bin --base 0x200000 --start 0x3057d2 --bytes 0x2b8 --r2 /opt/homebrew/bin/r2 --out x9/out/<新唯一名>/registration.json`。下一追 service1 wait indirect consumer，或249526真实caller和instance+456/+76安装来源；不再把两条wait、243534计时、1f1cc VENC status或WLA混为VDEC退休。没有源码编译/CI/boot/设备任务；完整硬解余项与所有authority/producer-exclusion/buffer-retirement门继续保留。

#### 2026-11-16 HEIF 显式 per-item 色彩归一化（最新交接）

- **实际代码/交付**：同一活动树，从 `13fd10a2` 实际改四个frontend文件，源码 **`4a0de39d`** 已push `13fd10a2..4a0de39d` 到 `origin/feat/hevc-hw`（新构建目录push-code.log/.exit=0）。上一turn失败patch未计为实现，本turn已完整落地；未改main/kernel/XNU/共享`.local`，未启子代理。完整HEVC/HEIF goal仍active，READY/producer-exclusion/buffer-retirement/普通VT gate均不放开，无boot。
- **真实调用而非未用API**：新增export `X9HEIFCopyHardwareImageWithColorOptions(file,id,workingICC,intermediateICC,sourcePolicy,graphColorPolicy,clapPolicy,budget,...)` 与 enum `INHERIT_IDENTICAL=1 / PER_ITEM=2`；CLI实际调用它。旧WithIntermediateICC委托新实现并固定INHERIT_IDENTICAL，旧WithOptions/exact ABI及原256MiB预算不变。默认仍允许缺child colr继承相同祖先解释、冲突/不同reuse context拒绝；没有悄悄放宽旧调用者。
- **PER_ITEM的精确范围**：这是**caller显式渲染选择，不是HEIF规范级derived inheritance或普通ImageIO/CMPhoto行为证明**。每个coded item仅用自身colr与实际PPS→SPS VUI解析source，祖先colr不填入leaf；共享item的解释因此不随祖先改变。每leaf继续真实DecoderID/UsingHardware硬解→独立YUV view的matrix/range/chroma→实际VTPixelTransfer到同一个workingICC；需fractional resampling时直接转到实际派生linearICC/RGBA-float、已有linear compositor合成、最后一次转原workingICC/RGBA-half。不同source ICC/CICP可在显式模式下归一化，不在item-only cache里偷换context，也不把不同RGB原样copy后贴一张ICC。**Derived colr只保留声明metadata，不作用于已归一化pixels，最终ICC仍由caller选定**；自身+SPS仍不明的字段首硬解前拒绝，绝不回退祖先/默认sRGB/另一graph策略。aux/Rext gate、finite/opaque alpha、实际transfer返回ICC核验、输入/ICC真实owned copies、budget和失败释放保持。
- **入口/metadata可重放**：CLI（本turn**未执行**）可用 `vdecctl decode-heif file.heic 0 working-rgb.icc icc --per-item-colors NEW.rgba16f NEW.info.plist`；组合fractional/LUT时 `... final-lut-rgb.icc icc linear-ceil 1024 --linear-intermediate matrix-rgb.icc --per-item-colors NEW.rgba16f NEW.info.plist`。可选clap/budget仍在flags之前，两个flags可互换，duplicate/unknown/missing value/exact+intermediate在读文件/硬解前拒绝；以`--`开头的ICC文件名需用`./`避免被当flag。原O_EXCL/0600/fsync/输出失败只删本次文件仍保留。info新增 `GraphColorPolicy / GraphColorInterpretation`，各node新增 `ColorInterpretation`，原ordered raw properties/EffectiveColr/ResolvedSourceColor/VUI/working与实际linearICC继续保留。manifest只新增explicit_per_item_color_normalization=true与default/interpretation；**mixed_derived_color_contexts / per_item_color_runtime_verified / complete_image_compositor / physical_provider / hardware_tested / automatic_injection仍false**。
- **确切编译/源码审查**：`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/heif-per-item-colors-20261116/frontend` 编译、链接dylib/CLI、ad-hoc签名 **exit0 / -Wall -Wextra -Werror无诊断**，frontend.log/.exit、source.diff、**source-head.txt=4a0de39d**在同目录。只分析本次改动的heif_native.c/vdecctl.c，`x9/out/heif-per-item-colors-static-20261116/analyze-source.py` 用相同iOS16 arm64/SDK/`--analyze -Wall -Wextra -Werror -Xanalyzer -analyzer-output=text`，analyzer.log/.exit **exit0/无diagnostic**，不压制analyzer、不重析未改compositor。人工diff核新旧策略、shared-cache/context、所有preflight/失败释放、ICC目标隔离和CLI边界；`git diff --check`通过。未运行测试脚本/套件/人工向量、CLI/sample/像素验证；无成功构建后二次解包/hash/反汇编/重建比较。
- **CI/状态/下一具体工作**：本turn `gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，gh-runs.json/.stderr/.exit在新frontend目录，无CI run ID；frontend/analyzer/gh/代码push已terminal。未编kernel/full link/XNU树、未操作设备/ADB/fastboot/mailbox/MMIO/SMC/分区，没有后台构建/设备句柄；仅本NOTES作交接，不新增报告。下一优先从上一小节的真实service1 callback block追**pfnWaitISR indirect consumer**，或249526 caller与instance+456/+76安装来源，严格区分c007 remote与本地IRQ wait，不能将其返回值当buffer退休。cold NS/controller/firmware/clock最高authority、BS/FB独立producer排他/durable dequeue、input/DPB/output cache/逐buffer退休/c006真实ACK、持续session_ops、DPB/layout/scratch及linear NV12/P010/HYFBC实际转换仍全部必做。HEIF规范级mixed-derived颜色、Rext单色alpha/aux、合法properties/implicit lengths、普通VT/CMPhoto/ImageIO自动入口和LUT/HDR/tone-map/chroma-siting实际效果也仍未完成；explicit per-item代码不替代这些验收。唯一最终boot继续等全部代码齐全；不合main、不缩小目标。

#### 2026-11-16 H265 本地 WaitISR → HEVC VLD 标量读取（当前交接）

- **恢复/实改**：同一活动树 `feat/hevc-hw`，恢复 `a70c8110` 的单文件草稿并人工审核；证据提取器源码提交 **`db74fff`**。本批是定向原件分析与实际 `firmware()` 输出路径的变化，不是硬解生产能力或物理验收。没有改 frontend/main/共享`.local`，没有子代理或设备操作，完整目标保持active。
- **区分真实路线**：注册的service1 pfnWaitISR仍是 `3058a2→242f00` / callback `30854a` / table cell `32c150` / c007远端sender；其offset0 indirect consumer、运行时注册与覆盖排他未闭。新增独立完整H265 HAL窗 `2ffcee..2ffda6`（66 instructions/6 unresolved），name `3013e8 H265_HAL_WaitISR`；context+`2ecc` word减48/seqz作selector，context+24的instance经 `2ffd84→249526→246b82` 本地共享wait。literal200/50之后的vendor算术、flags和result branch仍未解，**不称固定200ms、总返回1或completion**。本地helper补到 `249736 ret`（170/13），保存并返回s2；instance+456 follow-up、status bit16、reset候选与对象+76 callback都不是DMA drain。
- **post-wait实际mutation及纠正**：H265 caller前窗 `2d04e8..2d05e2`（85/7）经 `2d0542→2ffcee`，zero返回打印后仍join，再 `2d0592→2ffc0e`。后者从context+28 handle和+`2ed0` byte发 `2ffc26→254c28`，literal **75是HEVC VLD register word index，不是completion命令ID**；`254c3e`左移2，原name `2566b3 vdec_hal_read_hevc_vld`、log `2566ca reg_base.hevc_vld[hw_id]`。NULL handle fallback `ffff`，raw scalar low/high16经 `2ffc36/2ffc38`写输出，wrapper literal1不是read success。真实scalar read `254806`取object+36 base、+1332 callback，在 `254814`间接call；安装target、映射authority和word75语义尚未证，不能当buffer token/退休凭据。
- **精确资料/复提**：新窗口与候选脚本全部在 `x9/out/hevc-wait-indirect-consumer-20261116/`；只读 `rv32_slice.py` 用原DRAM base`200000`。最终结果 **`x9/out/hevc-h265-local-wait-final2-20261116/`** 的Python源码syntax、`extract-profile-exact-entries.py` 与 `extract-firmware-entry.py`均exit0：IRQ **85** / clock **48** / service **25** spans逐PC/raw/boundary匹配，mem-wait **63**不变，真实固定`vcp_b.img`经 `firmware()`输出IRQ match=true/85。新增两个runtime route flags及所有authority/producer-exclusion/DMA-retirement flags均false。source.diff与source-head.txt记录最终源码；旧83-span目录只是中间版本。复提将两extract helper复制到新的同级out目录再运行，不覆盖原结果；原件不改、不复检旧成功编译产物。
- **状态/下一动作**：`git diff --check`通过；本批没有kernel/frontend/full link/boot编译、测试脚本/套件/人工向量或上机。`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，日志在final2的gh-runs.*，无CI run ID或活动构建句柄。表类scanner没有找到remote wait consumer不证明不可达，offset72 `eVideoPutBuf`也不是offset0 wait；不重复无新线索的同类scan。下一优先追 **H265 context+24/+28的安装writer/owner及HAL object+1332读取callback安装来源**，再解释word75并连接真正VSI/DPB/output mutation。cold NS/controller/firmware/clock最高authority、BS/FB独立producer排他/durable dequeue、input/DPB/output cache/逐buffer退休/c006真实ACK、持续session_ops、DPB/layout/scratch和linear NV12/P010/HYFBC转换，以及规范级HEIF mixed-derived颜色、Rext alpha/aux、合法properties/implicit lengths、普通VT/CMPhoto/ImageIO入口及色彩实效均未完成；READY与普通VT门不放开，不合main，不出提前boot。

#### 2026-11-16 H265 context writer 与 common-HAL 标量callback安装（上一交接）

- **本turn实质进展/提交**：恢复批证据源码`db74fff3`和交接`44706a11`已push成功（`a70c8110..44706a11`，ls-remote同SHA；push日志在上一final2）。随后不是重复wait/table scan，而是依据新`+1332`读取槽追真实writer，再依据`+372`来源追wrapper，新增证据源码 **`adc89b44`**。活动树仍`.worktrees/hevc-hw`/`feat/hevc-hw`；本turn属于有新定向证据/提取器代码进展，不称硬解生产链完成。未修改main/frontend/kernel/共享`.local`、未启动子代理。
- **context+24的实际静态writer链**：`2cd65c` wrapper保存真实参数`a1→s6`，`2cd6cc`将其写driver context+372，`2cd6ea→2d2810`；实际H265 constructor保存driver input到s2，从`2d28d2→2cc9f0`结果得到s11 HAL context，`2d2916/2d291e`写outer+1296/HAL+784关联。`2d2acc`读取driver+372，`2d2ad6`写HAL+24，后者就是已闭本地WaitISR取instance的字段；**wrapper的a1上游producer、真正硬件owner、完整vendor分支与runtime安装仍未闭**。captured `3356b0` ops表**offset32**指向`2cd65c`，不是offset0；表静态值不证明selected ops或当前调用。
- **context+28与+1332实际安装**：同一constructor `2d2af2→300806`，后者 `30080a→2283c6`取固定global地址 **`329938`**，`300812`写HAL context+28，再返回literal1（不检查global已初始化）。`227eb0`清1360B对象；`2282a0`构造`2282e6`，`2282b4`存对象+1332。独立target **`2282e6 lw a0,0(a0); 2282e8 ret`**，故先前`254814`间接read有捕获的plain word-load安装路径，不再将静态target写成未知；实际runtime target/覆盖排他仍false。这是固定common-HAL对象，不是per-buffer completion context。
- **基址/最高层仍不授权**：installer把`36000000`存对象+56，参数a0=`36000000`/a1=0/a2=`40000`/a3=8经`227ee0→246908`，返回存对象+36。原log精确为`2283d0 io_base_pa 0x%x` / `2283e1 io_base_va 0x%lx`；wrapper `246912/246916→TCM10fdc`链接既有AP-address converter。该helper的`10ff2` table branch和`1101a`仍unknown，捕获candidate row `1c200=[30000000,0,20000000,0]`及low27 merge只限定静态转换，**不是AP可用MMIO映射/NS权限**。对象+400在`227fe8`写literal`28000`，HEVC VLD selected-hw算术和word75意义仍未解，不计算/使用猜测MMIO。新global-init前窗`316948..316974`显示`31696a→223a34` shared IRQ init后`316970→227eb0`，完整最高层reachability/初始化顺序与authority仍未闭。
- **原件/最终结果/确切执行**：新原件分帧、候选scripts/JSON/asm在 **`x9/out/hevc-hal-object-install-20261116/`**；global installer291/4、reader2/0、getter3/0、binder8/2、global caller15/2、H265 constructor前窗228/9、map wrapper29/5、TCM converter30/4、create wrapper前窗52/3（instructions/unresolved）。constructor首个slice请求因末尾落在指令中被工具拒绝，未产出伪帧；改用已确认`2d2af6`边界成功，所有前窗都不称完整CFG。标准store372/1332和getter/binder call scans仅候选，不当完整writer集合或不可达证；`224f12`是另一路map-result字段，不能因同offset混成H265 driver对象。最终 **`x9/out/hevc-hal-object-install-final2-20261116/`** syntax/exact-profile/真实固定`vcp_b.img`的`firmware()`提取均exit0：IRQ **101** / clock **48** / service **25** spans逐PC/raw/boundary匹配，mem-wait **63**不变；新增三个runtime安装/target flags及全部authority/producer-exclusion/DMA-retirement flags仍false。99-span的前一final目录只是中间版；最终source.diff/source-head.txt=`adc89b44`保留。
- **可重放/状态/下一动作**：用原DRAM执行`python3 -I x9/tools/rv32_slice.py x9/out/hevc-ring-producer-20261115/rv55-dram.bin --base 0x200000 --start 0x227eb0 --bytes 0x41e --r2 /opt/homebrew/bin/r2 --out x9/out/<新唯一名>/installer.json`；复提将最终两extract helper复制到新同级out目录再运行，不覆盖原件或旧结果。`git diff --check`通过，`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，final2有gh日志，无CI run ID。本turn没有C/frontend/kernel/full link/boot构建、测试脚本/套件/人工向量、成功编译产物复检或设备/MMIO/SMC操作；命令全部terminal，无后台句柄。下一沿 **ops+32实际indirect consumer和`2cd65c` a1上游instance producer/owner**，或追instance+456/+76 callback安装，再往H265 post-wait真实DPB/VSI/output mutation；不重扫已排除table/wait类。所有上一条完整硬解/HEIF余项继续必做，READY/普通VT门关闭、目标active、不合main，唯一最终boot仍等全部代码齐全。

#### 2026-11-16 embedded instance / LCU 状态与 Free/Display FB → ring 候选链（最新交接）

- **基线/代码里程碑**：继续 `.worktrees/hevc-hw` / `feat/hevc-hw`，恢复已push的 `e72fe282` 基线及上一上下文唯一未提交的提取器草稿；人工审核后加入新的 FB producer/consumer 链，源码提交 **`0365ab4`** 已push `e72fe282..0365ab4a`，ls-remote同完整SHA（final2的push-code.log/.exit=0及remote-code-head.txt）。只改 `x9/tools/vcp_artifacts.py` 和本交接，未改 main/frontend/kernel/XNU/共享 `.local`、未启子代理。本轮有新的定向原件证据与实际 `firmware()` 输出变化，但**没有新增原生硬解能力或物理验收**，完整目标保持 active，唯一最终 boot 仍等全部代码齐全。
- **真实 create 参数/embedded owner（观察）**：generic `26ff2c` 在 `27006e/270080` 使用 owner+48 = ops+32 的候选 `2cd65c`，a0 是 owner+80 **字段地址**、a1 是读取 owner+108 的 **instance pointer**，其余 a2=owner+116内容/a3=codec word/a4=stack+4/a5=0。`2700d2/2700d8` 安装 owner+160 embedded instance 到 owner+108，`2700e2→224b78` 把 a2/a3 写 instance+448/+452；候选成功分支 `2700ec/2700f2` 写 instance+456=owner/+460=owner+116内容，`2700fc` 写 owner byte112=1。完整 mapping 初始化、selected ops、word88语义和运行时 READY 未闭；legacy `27009e/2700b0` 的 ops+0 不与 ops+32 混同。
- **wait follow-up 重要纠正（观察）**：`24962c` 读 instance+456，`249630` 取 owner+76 = ops+60，**`249634 lw a0,80(a0)` 传入字段里的指针值，不是 owner+80 地址**。captured H265 table `3356b0+60` = `2cdb2c`；完整callback窗 `2cdb2c..2cdb94` 34/3，nonnull时保存1，`2cdb34 bnez a2,2cdb8e`。当前wait调用a2=1，因此该静态路径直接返回1，跳过只有a2=0才调用的 `2d4662`。这不是 DMA drain/fence，runtime target/覆盖排他仍false。
- **word75真实后续用途（观察）**：`2d05fc/2d0608` 把 stack+184/+180 的 low/high16 与 HAL+1444/+1448 减1比较，mismatch→`2d0696`，matched `2d060c` 清 byte85；`2d0792/2d079e` 再比较，`2d07a2` 清85，`2d07b0` 写1的前置vendor branch `2d07ac` 未解。原日志 `2d5208/2d5285` 明确 LCU(x,y)，matched `2d539a` 为 Decode OK，因此该scalar用于 **LCU坐标，不是buffer token**。selected-SPS bounds writer、完整CFG仍缺，坐标匹配/日志OK/清85不授予退休。
- **Free/Display列表生产者与清槽consumer（新观察）**：post-wait主窗实际 `2d0ac2→2ce0c2 H265_PutFrameBufferToDisp`，随后 `2d0acc→2cdfba H265_PutFrameBufferToFree`，另两处display call `2d0c9a/2d0e86`。Free pointer array offset **12e4**、stride16、literal64，`2ce07c/2ce07e` 写pointer/前一word=4；Display offset **16e4**、stride16、literal20，`2ce17a/2ce198` 写pointer、`2ce17c/2ce194` 写前一word=2，还写两个ordering metadata word但完整角色未闭。consumer **`2d3956`** 选display pointer后 `2d3a58/2d3a5c` 清对应16e0/16e4；**`2d3a94`** 从同12e4读取后 `2d3b00/2d3b04` 清前word/pointer。两者通过 **`2cc990`** 读取input object+368，校验resolved object+0等于a1后返回pointer；这是普通指针identity检查，不是NS mapping authority。64/20只记录软件search literal，不称硬件buffer容量；vendor循环、锁语义、运行时context身份/producer排他都未闭。
- **列表 → ring只闭静态候选路线，不闭退休事务**：`3097da` 使用selector2、`30995e` selector3调用 `27047c`。其中 `270574` 从 owner+28 (ops+12) call captured **`2cd362→2d3956`**，`27059e` 从owner+32 (ops+16) call captured **`2cd418→2d3a94`**；generic vendor selector taken edges未证明。result分别存owner+728/+732，并调用独立owner-list helper `26f85c/26f8bc`（不混成HAL stride16列表）。caller拿到frame pointer，`309810/3099a0→309dd4` 先转AP token，再 `309876/3099ec→30a06e` 发布24B shared FB ring；**frame pointer不是AP token**。converter可返回(-1,-1)，vendor算术/完整拒绝分支未闭，不能擅自计算token或以nonnull当合法。软件清槽和shared publication是分开的写入，没有durable dequeue/publication receipt/cache/逐buffer DMA退出证据，既有BS/FB消费者和退休门不放开。
- **原件/最终确切结果**：所有新材料在 **`x9/out/hevc-create-instance-input-20261116/`**；producer独立分帧Free85/12、Display98/10；wrapper各57/3；consumer Display114/6、Free51/4；generic selector189/19；两个query→ring窗79/5与61/5；converter46/7；publisher63/7；resolver34/2（instructions/unresolved）。复用旧 `hevc-codec-destructor-20261116/create-path.json`，不重解旧窗。main `2d08ec..2d0ed8` 482/29是续窗，`2d0ee4 ret`后还有jump chunks，**不是完整CFG/函数extent**；preview碰到内嵌strings只保留为候选，不当代码证据。最终 **`x9/out/hevc-fb-list-route-final2-20261116/`** source-head=`0365ab4`，源码syntax、exact-profile及真实固定 `vcp_b.img` 的 `firmware()` 提取 **均exit0**：IRQ **142**（新增41）/clock48/service25 spans逐exact-PC/raw/boundary匹配，mem-wait63不变，实际firmware() IRQ match=true/142。四个新增runtime route/state flags及全部authority/READY/producer-exclusion/DMA-retirement flags仍false。前一 `hevc-fb-list-route-final-20261116/` 的140-span输出只是resolver加入前中间版，最终以final2为准。
- **重放/CI/下一项**：复提将final2两个extract helper复制到**新同级out目录**再运行，exclusive-create，不覆盖旧件；其frame目录列表已加入旧codec-destructor与本轮create-instance-input。重切free producer：`python3 -I x9/tools/rv32_slice.py x9/out/hevc-ring-producer-20261115/rv55-dram.bin --base 0x200000 --start 0x2cdfba --bytes 0x108 --r2 /opt/homebrew/bin/r2 --out x9/out/<新唯一名>/free.json`；Display相应start2ce0c2/bytes12a。`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，final2有gh日志，无CI run ID。无测试脚本/套件/人工向量、C/frontend/kernel/full-link/boot构建、成功产物二次检查或设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作，全部工具terminal。下一沿**已定位真实列表caller的vendor失败/满ring分支、AP token转换与所有publisher存活/排他**，再补durable dequeue/cache/每buffer退休；不再把ops60快返当fence、不重复已闭context安装/word75查询。cold NS/controller/firmware/clock最高authority、BS排他/c006实际ACK、持续session_ops、真正DPB/layout/scratch与linear NV12/P010/HYFBC转换、规范级HEIF颜色/alpha/合法properties/implicit lengths、普通VT/CMPhoto/ImageIO入口和色彩实效仍全部必做；本树不合main。

#### 2026-11-16 FB publication 后的 descriptor 清理与 firmware cache 返回值边界（最新交接）

- **基线/实际交付**：从已push的 `d1cb67ea` 接上轮草稿，审核并补齐 `vcp_artifacts.py` 新 `rv55_fb_transfer_evidence()`，真实 `firmware()` 接入 `service_fb_transfer_evidence`。证据源码 **`656dfcb4`** 已提交并push（d1cb67ea..656dfcb4，最终目录push-code.log/.exit=0）。本轮为progress：新闭合publication→descriptor清理→firmware cache调用/返回覆盖静态链，**没有新增native DMA退休/free ACK或物理硬解成功**。同一 `.worktrees/hevc-hw` / `feat/hevc-hw`，不改main、kernel/frontend/XNU/固定`.local`，未启动子代理；完整目标active，READY/authority/producer-exclusion/buffer-retirement/普通VT门不放开，不合main、不出boot。
- **publisher不是容量回执**：`30a096`以(next_write&63)==read走full候选arm，`30a09a` vendor branch仍未知；可见full log `30c73b` 后 `30a0c4→30a13c`返回join。不能从wrapper返回推导已发布或durable dequeue。Free caller **3099ec→30a06e** 后 **3099ee**立即读frame+424，frame+424/+408均非零才 **3099fa→246b2c**、**3099fe/309a02**清这两word，**没有检查publisher返回值**。Display caller **309876** 后 **309878**读stack saved pointer、**30987a**读frame+424，同样不检查返回；其unmap call309886、clear30988a/30988e。converter **309e40/309e42**赋(-1,-1)、309e44跳return join；display **309818**与free **3099a8**只是OR后zero gate，本身不拒绝(-1,-1)。**后续vendor拒绝路径仍未闭，不声称stock必然发布非法token或所有满环分支必然丢帧。**
- **descriptor bookkeeping不是物理free**：Free后续搜索literal1152、stride464；AP token pair table literal512、stride8。**309a20** vendor loop未解，selected index/全循环仍false；两个subrange候选迭代offset0/32读member+64 pointer/+80 bytes，**309ae6→246b2c**。随后 **309afe/309b02**清464B descriptor、**309b0c/309b10**清AP token pair，vendor follow-up309b20/309b30下有309b38→26fd60、309b40→23c078。只确认这些软件写入，不能当作cache ownership、DMA drain、c006/free请求或d006 ACK。
- **MemUnMAP实际丢弃cache结果**：独立 **246b2c..246b6e**保存input a1→s0/a0→s1，246b36与246b4a调用firmware→AP converter；**246b42→246554**，但 **246b44 mv a0,s1**立即覆盖flush result。后续仅地址转换/log **24799a `[eVideoMemUnMAP] + uP_addr = 0x%llx, AP_addr = 0x%llx Size = %d\n`**，**246b68 li a0,0 / 246b6c ret**。这个0不是cache成功、physical free ACK或DMA退休，不能据名称把helper误认成c006 sender。
- **真正cache leaf只闭firmware-local边界**：wrapper **246554**真实name **24750e `eVideoFlushBufRange`**，保存a0/a1→s3/s2、a2→s6/a4→s5，stack+56/+48分别写range address/bytes，**2465de→246438**、2465e0保存结果s4、2466c4返回s4；另2466c2可赋literal2，vendor入口完整分支仍未知。leaf **246438**真实name **2474d2 `eVideoFlushCache`**，246440/246442读取struct+16/+8，range rounding literal128B（不是已证AP cache-line size）。candidate lock object **331af4**、lock246466/unlock246520；firmware-local register **212f3000** offsets96/100/104、command literals115/51与alternate99/35。raw **0f400500 / 0fc00500**仍unknown，全部10个encoded-operation PC均纳入raw evidence spans，不能按literal映射完整cache语义，更不授予XNU mapping/cache-handoff或跨hart/DMA producer exclusion。没有重试已排除的Andes weak opcode/LLVM feature类或执行MMIO。
- **原件/独立分帧**：新续窗/labels/preview在 **`x9/out/hevc-fb-transfer-boundary-20261116/`**：transfer3096c4..309c18 **456/30**，MemUnMAP **24/2**，cache wrapper **131/8**、leaf **84/15**（instructions/unresolved）。transfer是到adjacent wrapper的连续窗，**不是完整CFG/函数extent**；preview不作为边界证明。converter/publisher/上一instance窗直接复用 **`hevc-create-instance-input-20261116/`**，不重解原件或覆盖旧输出。原始DRAM仍2057906B/SHA200294bc…、TCM184320B/SHAf59d646b…，private输入不入git。
- **最终准确执行/重放**：**`x9/out/hevc-fb-transfer-final-20261116/`** source-head.txt=656dfcb4；syntax.log/.exit **0**，extract-profile-exact-entries.py / profile-exact.log/.exit **0**：新FB-transfer **29**、IRQ **142**、clock **48**、service **25** spans全部独立exact-PC/raw/boundary覆盖，mem-wait **63**未变。extract-firmware-entry.py / firmware-entry.log/.exit **0**，实读固定 `.local/x9/backups/partitions/firmware-originals/vcp-real-android-active-b/vcp_b.img` 经真实firmware()的新29与旧142字段均match=true、与独立提取JSON完全一致；**所有runtime flags=false**。产物fb-transfer-profile-exact.json、其余profile/span-framing-exact.json、firmware-entry-fb-transfer-and-vdec-irq.json完整保存。复提：新建本树 `x9/out/<新唯一名>`，复制最终两个extract-*.py过去，依次用 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/out/<新唯一名>/extract-profile-exact-entries.py` 和 `.../extract-firmware-entry.py`；helpers按同级路径定位仓库、exclusive-create，不能覆盖旧件。重切cache leaf用rv32_slice.py、同原DRAM/base200000、start246438/bytes11c、r2绝对路径，out须新文件。
- **CI/约束/下一具体stage**：git diff --check通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**（最终目录gh-runs.json/.stderr/.exit），无CI run ID。全部工具terminal，无后台构建/设备/代理；没有测试脚本/套件/人工向量、native/frontend编译/full-link/boot、成功产物二次解包/hash/反汇编/复建比较、ADB/fastboot/mailbox/MMIO/SMC/分区操作。下一从已确认transfer/converter内 **246e46/246ee0 paired helpers及其对象来源**收束软件临界区，核ring publication是否在同owner范围；firmware cache vendor语义与AP cache ownership仍须独立证据，不能拿MemUnMAP0或软件清零开放退休。BS/FB所有publisher存活/排他与durable dequeue、逐buffer cache/退休/c006实际ACK、持续session_ops、cold NS/controller/firmware/clock最高authority、DPB/layout/scratch/真实linear NV12/P010/HYFBC转换、规范级HEIF颜色/alpha/合法properties/implicit lengths、普通VT/CMPhoto/ImageIO及色彩实效仍必做；唯一最终boot继续等全部代码齐全。

#### 2026-11-16 FB 的两层 firmware mutex 与 AP ring 权限分离（最新交接）

- **新里程碑/代码**：从已push且干净的 `ae8d8e75` 接上一小节，源码 **`a00bbb64`** 已提交/push（ae8d8e75..a00bbb64，最终目录push-code.log/.exit=0）。同树/分支；本轮progress是新闭合mutex wrapper目标、软件锁对象与publication前后顺序，**不是native live dequeue/逐buffer退休实现**。只改vcp_artifacts.py/NOTES，未改main/kernel/frontend/XNU/固定`.local`，未启动代理；完整goal active，READY/authority/producer-exclusion/retirement/普通VT gate全保留，无boot。
- **真实wrapper是queue-backed mutex，不是AP ring lock**：新独立窗246e46..246ee0 **50/4**、246ee0..246f4c **36/3**。两者先从argument+0加载handle。raw AUIPC/JALR精算 **246e56/246ea2→TCM68e2**（try duration0 / blocking ffffffff），**246ef2→6788**（a1/a2/a3=0）；日志 **247c00/247c73/247c92/247ce6**明确Mutex lock/give名称，缺handle arm赋2，失败join赋6，vendor结果分支未解。新create246d94..246df0 **32/2**：a0=1、**246da0→6760**，246da6写handle；destroy246df0..246e46 **30/2**：**246e20→6f6e**后246e26清handle。TCM6760..6788 **17/2**：6768→66c2（count1/item-size0/a2=input），nonnull才清对象0/4/12并6780→6788 initial give。TCM仅private独立窗口，DRAM profile只证明encoded call目标，不借此宣称跨端可用mutex、当前handle或DMA drain；旧6788给队列/68e2取队列窗直接复用，没有重解。
- **两层scope与unchecked lock返回**：transfer外层 **owner+6528**，3097ae把参数存stack+16，**3097b0→246e46**，随后visible参数准备不检查返回；**309b68**reload saved pointer、**309b6a→246ee0**。inner token/descriptor表锁是 **owner+6536**：converter **309dea** lock后 **309dee**直接覆盖a0，失败309e16/匹配309e4c unlock；匹配分支在unlock后 **309e50/309e56/309e58**才读AP token pair。Free ring publication3099ec已先发生，**309a0a**才重新lock inner、**309a0e**覆盖返回；失败309a32/清464B与token后309b16 unlock。**inner mutex没有包住visible ring publication，也未证明unlock后的table读有完整lifetime保护；外层mutex的全路径/全部publisher与runtime ownership仍未闭。**不能以lock名称、wrapper0或owner指针取得AP跨端read/count RMW权限。owner conditional destroy **306d62/306d72/306d82**分别处理+6528/+6532/+6536，只证明这三个成员的可见software cleanup，不当全部publisher停机或DMA退出。
- **caller/constructor候选边界**：新独立 **30b08c..30b0ac 13/3**，**30b09c→3096c4**传a0=s2/a1=stack+68/a2=0，byte条件gate在30b094；更早owner来源和所有indirect caller仍false。半字扫描create31/destroy12/transfer1仅作candidate，不能升级为完整call graph；ctor stack+60/72/76在大dispatcher内多次复用，30765a/3078c2/307a54/307b2a等preview **没有闭到308028/30802e/308034的所有CFG/栈活跃期**，不把它们宣称为三handle的确定安装来源。新材料 **`x9/out/hevc-fb-lock-scope-20261116/`** 保存原件派生JSON/asm、exact call目标、wrapper labels、scanners/候选和preview；preview/半字候选不进profile语义。不重复此前已排除的ISA feature/弱mask类别。
- **最终确切核对/重放**：**`x9/out/hevc-fb-lock-final-20261116/`** source-head=a00bbb64；syntax.log/.exit **0**；extract-profile-exact-entries.py / profile-exact.log/.exit **0**，FB-transfer新加16、最终 **45** spans，IRQ142/clock48/service25逐独立exact-PC/raw/boundary一致，mem-wait63未变；另5个mutex AUIPC/JALR目标按独立frames精算全部一致。extract-firmware-entry.py / firmware-entry.log/.exit **0**，固定原始vcp_b.img经真实firmware()的FB45/IRQ142与独立profile完全相同，全部runtime flags=false（新runtime_firmware_mutex_ownership_proven也false）。最终profile/span-framing/firmware-entry JSON及source.diff保存。复提按上一小节步骤复制**本最新final目录**两个extract helper到新同级out，依次运行；新frame目录已加入lock-scope，expected FB45，不覆盖旧29-span结果或成功构建产物。重切mutex lock：rv32_slice.py 同原DRAM/base200000、start246e46/bytes9a，r2绝对路径、out新文件。
- **CI/收尾/下一准确stage**：git diff --check通过；gh run list --branch feat/hevc-hw --limit3 --json databaseId,status,conclusion,headSha **exit0/`[]`**（最终目录gh-runs.*），无CI run ID。全部工具terminal，无后台任务；没有测试脚本/套件/人工向量、native/frontend/full-link/boot构建、成功产物二次检查、ADB/fastboot/mailbox/MMIO/SMC/设备或分区动作。下一围绕 **30b09c 的真实owner/dispatcher路线与308028/30802e/308034 constructor stack活跃期**闭外层mutex存活、全部publisher与scope；不能重做已闭的246e46/246ee0或把inner/outer锁猜成AP协议。仍须真实AP cache ownership/可恢复producer exclusion与durable dequeue、cache/逐buffer退休/c006 ACK、持续session provider、cold最高authority、DPB/layout/scratch与linear NV12/P010/HYFBC转换，以及完整HEIF alpha/规范属性/自动VT-CMPhoto-ImageIO与色彩实效。功能树保留不合main，唯一最终boot仍等全部代码齐全。

#### 2026-11-16 FB operation 调用参数与 mutex constructor 候选栈绑定（最新交接）

- **活动基线/实际源码**：接已push的 `992fdf3f` 与上一turn留下的 `vcp_artifacts.py` 草稿，继续 `.worktrees/hevc-hw` / `feat/hevc-hw`。审查修正后的证据源码 **`4f32991`** 已正常push成功（992fdf3f..4f32991f，最终目录push-code.log/.exit=0）；本里程碑闭合新的独立指令边界、direct targets与参数/成员偏移，不是native live dequeue或硬解验收。未改main/kernel/frontend/XNU/共享`.local`或启动子代理；完整goal active，READY/authority/producer-exclusion/retirement/普通VT gate全部保留，无boot。
- **真实operation窗口与四个caller**：独立 **`30a432..30b0ac` 1017/72**（instructions/unresolved），输入a0→s2、a1→s7、a2→s3，**30a448**保存a3到stack68。owner+**5008**是codec pointer，owner+**6540**是descriptor-pool pointer；**30a46c→27047c**传selector4。尾部 **30aee6/30aee8**赋s4=**4097**，**30b094**检查该owner byte，**30b096** reload stack68，**30b09c→3096c4**传a0=s2/a2=0。四个direct calls **307964/30812c/308392/30b71a→30a432**均独立核对；前者在可见inner-table unlock后，第二/第四传mode3，第三位于profiling marker之间，marker不当锁/ACK。wrapper **30b694..30b728 53/6** 以selector17调用 **30b6de→2706bc**，**30b716**从owner+**4956**取operation a3，最后返回先前保存的result；不推导operation成功或runtime owner身份。
- **BS同窗口可见路线/不提升语义**：**30af8c→27047c**传selector20，**30afaa→30b310**转换token，**30afb6→30b264**发布BS，**30afbc→30b398** follow-up。这些是已分帧的static calls，不当DMA完成、逐buffer退休或跨端count RMW许可。operation窗口连续到ret，但vendor edges/indirect callers/完整寄存器寿命和所有publisher仍未闭；没有以“未见direct decode call”声称该operation不可能触发decode。
- **constructor新候选不是完整reaching-definitions**：复用旧独立 `hevc-c006-chain-framed-20261116/instance-dispatch` prefix **305c4c..3073e6**，新增suffix **3073e6..308068 1015/71**。局部标准指令确定 **307136**形成s10+6528、**3071d4**存stack60；**30716c/30716e**形成s10+6532并存stack76；**3071a2/3071a4**形成s10+6536并存stack72，前面分别conditional destroy旧handle。**3071ee**只证明标准调用清owner+4944起1688B，不赋runtime cleanup含义。后段 **307faa→26ff2c**，**307fb0** zero branch到307fe6，failure **307fe2→307352**；可见zero-result路线 **308022→23ba08**后 **308026/30802c/308032** reload stack60/76/72，**308028/30802e/308034→246d94**。但 **3072de→2466d0** 后的 **3072e2** vendor branch未解，大dispatcher还复用这些stack slots，绑定到create的全CFG/活跃期未证，`constructor_callers_and_full_owner_lifetime_proven`与两个新runtime owner/constructor flags仍false。
- **原始/最终可复制资料与失败保留**：新独立JSON/asm及grep-derived aids在 **`x9/out/hevc-fb-owner-caller-20261116/`**，preview和`constructor-known-direct-predecessors.json`仅为standard候选，未当完整CFG。首次exact核对在 **hevc-fb-owner-caller-final-20261116/profile-exact.log/.exit=1** 拒绝草稿span起点 **30aeda**（处于30aed8的4B指令内部），改为30aed8，未覆盖失败证据。最终 **`x9/out/hevc-fb-owner-caller-source-final-20261116/`** source-head=4f32991：syntax.log/.exit=0，**extract-profile-exact-entries.py / profile-exact.log/.exit=0**，FB-transfer **45→62 spans**，IRQ142/clock48/service25逐独立exact-PC/raw/boundary核对，mem-wait63不变；另核原5个mutex AUIPC/JALR及新增17个direct call/jump targets。原件经真实 **firmware()** 的FB62/IRQ142与独立profile完全一致，全部runtime flags=false：成功入口是 **extract-firmware-entry-pathcorrect.py / firmware-entry-pathcorrect.log/.exit=0**；先前复制时误把root(x9)当repo root的helper仅报FileNotFound、exit1，保留旧文件/log，不作输入证据。重提复制本目录 **exact helper + pathcorrect helper** 到新同级out后依次运行（JSON exclusive-create；firmware输入仍固定repo `.local`），不要运行旧path错误helper，不重跑成功构建产物。
- **CI/下一具体stage/停止边界**：git diff --check通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，记录在最终目录gh-runs.*，无CI run ID。下一仍从constructor **3072e2**及候选stack活跃期、operation全部publisher/外层锁owner lifetime追最早未证stage；不重复已闭mutex wrappers或旧半字caller扫描，不用参数保存、software mutex或zero return代替AP cache ownership/可恢复producer exclusion/durable dequeue。逐buffer cache/退休/c006 ACK、持续session provider、cold最高authority、DPB/layout/scratch/linear NV12/P010/HYFBC转换与完整HEIF alpha/规范属性/自动VT-CMPhoto-ImageIO/色彩实效仍必做。没有测试脚本/套件/人工向量、native/frontend/full-link/boot构建、成功构建产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC或分区操作；功能树保留不合main，唯一最终boot继续等全部代码齐全。

#### 2026-11-16 HEIF iovl 实际线性 overlay / straight-alpha 导出（上一里程碑）

- **上一turn/本turn实际进展**：上一turn是progress（4f32991f/5875bd42闭合新固件caller证据），本turn从tracked干净的 `5875bd42` 继续同一 `.worktrees/hevc-hw` / `feat/hevc-hw`。constructor3072e2/vendor分支未解，未猜ISA或重复旧scanner；转而推进完整HEIF派生图中此前完全缺失的 **iovl** 执行。十文件真实源码 **`0c69cc5a`** 已commit/push成功（5875bd42..0c69cc5a，最终目录push-code.log/.exit=0），不是只加接口/提取器或重新定义完成目标。未改main/固定`.local`/kernel权限或启动子代理；READY/authority/producer-exclusion/retirement/普通VT gate全保留，完整HEVC/HEIF目标active，无boot。
- **输入→实际几何/像素执行**：`heif_items`识别iovl，按v0的version/flags、4个原始unsigned16 RGBA、16/32-bit canvas尺寸及每个ordered dimg的signed16/32 x/y解析，明确符号扩展；检查payload恰好结束、至少1个child、canvas与现有ispe一致，不将负offset变成大unsigned。plan复用原active-stack cycle/完整DAG height/node/dimension门，保留每个placement；`requires_linear_composition`与`requires_resampling`分开。实际float renderer调用source取得转换后的fill，在signed64里算canvas intersection，按dimg原顺序做premultiplied source-over `src + dst*(1-src.a)`，完全offscreen合法且不读OOB；复用child缓存，执行已有child与parent有序crop/rotate/mirror。packed路径在callback前拒绝overlay，不在非线性/integer RGB上混色；每个最终cache重新核finite RGB、alpha[0,1]及alpha0→RGB0，防止插值下溢产生不可见alpha却污染后续overlay。
- **真实native fill/ICC路径，不伪造背景色空间**：所有native coded child仍走实际exact DecoderID/UsingHardware及原sample/VUI/range gate。iovl先按显式source/graph policy从own/明确inherited colr解释RGB fill；recognized ICC或CICP primaries/transfer/gamma必须实际存在，不借SPS默认或YCbCr matrix解释RGB16。背景作为opaque1x1 RGBA16经真实VTPixelTransfer转到实际derived extended-linear ICC，并核返回profile/finite/opaque，再由CPU乘原始alpha/65535并填canvas；**fill不计为hardware frame**。alpha0 fill无可观察RGB，直接透明0，不猜profile。即使几何全整数，有overlay也linearize；matrix working ICC或显式matrix intermediate限定仍在，LUT final仅经实际converter成功才可返回。此处是现有显式renderer的linear-light rendering选择，不声称普通ImageIO/CMPhoto或规范默认色彩/混合策略已还原。
- **half导出实际保留alpha，而非只改attachment**：以前export只允许opaque，现linear root在CPU保存float alpha、把premultiplied RGB显式unassociate并置opaque1，让VT只做opaque RGB→final working ICC/half转换；核返回profile/finite/opaque后，对自有half output writable lock，按nearest/ties-even（含subnormal）转换并恢复原alpha，half alpha0的RGB归0。最后标straight-alpha及真实/conservative opaque属性，不依赖VT猜alpha传播或premul约定，不改借来的decoded像素。float root16B + half8B + float alpha4B **28B/root pixel**的已知共存峰在decode前检查，实际三块CPU heap也受同一budget约束；全compositor caches/intermediates仍单独累加到相同raster budget，VT内部存储不是此额度。info区分LinearLightComposition与真实LinearLightResampling，记录实际linear ICC/alpha mode。
- **prem/aux边界与元数据**：新增raw ordered prem target枚举、PremultipliedReferenceCount/IDs；所有reachable prem source和aux在native preflight/compose callback前ENOTSUP，不再潜在忽略prem标记。metadata/CLI保留raw canvas RGBA16、signed placement及dimg order。**overlay背景/float callback alpha不是HEVC辅助alpha解码**：monochrome/Rext、auxC的真实alpha carrier、prem source色彩去关联仍未实现；旧bitstream header的“stays on Apple SW”注释已更正为真实ENOTSUP/no fallback，未改变shared parser/kernel ABI或capability。build manifest新增iovl input/linear renderer/native fill事实，但complete/auxiliary merge/runtime hardware仍false。
- **参考与最终确切编译/源码分析**：只下载公开libheif的`context.cc`、`image-items/image_item.cc`、`image-items/overlay.cc`到gitignored **`x9/out/heif-alpha-reference-20261116/`**（curl日志同目录），核对iovl field widths/sign/ordered placements及prem方向；其中关于clap clamp/默认alpha scale/颜色的注释和行为未照抄成规范/运行证据，也未成为构建依赖。首次frontend `x9/out/heif-overlay-first-20261116/` exit0，但最终以源码补齐prem metadata、export budget、零alpha/缓存验证后的 **`x9/out/heif-overlay-final-20261116/`** 为准：完整iOS dylib/client编译/链接/ad-hoc签名 **frontend.log/.exit=0，-Wall -Wextra -Werror无诊断**；`analyze-source.py / analyzer.log/.exit=0`对改变的items/compositor/metadata/native/CLI五单元做Clang源码分析，无diagnostic（日志含确切命令），source-head=0c69cc5a、source.diff/input-composition.diff/native.diff保留。没有运行测试脚本/套件/样本/人工像素向量，不把编译/analyzer绿色当实际overlay、HDR/色彩或硬件验收；没有成功构建产物二次解包/hash/反汇编/复建比较。
- **fill-only不能伪装成硬件成功/最终恢复源码**：收尾静态审查发现iovl的所有coded placements可能完全offscreen，此时只有真实背景fill可见，根本无需执行entropy decode；像素有效不证明硬件跑过。窄修复 **`c4612f7d`** 已commit/push（0c69cc5a..c4612f7d，最新目录push-code.log/.exit=0）：info总是记录真实HardwareDecodePerformed/NativeHardwareFrames，只有frames>0才记录实际DecoderID；frames0只记录AllowedDecoderID，不伪造选择/调用。没有跳过任何可见coded source的exact DecoderID/UsingHardware检查。此后最终完整frontend **`x9/out/heif-overlay-native-final-20261116/{frontend/,frontend.log,frontend.exit}` exit0/-Wall -Wextra -Werror无诊断**，仅重新分析改变的native **analyze-source.py/analyzer.log/.exit=0/无diagnostic**；其他四单元前一final的无诊断结论仍有效，不重复分析未变源码。最新source-head=c4612f7d，source.diff保留相对5875bd42的完整feature，native-final.diff为这项窄修；新编译只因源码实际改变，不做成功产物二次比较。此目录是最新完整可编译frontend，不以早期0c69cc5a产物替代最新metadata语义。
- **复编/CI/下一stage与完整完成边界**：复编在活动树用新唯一目录执行 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/<新唯一名>/frontend`；复制相应final的analyze-source.py到新out可重做源码SAST，不运行样本。git diff --check通过；最新gh run list --branch feat/hevc-hw --limit3 --json databaseId,status,conclusion,headSha **exit0/`[]`**，最新native-final目录gh-runs.*保留，无CI run ID。下一继续真实aux-alpha carrier/monochrome-Rext与prem源颜色链、规范属性/implicit lengths及普通VT-CMPhoto-ImageIO路由；coded-item sample/HardwarePicture仍是独立IDR gate，不能称已覆盖全部合法HEIF IRAP。底层constructor stack/全部publisher owner lifetime（3072e2仍未闭）、cold最高authority、可恢复producer exclusion/durable dequeue、cache/逐buffer退休/c006 ACK、持续session provider/完整HEVC视频、完整DPB/layout/scratch/linear NV12/P010/HYFBC及HDR/LUT/tone-map/chroma-siting实效全部仍必做。未kernel编译/full link/boot、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作，所有compile/analyzer/curl/gh/git工具terminal，无后台任务；功能树保留不合main，唯一最终boot仍等全部代码齐全。

#### 2026-11-16 独立 HEIF BLA/CRA → native session / drain（最新交接）

- **实际进展/固定恢复点**：本turn从tracked干净的`fe4d1a1e`继续`.worktrees/hevc-hw` / `feat/hevc-hw`，九文件源码 **`ee355858`** 已commit/push到`origin/feat/hevc-hw`（push-code.log/.exit=0）。新增独立IRAP输入及真实既有session dispatch接入，不是只把IDR改成无约束AU gate；本turn为progress，完整HEVC/HEIF目标仍active，未合main或出boot。未改固定`.local`软链/布局，未开子代理。
- **先核真实调用链，再接入口**：`vt_decoder.c:start`创建SESSION_CREATE并提供完整parameter-set catalog；`vt_video_sessions.inc:decode_frame`走AU gate→SESSION_SUBMIT，在EnableTemporalProcessing未置位时实际`video_collect(true)`执行SESSION_DRAIN→按原token RECEIVE，直到pending0与ENODATA；`finish`也走同一bounded drain。kernel `vdec_sessions.inc`对应实际session_ops，不借legacy IDR ioctl。DRAIN合同保留DPB/parameter sets、允许后续submit；独立hardware-picture每次使用fresh VT/native session、只submit一次、flags0同步drain再wait/invalidate。**源码路线已经接通不等于真实provider存在或CRA已实跑输出**。
- **新gate及保持的边界**：`x9_hevc_irap_annexb`与stateful AU共用完整bounded framing/catalog validation pass，但独立模式只允许base-layer BLA16..18、IDR19..20、CRA21；TemporalId0、恰好一幅picture、首slice的first标志、所有slice同NAL type/PPS/no_output_of_prior_pics_flag。检查AUD唯一且最前、matching in-band VPS/SPS/PPS及prefix SEI在VCL前、suffix SEI/filler在VCL后、EOS/EOB末端有序，拒绝suffix后继续VCL与leading/inter/reserved VCL；先完整验证和容量门，再写Annex B。与legacy不同不prepend全部PS，session create已送完整catalog。optional raw picture facts和written失败归0。**Main-compatible Main Still Picture profile3仍仅IDR**，不以Main兼容位放开BLA/CRA；原`x9_hevc_idr_annexb`及legacy内核ioctl没有修改，stateful AU模式原接受语义保留。
- **完整前端接入/元数据事实**：`heif_coremedia`、`X9HEVCCopyHardwarePicture`、raw/HEIF `vdecctl`都改用专用IRAP gate，避免CLI仍IDR挡住新HEIF输入。CM sample仍是原length-prefixed AU及全部parameter sets，并核CM生成catalog身份/geometry；VUI仍跟实际picture PPS→SPS，不借representative默认值。sample info新增真实 **IRAPNALUnitType / NoOutputOfPriorPicsFlag**；native prepare保留同一immutable coded sample的info，再单独添加effective/inherited color解释，故这些字段也进入最终Items。原始flag不是推断NoRaslOutputFlag，更不是硬件输出/退休证明。exact DecoderID/RequireHardware、UsingHardware前后、exactly-one原source callback、completion/token、NV12/P010 plane/depth/geometry门均保留。build manifest记录准确profile/dispatch范围，physical_provider/hardware_tested/IRAP runtime_verified/complete等仍false。
- **确切编译/源码分析/CI**：唯一目录 **`x9/out/heif-standalone-irap-20261116/`** 保留`source.diff`（相对fe4d1a1e的九文件修改）、`source-head.txt=ee355858`、原entrypoint检索另在`x9/out/hevc-irap-entrypoints-20261116.txt`。最终`frontend.log/.exit=0`：完整iOS dylib/client编译/链接/ad-hoc签名，`-Wall -Wextra -Werror`无诊断。`analyze-source.py`对修改的bitstream/coremedia/picture/native/CLI五C单元Clang源码分析，`analyzer.log/.exit=0`、无diagnostic。shared parser改变后，仅正式Builder（compiler_launcher空、禁ccache）重编受影响 **sweet_x9_vdec.o**，`kernel.exit=0`、**55 warnings**（含C99声明顺序等warning，不称无警告）；helper为`compile-native.py`，原console为`kernel-console.log`，Builder日志/对象/depfiles/result.json在`kernel/`。`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，gh-runs.*保留，无CI run ID。没有测试脚本/套件/样本/人工向量、full kernel link/boot、成功产物二次解包/hash/反汇编/复建比较或设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作。
- **复编/下一准确stage**：在本活动树执行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/<新唯一名>/frontend`；源码analyzer复制到新out并保存stdout，kernel helper复制并将自身out改成新唯一目录再运行（cwd必须本活动树），不能覆盖/重复检查旧成功产物。下一回到真实底层constructor **3072e2**的vendor branch/候选mutex stack reaching-definitions、全部publisher/owner lifetime与可恢复AP producer exclusion；未闭不能把mutex/return0/equal snapshot当durable dequeue或授予buffer退休。冷NS/controller/firmware/clock最高authority、真正持续session_ops provider/完整HEVC视频与DPB/layout/scratch、WAITISR/c006实际ACK、cache/逐buffer退休、linear NV12/P010/HYFBC、aux-alpha monochrome/Rext/prem源去关联、规范HEIF属性/implicit lengths、普通VT→CMPhoto/ImageIO路由及HDR/LUT/tone-map/chroma-siting实效仍必做；这次IRAP扩展不消除这些缺口。READY/authority/producer-exclusion/retirement/普通VT gate全保留，全部代码齐全后才出唯一最终boot。

#### 2026-11-16 constructor 前置 allocation callback / firmware alias 边界（最新交接）

- **活动源码/进展**：仍为`.worktrees/hevc-hw` / `feat/hevc-hw`，接`b219a1d6`及上turn未提交的`vcp_artifacts.py`，源码提交 **`30511a8`**。本批只闭合原固件新allocation调用链的静态身份/字段/边界，不是native provider、live mapping或逐buffer retirement实现；constructor **3072e2** vendor branch仍未解，未猜ISA。READY/authority/producer-exclusion/retirement/普通VT门全部保留，完整HEVC/HEIF目标active，不合main、不出boot。
- **观察：正确allocator路径**：**2466d0** log字符串247621实际为`eVideoMemAlloc`，并非直接`eVideoMemMAP`。它接88B descriptor，AP token在+80/+84、service在+64；table32c0f0+service*96的+24是allocation callback。service1安装窗口305844..30585e在30585c把 **308aa4**存stack48，经3058a2→242f00复制stack24的96B block，对应callback cell **32c168**（free cell32c170）。这些是captured安装路径，不是current runtime table/排他证明。
- **观察：两种对象与结果检查**：2467e6以a0/a1 token halves、a2=stack16调用callback，独立32B request/result block含type/length和三个u64结果（+8/+16/+24），不能混成88B descriptor。308b22→3052f2发送c005/service1/64B，308b26保存结果；308b28..308b42先复制三组reply字段，**308bdc**才检查保存的send result。可见zero308c0e、minus-one308be2在308c10汇合，但vendor return edges/完整reply-status validation仍未证；非零地址不等于transport成功或DMA完成。
- **观察：alias不是CPU地址**：wrapper将allocator-token stack40/44写descriptor+32/+36，selected address pair写+24/+28，选择的vendor edges未闭。24685a→**246908**才是实际`eVideoMemMAP` converter，246916标准AUIPC/JALR精算到已闭合的TCM **10fdc**（复用既有C.57 remap证据，不重解），24685e将firmware alias写+16。constructor3072de→2466d0传s10+**0x1990**、88，和cleanup同一descriptor offset；之后3072e2及mutex stack完整reaching-definitions/owner寿命仍未闭。firmware alias不能用来推XNU pointer，也不授权逐buffer退休。
- **原始/失败/最终提取资料**：新独立frames在 **`x9/out/hevc-rv55-instruction-origin-20261116/`**：2466d0..246b2c **390/38**、308aa4..308bc4 **114/4**、308bc4..308c1a **30/3**（instructions/unresolved）。`allocation-callback-through-next-entry`文件名的308bc4仅首窗末端，不是函数边界，实际return308c18。首次`hevc-memory-map-callback-final-20261116/profile-exact.exit=1`因span end246706落在246704的4B vendor instruction内部，保留失败件；修end246708及第二span end246724后，最终 **`x9/out/hevc-memory-map-callback-source-final-20261116/`** syntax/profile-exact/firmware-entry `.exit`均 **0**。新增12个mem spans均独立exact-PC/raw/boundary核对，mem **63→75**（原63本turn未重新分帧），IRQ142/FB62/clock48/service25原独立证据一致；真实原始vcp_b.img经`firmware()`与direct IRQ142/FB62/mem75完整相等，全部runtime flags=false。source.diff/source-head.txt、原log/JSON及helpers同目录；复提复制两个extract helper到新同级out目录，JSON exclusive-create，不覆盖旧件。
- **工具链线索与排除**：原TCM offset187ce有`FreeRTOS/Source/portable/LLVM/RV55/port.c`字符串，仅源码路径线索，不是ISA证明。当前与固定历史Andes opcode header的BEQC/BNEC仍不匹配0x67 vendor指令；相关GitHub工具链搜索无有效结果，原JSON/exit保留instruction-origin目录，不再重复弱mask/同组搜索，也不将standard-B-layout candidate当taken CFG。
- **CI/下一实际stage/边界**：`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，最终目录gh-runs.*，无CI run ID。本批无C/frontend/kernel/full-link/boot构建、测试脚本/套件/样本/人工向量、成功构建产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作；固定`.local`未改、无子代理。下一从3072e2/完整publisher owner寿命或新定位allocator真实对象链继续；cold最高authority、持续session provider/DPB/layout/scratch、AP排他/durable dequeue、cache/逐buffer退休/c006真实ACK、linear NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT-CMPhoto-ImageIO与色彩实效仍必做，唯一最终boot等全部代码齐全。

#### 2026-11-16 allocator → generic IPI transport 的返回来源修正（最新交接）

- **活动源码/阶段**：接已push的allocator证据`30511a8b`/交接`0ddae97a`，仍在`.worktrees/hevc-hw` / `feat/hevc-hw`。新增证据源码 **`2796571`**；没有native/kernel/frontend代码改动或硬解运行结果，本turn仍为progress，完整目标active。前一交接把308b26称“保存send result”，本批精确修正为 **保存service wrapper结果**；提取字段改为`callback_result.saved_wrapper_result/wrapper_result_gate`，不能据它证明generic transport或reply status成功。
- **新独立观察，不猜opaque epilogue**：完整frame **30507a..3052fc，210 instructions / 25 unresolved** 在`x9/out/hevc-allocator-transport-result-20261116/generic-send-through-service-wrapper.{json,asm}`，generic-send.log/.exit=0。service wrapper **3052f2**在 **3052f4→30507a**标准C.JAL后，**3052f6**明确执行`li a0,0`，然后是未解vendor epilogue **3052f8**与ret3052fa。故可见标准指令没有直接保留generic返回值；**不能进一步假定opaque epilogue保持a0，更不能宣称wrapper完整返回语义或所有路径必为0**。allocation callback308b26保存的是该wrapper结果，308bdc才检查它；reply三组地址已在前面复制，完整reply-status validation仍false。
- **generic transport的实际数据链**：305082保存bytes→s6，305084保存原payload→s3；stack8为service、stack12为length、stack16为payload，经 **3050a8→TCM701c**复制请求。**305166→TCM49a4**调用后30516a将result存s4；候选receive分支 **3051f2/30523a→TCM6e08**，随后分别读取packet id/context。**3052b2..3052bc→TCM701c**以a0=s3/a1=s2/a2=s6复制stack payload回原request，3052e6可见从s4赋a0，再经未知epilogue返回。6e08/49a4/701c在本批只定位标准direct target，不重新解析它们或凭名字证明队列/IPC寿命；branch/packet checks/full CFG仍未闭。source字符串30549a为`../../hardware/libvcodec_up/v4l2_vpud/posix/kernel/ipi.c`，VDEC ACK log3054d3仅为日志，copy/ACK字样不是allocation成功、c006退休或像素completion。
- **最终核对/原件重放**：**`x9/out/hevc-allocator-transport-result-final-20261116/`** syntax/profile-exact/firmware-entry `.exit`均 **0**；新增 **6** 个memory spans全部独立exact-PC/raw/boundary匹配，mem **75→81**，既有75 spans与上一final逐记录完全不变（未重新分帧）。额外独立精算五个AUIPC/JALR targets、3052f4 C.JAL及3052f6 literal-zero原bytes，IRQ142/FB62/clock48/service25仍与既有独立frames一致；真实原始vcp_b.img经`firmware()`与direct IRQ142/FB62/mem81全部相等，所有runtime flags=false。source-head.txt=2796571、source.diff、原log/JSON/两个extract helpers同目录；复提复制helpers到新同级out目录依次运行，JSON exclusive-create，不能覆盖旧件。新frame重放：`python3 -I -B x9/tools/rv32_slice.py x9/out/hevc-ring-producer-20261115/rv55-dram.bin --base 0x200000 --start 0x30507a --bytes 0x282 --r2 /opt/homebrew/bin/r2 --out x9/out/<新唯一名>/generic-send.json`。
- **CI/收尾与下一未证stage**：`git diff --check`通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，最终目录gh-runs.*，无CI run ID。本批无C/frontend/kernel/full-link/boot编译、测试脚本/套件/样本/人工向量、成功构建产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作；固定`.local`未动、无子代理。下一仍追constructor **3072e2**及全部publisher/owner lifetime，或利用正确wrapper/generic返回分层定位真实reply状态检查与对象链；不要把wrapper zero、ACK日志、复制地址或mutex当排他/退休，也不重做旧ISA弱搜索。cold最高authority、持续session provider/完整DPB/layout/scratch、AP排他/durable dequeue、cache/逐buffer退休/c006实际ACK、linear NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT-CMPhoto-ImageIO及色彩实效仍必做。READY/authority/producer-exclusion/retirement/普通VT门保留，不合main；全部代码齐全后才出唯一最终boot。

#### 2026-11-16 HEIF item-local HDR → CM → native callback（最新交接）

- **活动源码/阶段**：继续`.worktrees/hevc-hw` / `feat/hevc-hw`，基线`dac47207`。完成上一turn的11文件HDR草稿，源码提交 **`1e08ac3`**；这是一段实际HEIF输入/输出功能，不是物理硬解验收，完整目标active。前一allocator→generic返回来源修正仍有效：3052f6只证明可见literal-zero，opaque epilogue及wrapper完整返回语义仍未知，不能把saved wrapper result当generic transport/reply成功。
- **输入到输出真实接线**：item property parser识别普通（非FullBox）`mdcv`24B/`clli`4B，exact长度、同类重复association拒绝；item info保留原始big-endian bytes。新`x9_hevc_sample_hdr()`先通过完整AU/catalog门，再经已有canonical EBSP bit reader逐条读取SEI：255扩展type/size溢出检查、payload边界、每NAL最多1024 messages、必需rbsp trailing bits；不建RBSP heap、不缩放数值/重排primaries。同AU同类重复值只有逐字节相等才接受，失败输出归零。**当前仅解释base-layer prefix TemporalId0的直接137/144**；HDR suffix/nonzeroTemporalId、type133 scalable_nesting均返回ENOTSUP，不把未实现scope笼统称malformed，其他普通payload仅有界读过/验证不解释。没有persistent-video SEI state。
- **来源合并/所有权**：真正`x9_heif_copy_item_sample()`接SEI及容器HDR合并；两来源同类必须exact相等，缺失不生成默认值。merge先校验两类并分配全部CFData，失败不改变caller extensions，清理已分配对象。CM format创建后核其HDR24/4B与请求exact相等（包括absence），新增`ResolvedHDRMetadata`及两类SEI presence事实；原IRAP、PS identity、尺寸门保留。`x9_heif_copy_color_extensions()`仍只取colr，HDR不参与graph继承。
- **真实callback与转换边界**：CM→CV attachment只允许HDR24/4B；自有输出buffer在format缺失HDR时清掉对应旧值，防未来复用串帧。`X9HEVCCopyHardwarePicture()`在真实callback/DecoderID/UsingHardware/token/plane门后只读核CM→callback HDR identity，包括absence；不修改借来的pixel让验证通过。native raster的源item/SEI bytes继续保留在`Items`，成功ICC transfer的RGB target移除源mastering/MaxCLL，最终info明示disposition；不将裁剪/重采样/overlay后的RGB冒充重新测量的HDR输出，不声称tone mapping或真实色彩效果已实现。
- **参考与支持界限**：`x9/out/heif-hdr-property-reference-20261116/`留公开AVIF对ISOBMFF mdcv/clli的引用、FFmpeg movenc/HEVC SEI源文件及下载log/exit；既有`heif-item-offset-reference-20261115/libheif-box.cc`与Xcode SDK `CMFormatDescription.h:874–877`核普通box及CM24/4B big-endian HEVC payload布局。AVIF只是box参考，未加AV1 decoder；FFmpeg不是规范或实机结果，prefix/Tid0是本实现保守支持门。
- **确切编译/静态分析**：上一turn未确认的frontend句柄9552本turn已poll，**exit0**，原件在`x9/out/heif-hdr-direct-sei-first-20261116/`，未重启/覆盖。收紧scope与自有buffer absence后，最终 **`x9/out/heif-hdr-direct-sei-final-20261116/`** 的Python syntax、7个改变C单元的clang `--analyze -Wall -Wextra -Werror`（无诊断）、iOS dylib/client编译/链接/ad-hoc签名均 **exit0**。共享parser进入kernel，正常Builder关闭ccache仅编`sweet_x9_vdec.o` **exit0/58 warnings**（3条新增为SEI代码的C99 declaration-after-statement；未压制诊断）。对应`syntax/analyze/frontend/kernel` `.log/.exit`、两个helpers、source.diff/source-head.txt全保留；不二次解包/hash/反汇编或比较成功产物。
- **最小复编/CI**：活动树运行`python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/<新唯一名>/frontend`；源码analyzer/native复编helper复制到新out并仅改native helper的输出目录，cwd保持活动树。`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，gh-runs.*在final目录，无本轮CI run ID。没有测试脚本/套件/样本/人工向量、XNU对象树重建、full kernel link、boot pack或设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作；`.local`链接未动，无子代理。
- **下一具体工作/未完成**：HDR runtime与persistent/scoped SEI、tone-map/LUT/chroma实效仍未验/未完成；build事实将两项runtime/persistence设false。回到constructor **3072e2**及全部owner/publisher lifetime、真实generic reply状态，不重复旧ISA弱搜索或拿mutex/ACK日志当排他。cold最高authority、真实持续session provider/完整DPB/layout/scratch、AP producer-exclusion/durable dequeue、cache/逐buffer退休/c006实际ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths和普通VT-CMPhoto-ImageIO仍必做。READY/authority/retirement/普通VT门全保留；不合main，全部代码齐全后才出唯一最终boot。

#### 2026-11-16 generic transport 的 ACK-log gate 与 channel installer

- **活动源码/本turn进展**：上一turn是progress（HDR源码`1e08ac3d`/交接`131e24fa`已push）；本turn从tracked干净的`131e24fa`继续`.worktrees/hevc-hw` / `feat/hevc-hw`。constructor **3072e2**仍是`67160520`的未解vendor分支，没有重跑已排除的Andes weak-mask/LLVM feature类；转到已闭generic receive后的新direct helper及TCM send target。新提取器源码 **`a5f821d`** 新增真实`firmware()`字段`service_transport_send_evidence`（19 spans），已push（131e24fa..a5f821d2）；**不是新增native硬解能力/物理验收**，完整goal active，READY/authority/producer-exclusion/retirement/普通VT门全保留，无boot。
- **ACK gate不是reply validator（新观察）**：独立完整getter **243674..24367e，3 instructions/0 unresolved**仅`lui/lbu/ret`，读固定byte **32c216**，不读incoming参数、packet/context或status。独立writer **24367e..24370e，43/2**在 **24368a→243060**传字符串 **244dfa `vendor.mtk.vdec.perf.log.disable`**及a1=0，24368c `snez`、243694 `sb`；另2436a2同helper取 **244e1b `vendor.mtk.vdec.profile.enable`**，2436a8写独立word **32c218**。generic **305200→243674**后 **305204 C.BNEZ a0→3052b2**，目标是原request copy-back，跳过ACK日志。故这条分支不能解释为reply/allocation成功校验；property helper完整语义/当前global值仍未证，未据字符串推当前日志策略。
- **真实TCM send channel路径（新观察）**：独立 **49a4..4a14，47/9**，可见selector<=35、table **223ec + selector*8**的member0→send object；49c0→49da取object+16 queue handle，经 **49ea→68e2**（a1=-1），vendor result gate49f0未解；可见4a00→**4aba** tail jump。返回后49c2保存inner结果，**49cc→6788**给queue，49d0从saved s1赋a0；invalid/take-failure arm分别visible literal-3/-5。**opaque epilogues仍不假定保持参数/a0，不宣称所有路径返回这些值或已send/remote ACK**。
- **channel table实际installer与descriptor来源（新观察）**：独立 **4832..48d6，56/4**。global queue4838→6760，store223e8；send count从 **22518**取，descriptor base **2048c/stride24**，ID member12，**4868→6760**结果存member16，**4878**将descriptor pointer存`223ec + id*8 +0`。receive count **22510**、descriptor base **20550/stride64**，**48aa→66c2**以[1,0,3]创建queue，48b0存member16、48be存table member4。原件send rows **2048c=[0,18,0,1,0,0]**、**204d4=[1,18,0,17,0,0]**逐raw核对；结合下层capacity shift2，18是**wire slot word容量，不是HEVC DPB容量**。原件count globals及table id1/id17仍0，只说明on-disk初始化值，不能称runtime channel缺席。startup count writers、installer reachability/覆盖排他仍未闭。
- **slot copy/trigger不当DMA completion（新观察）**：独立 **4aba..4b7a，64/8**读取object+4 capacity、保存a0/a1/a2/a3→s6/s2/s3/s4；4ade/4b08→**4cce**传object+0 low4及object+8，retry调 **4af6→caf2**传literal1，但不称其单位/完整期限。**4b38→4b7a**前capacity<<2，4b34 vendor算术未知；**4b62→4c92**前取object+8并构造1<<index，trigger leaf未解，不冒认AP MMIO/remote receipt。copy leaf **4b7a..4bf2，40/6**使用table **203f0/stride52**、capacity+4/目的base+12，4be2→701c复制，4bec只increment软件member40。encoded operations **4bbe/4bc6/4bd8=`0f400700`**和firmware-local212f3000+96/literal99均只保留原bytes，cache语义/AP ownership仍false；软件counter、copy、trigger或visible0不是ACK、cache-handoff或逐buffer退休。
- **确切原件分析/最终资料**：新窗口/preview/字符串raw在 **`x9/out/hevc-transport-reply-identity-20261116/`**；preview不作独立边界证明。最终 **`x9/out/hevc-transport-send-log-gate-final2-20261116/`** syntax、`extract-profile-exact-entries.py`/profile-exact、`extract-firmware-entry.py`/firmware-entry `.exit`全 **0**：19 spans全部独立exact-PC/raw/boundary匹配，16个standard direct targets及305204 conditional target独立精算；先前IRQ142/FB62/mem81 **整份profile逐字段不变**，未重分旧帧。实读固定原始`.local/.../vcp-real-android-active-b/vcp_b.img`经真实`firmware()`的19/142/62/81均与独立profile完全相等，所有runtime flags=false；source.diff/source-head.txt=a5f821d与两个helpers/原log/JSON同目录。前一final仅15-span中间成功版，未覆盖旧结果或改原件。
- **重放/CI/约束**：新同级out复制final2两个extract helpers，依次`python3 -I -B x9/out/<新唯一名>/extract-profile-exact-entries.py`与`.../extract-firmware-entry.py`（exclusive-create）；新frame重放用`rv32_slice.py`、原TCM/base0、start4832/bytesa4或49a4/bytes70、r2=`/opt/homebrew/bin/r2`、out新文件。`gh run list --branch feat/hevc-hw --limit3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**；特定句柄26468已poll终态，gh-runs.*留final并复制final2，无CI run ID。只改Python提取源码/NOTES，**没有C/frontend/kernel/XNU/full-link/boot编译、测试脚本/套件/样本/人工向量、成功构建产物二次检查或设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作**；`.local`未动，无子代理。
- **下一准确stage/完整未完成**：从新定位的 **send/receive count globals22518/22510及4832 installer上游**追真实启动安装、channel table owner/覆盖寿命，或从4cce/4c92继续证明physical slot predicate/trigger与normal AP通路；不要重解已闭getter、mutex队列或用disabled log当reply验证。constructor3072e2/全部publisher lifetime、cold最高authority、真实持续session provider/完整DPB/layout/scratch、AP排他/durable dequeue、cache/逐buffer退休/c006实际ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT-CMPhoto-ImageIO及scoped/persistent HDR/tone-map/LUT/chroma实效仍必做。不合main；全部代码齐全后才出唯一最终boot。

#### 2026-11-16 channel literal counts 与 startup 调用顺序

- **活动树/本turn进展**：从tracked干净的`104c668a`继续`.worktrees/hevc-hw` / `feat/hevc-hw`，`.local -> ../../.local`未动。新提取源码 **`077c7d0`** 将真实`firmware()`的`service_transport_send_evidence`从19扩为**33 spans**，补充count leaf、启动调用顺序和全部固定descriptor；旧19 spans/ACK gate/channel wrapper/slot send-copy完全不变，旧IRQ142/FB62/mem81整份profile不变。这是**静态底层证据进展，不是原生硬解功能完成**；goal active，所有READY/authority/producer-exclusion/retirement/普通VT门保留，不合main，不出boot。
- **count setter / trampoline（新观察）**：独立完整 **`cafe..cb14`，7 instructions/0 unresolved**，`lui a0,22; li a1,9; lui a2,22; li a3,8; sw; sw; ret`；**cb0a→22510=9 receive**、**cb0e→22518=8 send**。独立 **`caf6..cafa`，1/0**，raw`6f70dfd3`是标准**JAL x0→4832 tail jump**，不是ra call。**11072→caf6**是JAL ra，visible return PC11076；initializer的opaque epilogue是否保持它仍未证。on-disk count0不等于runtime0，literal setter也不等于已执行或已安装。
- **setter真正上游（新观察/边界）**：独立 **`4f80..5006`，44/8** 保存a0/a1/a2/a3/a4→s0/s4/s1/s2/s3；**4f90=`67553400` vendor gate仍unknown**，不猜条件或是否到达4f9a。可见4f9a以52计算table **203f0**，4fb6/4fba写member0/8，**4fbe→cafe**；4fc2用s1&-3，标准4fc6 C.BEQZ→4fdc；非zero可见4fca写buffer base+12、4fd8 tail→ced0，zero可见4fe2→cb16。opaque entry/epilogues仍不冒认参数/a0保存或完整CFG。这解释count写入口不是4832内联初始化，且地址相邻不能代替调用证明。
- **outer startup 的静态先后（新观察，不当执行证明）**：独立 **`fdc6..fe46`，39/4**，fdca读mhartid、4(sp)快照，标准fdd8 C.BEQZ a0→fdfe；**fe3c→ff48，紧接fe3e→11030，再fe42→c516**。preinitializer `ff48..10266`由新head89/2 + suffix165/14分别分帧，可见**10220/1023a/10254→4f80**，slot literals0/1/2，a1=1/a2=0，firmware-local buffer literals **7e400/7e500/7e600**，output为sp+28/+24/+20；各后接**10226/10240/1025a→cb60**。`11030..1109e`独立32/1，prelude为1601a/14552/5162/52ba，1105c标准zero branch→1106a，11068 jump→11072，随后initializer trampoline；post直接调用bba6/9154/9220/be20/d2ec/b876/abec/aa2a，1108a+1108e标准AUIPC/JALR→**32627a**。**此顺序要求被调函数返回；4f90可否走setter、完整boot可达性/hart排他、寄存器保持和channel覆盖生命周期都未证**，runtime_startup_order/count_setter/channel_installation flags全false，不称9/8已生效或AP cache authority。
- **全部固定descriptor（新原件观察）**：send **2048c/stride24，8 rows**，ID为`[1,12,15,17,9,5,25,2]`；receive **20550/stride64，9 rows**，ID为`[0,11,15,16,9,22,2,3,32]`；各方向内ID唯一且<=35，完整words/raw均保留。send rows0/3还是先前18-word容量；receive rows0/3也有literal18，但**不据ID邻接/容量相等推实际channel pairing、HEVC DPB或NV12/P010 layout**。packed member0高位不猜role。提取数量跟随已证literal setter而非runtime globals，表的on-disk zero queue/callback不作runtime缺席证明。
- **确切产物/验证结果/重放**：新原件窗口、candidate finder（明确不是CFG/不可达证明）、独立JSON/ASM和全部descriptor raw位于 **`x9/out/hevc-channel-startup-source-20261116/`**；最终 **`x9/out/hevc-channel-startup-final-20261116/`** 的syntax/profile-exact/firmware-entry `.exit`全**0**。33 spans exact-PC/raw/boundary逐项匹配，**44个standard direct call/jump + 3个新C.BEQZ + 1个AUIPC/JALR**独立核对，完整count leaf及两个SW地址独立核算，17 rows与独立raw extraction完全相等。真实固定原始`.local/.../vcp-real-android-active-b/vcp_b.img`经`firmware()`的33/142/62/81均与独立profile完全相等、runtime flags全false；旧19及IRQ/FB/mem不改/不重分旧三份profile。helpers/source.diff/source-head.txt=`077c7d0`/log/JSON齐全；在新唯一同级out复制两个extract helpers，再依次`python3 -I -B x9/out/<新名>/extract-profile-exact-entries.py`与`.../extract-firmware-entry.py`（exclusive-create）。新frame复查为`rv32_slice.py` + TCM/base0 + `--start 0xcafe --bytes 0x16`、`--start 0xcaf6 --bytes 4`、`--start 0x4f80 --bytes 0x86`、`--start 0x11030 --bytes 0x6e`等，本目录保留全部输入窗口。不是测试样本/人工向量。
- **CI/操作/下一stage**：`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，无CI run ID；本轮只有Python源码和NOTES，没有C/frontend/kernel/XNU/full-link/boot构建、测试脚本/套件/样本、成功构建产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作或子代理。下一准确入口可从新 **11058→421c registration，descriptor20376/callback1804**（caller bytes已分帧；注册参数流/handler本体尚未闭）追channel IRQ→slot实际receive/dispatch，或从4f90的新gate/ced0继续证明slot setup是否写count；不要把已有调用顺序当安装成功，也不重跑已排除的constructor3072e2 weak-mask/LLVM feature类。完整剩余必做项仍是constructor/全部publisher lifetime、cold最高authority、真实持续session provider/DPB/layout/scratch、AP排他/durable dequeue、cache/逐buffer退休/c006实际ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT-CMPhoto-ImageIO与scoped/persistent HDR/tone-map/LUT/chroma实效；**全部代码齐全后才出唯一最终boot**。

#### 2026-11-16 slot IRQ / receive callback / deferred queue notification（最新交接）

- **活动源码/本turn进展**：上一turn是progress（`077c7d01`/`90584b59`已push），本turn从tracked干净的`90584b59`继续`.worktrees/hevc-hw` / `feat/hevc-hw`。新源码 **`13cfa0c`** 在真实`firmware()`接入独立 **`service_transport_irq_evidence`，27 spans**；原transport33和IRQ142/FB62/mem81全部字段不变。仍是硬解底层静态证据代码，**不是新增可运行硬解能力或完整验收**；完整goal active、不合main、READY/authority/producer-exclusion/retirement/普通VT门全保留、无boot。
- **修正上轮追踪假设（新观察）**：`11058→421c`的descriptor **20376=`0c0001`** / callback literal **1804** / a2=0已独立核对。handler **1804..18d4，67 instructions/4 unresolved** 可见读取pointer table175a8、firmware-local寄存器21100000/21110000，并调用1712a打印1761c/1764c/17658的`%08x`日志，末段是三条**JAL ra**→f366/f3b6/f3c4（不是tail jumps）。这些下游完整语义仍未证；**1804是与slot receive不同的入口，不能沿这个debug/dump入口冒认channel RX链**。
- **真实slot callback注册（新观察）**：上轮4fca附近的literal经标准addi独立核算为 **4d02，不是1804/4cd2**；**4fd8 tail→ced0**，新独立 **ced0..cf2c，32/2**。ced2从output pointer取slot到s0，ced4保存callback到s2，**ceda→4c5c**传slot/-1；descriptor为 **2037c+slot*3**，原件三行`[52,12,1] / [53,4,1] / [54,4,1]`，context为 **203f0+slot*52**；**cf08→421c**传descriptor/callback/context，标准cf0c zero→cf1e后**cf22→4056**传a1=1。opaque entry/epilogues、初次clear是否实际发生、注册/使能成功与跨hart排他全未证；不能将packed field0直接称AP IRQ号码。
- **slot ACK/trigger/status只证pointer访问（新观察）**：独立 **4c5c..4d02，56/7**；4c5c用table203f0/stride52的**member20** pointer，4c86写传入mask；4c92用**member16** pointer，4cbc写mask，4cc8软件member48++；4cce从**member28 override或member16 fallback**读取，后续bit运算vendor未知，4cfe只软件member44++。receive handler相应取**member32 override或member20 fallback**的snapshot。保存/恢复mstatus路径可见，但pointer当前目标、全部publisher/owner、**W1C含义/AP MMIO authority/cache handoff仍false**，不把软件counter或这次store当remote receipt/逐buffer退休。
- **IRQ receive本体与两个不同callback（新观察/边界）**：独立 **4d02..4f1a，180/17**。context首word&3fffffff→**4d18→cb14**（独立ret-only leaf），4d1e vendor index gate不猜；table203f0/stride52读取status到s2，扫描 **20550/stride64/count22510**，slot low4比较可见，4d84的pending bit操作未知。4daa把descriptor capacity+4<<2给copy byte数，4dd4→701c与4de0→4bf2是不同copy arms，4db4 vendor selection/完整目的指针链仍未证。可见**4e4c C.JALR a4**来自descriptor+20，部分参数是ID+12/第二参数+28/复制buffer候选+24；不是下述global deferred callback。cecc的时间戳语义/单位未知；+44计数只软件accounting。processed mask形成于4dea，可见 **4e82**经slot member20 pointer写mask，再经过**4e8a/4e8e vendor gates**才可见deferred路径；只记录visible地址/路径顺序，**不称所有执行都ACK成功后再通知或已经durably dequeue**。
- **deferred publisher→每channel ISR queue（新窄静态链）**：先前4832 installer未记录的末段 **48ca/48cc**构造literal **48d6**，**48d4是C.J x0→4f76**，不是call。独立完整setter **4f76..4f80，3/0**在4f7a写 **22514**并ret；48d0/48d2 opaque epilogue是否保持a0仍未证。IRQ扫描末段 **4ee2**读global22514，**4ef2**把descriptor+12 ID放a0，**4ef6 C.JALR a1**调它，+40只是软件counter。新独立 **48d6..491a，22/4** callback按selector<=35取 **223ec+ID*8+4** receive object、object+16 queue，**48fa→6d36**传a1=**2250c**；4902 vendor reschedule gate未解，4908可见tail→57da。**这个helper只到ISR queue signal，不复制payload，不是durable消息队列、remote ACK或DMA退休**；22514当前value/全部writer寿命和actual wakeup仍false，不能由setter或notify可见宣称硬解provider闭环。
- **receive client成员不能凭名字倒置（新观察）**：独立 **491a..49a4，49/6**，保存a1→s0/a2→s2/a3→s1，table receive member4对象；4950→68e2 take global223e8 queue、vendor gates4942/4956仍unknown，检查object+24是否0。可见 **4988: a1→member20，498c: a3→member24，4990: a2→member28**，4998→6788 give；与IRQ读member20作callback、member24复制buffer候选、member28作第二参数相吻合，**不能把a3/s1叫callback或member20叫context**。成员SW、IRQ LW参数与两处compressed indirect call均独立精算；真实client caller/所有publisher/覆盖排他及最终packet ownership仍未闭。
- **原件/最终验证/重放**：原件窗口、r2 preview、独立frames、只输出新target候选的finder在 **`x9/out/hevc-channel-irq-source-20261116/`**；final1是命名审查前的中间profile，不覆盖它。最终 **`x9/out/hevc-channel-irq-final2-20261116/`** syntax/profile-exact/真实firmware-entry `.exit`全**0**，27 spans exact-PC/raw/boundary逐项匹配，**29个standard direct targets、2个C.JALR、3个不同callback literals、完整setter/ret leaves、SW/LW成员offset和packed descriptors**独立核对。固定原始`.local/.../vcp-real-android-active-b/vcp_b.img`经真实`firmware()`的27/33/142/62/81均与独立profile完全相等、runtime flags全false；旧33/142/62/81四份profile完全不变、不重分旧帧。两个helpers/source.diff/source-head.txt=`13cfa0c`/原log/JSON齐全。复查新frames用`rv32_slice.py` + TCM/base0 + `--start 0x4d02 --bytes 0x218`或`--start 0xced0 --bytes 0x5c`，r2=`/opt/homebrew/bin/r2`、out新路径；新唯一同级out复制final2两个extract helpers，再依次`python3 -I -B .../extract-profile-exact-entries.py`和`.../extract-firmware-entry.py`（exclusive-create）。没有测试样本/人工向量。
- **CI/操作/下一准确stage与完整范围**：`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，无CI run ID；本turn只有Python/NOTES，无C/frontend/kernel/XNU/full-link/boot构建、测试脚本/套件/样本、成功构建产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作或子代理，`.local`未动。下一新入口是**cb60**安装slot表member16/20/28/32 pointer的来源，或**491a真实client callers**→buffer/callback publisher生命周期→queue consumer；不要再从1804 dump入口假定channel RX，不重跑已证33/IRQ/FB/mem helper或constructor3072e2已排除weak-mask/LLVM feature类。完整剩余仍包括constructor/cold最高authority、真实持续session provider/DPB/layout/scratch、全部publisher/AP排他/durable dequeue/cache/逐buffer退休/c006实际ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT-CMPhoto-ImageIO及scoped/persistent HDR/tone-map/LUT/chroma实效；**全部代码齐全后才出唯一最终boot**。

#### 2026-11-16 slot pointer 原件预置与 physical channel→outer service 桥（最新交接）

- **活动树/交付**：续接 `.worktrees/hevc-hw` / `feat/hevc-hw`，基线 `13045258`；完成上一轮未提交的 `vcp_artifacts.py` 草稿，源码 **`6df72c90`** 已 push。实际 `firmware()` 的 `service_transport_irq_evidence` 从27扩至 **40 spans**；旧27的字段/bytes（除version/new false flags）完整不变，transport33/IRQ142/FB62/mem81整份不变。这是底层静态证据代码，不是新增运行硬解能力；goal active，不合main，READY/authority/producer-exclusion/retirement/普通VT门全保留，无boot。
- **纠正上一交接的pointer installer假设（原件观察）**：slot表 **203f0/stride52** 三行原件已预置 `7e400/7e500/7e600` buffer，member4=`0x40`是64 words；member16/28分别为 `21380104/21390104/213a0104`，member20/32分别为 `21380108/21390108/213a0108`，member24=`21270020/24/28`。完整13words/raw已核对。AP live DTS:18482–18509的port是 `31b80000/31b90000/31ba0000`，SET+100/CLR+10c，**不同于FW+104/+108**；不靠减地址推AP alias/owner，不把on-disk presets当runtime residency或全writer排他。
- **cb60其实是descriptor layout builder（新观察/未知）**：独立 **cb60..ce24，244 instructions/13 unresolved**，receive20550/stride64、send2048c/stride24。标准 **cbec/cbf0** 与compressed **ccee/ccf0**写descriptor member8/word0，mask `ff00000f`；packing `cbe8/ccea`未知。firstword>>30再+1传算术helper **12a3a**，四call为cc46/cc70/cd42/cd6c；完整helper **12a3a..12a8a，25/1**，**12a7a=`33870587`仍opaque**，不声称完整multiply/返回公式或runtime layout。visible index limit33、cd96/cdcc比较和cdfa→ce24、ce14 tail→10266均核对。可见descriptor writes不是slot pointer安装，opaque operations/callees/外部writers未排除；不要重追“cb60是pointer installer”。
- **真实receive clients和缺失桥（新观察）**：两独立DRAM窗口 **3168fc..31691a / 316cfc..316d1a，各10/0**，分别physical channel16/0，callback literal **157b4**、a2=0，a3 buffers **1f894/1f8dc**，各72B且原件全0；31690e/316912与316d0e/316d12 AUIPC/JALR均→491a。491a成员映射仍a1 callback→20、a3 buffer→24、a2 second argument→28。独立 **15758..157d2，44/6** 的157b6 mask -17，157b8 zero→157cc，仅标准mask接受0/16；157cc packet a2→a0，157d0 tail→15758。后者读outer.service+0、length+4、payload+8，以 **1f7c4/stride8**加载callback并在157a0 C.JALR a4；与旧service registrar表一致。**physical channel0/16≠outer service1/2**；1575e vendor gate和opaque epilogue参数保持未证，client执行/覆盖排他、buffer寿命/queue copy/durable dispatch仍false。
- **精确产物/重放**：原件窗口/independent frames/候选finder在 **`x9/out/hevc-slot-pointer-source-20261116/`**（caller候选扫描不是CFG或可达性证明）；最终 **`x9/out/hevc-slot-service-bridge-final-20261116/`** syntax/profile-exact/真实firmware-entry `.exit`全部 **0**。40spans独立exact-PC/raw/boundary匹配；旧29+新7 direct targets、旧2+新1 C.JALR、2 AUIPC/JALR、3 presets、descriptor SW/C.SW、callback/buffer literals、outer0/4/8和table stride均独立核对，所有runtime flags=false。两个extract helpers/source.diff/source-head.txt=`6df72c90`/log/JSON齐全；新唯一同级out复制helpers，依次 `python3 -I -B .../extract-profile-exact-entries.py` 与 `.../extract-firmware-entry.py`（exclusive-create）。原件分帧沿用rv32_slice.py/TCM base0、DRAM base200000、r2=/opt/homebrew/bin/r2，范围见上述窗口和JSON。
- **CI/约束/下一实际stage**：`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，无CI run ID；无C/frontend/kernel/XNU/full-link/boot编译、测试脚本/套件/样本/人工向量、成功构建产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作或子代理，`.local`未动。下一从 **1575e service gate / registered callbacks116f2与118ac**追实际queue copy/admission→worker，或精确核FW preset和AP port配对；不重追1804 dump或constructor已排除weak-mask类。constructor/cold最高authority、持续session provider/完整DPB/layout/scratch、全部publisher/AP排他/durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT-CMPhoto-ImageIO及HDR/LUT/tone-map/chroma实效仍必做；全部代码齐全后才出唯一最终boot。

#### 2026-11-16 outer service callback → ISR payload queue / storage copy（停止时最新交接）

- **活动树/两次里程碑**：本turn已完成前一slot/physical bridge源码 `6df72c90` 与交接 `f7bfe707`，均push；随后继续新增源码 **`35779bf1`**，亦已push到 `origin/feat/hevc-hw`。仍在 `.worktrees/hevc-hw`，没有合main。实际 `firmware()` 的 **service_dispatch_evidence** 从25扩至 **35 spans**，旧25字段/bytes（除version/new false flags）完整不变；transport-IRQ40/transport33/VDEC-IRQ142/FB62/mem81整份不变。**新增的是已接实际入口的静态证据代码，不是持续session provider/运行硬解能力**；完整goal active，所有READY/authority/producer-exclusion/retirement/普通VT门保留，无boot。
- **service1/2真实record与queue选择（新观察）**：新增完整service1 **116f2..11782，51 instructions/7 unresolved**；复用既有service2 **118ac..1192c**独立frame，不重跑旧窗。两者frame88B，visible header写sp+8 service/sp+12 length，payload目的sp+16，wake flag sp+84；先读payload words0/4，copy call分别 **11718/118d2→701c**。虽然literal64和unsigned比较存在，**11712/118cc=`33f69620`决定copy长度的操作仍opaque**，不称已证min/max、完整长度校验或header/payload一致snapshot。标准message category mask **ff000**：service1 A000分支选cell **1f870**，D000选 **1f878**；service2 A000选 **1f874**，其他visible diagnostic arms。**11746/118f0→6c4e**参数a1=sp+8 record、a2=sp+84 wake pointer、a3=0；result gate1174a/118f4和yield gates11770/1191a仍unknown。queue handle存在、成功admission及record生命周期未证。
- **ISR queue send不同于channel deferred signal（新观察/边界）**：新完整 **6c4e..6d36，79/11**，保存queue/source/wake/position到s1/s4/s2/s5；visible **6cb2→5664**进入critical候选、**6d0e→56cc**退出。count+56/capacity+60，标准 **6cc2**在capacity<=count时→6d0c，visible s0=0，不走record copy；非full可见 **6cd0→6a8c**。waiter **6d24→8378** 后，6d28/6d2c vendor操作决定wake value，再6d30写wake pointer。**full callback结果不证明mailbox重发/背压或packet不丢失**；完整entry/admission gate及critical跨hart作用域未证。与physical RX callback调用后的 **48d6→6d36 channel queue notification**不同，这条路径确实包含payload record storage copy候选，但不授予durable runtime dispatch或BS/FB排他。
- **可见storage copy→cursor/count顺序（新观察）**：新完整 **6a8c..6b02，48/2**，读取queue count+56/record-size+64；position=0标准分支到 **6ae0**，目的来自queue+8，**6ae2→701c**，随后 **6aee**写cursor、6af0比较end+4、必要时6af6 wrap到begin+0，最后 **6af8 add1 / 6afc**写count。另一路6a9e→701c用member12。72B record-size来自已证service queue init literal，**不是捕获的runtime queue对象或DPB容量**。opaque entry/epilogue、701c完整body/寄存器保持、barrier、queue exclusion及实际执行仍未闭；visible copy/store顺序不是codec input/DPB/output DMA completion、cache handoff、c006 ACK或退休。
- **确切产物/重放与验收边界**：新增原件frames、日志/exit、r2 preview在 **`x9/out/hevc-service-admission-source-20261116/`**；preview不当独立边界证据。最终 **`x9/out/hevc-service-admission-final-20261116/`** syntax/profile-exact/真实firmware-entry `.exit`全部 **0**：10新spans独立exact-PC/raw/boundary匹配，12 standard direct targets、5标准branches、queue-cell地址、count/size/cursor字段、record/source/wake参数、opaque length raw及visible copy→store顺序核对；旧25和五份既有profiles完全不变、不重分旧帧。真实固定原始vcp_b.img经 `firmware()` 的35/40/33/142/62/81与独立profiles相等，runtime flags全false。两个extract helpers/source.diff/source-head.txt=`35779bf1`/log/JSON齐全；新唯一同级out复制helpers，依次 `python3 -I -B .../extract-profile-exact-entries.py` 和 `.../extract-firmware-entry.py`（exclusive-create）。新frame重放用rv32_slice.py/TCM base0，start116f2 bytes90、6c4e bytese8、6a8c bytes76，r2=/opt/homebrew/bin/r2，out新文件；不运行样本/人工向量。
- **CI/停止状态/下一最早不确定stage**：`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，无CI run ID；没有C/frontend/kernel/XNU/full-link/boot编译、测试脚本/套件、成功构建产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作或子代理；`.local -> ../../.local`未动，无后台任务。下一优先核 **11712/118cc copy-length与6c4e admission gates / 5664、56cc critical作用域 / 701c实际copy body**，再接已定位service workers/common dispatcher；不能用queue count变化或IRQ ACK代替最终硬解成功。不重追cb60 pointer-installer、1804 dump或constructor已排除weak-mask类。constructor/cold最高authority、持续session provider/完整DPB/layout/scratch、全部publisher/AP排他/durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT-CMPhoto-ImageIO及HDR/LUT/tone-map/chroma实效仍全部必做；**代码全部齐全后才出唯一最终boot**。

#### 2026-11-16 shared queue critical 的 hart owner / LR-SC guard（停止时最新交接）

- **本turn分类/活动源码**：上一turn是progress（`6df72c90/35779bf1`及交接`3d9c10c2`已push）；本turn从tracked干净的 `3d9c10c2`继续 `.worktrees/hevc-hw` / `feat/hevc-hw`。新源码 **`d4d5b811`** 已push，真实 `firmware()` 的 **service_dispatch_evidence** 从35扩至 **51 spans**；旧35的全部字段/bytes（除version/new false flag）不变，transport-IRQ40/transport33/VDEC-IRQ142/FB62/mem81整份不变。不是新增可运行native provider/物理硬解能力；完整goal active，不合main，READY/authority/producer-exclusion/retirement/普通VT门保留，不出boot。
- **critical不是仅本hart关中断（新原件观察）**：独立完整 **5664..56cc，38 instructions/5 unresolved** 与 **56cc..5720，30/6**。进入时读mhartid到s3、保存mie到s2，标准 **567a**按mask **ff030888**清mie，退出 **5716**恢复原mie；不是无条件清整个mstatus/MIE。两者使用共享literal **2000c**（原件3words=`[0,0,ffffffff]`），member0 guard/member4 nesting/member8 hart owner。**5692/56ec→1621c**以保存ra作为exchange value；**56a2**同hart→56ba increment nesting，zero nesting可见56b4写hart owner、56bc写count。foreign-hart路径先56a8→1613c释放短guard，再56ac/56ae等nesting归零后retry。退出56f8比较owner，5702先写nesting-1，**5704=`67462500`的清零gate仍opaque**，visible570a/570e写0/-1。这给出shared/hart-aware reentrant guard结构，**实际coherence、ordering、完整CFG和跨hart排他仍未证**，也不是codec instance token/BS或FB ring的AP授权。
- **guard exchange的标准窄fast path（新观察）**：完整 **1621c..16262，22/2**，**16220 LR.W a2,(a0) / 16224 SC.W a3,a1,(a0)**，word4B、aq/rl bits均0；标准16228 zero→1625e，1625e a2→a0、16260 ret，该成功窄路只有标准CSR/LR/SC/branch/move/ret。retry16254/16258再LR/SC，1625c失败→1622c；**1623a/16248** retry arithmetic/loop仍unknown，不声称完整进展/内存序。独立release **1613c..16170，18/3**，16142=`0fc00500` opaque ordering，16168标准store0到guard，诊断literal **1cef4 `illegal lock addr:%p`**来自captured bytes（邻接路径字符串不是ISA证明）。caller5696/56f0=`0f400a00`也仍unknown；不能用无aq/rl的LRSC单独当DMA/cache barrier。
- **701c必须面对真实vendor copy body（新观察）**：独立window701c..7148含body **701c..7144，97/23**及下一helper首4B；正式profile仅取body，不混邻接helper当返回。保存source到s1/destination到s2，7024同地址→713c，**7028 vendor length gate未知**；702c标准address direction branch→70a6。7078/70f8→**7144..7186，17/10**，visible block count>>5、threshold33/mask31；reverse arm70d4/70da标准byte load/store、710e/711c标准word load/store，但其它memory/loop raw大量unsupported。713c仅visible saved destination赋a0，opaque epilogue仍不假定保持寄存器。**不因memcpy标签、局部byte/word store或visible return证明全部72B copied、overlap/loop semantics、queue durability、AP DMA cache handoff或退休**。
- **确切资料/重放**：新原件frames/logs/exits、guard-preset-and-diagnostics.json及r2 previews位于 **`x9/out/hevc-queue-critical-source-20261116/`**；previews不能作为instruction boundary/完整CFG证据。最终 **`x9/out/hevc-queue-critical-final-20261116/`** syntax/profile-exact/真实firmware-entry `.exit`全部 **0**，16新spans逐exact-PC/raw/boundary匹配；旧12+新9 direct targets、旧5+新6 standard branches、6个新compressed branch、4 LR/SC operands/aqrl、CSR mask/restore、guard成员/原件preset及visible reverse stores独立核对。旧35和五份profiles完全不变、不重分旧帧；真实固定vcp_b.img经firmware()的51/40/33/142/62/81与direct profiles相等，所有runtime flags=false。helpers/source.diff/source-head.txt=`d4d5b811`/原log/JSON齐全。新唯一同级out复制两个extract helpers，依次 `python3 -I -B .../extract-profile-exact-entries.py` 与 `.../extract-firmware-entry.py`（exclusive-create）；原件rv32_slice.py/TCM base0/r2=/opt/homebrew/bin/r2，range为5664/68、56cc/54、1621c/46、1613c/34、701c/12c、7144/42（均hex），out新文件。
- **无新依据的输入路线已止步**：本turn另定向读取HEIF iloc reader和公开固定libheif v1.20.2 / GPAC v2.4.0 extractor，分别保存在 **`x9/out/hevc-iloc-implicit-source-20261116/`**（download logs/exit0）；它们仍未解决method0/1或非零offset+zero-length规范语义，和旧reference调查同类，**不是新格式成功/修复证据，不改任何HEIF gate或源码**。不要再次用这类extractor、旧搜索HTML、whole-source zero-offset特例猜剩余length；需真正规范或权威可辨输入证据。没有运行CLI/sample/人工向量。
- **CI/停止/下一准确stage**：`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，无CI run ID。无C/frontend/kernel/XNU/full-link/boot构建、测试脚本/套件、成功产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作或子代理，`.local -> ../../.local`未动，无后台句柄。下一可从新copy-body的**实际vendor memory/loop raw来源**或**5704 nesting清零gate/1621c retry**证明缺失ISA语义，并追真实queue consumer完整入场边界；只做精确新类别，不重复constructor3072e2已排除Andes weak masks/LLVM features，也不重追cb60 pointer-installer或1804 dump。不能以FW shared queue lock代替AP/codec publisher排他。constructor/cold最高authority、持续session_ops provider/完整DPB/layout/scratch、全部publisher/AP排他/durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT-CMPhoto-ImageIO及HDR/LUT/tone-map/chroma实效仍全部必做；**代码齐全后才出唯一最终boot**。

#### 2026-11-16 修正 worker receive 地址 / queue 消费端共享 critical（停止时最新交接）

- **本turn分类/固定源码**：progress；从 tracked 干净的 `7eafa757` 继续 `.worktrees/hevc-hw` / `feat/hevc-hw`。新源码 **`8352afdc`** 已 push 到 `origin/feat/hevc-hw`，不合 main。`vcp_artifacts.py` 的真实 `firmware()` 静态入口已接消费端，service-dispatch ABI4 **65 spans**；旧51 spans的 bytes完整不变，旧字段只有**明确纠错 `service1.worker_receive_helper: 5e08→6e08`**与profile版本改变，新增14 spans/两个静态对象/一个false runtime flag。transport-IRQ40/transport33/VDEC-IRQ142/FB62/mem81整份不变。没有新增可运行session provider或物理硬解结果；完整goal仍active，READY/authority/producer-exclusion/retirement/普通VT门全部保留，**不出boot**。
- **原件独立纠错（不是新函数猜名）**：旧完整worker frame的标准pair **327f02=`97f0cdff` / 327f06=`e78060f0`**，`327f02 + sign_extend(ffcdf000,32) - 250`清bit0得到 **6e08**，不是旧profile的5e08；service2 **32806a=`97f0cdff` / 32806e=`e780e0d9`**，同法加-610也得到6e08。两对AUIPC/JALR register/link/target已独立核对，复用旧DRAM完整frame，不重分旧窗。service1传a1=sp，header0/payload8；service2传a1=sp+8，header8/payload16，均a2=-1。不能以旧错误地址继续追5e08，也不因worker随后读header就认定receive返回成功或record完整。
- **真实nonempty消费顺序（新可见边界）**：新独立完整 **6e08..6f44，110 instructions/8 unresolved**。局部frame24B、timeout sp+20，queue/destination保存s0/s3；**6e64→5864**后6e68读count+56，6e6a zero→6ea4 empty路径。非empty6e6c读size+64，size0→6e86（可见跳过copy但仍减count）。size非零：6e70读cursor+12、6e72加record-size、**6e78先写cursor**，6e7a unsigned比较end+4、必要时6e80 wrap到begin+0，6e82把saved destination传a0、**6e84→701c**；a1是可见advanced/wrapped cursor。随后**6e86=count-1、6e8a visible result1、6e8c写count**。这与producer的copy→write-cursor→count顺序不同；不把member12误当member8，不把count减1当durable dispatch、AP ring消费、DMA/cache交接或逐buffer退休。701c未知memory/loop/寄存器保持与opaque epilogue仍全部保留。
- **empty/wait/wake仍有未闭边界**：6e48→**86b0** admission helper未知；6e2e/6e4e entry gates、6e9a sender-wake gate、6ea4 empty-timeout gate仍opaque。count减后6e8e读sender wait-list+16、6e96→8378、6e9e→57da；empty路径6eae→845a，6eb6→58cc，再6eba→5864，visible lock bytes+68/+69仅在255时写0；6eda→8480 timeout check、6ee2/6f04/6f16→6ba0 queue-unlock，6f10→82ee用receive wait-list+36和timeout，6f20→57da前读mhartid。retry6f2a nonempty→6e6c、6f2e timeout非零→6eb6；6f30/6f40 visible result0，6f36返回s2。**没有证明helpers的完整超时/调度语义、wake/admission、copy后的实际worker dispatcher执行或包不会丢失**。
- **task与ISR critical确实共享literal对象（新静态观察）**：消费端调用不同于ISR的 **5864..58cc（38 instructions/5 unresolved）** 与 **58cc..5928（33/8）**；task enter/exit标准mhartid读586a/58d2，shared base同为 **2000c**、guard/nesting/owner offsets0/4/8。588c/58ea→1621c以visible saved ra交换，589c同hart→58be递增、58b8/58c0可见owner/count stores；foreign路径58a2先释放再58b2等nesting，enter的58c8尾跳1613c。exit58f6比较owner、5900写nesting-1，**5902=`67462500`仍opaque**（与ISR5704相同raw，不是新ISA语义证明），visible5908/590c写0/-1。**5890/58ee=`0fc00900` ordering仍opaque**，task context/mask/restore分别调用4458、4632、46b8，尚未分析其完整作用，不能借用ISR的mie mask或宣称完整跨hart exclusion/coherence。shared firmware guard不授予AP/codec BS/FB publisher权威。
- **本turn承接的ISA候选已止步**：在 **`x9/out/hevc-vendor-memory-isa-source-20261116/`** 保存OpenHW CV32E40P固定commit **`b353438ff469c030c89f6804f3b24a2852880e8b`** 的tree/manual、download logs/exit0和`corev-exact-memory-encoding-comparison.json`。按该定义immediate load opcode0b/store2b与captured701c/7144 memory words逐exact比较：708a funct3=7不匹配、block forward/reverse operations均不匹配，某些apparent store反而匹配cv.lh。**该定义不能解释完整RV55 memory family，零散bit match不证明family/语义**；不改decoder、不称RV55=PULP、不再重复此弱匹配方向。没有执行样本或人工向量。
- **确切资料/重放**：新raw arithmetic、三个完整frames、r2 previews/logs/exits在 **`x9/out/hevc-queue-consumer-source-20261116/`**；preview不作独立boundary/CFG证据。唯一正式最终目录 **`x9/out/hevc-queue-consumer-final-20261116/`**：syntax/profile-exact/真实firmware-entry `.exit`全 **0**，14新spans逐exact-PC/raw/boundary匹配；2标准AUIPC/JALR、34标准direct targets、5标准branches、14 compressed branches及queue/guard成员、copy前cursor/count后写顺序已独立核对。其`span-framing-exact.json`明确记录旧helper纠错，不虚称旧51全字段不变。真实固定vcp_b.img经firmware()的65/40/33/142/62/81与独立profiles相等，runtime flags全false。两个extract helpers、source.diff、source-head.txt=`8352afdc`、JSON/log齐全；重放复制两个helpers到新同级out，依次`python3 -I -B .../extract-profile-exact-entries.py`与`.../extract-firmware-entry.py`（exclusive-create）。新frames用rv32_slice.py/TCM base0/r2=/opt/homebrew/bin/r2，start/bytes分别6e08/13c、5864/68、58cc/5c（均hex）。不运行测试脚本/套件/样本。
- **CI/停止/下一最早不确定stage**：`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` exit0/`[]`，无CI run ID。本轮只有Python syntax/source analysis，没有C/frontend/kernel/XNU/full-link/boot构建或成功产物二次检查，无设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作或子代理，`.local -> ../../.local`未动，无后台工具句柄。下一准确入口是**真实consumer的86b0 admission、task critical context/mask/restore4458/4632/46b8，及empty路径6ba0/845a/8480/82ee**；先根据已确认target确定原件函数边界，再独立分帧，不追旧5e08。701c vendor copy/5704与5902清零gate仍需新精确ISA来源，不重复已排除Andes/LLVM weak-mask、CORE-V family、cb60 pointer-installer、1804 dump或无新规范依据的libheif/GPAC implicit-length调查。constructor/cold最高authority、真实持续session_ops provider/DPB/layout/scratch、publisher/AP排他/durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT/CMPhoto/ImageIO及HDR/LUT/tone-map/chroma实效仍全部必做；**全部代码齐全后才出唯一最终boot，未称complete**。

#### 2026-11-16 consumer admission / task MIE nesting / timeout 与 queue-unlock（停止时最新交接）

- **本turn分类/活动源码**：上一goal turn是progress（receive地址纠错及交接`48276002`已push）；本turn从该tracked干净HEAD继续 `.worktrees/hevc-hw` / `feat/hevc-hw`。新源码 **`f4b2e2d3`** 已push，不合main。真实 `firmware()` 的 service-dispatch ABI5 **91 spans**，新增26 spans/3静态对象/1 false runtime flag；旧65整份字段/bytes只改变profile版本，保留上一轮明确纠正的6e08。transport-IRQ40/transport33/VDEC-IRQ142/FB62/mem81整份不变。新增内容是原件软件消费/调度边界证据，不是可运行session provider或物理硬解；完整goal active，READY/authority/producer-exclusion/retirement/普通VT门全部保留，**不出boot**。
- **admission不是codec完成状态（新观察）**：完整 **86b0..86de，15 instructions/2 unresolved**，读mhartid，visible literal **2433c**，opaque index86bc后load/branch86c2；zero arm86da visible result1。另一arm literal **24470**，86cc index仍opaque，86d0 load→86d2标准seqz→86d6 shift1，visible result只能0/2。不能把1/2当mailbox或硬解success，不能根据literal+mhartid猜stride。完整context **4458..4472，8/2** 使用literal **242c4**，4464 index/446c return gate仍unknown；完整 **89a0..89ce，15/2**使用2433c/2432c、visible pointer后member72 nesting load，无state arm返回1。实际table索引/current-task指针及完整context语义仍未证明。
- **task critical的实际MIE与task+72（新观察）**：完整 **890e..8964，29/6**，标准 **8910 csrci mstatus,8**，visible8936读+72、8938加1、893a写回；8920/8930 index和8940 gate未解，8944→4458用于后续assertion。完整 **8964..89a0，22/2**，visible8986检查+72非零、898e减1、8990写回，**8996 zero→899a csrsi mstatus,8**；没有假装它是ISR variant的saved-mie restore或每次无条件开中断。**task+72不是queue+72**，也不是codec instance token。函数索引仍opaque，static visible store/set不证明真实任务指针、完整CFG或原先interrupt状态保真。
- **wrapper还包含独立vendor CSR/shared counter与trace（新观察）**：完整 **4632..46b8，40/3**依次4634→890e、4638→10f0a、463c→89a0，4640 gate后可见high/low/high time采样（4666 retry）、11e7e conversion与mcycle/minstret snapshot，最后46b4尾跳10eee；完整 **46b8..4820，104/12**含46c0→10f0a、46c4→89a0、opaque trace gates/arithmetic，4812→10eee、**481c尾跳8964**。完整 **10f0a..10f24（9/2）**以shared literal **29580**/a1=1调用1609e；10f1a old-counter非零跳过，zero arm10f1c标准CSR指令清 **bd0 mask0x1**。完整 **10eee..10f0a（9/3）**以a1=-1调用1609e，**10efe gate未知**，visible10f02设bd0 mask0x1。CSR指令的bit操作可见，但**vendor CSR bd0功能/完整set条件、trace arithmetic、helpers ordering与runtime exclusion均未知**，不能称DMA/cache barrier。
- **shared fetch-add的窄标准fast path（新观察）**：完整 **1609e..160ec，24/2**，160a2 LR.W a6,(a0)，160a6 old+a1→a5，160aa SC.W a3,a5,(a0)，160ae成功→160e8旧值a6→a0、160ea ret；这条成功窄路全部标准指令。retry160da/160e2同为4B LR/SC、aq/rl=0，retry arithmetic/loop仍有2 unknown；不推完整进展/coherence或把shared counter当codec buffer retirement。
- **timeout是软件快照/剩余量，不是WAITISR/c006（新观察）**：完整 **845a..8480（12/2）** literal **24500/24478**，opaque hart indices后写snapshot0/4。完整 **8480..854a（66/10）**保存snapshot s0/remaining pointer s2，8490→5864/84c0→58cc；visible84b2读pointer对象+102、84bc清该byte、result1；flag0→84f8，再opaque84fc决定路径。可见851a elapsed subtract、851c unsigned compare、8524写remaining、8538/853c刷新snapshot并visible result0；8544 exhausted arm将remaining写0。**tick单位、wrap判定、无限等待、索引与完整branch语义未证明**，不能把task-like flag+102当codec完成、以remaining变化延长/删除硬件退休门。
- **queue-unlock不是全程持guard（新观察）**：完整 **6ba0..6c4e，53/9**，signed byte+69/+68及nonpositive gates，visible分别处理receive wait-list+36 / sender wait-list+16。queue+72 optional load6bba后可走6bc4→**6b02**（语义未读），另6bde/6c32→8378；vendor wake/loop gates未解，6bca/6c3a→854a。**6bec先写lock69=255，6bf0→58cc释放，接8条标准NOP，再6c14→5864 reenter，6c44写lock68=255**。这条visible release/reentry间隙不能当连续producer排他。完整 **854a..855c（6/1）**literal **244f0**、opaque index后visible store1，是pending-yield候选，非硬件IRQ ACK/退休。完整 **82ee..8330（22/3）**opaque current-task index后task list item+24，8322→**54ae**，a1=1后832c尾跳**7738**，尚未证明list/blocked-task完整生命周期及真正唤醒。
- **首次独立literal检查纠偏/最终资料**：初次source-exact helper在86c4 literal检查exit1，发现本turn草稿把ADDI0x470/0x478误写成24770/24778；已根据raw修成 **24470/24478**（只改未提交新字段，旧profiles不变）。原失败日志/exit保留为最终目录`profile-exact-preflight1.*`；不虚称所有尝试皆绿。15个新完整frames、previews和逐命令logs/exits在 **`x9/out/hevc-queue-scheduler-source-20261116/`**，`frame-extraction.log`保留每个start/bytes和正式rv32_slice.py参数；preview不能替代independent framing。唯一正式最终 **`x9/out/hevc-queue-scheduler-final-20261116/`**：最终syntax/profile-exact/firmware-entry/gh-runs `.exit`全 **0**，26新spans逐exact-PC/raw/boundary相等，**31 direct targets / 7 branches / 15 compressed branches / 4 LR-SC operands/aqrl**及MIE、BD0 mask0x1、task/queue成员、remaining/snapshot stores、8NOP已独立核。真实固定vcp_b.img经firmware()的91/40/33/142/62/81与独立profiles相等，runtime flags全false；旧65整份（除version）及五份旧profile完全不变、不重分旧frames。两个extract helpers、source.diff、source-head.txt=`f4b2e2d3`与JSON/log齐全；重放复制helpers至新同级out，依次`python3 -I -B .../extract-profile-exact-entries.py`和`.../extract-firmware-entry.py`，exclusive-create，不执行样本/人工向量。
- **CI/停止/下一最早边界**：gh branch run list exit0/`[]`，无CI run ID。本turn没有C/frontend/kernel/XNU/full-link/boot构建、测试脚本/套件、成功产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作或子代理；`.local -> ../../.local`未动，无后台句柄。下一从已确认targets **8378 waiter remove/wake、54ae list insertion、7738 blocked/delayed-task helper、6b02 optional queue helper**继续prove软件入场/退出；vendor address-index/gates与701c完整copy仍是真正缺口，不用FreeRTOS名字/相似源码、BD0 CSR编号或统计字段推全语义，不重试已排除ISA weak masks/CORE-V family、cb60 pointer-installer、1804 dump或旧implicit-length资料类。constructor/cold最高authority、持续session_ops provider/DPB/layout/scratch、publisher/AP排他/durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT/CMPhoto/ImageIO及HDR/LUT/tone-map/chroma实效仍全部必做；**代码全部齐全后才出唯一最终boot，未称complete**。

#### 2026-11-16 waiter / standard list unlink / optional queue-set record（停止时最新交接）

- **本turn分类/固定源码**：上一goal turn是progress（`f4b2e2d3`源码及`8fc7ed39`交接已push）；本turn从该tracked干净HEAD继续 `.worktrees/hevc-hw` / `feat/hevc-hw`。新源码 **`24f86bde`** 已push，不合main。真实 `firmware()` 的 service-dispatch ABI6 **106 spans**，新增15 spans/1静态对象/1 false runtime flag；旧91整份字段/bytes只改变profile版本，既有transport-IRQ40/transport33/VDEC-IRQ142/FB62/mem81整份不变。软件waiting/list边界更完整，但未新增实际session provider、排他或硬解结果；完整goal active，READY/authority/producer-exclusion/retirement/普通VT门全部保留，不出boot。
- **完整标准list窄边界（新观察）**：独立 **5496..54ae，12 instructions/0 unresolved** 为append，next/previous offsets4/8，54a4写item container+16、54aa增list count；独立 **552c..5552，17/0** 为unlink，5532/5538修双向links，553c检查list index、必要时5542修index，**5544只清item container+16，554c减list count、554e重新读count并return**。两整段逐标准RV32 encoding类别及所有memory operands已独立核，不仅依赖engine-accepted名字。**list container清零/count返回不是task退出join、停止codec producer或逐buffer DMA/free ACK**，runtime pointer/list integrity与exclusion仍未知。完整sorted insertion **54ae..552c，42/5**仍有54ba entry gate、未知epilogue；54d0→89a0 nesting、54fe unsigned walk，5508..5522可见links/container/count stores，不把局部标准尾部授予完整遍历/排他。
- **waiter从event/state list→ready/pending list（新观察/仍有gate）**：完整 **8378..845a，75/10**，837a list+12→head，837c item+12→owner(s0)，8394 owner+24、839a→552c remove event item；839e读owner hart byte+76，literal **24470** 后index83aa仍opaque。非zero arm83b8 vendor offset与pending list literal **244b8**；zero arm83c2 owner+4、83c8→552c remove state item，priority字段+44，可见ready-priority literal **24348**与ready-list literal **24350**，83e8 vendor offset未知（不能凭literal20猜完整list选择），83fe→5496 append。8418 unsigned比较selected/current priority，visible no-yield结果-1；yield arm842a向literal **244f0**经opaque index写1，842c返回selected hart byte。尾部delayed-list **24480**/next-wake **244e8** lookup与8454 store仍有unknown index。**没有证明实际task pointer、pending/ready选择、真实调度发生或packet持久消费**；priority不是buffer index/codec完成序号。
- **blocked/delayed-task路径不证明task已退出（新观察）**：完整 **7738..77ec，58/15**。传入delay保存s3、flag保存s2，literal **24478** tick与 **2432c** current task索引仍opaque；7762清对象+102、776a remove对象+4 state item。**776e/7772**决定indefinite/finite分支仍opaque；可见indefinite arm采用 **24490 + mhartid*20**（这一arm的标准MUL/ADD可见，不推广到其它vendor indices）、778e尾跳5496。finite arm7794 current+delay→s2，7798写item value+4，779c标准wrap比较，overflow-list **24488**/normal delayed-list **24480**分别尾跳/调用54ae，必要时77e2更新next-wake244e8。完整delay语义/tick单位/无限等待/实际task生命周期未知；不能用这些list changes补掉producer-exclusion或hardware retirement。
- **optional queue-set的source是handle栈字，不是重复service payload（新观察/限定）**：完整 **6b02..6ba0，56/6**，frame8B，**6b08把原queue handle写sp+4**，6b0c从原queue+72取queue-set handle，后者count+56/capacity+60；6b46 full arm→6b96，visible result0。nonfull **6b4a a1=sp+4，6b54→6a8c**，a2来自saved position，source以原queue handle word开头，与callback record/payload的stack位置不同。**实际queue-set record-size/701c完整copy未知，绝不称只复制4B或完整handle-only record**。lock69=127 assertion、255 unlocked path可尾跳8378唤醒receiver；另一arm6b8e increment、6b92写lock69。以上handle notification与72B service payload queue不同，但完整copy/admission/packet不会丢失与codec buffer状态仍未证明。
- **资料/最终独立结果**：6个新完整frames/previews/逐命令logs/exits在 **`x9/out/hevc-queue-waiter-source-20261116/`**，`frame-extraction.log`有每个start/bytes与正式rv32_slice.py/TCM base0/r2=/opt/homebrew/bin/r2参数。正式唯一最终 **`x9/out/hevc-queue-waiter-final-20261116/`** 的syntax/profile-exact/firmware-entry/gh-runs `.exit`全 **0**，15新spans逐exact-PC/raw/boundary相等，**15 standard direct targets / 12 standard branches / 6 compressed branches / 29完整标准list instructions**及item/queue成员、container/count、原handle栈source和literal地址独立核对。真实固定vcp_b.img经firmware()的106/40/33/142/62/81与direct profiles相等，runtime flags全false；旧91整份（除version）及五份旧profile完全不变，不重分旧窗。helpers/source.diff/source-head.txt=`24f86bde`/JSON/log齐全。重放复制两个extract helpers到新同级out，依次`python3 -I -B .../extract-profile-exact-entries.py`与`.../extract-firmware-entry.py`（exclusive-create），不执行样本/人工向量。
- **CI/停止/下一最早实际codec边界**：gh branch run list exit0/`[]`，无CI run ID；无C/frontend/kernel/XNU/full-link/boot构建、测试脚本/套件、成功产物二次检查、设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作或子代理，`.local -> ../../.local`未动，无后台句柄。本轮已沿实际consumer闭合其直接waiter/list/queue-set窄callee；**不要为凑静态profile继续无关联扩张其它RTOS helper**。下一回到真正codec IPC/object flow：复用已有完整generic sender **30507a..3052fc** 的 **3051f2/30523a→6e08**，核实际reply queue/record与service1 D000→1f878的关系、reply字段/状态与allocator callback对象链；或出现精确新ISA来源后再解constructor3072e2与publisher owner lifetime。不是再次分帧/猜旧5e08、重试Andes/LLVM/CORE-V weak-mask类、cb60 pointer-installer、1804 dump或无新规范证据的implicit-length资料。cold最高authority、持续session_ops provider/完整DPB/layout/scratch、publisher/AP排他/durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT/CMPhoto/ImageIO及HDR/LUT/tone-map/chroma实效仍全部必做；**全部代码齐全后才出唯一最终boot，未称complete**。

#### 2026-11-16 codec reverse reply：VDEC／VENC 队列身份与 payload word1（停止时最新交接）

- **活动树／本turn实际交付**：从已push且tracked干净的`51863ffc`继续`.worktrees/hevc-hw`／`feat/hevc-hw`，没有合main或开子代理。新提取器源码 **`9530eac6b48b773e134ac22cb834076ed9b12f1d`** 已正常push（`51863ffc..9530eac6`，push-code.exit0）。真实`firmware()`的service-dispatch升 **ABI7／113 spans**：新增7条VENC身份/注册/record窄spans与1个false runtime flag；旧106整份只改变版本并增加新对象。mem-wait仍 **81 spans**，明确增加receive layout与未证明的incoming-length copy bound，**不是声称该整份profile完全不变**；原字段未纠错/重命名。transport-IRQ40／transport33／VDEC-IRQ142／FB62整份不变。完整目标active，本批没有实际provider或硬解能力交付，READY/authority/producer-exclusion/retirement/普通VT门全部保留，不出boot。
- **两条generic receive已按原raw分开（观察）**：复用完整`30507a..3052fc`，不重分旧窗。标准 **3051e2 LUI20／3051ea LW -1928 = cell1f878**，**30522a LUI20／305232 LW -1912 = cell1f888**；标准pairs **3051ee/3051f2**与 **305236/30523a**均独立算得 **6e08**，a1=sp+8、a2=-1。service1原callback116f2的 **D000 branch11734→base1f864+20→cell1f878→11746/6c4e** 与第一receive一致。**1f888不是service2的1f874**，不能按receive顺序/service编号或physical channel归属猜测。
- **第二cell的精确新producer身份（观察／不是runtime注册）**：新增完整 **TCM1192c..11a68，97 instructions／5 unresolved**，**11a68..11aec，46／5**。descriptor **20bb8** 的六word为`[5a5a5a5a,1c950,1192c,328136,0,328384]`，name=`venc_service`；原DRAM pointer cell **3389ec→20bb8**，表中顺序实际是service1／service3／service2，不是编号顺序。initializer **11956 a0=3／11958 a1=11a68／11964→30502c**，11974存dispatcher **30eeb4→29b60**。base **1f864**，119a0／119be调用 **660a**，均entries2／record72B；**119ac存base+32=1f884**，**119c6存base+36=1f888**，独立semaphore为base+40=1f88c。只证明捕获静态initializer与同cell，不授予当前table有效、task创建成功、VENC能力或任何VDEC/HEVC权限。
- **VENC callback与VDEC的栈record不同（观察）**：callback11a68 frame80B，**service sp0／length sp4／payload sp8／wake sp76**；visible limit64，**11a80** copy-length vendor op仍opaque，**11a86→701c**完整copy仍未解。message maskff000，**11a9c type1000→base+32**；非该type经 **11aa2 type4000→base+36**，其余走log；**11ab0 load queue／11ab2 a1=sp／11ab4 a2=sp+76／11ab8→6c4e**。isr result gate11abc与wake gate11ade未知，11ae2→57da不当IRQ/producer退出。该record与service1的sp8／payload16不同；同为72B初始化参数不证明完整record admission/copy或实际queue排他。
- **word1不是本codec ABI的status（撤回候选假设，不是行为修复）**：两条receive分别 **3051f8／305240读取sp16=payload word0**，**3051fa／305242读取sp20=payload+4=word1**。既有RV55 INIT header与当前native c005/d005 parse/encoder一致：message0、**context4／AP token8／status16**；因此本turn开始的“word1可能是status”候选不成立，**保留`received_context_loads`**，新增neutral stack/word offsets和仅限known VDEC ABI的说明；不把VDEC布局推广为已证明VENC header ABI。尤其context load不是status validator，更不是token匹配。**3052b2..3052bc**仍以原s3／s2／s6复制回caller，原bytes而非已证明受incoming length限制；完整vendor gates/701c/状态检查仍false。c005 callback先复制返回三组u64，再检查wrapper结果，wrapper3052f6 literal0及opaque epilogue仍不代表generic/回复status成功；c006仍无free ACK。
- **最终资料／独立源码分析结果**：原件限定两个固定payload，半字候选scan只用于定位literal，候选本身不证明边界/寄存器活性。新窗/previews/logs/exits/candidate JSON在 **`x9/out/hevc-codec-reply-source-20261116/`**；正式唯一最终 **`x9/out/hevc-codec-reply-final-20261116/`** 的syntax／profile-exact／stack-pointer／firmware-entry／gh-runs `.exit`全 **0**。新7spans与复用6个generic mem spans逐exact-PC/raw/boundary相等，**14 standard direct targets／18 memory operands／4 standard branches／30 immediate operands＋6 stack-pointer operands**独立核；descriptor／字符串／startup pointer raw及当前native header offsets已核。第一次helper preflight因误写dict key`alloc`而KeyError exit1，仅分析helper错误，原失败log/exit保留；修成实际`allocation`并加C.ADDI16SP解码后成功，未改来源bytes/未执行向量。真实固定vcp_b.img经`firmware()`的113/40/33/142/62/81与direct profiles相等，runtime flags全false。source.diff/source-head.txt=`9530eac6`与3个helpers齐全；在新同级out复制helpers，依次运行`python3 -I -B .../extract-profile-exact-entries.py`、`.../extract-stack-pointer-operands.py`、`.../extract-firmware-entry.py`（exclusive-create），不运行测试脚本/套件/样本。
- **CI／停止／下一实际codec边界**：gh branch run list exit0／`[]`，无CI run ID。没有C/frontend/kernel/XNU/full-link/boot编译、成功构建产物复检、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作；`.local -> ../../.local`未动，无后台句柄。不继续扩张VENC/RTOS旁支来凑profile；第二receive身份已解，下一沿 **service1 D005→第一receive→payload16真实status/allocator result→88B对象**与实际codec owner接线继续，只有新精确ISA依据才解释3051fc等vendor gates/3072e2，不能凭context或队列操作写恒成功验证器。cold最高authority、持续session_ops provider/完整DPB/layout/scratch、publisher/AP排他/durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、规范HEIF属性/implicit lengths、普通VT/CMPhoto/ImageIO及persistent/scoped HDR/LUT/tone-map/chroma实效仍全部必做；**代码全部齐全后才出唯一最终boot，未称complete**。

#### 2026-11-16 HDR sequence 规范取得（续接里程碑，尚无源码 delta）

- **基线/范围**：从已push的`c7f9ab90`继续活动树`.worktrees/hevc-hw`/`feat/hevc-hw`。复用service1 allocation完整帧确认`308b64/308bb0`读sp48即payload16 status，但完整成功分支仍opaque，不据此制造validator或扩张RTOS profile。真实普通VT `decode_frame()`未接sample_hdr、state未保存HDR；按此实际前端缺口转向规范驱动的代码工作。
- **新的权威输入**：官方ITU-T H.265 V9 (09/2023)，718页/12,066,538 B，原件和提取文本在`x9/out/hevc-hdr-sequence-source-20261116/`。官方index `https://www.itu.int/rec/T-REC-H.265/en`，PDF `https://www.itu.int/rec/dologin_pub.asp?lang=e&id=T-REC-H.265-202309-S!!PDF-E&type=items`；该版本已superseded所以后缀S。错误I链接返回HTML与最初`python -I`导致pypdf不可见的失败日志均保留，不混作成功证据；实际离线提取使用`python3 -B`用户site的pypdf，不增加构建依赖。
- **规范观察/不能混用**：D.3.28要求mastering消息若CLVS中存在则首picture必须有，沿decoding order持续到CLVS结束且全CLVS重复一致；D.3.35 CLLI作用于整个CLVS且重复一致，**无首picture必须存在条款**。D.3.1将137/144列为VCL-associated，direct SEI TemporalId须等于AU/VCL，而非硬编码0。3.30/3.31及8.1的CVS边界是IRAP且NoRaslOutputFlag1：IDR/BLA必是，CRA只在bitstream首picture/EOS后或外部HandleCraAsBla1；不能用slice no_output或每个CRA无条件清HDR。7.4.2.4.4允许VCL之间的prefix，D.3.1另要求同type先在firstVCL前出现，不能一概拒绝合法mid-picture重复。suffix的137/144含义还需D.2.1实际分支核定。
- **未完成/下一步**：这批只有离线原件/规范阅读；尚未修改parser/VT、编译或CI。下一接真实完整AU facts、direct SEI与TemporalId/placement，再实现SUBMIT-success-only的prospective序列状态和逐token metadata；cancel/fault/new-session清除、普通DRAIN保持；同CLVS冲突不能last-wins，晚到CLLI不能用伪首帧限制掩盖已输出帧的回溯缺口。保留所有硬解/注册门。无测试/样本/人工向量、设备/MMIO/SMC/mailbox/分区操作、子代理或boot。完整目标仍active。

#### 2026-11-16 普通 VT direct HDR forward state（代码里程碑，完整目标未完成）

- **真实入口 delta**：shared `x9_hevc_sample_metadata()`先验证完整AU/catalog，再原样解析direct prefix137/144；返回picture NAL/TemporalId/PPS、slice no_output及实际EOS/EOB。`x9_hevc_sample_hdr()`复用该路径，HEIF不另造解析器。`decode_frame()`使用新metadata→prospective SEI-only sequence→owned CM description，**只有native SUBMIT成功才提交状态/逐token快照**；失败释放未转交snapshot，不污染后续状态。RECEIVE仍用原token description，cancel/fault/new-session清状态，普通DRAIN不清。CM/container值保持picture-local，不变成SEI持久状态；与当前/继承SEI重叠的24/4 B必须exact一致，否则SUBMIT前拒绝。
- **规范驱动门/重要纠正**：ITU D.2.1的suffix137/144属于`reserved_sei_message`而非HDR，完整长度/EBSP消费但不提升为HDR。direct prefix的TemporalId必须匹配实际VCL；合法mid-picture重复只有在firstVCL前已有同type时接受；D.3.1每个HDR type同picture至多8份identical结构。AU统一拒绝重复/非首AUD、firstVCL前suffix/filler、lastVCL后prefix、EOS/EOB后的非法NAL，并保留合法mid-prefix与普通路径mid-suffix；standalone保留原较窄prefix-before-all-VCL域，未声称全SEI语义已验证。**PDF page59/7.3.2.6/.7实际EOS/EOB RBSP为空，不含trailing_bits**；初稿假设trailing0x80在规范核对时撤回，最终接受恰好2 B的EOS/EOB header，且7.4.2.2 TemporalId0。legacy IDR函数自身没有改动。保留现有in-band PS限制，未声称覆盖所有合法AU布局。
- **序列语义/边界**：first input须IRAP，EOS后next input亦须IRAP；IDR/BLA开始新CLVS，普通middle CRA即使slice no_output1也不清状态，无外部HandleCraAsBlaFlag。D.3.28 mastering首picture缺席则同CLVS后续新增拒绝，已存在则exact持久/重复一致；CLLI没有伪首picture限制，晚到允许但同CLVS已有值冲突拒绝。EOB后该bitstream不再接新picture。新CLVS不继承旧SEI，旧pending token仍保留自己的描述。
- **standalone/输出**：`x9_hevc_copy_hdr_description()`复制全部原CM extensions，只merge实际present SEI，不修改source或像素；原CM缺席与SEI缺席分开。`X9HEVCCopyHardwarePicture()`独立从original AU构造expected enrichment，再readonly比较真实callback HDR，避免SEI-only input因原CM缺少HDR误报，绝不补写borrowed pixel使验证通过。attachment/ICC/CICP/chroma/tone-map实现不借此宣称实效。
- **诚实限制/manifest**：本批是**direct forward persistence**，晚到CLLI只用于当前/未来输入，不能回写早先pending快照或已经emitted的pictures；整CLVS回溯/容器历史冲突协调及nested scope仍未完成。因此`hevc_persistent_hdr_sei_state=False`保留，另列`hevc_direct_hdr_forward_state=True`和late_clli_retroactive/nested/runtime false，所有READY/provider/authority/AP排他/retirement/普通VT注册门保留。service113/mem81等firmware证据profiles本轮不改，不构建固件/boot。
- **固定提交/正式结果/重放**：源码 **`d805b47e`** 已push到`origin/feat/hevc-hw`（`c7f9ab90..d805b47e`，push-code.exit0），未合main。正式ignored目录 **`x9/out/hevc-hdr-forward-final-20261116/`** 保存source-head.txt/source.diff、analyzer/native helpers及原log/exit。五个真实source单位（bitstream、VT decoder及include、picture、metadata、HEIF CoreMedia）Xcode iOS arm64 `--analyze -Wall -Wextra -Werror` exit0，无diagnostic；固定源码 **`python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/hevc-hdr-forward-final-20261116/frontend`** 完整dylib/client正常编译/链接/签名 **exit0**。随后 **`python3 -I -B x9/out/hevc-hdr-forward-final-20261116/compile-native.py`** 真实Builder关闭ccache仅`sweet_x9_vdec.o` **exit0/60 warnings**（包含declaration-after-statement/cast-align；不是零warning），kernel/result.json及build.log保留。重放需把helper的out改为新唯一目录，不覆盖这次成功输出。gh branch run list exit0/`[]`，无CI run ID。没有全kernel/XNU/boot/固件构建、样本/测试/人工向量或构建成功产物二次检查；设备/MMIO/SMC/mailbox/分区未动，无子代理/后台句柄，`.local`链接保持原样。
- **停止前边界/下一动作**：这批交付的是实际VT入口forward HDR代码与正常编译，**不是物理硬解/整CLVS HDR验收**。晚到CLLI仍须解决同CLVS已提交/已输出metadata的范围与来源历史；不能通过给CLLI加伪首picture要求、把middle CRA或DRAIN当序列reset、修改borrowed callback pixel或取消原token绑定来“解决”。完整provider/cold authority/DPB/layout/scratch、publisher/AP排他/durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、其余HEIF属性/implicit lengths、普通VT/CMPhoto/ImageIO与scoped HDR/LUT/tone-map/chroma实效仍全部必做。保持目标active，只在全部代码齐全后出唯一最终boot；不重跑未改源码的成功对象或回到已排除ISA/RTOS/implicit-length旧路线。

#### 2026-11-16 HDR late CLLI：CLVS 身份、pending 原子回填与已接受 CM 来源历史（当前交接）

- **基线/上轮分类/范围**：从已push且tracked干净的`bc341bd4`继续活动树`.worktrees/hevc-hw`/`feat/hevc-hw`。上一轮`d805b47e`是实际VT代码+正式编译交付，属于progress；本轮继续它的真实晚到CLLI缺口，不用静态profile扩张替代完整目标。复核ITU PDF404/D.3.35整CLVS作用域及重复一致，未新增伪首picture要求。当前Xcode SDK `CMFormatDescription.h`明确`CMVideoFormatDescriptionCreate` deep-copies extensions，因此不把已有CM snapshot误当可变浅引用，不做无依据的重复深拷贝“修复”。这两份实际条款摘录保存在新目录scope-inputs.txt；不是设备运行证明。
- **真实序列身份/原token不丢失**：shared sequence增加local uint64 `clvs`：fresh首IRAP=1，只在既有genuine新CLVS门递增，middle CRA/no_output与DRAIN不递增；非zero identity与started一致，计数overflow在SUBMIT前失败。没有新增硬件token/POC或私有ioctl ABI字段。每个pending保留original native token加当时clvs，pending_take照原样携带；旧CLVS在新IDR/BLA入场后仍使用自己的immutable description，绝不用后一个CLVS的global HDR覆盖。
- **late CLLI准备→SUBMIT→提交链**：当current CLVS之前尚无SEI CLLI而新AU带有它，`video_hdr_prepare_pending`只对同clvs、仍pending的原tokens构造owned replacement descriptions。保留各token自己的ICC/VUI/非HDRextensions与原CM HDR，重叠HDR仍exact相等；不改原CM或任何borrowed/emitted pixel。固定`X9_VDEC_MAX_PENDING=32`槽plan全部在SUBMIT前分配/验证，任何中途失败释放全部未发布replacement和current snapshot，不改已有pending/sequence/history。native SUBMIT成功后在同一session lock内一次提交sequence/history/全部replacement/current token，再执行原collect/emit；failure/fault/callback-invalidate释放路径仍由原生命周期处理，无成功ACK当frame。
- **已接受来源历史不是事后last-wins**：`x9_hevc_read_hdr_description`从原CM/container而不是SEI-enriched snapshot读exact24/4 B，两字段全验证后才发布raw值，失败输出全清。current CLVS的CM light history只保存一个4 B代表值+seen/mixed flags，覆盖已接受inputs（包括已emitted的），不无界保存frames。CM-only值不同在尚无CLVS SEI时仍可接受；晚到/已有SEI CLLI若不能与全部原来源一致，在当前SUBMIT前拒绝。缺席CM不是冲突约束；不得把它伪造为0 B/0值或SEI来源。CMhistory同样只在SUBMIT成功后提交，new CLVS/cancel/fault/new-session清除，DRAIN保持。mastering的原首picture/持久exact门不变。
- **诚实限制/剩余路径**：这批补上“仍pending、同CLVS”的回填和已接受来源冲突历史，**不是对已emitted pixels的完整回溯**。现有满队列入场backpressure collect可能在新AU解析/接受前输出老帧；也可能有早先非temporal同步DRAIN输出。不能假装这些帧仍在pending、延长hold到整个CLVS、放宽native capacity、修改borrowed callback pixel或取消同步VT语义来掩盖。manifest新增pending_backfill=True与bounded CM history说明，`hevc_persistent_hdr_sei_state`/late_clli_retroactive/nested_scope/runtime仍false。完整scope/output契约、nested HDR和真实provider/DPB/retirement/HEIF/色彩实效仍未完成；READY/authority/AP排他/普通VT注册门全保留，不生成boot或做设备动作。
- **固定源码/正式结果/重放**：源码 **`c2f08e44`** 已push到`origin/feat/hevc-hw`（`bc341bd4..c2f08e44`，push-code.exit0），未合main。新唯一ignored目录 **`x9/out/hevc-hdr-clvs-pending-final-20261116/`** 保存source-head.txt/source.diff、analyzer/native helpers、scope-inputs与原log/exit。bitstream/VT decoder含sessions include/picture/image metadata/HEIF CoreMedia五个source单位iOS arm64 `clang --analyze -Wall -Wextra -Werror` **exit0，无diagnostic**（之后仅澄清一处comment）；固定提交 **`python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/hevc-hdr-clvs-pending-final-20261116/frontend`** 完整dylib/client正常编译/链接/签名 **exit0**。随后 **`python3 -I -B x9/out/hevc-hdr-clvs-pending-final-20261116/compile-native.py`** 真实Builder关闭ccache仅`sweet_x9_vdec.o` **exit0/60 warnings**，kernel/result.json/build.log保留；没有重编未改源码的其它成功对象。重放使用新唯一out，修改native helper硬编码路径，禁止覆盖成功产物。gh branch run list exit0/`[]`，无CI run ID。无测试/样本/人工向量、成功产物二次检查、全kernel/XNU/boot/固件构建/设备/ADB/mailbox/MMIO/SMC/分区动作，无子代理/后台句柄，`.local -> ../../.local`未动。
- **下一实际边界/完整目标**：本批把原forward代码推进到same-CLVS pending回填与已接受来源冲突协调，但**并未完成已输出帧上的整CLVS metadata契约**；下一核定VT/CM输出如何表达晚到CLVS-wide信息及其完成依据，不靠更改已有pixel/伪首picture门/无界hold掩盖，再继续真实cold最高authority/持续provider、DPB/layout/scratch、durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem与其余HEIF/普通VT/CMPhoto/ImageIO/scoped HDR/LUT/tone-map/chroma实效。已排除ISA/RTOS/implicit-length旧路线没有新依据不重试。整个HEVC/HEIF全部代码目标仍active，**只有全部齐全后出唯一最终boot**；源码/编译绿色不是物理硬解验收。

#### 2026-11-16 HDR scalable nesting：接续未提交实现与规范边界（当前交接）

- **接续状态/范围**：活动树`.worktrees/hevc-hw` / `feat/hevc-hw`，已push基线`66ad6407`。接续时五个tracked源码文件已有scalable-nesting修改，尚未提交；新目录`x9/out/hevc-hdr-scalable-final-20261116/`已有六个真实source单位analyzer日志及exit0，无diagnostic。**本批正常frontend/native编译尚未执行**，不能套用上一批编译结果。本轮先记录，再复核logical parent/child bounds、selected VPS scope、last-nesting/trailing及CFNumber释放，随后固定源码并正常编译。
- **新代码/非runtime证据**：shared parser在canonical EBSP reader上增加logical RBSP cursor、独立child payload限界；解释D.2.24/D.3.24 layer scope与operation layer-set union，operation查询实际picture PPS→SPS→VPS prefix，不用config代表SPS，也不宣称完整VPS/HRD/extensions验证或multilayer硬解能力。direct/nested适用base HDR共用exact24/4、agreeing repeats<=8和firstVCL前同type placement门；非base scope不提升为HDR。新增direct/layers/operations位来源，HEIF item info记录它们，CM/container reader保sources0，sequence按原SUBMIT-success-only接线保留来源。其他nesting families/unknown payload semantics仍未解释。
- **输出契约/依据**：既有官方H.265 V9原件及D.3.24摘录仍在`x9/out/hevc-hdr-sequence-source-20261116/`，本批新增vps-nesting-prerequisite/vps-layer-syntax摘录。Xcode SDK明确两decode flags都clear时callback应在DecodeFrame返回前执行，不能无限hold整CLVS；PropagatePerFrameHDRDisplayMetadata仅per-frame propagation，GeneratePerFrameHDRDisplayMetadata需iOS17，不能当iOS16 static CLLI已输出帧回溯接口。仅核这些公开SDK契约，不据此断言任何private接口不存在；已emitted整CLVS metadata契约仍未完成。
- **保持的边界/下一动作**：scalable scope能力单列，完整persistent HDR、late CLLI emitted回溯、全nesting/runtime仍false。完整cold最高authority/持续provider/DPB/layout/scratch、producer排他/durable dequeue/cache/逐buffer退休/c006 ACK、NV12/P010/HYFBC、aux-alpha/Rext/prem、其余HEIF属性/implicit lengths、普通VT/CMPhoto/ImageIO与LUT/HDR/tone-map/chroma实效仍全部必做。READY/authority/producer-exclusion/retirement/普通VT注册门保留；完整目标active，无boot/设备/测试/样本/人工向量/子代理，不合main、不动共享`.local`。实际提交、正常编译及gh结果在收尾补记，不提前报成功。
- **本批复核与固定源码**：已复核D.3.1两份type名单/sole-active和timing混合、D.3.24 default operation的MaxTemporalId取当前NAL、indexed operation上限、all_layers隐含MaxTemporalId6、VCL-associated union及last-message限制；未把default operation偷改成6以放行HDR。payloadSize用logical RBSP bits，对每个child设独立end，canonical escape仍由原reader读取，outer末尾独立消费trailing bits；parent不能藏reserved extension/额外outer消息。HEIF新增两CFNumber在任一分配失败及正常发布后均释放，dictionary保自有引用。源码提交 **`afd2a983fea9e6d22682aed0763e53797f5629dc`**（`afd2a98`）；source-head.txt/source-commit.txt/source.diff已固定在本批out，不回写旧批材料。
- **真实正常编译/日志/重放**：上述固定源码 `python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/hevc-hdr-scalable-final-20261116/frontend` 完整iOS dylib/client编译、链接、签名 **exit0**，frontend.log/frontend.exit保留。真实Builder关闭ccache、仅编`sweet_x9_vdec.o`的 `python3 -I -B x9/out/hevc-hdr-scalable-final-20261116/compile-native.py` **exit0/66 warnings**，native.log/native.exit及kernel/build.log/result.json保留；新增warnings为shared C declaration/style，未放宽编译选项。接续前六source单位analyzer **exit0，无diagnostic**，接续仅澄清header comment再固定提交。重放须选新唯一out并改native helper硬编码路径，不覆盖成功目录。无成功产物解包/hash/反汇编/重建比较，无测试/样本/人工向量、全kernel/XNU/boot/固件构建或设备动作。gh feature run list **exit0/`[]`**，无CI run ID；frontend句柄14353/native正常终止/gh4087已terminal，无仍运行的编译任务。
- **下一实际工作/不可冒充完成**：本批已把direct及resolved-scalable HDR接到同一SUBMIT-success-only forward/pending状态，**不是整CLVS emitted回溯或硬解验收**。下一仍需核晚到CLVS-wide信息如何在真实VT/CM输出契约表达、其他scoped HDR语义和持续native provider链；不能改borrowed pixels、伪首picture gate、无限hold或放开READY绕过。全体硬件authority/producer-exclusion/retirement/普通VT及其余HEIF/色彩缺口仍按上条全部保留。feature代码push及最终handoff提交结果在下一条记录，不合main，不出boot；目标保持active。
- **实际代码push/停止前状态**：`git push origin feat/hevc-hw`已成功 **`66ad6407..afd2a983`**，push-code.log/push-code.exit0保留，句柄23468已terminal；main、其它worktrees及共享`.local`均未改。native原日志分类为62 declaration-after-statement与4既有cast-align warnings，不把warnings抹成零或视为硬件证明。本条随正常handoff提交push，无新增功能源码，固定源码/编译仍以`afd2a98`为准；工作树保留，下一只从未闭合实际路径继续，不重复旧成功编译或已排除分析类。

#### 2026-11-16 Ambient viewing environment：direct/scalable SEI148到真实CM/CV路径（当前交接）

- **基线/本轮分类**：活动树`.worktrees/hevc-hw` / `feat/hevc-hw`，起点`d93cb0f` tracked干净、上一轮源码`afd2a98`及正式编译属于progress。本轮不是重复旧成功构建；补当前生产路径未解释的SEI148 nominal ambient viewing environment，不放开任何硬件准入门，不合main、不改共享`.local`，无子代理/测试/样本/人工向量/设备动作。
- **原规范/SDK新依据**：从已有官方H.265 V9 PDF离线仅提D.2.38/.39与D.3.38/.39所在PDF327–328/405–406，结果`x9/out/hevc-ambient-source-20261116/itu-h265-ambient-alternative.txt`及extract.log/exit0。D.3.39要求若CLVS任一picture有ambient且首picture为IRAP，则首IRAP必须有；沿decoding order持续至CLVS结束、同CLVS重复完全相同。illum u32不能0、xy各u16范围0..50000，单位0.0001lux/0.00002保原字节。它不提供保持creative intent的颜色变换。Xcode SDK已有iOS15的CM/CV AmbientViewingEnvironment exact8 CFData key，故可接iOS16；SEI147 AlternativeTransferCharacteristics另有iOS12 CM key和preferred transfer规则，**本批尚未实现147，不用ambient代替它**。
- **当前源码改动/尚未编译**：shared parser对direct/resolved-scalable base-prefix148加入exact8、numeric、agreeing<=8、firstVCL前同type和正确TemporalId/scope门，suffix148按D.2.1 reserved不提升。ambient presence/来源与mastering/light独立；shared shape/来源/ambient数字validator被parser、sequence及CM/HEIF真实入口调用。sequence复用原genuine CLVS门，ambient与mastering同首IRAP/持久exact规则，原VT SUBMIT-success-only提交/逐原tokenimmutable snapshot/新CLVS与cancel/fault清除/DRAIN保留自然携带新字段，无伪lateambient回填。CM raw-reader/copy/HEIF extensions及item provenance、CM format建立后的identity gate、owned像素metadata和read-only callback identity扩至24/4/8；全部分配/冲突检查后发布。converted RGB的source raw留Items，target不伪传source nominal environment为destination条件；没有修改borrowed pixel或加默认tone mapper。
- **下一实际动作/完整边界**：当前九源码文件改动尚未提交，**没有本批analyzer或正式frontend/native结果**；下一复核三字段数组/CF释放/validator与实际VT decode/HEIF调用链，再固定提交，运行真实source analyzer和正常编译，记录新唯一out及gh/push结果。原complete HDR/late-emitted CLLI/其他nesting、147/149与tone-map/LUT/chroma实效仍未完成；cold最高authority/持续provider/DPB/layout/scratch、producer排他/durable dequeue/cache/逐buffer退休/c006 ACK、linear NV12/P010/HYFBC、aux-alpha/Rext/prem、其余HEIF/普通VT/CMPhoto/ImageIO仍全部必做。完整goal active，只有全部代码齐全后才出唯一最终boot；本批没有boot或物理硬解证据。
- **源码复核/analyzer里程碑**：确认真实`vt_video_sessions.inc:206→216→231→240`仍是AU事实→prospective sequence→owned merged description→SUBMIT成功才提交→original-token pending；148无需CLLI-style回填或额外emitted history，因为合法首IRAP已有它，后来缺席沿sequence持久。`vt_picture.c:87→131`的explicit硬解expected/callback readonly identity与HEIF实际sample/catalog也消费三字段，native借用source view只继承原pixel attachments，converted target才清source nominal environment。三数组长度及CFData/CFNumber失败/成功释放已复核；source型validator不宣称完整色彩conformance。新唯一`x9/out/hevc-ambient-final-20261116/`保存SDK原header摘录、analyzer/native helpers；七真实source units（新增heif_native）iOS arm64 `clang --analyze -Wall -Wextra -Werror` **exit0，无diagnostic**，句柄77602已terminal。下一固定代码commit后正常frontend和native单对象编译，不沿用上轮对象；此时尚无本批正常构建结果。
- **固定源码/实际正式编译**：代码 **`7ab8e2c48f2f6b5de4fed9986be5cbbf2857f739`**（`7ab8e2c`），source-head.txt/source.diff固定在本批out。`python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/hevc-ambient-final-20261116/frontend`完整iOS dylib/client编译、链接、签名 **exit0**；`python3 -I -B x9/out/hevc-ambient-final-20261116/compile-native.py`真实Builder关闭ccache仅编`sweet_x9_vdec.o` **exit0/66 warnings（62 declaration-after-statement+4既有cast-align）**。frontend.log/frontend.exit、native.log/native.exit及kernel/build.log/result.json保留，没有额外解包/hash/反汇编/重建比较或执行测试/样本/人工向量。重放须新唯一out并改native helper的固定路径，不覆盖当前成功目录。frontend29913/gh16358已terminal，native编译正常结束；gh feature run list **exit0/`[]`**，无CI run ID。未构建full kernel/XNU/boot/固件，也未操作设备。
- **下一具体缺边/目标仍active**：D.3.38已给147真实规则：preferred_transfer若异于selected SPS transfer，decoder should用preferred；同首IRAP/持久/同CLVSexact，SDK iOS12有标准CM AlternativeTransferCharacteristics key。下一接raw优选值、correct scope与sequence，再沿真实CM description/HEIF source_color/VTPixelTransfer区分raw SPS、显式container/ICC policy和effective interpretation，不只加一个未消费field或手改borrowed pixel，也不拿148 nominal environment代替147/颜色变换。已emitted晚到CLLI、149/其他nesting/完整tone-map与LUT/chroma效果仍缺；同时cold最高authority/持续native provider/DPB/layout/scratch、BS/FB独立producer排他/durable dequeue/cache/逐buffer退休/c006 ACK、linear NV12/P010/HYFBC、aux-alpha/Rext/prem、其余HEIF属性/implicit lengths/普通VT/CMPhoto/ImageIO全部继续。所有READY/authority/retirement/普通VT门原样保留，没有最终boot；仅源码与编译完成，不称HEVC/HEIF可用。
- **实际代码push/收尾**：`git push origin feat/hevc-hw`成功 **`d93cb0fa..7ab8e2c4`**，push-code.log/exit0保留，句柄67582已terminal，无后台编译/分析任务。本交接随正常NOTES-only提交push，固定源码及编译证据仍指`7ab8e2c`。活动树与全部本地原件保留，不合main、不改其它worktree/共享`.local`；目标active，不以本批ambient字段/编译绿色替代完整硬解或最终验收。

#### 2026-11-16 Preferred transfer：SEI147 raw身份与CM/CV/HEIF实际颜色选择（当前交接）

- **基线/分类**：活动树`.worktrees/hevc-hw` / `feat/hevc-hw`，`927ce1a` tracked干净，上一批`7ab8e2c`代码及编译属于progress。本轮从已取得的官方D.2.38/D.3.38与iOS12 CM AlternativeTransferCharacteristics、iOS11 CV CICP getter标准契约继续，未重编旧成功对象，不合main、不动其它树/共享`.local`，无子代理/测试/样本/人工向量/设备操作。
- **当前未提交的真实接线**：shared direct/scalable parser加入prefix147 exact1、独立raw code/来源、agreeing<=8、正确scope/Tid/placement；suffixreserved不提升。sequence复用genuine CLVS首IRAP/持久exact及SUBMIT-success-only提交，middle CRA/no_output/DRAIN不reset。CM快照保raw `X9PreferredTransferCodePoint`并按SDK生成独立标准alternative string；raw/CM来源必须一致，不把CFString alias反推为原bitstream值。unknown/unspecified保raw不造标签，effective CV transfer/gamma不借用被supersede的旧SPS值；4/5显式gamma2.2/2.8。owned pixel metadata实际消费preferred，readonly callback校验effective transfer/gamma及absence，不改borrowed pixels。持久state/pending CM快照扩展分配与冲突检查在SUBMIT前；owned输出的effective gamma字典在pixel attachment修改前分配，若输出阶段OOM沿原token显式error完成，不假装取消已接受责任。原token持有immutable快照。
- **HEIF实际颜色而非unused key**：内部sample adapter额外输出真实AU hdr（public API不变），`source_color`收到它而非从可能合成的CM字段猜值。共享color resolver先用147替代selected-SPS fallback，再维持现有明确non-2 container优先；ICC仍由caller原policy选择。raw SPS VUI不改，raw147/presence/provenance保item info，实际source-view TransferFunction/Gamma进入既有VTPixelTransfer。ordinary VT用preferred；HEIF的explicit container/ICC解释仍由原显式policy负责，不能混同。未知effective CICP在没有明确可用container/ICC时原preflight继续拒绝，不偷回退BT.709。当前源码已接这些路径，但**尚未提交/分析器/正常编译，不是颜色实效证明**。
- **下一动作/完整目标**：下一复核preferred/gamma分配释放、三CFData+独立raw/string冲突原子性、schema调用点/format identity和新scope状态，再固定代码commit、七source analyzer、正式frontend及改动native对象编译，记录新唯一out/gh/push。147像素效果、已emitted late CLLI、149/其他nesting/完整tone-map/LUT/chroma仍未完成；cold最高authority/持续provider/DPB/layout/scratch、BS/FB独立producer排他/durable dequeue/cache/逐buffer退休/c006 ACK、linear NV12/P010/HYFBC、aux-alpha/Rext/prem、其余HEIF属性/implicit lengths/普通VT/CMPhoto/ImageIO全部继续。READY/authority/producer-exclusion/retirement/普通VT门不变；goal active，全部代码齐全后才出唯一最终boot。
- **源码/analyzer里程碑**：已核实际sample两调用点/共享color resolver两调用点都采用新内部参数；public X9HEIFCopyItemSample ABI保持原样。raw preferred与standard alternative格式建立后的identity校验包括类型/整数形态/absence；new source CFNumbers及gamma字典在失败/成功都有释放。owned pixel attachment修改前已验证全部字段并分配effective transfer，readonly校验只读expected dictionary与callback pixel；没有发布后再分配的伪原子链。新唯一`x9/out/hevc-preferred-final-20261116/`保存规范/SDK原摘录和analyzer/native helpers。七source单位iOS arm64 `clang --analyze -Wall -Wextra -Werror` **exit0，无diagnostic**，句柄11834已terminal。尚未提交/正常frontend/native编译，不将此分析结果称为像素或provider验收；下一固定源码再正式编译。
- **固定源码/正常编译结果**：代码 **`555b45e194336dd5e1d8021317c856a131959c23`**（`555b45e`），本批source-head.txt/source.diff已固定。`python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/hevc-preferred-final-20261116/frontend`完整iOS dylib/client正常编译、链接、签名 **exit0**，frontend.log/frontend.exit保留。`python3 -I -B x9/out/hevc-preferred-final-20261116/compile-native.py`正式Builder禁用ccache仅编`sweet_x9_vdec.o` **exit0/66 warnings（62 declaration-after-statement+4既有cast-align）**，native.log/native.exit及kernel/build.log/result.json保留。重放用新唯一out并改native helper固定路径，不覆盖本批成功产物。frontend34288、gh25885已terminal，native正常结束；gh feature run list **exit0/`[]`**，无CI run ID。没有测试/样本/人工向量、成功产物二次解包/hash/反汇编/重建比较、full kernel/XNU/boot/固件构建或设备操作。
- **完整边界/下一真正工作**：本批推进147原始身份、forward状态及颜色选择实际接线，**不是transfer效果或物理硬解验收**。下一继续SEI149 content-colour-volume等真实持久/取消/逐picture语义与iOS16消费契约，或者有新依据的native provider/最高authority缺边；不得把iOS17-only标准key直接链接到iOS16、造默认颜色曲线、只留无人消费的字段或重跑已排除ISA/RTOS/implicit-length。已emitted late CLLI/其余nested HDR、LUT/tone-map/chroma实效和上条完整硬件/HEIF余项全部保留；READY/authority/producer排他/retirement/普通VT门仍不变。全体HEVC/HEIF代码goal保持active，未生成boot，不缩小为本批147或源码绿色。
- **实际push/收尾**：代码push成功 **`927ce1a4..555b45e1`**，push-code.log/push-code.exit0在本批out，句柄87073已terminal，无仍运行的编译/分析任务。本交接正常NOTES-only commit/push，固定源码/编译仍以`555b45e`为准。活动树与长期原件保留，不合main，不改其它树或共享`.local`，无最终镜像/上机结果；下一从真实未闭合生产缺边继续。

#### 2026-11-16 CCV149 完整规范 / iOS16 原件分派 / output-order 缺边（当前交接，无生产源码改动）

- **基线与本轮分类**：活动树`.worktrees/hevc-hw` / `feat/hevc-hw`，基线`ebf71abd` tracked干净；最新功能源码/正常编译仍为`555b45e`及`x9/out/hevc-preferred-final-20261116/`，本轮不重编未变源码、不改旧材料。取得新规范与原件分支证据，但**没有CCV实现或production delta**；不能把这条NOTES提交计成功能推进。全体HEVC/HEIF目标active，未合main、不出boot，无子代理、测试/样本/人工向量或设备操作。
- **完整D.3.40已补齐**：从既有官方H.265 V9 PDF离线提PDF328/406/407/408，`x9/out/hevc-ccv-source-20261116/itu-h265-ccv.txt`完整包含syntax与semantics至D.3.41之前。cancel/persistence按**output order**；persistent结束条件是新CLVS、bitstream结束、或当前层含applicable CCV的picB实际output且其解码得到的POC严格大于picA。persistence0仅当前decoded picture，不可套137/147/148的decoding-order全局状态。xy为signed i32、范围±5000000；luminance为u32归一化量（步进0.0000001），不是直接nits；present字段间min≤avg≤max，四presence不能全0。reserved bits的decoder语义是ignore，不擅自当强制decoder拒绝门。规范的`transferCharacteristics`（147优先）与`transfer_characteristics`（SPS）两名保留原区别，不能混同忽略条件；BT.1886/HLG/其他exact-inverse建议也不是默认tone mapper。
- **iOS16三原件实际只读**：精确读固定`.local/apple/dsc-20H392/System/Library/Frameworks/{CoreMedia,CoreVideo,VideoToolbox}.framework/<同名>`，nm/otool/strings原输出及command/exit在本批out；三个符号/字符串表未命中typed ContentColorVolume/CCV入口，既有Ambient与HDR符号则存在。进一步读CoreMedia真实`hevcbridgeParseSEIRBSP 0x1916658ac`：prefix的special guard是payloadType−148与mask`0x10020001`（148/165/176）加type4；149不进入此typed payload dispatch。**generic raw-payload callback仍可拷贝149，不能据此宣称所有private consumer都不存在**；iOS17-only标准CM key仍不得直接链接到iOS16。
- **原件地址资格而非构建产物复检**：CoreMedia UUID`737230ca-7a8d-32b8-91da-d58e680aa9e9`与已保留root-clone的Cryptex cache匹配；SEI parser4656B及Ambient getter/callback364B与`/private/tmp/x9root/.../dyld_shared_cache_arm64.05`相应mapping逐字相同，`qualify-original.py/.log/.exit0`与`original-range-qualification.json`保存VA/file offsets。这是原件/持久root资格，不是当前设备runtime、CCV消费者成功或自建产物检查。host只读hdiutil来源记录说明sysos旧挂载仍由HelioDarwin原路径提供，**未使用它做资格核对或构建输入**；root挂载记录有旧rootpull文件路径，事实与当前固定`.local`布局不混淆，不重挂/重指共享链接。
- **现有output合同不足以接完整CCV**：实际`x9_vdec.h`要求RECEIVE display-order，但token只是原input身份、completion/cookie是退休依据；`vdec_sessions.inc`与`vt_video_sessions.inc`没有POC或CCV有序event。DROPPED只是no-pixels retirement，不能拿它的到达顺序当显示事件；SUBMIT/retire也不替display。qualified MT6993 reference `vdec_ipi_msg.h:432`注释有`@poc`，**实际`vdec_fb_entry`只有va/timestamp/field/frame_type/flags，没有poc字段**；timestamp是caller数据，不推断POC。当前offered FB也不是selected/display FB。没有新增unused CCV字段、假event/API、host POC sort或塞进`hdr_sequence_next`，READY/authority/producer-exclusion/retirement/普通VT门全部不动。
- **材料/确切执行结果**：`x9/out/hevc-ccv-source-20261116/`有`source-head.txt`、extract-original/capture-current-contract/qualify-original三个helper与原log/exit0、五份带行号source窗口、三framework原表、规范全文、`findings.json`与`replay-commands.txt`。llvm-objdump的Mach-O模式未遵守start/stop，实际输出整个原件269707行，已保留但只截1165行SEI窗口入分析，**未重跑大反汇编或流出全文**。gh feature list exit0/`[]`，无CI run ID；本轮没有analyzer、正常编译、full kernel/XNU/固件/boot，源码未变故不重编。所有句柄终止情况及push结果在收尾记录。
- **下一具体工作/完整边界**：CCV需要真实provider证明原token↔实际output、POC或同等可验证的边界与drop/cancel语义，再接真正iOS16可消费路径；独立fresh单IRAP HEIF可另行处理当前picture，但也必须有实际colour-volume/EOTF消费者，不能只加info key冒充实现。本批证据不证明CCV永远不能实现，也不是整个goal的三轮阻断审计。已emitted late CLLI、其余nesting/HDR/LUT/tone-map/chroma实效、cold最高authority/持续provider/DPB/layout/scratch、BS/FB独立producer排他/durable dequeue/cache/逐buffer退休/c006 ACK、linear NV12/P010/HYFBC、aux-alpha/Rext/prem及其余HEIF/普通VT-CMPhoto-ImageIO仍全部必做。下一不重复无命中strings/这条special switch或重编旧成功产物；唯一最终boot继续等全部代码齐全。
- **实际交接push/停止状态**：规范与原件边界NOTES提交`32290031`已push成功`ebf71abd..32290031`，commit.log/exit0及push-handoff.log/exit0在本批out；gh56705、原件objdump89037、git/push20779均已terminal，无后台任务。本条最后收尾正常NOTES-only提交push，仍不是生产源码变化；功能树保留，不合main、不改其它树/固定`.local`，全体goal保持active，最新有效功能源码与编译仍以`555b45e`为准。

#### 2026-11-16 HEIF amve → 既有 CM/CV nominal-ambient 实际路径（当前交接）

- **基线/本轮分类**：从`1d230a26` tracked干净的活动树`.worktrees/hevc-hw` / `feat/hevc-hw`续接。上一轮是改变下一动作的原件/规范证据，不是生产delta；本轮实际补缺失container入口，功能源码**`c9baabd2`**已commit/push（`1d230a26..c9baabd2`）。未合main、未改其它树/固定`.local`，没有子代理、设备、测试或boot操作；完整HEVC/HEIF goal不缩小为metadata功能。
- **先排除猜测的pasp，再闭可消费amve**：只读既有libheif `Box_pasp`及image-item消费位置、AVIF表和SDK ScalingMode Normal规则；pasp与旋转/fractional clap的完整几何解释尚未闭合，**没有写pasp支持、比例交换或伪square-pixel转换**。同批发现既有libheif `Box_amve::parse/write`真实ordinary Box布局为u32 illumination+u16 x+u16 y（exact8、无FullBox prefix），constructor明确0初始化值不是valid值；image-item实际set_amve消费，AVIF表将amve引用为ISOBMFF的nominal环境信息。证据摘录、官方SEI148语义与iOS15标准CM/CV CFData契约在`x9/out/heif-amve-source-20261116/`。这些是公开reference/官方规范/SDK，不宣称Apple HEIF当前runtime已执行。
- **真实生产变化/无新空接口**：`heif_items.c`识别amve（bit64独立去重），exact8、illumination非0、xy各≤50000，按现有nominal-ambient消费边界验证，原大端字节/单位不缩放，known malformed property即使nonessential也不被静默忽略；重复amve property沿原singleton规则EEXIST。`heif_metadata.c:hdr_extension`加入amve→标准CM AmbientViewingEnvironment CFData，独立验证direct metadata caller的numeric形状后才分配/发布，保owned字节。没有新增仅展示的字段/API、改public ABI、改变working ICC/transfer或给默认tone-map。四文件改动仅items/metadata/header/build manifest，没有shared HEVC或kernel源码改变。
- **实际消费链与边界**：item info的standard raw值与ordered Properties保container来源；`heif_coremedia`原调用把它传入已有`x9_heif_resolve_hdr_extensions`，amve与同AU SEI148必须exact相等、冲突/分配检查在mutation之前。CM建立后的`format_hdr`身份门、`vt_picture`expected/read-only callback identity与普通VT owned CV attachment现自然覆盖property-only amve；bitstream-only的SEIAmbientViewingEnvironmentPresent/Sources仍为false/0，不将container伪标成SEI。native prepare保同一sample的Items/ResolvedHDRMetadata，amve不进入colr ancestry；实际RGB transfer后原逻辑继续清source nominal环境，**不把它伪传成derived destination conditions，也不是ambient适配/像素实效验证**。
- **固定源码/确切正常构建**：`x9/out/heif-amve-final-20261116/`保存`source-head.txt`、`source.diff`、source-path/replay JSON和原log/exit。两个真实改动单元items/metadata按iOS arm64 `clang --analyze -Wall -Wextra -Werror` **exit0/无diagnostic**，analyze-source.py/analyzer.log/.exit保留，句柄16819已terminal。固定`c9baabd2`的`python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/heif-amve-final-20261116/frontend`完整iOS dylib/client编译、链接、签名 **exit0**，frontend.log/.exit保留，句柄18280已terminal。未改kernel/shared parser，故**没有kernel单对象复编、full kernel/XNU/固件/boot构建**；没有测试/样本/人工向量、成功产物二次解包/hash/反汇编/重建比较或设备操作。
- **实际push/CI与重放**：git diff --check通过；gh本feature list **exit0/`[]`**，gh-runs.json/.log/.exit保留，无CI run ID。代码push **exit0**，push-code.log/.exit保留，句柄26953已terminal，无后台任务。重放在活动树取新唯一out，复制analyze-source.py运行，再用上列frontend命令指向新目录；不覆盖本批成功输出、不重编旧对象。本条最后NOTES正常commit/push，源码/构建仍以`c9baabd2`为准。
- **下一具体工作/完整goal仍active**：本批只补container nominal信息到已存在的消费链，不解决CCV output/POC、已emitted late CLLI、其余nested HDR或HDR/LUT/tone-map/chroma实效。下一继续真实native provider/authoritative output链：不能将offered FB或DROPPED退休当显示事件，BS/FB独立producer排他与durable dequeue、cache/逐buffer退休/c006真ACK、cold最高authority与持续session_ops、完整DPB/layout/scratch、linear NV12/P010/HYFBC仍必须闭合。pasp几何、aux-alpha/Rext/prem、其余合法HEIF属性/implicit lengths和普通VT-CMPhoto-ImageIO自动入口亦保留。READY/authority/producer-exclusion/retirement/普通VT门全部原样；无最终镜像，唯一最终boot继续等全部代码齐全。

#### 2026-11-16 CM 创建原件 / VUI fallback 来源边界（当前交接，无生产源码改动）

- **基线/分类**：活动树仍为`.worktrees/hevc-hw` / `feat/hevc-hw`，从`f759de7f` tracked干净续接，最新功能源码/正常frontend编译仍是`c9baabd2`和`x9/out/heif-amve-final-20261116/`。本轮只有改变下一动作的原件静态证据，**没有生产源码delta，不能把NOTES提交计作硬解功能交付**；不合main、不改其它树/固定`.local`，无子代理。完整HEVC/HEIF goal继续active，未出boot。
- **先排除错误的ownership修复假说**：iOS16 CoreFoundation原件`CFDataCreateCopy 0x186ea16b0`的allocator-match immutable fast path仅对inline/owned storage留retain；`+0x10c..+0x110`检查非inline的`_bytesDeallocator`，非0回`+0xc4`实际复制。原`CFDataInit 0x186e64064 +0x1dc..+0x1ec`保留NoCopy deallocator，包括`kCFAllocatorNull`，故**native CFDataCreateWithBytesNoCopy不能据此称会被CreateCopy原样借用**。这是native CF路径；ObjC NSData/subclass另走copy dispatch，本批未证明其全部backing规则。此次新取旧公开CFData.c甚至一直按字节复制，不能用公开旧版解释当前优化或支持相反假说。之前HEIF显式CFDataCreate owned bytes仍保留，但不将这次排除项改成冗余“修复”。
- **CM并非仅浅拷贝extensions**：原`CMVideoFormatDescriptionCreate 0x1915cd114`进入`FigDerivedFormatDescriptionCreate`，`0x1915bab10`实际调用cache optimized stub`0x194918e34`；按ADRP/ADD/BR解到`CFPropertyListCreateDeepCopy 0x186e0b990`，参数mutability0。原deep-copy的Data分支`0x186e0bbd0`再调用上述CreateCopy。现`x9_hevc_copy_hdr_description`虽先建浅root dictionary，交给CM后走真实递归copy；不能仅凭本地CreateMutableCopy那一行认定pending快照没有冻结nested containers，更不加重复plist serialize/parse helper冒充推进。没有执行构造NoCopy/变更字节的人工向量。
- **新的普通CM/VUI可达链与precedence**：原`CMVideoFormatDescriptionCreateFromHEVCParameterSets 0x191646bb4`→`FigVideoFormatDescriptionCreateWithSampleDescriptionExtensionAtom`，其HEVC分支`0x191646720`→`FigBridge_MakeFallbackExtensionsFromVUI 0x19162bbb8`。fallback `0x19162c0f0`明确查询NALtype33/**index0**的SPS，不是AU实际PPS→SPS。`0x19162c214..0x22c`检查原dictionary的ColorPrimaries存在；存在便跳过NCLC三字段group，缺席才通过真实SDK code-point getters补primaries/transfer/matrix。这一group未给standard strings另记显式/合成来源，故**不能以CFString相等反推provenance，也不能直接覆盖caller颜色或未经证明套HEIF逐字段policy**。普通VT逐picture selected-SPS VUI候选仍未闭；没有新增缺席字段helper、unused VUI接口或拒绝门来冒充完整接线。
- **原件资格/准确重放**：新唯一`x9/out/hevc-owned-snapshot-20261116/`保存各function限定的原件asm/err/exit、旧公开CF/dyld-layout reference、`resolve-original.py`、`qualified-original.json`、`findings.json`和`replay-commands.txt`。CoreFoundation UUID`55b9ba28-4c5c-3fe7-9c47-4983337d6e83`及CoreMedia UUID`737230ca-7a8d-32b8-91da-d58e680aa9e9`、七段原件代码与retained root-clone Cryptex cache一致，`qualify-final.exit=0`。GOT pointers使用实际mapping-and-slide table的v2 delta_mask/value_add，不采用首轮未解chain的高位地址或猜mask。`llvm-objdump --disassemble-symbols=...`使用**普通模式、不带--macho**；最大fallback输出1727行保文件，仅窄address窗口进入分析，未重跑前一轮整个CoreMedia反汇编。复制helper到新唯一out即可重放；这些是原件/持久root静态资格，不是设备当前runtime、decoded pixels或自建成功产物复检。
- **本轮结果/下一动作**：gh feature runs **exit0/`[]`**，`gh-runs.json/.log/.exit`保留，无CI run ID，句柄84060已terminal；其余命令均结束。生产源码未变，所以**没有analyzer、frontend/native对象复编、full kernel/XNU/固件/boot构建**；无测试/样本/人工向量、成功产物二次检查或设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作。下一不重查native NoCopy/CM deep-copy或这条VUI fallback；普通VT颜色需要真实source-provenance或更高层解析入口，而native仍须闭最高authority/cold provider、持续session_ops、BS/FB独立producer排他/durable dequeue、DPB/layout/scratch/cache/逐buffer退休/c006真ACK、linear NV12/P010/HYFBC。CCV output/POC、已emitted late CLLI、其余nested HDR/tone-map/LUT/chroma实效、pasp/aux-alpha/Rext/prem/其余HEIF属性和普通VT-CMPhoto-ImageIO自动入口全部继续，READY/authority/producer-exclusion/retirement/普通VT门原样。没有全goal三轮外部阻断，不能标blocked或complete；唯一最终boot仍等全部代码齐全。

- **实际交付/停止状态**：上述NOTES-only证据交接提交`3ef4690e`已push成功（`f759de7f..3ef4690e`），`commit.log/.exit0`、`push-handoff.log/.exit0`在本批out，句柄78964已terminal；不是新功能源码或硬解验收。活动树与长期原件保留，无后台分析/编译/设备任务。本条最终补记正常commit/push，最新有效功能源码与编译仍是`c9baabd2`，完整goal保持active。

#### 2026-11-16 AP IRQ 候选与 captured VCP power 分支（当前交接，无生产 delta）

- **活动基线/范围**：从已push、tracked干净的`0c0f051f`继续`.worktrees/hevc-hw` / `feat/hevc-hw`。此前续接只有只读检查，没有新的代码/构建里程碑；本轮转向native WAITISR的AP IRQ候选，得到改变下一动作的分支证据，**没有新增硬解生产代码**。完整HEVC/HEIF目标active，不合main、不启子代理、不改共享`.local`，唯一最终boot仍等全部代码齐全。
- **先修正reference路径，再排除照搬AP handler**：实际驱动在固定reference的`kernel/kernel_device_modules-6.12/drivers/media/platform/mtk-vcodec/`，不是此前不存在的`.../platform/mediatek`。保存的`.local/x9/modem/live.dts`之`/soc/vdec@36000000`明确`vdec-power-in-vcp=1`，且该节点没有`reg`/`reg-names`/`interrupts`。reference commit **`4d505a4292dab3176a45fec66dc85debc362e24c`**的`vdec_decode_prepare/unprepare`在`power_in_vcp`时直接return；其它分支的IRQ enable/disable还要求`!mtk_vcodec_is_vcp`。其IRQ setup对first IRQ缺失可直接返回0，已注册IRQ则先disable；因此函数存在或probe返回0不能当AP中断已安装/启用。**这些是captured DT和reference静态分支，不是matching loaded KO或当前XNU运行证明**；不能从VDEC基地址、RV55 packed IRQ descriptor或别的设备GIC实现猜AP SPI。
- **WAITISR owner/结果仍不能伪造**：reference reverse c007读取`msg->status`作hardware selector，等待实际context的`int_cond[core_id]`，结束时清condition/type，再回d007；同一condition还有`wake_up_dec_ctx`软件writer，且secure5 handler可跳MMIO后wake，不能把condition1当物理IRQ证据。现有RV55原件profile另有H265-local→shared semaphore及registered-remote两条路线，但其runtime选择/owner仍false；**AP资源缺席不证明所有HEVC wait都local，也不授权默认成功、略掉mandatory wait或授予buffer退休**。当前native control仍有64-record持续journals/自动service pump，rings仍observation-only；clock/wait/qualify_start、bus-owner真实halt/drain及所有READY/authority/producer-exclusion/retirement/普通VT门原样。
- **本地材料/确切结果**：新唯一 **`x9/out/hevc-ap-irq-route-boundary-20261116/`**保存10份带原行号的有界source/DT摘录、`findings.json`、`source-head.txt`和`replay-commands.txt`。只读摘录脚本exit0；已有type1 vendor_boot提取的模块名单单独保留，含clock/VCP/SMMU而无decoder/common KO，**只描述这个subset，不宣称整个`.local`或设备缺module**。没有重提旧RV55帧、弱ISA/RTOS/implicit-length类，没有测试/样本/人工向量、analyzer/frontend/native单对象/full kernel/XNU/固件/boot构建，也没有成功产物二次解包/hash/反汇编/重建比较或设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作。最新有效功能源码/编译仍为 **`c9baabd2` / `x9/out/heif-amve-final-20261116/`**，无新CI build；本条正常commit/push结果另记，不算功能交付。
- **下一准确边/完整范围不缩小**：停止未经qualification的AP IRQ移植；先闭真实VCP-owned IRQ/clock、selected HEVC local/remote wait及instance/generation关联，再绑定mandatory native hardware ops，不能写无人消费的IRQ接口或成功空provider。cold最高authority、真实持续session_ops、BS/FB独立producer排他/durable dequeue、DPB/layout/scratch/cache/逐buffer退休/c006真ACK、linear NV12/P010/HYFBC、CCV output/POC与late CLLI/nested HDR、HDR/LUT/tone-map/chroma实效、pasp/aux-alpha/Rext/prem/合法HEIF属性/implicit lengths和普通VT-CMPhoto-ImageIO自动入口全部保留。未满足全goal连续三轮同一外部阻断审计，不能标blocked或complete。
- **实际收尾**：本条证据交接 **`626bac33`** 已push成功（`0c0f051f..626bac33`），`commit.log/.exit=0`、`push.log/.exit=0`在本批out；句柄47195已terminal。随后gh feature list **exit0/`[]`**（`gh-runs.json/.stderr/.exit`），句柄32052已terminal，无CI run ID、后台任务或生产delta。功能树保留，最后这一行正常commit/push，不重编旧功能、不改main。

#### 2026-11-16 selected slice POC → 原 token 输出顺序核验（当前代码交接）

- **上一turn/活动源码**：上一turn为progress（AP IRQ/VCP分支证据改变下一动作，不是硬解交付）。本轮从tracked干净的`accf3ff6`续接同一活动树，未重复AP/弱ISA/RTOS/implicit-length类；改为补真实picture facts及输出关联缺口。五文件功能源码 **`b7b76144`** 已commit/push成功（`accf3ff6..b7b76144`，`push-code.log/.exit=0`，句柄98470已terminal）。这是实际parser/VT代码delta，不是空接口或仅metadata展示；完整HEVC/HEIF目标继续active，所有READY/authority/producer-exclusion/retirement/普通VT门保留，不合main、不出boot。
- **规范依据/真实输入字段**：官方既有H.265 V9 PDF按真实outline取7.3.6.1/7.4.7.1/8.1.3/8.3.1/C.5.2.4，源文件/摘录在 **`x9/out/hevc-poc-source-20261116/`**，关键`itu-exact-syntax-poc.txt`、`itu-no-rasl-output-bump.txt`。新`slice_prefix`沿实际catalog PPS→SPS取得POC width、CTB几何和PPS dependent/output/extra-header flags；`slice_facts`在正确bit位置读取独立segment的slice type、`pic_output_flag`、POC LSB。nonfirst读取真实CTB address和dependent flag；dependent段不把coded data误读成独立header。所有独立段的POC/output必须一致，slice type不强行要求彼此相同；IRAP须I。IDR LSB0、缺席pic_output_flag1是规范推导，不是硬件成功默认。完整AU/catalog门成功后才发布新增facts，仍不声称完整slice/RPS/DPB语义验证或扩张profile/capability。
- **已消费的POC状态，不是无人调用helper**：`x9_hevc_poc_sequence_next`实现8.3.1的实际prevTid0选择，排除RADL/RASL与*_N，不按“上一AU”更新；回绕严格保留两支`>=half`/`>half`差别，signed64算MSB/加法后核full POC在signed32域。IDR/BLA、首IRAP或EOS后IRAP令MSB0并换CLVS；middle CRA/no_output flag/DRAIN不重置，无外部HandleCraAsBla。`decode_frame`在SUBMIT前准备prospective POC与HDR并核CLVS相同，**只有真实SUBMIT success才提交**并在original pending token保存POC/原output flag。错误/取消/新session清除，DRAIN保留；没有生成新firmware token、RPS、软件参考帧或主机排序。
- **真实RECEIVE→VT效果与边界**：现有session_ops契约`x9_vdec.h:105`已要求display-order output+original token。`video_receive`先保原packet/epoch/shape/token门，只有correlated non-DROPPED pixels且raw pic_output_flag1才推进`last_output_(clvs,poc)`，核同CLVS严格增加/旧CLVS像素不晚于新CLVS；不用PTS或accepted input/FB offer判断输出。caller DoNotOutputFrame不抹去这次物理pixel receipt；DROPPED不推进display边界。raw flag0只在**实际terminal RECEIVE后**抑制VT pixels、emit FrameDropped，不制造receipt、free或推断DMA退休。顺序冲突在pending_take前失败，现有fault路径仍保每个accepted token的唯一终态。此核验不是完整CVS POC uniqueness验证，更不是CCV149/output persistence、真实硬件运行或完整decoder。
- **固定源码的确切正常构建**：最终 **`x9/out/hevc-poc-token-final-20261116/`**保存`source-head.txt=b7b76144`、`source.diff`、`source-path.json`、`replay-commands.txt`及helpers/log/exit。改动TU `hevc_bitstream.c`和`vt_decoder.c`（含改动inc）的iOS `clang --analyze -Wall -Wextra -Werror` **exit0/无diagnostic**，句柄65352已terminal；共享parser实际进入的recipe **`sweet_x9_vdec.o`正常单对象编译exit0/69 warnings**，没有全kernel link。固定b7b76144的 `python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/hevc-poc-token-final-20261116/frontend` 完整iOS dylib/client编译、链接、签名 **exit0**，句柄61377已terminal；改动build.py syntax compile exit0，不是测试。编译后仅澄清header里firmware POC/DPB owner与host诊断POC的注释，**无新增功能变化/旧产物复编**；该注释随本交接正常提交。
- **CI/重放/操作约束**：push后gh feature list **exit0/`[]`**（`gh-after-push.json/.stderr/.exit`，句柄46128已terminal），无CI run ID；push前原gh记录亦保留，未覆盖。按本批replay新建唯一out，复制analyzer helper、复制native helper并改其out目录，再正常frontend命令指向新目录；禁止覆盖/复检这批成功产物。没有测试脚本/套件/样本/人工向量、full kernel/XNU/固件/boot、成功产物二次解包/hash/反汇编/重建比较或设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作；没有子代理/后台任务，不改共享`.local`或其它树。旧amve构建保留，但不再当最新功能源码。
- **下一实际缺口/完整范围保留**：现在有真实POC和original-token输出边界，可进一步闭CCV149的合法payload、selected-SPS/SEI147适用性、按output order的cancel/persistence及真实CM/CV消费者；**本轮没有CCV payload/consumer，manifest仍明确false，不能把POC顺序核验说成CCV完成**。late CLLI已emitted回溯、其余nested HDR/LUT/tone-map/chroma实效、cold最高authority/VCP-owned IRQ-clock/真实持续provider、BS/FB排他/durable dequeue、DPB/layout/scratch/cache/逐buffer退休/c006真ACK、linear NV12/P010/HYFBC、pasp/aux-alpha/Rext/prem/合法HEIF属性/implicit lengths及普通VT-CMPhoto-ImageIO自动入口仍全部必做。唯一最终boot仍须等完整代码齐全，不标complete或blocked。

#### 2026-11-16 associated IRAP → 派生 RASL 输出资格（当前代码交接）

- **接续/实际交付**：同一活动树`.worktrees/hevc-hw` / `feat/hevc-hw`，从`23677e3a`及五文件未提交草稿续接；源码逐分支复核后提交 **`2af74a52`**，push成功`23677e3a..2af74a52`。这是实际POC/VT渲染修正，不是CCV实现或仅交接进展；完整HEVC/HEIF目标继续active，不合main、不出boot。READY、authority、producer-exclusion、retirement与普通VT注册门全部不动。
- **规范与受控状态**：H.265 3.3的associated IRAP是**前一个decoding-order IRAP**；8.1.3规定RASL且其关联IRAP的NoRaslOutputFlag1时，derived PicOutputFlag0，否则等于raw pic_output_flag。既有`x9/out/hevc-poc-source-20261116/itu-no-rasl-output-bump.txt`和本批原件目录`itu-associated-irap.txt`保留精确依据。`x9_hevc_poc_sequence_next`新增association状态及派生结果：首IDR/BLA/CRA、EOS后IRAP关联1；middle CRA关联0但不换CLVS/重置POC；非IRAP继承。状态布尔/零基线门保留，仍由实际sample_metadata提供facts，无外部HandleCraAsBla默认。
- **真实消费者/失败语义**：`decode_frame`先算prospective association，仍只在**SUBMIT成功**后提交；每个original pending token现在保存derived eligibility，不再保存raw flag作为输出资格。RECEIVE原packet/token/epoch/shape门成功后才消费，flag0包括raw0及关联IRAP抑制的RASL；真实terminal收到后发VT FrameDropped、不返回像素。DROPPED和derived flag0都不推进display POC边界；caller DoNotOutputFrame不抹除flag1的物理观察。失败SUBMIT不提交新association；cancel/fault/new-session清，DRAIN保。无主机排序/DPB、fake receipt/free/提前退休；不声称完整leading-picture类型合法性、CVS唯一性或HEVC conformance。
- **固定源码/正常构建结果**：新唯一 **`x9/out/hevc-rasl-eligibility-final-20261116/`**保存`source-head.txt=2af74a52`、源码diff/path、replay和helpers/log/exit。`hevc_bitstream.c`与`vt_decoder.c`（含inc）iOS `clang --analyze -Wall -Wextra -Werror` **exit0/无diagnostic**，句柄76383已terminal；recipe `sweet_x9_vdec.o`正常单对象编译 **exit0/69 warnings**；完整frontend dylib/client编译、链接、签名 **exit0**，句柄83738已terminal；build.py syntax compile **exit0**。无全kernel link/XNU/固件/boot，也没有把上一批成功结果套到本批。
- **push/CI与复现约束**：`push-code.log/.exit=0`（句柄93653已terminal）；随后gh feature list **exit0/`[]`**，`gh-after-push.json/.stderr/.exit`保留（句柄41607已terminal），无CI run ID。按本批`replay-commands.txt`新建唯一out，复制两helper并改native out，再正常frontend；不覆盖旧材料。未运行测试脚本/套件/样本/人工向量，未二次解包/hash/反汇编/重建比较成功产物；无子代理、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作，共享`.local`和其它树不动。
- **CCV原件资格/不虚构消费者**：本轮接续材料在 **`x9/out/hevc-ccv-original-20261116/`**：目标20H392 CM/CV原件defined-symbol table及CCV字符串筛选未命中typed ContentColorVolume标准key，mastering/CLLI/ambient exports则在；当前SDK声明`kCMFormatDescriptionExtension_ContentColorVolume`为**iOS17+**，CV同名key注释为to-be-added。此项仅symbols/strings/SDK资格，不是新的完整export-trie/cache-bytes核对，不宣称iOS16没有所有私有消费者；也不重复此前special-dispatch扫描当新功能。未猜CFSTR key/weak attachment冒充消费，manifest `hevc_ccv_output_state`仍false。
- **下一真实代码边/完整范围不缩小**：现有original-token POC核验和derived输出资格可供真正CCV output-boundary使用，但下一必须先闭**iOS16真实消费者资格或自有明确目标颜色/显示条件的消费者**，再一起接149合法payload、selected-SPS/SEI147适用性、output-order cancel/persistence；不做无人消费的raw字段或unused helper。不重扫已排除ISA/RTOS/implicit-length/CCV空字符串类。最高cold authority/VCP-owned IRQ-clock/持续physical provider和session_ops、BS/FB排他/durable dequeue、DPB/layout/scratch/cache/逐buffer退休/c006真ACK、linear NV12/P010/HYFBC、已emitted late CLLI/其余nested HDR/LUT/tone-map/chroma实效、HEIF pasp/aux-alpha/Rext/prem/合法属性/implicit lengths与普通VT-CMPhoto-ImageIO自动入口全部仍必做。唯一最终boot继续等全部代码齐全；未满足complete或全goal三轮外部blocked条件，无live句柄。

#### 2026-11-16 HEIF pasp coded sample → untransformed native raster（当前代码交接）

- **进展分类/实际源码**：上一goal turn是progress（`2af74a52`真实RASL输出修正），本轮从tracked干净`186f1247`继续同树，不重复CCV空字符串/弱ISA/RTOS/implicit-length类。两批实际功能分别 **`6f257d23`**（coded CM/CV pasp）和 **`ee745b37`**（untransformed coded raster真正消费）；均已push `origin/feat/hevc-hw`，不合main、不出boot。完整HEVC/HEIF目标仍active，不将本项当完整pasp几何或硬解交付；全部READY/authority/producer-exclusion/retirement/普通VT门原样。
- **新输入资格/不用猜变换语义**：**`x9/out/heif-pasp-source-20261116/`**保存既有Nokia tree的固定SHA身份，以及按该SHA新取的`nokia-pasp.java`、`nokia-descriptive.h`、`nokia-pasp.cpp`与curl命令/header/log/exit0：明确pasp为descriptive ordinary Box、两个u32 spacing，不是FullBox或resize。SDK CM/CV标准key和Normal规则保留；`sdk-pasp-numeric-contract.txt`核`CFNumberGetValue`对有损/越界conversion返回false。目标20H392 CM/CV原件defined-symbol table确有三个typed aspect keys，CV另有`CVImageBufferSetPixelAspectRatio`/unsigned64 C++ setter符号；只是exports/SDK资格，没有新完整export-trie/cache-byte核对或原Apple HEIF/当前设备runtime证明。公开Nokia参考不进git、不成为build依赖；AVIF的1:1约束不套HEVC HEIF。
- **实际coded-sample流**：`heif_items`新增exact8/两spacing非零/duplicate门，保原raw unsigned32（absent0）；不再静默忽略nonessential pasp。`heif_metadata`给mandatory info及CM extensions建立owned标准aspect dictionary，raw properties原样保留，非colr/继承颜色。`X9HEIFCopyItemSample`创建CM后核其实际rational ratio；标准CM/SPS自动值没有被伪标成container provenance。真实`pixel_buffer→x9_hevc_attach_image_metadata`使用unsigned32全域，不再错误限INT32_MAX；`X9HEVCCopyHardwarePicture`的read-only callback核验要求present CM ratio与真实CV ratio相同，不补写borrowed pixels。允许等价约分pairs，uint64 cross-product在完整u32域无溢出；精确整值CFNumbers可转换，fraction/NaN/overflow不被截断成功。不是只展示raw info。
- **已消费的native raster，非假square转换**：private shared-source geometry/raster新增output spacings，public CV image入口和ioctl ABI不变。plan先检查全reachable graph，只有**无任何HEIF transform的coded root**可保non-square；non-square transformed root或任一child在任何source callback/hardware前ENOTSUP，square的任意相等positive spacing无需纠正。`compose`真正把admitted root spacing交给owned raster；native在prepare/decode前分配owned aspect。actual硬件callback→同尺寸view→Normal ICC transfer先核真实target aspect，随后packed bytes→owned export source带同一已知metric，final output再read-only核比例并在CV及mandatory info发布。每次现有Normal call均保持sample dimensions，不resize/换轴/排序，不把丢失attachment补写成成功。无transform的coded root不涉及尚未知的pasp轴语义；不会借此宣称完整transformed/derived physical-metric实现。
- **固定最终源码/确切编译**：第一阶段`6f257d23`在 **`x9/out/heif-pasp-final-20261116/`** analyzer/frontend/syntax均exit0；其产物不复检、不套后续源码。第二阶段以 **`x9/out/heif-pasp-raster-final-20261116/`**为最新：`source-head.txt=ee745b37`、两功能累计source.diff、source-commits/path/replay保存；九个受改动private headers/源码影响的userland TU（items/compositor/metadata/coremedia/vt_image_metadata/vt_picture/native/CLI/vt_decoder）iOS Clang analyzer **exit0/无diagnostic**（句柄65316已terminal）；完整frontend dylib/client编译、链接、签名 **exit0，-Wall -Wextra -Werror无diagnostic**（句柄54277已terminal）；build.py syntax **exit0**。只改userland HEIF/CM/CV源码和private headers，无kernel对象改动，所以无native对象/全kernel/XNU/固件/boot构建。
- **push/CI/重放与禁止事项**：第一阶段push `186f1247..6f257d23`/gh`[]`，最终push `6f257d23..ee745b37` **exit0**（本批`push-code.log/.exit`，句柄57388已terminal），随后gh feature list **exit0/`[]`**（`gh-after-push.json/.stderr/.exit`，句柄8456已terminal），无CI run ID。新目录重放两批各自source-head，复制相应analyze helper，再正常frontend命令指向新唯一out；禁止覆盖/复检成功产物。无测试脚本/套件/样本/人工向量、成功binary二次解包/hash/反汇编/重建比较、子代理或设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作，共享`.local`/其它树不动。`evidence.json`保第一阶段原范围，新增`raster-evidence.json`记录第二阶段真实消费，不回写旧批结论。
- **下一完整代码边/未完成边界**：manifest `heif_pasp_derived_geometry`和runtime仍false，**不能把untransformed coded-root支持替代整个pasp要求**。下一需规范或qualified实际consumer明确pasp对ordered transform的轴/适用阶段及derived placement sample metric，再实现旋转、fractional clap和derived图的完整geometry/actual输出；不只复读同一libheif后贴metadata或Nokiadescriptive分类来猜换轴。CCV仍须真实iOS16颜色/显示消费者与149 payload/selected-SPS/147/output-order cancel/persistence，已emitted late CLLI/其余nested HDR/LUT/tone-map/chroma实效亦缺；最高cold authority/VCP-owned IRQ-clock/持续physical provider-session_ops、BS/FB独立排他/durable dequeue、DPB/layout/scratch/cache/逐buffer退休/c006真ACK、linear NV12/P010/HYFBC、aux-alpha/Rext/prem/其余合法HEIF属性/implicit lengths与普通VT-CMPhoto-ImageIO自动入口全部仍必做。目标没有complete或连续三turn真实外部blocked条件；唯一最终boot继续等全部代码齐全，无live构建/CI句柄。

#### 2026-11-16 HEIF present pasp → coded-root 有序输出 metric（当前代码交接）

- **实际功能/固定源码**：从 tracked clean `2194aa85` 续接同一 `.worktrees/hevc-hw` / `feat/hevc-hw`，八文件生产源码 **`4804863f`** 已 commit/push（`2194aa85..4804863f`，push-code.log/.exit=0）。这是实际 ordered raster metric 与 native output 改动，不是只扩接口/证据字段；完整 HEVC/HEIF goal 仍 active，不能称完整 pasp/派生图或硬解已完成。未改 main/kernel/XNU/固定 `.local`，无子代理、设备或 boot；所有 READY/authority/producer-exclusion/retirement/普通 VT 注册门原样。
- **原件输入 stage 已闭，新证据不冒充最终显示行为**：新增 ignored **`x9/out/heif-pasp-transform-source-20261116/`**。MediaToolbox UUID **`9c23f13c-2605-3ed2-bf6b-f8d537ee6655`** 与既有 root clone 实际 Cryptex cache 相同，9 个 selected 函数 ranges 逐字匹配（qualify-original.log/.exit=0、original-range-qualification.json），仅原件来源资格，不是当前设备 runtime/自建产物复检。`_createPixelAspectRatioExtensionDictFromIFFItemProperty 19554d098` 直接向已精算岛 `19b7212d0→186e63264 CFNumberCreate` 传 pair/pair+4，w1=3 是 SInt32，**不照抄其 signed32 窄化**；另两岛确认为 CFDictionaryCreate/AddValue。真实 slide-v2 delta-mask/value-add 解码 GOT→export-variable→CFString 得到 **HorizontalSpacing / CVPixelAspectRatio / VerticalSpacing**；初始误预期 PixelAspectRatio 导致 assert1，失败 log 原样保留，最终 resolve-keys-and-creation-final.log/.exit=0，结果在 resolved-original-keys-and-creation.json。完整 `_createFormatDescriptionForItem 19554e4dc` 复核：原 item+50 ispe、+58 pasp、+78 clean aperture 分开，19554e640 岛实际为 FigVideoFormatDescriptionCreateWithSampleDescriptionExtensionAtom，19554e7a0 为 CMVideoFormatDescriptionCreate；source aspect 未按 item rotation 先交换。**仅证明 codec-facing/pre-item-transform source stage，不是完整 HEIF normative 或 Apple 最终显示/渲染 oracle**。CMPhoto metadata/orientation 支路与两个搜索 miss 不再重复。
- **输入→真实 ordered metric→像素/输出调用链**：compositor 的 present source pasp 随 property association 原顺序：mirror/整数 extent crop 保 ratio，奇数 quarter-turn 换轴；现有显式 LINEAR_CEIL 的采样中心跨 exact aperture，ratio 精确乘 **`(width_n * height_d * out_height)/(width_d * out_width * height_n)`**。非整数 origin 而整数 extent 只插值、不变 spacing；不从 double 推 ratio，不改已有 bilinear/线性 ICC 像素执行。最多 `1+3*32` 因子每轴，所有 ordered transforms 后跨轴 GCD 消掉再乘，避免中途假 overflow；最终不可表示返回 E2BIG，不 round/clamp/default。raw `x9_heif_image` 仍 unsigned32；private geometry/raster、typed CM/CV reader 升为 **`1..INT64_MAX` uint64**，真实 CFNumberGetValue 精确转换、只读 CM/CV ratio 核验用 unsigned128 cross-products，允许等价 pair、不修 borrowed pixels 或丢失 attachment。
- **native 真消费与仍保留的 graph gate**：输出 aspect 在 prepare/首硬解前 owned 分配，实际 compose raster 与 geometry pair 一致才 export；已有自有 CV export source 使用 **transformed ratio**，Normal transfer 仍同尺寸，最终只读核 CV ratio，标准 CV 和 mandatory info 发布。每节点 info 新增 **TransformedPixelAspectRatio**，原 raw properties/coded CM aspect 不被覆盖；CLI heif-info 仅打印 planned ratio，不跑 pixels。全 graph 同时检查 **raw source 非方形 OR transformed output 非方形**：只 coded root 放开，任何 derived/child 仍在 callback/hardware 前 ENOTSUP；避免 square fractional crop 变非方形被忽略，也不因 raw non-square 偶然变 square 就授予派生继承。缺席 pasp 保 0/0，**selected-SPS/真实 CM aspect 的 raster propagation 未完成**，不把缺席当 1:1；manifest heif_pasp_derived_geometry / selected_sps_raster_metric / runtime_verified 仍 false。aux/Rext/prem 和全部物理 gate 不放开。
- **本次正常编译及 shell 记录故障必须区分**：固定源码 **4804863f** 的唯一目录 **`x9/out/heif-pasp-ordered-final-20261116/`** 保存 source-head/diff/path/replay 与 command-results.json。正常 `python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out .../frontend` 的两项 compile/link、两项 codesign 全部通过 `subprocess.run(check=True)`，frontend.log 达到源码末尾 success line，**这些子命令为0，-Wall -Wextra -Werror 无诊断**。但外层 zsh 使用只读变量 `status=$?` 记录返回值时失败：实际 shell **exit1**，Python exit 未独立捕获，记录 frontend-wrapper.exit=1；**不能把 wrapper 写成 exit0，也未为了记码复建成功产物**。九 TU analyzer log 无 diagnostic；后续 TU 行证明前八项 check=True 返回0，第九项及 Python exit 未独立捕获，同一 wrapper 错误 shell1（analyze-wrapper.exit=1），不虚写九项 exact exit0、不重跑。build.py in-memory syntax **exit0**；今后 wrapper 用 `rc=$?` 或 Python 直接记录 subprocess returncode，复放命令已注明。只有 userland/private headers 改动，所以未编 native kernel 对象/full kernel/XNU/固件/boot。
- **CI/复放/完整目标下一边**：`git diff --check` 通过；`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，gh-runs.* 保存，无 CI run ID。原件两 resolver、全部 build/analyzer/gh/push 工具已 terminal，无后台句柄；未运行测试脚本/套件/样本/人工向量/CLI，未二次解包/hash/反汇编/复建比较成功自建产物，无 ADB/fastboot/mailbox/MMIO/SMC/分区操作。重编只能用 replay-commands.txt 新唯一 out；原件证据与 ordered-metric-evidence.json 保持 separate/ignored。下一补**selected PPS→SPS VUI/实际 coded CM source aspect 在缺席 pasp 时进入同一 preflight/ordered metric/native export**，不能只加 unused helper 或把 CM 默认伪称 container provenance；然后闭完整 derived metric/合法属性/aux-alpha。cold 最高 authority、VCP-owned IRQ/clock、持续 physical provider/session_ops、BS/FB 独立 producer 排他/durable dequeue、DPB/layout/scratch/cache/逐 buffer 退休/c006 真 ACK、linear NV12/P010/HYFBC 真转换、CCV 真实消费者/149 payload/selected-SPS+147 applicability/output-order 状态、late emitted CLLI/其余 nested HDR/HDR-LUT-tone-map-chroma 实效、aux-alpha/Rext/prem/implicit lengths 和普通 VT-CMPhoto-ImageIO 自动入口仍全部必做；不缩小全 goal，唯一最终 boot 继续等全部代码齐全，不合 main。

#### 2026-11-16 缺席 pasp 的 selected-SPS/实际 CM source-aspect → native ordered raster（当前交接）

- **进展与交付**：上一 goal turn 为 progress（`4804863f` 实际有序 metric），本轮从 tracked clean **`e06ff645`** 继续同一 `.worktrees/hevc-hw` / `feat/hevc-hw`，11 文件生产源码 **`d448b9c9`** 已 commit/push（`e06ff645..d448b9c9`，新目录 push-code.log/.exit=0）。不改 main/kernel/XNU/共享 `.local`，不启子代理；完整 HEVC/HEIF goal 仍 active，未以本窄项代替全目标。全部 READY/authority/producer-exclusion/retirement/普通 VT gate 保留，无 boot 或设备动作。
- **实际 selected source，而非 representative SPS/default1:1**：复核现有 `hevc_bitstream.c:sample_vui` 的完整 AU gate→VCL PPS id→retained PPS.reference_id→exact SPS/VUI，已有 SAR table IDC1..16、raw Extended_SAR255 不改 parser/kernel。`X9HEIFCopyItemSample` 现实际请求/核验 pre-transform CM aspect：已有 explicit pasp 优先保原 provenance；缺席 pasp 时，实际 selected-SPS 的已知 ratio 注入 owned 标准 CM extension 并与真实 CM rational ratio 核对。VUI absent/IDC0 未指定时**只读取实际 CM format 返回值**，存在记 COREMEDIA-only、缺席保0/0，不生成1:1，不伪称 container/SPS；reserved IDC17..254 或 zero Extended_SAR 在缺 pasp 时 ENOTSUP，绝不借 CM fallback 伪成功。全 catalog/IRAP/dimensions/HDR 验证保留，原 raw pasp/Properties/VUI 不被覆盖；新增 **SourcePixelAspectRatio / SourcePixelAspectOrigin / SourcePixelAspectInterpretation**，origin 明确 **absent0/pasp1/selected-SPS2/observed-CM3**。公共 X9HEIFCopyItemSample ABI 原样，内部 adapter 多返回 typed source snapshot。
- **真实 immutable plan→metadata preflight→hardware callback→export**：native 真调用 `x9_heif_plan_create_with_source_aspects(...,native_source_aspect,c,...)`；创建阶段每 reachable coded id 只查询一次 CM/sample metadata，不注册 VT、不解码/碰硬件，立即释放 sample/info，返回 plan 不保 callback/context。geometry 新增 source_aspect 及 provenance，**同一已有 ordered factor metric** 以它为 source，所有 child/derived 的非方形 SOURCE OR OUTPUT gate 也覆盖 SPS/CM 值，不再因缺 container pasp 漏过。callback typed pair/presence/origin/range 及 explicit pasp rational identity 在 plan 内验证。`prepare` 独立核 source origin/实际 CM ratio；`native_raster` 在硬解前再核 fresh CM，并只读核实际 decoded CV ratio/presence，连“planned absent 却实际 present”也拒绝，不修 borrowed pixels。实际同尺寸 Normal ICC transfer、compose→owned export source/final CV 输出继续使用正确 ordered ratio，root info 分别保 pre-transform source 与 output；输出 dictionaries 均首硬解前 owned 分配。typed pixel-aspect constructor 统一到实际三方消费者 `x9_hevc_copy_pixel_aspect`（raw metadata、CM request、native export），positive signed64、exact CFNumbers、owned immutable dict；不是 unused helper。
- **范围/成本必须保留**：原 container-only `plan_create[_with_policy]`/CLI heif-info 仍只是 container geometry planning，不能把它当 selected-SPS/CM/硬解证明；真实 native 入口用新 metadata resolver。新增一次串行 CM/sample metadata pass，**没有 graph-wide AU/sample cache**，以后可做有寿命证明的复用，但本轮未用大缓存绕开原预算；原 graph metadata/raster budget、颜色 preflight 与失败释放保留，VT transient storage 独立。已有 source-stage 原件资格只复用上一 **heif-pasp-transform-source-20261116** 的9 ranges/UUID/真实 key+callee JSON，没有重扫/覆写；新 SDK CM HEVC parameter-set/extension 契约及 `source-aspect-evidence.json` 在新目录。它们与 source call chain 只证明当前代码接通，**没有 iOS 原件/设备最终渲染 runtime 结果**。Manifest selected_sps_raster_metric 现 true，但 derived_geometry/runtime_verified/physical_provider/complete_image_compositor 仍 false，非方形 derived/child、aux-alpha/Rext/prem、ordinary routing 仍未支持。
- **固定唯一构建/明确退出码**：**`x9/out/heif-source-aspect-final-20261116/`** source-head.txt=`d448b9c9`，source.diff/path/replay/SDK/evidence/command-results 保存。`compile-source.py` 用实际 Xcode iOS16 arm64 Clang 对 items/compositor/metadata/coremedia/vt_image_metadata/vt_picture/native/CLI/vt_decoder **九 TU analyzer 各 returncode0、无 diagnostic**（analyze-results.json、analyze.log/.exit=0）；正常 `python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out .../frontend` 完整 dylib/client compile/link/ad-hoc sign **exit0 / -Wall -Wextra -Werror 无诊断**（frontend.log/.exit）。build.py in-memory syntax **exit0**，driver compile-source.log/.exit **0**；这次 Python 直接捕获 subprocess returncode，外层用 `rc=$?`，没有上一 zsh status 记录错误，不将旧失败件重标为成功、不复建旧产物。只有 userland/private headers 改动，未编 native kernel 对象/full link/XNU/固件/boot；没有测试脚本/套件/样本/人工向量/CLI 或成功自建产物二次解包/hash/反汇编/复建比较。
- **CI/重放与下一完整 stage**：`git diff --check` 通过；源码 push 前/后 `gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**（gh-runs.* / gh-after-push.*），无 CI run ID，全部工具 terminal、无 live 句柄。复放复制本新目录 compile-source.py 到**新 sibling out**，记录当次 git HEAD 后 `python3 -I -B x9/out/<new-unique>/compile-source.py`；绝不覆盖/套用 d448b9c9 结果。下一优先闭**完整 derived sample metric/placement/inheritance**：沿已定位原 IFF **`__tilesHaveSameBaseProperties 19554b988`** 与真实后续 render consumer 取得独立资格，不能把 shared base-property eligibility flag、tile 尺寸相等或单份 root aspect 当最终继承；或继续 physical constructor3072e2/最高 owner，不能猜 vendor ISA/重扫旧弱类。cold 最高 authority/VCP-owned IRQ-clock/持续 physical provider/session_ops、BS/FB 独立排他/durable dequeue、DPB/layout/scratch/cache/逐 buffer 退休/c006 真 ACK、linear NV12/P010/HYFBC 真转换、CCV 真消费者/149 payload/selected-SPS+147 applicability/output-order 状态、late emitted CLLI/其余 nested HDR/HDR-LUT-tone-map-chroma 实效、aux-alpha/Rext/prem/implicit lengths/其余合法 HEIF properties 与普通 VT-CMPhoto-ImageIO 自动入口全部仍必做。没有设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作；不合 main，唯一最终 boot 继续等全部代码齐全，不调用 complete/blocked。

#### 2026-11-16 每条 grid/iden edge → 一致 declared sample lattice 实际 raster 消费（当前交接）

- **活动源码/真实交付**：上一 turn 为 progress（selected-SPS/CM source aspect 的11文件接线），本轮从 tracked clean **`d2fb822e`** 继续唯一 `.worktrees/hevc-hw` / `feat/hevc-hw`。五文件生产代码 **`196596e1`** 已 commit/push（`d2fb822e..196596e1`，本批 commit-code.log/.exit=0、push-code.log/.exit=0），不是只有 metadata/helper/NOTES。**这是一致 lattice 的 grid/iden exact-copy 子集，不是完整 derived inheritance 或全硬解完成**；完整 goal 仍 active，所有 READY/authority/producer-exclusion/retirement/普通 VT 注册门保留。不合 main、不改其它 worktree/kernel/XNU/共享 `.local`，不启子代理。
- **原件观察/先排除错误依据**：新唯一 **`x9/out/heif-derived-metric-source-20261116/`** 的 IFF/CMPhoto 四份限定 asm 保存。`__tilesHaveSameBaseProperties 19554b988` 的 `19554ba50..ba6c` 比较 item `+48/+50/+68/+70` 配置/尺寸/颜色指针，**没有比较 `pasp +58`**；grid/iden/iovl 子项令 eligibility byte false。这不是最终 sample metric/inheritance。`_IFFPictureTileAccessorCreate 195551150` 的 `19555131c..13b0` 真查询每 child 的 transformed output dimensions、核相同尺寸、累加整数 row-major x/y；`_IFFPictureTileAccessorCopySpatialRelationDictionary 195551884` 按 canvas 和 clap 裁末边。`_PictureTileCursorCopyProperty 195305ff4 +a8` 调该 spatial getter；CMPhoto `_TileIterator_getTileSourceRect 1a5dcfd28` 真查询 **TileSpatialRelation** 并消费 **Width/Height**，不是从名称猜 clean aperture。新 slide-v2 pointer→variable→CFString 解析还确认 **HorizontalOffset/VerticalOffset**，`resolved-new-tile-keys.json` 与 helper/log/.exit=0 保留。`_HEIFItemDecoderCreate 1a5d90354` 只追到 canvas create/color/eligibility，**本轮仍没有已解析的 Apple 最终 aspect/render/inheritance 消费者或 runtime 显示证据**。
- **原件独立资格**：`qualify-selected.py` 在新目录只资格本轮 **16 个新增 selected ranges**，与 retained root-clone Cryptex cache 逐字相同；MediaToolbox UUID **9c23f13c-2605-3ed2-bf6b-f8d537ee6655**（9 ranges）、CMPhoto UUID **8c636e49-40af-38ac-87e6-4943a376eed2**（7 ranges），`qualified-selected-original.json`、qualify-selected.log/.exit=0。四次 objdump 各 exit0，原 stderr/exit 分开保留；没有重提/覆写上一批9 ranges或成功自建产物。此资格是持久原件 provenance，不是当前设备运行/最终图片证明。`findings.json` 明确观察、生产接线和未闭边界，无新增过程报告 Markdown。
- **实际新窄流/不是 root-only 放门**：plan 全图完成后，**每条 grid/iden dimg edge（包括重复 placement 和 DAG 新 parent）** 用 unsigned128 exact products 比较 **child ordered OUTPUT** 与 **canvas pre-transform INPUT**，不是 raw child pasp、tile0或 shared base-property flag。derived canvas **只用自身 explicit pasp**，不把 child/SPS/CM 值塞入 container provenance。已知 ratio 不同、非方形边任一侧缺席，在首 callback/hardware 前 ENOTSUP；旧 absent/square 路径原样保 metadata，**没有合成1:1或称已证明 square**。每个 `render` 完成真实 child crop/mirror/rotation（含既有 explicit LINEAR_CEIL）后发布自身 cached raster OUTPUT pair；parent 的**真实 sample-copy path 再消费/核该 pair**，再做整数 row-major placement/末边裁切/identity，最后执行 parent ordered transforms。compose 不再只在最终 root 手写 planner ratio；native 仍核实际 source CM/CV、同尺寸 Normal transfer、completed root pair 与 plan 一致，再用真实 owned export source/CV 输出 metric。所有 item info 补独立 SourcePixelAspectRatio/Origin 和 RasterPlacementPolicy，保 raw Properties/pasp；首硬解前完成 owned metadata 分配。原预算/缓存/释放/颜色政策不改，无新 AU cache/metadata pass。
- **范围/精确编译结果**：manifest 新 **`heif_pasp_agreed_grid_identity_lattice=true`**，但 **derived_geometry/runtime_verified/physical_provider/complete_image_compositor 仍 false**；非方形 overlay/aux、absent 继承、mixed-lattice resampling/full normative semantics 不支持，不以 gate/parser 代替完成。新唯一 **`x9/out/heif-agreed-lattice-final-20261116/`** source-head.txt=`196596e1f2bef7bea17c5bc24bd3ea0ae84e1347`，source.diff/commit/path、replay、command-results 保存。`compile-source.py` 九 TU iOS16 arm64 source analyzer **各 exit0、无 diagnostic**；正常 frontend dylib/client `-Wall -Wextra -Werror` compile/link/ad-hoc sign **exit0、无诊断**，frontend.log/.exit；build.py in-memory syntax **exit0**，driver compile-source.log/.exit **0**，句柄24805已 terminal。只 userland/private header 改动，没有 native kernel 对象/full kernel link/XNU/固件/boot，也没有测试脚本/套件/样本/人工向量/CLI、成功自建产物二次解包/hash/反汇编/复建比较。
- **CI/复放/下一具体边**：push 前后 `gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **各 exit0/`[]`**（gh-before-push.* / gh-after-push.*、delivery-results.json），无 CI run ID，句柄30337已 terminal。复放在活动树**新 sibling out**复制本批 compile-source.py、记录当次 HEAD，再 `python3 -I -B x9/out/<new-unique>/compile-source.py`，或按 replay 的正常 frontend 命令使用另一个新目录；不覆盖旧成功结果。下一不要重查 base-property byte/本批 spatial keys：完整 derived metric/inheritance 需沿已定位 CMPhoto **`_HEIFItemDecoderCreate → __createPixelBuffer 1a5d89508`** 或 **`CMPhotoGetInfoFromFormatDescription 1a5d7e1a0`** 的真实 output-aspect/geometry consumer 得到可消费语义，不能从相同 tile 尺寸/本次 agreeing-subset 推广；亦可继续 physical constructor3072e2/最高 owner，不猜 vendor ISA/重扫旧弱类。cold 最高 authority/VCP-owned IRQ-clock/真实持续 physical provider/session_ops、BS/FB 独立排他/durable dequeue、DPB/layout/scratch/cache/逐 buffer 退休/c006 真 ACK、linear NV12/P010/HYFBC、CCV 真 iOS16 消费者/149 payload/selected-SPS+147 适用性/output-order cancel-persistence、late emitted CLLI/其余 nested HDR/HDR-LUT-tone-map-chroma 实效、aux-alpha/monochrome Rext/prem/implicit lengths/其余合法 HEIF properties 和普通 VT-CMPhoto-ImageIO 自动入口全保留。无设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作；唯一最终 boot 继续等完整代码齐全，未调用 complete/blocked。本交接最后正常 commit/push，源码构建仍固定196596e1。

- **实际交接提交/收尾状态**：上述 NOTES 里程碑提交 **`b6c36ad0`** 已 push 成功（`196596e1..b6c36ad0`），本批 commit-handoff.log/.exit=0、push-handoff.log/.exit=0，句柄50355已 terminal。这只是交接，不改变五文件功能源码/有效编译 **196596e1**；所有编译/分析/delivery 句柄已 terminal，无后台或设备任务。最后此行正常 commit/push，不重编旧成功产物。完整 goal 未完成、仍 active。
- **收尾 wrapper 的准确异常**：再补记提交 **`88cb7120`** 的 commit-close/push-close **各 exit0**，但之后 `git rev-parse @{upstream}` 因此 checkout 未保存 feature 的 remote-tracking ref **exit128**，外层句柄93401 **exit1**；这是 Git tracking 检查失败，**不重标整个 wrapper 成功、不称编译/push 失败**，`close-upstream-failure.json` 保留。改用唯一准确 ref 的 `git ls-remote --heads origin refs/heads/feat/hevc-hw` **exit0**，remote HEAD 与本地88cb7120完全一致，tracked clean；remote-head.log/.stderr/.exit、close-results.json保留，句柄15878已 terminal。没有改变远端配置、重复push旧源码或重编成功件；本行最后正常commit/push，全部功能与编译仍以196596e1为准。

#### 2026-11-16 CMPhoto 真像素 consumer / VT output-attachment 发布与 upstream pasp gate（当前交接，无生产 delta）

- **活动树/源码未变**：从 tracked clean **`c7b39f1c1788af10c85fcb9fd6cc56de1de2755e`** 接回唯一 `.worktrees/hevc-hw` / `feat/hevc-hw`；有效功能与成功编译仍固定 **`196596e1`**。本批只有 ignored 原件分析材料和本 NOTES 交接，**没有生产源码改动、编译/复建、测试脚本/套件/样本/人工向量/CLI、成功自建产物二次检查、kernel/XNU/固件/boot 或 runtime**；不合 main、不启子代理、不改其它树/共享 `.local`，无手机/ADB/fastboot/mailbox/MMIO/SMC/分区动作。完整 HEVC/HEIF goal 仍 active，不能把本批 static evidence 当全目标或无法继续的外部 blocker。
- **独立原件资格（已保存，不重跑）**：材料统一 **`x9/out/heif-output-metric-source-20261116/`**。既有 `qualified-selected-original.json` **11 ranges**，前段新 `qualified-vt-and-copy-original.json` **9 ranges** 和 `qualified-copy-gate-original.json` **1 range**；本次沿真实 caller 再加 `qualified-root-dispatch-original.json` **7 ranges**（HEIFItemDecoderCreate/createPixelBufferFromImage/canvas callback/async completion/真实 __decodeItem/两个 block）及 `qualified-root-gate-helpers-original.json` **2 ranges**（CMPhoto typed number + CoreMedia FigSignalErrorAt），**共30 selected ranges**，对应五次 qualifier log/.exit **均0**，逐字匹配 retained root clone 的 Cryptex cache。CMPhoto UUID **`8c636e49-40af-38ac-87e6-4943a376eed2`**，VT **`4fedef35-6b79-3586-b249-238e04a4c951`**，CoreMedia **`737230ca-7a8d-32b8-91da-d58e680aa9e9`**。新四份 `cmphoto-root-output.asm` / `cmphoto-derived-dispatch.asm` / `cmphoto-aspect-number.asm` / `coremedia-error-return.asm` extraction **各exit0**、独立 command/stderr/exit；只核 Apple 原件，不是当前设备 runtime、自建产物或所有 CMPhoto 入口证明。
- **解开的 keys/stubs 与两个保留失败**：creation 四键实际是 **ExtendedPixelsRight/Bottom/Left/Top**，不是 pasp；颜色传播五键 **CGColorSpace/ICC/primaries/transfer/matrix 不含aspect**。`resolved-output-keys.json`、`resolved-vt-metric-keys.json` 保存 slide-v2 实解 **CVPixelAspectRatio/HorizontalSpacing/VerticalSpacing**，以及 **Normal/CropSourceToCleanAperture/Letterbox/Trim**。第一版 `resolve-output-keys.exit=1` 为五个 GOT 地址多写一位0，修正副本 `resolve-output-keys-final.exit=0`，不覆盖失败；第一版 `resolve-final-attachment-and-copy-stubs.exit=1` 是末 stub **ADRP+LDR+BR** 被误按ADD解析，opcode区分并用真实slide-v2指针修正后的 `*-final.exit=0`。后者 **`1a5e1d4b0→1d1b01dc0`** 在独立 `platform-copy-symbols.txt` exact匹配 **`__platform_memmove`**（nm exit0），不重跑。两个未精确命名的CGRect候选岛仍不标已证。其余 output/VT/final/root-dispatch/root-number resolver 均有独立 log/.exit0/JSON。
- **真 sample copy 与实际 CMPhoto→VT call，不等于派生 metric admission**：真实 canvas callback 的 block **`1a5d94a54`** 调 `__transferToCanvas`，结果在 **`1a5d94a9c`** 交 async completion；该 transfer 的 **`1a5d7cb28..cb80`** integer source rect/dest offset 调 `CMPhotoPixelBufferCopyCropRectToOffset`，其它路径走 WithRect→WithOptions→OddSafe→`__convertAndScalePixelBuffer`→VT。copy gate **`1a5d8abd8`** 查 format/extended pixels/subsampling/alignment，**函数内不查aspect**；copy **`1a5d80dd4`** 的 **`1a5d81174..811a0`** 真逐row platform memmove，row bytes=width×bytes-per-pixel、source/dest各自stride，不比较或继承metric。转换路径 **`1a5d7d6dc..6f4`** 真设 ScalingMode=CropSourceToCleanAperture，**`1a5d7d7f4..808`** 真设 SourceCropRectangle=x23（supplied source rect或实际CLAP）；**`1a5d7d940..94c`** 真transfer，岛 **`1a9450954→194ccd304 VTPixelTransferSessionTransferImage`** exact已证。只证明实际sample-coordinate consumer，不能跳过上游资格、error或final效果。
- **VT output-aspect 的完整实际发布边**：CreateScalingParameters **`194d01cb4`** 的 Crop分支 **`194d01e68..ec4`** 保显式destination aspect（若传入），直接到rect输出，不进source PAR/picture-aspect与Letterbox/Trim比例调整；Letterbox/Trim真调用metric reader，Normal **`194d021e4..194d02328`** 真调用 **`vtBufferCreateScaledPixelAspectRatio 194d029a8`**，source实际CVPAR缺席输出NULL，unchanged retain、changed进attachment creator（后一helper的精确reduction未提取，不猜）。真实 **BuildChain `194ec5ce4`** 在 **`194ec614c..6170`** 传 session+b8/c0 owned outputs，**`194ec665c..6690`** 真调scaling，**`194ec8110..8130`** 将非空CLAP/PAR加入真实dictionary，**`194ec8260`** 存session+438；真实 **TransferImage `194d1fd8c`** 在 **`194d202e4..20308`** 从source ShouldPropagate副本删除CLAP/PAR，**`194d2036c..37c`** 真合并session+438，**`194d204a8..b4`** 真CVBufferSetAttachments(destination,dict,ShouldPropagate)。不是孤立helper/metadata猜测，但仍须后续像素执行成功；rotation只观察先移除sourceCLAP/PAR，未证明最终swap/继承。
- **本次 decisive 上游证据，阻止从局部copy误放生产 gate**：真实 **`__decodeItem 1a5d72938`** 在 **`1a5d729a0`** 保当前reader=x1→x28；**`1a5d729fc..72a14`** 通过base vtable+30查询 **当前item CVPixelAspectRatio**。`CFGetAllocator`、`FigPictureReaderGetFigBaseObject`、`CMBaseObjectGetVTable` stubs exact已解。property **−12783** 在 **`1a5d72a18..20`** 允许继续，不就地制造1:1；成功取得dict后 **`1a5d72a2c..5c`** 读Horizontal/Vertical到sp+530/sp+380，**`1a5d72a64..70`** unequal跳 **`1a5d74eb0` w0=−16995**，**`1a5d74ed0`** 调 **FigSignalErrorAt `1915c2638`**，helper真保存/返回原x0（`1915c2668/2698`），回到x27、释放dict后 **`1a5d72a8c→1a5d746b8/746d0`** 返回错误，**没有final像素交付**。这是在 verifyPixelInformation/GetItemType 和 **`1a5d72ac0..af8` iovl/iden/grid分派之前** 的限制；因此下游Crop/sample-copy不能当Apple接受mixed-lattice派生图的oracle，也不能据此删现有child/canvas gate。原 number helper `1a5d8161c` 请求SInt32且忽略CFNumberGetValue失败返回，**不照抄其signed32/窄化行为**；生产unsigned32/positive signed64与exact conversion门保留。此结论只覆盖这条qualified dispatch；missing入口继续亦不证明最终absent继承。
- **整理/交付/下一明确动作**：`assemble-output-metric-evidence.py` 只消费已有JSON/asm、不重新提取或qualification，**exit0** 输出 **`output-metric-evidence.json`**，分别列observed、inference、失败及后续边；本目录command/log/exit和原件分开保存。本交接 `gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**（gh-handoff.*），**没有CI run ID/build/boot/runtime**，全部工具terminal、无后台。正常commit/push本NOTES到origin/feat/hevc-hw，交接hash可由 `git log -1 -- NOTES.md` 取，功能仍196596e1且不重复成功构建。下一获取**独立derived descriptive-property适用阶段/继承与最终output consumer**，不要再次把原 IFF current item+58 getter、base-property byte、孤立Crop/copy局部行为代替完整规范；若沿absent iden，实际下一helper **`CMPhotoPictureReaderCopyBaseReaderAndPropertiesForDerivedPicture 1a5e02a44`**（`1a5d73384`调用）位于该current-item gate之后，**尚未提取/证明语义**。亦可继续已定位physical constructor/最高owner；cold最高authority、VCP-owned IRQ-clock/真实持续physical provider-session_ops、BS/FB独立排他/durable dequeue、DPB/layout/scratch/cache/逐buffer退休/c006真ACK、linear NV12/P010/HYFBC、CCV真iOS16消费者/149payload/selected-SPS+147适用性/output-order cancel-persistence、late emitted CLLI/其余nested HDR/HDR-LUT-tone-map-chroma实效、完整derived placement/metric/inheritance、aux-alpha/monochrome Rext/prem/implicit lengths/其它合法HEIF属性及普通VT-CMPhoto-ImageIO自动入口全部仍必做。现manifest一致lattice子集true，但 **derived_geometry/runtime_verified/physical_provider/complete_image_compositor仍false**；完整goal不complete/blocked，唯一最终boot仍等全代码齐全。

#### 2026-11-16 absent-pasp identity → unique child ordered output → native 实际 metric 保留（当前交接）

- **活动树/实际功能交付**：从 tracked clean **`f5c3d575`** 接回唯一 `.worktrees/hevc-hw` / `feat/hevc-hw`。本次六文件生产源码 **`200e35bff8f785a1ab87031b7e213d6852cbc122`**（`200e35bf`）已 commit/push，属于实际 compositor/native 接线的 progress，不是 NOTES-only 或完整派生语义完成。未合 main、不改其它树/kernel/XNU/共享 `.local`，无子代理；完整 HEVC/HEIF goal 保持 active，不出最终 boot。
- **新增原件资格/实解属性**：新唯一 **`x9/out/heif-derived-base-reader-source-20261116/`** 保留本轮 helper `_CMPhotoPictureReaderCopyBaseReaderAndPropertiesForDerivedPicture 1a5e02a44` 的 asm/command/stderr/exit。`qualify-base-reader.py` 仅资格这一份 **1 selected original range**，CMPhoto UUID **`8c636e49-40af-38ac-87e6-4943a376eed2`**，逐字匹配 retained root-clone Cryptex cache，qualified-base-reader-original.json/log/.exit **0**；没有重资格前批30 ranges。slide-v2 GOT→variable→CFString 新实解 **`1dacc1e98 RotationDegreesCCW` / `1dacc1e68 MirroringAxis` / `1dacc1e08 Derivation` / `1dacc1e20 DerivationParents` / `1dacc1e18 DerivationDetail`**，resolved-base-reader-keys.json/log/.exit **0**。新六 stub exact 匹配 CFDictionaryGetValue、CFArrayGetTypeID/GetCount/GetValueAtIndex、CFRetain、CFDictionaryGetTypeID，resolved-base-reader-stubs.json/log/.exit **0**；CFGetTypeID/format等已解 stub直接消费旧JSON，不重跑成功resolver/nm。
- **观察与推断分开**：helper **`1a5e02c18..c70`** 要求 parents 为 typed array/count1、retain element0为返回base；最终 **`1a5e03008..3028`** 只输出base/crop/EXIF orientation，**没有aspect查询或输出**，不是完整descriptive inheritance helper。消费上一批已资格 `__decodeItem`：iden在 **`1a5d73334..73384`** 调该helper，**`1a5d73704..73778`** 对base递归真正decode，成功 **`1a5d73794..737bc`** 取CV输出→x28→final；无需transfer的实际分支 **`1a5d74468..7446c→1a5d7464c..74688`** 保同一x28并写最终CV输出。此路径支持保留唯一base实际输出，而**不证明所有入口接受缺席container/非方形SPS、所有transform metadata继承或完整HEIF规范**；上游current-item显式non-square gate仍是已观察事实。`identity-output-evidence.json` 与两份有界 selected-source.txt 记录观察、实现政策和未证边；无新增报告Markdown。
- **实际窄流与保护门**：`get_image` 原有 exactly1 child约束不变；`visit` 递归完成child ordered OUTPUT geometry后，先核identity input尺寸逐轴完全相同。**只有自身pasp缺席的iden** 保child OUTPUT pair/presence作为自身pre-transform INPUT，present使用新 **`X9_HEIF_ASPECT_IDENTITY_OUTPUT=4`**，absent仍0/0、origin0；独立 **`source_aspect_item_id` / native `SourcePixelAspectItemID`** 始终记录immediate child ID，不改raw Properties/pasp、不伪称自身SPS/CM。所有node的 `transforms` 只在child完成后计算/append一次，再执行自己的ordered crop/mirror/rotation和显式LINEAR_CEIL exact metric，DAG缓存不重复append、不污染resampling状态。coded source callback仍严格拒绝origin>COREMEDIA，不能伪造identity来源。plan仍检查**每条**explicit/grid parent edge；render的真实child完成→sample-copy再次核ratio，identity还核presence（已知square也不能悄悄丢attachment），之后才逐row memcpy/own transforms并发布cached OUTPUT。native每node独立发布input origin/ratio/child ID和output ratio，root实际compose→plan一致性→owned export→实际CV attachment只读核验继续消费新metric。explicit parent mismatch、grid缺席非方形metric、overlay/aux、颜色/预算/寿命gate原样；不继承grid、不引入mixed-lattice resampling或1:1默认。
- **固定源码的确切必要编译**：新唯一 **`x9/out/heif-identity-output-final-20261116/`** source-head.txt固定 **`200e35bf`**，source.diff/path/replay保存。复制前批compile-source.py到新目录，`python3 -I -B x9/out/heif-identity-output-final-20261116/compile-source.py`：items/compositor/metadata/coremedia/vt_image_metadata/vt_picture/native/CLI/vt_decoder **九TU iOS16 arm64 source analyzer各returncode0、无diagnostic**（analyze-results.json、analyze.log仅九条command、analyze.exit0）；正常frontend dylib/client **`-Wall -Wextra -Werror` compile/link/ad-hoc sign exit0**（frontend.log/.exit），build.py in-memory syntax **exit0**，driver compile-source.log/.exit **0**，句柄87501已terminal。frontend原log明确 **`physical VCP provider absent, no auto-injection`**，不当作硬解成功。本轮只userland/private header，所以无native kernel对象/full link/XNU/固件/boot构建；没有测试脚本/套件/样本/人工向量/CLI执行或成功自建产物二次解包/hash/反汇编/重建比较。复编只在当次固定源码用**新 sibling out**复制本helper、记录HEAD再运行，不覆盖或复检任何旧成功件。
- **push/CI与下一具体边**：本批 push-code **exit0**，gh feature run list **exit0/`[]`**（push-code.*、gh-after-code.*、code-delivery-results.json），无CI run ID，句柄22403已terminal；所有分析/编译任务结束。manifest新增 **`heif_pasp_identity_output_preservation=true`**，但 **derived_geometry/runtime_verified/physical_provider/complete_image_compositor仍false**。下一沿新helper实际调用的 **`CMPhotoPictureReaderGetImageGeometryFromPictureReader 1a5d84180`** 获取独立geometry/descriptive applicability阶段，再追grid最终canvas input/output metric；不能从identity保留推广tile0/grid继承，不能重跑本轮成功资格/keys或旧base-property/Crop局部路径。完整derived metric/placement/inheritance、aux-alpha/monochrome Rext/prem/implicit lengths/其它合法HEIF属性；cold最高authority/VCP-owned IRQ-clock/真实持续physical provider-session_ops、BS/FB独立排他/durable dequeue、DPB/layout/scratch/cache/逐buffer退休/c006真ACK、linear NV12/P010/HYFBC；CCV真iOS16消费者/149payload/selected-SPS+147适用性/output-order cancel-persistence、late emitted CLLI/其余nested HDR/HDR-LUT-tone-map-chroma实效及普通VT-CMPhoto-ImageIO自动入口全部仍必做。所有READY/authority/producer-exclusion/retirement/普通VT注册门保留，无设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作；未调用complete/blocked。本NOTES交接最后正常commit/push，交接hash由 `git log -1 -- NOTES.md` 取，有效生产源码/构建固定200e35bf。

#### 2026-11-16 derived descriptive 适用阶段 / 固定 Nokia 真 property loader（当前交接，无生产 delta）

- **基线/分类**：上一 goal turn 是实际六文件功能 progress（`200e35bf`），本轮从 tracked clean、已 push **`81ba19aa52ce353f2663b758ca5febc3f0bbd1e3`** 继续唯一 `.worktrees/hevc-hw` / `feat/hevc-hw`。取得新独立官方处理阶段和固定源树loader证据，改变下一调查入口；**没有生产源码delta，不能将本NOTES交付计作硬解功能完成**。有效源码/正常编译仍固定200e35bf与 `x9/out/heif-identity-output-final-20261116/`。不合main、不改其它树/kernel/XNU/共享`.local`、不启子代理，完整HEVC/HEIF goal继续active。
- **新唯一材料/官方适用阶段**：**`x9/out/heif-derived-property-reference-20261116/`** 保存当前 Nokia 官方 `https://nokiatech.github.io/heif/technical.html`（98879 B，HTTP200，technical.html/headers/command.json/download.log/.exit0）、去script/style的technical.txt及exact numbered摘录。原HTML **1992..2003** 明确：derived operation消费input的 **output image**；derived自身descriptive properties一般描述 **reconstructed image**，自身transformative properties再将它变成output，output可供下一derived输入。原HTML **1747..1756** 将pasp列为descriptive，并引用14496-12 **syntax**；**没有grid缺席spacing继承、缺席默认或explicit canvas与input metric冲突的规范解法**。这支持现有child ordered OUTPUT→parent INPUT的阶段顺序，不足以删placement gate、从tile0继承或称完整ISO规范/Apple运行证据。
- **固定原始源树的真load流**：沿已保留Nokia tree **`363cf942bd45ec6772c52b04d7d0b016801c2db4`**，新下载五个指定源文件，git blob逐字身份各匹配（仅原件reference，不是自建产物复检）：DerivedImageItem.cpp **c622af74cf5c8b05427b3c4e74f4ff224ab168b3**；ImageItem.cpp **70f8d395bc05528348c25ae28215a0a6ca14f0fb**；GridImageItem.cpp **5eae42d8a6e39771e464b1cbedb8ceb71681d67f**；Item.cpp **030e9bbcaf0305f76c5b24c86a300a6c3ce34169**；IdentityImageItem.cpp **ea83938153c4eb0cdfbb87d3999a8f78a91c99d1**。两个curl helper各exit0、五次curl各exit0，selected-reference-curl-results.json/selected-property-loader-results.json保存URL/固定tree/blob/命令/returncode和独立headers/logs。实际 **DerivedImageItem::load→ImageItem::load→Item::load** 先按 **当前aId** 的getItemProperties构造/add本item属性，再读own宽高，随后按ordered dimg构造各source；ImageItem::pixelAspectRatio只查询本property list。Grid load核own输出尺寸/保存ordered source IDs，identity load要求unique source；这些源流**没有通用child-property继承，也没有实际pixel renderer**，不可当作missing grid spacing oracle、normative排除所有继承或已编译runtime。
- **收束旧geometry候选，不重复成功材料**：前条下一helper `CMPhotoPictureReaderGetImageGeometryFromPictureReader 1a5d84180` **早已在上一批原件asm和qualified-selected-original.json中**，本轮只消费，不重提/qualification/resolve/nm。真实 **`1a5d841e0..841f8`** key是 **PictureDimensions**（GOT1dacc1e88），**`1a5d84260..84278`** 是 **CVCleanAperture**（GOT1dacb44a0）；输出size **`1a5d8434c..84358`** 来自原PictureDimensions，另输出clap crop且有独立clip模式，**不是ordered rotation输出尺寸/aspect、更不是通用继承**。不照抄其double/truncate/round-to-integer为本项目exact metric政策。`stage-evidence.json`引用现有576B qualified range及已解keys，记录新官方阶段/五份原件loader/观察vs推断和下一边；新numbered extracts、必要raw inputs和replay保留，未新增报告/timeline Markdown。
- **明确失败与下一动作/保持全目标**：初版 `fetch-selected-reference.py` 因Python默认SSL trust store缺issuer **exit1**，没有HTTP响应；fetch.stderr/results保留，不重标成功或关闭证书核验。改用系统curl正常TLS，各新请求成功，两个helper终结句柄37162/38875均已terminal；不重复已成功下载/原件资格或自建产物复检。**下一从“取geometry/helper或Nokia getter推继承”改为直接取得reconstructed-image pasp适用/缺席/覆盖的独立exact规则，才改grid最终sample-placement与native consumer；不能把泛用处理阶段当spacing裁决**。亦可继续既有physical constructor/最高owner窄流，不让此图像metadata线代替全目标。identity保留和原explicit/grid/overlay/aux门不变，derived_geometry/runtime_verified/physical_provider/complete_image_compositor仍false；cold authority/IRQ-clock/持续physical provider-session_ops、BS/FB排他/durable dequeue、完整DPB/layout/scratch/cache/逐buffer退休/c006、linear NV12/P010/HYFBC、CCV149/真实iOS16消费者与output-order状态、late emitted CLLI/其余nested HDR/tone-map-LUT-chroma实效、完整derived metric/placement/inheritance、aux-alpha/Rext/prem/implicit lengths/合法HEIF属性及普通VT-CMPhoto-ImageIO全保留。没有analyzer/frontend/native/kernel/XNU/固件/boot复编、测试/样本/人工向量/CLI执行、设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作，gh feature list **exit0/`[]`**（gh-handoff.json/.stderr/.exit，句柄43575已terminal），无CI run ID/build/boot/runtime。全部工具终结，未调用complete/blocked；本NOTES正常commit/push，交接hash由 `git log -1 -- NOTES.md` 取，功能/成功构建仍200e35bf。

### HEVC session 结果保留与显式交付确认（2026-11-16，本轮活动交接）

- **基线/实际生产进展**：从 tracked clean、已 push `d216023f` 继续唯一 `.worktrees/hevc-hw` / `feat/hevc-hw`；六文件功能源码 **`5ae0086d`** 加 kernel-only aligned ACK snapshot修正 **`741ad4d2`** 均已 commit/push 到 `origin/feat/hevc-hw`。新增的是实际 kernel session→VT 消费链的结果交付事务，不是 physical provider、固件 free ACK 或完整硬解验收。完整 HEVC/HEIF goal 仍 active，不合 main、不改共享 `.local`/其它树、不启子代理，唯一最终 boot 仍等全部代码齐全。
- **源码依据/排除项**：`x9/out/hevc-session-delivery-final-20261116/` 保存 base-head、source.diff、原 baseline session 源码、XNU `sys_generic.c:1009..1015` exact 摘录、bus route lock 摘录和 stage-evidence.json。XNU 先 `fo_ioctl`，成功后才进行 generic `IOC_OUT copyout`；旧 `session_receive` 在驱动返回前已 release frame、删除 token。旧 header 明确记载此局限，本次是闭合已知交付缺口，**不是新 runtime 漏洞证据**。上一 worker/reverse lock 候选已排除：`route_packet_begin/end` 都先释放 `vcp_bus_lock`，再调用 foreign `route_enter/leave`；没有猜测性死锁补丁。UniXNU selected-source 查找和 hackios `inject_media.c` 不覆盖 X9 clone-session 事务，未移植软件 decoder。
- **实接线/保持的门**：新 `SESSION_PEEK` 用同一严格 output schema、原 token/completion/layout 和 CPU-owned frame；payload 或最终 ioctl struct copyout 失败均不 release/delete，重读复用 held frame、不再次 provider.receive。成功 peek 后 legacy RECEIVE 返回 EBUSY。新 input-only `SESSION_ACK` 核当前 clone/session/generation、非零且 exact held token/completion、peek_ready/pending/cookie，才一次 CPU release+移除 token；错配/重复 pair 返回 ESTALE。GET_CAPABILITIES 只在 ready resident sessions 下额外发布 host `OUTPUT_ACK`，provider 自身 required=15 不变，VT 必须 require delivery=31、没有旧 RECEIVE fallback；VT 完整核结果/真实 output POC 后 ACK，ACK 成功后才改 output-order/pending_take/原 immutable CM 描述的 emit。close/DESTROY 保留 generation loss 下释放 held CPU frame和真实 provider.destroy 的既有路径。ACK **不**证明 DMA fence/缓存/producer排他/逐buffer退休/c006；真实 provider 必须保持 frame/planes/cookie 至 CPU release 不变。
- **固定源码的必要构建/无旧件复检**：`x9/out/hevc-session-delivery-final-20261116/source-head.txt` 固定 **5ae0086d**；复制上批compile-source.py到此新目录：九 TU iOS16 arm64 analyzer 各returncode0、无diagnostic，正常frontend dylib/client `-Wall -Wextra -Werror` compile/link/ad-hoc sign **exit0**，build.py in-memory syntax/driver **exit0**。`compile-native.py` 用正式 Builder/recipe、关闭ccache，仅改动 `sweet_x9_vdec.o` **exit0**；原 native log 有71 warnings。核本次log后对新增ACK改用 aligned local `memcpy` snapshot，并将新增index声明移到block头；这是新生产delta741ad4d2，**只**在新 sibling **`x9/out/hevc-session-delivery-aligned-final-20261116/`** 正常编该native对象 **exit0**（69 warnings：65 declaration-after-statement、4既有 caddr_t typed casts；没有 ACK cast/index新增warning）。该kernel-only修正不改frontend/header；其有效正常frontend/analyzer仍5ae0086d，**未重编或复检成功frontend**。两目录保留helpers/logs/exits/result/source.diff/HEAD/replay，final-state.json在aligned目录汇总。原frontend log明确 **`physical VCP provider absent, no auto-injection`**；不能当runtime硬解成功。
- **交付/CI/下一实际缺口**：code push **exit0**（d216023f→741ad4d2），gh feature list **exit0/`[]`**，aligned目录保存push-code.*、gh-runs.*，无CI run ID。全部compile/gh/git工具terminal，句柄29393/53863已结束。没有测试/样本/人工向量/CLI、完整kernel/XNU/full-link/固件/boot构建（仅上述单native对象编译）、成功自建产物二次解包/hash/反汇编/重建比较、设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作。本交付只闭host result-copyout事务，next回到真实 **cold owner.power_token→normal controller/storage/ASID/CMDQ owner→DMA create/install→shared publish→bus bind/start** 的最早未证authority和生命周期，不能把guard/回调契约当已注册物理owner；亦不重跑已排除的constructor3072e2 ISA/弱mask/table类调查。
- **全目标继续/不偷换退休含义**：cold最高authority/VCP-owned IRQ-clock、持续physical provider/session_ops、BS/FB独立producer排他/durable dequeue、DPB/layout/scratch/input-DPB-output cache/逐buffer退休/c006真free ACK、linear NV12/P010/HYFBC真转换；CCV149真iOS16消费者/selected-SPS+147适用性/实际output-order cancel-persistence、late emitted CLLI/其余nested HDR及HDR-LUT-tone-map-chroma实效；完整derived metric/placement/inheritance、aux-alpha/monochrome Rext/prem/implicit lengths/其它合法HEIF属性及普通VT-CMPhoto-ImageIO自动入口仍全部必做。新delivery runtime_verified与physical_provider/hardware_tested仍false，READY/authority/producer-exclusion/retirement/普通VT注册门保留。没有调用complete/blocked；本NOTES最后正常commit/push，交接hash由`git log -1 -- NOTES.md`取，实际生产恢复点741ad4d2。

### HEVC cold owner：EL2 shadow / donation / 真 ops 安装来源（2026-11-16，当前活动交接）

- **基线/交付范围**：从 tracked clean、已 push **`00c07d6b`** 继续唯一 `.worktrees/hevc-hw` / `feat/hevc-hw`。新源码提交 **`8448ea94`** 在 `x9/tools/vcp_artifacts.py` 增加 `pkvm_smmu_owner_evidence()` 并接入真实 `module()` 的 `pkvm_smmu` 分支；这是可重放的**原件离线证据工具改动，不是 native cold provider 或硬解生产 delta**。上一生产源码/成功构建仍为 `5ae0086d`＋kernel-only `741ad4d2`，不重编/复检其成功件。本轮未改 frontend/native/kernel/XNU/main、共享 `.local` 或其它树，没有子代理，完整目标 active、唯一最终 boot 仍等全部代码齐全。
- **承接上一轮未写入的原件窄流**：**`x9/out/hevc-ns-el2-share-source-20261116/`** 保留原 `pkvm_smmu.ko` 的七个新 `.hyp.text` 函数：share `32a8/38c`、DABT `542c/848`、check STE `614c/12c`、write STE `3f8/68`、issue CMD `4d8/40c`、physical queue wait `abc/11c`、error recovery `8e4/1d4`（offset/bytes）。父原件沿用 `hevc-vendor-source-20261115/extraction.json` 的 158896 B / recorded SHA256 `d77ff428fcf8b2e7c0931f051b9e33033174b9f674540e665a3da4b5dafff546`，**未重 hash 或 qualification，loaded pairing=false**。本轮仅消费旧 raw/asm：share **32cc** 先写 guest regs0=0，多条错误路径仍返回0；CMDQ **5578/557c** 保存 guest base/PA，却在 **5658** 写 EL2-selected physical backing，STE **57dc** 保存 guest 字段而 **5828** 写 global host backing。guest CR0 读 **56b8→device+70**；physical CR0/ACK相等后 **5790/5794** 发布 guest ACK shadow；PROD **579c→+38**、CONS **54ec→+3c**，base读也走 `+30/+58`。因此**若该 handler 实际被安装并执行**，AP 读数可能是 shadow，不能凭它们相等推 physical bank/NS delegation；**并未证明当前 XNU boot 存在该 protected handler**。
- **实际 mutation / error-repair 闭环与排除项**：check STE **61d4..620c** 构造 `(guest.word0 & ~0xd) | (old_host.word0 & 0xd)`，word1/4..7来自guest、host word2/3保留；write STE **404** 先清word0，再写64B、flush、`dsb sy`。physical wait **b64→8e4** 可修复错误命令；**9e8** 将原命令替换为 `{0x46,0}` plain CMD_SYNC，**a98..aa0** 对 physical GERRORN（base+64）写原值 XOR1。DABT **5b80** issue失败记录后仍可继续，**5bb8** 保存guest PROD，physical wait返回1才 **5bc8/5bcc** 将guest CONS设为PROD；队列相等不证明原命令未被替换。回到 native **`x9_mm_smmu.c:1030..1060`** 已有 clear-V→sync、完整STE zero→**第二次 invalidate_context**，才清CD V/完成退休；不添加“native仅清V一次”的猜测修复，不将guest V或ACK冒充真实S2/AXI/cache退休。
- **新独立 owner 安装范围与代码**：**`x9/out/hevc-ns-el2-owner-install-20261116/`** 新选 `.hyp.text` `add_smmu_device 49c8/190`、`mtk_iommu_init 4ea8/56c`、`smmu_hyp_init 5e88/68`、`.hyp.data smmu_ops 0/c8` 和 AP `.text mtk_kvm_arm_smmu_v3_init 1ed4/118`，四次 exact-address objdump各exit0，不重跑旧nm/AP qualification。device **49e0** 先写guest result0，两次malloc 128/56B；**4a68** 调 ops+e0（source-correlated host_donate_hyp，arg2=1），只有donation返回0才 **4b38** 保存HYP VA、**4b40** 递增active device count。pool **4ed0** 先清result0，复制1608B page list；**5170** donate arg2=0、失败log后继续，仅成功 **50d8** 记pool，**534c** 为pool加nomap保护；初始share失败 **4f60/4f64** 甚至可返回guest `{0,0}`。这些是**donate给EL2**，不是委托给XNU或SIDa0/b0 DMA grant。AP **1f1c/1f20** 真重定位 `smmu_ops`，经module HYP address转换后 **1f50** 调外部 `kvm_iommu_init_hyp`；`.hyp.data+50` RELA精确指向DABT **542c**。工具独立核五个**selected范围**的全字节SHA及14个linked RELA，输出13个aligned spans；match只指这些范围/重定位，**不提高到whole-module或runtime identity**。source相关名字仅解释，外部KVM install body和finalise S2 snapshot未捕获，不补成功oracle/注册者。
- **可重放/确切结果/失败保留**：最终 **`x9/out/hevc-ns-el2-owner-source-final-20261116/`** 保留 `extract-owner-profile.py`、`pkvm-smmu-owner-profile.json`、`saved-assembly-coverage.json`、stage-evidence/numbered source、source-head=`00c07d6b`＋source.diff（对应8448ea94）、code-head。Python in-memory syntax **0**、首次新owner profile提取 **0**、四个完整新function的saved-asm exact-PC/raw覆盖通过（没有重跑objdump）。旧share目录补齐stage/七函数saved覆盖、suffix.command与真正replay helper；初始DABT symbol-disassembly虽rc0但只覆盖到 **5938 exclusive**，已保留原 **5938..5c74** suffix，不覆盖初始材料。原两行placeholder另存 `.placeholder.py`；新replay helper只syntax检查0，**未执行重提旧成功范围**。首次packaging误把第二JSON list按dict读取 **exit1**，failed helper/log/exit保留；修正后 `package-fixed.exit=0`，不把失败标成功。重放share用原helper `--out x9/out/<新唯一名>`；重放owner将final helper复制到新sibling目录再运行，exclusive-create，不调用旧module全资格/nm。
- **当前停止/下一最早入口**：code push **exit0**（00c07d6b→8448ea94），`gh run list --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0/`[]`**，final目录保存push-code.*、gh-runs.*；无CI run ID，工具句柄58551已terminal，无后台任务。本轮无测试/样本/人工向量/运行CLI、Clang analyzer/frontend/native/kernel/XNU/固件/full link/boot构建、成功自建产物二次解包/hash/反汇编/重建对比，或设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作。下一从**新独立 `kvm_smmu_v3_ops.init_driver` 重定位、外部 `kvm_iommu_init_hyp` 安装和 finalise host S2 snapshot**追实际active owner，或取得独立direct-bank cold delegation证据；不再从AP share/HVC0推NS授权，不重跑本轮成功范围。cold最高authority/VCP-owned IRQ-clock与持续physical provider/session_ops、BS/FB独立producer排他/durable dequeue、完整DPB/layout/scratch/cache/逐buffer退休/c006真free ACK、linear NV12/P010/HYFBC真转换、CCV149真iOS16消费者/selected-SPS+147/output-order状态/late CLLI/其余nested HDR及LUT-tone-map-chroma实效、完整derived metric/placement/inheritance、aux-alpha/monochrome Rext/prem/implicit lengths/其它合法HEIF属性及普通VT-CMPhoto-ImageIO自动入口均继续必做。全部READY/authority/producer-exclusion/retirement/普通VT门保留，不合main，不称HEVC/HEIF可用；本NOTES最后正常commit/push，恢复hash以`git log -1 -- NOTES.md`为准。

### HEVC SMMU guard 正式编译 / EL2 finalise 实际边界（2026-11-16，当前活动交接）

- **活动基线与真实进展**：从 tracked clean **`a2b3aaec`** 续接唯一 `.worktrees/hevc-hw` / `feat/hevc-hw`。先补齐该生产窄修的正式编译，再消费上一 turn 已保存但未交接的 finalise 原件，不重复提取旧成功函数。本轮新提交 **`13643b42`** 为 `vcp_artifacts.py:pkvm_smmu_finalise_evidence()` 及真实 `module()` 分支连接，属于原件离线证据工具，**不是冷启动 provider 或硬解生产完成**。完整 HEVC/HEIF 目标保持 active，不合 main、不启子代理、不改其它树或共享 `.local`。
- **上一生产窄修的确切编译**：`a2b3aaec` 在 adopt 的失败 pin/storage/capture 后不再落入 CMDQ MMIO 读取；camera leave 只在成功 guard、真实 completed queue 与可读 STE 后更新 ledger，失败保留旧 ledger，不将未知 queue 记为退休。按正式 Builder recipe、禁 ccache 编译 **`x9_mm_smmu_owner.o` exit0 / 19 warnings**。输入 head/diff、helper、完整日志/exit/result/object 在 **`x9/out/hevc-owner-guard-final-20261116/`**。未改 frontend/shared parser，因此不重编它们；无 full kernel link、XNU/固件或 boot 构建。这只是源文件编译证据，未有物理执行验收。
- **复用已保存的 finalise 原件**：实际先发现 **`x9/out/hevc-ns-el2-finalise-source-20261116/`** 已存在（11:16，三次 exact-address objdump 原 exit0），目录创建拒绝后没有覆盖/重跑。新收尾目录 **`x9/out/hevc-ns-el2-finalise-final-20261116/`** 保留独立 exact profile 与三份旧完整函数 saved-PC/raw coverage。五个 selected anchors + 17 个 linked RELA 核验成功，**只限定这些范围/重定位，不资格化整 KO、loaded pairing 或当前 XNU owner**；没有重 hash 父原件或重跑旧 owner/share profile。首次临时字段查询误用不存在的 `ELF.relocations`，exit1，仅为查询失败；改按真实 linked symtab 读取，没有据其构造地址或运行动作。
- **闭合 driver 注册与排除假 ready**：原 `.data kvm_smmu_v3_ops+0` 为 section-symbol+addend **`.text+1ed4`**，不是 NOBITS 的运行值；原 `.init.text 24c/250→254` 真实重定位至该 object 和外部 `kvm_iommu_register_driver`。EL2 `.hyp.data ops+0` 则为 **`.hyp.text+5d34`**；新唯一 8B 原件 disassembly **`mov w0,wzr; ret`**（ops-init exit0）明确它无 hardware 初始化，不能拿 init0 当 cold readiness/delegation。外部 register/init_hyp body仍无 captured binary；固定 reference 的 cmpxchg-release、acquire driver init→HYP init 只解释其预期调用，不能提升成实际安装证。
- **finalise 与真实 stage-2 consumer**：原 **`5cd8..5d30`** 经 retained module-ops **`+1c0`**、domain argument0 在 **`5d14`** indirect call，**`5d18/5d1c`** 将 signed32 callback result写 guest regs0；没有源码 `snapshot_done=true` 的 store（该未消费 source 状态不可用作 readiness）。新精确 ops slots **`+58→5d6c`、`+60→5e64`**：idmap必须两次 VM0/1 range-check均成功才进入 ticket lock；size为end−start的low32，prot0走VM0 unmap，非0且mask6命中走map(permission7)，map/unmap返回不被消费，callback没有逐range error返回。complete(map=true)直接return，只有map=false才call真实sync **`5e74→2dc0`**；不把 void callback/一次snapshot result 当逐buffer DMA/S2退休 receipt，不移植low32范围截断。
- **外部 source 的严格边界**：新 numbered spans 是固定 OPPO reference 的 host `iommu.c:56..99`、HYP `iommu.c:266..304/751..812`、module-ops成员、`pkvm.c` finalize顺序与模块 finalise 源码，索引在 `reference-spans.json`。source 的 snapshot under host_mmu.lock、walk整个ia_bits、只有walk0才release-publish idmap-ready，解释为何必须完成真host S2安装；**并未捕获这段外部原件，也未证明 void module callbacks全部修改成功**。这是 Android EL2 host-S2→IOMMU机制，不是 XNU SIDa0/b0 lease。现 shim 的 EL1/EL2 分支也不能单凭源码决定本 boot 是否有这些 protected handlers，更不凭 guest ACK/shadow 相等授予 direct-bank authority。
- **实际交付与未执行项**：code push **exit0，`21e8d1f4..13643b42`**（含此前未push的a2b3aaec），`gh run list --repo tosasitill/ios_plj110 --branch feat/hevc-hw --limit 3 --json databaseId,status,conclusion,headSha` **exit0 / `[]`**，无CI run ID；push句柄65626和gh句柄80577均terminal。Python in-memory syntax与独立 profile提取exit0，日志/helper在final目录；这不是测试套件/人工向量。没有 Clang analyzer/frontend/native其他对象复编、成功自建产物二次解包/hash/反汇编/重建对比，或设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作。必要交接最后正常commit/push，恢复hash以 `git log -1 -- NOTES.md` 取。
- **下一准确动作/完整 scope**：已闭的 stock ops-init literal0、driver RELA、finalise及三段idmap不再重复当新进展。冷启动需真实 **direct-bank NS delegation 与共享 storage/writer owner**，或独立 captured external KVM install/snapshot及当前boot active-owner证据，不能再把 stock init0/share/HVC0/snapshot0拼成成功 provider。持续 physical `session_ops`、VCP-owned IRQ/clock、BS/FB独立producer排他与durable dequeue、完整DPB/layout/scratch/cache/逐buffer退休/c006真free ACK、linear NV12/P010/HYFBC真转换；CCV149真iOS16 consumer/selected-SPS+147/output-order cancel-persistence、late emitted CLLI/nested HDR与LUT/tone-map/chroma实效；完整HEIF derived metric/placement/inheritance、aux-alpha/monochrome Rext/prem/implicit lengths/其它合法属性及普通VT-CMPhoto-ImageIO自动入口仍必做。全部READY/authority/producer-exclusion/retirement/普通VT门保留，**没有最终镜像，唯一最终boot继续等全部代码齐全；不标complete或blocked**。

### HEIF agreeing iovl lattice → 真实 source-over（2026-11-16，当前活动交接）

- **活动树/生产提交**：从 clean `15083bc0` 继续唯一 `.worktrees/hevc-hw` / `feat/hevc-hw`，新增 **`4793ca00`**。这次改的是实际 compositor 像素消费，不是新增无人调用的 metadata/API。没有合 main、清理活动树、开启子代理或改其它工作树/共享 `.local`。完整 HEVC/HEIF 目标仍 active；先前交接的其余缺口全部保留，不以本子集称完整硬解可用。
- **新闭合的窄路径**：原 `plan_create` 对 `iovl` 及其子图 blanket 拒绝任一 source/output 非方形 metric。现在同 grid/iden，逐条 ordered dimg edge 比较**子图完成变换后的 OUTPUT 比例**与**此画布变换前的 INPUT 比例**，用已有 unsigned128 cross product，不比较 raw child pasp 或画布后续变换 metric。适用于 declared canvas 与所有 child lattice 比例一致的非方形 source-over；非零16/32位 signed offsets 继续是 sample 坐标，clipping/blending 不缩放offset、不resize子图。匹配依据仍是明确的 raster placement contract，**不是 full normative derived inheritance 的证明**。
- **真实消费与拒绝边界**：whole graph preflight 检查每条父边，包括 DAG 重用与 fully invisible overlay；真实 `render` 在 visible cached child source-over读像素前再次比较其 completed OUTPUT metric。原线性光 premultiplied RGBA float、signed64交集、finite/alpha检查、heap budget、画布自身有序 crop/mirror/rotation、最终 raster→CV aspect发布和只读复核保留。显式比例不一致或非方形所需metric缺席仍 ENOTSUP；不从子图给 grid/overlay 推 absent metric、不填1:1、不实现mixed-lattice resampling。aux/prem的compose/native preflight gate未松；不能由此称 auxiliary alpha 解码完成。`heif_native.c` 真实结果metadata和 `build.py` manifest同步准确边界。
- **确切正常构建**：按 `x9/hevc/build.py`，`arm64-apple-ios16.0` / `-Wall -Wextra -Werror`，全部 iOS dylib/client 编译、链接及 ad-hoc 签名 **exit0，无诊断**。输入 head=`15083bc0`＋完整源码diff、exact argv、原始log/exit与产物在 **`x9/out/heif-overlay-lattice-20261116/`**（`frontend/`、`frontend.log`、`frontend.exit`、`build-command.json`、`source-head.txt`、`source.diff`）。工具句柄29460已terminal。重放仅正常构建并选新唯一out：`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I -B x9/hevc/build.py --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer --out x9/out/<新唯一名>/frontend`，预先exclusive-create父目录，不能覆盖本次或旧产物。没有测试脚本/套件、人工向量、Clang analyzer、kernel/XNU/固件/full link/boot构建或成功自建产物二次解包/hash/反汇编/重建比较；编译通过不等于已实跑非方形overlay或physical decode。没有触发CI run，最后已记录的feature gh结果仍`[]`。
- **补齐两个已读的下一步约束**：既有 `/Users/tosasitill/321.txt:3` 是 `boot EL=...1`；`start.S` 保存entry CurrentEL至x20，经`x20>>2`传给`shim_main`直接打印，故仅证明该日志的**EL1 entry**。不能用自己的EL2→EL1分支（HCR RW-only normalization）声称这次运行清了S2或获得30800000 direct-bank权限；321仍无唯一镜像identity/current EL2 owner pairing。当前 Xcode SDK `CMFormatDescription.h:878..879` 的 `kCMFormatDescriptionExtension_ContentColorVolume` 为 **ios(17.0)**，不是iOS16可直接链接消费的key。旧20H392原件typed parser无149分派，仅排除该分支，不证明所有private消费者缺席；不凭SDK注释新增自定义CCV输出或放开色彩gate。这两项只读事实无新设备抓取或原件重反汇编。
- **下一实际动作**：保留cold最高NS delegation/shared storage/writer owner、VCP IRQ/clock与持续physical session_ops、独立producer排他/durable dequeue、DPB/layout/scratch/cache/逐buffer退休/c006真free ACK及linear NV12/P010/HYFBC转换门；普通VT→CMPhoto→ImageIO自动入口仍未开放。继续从尚未闭合的真实输入/消费者或物理provider新证据推进，**不重跑已闭的stock pKVM init/share/finalise**，不重编未改的owner对象。CCV149/selected-SPS+147/output-order cancel-persistence/late emitted CLLI/nested HDR/像素实效、完整derived descriptive placement/inheritance、aux-alpha/monochrome RExt/prem/implicit lengths/其他合法HEIF属性仍未完成。无手机/ADB/fastboot/mailbox/MMIO/SMC/分区操作，无最终boot，不标complete/blocked。源码与本交接正常commit/push后，恢复HEAD以`git log -1 -- NOTES.md`为准。

### HEVC RESET → 两种 H265 null-decode 边界（2026-11-16，当前活动交接）

- **活动树/实际代码**：接干净且已push的 `d8c7bd17`，仍只在 `.worktrees/hevc-hw` / `feat/hevc-hw`，源码提交 **`1dca28d8`**。新增 `rv55_reset_evidence()` 并接入真实 `firmware()` 的 `service_reset_evidence`，新增 **33** 个独立原件指令区间。此里程碑是新RESET/codec契约证据代码，**没有新增native drain/session provider或硬解验收**。不合main、不清理活动树、不触其它树/共享`.local`，无子代理；完整goal active，READY/authority/producer-exclusion/retirement/普通VT门继续关闭。
- **上轮未记交接的RESET原件**：`x9/out/hevc-reset-path-source-20261116/`保存selector/request/candidate-body frames，rv32_slice均exit0。标准 **305d74→30626c**分派 **a003**，request **+8**于3062ae成为s11实例alias，**+16**于306290作为log参数；可见错误路线306304/30630a赋 **−14**，30630c/306314构造 **b003**，30631c→30b55e使用40B ACK helper。**3062d2 `e7110532`仍是vendor instruction**；306df4仅来自标准B-immediate布局假设，独立framing不证明incoming CFG edge。candidate内306eb4→270222传a1=0/a2=0，306eb8保存result于stack80，**306eba标准jump→307864**，不当成功drain。
- **本轮generic→codec新窄链**：独立270222..27047c覆盖两个wrapper，270222于270324 ret，后为270326新入口。前者name **270bda `eVDecDrvDecode`**；**270246标准beqz s1→2702bc**，加载owner **+24 callback / +80 context**，2702c4标准C.JALR传原s1/s2、2702c6保存result。NULL arm有 **270c83 `eVDecDrvDecode EOF`**、software bytes1764/1765与2702f8→26f91a，**不证明DPB或DMA退休**；return table2710b0与vendor remap索引保留，不把callback值直接当errno。270326另有owner+56 callback/2703a0及+24 fallback/270404。captured H265 table3356b0 **+8→2cd1ce / +40→2cd924**、原generic create的ops目的owner+16均保留，不能由static copy/table presence推runtime selection。
- **真实两种H265 NULL wrapper/最早未证stage**：新独立2cd1ce..2cd362及2cd924..2cdab8均exit0；标准 **2cd1e8→2cd290**、NULL call **2cd2b4→2d3946**；另一标准 **2cd93a→2cd9de**、NULL call **2cda0c→2d394e**（原name `h265_dec_decodeEx`，log `Flush buffer`）。2d3946做 **a6=incoming a5 / a5=0**，2d394e做 **a5=1 / a6=0**，标准tail **2d394a/2d3952→2f8fa2**，不是可互换的flush实现。新common-entry保存input a2到stack192、2f9082 reload，但 **2f9084是vendor gate**；NULL下游路径及DPB/reference retention未闭，不按猜测target再扩成成功CFG。
- **307864 suffix复用旧成功frames**：直接消费既有 `hevc-fb-owner-caller-20261116/dispatcher-late-chunks-through-constructor-return-jump.json`，不重复r2。307868→30b4e6传mode0；新helper窗保留instance+4960/codec+5008条件分支，不当halt。307874再读request+16，labels **30e007 reset(flush) / 30e014 reset(drain EOS) / 30e025 reset(drain)**，但 **307894/307898 vendor selection**未知。3078c4/307946是owner+6536 mutex，64-entry token pair stride8 / descriptor stride464循环是 **dump/log**，不是clear/free；30795a reload stack80后307964→30a432，不授予DMA完成。其它chunk30783c→270326、NULL a1也只保留标准call，incoming request路径未证。固定reference `vdec_ipi_msg.h`的drain_type0/1/2、`vdec_vcp_if.c:1476..1499`的24B a003及`vdec_common_if.c:162..184/209..211`解释NULL bs→RESET不同于empty a001；**flush_type BITSTREAM1/FRAME2是另一字段，不能混为drain_type**。reference不能替代vendor/DPB证据。
- **准确验证/产物/重放**：六个新rv32_slice windows、exact command JSON/log/exit、labels、ops table、prior-frame-coverage与两个extract helpers均在 **`x9/out/hevc-reset-continuation-20261116/`**，全部slice exit0。`syntax.log/.exit=0`（正式py_compile，未跑测试/人工向量）；`extract-reset-exact.py` **exit0**，33 spans逐条exact-PC/raw/boundary匹配、额外精算 **20** standard direct edges和3处C.JALR；`extract-reset-firmware-entry.py` **exit0**，固定原始`.local/x9/backups/partitions/firmware-originals/vcp-real-android-active-b/vcp_b.img`经真实firmware()的新RESET字段与独立profile完全一致。**全部8个runtime flags=false**，未重新核旧IRQ/FB/clock整份profile。source-head.txt=d8c7bd17是修改前基线、source.diff保存145行delta、committed-source-head.txt记录1dca28d8，不回写基线。复提：exclusive新建本树同级 `x9/out/<新唯一名>/`，复制两个helpers后依次 `python3 -I -B .../extract-reset-exact.py` / `.../extract-reset-firmware-entry.py`；复用旧frames，不覆盖产物。gh run list（repo tosasitill/ios_plj110、branch feat/hevc-hw、limit3） **exit0/[]**，gh-runs.*同目录，无CI run ID。无C/frontend/kernel/full-link/boot构建、成功产物二次检查或设备/ADB/fastboot/mailbox/MMIO/SMC/分区动作。
- **另两组上一turn结果/下一动作**：`hevc-module-location-20261116/`只从固定vendor_boot_b v4/type1+legacyLZ4+newc片段取此前未保留的modules.alias/dep/load/softdep和321个KO路径，extract exit0；该片段无decoder候选，**不证明其它分区/内建kernel/全部备份不存在**，不再重提同一KO子集。`heif-iloc-normative-20261116/`两份所谓ZIP实际license HTML，旧ITTF已关闭、最后document GET404/curl22，**没有规范PDF**；Chrome连接EPERM且无会话，停止旧URL/搜索重复，implicit-length gate不改。下一闭 **3062d2 incoming edge / request+16实际选择 / 2f9084 NULL decoder路线及DPB保存**，再接真正持续drain/EOS；不能把labels、EOF software flags、NULL callback或b003当producer排他/逐buffer退休。cold NS delegation与shared controller/storage/CPU/FW writer owner、physical session_ops/IRQ/clock、BS/FB独立排他/durable dequeue、DPB/layout/scratch/cache/逐buffer退休/c006真ACK、linear NV12/P010/HYFBC、CCV/HDR像素实效、完整HEIF derived inheritance/aux-alpha/monochrome RExt/prem/implicit lengths及普通VT→CMPhoto→ImageIO自动入口仍全部必做。**无最终镜像，全部代码齐全后才出唯一最终boot**；本轮progress而非complete/blocked。

### HEVC decoder 原件入口：实际 mounted vendor_dlkm 路径（2026-11-16，当前活动交接）

- **活动状态/范围**：从 tracked clean、已 push **`11497ded`** 接续唯一 `.worktrees/hevc-hw` / `feat/hevc-hw`。本轮取得当前 mounted 文件清单的新入口，并收束原件缺失范围；**没有生产源码或证据工具源码改动**，RESET 工具仍 `1dca28d8`，不能称 native RESET/drain、DPB 或硬解完成。完整 goal active，不合 main、不清理树，不改其它任务/共享 `.local`，无子代理，全部 READY/authority/producer-exclusion/retirement/普通 VT 门保持关闭。
- **观察/原件位置**：只读消费另一任务已授权保存的主 checkout **`x9/out/android-call-inputs-20261116-092442/`**；`module-file-inventory.txt:137–139` 实际列出 `/vendor_dlkm/lib/modules/mtk-vcodec-common.ko`、**`mtk-vcodec-dec.ko`**、`mtk-vcodec-enc.ko`。`mounts.txt:15` 为 dm-30 挂载 `/vendor_dlkm`、EROFS、ro；`build.txt` 为 `OPPO/PLJ110/OP5E17L1:17/CP2A.260605.016/B.46c413f-20895a6-20bdbdd:user/release-keys`、slot `_b`。这证明采集时存在的 mounted 路径，**不证明 decoder 已加载、模块 bytes/build-id，或与 2026-11-10 保留的 VCP 分区配对**。旧 vendor_boot 的 321 KO 子集不是此 mounted 目录的全集，不再用其缺 decoder 推所有原件不存在；audio-loaded-modules.txt 是过滤结果，不据其缺项判断 vdec 状态。
- **缺口界定/私有证据**：上述 `files-manifest.json` 只抓七个 audio KO，两个 decoder/common 路径均未采集。接上轮主 `x9/out` 和 backups 的定名结果，本轮将 exact basename `mtk-vcodec-dec.ko / mtk-vcodec-common.ko / vendor_dlkm_b.img / vendor_dlkm.img` 检索扩到全部 `.worktrees` 与跟随软链的 `.local`，均 exit0、0 matches；**不排除改名文件或归档内 bytes**。新唯一 **`x9/out/hevc-decoder-mounted-input-20261116-1120/`** 保留 `mounted-decoder-input-evidence.json`（source HEAD11497ded、两个路径/line、来源、精确检索 argv/exit、限制）、selected inventory/build/mount 原始摘录。仅这些字段被读取/保存，没有读取 identity/serial/device-list，没有新 ADB 连接、采集或设备操作。复核直接 `sed -n '137,139p' <主checkout>/x9/out/android-call-inputs-20261116-092442/module-file-inventory.txt`、`sed -n '15p' .../mounts.txt`，并核 files-manifest 的 path 字段；不再重复缺项定名检索或旧 vendor_boot 提取。
- **下一具体输入/不越权**：请用户提供上述两个同版原始 KO，或明确授权**只读 ADB 取得这两个路径**；其它任务的 ADB 授权不沿用。取得后先核 ELF/modinfo/build-id/版本来源，现有 `decoder_common_evidence()` 的旧 SHA/build-id 门不得为了匹配新件而跳过；再从实际 `vdec_vcp_reset`、BS/FB 消费、DPB/layout/最终退休读取真实窄流。原件未到前不按 `3062d2 / 2f9084` 的 vendor 猜测扩 CFG，不加空 drain/provider 或假 c006 free。cold NS delegation/shared controller-storage-CPU-FW owner、physical session_ops/IRQ-clock、独立 producer 排他/durable dequeue、DPB/cache/逐 buffer 退休、真实 linear NV12/P010/HYFBC、CCV/HDR 像素、完整 HEIF derived/aux-alpha/monochrome RExt/prem/implicit lengths 与普通 VT→CMPhoto→ImageIO 自动入口全部仍必做。
- **本轮结果边界**：只有离线原件入口/NOTES 交接；没有测试/样本/人工向量/CLI、analyzer/frontend/native/kernel/XNU/full-link/固件/boot 构建，未复检任何成功自建产物，未发起 CI、无新 CI run ID。没有 ADB/fastboot/mailbox/MMIO/SMC/分区操作；无最终镜像，仍须全部代码齐全后才出唯一最终 boot，物理验收尚未进行。新入口是调查进展，不是完整功能完成；未满足连续三轮相同全 goal 外部阻断条件，不标 complete/blocked。本条正常 commit/push 后的交接 hash 用 `git log -1 -- NOTES.md` 取得。

### HEIF implicit extent：Apple 原始 parser→sample-location 窄流（2026-11-16，当前活动交接）

- **范围/分类**：上一 goal turn 为 progress（真实 mounted decoder 路径改变下一原件入口），本轮从 tracked clean、已 push **`e54c1d6a`** 继续同一 HEVC 活动树。没有收到新设备采集授权或两个 KO 原件；不重复其缺项定名检索。改核此前未分析的 Apple iloc 实际解析/消费流，获得新独立负证据，**没有生产或证据工具源码 delta**，不称隐式长度/硬解完成。完整 goal active，未达到三轮相同全 goal 外部阻断审计条件，不标 complete/blocked；不合 main、不触其它任务或共享 `.local`，无子代理，所有硬解门保留。
- **新原件/真实注册**：唯一 **`x9/out/heif-apple-iloc-source-20261116/`** 保存五批新原件 asm、exact argv/stderr/exit、五个 qualification helpers/results/logs/exit，全部 **exit0**，合计 **10 个新 selected original function ranges**。MediaToolbox UUID **`9c23f13c-2605-3ed2-bf6b-f8d537ee6655`** 与 retained root-clone Cryptex cache相同，新ranges逐字相同；只是原件资格，不是当前设备runtime或成功自建产物复检。`_ParseGlobalMetaDataAtom 1952435b4` 在 **19524362c..195243640** 把 `1d6a07af8`、count9传给 `_IFFParseChildAtoms 1952436fc`；新 slide-v2 table 解码确认 **entry1d6a07b38 = iloc，callback195244ab8**，child helper **195243778..1952437d8** 实际按 FourCC 匹配/16B stride取callback并BLR。`iloc-dispatch-table.json`、`resolve-iloc-dispatch.py`保留原cache offset/raw/delta-mask/value-add，未重跑旧已成功key resolver。最初scratch复用helper未提供 `__file__` 导致NameError/exit1，随后补namespace后实际table读/解码成功；失败仅记录在stage JSON，不伪称首次成功。
- **已闭的窄输入→输出/不能当规范**：`_ParseGlobalMetaDataItemLocationAtom 195244ab8` 的 **195244bc4..195244bd0** 只接受 `length_size=4/8`，**0直接走195244e98的−12848**。**195244d8c..195244d90** 与 reserve **19554acb8..19554acbc** 都拒绝method≥2，是Apple实现子集，不能删本项目已有method2能力。显式4/8B length在 **195244e34..195244e70** 原样BE读入16B `{offset,length}`表的+8；reserve保存item+120=count/item+128=table，getter19554abec不扩0。coded-picture **19554f1e8..19554f200** 与tile **195551ce8..195551d0c** 都以skip x1=0 tail到 `_copyLocationFromItem 19554ebfc`；**19554ec84/19554ecc4..19554ed18** 读取同表，仅length>remaining-skip才输出extent，因此这条已证链的显式0被跳过，**没有读到文件/idat末尾的替换**。这闭合的是实际原Apple静态处理，不证明ISO对absent/explicit0的合法语义，也不证明其它入口runtime；现有method0/1 zero-length ENOTSUP门不改。
- **下一边/完整范围**：不再拿此Apple subset、旧libheif/GPAC未支持代码、Nokia getter或失败ITTF URL当implicit-length oracle；需独立适用规则后才改实际resolver/CM入口。native主线仍等待用户提供或授权只读取得上条两个decoder/common KO，随后核版本身份和实际RESET/BS/FB/DPB/退休；不从未知vendor指令造CFG或加空provider。cold NS delegation与shared controller/storage/CPU/FW writer owner、持续physical session_ops/VCP IRQ-clock、BS/FB独立排他/durable dequeue、完整DPB/layout/scratch/cache/逐buffer退休/c006、真实linear NV12/P010/HYFBC、CCV149/HDR像素及普通VT→CMPhoto→ImageIO自动入口、完整derived/aux-alpha/monochrome RExt/prem/合法HEIF属性全部仍必做。stage-evidence/replay与helper in-memory syntax exit0保留在新目录；没有测试/样本/人工向量/CLI、C/frontend/native/kernel/XNU/full-link/固件/boot构建，未发起CI、无新run ID，无设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作。没有最终镜像或物理验收；本NOTES正常commit/push后恢复hash用 `git log -1 -- NOTES.md` 取。

### HEVC 完整目标外部输入阻断审计（2026-11-16，当前活动交接）

- **审计基线/分类**：同一 `.worktrees/hevc-hw` / `feat/hevc-hw`，tracked clean、已 push **`af7fbf39`**。上一turn为progress（原Apple parser/实际消费者的新负证据）；本turn仅重新核定真实源码前置条件，**没有功能进展，不伪称verified wait**，没有live构建/CI/tool handle需要等待。从mounted原件入口`e54c1d6a`、Apple窄流`af7fbf39`到本次自动接续，**缺两个原始KO且未获本任务只读ADB授权**的同一外部条件连续存在三轮；旁线原件证据没有消除它。本轮不再次搜索缺项、不重提旧原件、不重编成功源码、不扩未知vendor CFG或制造空provider。
- **当前源码反证/不是manifest代替验收**：`rg -n 'x9_vdec_register_platform\(|x9_vcp_bus_create\(|x9_vdec_control_create\(|x9_vdec_dma_create\(' x9 --glob '*.[ch]' --glob '*.inc' --glob '*.cpp'`实际仅有定义/声明，无physical组装或注册caller。`sweet_x9_vdec.c:27–48`只是注册门；末尾init仍默认`x9_vdec=0`，无provider时ready/profiles不会建立，boot-device无该参数。`x9_vdec.h:121–159`要求真实持续DPB、display-order原token、bounded drain保reference、CPU-owned completed linear frame和真prerequisites，现有wire/control/host PEEK/ACK不能替代。`heif_compositor.c:530–542`、`heif_native.c:204–214`真实preflight仍拒aux/prem，`heif_items.c:413`真实resolver仍拒method0/1 zero-length；这些是未完成源码路径，不由frontend绿色或manifest推完成。完整scope（native authority/layout/退休/像素、持续HEVC、完整HEIF、HDR及普通系统入口/物理验收）不缩小。
- **为何需要外部改变/精确解阻**：只读复核既有capture的files-manifest path字段，`/vendor_dlkm/lib/modules/mtk-vcodec-dec.ko`和`mtk-vcodec-common.ko`仍未保存；没有新用户授权/输入。当前已证原件边不足以写RESET NULL downstream/DPB/最终退休，也不足以证明cold NS owner；已追的Apple subset并非合法implicit-length规则。继续重复ISA/规范下载/相同helper或再加无人消费接口不会使完整目标更真。在当前输入与操作限制下，本轮没有可依据已证事实推进的下一native实现/物理验收动作，因此本次交接commit/push后将goal标 **blocked（不是paused/complete）**。请用户提供两个同版原始KO或明确授权仅只读取得这两个路径；拿到后先核ELF/modinfo/build-id/版本来源、不跳旧profile身份门，再从真实RESET/BS/FB/DPB/退休最早未证stage继续。此输入是下一解阻步骤，不是保证拿到KO即可完成，cold owner/全部代码/唯一最终boot与用户手动验收仍必需。
- **收尾边界**：本轮只有源码/既有manifest的只读审计和NOTES，没有新生产或工具源码，没有analyzer/frontend/native/kernel/XNU/full-link/固件/boot编译，没有测试/样本/人工向量/CLI、成功自建产物复检、新CI/run ID或设备/ADB/fastboot/mailbox/MMIO/SMC/分区操作。无最终镜像、无物理硬解成功，所有门保持关闭，不合main、不清理活动树或已有原件；用户确认解阻后继续当前分支。交接正常commit/push，恢复hash用`git log -1 -- NOTES.md`取，源码仍af7fbf39所含实现。

### HEVC decoder/common：用户授权只读 ADB 原件采集成功（2026-11-16，当前活动交接）

- **授权/活动基线**：用户明确“进 adb 了 你提取吧”，在 `.worktrees/hevc-hw` / `feat/hevc-hw`、已 push 的 **`0aeb5118`** 上仅只读取得先前指定的两个 KO 与窄来源信息。新唯一目录 **`x9/out/hevc-decoder-adb-20261116-132504/`**；`capture.py` 确认唯一在线设备、固定两个路径、exclusive-create，`capture.exit=0`。上一节“两个原件缺失且未获本任务采集授权”的具体外部条件**已经解除**，不是完整硬解完成；不沿用旧缺项检索或继续要求用户提供已取得原件。
- **原件身份/保全**：两个源均在 `/vendor_dlkm/lib/modules/`，采集前后 stat 一致、字节长度符合原文件、设备原文件 SHA-256 与本地捕获一致。私有 `capture-manifest.json` 和各命令 stdout/stderr/argv 保存完整记录；序列号仅在 ignored 私有采集记录中，不复制到本文件或 git。

  | 原件 | 字节数 | SHA-256 | GNU build-id |
  | --- | ---: | --- | --- |
  | `mtk-vcodec-dec.ko` | 607352 | `4de1ff25c4b16936bc487f6176d9feda24b4b9c85254f4921d4f8f330eb2bae0` | `a5f63a89f1658568bb7ffc2ae837e96df2361ea1` |
  | `mtk-vcodec-common.ko` | 172288 | `de863471e77b05cdec514c7c523c12a785fc1c81e603bdf222a6054f987b7de5` | `aca809858dda2fc48a73dfa661324e402d0361a9` |

- **采集时 runtime 来源**：`loaded-vcodec.stdout` 窄过滤确认两个模块均 **Live**；两份 `/sys/module/<name>/notes/.note.gnu.build-id` 成功读取，与原 KO build-id 匹配。`vendor-dlkm-mount.stdout` 确认 dm-30 / EROFS / ro。build 前后相同，fingerprint 为 `OPPO/PLJ110/OP5E17L1:17/CP2A.260605.016/B.46c413f-20895a6-20bdbdd:user/release-keys`、slot `_b`；uname `6.12.69-android16-6-g242e1a07f878-ab15865446-4k`，两个 KO vermagic 均 `6.12.69-android16-6-o-gb79517a1cbef-4k SMP preempt mod_unload modversions aarch64`。**git suffix 不同，不能泛称完全相同 kernel build**；loaded-note match 不等于 loaded text hash、当前 VCP firmware pairing 或硬解运行成功。
- **真实现有 parser 结果**：执行维护版 `x9/tools/vcp_artifacts.py`，`parse.exit=0`，输出 **`parsed/artifacts.json`**。原件正好命中已有固定 whole-file SHA/build-id 门，**没有放宽任何 gate**：decoder 的 common/init/decode/bs_route/vp/mem_wait/clock_ring/output 八项及 common 的 vp/mem_wait/irq_owner 三项 `profile_match=true`；两个 `captured_loaded_build_id.matches_file_build_id=true`。报告仍为 **`runtime_ready=false` / `module_firmware_pairing_proven=false`**，不能把静态 profile 或 Live 当作 DPB/DMA 退休、线性像素输出或最终物理验收。
- **已保存的 RESET 窄原件观察**：`reset-original.asm` 及独立 command/stderr/exit0 保存 `llvm-objdump -dr --disassemble-symbols=vdec_vcp_reset,vdec_flush,vdec_if_flush <本目录>/mtk-vcodec-dec.ko`。原 `vdec_vcp_reset .text 24b94/1a4`：w1 保存 drain_type，`24bf0` 写 msg+16；`24bf4` 写 a003/ctx-id，msg+8 从 inst+58 取 alias，+20 保零；`24c1c..24c34` 以 **24B、flags 0/1/0** 直接调用 `vdec_vcp_ipi_send 230e0`。返回后 VSI bytes4096/4097/4099 更新 ctx+fe8/fec/ffc，后两项 bool 化，**不证明 DMA drain 或 DPB/reference 保留**。`vdec_flush 1e6f0/128` 的 `1e73c` relocation 实际指向 **vcu_dec_reset**，不可擅自拼为 VCP RESET；`vdec_if_flush 16460/12c` 走 ctx+470 vtable+28 callback / ctx+480 handle，缺 callback fallback `vdec_if_decode`，仍缺真实 runtime selection。符号表和原件 asm 不重提、不覆盖。
- **重放/下一离线动作**：如需要重新采集，在活动树新建唯一 `x9/out/<新名字>/`，复制本目录 `capture.py` 后用 Python3.13 执行（不覆盖成功目录）；其固定 allowlist 和唯一在线设备检查保留。仅重新解析可用 `python3 -I -B x9/tools/vcp_artifacts.py --module <本目录>/mtk-vcodec-dec.ko --module <本目录>/mtk-vcodec-common.ko --loaded-module-note mtk_vcodec_dec <本目录>/mtk_vcodec_dec.loaded-build-id.note --loaded-module-note mtk_vcodec_common <本目录>/mtk_vcodec_common.loaded-build-id.note --provenance '<明确本次用户授权采集来源>' --out x9/out/<新唯一解析目录>`。当前不重复已成功 hash/profile/反汇编窗；下一从原 **`vdec_vcp_decode 25558/754`** 的 NULL→RESET/drain_type 实际分派继续，再追真实 BS/FB/get_param/DPB/layout/c006 free。RV55 未知 `3062d2 / 2f9084` 不因取得 KO 就推定 vendor CFG。
- **操作/完整边界与交付**：此次没有设备写入、重启、flash、mailbox、MMIO/SMC、其它模块/固件/分区采集；没有生产源码 delta、测试/样本/人工向量、编译、CI run 或新 boot。所有原件及 private manifest 经 `git check-ignore` 确认 ignored；仅本交接正常 commit/push 到 `origin/feat/hevc-hw`，hash 用 `git log -1 -- NOTES.md` 取，实际 git stdout/exit 保存本次 ignored 目录。不合 main、不清理活动树、不动其它任务/共享 `.local`，无子代理。完整 cold NS/shared controller-storage-CPU-FW owner、持续 physical session_ops/VCP IRQ-clock、BS/FB 排他/durable dequeue、DPB/layout/scratch/cache/逐 buffer 退休/c006 真 ACK、linear NV12/P010/HYFBC、CCV149/完整 HDR 像素/late CLLI、HEIF derived/aux-alpha/monochrome RExt/prem/implicit lengths/合法属性及普通 VT→CMPhoto→ImageIO 自动入口与物理验收仍全部保留；**代码全部齐全后才出唯一最终 boot，不标 complete**。

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
