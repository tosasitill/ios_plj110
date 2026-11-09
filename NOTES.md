# ios-sweet / X9 工作笔记(精简版,2026-11-09 合并更新)

> 旧的完整流水账(7784 行,含 sweet/SM7150 时代、rev2xx 全部 handoff)在 git 历史里:
> `git show a8f1955:NOTES.md`。需要翻某个 revision 的细节时 `git log -S'<关键字>' -- NOTES.md` 再 show。
> 截至 2026-11-09 的完整 cn 交接在 `git show 636edc2:NOTES.md`;当前以 §7 的 HANDOFF (84) 为准。
> 本文件只留:目标、环境与命令、当前基线、已验证/未解决、关键结论、下一步。
> 约定(AGENTS.md):每个里程碑后更新「当前交接」;区分**观察**与**推断**;不把序列号/设备转储/Apple 产物放进 git。

## 分支交接(2026-11-09)

- 按用户要求,已推送 cn 的末端 `636edc2`,再以合并提交 **`190878a`** 合入并推送 `main`;两个父提交为远端文档整理 `6379df4` 和 cn 的 `636edc2`,没有覆盖远端历史。
- 本地与远端 `rev252cn` 已删除,旧 `origin/rev252cn` 跟踪引用也已移除;当前工作区为 **`main`**,跟踪 **`origin/main`**。另一份 Wi-Fi detached worktree 未操作。
- 唯一冲突是 NOTES:保留 main 的精简结构/README 等文档整理,补入 HANDOFF 82、清理和 cc-remote 交接;其余合并树条目已逐一核对,均与相应父提交完全一致。
- 合并时,原 `.gitignore`、`SweetX9RenderEngine.inc`、`X9CSFRenderQueue.cpp/.h` 四份未提交修改在 main 原样恢复,SHA256 与操作前一致,没有夹带进合并提交;后续用户明确要求入库,见下一节。临时 stash 已清掉,之前已有的 stash 保留。
- 构建输入路径检查通过;`gh run list --commit 190878a` 返回空列表,无此次合并的 CI run。本次无构建、无设备启动/写入,不产生新的镜像验证结论。当前硬件任务仍为 §7,不是 §8 的历史 GPU 计划。
- 私有备份与合并校验:`.local/git-integration/20261109-cn-to-main/`(原始四文件、binary diff、分支/stash 状态、NOTES 两侧副本和合并验证);均不入 git。

## 四份本地修改入库(2026-11-09)

- 用户要求说明四份未提交修改,无冲突则提交推送。四文件 SHA256 与合并前保存的副本一致,本次没有重写这些代码。
- `.gitignore` 独立提交 **`f5380e6`**:忽略本地 agent/cc-remote/acpc 状态、密钥、环境变量文件及认证/ACP 日志,没有添加这些敏感文件本身。
- `SweetX9RenderEngine.inc`:可选 **`x9cs=1`** 在 8 个硬件 CSG 槽耗尽时选择空闲的旧 group,保存挂起状态并复用其槽位;原 group 再提交时绑定槽位并恢复。group 使用独立 VA 索引,避免换槽破坏 CALL 地址。
  同文件的 **`x9gt=1`** 写入/读取 GPU 时间戳,区分 doorbell→CALL 的固件接单、CALL→wait-all 的 GPU 执行、完成→CPU 观察到 fence 三段耗时。
- `X9CSFRenderQueue.cpp/.h`:配置接口增加 `bool reuse=false`,复用时接受 REQ/ACK 一致的空闲/SUSPEND 槽状态,与上述保存/重绑流程配套。**x9cs/x9gt 均默认关闭**,不能把入库视为功能验证通过。
- 检查:提交前 fetch 后 `HEAD...origin/main = 0/0`,无未解决合并冲突,`git diff --check` 通过。本次只整理提交并推送 main,未重新构建、运行测试套件或上机;当前硬件结论仍以 §7 为准。

## 1. 目标与硬件

- 目标:在 **OPPO Find X9(PLJ110 / OP5E17L1,MT6993,12 GiB RAM,面板 1256x2760)** 上直接从 MediaTek LK 启动自编译 XNU
  (xnu-8796.141.3 + SWEET 补丁),跑**未修改的 iOS 16.7.16(20H392,iPhone X/D22 身份)**用户态到 GUI。
- 启动链:LK → Android boot.img v4(legacy LZ4 arm64 Image)→ x9 裸机 shim → 内嵌 SWEETXNU payload → XNU。
  设备每次用临时 `fastboot boot`,**由用户手动刷/启动**;不自行 flash/reboot/写分区(x9/AGENTS.md)。
- 根盘:`userdata` 里 pin+immutable 的专用 extent 文件(16 GiB HFSX,可写副本),内核原生 `ufsroot0` 挂载(`rd=ufsroot0 rootrw=1 ufsrw=1`);
  Android userdata 保留。详情与路径见 `x9/AGENTS.md`、`x9/README.md`。UFS 链路 HS-G1/RateB/2 lane 已硬件验证(~130-155 MiB/s)。
- 旧项目 sweet(Redmi Note 10 Pro / SM7150)的历史见 `README.md` 与 git 历史,当前不再工作。
- 屏幕:ADT canvas / 帧缓冲呈现 **1242x2760**(414x920 pt @3x,左右各 7 px 黑边);宽 414 是为修键盘 123(见 §6)。
  物理面板仍 1256x2760 stride 5120,framebuffer 物理地址 0xfc910000。

## 2. 环境与命令

