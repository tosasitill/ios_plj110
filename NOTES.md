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

#### 2026-11-16 SMS 同 ID 退休 / ACK（入口 checkpoint；下游仍未闭合）

- **真实ACK入口**：原已读registrar1004b9764..1004b977c供应 **10123c366=kSmsAcknowledgeMessage → handler1004bad6c，w3=3**，不猜flags语义。完整原FS1004bad6c..1004baf70/516B从上轮保留NEW原字节promote，actual exit0、没有新读原text。经Registry::getServiceMap与旧getter1000a95fc得到service；kSmsMessageID按unsigned转换，default **-1**（retrieve为0），另取尚未资格key10123c5e5，import1010817b4的default为0。service非空时，只有 `((ID == -1) OR 第二转换值)` 的bit0为0才在1004bae74实际调service virtual **+e0**，x1保原ID；否则以w0=0进入reply包装。第二字段、包装import101085084和reply key10123c603均未补猜测名字；不能把该virtual调用/回复认作map删除、Messages COMMIT或物理ACK。
- **两lead纠偏**：bounded原CString10123c31c实际为 **kSmsGetRecievedMessageIds**（保留原拼写），不是ACK/退休；handler1004ba4f4..1004ba8f4/1024B仅经同getter调virtual+100填两组u32 vectors，无request单ID。相邻10123c336实际 **kSmsGetIncomingMessageCount**，handler1004ba8f4..1004baa84/400B仅virtual+d8 count。两body都从保留raw资格、actual exit0；不以临时vector/free称map退休。其余7个原registrar精确名字资格exit0后只展开真ACK，不扩MMS/模拟/emergency/phone-reg业务；初始暂定文件名root-registered-retire-handler原样保留，角色以此纠偏为准。
- **范围/停止点**：本轮单代理 `feat/sms-retire-ack`，clean parent **5f406973**；checkpoint时观察main/origin已由其它工作推进至 **f1d91a25**，不冒充本轮输入。累计 **3完整原FS函数1940B、无新import资格**，全部复用上轮16998412B原text库存，80页chains不重遍历。报告 `docs/2026-11-16_reverse-sms-ack-handler-report.md`；raw/精确argv/stdout/stderr/exit、scope/timeline及checkpoint合同在 `x9/out/sms-retire-ack-20261116/`。无生产delta/测试/人工向量/compile/kernel/boot/CI run/设备/ADB/mailbox/mount/数据库/分区或共享输入操作，无待取reader句柄；最新有效CALL/SMS kernel仍41deaefb输入的 `x9/out/x9-call-end-native-owner-20261116/kernel.development.sweet`，未复检。
- **下一唯一窄边**：先矿保留data，再bounded资格10123c5e5/10123c603及精确imports1010817b4/101085084；读取同Model AP101528d10的+e0 slot **101528df0** 后只沿真target查同u32 ID/Model+60 map的callback、erase、size变化与下游Controller ACK，先矿旧body再promote原库存，不重扫原text或80页chains。server+128 Registry安装/current concrete选择、publisher原init entry仍条件化。完整通话短信goal继续active：GUID/record/COMMIT重开、MT物理ACK/MO、CALL逐动作owner/UUID/nativeepoch/双向媒体均未验收。git提交/整合/push确切结果随后记录；checkpoint不是完整功能完成。


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
