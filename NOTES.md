# ios-sweet / X9 工作笔记(精简版,2026-11-08 整理)

> 旧的完整流水账(7784 行,含 sweet/SM7150 时代、rev2xx 全部 handoff)在 git 历史里:
> `git show a8f1955:NOTES.md`。需要翻某个 revision 的细节时 `git log -S'<关键字>' -- NOTES.md` 再 show。
> 本文件只留:目标、环境与命令、当前基线、已验证/未解决、关键结论、下一步。
> 约定(AGENTS.md):每个里程碑后更新「当前交接」;区分**观察**与**推断**;不把序列号/设备转储/Apple 产物放进 git。

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

最新:**x9-rev252dc**(内核-only,插件 275 不变,沿用 rev252cx 之后的 mailbox)boot_sha256 `041d267fc297dd0830bd50407b85579679f0ace28ad2ba94a7c825e3bd0fd80c`,**未上机**。
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

## 5. 未解决问题(按优先级)

1. **GPU 带宽/帧率**:avg ~17-22 fps,稳态 24 fps;job 内 GPU wait 6-15 ms。判断显存带宽受限。**DVFSRC 从未初始化**(110.txt:`basic=8 opp_count=1 gear=0`,SW_REQ 被忽略)。
   rev252dc 加 TF-A SMC INIT:`MTK_SIP_VCOREFS_CONTROL = 0xC2000506`,cmd INIT=0(a0=0 成功,a1=dram_type)锁高 OPP,START=1(`x9_dvfsrc=2`)后自由运行。**待上机**,SMC 可能挂。
2. **CSG 槽耗尽**:前 8 个 GPU 进程各占一个硬件 CSG 槽(group 创建时绑定,关设备才放);第 9 个客户端(AegirPoster)`X9RenderCreateGroup` 返回 0xe00002be,43 次失败。
   思路(未实现):冷提交时从最久未用的挂起 group 抢槽;障碍是 PanVK 初始化 CS 状态(subqueue ctx 寄存器、scoreboard、cs_heap_set、progress seqno)TERMINATE 后丢失,需内核保存/重放。
3. **开/关 app 动画出现 4 条竖向透明马赛克线**(间距 256 px,**一直都有**,非新回归)。已证伪:CA 256 宽 tile 上传缺失(TILEPROBE 无 256 宽 CPU 上传)、swipe 旧马赛克(InPlaceWrites 未启用)。
   现线索:缝位置 ≈ 层原点 + 256·k,即每行 **1024 B 边界**,指向某个渲染目标/IOSurface 每 1 KiB 处像素坏(缓存/内存类),不是绘制拆分。需要一份含开关 app 时刻的完整日志;x9vf 帧抓取只按固定 uptime 触发(参数上限 607 B),需改成按 transition 触发。
4. **Wi-Fi**:WPA2 阶段 2(4-way、INSTALL_KEY_V3、受保护收发)**rev252cm6 已构建、从未上机**;captive portal 弹窗不出现(探测 `captive.apple.com` 从未发出,疑 configd/CNS 的 SC 状态缺失);
   RUC-Mobile(802.1X)EAP 投递 ioctl 待映射;DBDC 重加入偶有无响应。
5. 硬件 vsync/帧节奏、背光控制(无 `backlight-control` 服务)、modem/WirelessRadioMan 噪声、部分 daemon 反复崩(locationd/speech、mediaserverd/AegirPoster、syncdefaultsd、CommCenter)、
   AKS selector(securityd 无真 keychain 保护)、hfs `LowDisk`(约 1 GiB 空闲)。
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

## 7. 当前交接(handoff 66,2026-11-08)

- 分支状态:`rev252cn` 已合并进 `main` 并删除(合并提交 a8f1955);代码最新提交 2a9a3a6(DVFSRC SMC)+ notes 2406f8d。
- 上一次实测(110.txt = rev252db):DVFSRC 未初始化;presented fps avg 22.5(55 窗口,max 35.8),60 s 后稳定 24.0 fps = 某个 24 Hz 动画,不是 GPU 受限;GPU 1716 MHz 全程。
- 已 BUILT 未上机:rev252cz(x9lc)、rev252da(1242x2760)、rev252db(DVFSRC 探针)、**rev252dc(INIT SMC,最新)**。
- **下一步**:
  1. 上机 rev252dc(无需 mailbox,插件仍 275):看 `X9 DDR: SMC VCOREFS INIT -> a0=…`、gear 是否变化、`wait_us`、presented fps。INIT 成功但 gear 仍 0 → 试 `x9_dvfsrc=2`(INIT+START)。若 SMC 卡死,退回 rev252db 并去掉 x9_dvfsrc。
  2. 检查 1242x2760 的触摸映射/黑边/123 是否仍正常(HID 偏移与 `SWEET FB … presented as 1242x2760` 日志)。
  3. 根据 DRAM 结果决定做 CSG 抢槽(§5.2)还是继续带宽方向。
  4. 竖线马赛克:拿一份含开关 app 的完整日志再做 §5.3 的前端纹理上传探针。
  5. Wi-Fi:上机 cm6 系列验证 WPA2-PSK 4-way(期望日志 `M1 -> PTK / M3 ok / install PTK / M4 / GTK installed`),再看 captive 探测。