**机器分工**:重活在 Mac(Xcode/LLVM/ccache/反汇编),服务器只读文件、编辑、git。
- Mac:`ssh -p 2222 tosasitill@localhost`(反向隧道,偶尔断,autossh 会重连),仓库 `~/ios_plj110`,构建产物 `~/ios_plj110/x9/out/<name>/`。
- 设备日志:用户放在 **Mac 家目录 `~/NNN.txt`**(104 起),用 ssh 读,不在 `/tmp`。老日志副本在 `.local/x9/logs/`(gitignored)。
- 反汇编工具(Mac,不在 git):`~/x9-kbd-dis/{dis.py,callers.sh,ivars.py}`、`q.dis`(QuartzCore)、`.local/x9/work/dsc-20H392/`(设备实跑 dyld cache 拷贝)。
- 抓日志:`python3 x9/tools/usb-live/usb_log.py --output ~/N.txt`(USB 实时控制台);另有 pstore / 磁盘 sidecar 日志。

**构建**(在 Mac,仓库根):
```bash
python3 -I x9/build/build.py all --revision <插件号> --name x9-rev252xx [--boot-arg ...]   # 插件+内核+boot.img
python3 -I x9/build/build.py kernel|boot|plugins ...                                       # 分步;boot 可 --kernel <已有内核>
python3 -I x9/build/build.py --dry-run                                                     # 看全部命令
python3 x9/build/prepare-local.py                                                          # 首次/搬脚本后,链接 .local
```
- `x9/build/local.json`(gitignored,由 config.example.json 拷)写机器路径:`developer_dir`(Xcode 15)、`dxb`(冻结 XNU 对象树)、`llvm_clang/lld`(LLVM 22)、
  `root_manifest`(含序列号与 extent 表,**绝不入 git**)、`compiler_launcher=ccache`。
- `build.py boot` 以 `x9/build/boot.json` 为基础参数;**实验参数每次都要用 `--boot-arg` 全量传**(只用 boot.json 会起一个没有 Wi-Fi/GPU 令牌的系统,handoff 48 踩过)。
  完整参数集可从上一个镜像恢复:`strings x9/out/<prev>/shim/embedded-payload.bin | grep x9_metal_render`。
- 缺 llvm-objcopy 时 `all` 会停在 boot:用 `build.py boot --kernel x9/out/<name>/kernel.development.sweet` 补;Mac 上 python 要有 lz4(用 Framework python3.13)。
- 同名输出目录会被覆盖:每个镜像用唯一 `--name`。**构建成功后不要再二次校验/重算 hash**(x9/AGENTS.md 2026-10-25/28 要求)。
- 源码布局:`x9/kernel/src`(内核 overlay,57 个对象)、`x9/shim`、`x9/metal/General`(Metal 前端+PanVK 桥)、`x9/mailbox`、`x9/tools`、`docs/`(分系统评估)。
  Wi-Fi 驱动在 `X9WifiNet.inc`。第三方:Mesa `300b5fd7…`+`00-mesa-panvk-x9.patch`,metal2vulkan **963f0e64**(UniXNU 642c4fef 系列)+补丁。

**插件 mailbox(只有插件包变了才需要)**:内核只信 `x9/kernel/generated/x9_render_external_manifest.h` 里列的 path/size/SHA-256;
插件二进制不在 boot 分区,存在设备 pinned mailbox。**新的 plugin-package(build 时出现新插件号/哈希)必须先**
(Android 开着、用户给序列号):
```bash
X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/<name>      # 之后再让用户 fastboot boot x9/out/<name>/boot.img
```
纯内核重编(插件号不变,allowlist 头不变)**不需要**重新 stage,上机前只要说"无需 mailbox"。插件重建非确定,allowlist 头要随该 revision 一起提交。
当前插件:**275**。

**上机流程**:用户把手机 `fastboot boot x9/out/<name>/boot.img`,跑场景,日志放 Mac `~/N.txt`,我读。永不 `fastboot getvar`。

## 3. 当前基线镜像与参数

最新:**x9-rev252hk-md**(修复提交 `d1f8515` + 当时未提交的渲染改动,现已入库),boot.img **15278080 B**;**156 与本次 ADB 定点取证确认 reserve 调整生效、efs1.bin 已分配 1 MiB,但 iOS 基带握手未推进**(HANDOFF 84)。
本机镜像:`x9/out/x9-rev252hk-md/boot.img`;配置沿用 `.local/x9/modem/boot-rev252hi-md.json`。
本次 BBUpdater 日志的15条 reset 原因全部为 **error 2 / `fPhases.size()` 断言**,不再出现之前的 scratch error 104;efs1.bin 零字节问题已消失,不能再把该断言仅归因于 scratch 失败。断言的上游原因及最终 iOS 蜂窝功能仍未解决,详情/下一步见 §7。以下 GPU 专用基线与参数为 **2026-11-08 历史记录**,不要当作 hk 参数直接复用。

历史 GPU 基线:**x9-rev252dc**(内核-only,插件 275 不变,沿用 rev252cx 之后的 mailbox)boot_sha256 `041d267fc297dd0830bd50407b85579679f0ace28ad2ba94a7c825e3bd0fd80c`,**未上机**。
已用户确认好的最后一版:**rev252cy**(123 修好、控制中心不卡);之后 cz(x9lc)、da(1242x2760)、db(DVFSRC 探针)均未上机验证。

完整启动参数(rev252dc;`x9_wifi_xo0..6` 是射频校准字,值不进 git,从 Android 拉取/旧镜像 payload 恢复):
```
x9_metal_render=1 launchd_unsecure_cache=1 -apfs_shared_datavolume ignition_level=1 debug=0xc serial=3 cpus=8 keepsyms=1 -v
unentitled_ios_sim_launch=1 rd=ufsroot0 rootrw=1 ufsrw=1 launchdsuffix=release -nobsdmgroot
x9_wifi_power=6 x9_wifi_real=1 x9_gpu_khz=1716000 x9_gpu_floor_khz=1716000 x9_gpu_park=1 x9_submit_async=1
x9_wifi_xo0..xo6=<校准字> x9_cache_scope=1 x9kbdhook=1 x9pl=1 x9sl=1 x9lc=1 x9_ddr_level=9 x9_dvfsrc=1
```
令牌速查(多数默认关;`x9_*`=boot-arg):
- GPU:`x9_gpu_park`(驻留渲染 CSG)、`x9_submit_async`、`x9pl`(驻留上下文活体 map/unmap,需 park)、`x9sl`(无锁 SubmitAsync,需 async;与 x9dc 互斥)、
  `x9lc`(按 IOSurface 锁协议做 CPU 缓存维护)、`x9_cache_scope`(前端 scope 缓存)、`x9dc`(脏区间 CIVAC)、`x9if`(双 job 在飞;**怀疑卡死,基线已去掉**)、
  `x9_gpu_khz/x9_gpu_floor_khz`(GPU 频率上限/下限,1716000=最高并钉死,governor 每 50 ms 纠正固件降频)。
- DRAM:`x9_ddr_level=N`/`x9_emi_level=N`(写 DVFSRC SW_REQ)、`x9_dvfsrc=1|2`(TF-A SMC INIT / INIT+START)。
- Wi-Fi:`x9_wifi_real=1`(真实射频,没有它也没有 `x9_wifi_fake=1` 就不创建 en0)、`x9_wifi_power=6`、`x9_pkt_log`、`x9_wifi_probe`。
- 诊断:`x9kbdhook`(键盘探针)、`x9tile`(256 宽纹理探针)、`x9_sandbox_ops=1`、`x9_sample_procs=<名>`、`x9_mgwipe=1`(一次性清 MobileGestalt 缓存,验证后别常开)。

## 4. 已验证能用(设备观察)

| 项 | 状态 |
|---|---|
| 启动/根盘 | LK→shim→XNU,原生 UFS 根盘 rw 挂载,HS-G1 双 lane;launchd/keybagd/usermanagerd 起来,Early boot complete |
| 桌面/GUI | SpringBoard 桌面、触摸(S3910)、电源/音量键、电池服务、状态栏、机型显示(iPhone,不再 iPod)|
| 时间 | 读 MT6687 RTC(PMIF/SPMI)修开机 1970(`X9 RTC:` 行)|
| 相册/TCC | 沙箱 stand-in 对 op5/op6 回真 token,tccd 签发 Photos 扩展,assetsd 不再崩 |
| 键盘 123 | **rev252cy 用户确认修好**(见 §6)|
| GPU | CSF 持久会话(8 CSG 槽),PanVK+metal2vulkan 经 mailbox 插件渲染桌面;park/async/x9pl/x9sl/x9lc 全部上机过(至 cz 前);fps ~17-22 |
| Wi-Fi | 固件起来;真实 SSID 列表;Open 网 RUC-Web(5180)可关联、DHCP(10.46.12.100)、DNS、HTTP 劫持页能收;离开/重加入 OK |
| 网络特权 | 沙箱 stand-in 授 PRIV_NET_* 10000-10008、11000/11001(WebKit.Networking 套接字委托)|

## 5. 未解决问题

当前主线是 **efs1.bin 已分配后,定位 BBUpdater 的 `fPhases.size()` 断言为何仍在 PCI/Sahara 传输前触发**(§7);下列为保留的 GPU/Wi-Fi 待办,不是新的硬件验证结论。

1. **GPU 带宽/帧率**:avg ~17-22 fps,稳态 24 fps;job 内 GPU wait 6-15 ms。判断显存带宽受限。**DVFSRC 从未初始化**(110.txt:`basic=8 opp_count=1 gear=0`,SW_REQ 被忽略)。
   rev252dc 加 TF-A SMC INIT:`MTK_SIP_VCOREFS_CONTROL = 0xC2000506`,cmd INIT=0(a0=0 成功,a1=dram_type)锁高 OPP,START=1(`x9_dvfsrc=2`)后自由运行。**待上机**,SMC 可能挂。
2. **CSG 槽耗尽**:前 8 个 GPU 进程各占一个硬件 CSG 槽(group 创建时绑定,关设备才放);第 9 个客户端(AegirPoster)`X9RenderCreateGroup` 返回 0xe00002be,43 次失败。
   可选实现现已入库(`x9cs=1`,默认关):复用最久未用的挂起 group 槽位,保存/恢复其 suspend buffer,不经 TERMINATE 丢失 PanVK 初始化 CS 状态。入库不等于当前实机验证,仍需带明确开关的对应日志确认。
3. **开/关 app 动画出现 4 条竖向透明马赛克线**(间距 256 px,**一直都有**,非新回归)。已证伪:CA 256 宽 tile 上传缺失(TILEPROBE 无 256 宽 CPU 上传)、swipe 旧马赛克(InPlaceWrites 未启用)。
   现线索:缝位置 ≈ 层原点 + 256·k,即每行 **1024 B 边界**,指向某个渲染目标/IOSurface 每 1 KiB 处像素坏(缓存/内存类),不是绘制拆分。需要一份含开关 app 时刻的完整日志;x9vf 帧抓取只按固定 uptime 触发(参数上限 607 B),需改成按 transition 触发。
4. **Wi-Fi**:WPA2 阶段 2(4-way、INSTALL_KEY_V3、受保护收发)**rev252cm6 已构建、从未上机**;captive portal 弹窗不出现(探测 `captive.apple.com` 从未发出,疑 configd/CNS 的 SC 状态缺失);
   RUC-Mobile(802.1X)EAP 投递 ioctl 待映射;DBDC 重加入偶有无响应。
5. 硬件 vsync/帧节奏、背光控制(无 `backlight-control` 服务)、modem/WirelessRadioMan 噪声、部分 daemon 反复崩(locationd/speech、mediaserverd/AegirPoster、syncdefaultsd、CommCenter)、
   AKS selector(securityd 无真 keychain 保护)、hfs `LowDisk`(11-09 根盘空闲约 167 MiB,原 1% 保留下非 root 仅约 3.3 MiB 可用;见 §7)。
6. 设备 GPU 内存:backboardd 长期累积(resources→~1900,~700-900 MiB,预算 1024 MiB),泄漏/池化不回收待查。

## 6. 关键结论与坑(别重新踩)

**GPU / 渲染**
- 帧是串行:每 `vkQueueSubmit` 先走 X9RenderCache 选择器抢 context+owner 锁,而 async worker 整个 fence poll 都持锁 → x9sl 用叶锁 `submit_lock` 解开。
- 固件会把 GPU stack 降到 260 MHz(钉死频率时 governor 不再 commit):governor 现每 50 ms 比对 `cur_oppidx` 并重 commit;GOV 行的 `gpu=` 是请求值,`actual=` 才是读数。
- CPU 缓存维护曾是每 job 最大开销(16 MB pre/14 MB post):x9lc 后 pre 0.4-0.9 ms / post ~70 µs。**ACE-Lite 一致性已搁置**(即使全维护仍坏数据,SMMU bypass 强制 non-shareable)。
- PanVK 驻留 group 遇 map/unmap 原本必须 retire(cold 64%):x9pl 直接改附着页表并按 panthor 做 AS1 LOCK→FLUSH→UNLOCK。
- 历史:swipe 马赛克根因 = `353ae1c InPlaceWrites`;控制中心拆分用前端 padding 修过;Kraid/HSR 一系列静态分析结论在旧 NOTES(`git show a8f1955:NOTES.md`)。
- 新插件包变更 ⇒ 必须重 stage mailbox;重建非确定性,allowlist 随提交走。曾因没 stage 导致 Apple logo 卡住/黑屏(rev252v、rev249 教训)。

**键盘 123(已修)**:根因是屏宽非标——UIKeyboard 帧宽 414 ≠ 面宽 418.67(1256 px @3x),切到数字面时 `sizeDidChange` 触发 `setFrame:` → `showKeyboard…reload:` 回到字母面。呈现 414 pt 宽的屏后修好。
  不要改成其他 iPhone 身份:根盘是 iPhone X 的 IPSW(资源按设备裁剪),14 Pro 是 arm64e 另一套 dyld cache;状态栏强制 DynamicSplitMedium。

**Wi-Fi(MT6653,PCIe,gen4m 参考驱动)**
- 参考源码(Mac):`~/HelioDarwin/oppo-source/modules/vendor/mediatek/kernel_modules/connectivity/wlan/core/gen4m`;OPPO 内核:`kernel_device_modules-6.12`。
  Android 抓包在 `.local/x9/android-join-{1,2}/`(gitignored,join-2 是 RUC-Mobile 802.1X)。
- 必须用 **LK 的 Wi-Fi DMA pool**(EMI MPU);固件 ROM patch → PHY 固件 → N9 RAM code 的顺序;`WIFI_START` 后 READY 位;NVRAM 37 片;设备带**外置 FEM**(`connfem` 节点,.dts 在 `.local/x9/wifi-nvram/`)。
- RX:数据帧走 **RX5**(硬件头翻译,RXD DW2 b7),扫描/管理帧在 RX7;DBDC 要开;Auth 用 STAREC/TXD 与 Android 抓包对齐后才被 ACK。
- WPA2 实现(X9WifiNet.inc):PRF-512 + HMAC-SHA1 MIC v2;corecrypto **无 MD5、只链 CBC**(AES-ECB 用零 IV 的单块 CBC 实现 RFC3394 解 GTK);
  INSTALL_KEY = STAREC tag 0x27 len 68(AlgorithmId=4 CCMP,KeyLen 16);TXD DW3 b1=PROTECTED;BSSINFO SEC AuthMode 6/7、EncStatus 6。
- Wi-Fi 测试启动必须传完整 `--boot-arg` 集(见 §2)。

**沙箱/用户态兼容**:`sweet_amfi.c` 的沙箱 stand-in 逐个 privilege 放行,拒绝一切未列项;缺什么在日志里搜 `SWEET Sandbox: ... denied`。
  键盘/MG 缓存:`x9_mgwipe` 只在验证时开一次。keybag 绕过用 `-apfs_shared_datavolume`,cache loader 用 `launchd_unsecure_cache=1`(均来自 UniXNU/hackios/dali 的现成修复)。
  **遇到 XNU/iOS 兼容问题先查本地 UniXNU / hackios-xnu-kernel 的现成修复**(`work/references/…`),再自行设计。

## 7. 当前交接(HANDOFF 84,2026-11-09): ADB 定点取证仅保存 8.15 MiB;efs1.bin 已为 1 MiB,本次15条 reset 全为 fPhases 断言

- 用户告知 ADB 已连接,询问是否需要全读。**没有再次拉16 GiB根盘**;先运行现有只读 mutable-root preflight,设备身份、pin+immutable、inode/FIEMAP、分区映射、HFS UUID/容量均通过。随后仅用设备现有 `pinfile rawreadat` 读专用根盘 extent;不解除 pin、不写分区、不 flash/reboot、不挂载设备或本地根盘,不改变旧快照。
- **新证据位置**:`.local/x9/work/x9-rootpull-156/`。`ranges/` 保存原始读取块,共 **8544256 B = 8.1484375 MiB**(另有少量预检/稳定性复读);`ranges.jsonl` 记录时间、偏移、大小、SHA256和脱敏命令。`identity.json`/`stability.json` 记录来源及采集前后 volume/journal header 不变,完成时间 **2026-11-09 19:43:24 +0800**。这些是156之后的新采集,不是旧根盘副本。
- **正确处理未回放 journal**:读取8 MiB journal,校验 header、17个 committed block-list及启用校验的非零 data checksum;有效列表共5185536 B,按顺序在**本机内存**覆盖2760个512 B扇区,没有写回源。末尾不存在原生 replayer 可继续接受的 extra transaction。随后按 HFSX B-tree 逐级查两个目标,没有全扫文件/目录,没有用未回放的旧 catalog 得出文件大小。
- **决定性观察**:`/private/var/wireless/baseband_data/bbfs/efs1.bin` 当前 **1048576 B**,已分配 **256 × 4096 B**,owner/group **25:25**,mode **0600**,create/modify 时间均 **19:32:08 +0800**。只取 catalog 元数据,未读取 EFS 内容。此前的零字节文件问题已消失。
- **本次真正记录的失败**:`log-bb-live-stats.txt` 为 **19823 B**,mtime **19:32:08 +0800**,导出为同目录同名文件。19:31:52–19:32:08 的 **15条 reset 日志全部**是 `BBUpdater error 2 - Assertion failure ( fPhases.size() ) kBBUReturnBadArgument@BBUConsoleFeedback.cpp:93`;本次条目中 **error 104 = 0**。文件历史段仍有旧104,必须按时间区分。SHA256=`794e75387c034984c851b84623f9b05bc88def7be1e6f63ce2e4ba3cafc3d091`。
- **结论边界/下一步**:hk 的 reserve 调整有实际改善,efs1.bin 已正常获得1 MiB分配,但 `fPhases.size()` 断言仍在,不能沿用“只是 scratch 失败的连带结果”的旧推断。下一步回到实际20H392 BBUpdater 的 phase/feedback 调用顺序,确认空 phase 集合是在何处、因何前置错误产生;需要更多日志时继续定点取证。不能用“忽略断言/返回成功”代替打通传输。原生 MD READY/注册与 iOS PCI/Sahara 零传输的区别仍见下方156记录。
- **复放**(仓库根):`python3 .local/x9/work/x9-rootpull-156/extract.py --offline` 已从保存的原始块重现相同日志SHA和EFS元数据,不访问设备。脚本也保留在线采集模式,但**未来新启动必须用新采集目录和新 identity,不得复用156缓存拼接成新快照**。`journal.json`、`result.json` 保存解析结果;当前卷空闲 **132276224 B ≈126.15 MiB**,长期日志填满风险仍在。
- 本次代码仍为 `3b947a1`,前一交接提交 `b1d025c`;仅更新 NOTES,无新内核/插件修改、无构建/CI/测试套件、无需 mailbox。采集脚本/原始块/日志留在 gitignored `.local`,不入库。

### 前次交接(HANDOFF 83,提交 b1d025c): 仅156 USB日志能确认 reserve 改动,尚未取得落盘错误(现由84补齐)

- 本次仅只读分析用户提供的 `~/156.txt`(1306935 B,mtime 2026-11-09 19:32:34,记录到 uptime 58.897 s),对比 `~/155.txt` 和现有源码;未操作设备、未构建/CI/运行测试套件、未重新检查镜像。代码基线 `3b947a1` 已推送 main;本次只更新交接,原始日志不入 git。
- **已验证 reserve 改动,不是完整修复**:156:1031 / 4.082 s=`root HFS reserve 167772 -> 16384 KiB, free 170584 KiB, non-root usable 154200 KiB errno=0`。非 root 可用约 **150.6 MiB**,不再是旧快照的约 3.3 MiB。156:446 的 `build=rev252hj-md` 是 `x9_modem.inc:879` 沿用的硬编码字符串;hk 提交未改它,不能据此说用户启动了旧镜像。
- **当前配置的固件文件已就位**:156:1176 / 4.638 s=`files 10 in 2 paths written 0 same 20 ... errno=0 (direct write, no ticket/MD download)`。两处各 10 文件已存在且 hash 一致,仍需要这些文件,但不需要另行挂载。`qdsp6sw.mbn` 是当前配置明确排除的文件,155/156 都报其 stat ENOENT,不是本次新增回归;不能把这 10 文件校验称为完整 bbfw 校验。
- **原生 MD/SIM/注册正常,不等同 iOS 蜂窝可用**:15.936 s HS2→READY;16.034 s 发布 22/22 服务;21.643 s 和 29.621 s `CPIN: READY`。25.317 s `C5GREG: 1`、36.452 s 查询 `C5GREG: 2,1`(5G 已注册);49.244 s `CEREG: 1`、54.398 s 查询 `CEREG: 2,1`(LTE 已注册),54.627 s 的 5G 查询已为 `2,0`。是先后观察,不声称两者同时注册或蜂窝数据已通。56.248 s 原生 MD 仍 `ready=1`。
- **iOS 侧没有突破**:156:8030 / 27.344 s reset 请求计数 8;156:10450 / 38.785 s 到 15;156:11912 / 50.190 s 仍 15。三次摘要均 `protocol queries 0 accepted 0 / PCIControl methods 0x0`;Sahara `state 0, 0 executes, 0 requests`,data port `step 0, 0 endpoints named, 0 activations`。与155最终的15次重试/零传输相同;这里是 iOS 兼容层 reset 计数,不代表原生 MD 实际掉线15次。
- **本次具体失败仍缺证据**:16.824 s 起能打开固件;17.017–17.028 s 能 stat/open `efs1.bin`,17.466 s 打开 `log-bb-live-stats.txt` 追加,18.432 s 首次 reset,随后重复。155 也有 `efs1.bin flags=601 errno=0`,因此 open 成功不能证明 ftruncate 成功。156 没有 scratch 目标大小、ftruncate 返回值或 `BBUpdater error` 正文;现有 CC open/stat 跟踪有输出上限,不能因后面没有文件记录就断言不再访问。
- **下一步**:优先只读取得**156 这次启动之后**的 `/private/var/wireless/Library/Logs/log-bb-live-stats.txt`,对齐本次约 19:31 的条目,并核对 `/private/var/wireless/baseband_data/bbfs/efs1.bin` 实际大小。先判断 error 104/`fPhases.size()` 是否仍存在或已换新错误,再决定修复方向;不盲目继续改探针镜像,不默认再拉整个16 GiB根盘。现存 `root-20261109.hfs`、`.clone.hfs`、`/tmp/x9root` 和 `x9b.logarchive` 都是156之前的快照,**不能拿它们验证本次错误/文件大小**。
- 次要观察:58.716 s mediaserverd 的 Metal device 创建失败(`IOKit=0xe00002be`),58.782 s `pc=0` 崩溃;日志末尾仍有触摸活动,无实际 kernel panic 证据。此前已用禁用 mediaserverd 排除它作为 BBUpdater 前置阻塞,本次不重开这条旧猜测。
- 最小复核(本机,只读):`rg -n 'root HFS reserve|original 20H392|HS2 -> READY|CPIN:|C5GREG|CEREG|handshake [0-9]+ s|sahara state|efs1.bin|log-bb-live-stats.txt' ~/156.txt`。需要复现的未完成目标仍是 BBUpdater 进入真实传输/最终 iOS 蜂窝状态,不是服务发布或原生 AT 注册。

### 背景(HANDOFF 82): 旧快照确认 scratch 创建错误;ENOSPC 是结合 HFS 保留空间和静态调用链的推断

- 只读拉取:adb + `pinfile rawread/rawreadat` 拉整个 16 GiB 根盘到 `.local/x9/work/x9-rootpull/root-20261109.hfs`(断一次,rawreadat 1 GiB 续传,大小 17179869184)。
  卷带日志未回放,只读挂不上;**7z 直读会把回放前的新文件读成 0 字节**(此前据此得出"logd 配置 0 字节"是错的,已作废)。
  正确做法:`cp -c` APFS 克隆 `root-20261109.clone.hfs`,`hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse -owners off -mountpoint /tmp/x9root` 读写挂克隆(回放日志只改克隆)。
- 观察:三份 logging plist(CommCenter/telephony.abm/telephony.bb)213 字节、15:18 已正确写入 Enable-Private-Data。
- **观察(关键)**:`/private/var/wireless/Library/Logs/log-bb-live-stats.txt` 直接记录每次 reset 原因,不需要解码 tracev3。
  14:09-15:58 = `Baseband Firmware Path Not Found`(固件门,已解决)。16:10/16:23/16:35/16:43 四次启动交替:
  `BBUpdater error 104 - Failed to create scratch /private/var/wireless/baseband_data/bbfs/efs1.bin (11|15) kBBUReturnFileError@BBUEUR10UpdateSource.cpp:312`
  和 `BBUpdater error 2 - Assertion failure ( fPhases.size() ) @BBUConsoleFeedback.cpp:93`。盘上 `bbfs/efs1.bin` 存在、0 字节。
- 静态(libBBUpdaterDynamic 20H392):BBUScratchFile::createWithFile(0x1ee217de0)= 删旧文件失败→11;open(0x601,0600) 失败→15;
  ftruncate(fd,size) 失败→11("cannot truncate file");getpwnam("_wireless")/fchown 失败→11;前面失败后 close 失败→15。/etc/passwd 有 _wireless(25)。
- **此前 ENOSPC 推断依据(没有现场 ftruncate errno)**:sweet_hfs root mount 设 reserveBlocks = 1% totalBlocks(≤250 MiB)= 41943 块(164 MiB);非 root 的 truncate/write 带 kEFReserveMask
  (hfs_readwrite.c:3761/472/4388),只能用 free-reserve。拉取时 free 42777 块(167 MiB)→ _wireless 只剩 ~834 块(3.3 MiB),EFS 扩展 ENOSPC。
  (Mac 上 df 显示 167 MiB 可用是因为非 root mount 不设保留。)根盘占用大户:db/diagnostics 1.1 GB、db/biome 0.58 GB、db/uuidtext 0.47 GB、mobile/Library/Caches 0.25 GB。
- hk(x9-rev252hk-md,boot.img 15278080 B,配置沿用 `.local/x9/modem/boot-rev252hi-md.json`,= hi + 本改动;hi/hk 都含 11-08 当时未提交、现已入库的渲染改动):
  hfs_readwrite.c 新增 `x9_hfs_lower_reserve()`(只降不升,hfs_lock_mount 下改 reserveBlocks);x9_baseband_firmware.inc 在 x9_bb=1、可写根、PID1 时
  调用,boot-arg `x9_hfs_reserve_mb`(默认 16)。上机看 "X9 BBFW: root HFS reserve ... non-root usable ... KiB",然后看 log-bb-live-stats.txt 是否不再有 efs1.bin 错误。
  当时未上机,现已由156确认 reserve 调整生效(见上文),scratch 是否成功仍未验证。未验证 fPhases 断言是否是 scratch 失败的连带结果(推断是重试路径)。长期:根盘 16 GiB 会继续被日志填满,需要清理或换更大根盘。
- logd 解码工具:mandiant/macos-UnifiedLogs 在 `/tmp/ul-src/src`(rustup stable 编译;Homebrew rust 1.97 与 llvm 23 不匹配起不来);
  Apple `log show --archive` 对这个手拼归档崩溃。新归档 `x9b.logarchive`(来自克隆挂载,完整)尚未解析。

## 8. 历史 GPU 交接(handoff 66,2026-11-08)

> 保留远端整理的旧交接,不作为当前基线/下一步;当前看 §7。

- 分支状态:`rev252cn` 已合并进 `main` 并删除(合并提交 a8f1955);代码最新提交 2a9a3a6(DVFSRC SMC)+ notes 2406f8d。
- 上一次实测(110.txt = rev252db):DVFSRC 未初始化;presented fps avg 22.5(55 窗口,max 35.8),60 s 后稳定 24.0 fps = 某个 24 Hz 动画,不是 GPU 受限;GPU 1716 MHz 全程。
- 已 BUILT 未上机:rev252cz(x9lc)、rev252da(1242x2760)、rev252db(DVFSRC 探针)、**rev252dc(INIT SMC,最新)**。
- **下一步**:
  1. 上机 rev252dc(无需 mailbox,插件仍 275):看 `X9 DDR: SMC VCOREFS INIT -> a0=…`、gear 是否变化、`wait_us`、presented fps。INIT 成功但 gear 仍 0 → 试 `x9_dvfsrc=2`(INIT+START)。若 SMC 卡死,退回 rev252db 并去掉 x9_dvfsrc。
  2. 检查 1242x2760 的触摸映射/黑边/123 是否仍正常(HID 偏移与 `SWEET FB … presented as 1242x2760` 日志)。
  3. 根据 DRAM 结果决定做 CSG 抢槽(§5.2)还是继续带宽方向。
  4. 竖线马赛克:拿一份含开关 app 的完整日志再做 §5.3 的前端纹理上传探针。
  5. Wi-Fi:上机 cm6 系列验证 WPA2-PSK 4-way(期望日志 `M1 -> PTK / M3 ok / install PTK / M4 / GTK installed`),再看 captive 探测。

## 9. 本地清理(2026-11-09): 清理过时镜像/解码日志/增量缓存;实际释放 33.67 GiB

- 用户要求清理项目 `.local` 和 `~/Heliodarwin` 的过时产物。本次无设备操作、无挂载变更、无构建/CI/启动;设备工作继续以 HANDOFF (82) 为准。
- 删除 9 项:未完成的 `.local/x9/work/x9-root-pull/root-20261109.hfs` (9.49 GiB)、旧 pan85 的 `root-after-pan85.partial.raw` (0.51 GiB);
  `.local/x9/work/x9-rootpull/` 中回放日志前的 `x9-unified.jsonl` (11.42 GiB)、`logd-20261109/` 和 `x9.logarchive/`;
  `x9-metal-render/general-target/{aarch64-apple-ios/release,release}/incremental/` 两处 Rust 增量缓存;
  `~/Heliodarwin` 顶层两个 iOS 16.7.16 IPSW (完整但已解包的 5.75 GiB 安装包 + 无有效 ZIP 目录的 70 MiB 残缺下载)。
- 删除完整 IPSW 前记录 SHA256/ZIP 文件索引并保存 BuildManifest.plist、Restore.plist;保留的 OS/SystemOS/AppOS 三份 DMG 与 Mav17 固件均逐一通过原 ZIP 条目的大小/CRC32 检查。
  `sources.json` 中 ipsw 字段现在是历史来源路径,不再存在;当前 `build_full_ios_root.py` 使用的三个 `.local/heliodarwin/.../*.dmg` 均保留。
- 保留:最新完整 16 GiB `x9-rootpull/root-20261109.hfs`、已回放的 `.clone.hfs`、正确的新 `x9b.logarchive` 和筛选后的诊断摘要;
  pan172 回滚根盘、所有挂载中的镜像、分区/基带备份、源码/补丁、冻结 dxb 输入、所有插件包、Rust release 库/deps/fingerprints、`~/.local` 开发工具。
  仅删 Rust 增量缓存,后续需要重编的 crate 会重建缓存;没有删掉当前 `libx9_general_metal_bridge.a`。
- 验证:`python3 -I x9/build/prepare-local.py --check` 清理前后均通过;受保护文件 inode/大小核对通过;两个仓库原有 tracked 修改的 SHA256/删除状态完全一致。
  原内核提交仍为 `d1f8515`;hk 镜像保留,`x9/out/x9-rev252hk-md/boot.img` = 15278080 B,
  SHA256 `136cd18d3fd25c3fa66e416117cf6488c0ea4c731d6ba32694ce0787628325a4`。本次未对镜像作新构建或设备验证。
- 实测文件系统可用空间 `31466758144 -> 67624251392` B (29.31 -> 62.98 GiB),释放 `36157493248` B (33.67 GiB)。APFS 共享块/硬链接使逐项大小之和不等于实际释放量。
- 私有完整清单/前后状态/校验依据/执行脚本:`.local/cleanup/20261109-1907/` (`plan.json`, `removed-files-manifest.jsonl`, `ipsw-provenance.json`, `result.json`, `prepare-local-after.log`);均不入 git。
  历史交接中指向上述已删除产物的路径不能再直接复用。清理记录提交为 `636edc2`,未夹带原 `.gitignore` 和三份渲染内核修改。

## 10. 开发工具交接(2026-11-09): cc-remote 配置完成;ss.key + sudo -i;已验证远端 Claude 读取本机文件

- 本次仅配置开发工具,未构建/启动设备,未修改内核代码;设备工作以 §7 的 HANDOFF (82) 为准。
- 安装:Bun 1.4.4、cc-remote 0.1.0 (claude-agent-acp 0.99.1)、acpc 0.1.0;三个命令均可从已有 PATH 的 `~/.local/bin` 使用,未改 shell 配置。
- cc-remote 源码:`~/.local/share/cc-remote/source`,上游提交 `063f81524e84b7ad40a203d4c7da6f92fcfd2e41`;按锁文件执行 `bun install --frozen-lockfile`,再 `bun run compile` 和 `bun link`。
- 主机文件:`~/.config/cc-remote/host` = `cc-remote-server`;对应 SSH 别名位于 `~/.ssh/config`。首次密钥被服务端拒绝;用户随后提供 `Downloads/ss.key`,该密钥已复制到 `~/.ssh/cc-remote-ss.key` (600),SSH 登录成功。旧密钥副本和 SSH 配置备份保留。地址和私钥仅存本机,不入 git。
- acpc 配置实际路径(macOS):`~/Library/Application Support/acpc/config.toml`;已注册 `Claude (remote)` / slug `cc-remote`,设为默认 agent,启动命令为 `cc-remote`。
- 用户确认 Claude 装在 root 下,应使用 `sudo -i`。运行时已验证 root 的 `/root/.local/bin/claude` = 2.1.319,`auth status` = loggedIn true / claude.ai / pro。不需要重新登录或安装 root Claude。此前启动的 ubuntu 用户安装进程已精确停止,其下载/部分安装文件保留,未清理已有 root 安装。
- 转接:`~/.config/cc-remote/ssh-sudo` 保留 SSH 参数和 stdin/stdout,将整个远端命令包装成 `sudo -n -i -- /bin/sh -c <quoted-command>`,带 SSH 连接复用和 keepalive。`~/.local/bin/cc-remote` 现为启动包装器,设置 `CC_REMOTE_SSH` 后执行原编译二进制;原 symlink 备份为 `cc-remote.without-sudo-*`。上游源码/依赖未改动。
- 已确认的兼容性问题:本机 ACP adapter 非 root,默认把 allow-dangerously-skip-permissions flag 传给远端,root Claude 因此在 session/new 退出。使用独立 `~/.config/cc-remote/claude-config/settings.json` (从现有本机 settings 复制,仅加 `permissions.disableBypassPermissionsMode="disable"`)解决;包装器设置本机 `CLAUDE_CONFIG_DIR`,远端 spawner 不转发该变量,继续使用 root 原登录。原全局 Claude 设置未修改;后续全局 settings 修改不会自动同步到此副本。
- 本机会话镜像现在存于 `~/.config/cc-remote/claude-config/projects/`,不是默认 `~/.claude/projects/`。可用模式已验证为 default / acceptEdits / plan / auto,不含 bypassPermissions。
- 验证(本机):编译成功;`bun test` = 30 pass / 0 fail / 76 expect;已编译二进制的 ACP `initialize` 返回 protocolVersion 1、agent version 0.99.1,无错误。
- 验证(端到端):`cc-remote doctor` = remote claude 2.1.319 / ok;新 ACP session `114ed4e7-a45d-4faf-b2c4-de4191a5e603` 成功调用 `Read File`,准确返回随机临时文件内容,end_turn。额外从 root SSH 检查映射远端目录中该文件不存在,证明读取发生在本机。测试不批准任何权限请求,未修改项目文件。
- 复验:`cc-remote doctor`;`python3 ~/.local/share/cc-remote/verify-local-read.py` (每次生成全新临时目录/随机内容);实际使用在项目目录运行 `acpc` 或 `acpc -a cc-remote`。SSH/sudo 均免交互,不需要手动 sudo -i。
- 证据:`~/.local/share/cc-remote/setup-logs/{doctor-root.log,auth-status-root.json,acp-local-read.jsonl,acp-local-read.stderr.log,acp-local-read-result.json}`;日志文件权限 600,包含账户元数据的日志不入 git。安装/测试日志同目录。
- 初始交接提交 `07c6e99`;成功结果记录于独立文档提交 `fb333b9`;推送状态以本次分支交接为准;既有内核未提交修改保持原样。
