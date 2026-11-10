# ios-sweet / X9 工作笔记(精简版,2026-11-09 合并更新)

> 旧的完整流水账(7784 行,含 sweet/SM7150 时代、rev2xx 全部 handoff)在 git 历史里:
> `git show a8f1955:NOTES.md`。需要翻某个 revision 的细节时 `git log -S'<关键字>' -- NOTES.md` 再 show。
> 截至 2026-11-09 的完整 cn 交接在 `git show 636edc2:NOTES.md`;当前以 §7 的活动交接为准。
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

- **最新全功能整合候选(117)**:**`x9/out/x9-all117-dualsim-md/boot.img`**,固定完整main源码 **`5ce2d93`**,构建exit0、**13131776 B**,58/58 overlay+6prebuilt。包含相机7258af4、BT、torch、ALS、SMS/IMS、DMA40/PDP/PIT及本轮真实双卡/DDS切换修复。新增`x9_dual_sim=1 x9_camera=1`,保留ALS1/torch3/BT1与完整Wi-Fi/GPU/蜂窝参数;仅移除诊断`x9_md_poll=1 keepsyms=1`,最终605B。32GiB manifest/插件275不变,**无需mailbox、未上机**。双卡设置显示/切换仍待真实运行验收,相机/蓝牙/ALS物理后端仍有明确未实现部分,不能把联合编译通过当作全部功能可用;详见§7。
- **上一整合候选(116)**:**`x9/out/x9-cellular-als-torch-bt175-md/boot.img`**,构建exit0、**13049856 B**。在115基础纳入指定c9b3e46/3535ac5,固定独立源码f1cddba,不包含SMS/相机;新增只读`x9_bt=1`,probe/raw/discovery关闭。仅移除已被AT桥强制lvl3覆盖的冗余`x9_md_at=3`,603B完整参数;蜂窝/ALS1/torch3/Wi-Fi/GPU语义保持。32GiB manifest/插件275不变,无需mailbox,**未上机**;详见§7。
- **上一整合候选(115)**:**`x9/out/x9-cellular-als-torch175-md/boot.img`**,构建exit0、**13037568 B**。上一DATA+ALS+legacy PIT修正基线继续纳入用户指定5e06897/a4c8857;固定独立源码586184f,不包含BT/SMS。启用`x9_torch=3`,不自动点灯;仅去掉诊断`-v`以容纳606B完整参数,其余Wi-Fi/GPU/蜂窝/ALS配置不变。32GiB manifest/插件275不变,无需mailbox,**未上机**;详见§7。
- **175日志 / 上一DATA+ALS候选**:175的启动参数与`x9-cellular-data1-md`一致(完整hi + `x9_dpmaif=2 x9_mipc=1 x9_md_lte=0`),日志确认40-bit分配、INIT_DONE和真实SA→LTE;用户观察5G可显示/可上网,但该日志截至121s仍没有WDS Start或蜂窝收发,详见§7。上一整合候选 **`x9/out/x9-cellular-als175-md/boot.img`**,构建器报告**13025280 B**,包含DATA+ALS及15f597b的窄范围legacy PIT状态修正,仅追加`x9_als=1`(598B),32GiB manifest/插件275不变,无需mailbox;**未上机**。以下ic/ij等为历史基线,不再代表最新日志。

最新实测:**x9-rev252ic-md**(168,见HANDOFF102;LTE-only注册稳定,仍不拨号);再前 **x9-rev252ib-md**(167,见HANDOFF101);再前 **x9-rev252ia-md**(166,见HANDOFF100);再前 **x9-rev252hz-md**(165,见HANDOFF99);再前 **x9-rev252hy-md**(164,见HANDOFF98);再前 **x9-rev252hx-md**(163,见HANDOFF96);再前 **x9-rev252hw-md**(162,见HANDOFF95);再前 **x9-rev252hv-md**(161,见HANDOFF94;MIPC就绪、UIM读通);再前 **x9-rev252hu-md**(160,见HANDOFF93;RAM 10.0GiB生效);再前 **x9-rev252ht-md**(159,见HANDOFF92);再前 **x9-rev252hs-md**(158,见HANDOFF91;MIPC/DPMAIF在prepare/OPEN即隔离,UIM卡状态被拒);前一实测 **x9-rev252hl-md**(代码提交 **`20c64f2`**,构建交接 `0f9d983`),boot.img **12877824 B=12.28125MiB**,路径 `x9/out/x9-rev252hl-md/boot.img`。**157确认完整11文件安装成功、qdsp可读、Sahara finished并进入QMUX/QMI,握手摘要reset=0;真实iOS蜂窝尚未打通**,见HANDOFF87。独立34MiB固件与插件275不需再stage。
最新实测:**x9-rev252ij-md**(172,见HANDOFF109;根盘已扩到32GiB,能力门已装,SA注册,界面仍无信号/5G,SIM2被MD报未插卡)。旧16GiB manifest镜像已不可用;ig/if未单独上机;更早候选 **x9-rev252hq-md**(代码`ccbad81`),boot.img12906496B;沿用hn AT→QMI,新增DPMAIF只读/分配探针和WDS前置状态查询,见HANDOFF89。hq默认`x9_dpmaif=1`,不初始化硬件或激活PDP;hn保留。最新实测仍hl;不要把候选当作SIM/蜂窝已验证。
回退:**x9-rev252hk-md**(修复提交 `d1f8515` + 当时未提交的渲染改动,现已入库),boot.img **15278080 B**;156确认reserve生效、efs1.bin已有1MiB,当时15次reset及零传输见HANDOFF84。
本机回退镜像:`x9/out/x9-rev252hk-md/boot.img`;hl/hk配置沿用 `.local/x9/modem/boot-rev252hi-md.json`。
157已越过缺qdsp的前置阻塞,**当前阻塞在X9的QMI→MTK适配**:UIM/NAS/DMS/WDS/WDA/DPM请求实际到达后返回0x5e,数据端口配置失败,四个PDP接口仍blocked/tx=rx=0。Sahara是兼容桥交互,不向物理MTK下载Apple固件;PCIControl methods仍0x0,不能说PCIControl已通。以下GPU专用基线与参数为 **2026-11-08 历史记录**,不要当作hk/hl参数直接复用。

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

当前主线是 **QMI→MediaTek AT控制查询 + MIPC1/WDS + DPMAIF**(§7):175确认40-bit分配与INIT_DONE、真实SA→LTE,但没有WDS Start/ACT/GET/EIF或PDP收发;41.523s因raw DL状态0x8被隔离。15f597b区分未使用legacy PIT的masked状态与真实LRO/DMA错误,已与ALS整合编译,实机效果仍未验收。下一步先追iOS为何没有Start及NR回退的真实MD/网络原因,不伪造connected、不强锁NR、不连续刷探针。下列为保留的GPU/Wi-Fi待办,不是新的硬件验证结论。

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

## HEIF活动交接(HEIF-1,2026-11-10): 硬解接入路线调查,未实现/未构建

- **本轮范围**:用户问“看看heif硬解怎么实现”。只读175/既有Android日志、captured DT、厂商源与20H392映像,做本地容器解析/反汇编;没有新增驱动、修改Apple文件、构建/CI、测试套件或设备操作。共享main其他会话继续整合功能,本节不改变§7候选/验收状态。Apple文件/日志/分析脚本均在`.local`或既有`/tmp/x9root`,不入git。
- **iOS接入证据(20H392 unslid VA)**:`ImageIO::IIODetermineHEIFSupport`0x18bc9bdbc→`CMPhotoDecompressionSupportedForContainerAndCodec`0x1a5d82244;后者对container0/hvc1/null options返回类型支持,不是硬件探测。`CMPhoto::VTDecompressionPluginClass_create`0x1a5dde4b8→`VTDecompressionSessionCreateWithOptions`,decode0x1a5dde978→`VTDecompressionSessionDecodeFrame`。VT的`VTLoadVideoDecoders`0x194cc7ea4在0x194cc7fc8..0x194cc7fd4无条件尝试dlopen系统VCPHEVC并调用VCPHEVCRegisterDecoder;其once回调0x1bf94da04在0x1bf94da88调用`VTRegisterVideoDecoderWithInfo('hvc1',info,HEVCVideoDecoder_CreateInstance)`,info明确`IsHardwareAccelerated=false`。这证明系统已有软件注册路径,**不证明当前实机已成功解出HEIC**,也不需要为软件回退再伪造AppleVXD服务。
- **适配路线**:优先自有VT decoder通过上述注册入口接MT6993后端,保留ImageIO/CMPhoto处理HEIF item/grid/方向/ICC/alpha,不先复制AppleVXD UserClient全协议。VCPHEVC的CMBase class版本1、decoder表版本3已读出;自有factory/class各槽的完整ABI、registry选择/优先级和远程mediaserverd注册时机仍需确认,不能直接猜vtable。CMPhoto有Enable/RequireHardware不同分支和普通DecodeFrame路径;先做非native-tiling单item,再做grid,不提前声明native tile能力。torch/camera已有注入,新增桥需统一多库装载,不能互相覆盖。175仅有SpringBoard匹配AppleVXD375/390/393→none与Arm.HEIC写入,没有硬解完成证据。
- **硬件/固件证据**:live.dts18434的MT6993 vdec节点ipm2、power/mmdvfs-in-vcp1、mm_smmu_codec/20个VDEC ports;节点**没有reg/interrupts**,不可按旧平台猜VDEC IRQ/MMIO。VCP节点有SRAM0x31800000/0x80000、三mailbox和两核。既有Android dmesg12019确认真实HEVC实例,12308报1256x2760/10bit、buffer1280x2816、fm=P01H;加载模块表是vcp_v2,因此后续看`drivers/misc/mediatek/vcp/rv_v2`,不用旧rv启动代码。厂商`vdec_drv_if.c`→`vdec/vdec_vcp_if.c`以VSI中的bitstream/output IOVA发INIT/START/QUERY_CAP,IPI OUT0/IN1;v2 reset_vcp先写packed IOVA/size再SMC RESET_RELEASE并等READY。**核心是VCP固件+共享内存/SMMUv3/时钟电源/完成回调移植**,不是Linux V4L2接口机械改名;固件来源、LK暂态启动后的可用性和loaded KO精确ABI尚未确认(Android函数日志行1454与本地源1345不同),不能硬编码source-only结构为已验证协议。
- **输出与真实样本的两个坑**:`mtk-v4l2-controls.h:93`明确P01H=`P010_HYFBC`,不是线性P010,不能直接封为CVPixelBuffer。需真实QUERY_CAP确认可选线性NV12/P010,否则增加支持该压缩布局的硬件转换;同时接IOSurface缓存维护/stride/crop/8与10bit/色彩附件/完成同步。既有375w壁纸Arm.HEIC离线box与ffprobe一致:primary10=grid,9块512x512 Main/8bit/420彩色,canvas1326x1326;aux20=grid,另9块Rext(profile4)/8bit/monochrome,auxl20→10、auxC=`urn:mpeg:hevc:2015:auxid:1`(alpha)。**普通Main/Main10成功不等于这张壁纸全硬解**;需确认固件单色Rext能力,不支持就保留该item软件回退,不能丢alpha或把全文件当一帧。
- **参考/最小推进**:本地UniXNU ae68700的壁纸脚本转PNG;hackios c84b463及hakios-xnu-kernel-g52 8851317的inject_media明确用系统VCPHEVC软件回退,未找到可直接移植的MT6993 HEIF硬后端。下一步先离线固定实际VCP固件/loaded KO和内存/IPI ABI,然后VCP READY→QUERY_CAP→一块真实512x512 Main item→线性输出→VT/CMPhoto/ImageIO消费;之后扩Main10、grid和alpha。验收要有硬件任务完成与真实像素输出,以及指定硬件decoder被选择的证据;服务出现/函数返回0/显示器名都不能代替。先不安排连续探针镜像。
- **复查材料**:`.local/x9/work/heif-20261110/`含`replay.txt`精确命令、`heif_dis.py`/targets、`heif_boxes.py`、`arm-boxes.txt`/`arm-stream-groups.json`、VT/VCP/CMPhoto反汇编与nm、Android/175证据。反汇编复用既有macho_evidence.py和真实split cache;其raw cache常量的二次解引用注释有误配风险,结论取指令/GOT符号与box原字节,不据注释猜CF对象。本轮只有本节交接入库,无新boot.img/CI run/上机结果。

## 相机活动交接(CAMERA-2,2026-11-10): 有条件AP-I2C/WL28681C芯片探测;采集/冷启动资源仍缺

- **续做范围/隔离**:新独立worktree `.local/x9/worktrees/camera-probe` / `work/x9-camera-probe`,创建时main另一窗口恰好提交双卡整合,实际基线为 **5ce2d93**(含117完整功能源码),不是旧phase1c树。未夹带/覆盖另一会话的暂存修改,不主动push/设备操作/测试套件。
- **板级事实**:captured `.local/x9/modem/live.dts`追到WL28681C在 **i2c12/0x35**,主摄AVDD/AVDD1/DVDD是WL4/7/2,前摄AVDD/DVDD是WL3/1;DOVDD为SID3 LN60_7,主AFVDD为SID6 LDC0_3。MT6993 IMPC i2c12为bit0、IMPE i2c2/4为bit1/2;不能用mt6991 clock表的bit位置。DT没有ch-offset-i2c,实际channel0而非猜测AP+0x10000。CAMTG7/1 parent7是UNIVPLL_192M_D8。来源为OPPO公开clk-mt6993、pinctrl-mt6993、i2c-mt65xx、spmi-mtk-pmif-core、mt6661-regulator头及OPLUS WL driver,不是本轮XNU实测。
- **实现**:`x9_camera_mt6993.inc`含AP FIFO/WRRD(每请求约5ms上限)、Sony16-bit寄存器ID读取、WL产品ID0x0d/指定通道电压/启停、reset/MCLK pinmux、失败/还原journal。无DMA/总线reset/宽泛地址扫描/重试,未知继承IRQ不清。保留第一个出错状态;独立restore_errno,还原失败锁存、不宣布资源已恢复。只改disabled-at-entry的目标WL位,其他通道保持,live通道不改电压;不重置WL共享GPIO203/187/202。
- **重要前置/未完成**:这是**有条件的probe**,不是完整power/capture backend。只借用已运行24MHz CAMTG、已ungated且timing非零/四bank idle的AP I2C、已normal/DA-enabled的精确DOVDD1.8V(主另需AFVDD3V),不改PLL/mux/voter或共享PMICrail、不接管SCP引脚。reset必须output-low/MCLK pinoff/目标WL通道off。冷LK基线很可能不满足,将返回明确blockers而非强行上电。仍缺冷启动clock/shared-regulator ownership及SENINF/RAW DMA/SMMU,**未读到任何本轮实机ID、不能拍摄**。
- **模式/ABI/iOS**:新增 **x9_camera=3**仅root显式PROBE_SENSOR ioctl(需FWRITE),节点root:root/0600;启动/status零MMIO、不写root/不注入iOS、不写sensor stream-on/厂商表。每sensor每boot最多成功一次,任何失败锁存。GET_PROBE保留最后证据;原status88B ABI不变,使用两reserved字传probe_mask/flags。MATCHED+RESTORED才置PROBED,**supported_mask与READY仍0**,没有假capture回调或platform注册。mode1/2原语义不变。cameractl加`probe back/front`及决定阶段/错误打印。
- **共享PMIF必要修正**:按键与RTC实际也用PMIF-M SWINF2,此前未进入X9PMIF锁;本轮把按键完整transfer、RTC单次transfer、modem已有error-clear接入同一锁,不改变业务命令/日历/按键逻辑。RTC不持总线锁IOSleep。相机只在仍持该锁时排空自身确定的迟到读响应,解锁后不猜归属/清响应。该锁变化已编译、未实机验证。
- **编译里程碑**:用户态camera桥/cameractl交叉编译+签名通过(桥117040B);camera、keys、SweetPlatformExpert三个对象本地编译通过。首次孤立编译gpu_dependencies被缺少尚未生成的x9_mdota.inc阻止,不是编译通过;完整构建器会先生成依赖。本轮未运行测试脚本/测试套件。原始日志 `.local/x9/work/camera-probe-20261110/{object-build.log,object-build2.log}`;gh run list为空,无CI run。
- **完整构建结果**:固定代码提交 **bf2e2c6**(5ce2d93+本轮相机/共享锁)的联合树构建 **exit0,58/58 overlay+6prebuilt**;完整构建器生成MDOTA等依赖,包含此前单对象因缺生成头未能编译的gpu_dependencies,内核link/strip与shim/boot打包均通过。构建器报告 **`x9-camera-probe2-all117/boot.img` 13135872 B**,camera桥117040B、torch桥72464B;没有镜像成功后再解包/hash/反汇编/二次重建或额外测试。
- **参数/输入/交付**:继续用固定117的完整配置,唯一参数变化x9_camera=1→3,仍 **605B**/32GiB manifest/插件275不变,**无需mailbox**。保留双卡/蜂窝/Wi-Fi/GPU/ALS/torch3/BT1等既有参数;mode3不安装相机桥,不引入torch的双桥注入问题。私有 `.local/x9/work/camera-probe-20261110/{boot-config.json,local-config.json,inputs.json,build.stdout.log,build.exit}`(exit内容0)。完整原始输出保留独立树`x9/out/x9-camera-probe2-all117/`,并原样复制主checkout同名目录,没有覆盖117或phase1c成功镜像。该次没有重新编译/额外交付btctl;cameractl在`camera/cameractl`,没有自动部署。
- **main整合/停工交接**:代码已cherry-pick为 **d9d417b**,构建交接 **ea7724b**;只有NOTES插入点与另一会话HEIF-1交接冲突,已完整保留HEIF-1与HANDOFF117后插入CAMERA-2,所有源码无冲突。此次构建仍对应固定bf2e2c6,不宣称main之后的新提交已经重新构建。独立树提交bf2e2c6/e674243保留,main工作区干净;没有主动push或设备操作。
- **精确复建**:在固定bf2e2c6的相机probe worktree运行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --name <新的唯一名字> --config /Users/tosasitill/ios_plj110/.local/x9/work/camera-probe-20261110/local-config.json --boot-config /Users/tosasitill/ios_plj110/.local/x9/work/camera-probe-20261110/boot-config.json -j 8`。无CI run/主动push/ADB/fastboot/flash/reboot/根盘写入,没有本镜像新设备日志或ID读数。
- **下一步**:代码与构建阶段已推进;不要求现在刷冷启动前置未完成的镜像。之后补真正的CAMTG/PLL/voter和共享rail ownership,再在获用户操作的同一日志中确认主摄ID→恢复,然后前摄;只有真实SENINF→RAW DMA完整帧之后才推进iOS拍摄/落盘验收。

### 前次相机交接(CAMERA-1,2026-11-10): 控制/RAW10/iOS双流初步移植;硬件后端未实现

- **用户目标**:参考 UniXNU / hackios 做前后相机初步实现、驱动构建与 iOS 对接。当前仅代码/构建工作;没有设备操作、flash/reboot、分区写入、mailbox staging、测试套件或 CI run。
- **上游核对**:本轮 fetch 后 UniXNU `95ad781`没有相机/MT6993 ISP实现;HakiOS `63fc2b4`的userland `camera_device.c/camera_isp.c`提供RAW10→软件ISP→FigCaptureDevice双流。其高通内核相机已迁到drivers/camera/hakios.camera.c,没有移植高通MMIO。移植保留MIT许可证;厂商寄存器表仅本地生成,保留GPL/OPLUS/MediaTek声明,不入git。
- **设备观察来源**:既有`.local/x9/android-ccci-20261109-125536/dmesg.txt`5626起选中主摄`hhmain_mipi_raw`、6036起选中前摄`hhfront_mipi_raw`;captured DT `.local/x9/modem/live.dts`对应i2c4/0x1a和i2c2/0x10。芯片物理ID0x9186/0x0615来自厂商实际get_id分支,不是宏中加offset的软件ID。不是本轮XNU读到了ID。
- **20H392 ABI证据**:已有设备cache抽取`H10ISP.mediacapture`的base/device/stream表确认base v1、device v3、stream v2,设备和流表分别5/4个64-bit槽;CMCapture vendor IMP为0x1a1f40c84,调用initWithDeviceCreateFunction:在0x1a1f40cd8。UUID沿用已核对45d37488-2cc7-3028-8a4d-936bce7a220b。原始证据`.local/x9/work/camera-20261110/abi.txt`及既有torch的`cmcapture-dis.txt`;共享cache encoded指针保留原值,不当成可直接调用地址。
- **实现边界**:`sweet_x9_camera.c/x9_camera.h`含独占owner、两传感器power数据/ID验证、init/preview、stream/exposure、16MiB RAW10、有界1s等待、generation旧帧拒绝、关闭排空/恢复。控制锁与帧锁分开,停流不持帧锁等待DMA worker。Sony倒数编码增益转线性Q10,不照搬高通线性寄存器。`x9/camera/`有MIT双流/软件ISP/状态工具/构建器;只提供D22 Photo兼容格式,未声明视频/高FPS/AF/OIS/flash。
- **模式**:x9_camera=0默认off;=1只创建设备/状态,不访问MMIO/I2C/电源、不写root;=2才允许后端注册并安装hash-addressed自有dylib/注入mediaserverd。**MT6993 power/I2C/SENINF/RAW DMA/SMMU adapter尚未实现、没有register调用者,所以supported=0、无READY是当前正确状态,不能拍摄**。两枚真实ID确认后才建iOS vendor;不覆盖原非空vendor,不伪造图案/照片。模式2首次设备运行须先确认root写入。
- **对接约束**:20H392原plist运行mediaserverd为mobile(501:501),非macOS的32;writer限root或该身份mediaserverd。原DYLD_INSERT_LIBRARIES保留,若torch=3已注入,相机跳过;多桥同时启用尚未整合。颜色CCM=identity/无LSC,光学字段是D22兼容描述,朝向/Bayer/实际帧率/色彩待实机帧确认。
- **已知构建里程碑**:用户态117040B初步桥交叉编译/签名通过;首次kernel使用已从XNU移除的_MALLOC/M_DEVBUF失败,改kalloc_data/kfree_data后`phase1b`56/56 overlay+6prebuilt、link/strip/boot打包exit0(13090816B)。随后静态审查修复停流/完成锁顺序、读等待deadline/session取消;最终干净源码`ca3a93e`构建`x9-camera-phase1c`exit0,56/56 overlay+6prebuilt、link/strip/boot通过,boot.img **13094912B**,用户态桥117040B;phase1b不再是最终候选。无测试套件/成功后二次镜像检查。
- **提交/最终产物**:代码`ca3a93e`在独立worktree `.local/x9/worktrees/camera` / 分支work/x9-camera,基于5e06897(含已提交ALS/torch,不含后续SMS/Bluetooth修改)。最终产物`x9/out/x9-camera-phase1c/`已原样复制到主checkout同名目录;mode1、完整boot参数601B、32GiB manifest、插件275不变,**无需mailbox**。本次不是包含后续全部功能的联合镜像,不要求现在上机。末尾仅清理import行尾空格并补交接,无功能修改,不二次解包/hash/验证镜像。
- **main整合**:相机实现已cherry-pick为 **`7258af4`**,构建交接 **`343608a`**;唯一冲突为NOTES,保留当前main全部114/torch/SMS/Bluetooth交接后插入相机节。构建器/recipe/link-order自动合并,没有覆盖其他功能代码。当前main还含后续SMS/Bluetooth/torch改动,本轮未重编联合树;上述phase1c仅代表固定相机worktree的构建,不能说全部功能联合镜像已验证。没有主动push。
- **重建与证据**:在上述worktree运行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --name <新的唯一名字> --boot-config /Users/tosasitill/ios_plj110/.local/x9/work/camera-20261110/boot-config.json`;worktree私有local.json含camera_vendor_root指向OPPO模块common目录。日志`.local/x9/work/camera-20261110/{user-build.log,user-build2.log,build-phase1.log,build-phase1b.log,build-phase1c.log}`,最终输出下camera/sensor-tables.json记录表对数/来源;ABI/配置/设备信息均不入git。未建立CI run(gh run list空),无新增设备日志。
- **下一步**:硬件阶段按一枚sensor上电→AP I2C ID→恢复,再SENINF包→RAW DMA完整帧,最后前后切换/预览/落盘照片。不能通过空ACK或字符节点“验收相机”。不要求用户现在连续刷试探镜像。

## 7. 当前交接(HANDOFF 117-ALL,2026-11-10): 最新main全功能联合镜像构建通过,未上机

- **本轮要求/里程碑**:用户先要求继续反编译修复真实双卡显示/切换,随后明确要求把其他窗口的手电筒、蓝牙、相机`7258af4`等全部纳入最新完整镜像。`7258af4`及SMS/IMS、BT、ALS、torch、DMA40/PDP/PIT均已是当前main祖先;不是继续构建116那份缺SMS/相机的独立树。双卡先在`fix/dual-sim-switch`完成代码提交 **`2fd6b03`**,再逐hunk集成main;三个冲突都保留两侧的MIPC白名单、WMS/IMSA服务和SMS生命周期回调,没有覆盖其他窗口的实现。本次无设备操作/测试套件/插件重建。
- **双卡确定证据/修复**:172反复SIM1真实READY/EF_DIR app1、SIM2由MD报absent,174没有开启dual;Android既有只读状态是两卡LOADED。原厂PLJ110 `libmtk-ril::bootupSetRadioPower`(ELF VA`0x397020`)按启用槽bitmask发送`EFUN`,两槽为 **`AT+EFUN=3`**,而旧实现只有CFUN1。启动与DMS ONLINE均补EFUN3,单卡保留CFUN1;CFUN4/enable需真实无overflow的final OK,ERROR/timeout不释放桥、不重发未知操作。**原厂流程差异已确认,尚未证明是现场SIM2 absent的唯一根因**。
- **真实DDS而非BindSubscription**:新增DSD2a服务,实现38/4e/4f。20H392反汇编确认4e目标TLV1/u32、type10/u32,4f current10/u32,50 current IND1/u32、4e completion IND1/u8;`0x1f1a01048`的current IND才完成iOS动作。MTK`RmmDcEventHandler::requestSetPreferredDataModem`(`0x2cb1a0`)发送 **020d、header mask1/2、无TLV**,匹配020e/TLV0 u32 result0后才更新current并通知。缺少/畸形CNF、timeout/client死亡保留mutation owner不重复020d;两张真实distinct ICCID/READY、同card epoch才接受成功,切卡前排空真实owned WDS并挡新Start。原厂RF occupy false在缓存已false时跳过(`0x2bc830`),本桥从未声明occupy,没有添加猜测RF命令。
- **初始状态/后续路由**:没有建立MTK独立DDS读取命令,不把SIM1常量伪装成读回。首次DSD请求后,两真实卡READY才明确执行host启动选择策略 **`x9_dds_boot_slot=1`默认(2选SIM2,0禁用)**;真实020e后4f才回答已确认状态,之前有界等待/NotReady。WDS5569经`0x1f19d751c/0x1f1925bcc`确认NotifyCurrentDataSub(TLV10/u8、11/u8 reason1..3),仅允许路由到已确认DDS且不重定向活跃bearer,不是切卡成功ACK。172/174 QMUX都达到64 live clients,客户端有界表扩大到128,为双卡额外NAS/WDS/DSD留容量,CID/DEL_CLIENT代际语义不变。
- **Settings显示条件已反编译**:`PSSimStatusCache::isDualSimCapable`(`0x1f3d3e2b0`)就是subscriptionContexts.count>1;`SettingsCellularUtils::isUIDualSIM`(`0x1f12ccf88`)还要求真实plan/dangling/pending总数>1。`setSelectedPlanItemForData`(`0x1f1316adc`)走CTCellularPlanManager,CC server(`0x100c5c334`)按真实ICCID调用控制器。不加占位SIM/plan或改UI判断;必须由第二槽真实CPIN/ICCID/EF_DIR和原有provisioning映射形成第二context/plan。**设置两卡显示、两个方向切换和对应卡收发尚未实机验收**。
- **编译/私有证据**:隔离树最终两个单元`sweet_x9_gpu_dependencies.o`与`X9Baseband.cpp.o`编译exit0,只编译未跑测试。反编译/脚本/日志保留`.local/x9/work/dual-sim-20261110/`(`ril-radio.asm`,`mipc-dds.asm`,`rf-occupy.asm`,`ps-dual-capable.asm`,`settings-ui-dual.asm`,`dsd-current-handler.asm`,`wds-route-wire.asm`,`compile-isolated-final.stdout.log`,`compile-selected.py`)不入git。双卡与最新main整合提交 **`5ce2d939a90d154dff28b88b81061a0146ca926d`**,随后冻结此完整树构建,下列结果不是116或相机单功能树的结果。
- **冻结源码/包含范围**:detached worktree **`.local/x9/worktrees/all-features-117/`**,HEAD **`5ce2d93`**。其祖先含相机 **`7258af4`**、蓝牙 **`c9b3e46`**、torch **`5e06897/a4c8857`**、ALS **`9323bcc`**、短信/IMS **`6eb119f/fa83fbf`**、数据面 **`be06cf1`** 与PIT修正 **`15f597b`**。本次全量编译联合源码,不复用116旧蜂窝对象;之后若其他窗口继续提交,不能把新提交冒充已包含在这份固定镜像里。
- **完整启动配置**:沿用116完整Wi-Fi校准/GPU/蜂窝/ALS/torch/BT配置,新增 **`x9_dual_sim=1 x9_camera=1`**;保留 **`x9_md_lte=0 x9_dpmaif=2 x9_mipc=1 x9_bb=1 x9_als=1 x9_torch=3 x9_bt=1`**。为607B ABI上限仅移除诊断 **`x9_md_poll=1 keepsyms=1`**,最终 **605B**;没有移除GPU频率、Wi-Fi校准或数据链路参数。32GiB root manifest不变;私有local-config补入真实camera_vendor_root。插件仍 **275**,没有传--revision/重建插件,**无需mailbox**。
- **最终构建结果**:本地构建器报告 **58/58 overlay + 6 prebuilt**,kernel link/strip、shim/boot打包均成功,**exit0**。torch桥 **72464 B**、camera桥 **117040 B**,BT固件仍11节metadata,不下载固件字节。最终 **`x9/out/x9-all117-dualsim-md/boot.img`**,构建器报告 **13131776 B**(约12.52MiB);冻结worktree完整原始输出目录已原样复制至主checkout同名目录,包含`bluetooth/btctl`。成功后未解包/hash/反汇编/二次重建,未跑测试套件/CI/push,没有上机、ADB/flash/reboot/分区写入,**构建通过不是硬件验收**。
- **可重建输入/原始日志**:私有 **`.local/x9/work/all-features-117/{local-config.json,boot-config.json,inputs.json,bt-client.stdout.log,build.stdout.log,build.exit}`**,build.exit内容0。在上述固定5ce2d93 worktree运行 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --name <新的唯一名字> --config /Users/tosasitill/ios_plj110/.local/x9/work/all-features-117/local-config.json --boot-config /Users/tosasitill/ios_plj110/.local/x9/work/all-features-117/boot-config.json -j 8`。btctl由同一5ce2d93源码先独立编译后复制随目录交付;需重建此客户端时运行 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/bluetooth/build.py --out x9/out/<该次新名字>/bluetooth`。用户手动启动命令 **`fastboot boot x9/out/x9-all117-dualsim-md/boot.img`**;本轮没有要求立即反复试探启动。
- **功能边界/下一步**:双卡真实EFUN3→两槽CPIN/ICCID/EF_DIR→两context/plan→DSD020d/020e→current IND链已编入,仍没有本镜像的设置两卡显示、SIM1↔SIM2切换和对应卡收发证据;不宣告双卡目标已达成。4G/5G最新数据面/信号修正已含,175的真实SA→LTE与用户联网观察不等于本镜像WDS收发验收。torch3桥启动不点灯,实体点灯/熄灭未验收。**BT1只读诊断,firmware_ready仍0;完整上电/固件/校准及原始bluetoothd后端尚未完成,不能扫描配对**。**camera1仅状态诊断;MT6993 power/I2C/SENINF/RAW DMA adapter尚无实现,supported=0,不能拍摄**,未启camera2安装/注入,也未整合其与torch3的多桥注入。ALS物理SCP输入/背光仍缺;SMS GSM收发ACK与IMS注册适配已含,实际配送/VoLTE业务未验收。Wi-Fi/GPU/触摸/桌面保留既有能力,本轮没有新的运行验证。继续修复时先用现有反编译证据与源码追真实状态,不造卡/plan或成功ACK,不把诊断服务出现当作硬件可用。

### 前次交接(HANDOFF 116-INTEGRATE-BT,2026-11-10): 蜂窝+ALS+torch+BT独立整合构建通过,未上机

- **用户要求/源码范围**:用户指定代码c9b3e46、交接3535ac5。两者已在main历史,不重复提交代码;把它们继续纳入上一蜂窝+ALS+torch候选。新独立树`.local/x9/worktrees/cellular-als-torch-bt-175/`以586184f为基线,依次cherry-pick为**efad465**(代码)、**f1cddba**(交接),只增加12个BT实现文件及11行BT交接。代码无冲突;文档插入上下文原本含SMS段,仅抽入完整BT段并保留独立树既有文档,未夹带SMS代码或回退main活动交接。
- **参数/边界**:保留DMA40/PDP/WDS、single-SIM+NR、ALS1、torch3及Wi-Fi/GPU配置,新增**`x9_bt=1`**只读AO/时钟门诊断;probe/raw/discovery仍关闭,不接管UART/自动发ROM查询。606B旧参数追加BT超607B;源码`x9_at_bridge.inc:17`明确`x9_bb=1`且默认`x9_qmi_at=1`启用AT桥,`x9_modem_s3.inc:1127`强制lvl3,所以仅移除冗余`x9_md_at=3`,新完整参数**603B**,AT初始化语义不变。32GiB manifest、插件275不变,无需mailbox。
- **构建结果**:固定f1cddba独立树的iOS16 arm64 `btctl`编译/adhoc签名、torch桥/工具编译通过;BT固件**11节metadata**(不嵌入/下载字节),torch桥72464B。57/57 overlay+6prebuilt、kernel link/strip、shim/boot打包exit0。**`x9/out/x9-cellular-als-torch-bt175-md/boot.img`**构建器报告**13049856 B**;完整原始目录`.local/x9/worktrees/cellular-als-torch-bt-175/x9/out/x9-cellular-als-torch-bt175-md/`已原样复制主checkout,旧成功候选未覆盖。客户端在新产物的`bluetooth/btctl`,未自动安装/运行。成功后未解包/重算hash/反汇编/二次重建;无测试套件、CI run、push、设备操作/flash/reboot/分区写入,无需mailbox。
- **精确复建/交接**:在上述f1cddba独立树先`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/bluetooth/build.py --out x9/out/<新名字>/bluetooth`,再`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --name <同一个新名字> --boot-config /Users/tosasitill/ios_plj110/.local/x9/work/cellular-als-torch-bt-175/boot-config.json`。输入/原始日志`.local/x9/work/cellular-als-torch-bt-175/{boot-config.json,bt-client.stdout.log,build.stdout.log}`;main里程碑文档提交3cf79ff,本次续补最终结果。用户手动启动路径`fastboot boot x9/out/x9-cellular-als-torch-bt175-md/boot.img`;若有新日志,先查`X9 BT:`只读前置与状态,不以服务/11节表/编译当作ROM握手或蓝牙可用。**firmware_ready仍0**,完整上电/固件/校准/ownership和原始bluetoothd传输未实现,扫描/配对未验收。蜂窝真实收发、ALS物理SCP/背光、torch实体灯及MG缓存等约束不变;本轮只整合指定提交,未上机。

### 前次交接(HANDOFF 115-INTEGRATE,2026-11-10): 蜂窝+ALS+两个手电筒提交独立整合构建通过,未上机

- **本轮范围**:用户明确“继续合:5e06897、a4c8857这两个”。它们已是main祖先,不在main重复提交源码;本轮是把这两份代码纳入上一蜂窝+ALS候选。独立树`.local/x9/worktrees/cellular-als-torch-175/`从ca4980f开始,依次cherry-pick成**d68c37b**(5e06897)、**586184f**(a4c8857),保留DMA40/PDP/WDS、single-SIM+NR能力门、ALS与legacy PIT修正;没有夹带BT/SMS等功能。
- **合并里程碑**:代码没有冲突;只有5e06897的NOTES插入依赖已存在的ALS交接段,已在独立树采用该提交的文档版本后继续,主checkout活动交接不回退。a4c8857完整应用。源码独立固定586184f,main不切分支、不覆盖其他会话源码。
- **参数选择**:完整上个蜂窝+ALS配置继续保留,启用**`x9_torch=3`**iOS受限writer/flash元数据/自有mediaserverd桥,**启动不自动点灯**,不选择会自动25mA脉冲的mode2。598B旧参数追加torch会达609B而超607B ABI上限;仅删除诊断`-v`,新参数**606B**,Wi-Fi校准/GPU/蜂窝/ALS开关不变。32GiB manifest、插件275不变,无需mailbox;不自动清MG缓存。
- **构建结果**:本地iOS16 arm64桥/torchctl编译和adhoc签名通过,桥**72464 B**;55/55 overlay+6prebuilt、kernel link/strip、shim/boot打包exit0。**`x9/out/x9-cellular-als-torch175-md/boot.img`**构建器报告**13037568 B**;完整原始目录`.local/x9/worktrees/cellular-als-torch-175/x9/out/x9-cellular-als-torch175-md/`已原样复制主checkout,没有覆盖上一成功候选。编译不等于实机成功;成功后没有解包/重算hash/反汇编/二次重建。没有测试套件、CI run、push、ADB/flash/reboot/设备分区写入,无需mailbox。
- **重建/交接**:在固定586184f独立树运行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --name <新的唯一名字> --boot-config /Users/tosasitill/ios_plj110/.local/x9/work/cellular-als-torch-175/boot-config.json`;原始输入/日志在`.local/x9/work/cellular-als-torch-175/{boot-config.json,build.stdout.log}`。源整合里程碑已在main文档提交b0027aa,本次续补最终结果。手动启动路径`fastboot boot x9/out/x9-cellular-als-torch175-md/boot.img`,仅由用户操作;若提供新日志,看`X9 TORCH:`安装/环境与`X9 TORCH MEDIA:`hook/RPC,最终仍需实体点灯和熄灭观察。旧MG缓存可能隐藏按钮,本轮不自动清理;panic/停调度时可靠熄灭未实现。蜂窝真实收发、NR回退原因、ALS物理SCP与背光边界仍同114,本轮只整合指定提交,不扩展验收结论。

### 前次交接(HANDOFF 114-175,2026-11-10): 175真实SA→LTE;蜂窝与ALS独立整合构建通过,收发/物理光感仍未验收

- **用户观察与日志边界**:用户报告5G现在能显示、会回LTE且可以上网。只读`~/175.txt`(2067434B,末121s),3.153s能力门切为single-SIM+NR;16.011s DMA40分配通过,高位均1;22.859s INIT_DONE,22.871s MIPC OPEN/DEACT/EIF订阅完成。25.122s真实`C5GREG=1,CEREG=0,AcT=11`(SA),55.144s真实`C5GREG=0,CEREG=1,AcT=7`(LTE)。**本日志没有WDS Start(0x20)、ACT/GET/EIF、成功绑定;截至119.253s四个PDP仍families0/blocked1/tx0/rx0,DPMAIF submitted/completed/RX均0**;120.978s Wi-Fi亦joins0/tx0/rx0/en0=none。不能用这份日志验证蜂窝或Wi-Fi联网,也不能否定日志结束后用户的上网观察;需要对应实际浏览时间的记录。
- **NR回退结论**:19.282s明确`x9_md_lte=0, RAT left as the MD default`;NAS0x33/0x34策略请求被拒,没有成功设置LTE-only的证据。回退是MD查询得到的真实注册变化,不是仅UI替换。没有PDP激活,不能说是已联网PDP切换失败。MD/网络的降级原因尚无reject/URC证据,本轮不猜运营商/信号、不发送ERAT锁NR。
- **窄范围修正15f597b**:41.523s旧轮询因DL raw0x8隔离。厂商`ccci_dpmaif_reg_com.h:573`把bit3定义为**legacy/non-LRO PITCNT**;`drv3_isr0`先用mask→status转换过滤,该bit在LRO默认路径不参与lenerr handler,真实LRO PIT错误是status bit8/9。轮询所有IRQ masked不能简单套整张mask。只在bit3仍masked、HPC模式仍2时排除这一未使用legacy源;记一次明确日志,其他LRO/MTU/BAT/SKB/未知错误保留fatal/原始快照/DMA保留,无reset/retry。**源码及编译证明这条分支已修正,尚无新实机日志证明运行效果或联网成功**。代码在独立树提交ca4980f后cherry-pick到main成15f597b,只改X9DPMAIF.inc。
- **ALS合入范围**:9323bcc原本已在main,没有重复cherry-pick;上一DATA镜像没包含它。本次独立树固定**9323bcc + ca4980f**,保留be06cf1蜂窝/PDP/WDS和1ce5010能力门,全量编译ALS HID/MTK ABI入口,不复用il旧蜂窝对象、不混主checkout其他会话的BT/SMS/torch(即使它们稍后已经提交)。参数仅在175对应完整DATA配置追加**`x9_als=1`**真实输入模式,不是诊断固定lux。**物理SCP/IPI producer仍未实现,不会产生物理lux;自动亮度还缺面板背光控制**,详见113-ALS,不以服务发布当作光感已修好。
- **构建结果**:本地54/54 overlay+6prebuilt、kernel link/strip、shim/boot打包exit0;`x9-cellular-als175-md/boot.img`构建器报告**13025280 B**,原始输出在`.local/x9/worktrees/cellular-als-175/x9/out/x9-cellular-als175-md/`,已原样复制主checkout **`x9/out/x9-cellular-als175-md/`**。完整598B启动参数、32GiB manifest、插件275不变,**无需mailbox**。成功后未解包/重算hash/反汇编/二次重建;没有测试套件、CI run、主动push、ADB/flash/reboot/分区写入或新候选上机结果。
- **精确重建/证据**:在`.local/x9/worktrees/cellular-als-175/`的ca4980f源码树运行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --name <新的唯一名字> --boot-config /Users/tosasitill/ios_plj110/.local/x9/work/cellular-als-175/boot-config.json`。私有原始/解码输入`.local/x9/work/cellular-als-175/{boot-config.json,build.stdout.log,log175-state.txt,vendor-irq-filter.txt}`,完整关键行`.local/x9/work/cellular-data-20261110/log175-key.txt`;不入git。可复放日志查询`rg -n 'X9 DPMAIF:|X9 WDS:|X9 MIPC:|NAS SIM1 registration|COACHZ pdp:.*(blocked|active families)|X9 WIFI STATS:' ~/175.txt`。
- **下一步/未完成**:保留当前可显示5G的single+NR基线,先反编译iOS WDS初始化到Start的决定分支(175实际仍拒绝0xcd/0x3e/0x85,不能凭unsupported先伪ACK),分别确认LTE/NR真实ACT→GET→EIF/IFUP→route→HTTP/DNS及硬件TX退休/RX。安全ccmni复用、IPv6/profile/auth仍缺;ALS要实现真实SCP生产者与光照变化/CoreBrightness消费后才可验收。不给用户连续安排探针镜像;本轮只交付其要求的独立整合候选。

### 前次交接(HANDOFF 113-DATA,2026-11-10): 原生蜂窝数据面/流量统计实现与首次候选

- **本轮用户目标**:完成4G/5G蜂窝数据的内核和iOS侧数据链路。没有flash/reboot/设备分区写入/ADB操作/插件重建/测试套件。不要将以下代码完成或编译通过等同于实机联网成功。
- **本轮代码提交**:**`be06cf1`**(main),仅四个蜂窝实现文件与本交接;未夹带其他会话改动。没有主动push/CI run。`kern_exec.c`能力门修正来自已提交的1ce5010,不是本轮新实现。
- **实际阻塞证据**:`~/174.txt`14.160s仍是`DMA alloc of 16384 bytes returned no descriptor`,DPMAIF在prepare隔离;20.557s MIPC握手完成;25.937s真实SA注册,45.946s真实LTE注册;仍没有WDS Start。另一个确定源码阻塞:`coachz_bb_pdp.cpp`仍要求`bb_data.step==CBD_DONE`和IPA/QMAP格式,而`COACHZ_NATIVE_DPMAIF`早已禁止该初始化,所以即使原生拨号成功PDP网络栈也永远关闭。
- **DMA实现**:`X9DPMAIF.inc`使用host-contiguous、无IOMMU的40-bit IOBuffer,保留32KiB对齐及segment/范围验证。依据MT6993厂商`ccci_dpmaif_com.c`的`DMA_BIT_MASK(40)`、普通`dma_map_single/page`和`ccci_dpmaif_drv_{v3,com}`地址写入:DRB高位在UL SRAM+0x18,BAT高位在DL+0x50;**LRO PIT高位在CON1[31:24],不是非LRO的CON4**。DT alloc-ranges只约束可选reserved-memory分配器,不是所有DMA的4GiB硬限制。不扩XNU低端RAM、不占用/改写LK或secure carve-out;硬件DMA可达性仍待证实。
- **iOS侧PDP实现**:`coachz_bb_pdp.cpp`原生模式以`cbp_observe`的成功会话状态∩`coachz_modem_data_state`的真实MIPC/DPMAIF route为gate,不再等IPA。raw-IP默认MTU1500,使用MD协商MTU(有界3072),保留非X9的1492/QMAP路径。正确处理EBUSY背压;block或route epoch变更清掉旧mbuf队列,避免重连跨会话发送。`ifnet`成功收发计数供iOS原生网络统计使用,未硬编码地址/DNS/路由或UI联网状态。
- **WDS实现**:`X9WDS.inc`支持0x24 PacketStatistics、0x86计数重置(仅client baseline)、0x01按请求间隔的流量事件;u32包数/u64字节数LE,TX只在payload DRB硬件退休后成功计数,RX在校验真实IP/route后计数;会话按mux/epoch隔离,断开后0x24返回OUT_OF_CALL和last-call总量。0x44 CurrentBearer使用新鲜真实NAS快照的LTE bit5/NR bit10;旧0x37没有NR枚举时返回UNKNOWN,不伪装LTE。0x30 Dormancy只报枚举UNKNOWN,不从CGATT伪造RRC ACTIVE。允许空/省略APN请求网络缺省以及automatic/3GPP technology preference;仍仅IPv4/no-auth,不猜IPv6/SLAAC、profile/IMS或运营商APN。连接指示只在ACT+GET+EIF/IFUP+DPMAIF bind全通过后发送。
- **构建里程碑**:首次本地53/53 overlay编译、内核链接/strip通过;`im`打包误把产物`boot.json`当构建配置,缺`boot_args`而退出,**im没有boot.img**。主checkout其他会话同时修改光感/手电筒/电池/构建器等,本轮不提交/覆盖它们。独立worktree`.local/x9/worktrees/cellular-data`基于3865453并挑入已提交的能力门修正1ce5010(该worktree提交48b2049,源码相同),只复制本轮四个蜂窝文件;**`x9-cellular-data1-md`构建exit0**,53/53 overlay+6prebuilt、链接/strip/打包通过,构建器报告boot.img **13017088 B**。参数为完整hi加`x9_dpmaif=2 x9_mipc=1 x9_md_lte=0`,32GiB manifest;原始产物在该worktree的`x9/out/x9-cellular-data1-md/`,已原样复制到主checkout **`x9/out/x9-cellular-data1-md/`**。不含其他会话未提交代码;未运行测试套件,成功后未解包/重算hash/反汇编。插件275不变,无需mailbox。
- **原始证据/构建输入**(不入git):`.local/x9/work/cellular-data-20261110/{scope.md,qmi-enums-wds.h,ios-data-state.txt,boot-config.json,build-im.stdout.log,build-data1.stdout.log,worktree-integration.log}`。现有UniXNU-Dali参考没有可直接移植的MT6993/WDS驱动。没有CI run。
- **重建/日志复核**:在仅含本轮蜂窝代码与1ce5010能力门修正的源码树运行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --name <新的唯一名字> --boot-config /Users/tosasitill/ios_plj110/.local/x9/work/cellular-data-20261110/boot-config.json`。如后续确有新设备日志,用`rg -n 'X9 DPMAIF:|X9 WDS:|X9 MIPC:|COACHZ pdp:.*(active families|tx |rx )' ~/NNN.txt`先确认DMA高位/INIT_DONE→Start→ACT/GET/EIF→真实tx/rx;不能以`initialized`或Start成功单独验收。暂不要求用户启动另一试探镜像。
- **未完成/验收边界**:①真实DPMAIF硬件初始化与DMA可达性;②iOS发出WDS Start及APN/protocol策略(174没有Start,本轮不以ACK或图标假造突破);③真正ACT→GET→EIF/IFUP→绑定→iOS HTTP/DNS收发,分别LTE和NR;④原代码仍禁止复用旧ccmni channel,需真实DEACT/IFDOWN和DMA排空证据后设计安全复用;⑤IPv6/profile/auth尚未实现。遵守HANDOFF112的“先反编译,不连续要求刷试探镜像”;当前编译不构成完成声明。

### 并行功能交接(TORCH-2,2026-11-10): 手电筒初步开发/20H392 iOS对接已编译打包,两版均未上机

- **范围/观察**:查过UniXNU `ae68700`/`f0721e9`(未发现torch后端)、hackios `c84b463`、OPPO modules `4d505a42`。本机捕获DT为MT6379 flash/SID14/PMIF-P/三路,不是hackios的PM8350C。20H392 CMCapture UUID=`45d37488-2cc7-3028-8a4d-936bce7a220b`;实跑cache抽取符号和cache指令确认FigFlashlight→BWFigCaptureDeviceVendor→TorchLevel,含写失败返回分支。私有原始定位/编译日志`.local/x9/work/torch-20261110/`。
- **实现**:`sweet_x9_torch.c`+ABI,从原生UFS初始化入口启动;`x9_torch=0`默认关闭,`=1`只读,`=2`25mA/2s一次脉冲,`=3`iOS受限writer(启动不点灯)。仅写ITORCH选中路及FLED_EN,复用PMIF锁;默认单路100mA/15s,硬上限100mA/30s;新鲜电池温度门、100ms故障/热/使能监控、超时必须先0、失败锁存、关灯读回失败不伪报dark。
- **iOS集成**:`x9/torch/media_bridge.m`按20H392 UUID/IMP/7个准确调用返回点适配hackios的FigFlashlight专用vendor,保留真实vendor与非flashlight调用;写错误传回FigFlashlight。仅mode3发布`/product/camera/flash=1`并在PID1安装自有内容寻址dylib,每次mediaserverd exec核对内容/权限后附加DYLD环境;不改Apple程序/plist/分区、不覆盖不同文件、不自动清MG缓存。工具`torchctl`只编译,不自动部署/运行。技术使用文档`x9/torch/README.md`。
- **提交/编译里程碑**:初版`5e06897`,只读继承点灯快照/unknown read修正`a4c8857`;自有iOS16 arm64桥与工具编译/adhoc签名通过,桥72464B。独立源码快照`.local/x9/work/torch-20261110/source/`固定`5e06897`已提交树加`a4c8857`两份驱动/ABI改动,不含其他会话未提交蓝牙/短信代码;继承已提交的蜂窝/ALS源码(ALS默认关,本镜像没有启用主动DPMAIF)。55/55 overlay+6prebuilt、内核link/strip通过。首次torch1 boot因快照遗漏根目录`tools/`而失败,补齐同一提交的`tools/`/`shim/`后只续打包exit0;torch1已被torch2替代,没有覆盖已成功镜像。没有测试套件、CI、ADB/刷机/重启/分区写入,成功后没有解包/重算hash/反汇编镜像。
- **最新候选**:`x9/out/x9-torch2-probe/boot.img`(`x9_torch=1`,只读,不点灯/不注入/不发布flash)与`x9/out/x9-torch2-ios/boot.img`(`x9_torch=3`,启动不点灯,可由iOS请求点灯),两次打包均exit0,构建器均报告**13037568 B**。mode3复用probe目录的`kernel.development.sweet`与`baseband-firmware.json`,不重编插件;工具位于`x9/out/x9-torch2-probe/torch/torchctl`。参数以完整hi配置为基,单卡、`x9_md_lte=0`,额外torch令牌;32GiB manifest不变。私有构建配置`boot-base.json`,原始日志`build-probe2.stdout.log`/`build-ios2.stdout.log`(均在上述torch工作目录)。该目录保留了首次单文件编译及torch1失败/续打包日志,不得拿它们当torch2的运行日志。
- **精确重建**:在上述固定源码快照运行`/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --name <新的唯一名字> --boot-config /Users/tosasitill/ios_plj110/.local/x9/work/torch-20261110/boot-base.json --boot-arg x9_torch=1`。mode3用`build.py boot --kernel <该次kernel.development.sweet> --name <另一个唯一名字> --boot-config <同配置> --boot-arg x9_torch=3`。按现有约定只由用户手动fastboot boot;没有自动启动新候选。取得新日志后`rg -n 'X9 TORCH:|X9 TORCH MEDIA:' ~/NNN.txt`对齐驱动ready→bridge install/env→hook/vendor→requested/confirmed/uA/enable,最终仍需实体灯和熄灭观察。
- **未验收/约束**:尚无实体点灯、通道选择、关灯、控制中心RPC或新镜像启动结果。panic/停调度时可靠熄灭未实现;watchdog自行熄灭还未主动同步UI通知;MG缓存可能使按钮仍不出现。下一步先mode1读回(不点灯),再确认是否进行mode2/3实机验证;不连续要求用户刷探针镜像。插件275未改,无需mailbox。

### 短信/IMS 并行交接(HANDOFF 114-SMS,2026-11-10): 原生 GSM 收发/ACK 与 IMS 注册适配;尚未上机

- **分工**:用户明确“另外的窗口也在看dpmaif,你看后续”。本窗口不修改 DPMAIF/WDS/PDP,仅增加 `X9Messages.inc`、`x9_mipc_messages.inc`,在 `X9QMIAT.inc`/`X9Baseband.cpp` 做服务及 poll/del 接线,扩充 MIPC 消息专用 API。Apple 用户态未修改,由内核 QMI 适配供原始 iOS 消费;不能称完整 IMS/VoLTE 已完成。
- **Evidence → implementation**:174 只有真实 OPEN/UIM/NAS 与 DPMAIF 隔离,没有短信运行证据。原厂实际加载的 `libmtkmipc-ril.so` 证明 GSM `0605` 输入 `100/type=0 + 8101/完整SMSC+TPDU + 102/TPDU长度`,成功 `0606/103` 为 message reference;接收 `8601/type=3/8104` 内头8B后为完整PDU,状态报告 `8603/8100`;`libmipc::mipc_msg_rsp` 保持原始 id+1/txid/slot,不是普通 submit 的新事务。由此实现独立消息 inbox、原 txid ACK 和唯一 MD writer 的物理发送结果回收。所有 SMS/SMSC 只在有界 RAM,不打印 PDU/号码/SIM 身份。
- **iOS 合同纠正**:20H392 `QMISMSCommandDriver` 实际使用 `47` RegisterIndications(13=ServiceReady)、`4c` BindSubscription(01/u8,0/1)、`5c` ReadyStatus(11/u8)和 `5d` ready IND(10/u8)。`qmi::wms::asString(WMSReadyStatus)` 表直接证明 **1=3GPP,2=3GPP2**,不沿用其他协议版本枚举。适配 `20` RawSend、`37` SendAck、`34` GetSMSC、`30` MessageProtocol、`01` EventReport、`32` transfer/discard Routes。只在真实 `030b` CMD 注册/`0307` IND 注册/`0601` host-ready 均确认后报告 GSM ready;它不是网络/IMS注册或配送证明。
- **所有权/失败边界**:MD command 使用不复用本地 token 作为 WMS ACK transaction;同一接收只交一个 client,校验 SIM/port generation。发短信与 ACK 在 timeout/DEL_CLIENT 后保留 token,不重发,消费 late CNF/TX result;每 SIM 只允许一个未完成 send。transfer-only 等 host ACK,显式 discard 才自动 ACK;不支持 NV/SIM 存储路由。消息 overflow 隔离消息队列,不混进 WDS 的 4202/4302 inbox。未知/畸形/IMS8105 command 保留且不成功 ACK,满队列后需重启恢复。
- **IMS 可用部分**:IMSA(21) `20` 真实 `0a04/0a05` 注册查询、`22/23` 注册订阅/指示,并按需每15s查询。query `101` 是 CIREG 0..4(2=registered,3/4=no registration,1含义未确认则失败),IND `4a01/101` 是 **CIREGU 0/1**,分别映射;不把两个枚举混用或凭数值相似生成 REGISTERING。`102/extInfo` 服务位尚未解码,不伪造 SMS/voice/video capability;GetServices 返回 unsupported。IMS-only RawSend、IMS8105 编码/ACK、IMS settings/EIMSCFG 六字段、SIP/鉴权/VoLTE 没有被证明可用,**仍未实现**。LinkTimer 0/1 只当复用提示,保留 MTK 原生链路寿命;不实现其精确计时。
- **分析/编译输入**(私有):`.local/x9/work/sms-ims-20261110/` 的 `mtk-*.asm`、`ios-*.asm`、`qmi-service-*.json` 与枚举、只读 ELF/Mach-O 解析器;`compile.stdout.log` 包含两编译单元 `sweet_x9_gpu_dependencies.o` / `X9Baseband.cpp.o` exit0(随后继续作源码边界修正,最终构建另记录)。无额外测试套件/测试脚本,无设备启动/flash/分区写入,未主动 push/CI。
- **提交/最终构建**:实现 **`6eb119f`**,含糊 send CNF 锁存补丁 **`fa83fbf`**(仅本窗口文件)。若 send CNF 缺少/破坏 result,或成功 CNF 缺少真实 message reference,不释放该 SIM 的发送门,擦掉 host PDU,需重启恢复;避免“返回错误后再次发送”造成重复短信。独立 detached worktree `.local/x9/worktrees/sms-ims` 构建已提交树 `fa83fbf`;它包含另一窗口已提交的 **`15f597b`** PIT 状态修正,不包含其他窗口未提交文件。`x9-sms-ims1-md`(6eb119f)与最终 **`x9-sms-ims2-md`(fa83fbf)** 均 build exit0,57/57 overlay+6prebuilt、链接/strip/shim/打包成功;构建器报告最终 boot.img **13066240 B**,已原样复制至主工作区 **`x9/out/x9-sms-ims2-md/boot.img`**。旧1不含15f597b/锁存修正,不要用它替代最终2。插件275不变、32GiB manifest,**无需 mailbox**;未上机,未在成功后解包/hash/反汇编二次验证。
- **重建命令/日志**:在上述 worktree 的已提交 `fa83fbf` 树运行 `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 -I x9/build/build.py all --name <新的唯一名字> --config /Users/tosasitill/ios_plj110/.local/x9/work/sms-ims-20261110/local-config.json --boot-config /Users/tosasitill/ios_plj110/.local/x9/work/sms-ims-20261110/boot-config.json`。私有 `build2.stdout.log` 为最终原始构建记录,`build.stdout.log` 为旧1;`local-config.json/boot-config.json` 是固定的完整数据候选启动配置,未重建插件。未来日志用 `rg -n 'X9 SMS:|X9 IMS:|X9 SMS/IMS:|WMS\(0x5\)|IMSA\(0x21\)' ~/NNN.txt`;ACK 日志明确记录 local token、MD reply id、原始 wire txn 与物理 transport result,不包含 PDU/号码。没有主动 push/CI run。
- **下一步/验收**:独立工作树直接构建最终候选,不夹带其他窗口未提交修改。未来用户提供实际启动日志后,先看 `X9 SMS:` 初始化真实 CNF;再由用户明确发一条短信,核对 modem acceptance 与接收端真实收到;反向接收要看到同一个 command token 的 host ACK→原 txid 物理 TX,状态报告须真实 PDU。IMS 必须得到真实注册与服务能力并证明 iOS 消费/SIP 路径,不能仅用服务公告、编译成功或注册查询代替验收。

### 蓝牙并行交接(BT-1,2026-11-10): MT6653 UART/H4 bootstrap和自有iOS诊断ABI已编译打包;未验证原始bluetoothd或射频

- **代码提交**:`c9b3e46`(main),12个蓝牙实现/构建/使用文档文件,未夹带其他会话未提交修改。完成的是首阶段诊断链路,不是完整蓝牙功能。没有push/CI、测试套件、ADB/flash/reboot/分区写入或mailbox更新。
- **参考/观察**:本地UniXNU `ae68700`、hackios `c84b463`未提供可直接移植的本板BT后端;借用IOResources注册模式。OPPO linux_v2 `uart/btmtktty.c`、`btmtk_main.{c,h}`及MT6993 DTS确认UART3 `0x16030000`→UARTHUB→GPIO241/242,BT_RST GPIO249。历史Android日志`.local/x9/android-pull-20261106-132120/dmesg.txt`14.102–14.119s确认真实查询/chip-id、115200→24M和dx5_2g2a固件;**不是本轮XNU握手验证**。20H392 bluetoothd字符串含AppleBluetoothModule/AppleBT及cu.bluetooth/uart.bluetooth,不足以证明传输选择/selector ABI;没有改Apple程序或发布串口别名。
- **实现**:`x9_bluetooth.c/.h`提供有界H4分片/连续帧、ACL/SCO/event/ISO长度、超长包按声明边界丢弃、半帧超时、8槽TX/RX队列和真实计数。`SweetX9Bluetooth.cpp`注册`X9BluetoothTransport`,自有同步inband ABI v1(selectors 0状态/1节表/2发包入队/3收包),H4仅管理员独占,未知selector/descriptor/async拒绝;启动查询回复不让外部客户端抢读。**不是AppleBT shared-memory/pipe ABI**;入队成功不是硬件命令成功。
- **开关/硬件边界**:`x9_bt=1`只读AO GPIO/时钟门,默认关闭。`x9_bt_probe=1`另启用两条WMT ROM查询,仅接管已上电/已开时钟/IRQ-DMA-FIFO空闲、MD/ADSP hub未使用的AP/CMM端口;不开时钟、不写共享PMIC/POR/WF_RST/reset,门未开不读off-domain。失败恢复改过的UART配置/GPIO/bypass位,无assert/重试循环(已消耗FIFO字节不可撤销)。`x9_bt_raw=1`还需真实ROM_READY才接受H4;`x9_bt_discovery=1`只是native name-match观察。完整hub/SSPM上电、固件/FEM/校准/ownership未实现,**firmware_ready始终0**。
- **固件/用户态**:`bluetooth_firmware.py`按BT小端v2容器校验节表/范围/重叠,本机dx5_2g2a固件774585B/11节;仅生成metadata,不嵌入/下载字节,缺输入显式stub。`x9/bluetooth/btctl.c`经自有IOKit ABI查询/诊断;`.local/x9/bluetooth/client2/btctl`已iOS16 arm64编译/adhoc签名,未安装/运行。首次内核C编译生成头include缺路径、首次客户端kIOMasterPortDefault被iOS SDK禁用,均已修正;最终两个新增对象及客户端均编译通过。
- **构建结果/隔离**:独立worktree`.local/x9/worktrees/bluetooth`基于`3865453`(il的32GiB shim修复)+仅蓝牙改动,未合入后来的蜂窝/ALS/torch/SMS。`build.py --name x9-bluetooth1 --boot-config /Users/tosasitill/ios_plj110/.local/x9/bluetooth/boot-config.json` exit0,**55/55 overlay+6prebuilt、link/strip/boot打包通过**,构建器报告boot.img **13021184B**。完整il参数从旧payload恢复,只新增`x9_bt=1`,无probe/raw/discovery/rootgrow;用当前32GiB manifest。插件275不变,无需mailbox。成功后未再解包/算hash/反汇编/测试。
- **产物/复建**:`x9/out/x9-bluetooth1/`是上述独立输出的symlink,含boot.img。原始编译/构建日志及输入在`.local/x9/bluetooth/`(`compile1/`, `compile2/`, `compile2.stdout.log`, `client1.stdout.log`, `client2.stdout.log`, `boot-config.json`, `build1.stdout.log`, `baseline3865453-integration.patch`),均不入git。保留该源码worktree;复建用新的唯一名字:`python3 -I .local/x9/worktrees/bluetooth/x9/build/build.py --name <新名字> --boot-config /Users/tosasitill/ios_plj110/.local/x9/bluetooth/boot-config.json`。自有工具构建/调用见`x9/bluetooth/README.md`。
- **下一步/验收**:先证明真实LK时钟/SSPM/UARTHUB前置并补齐初始化→WMT完整回复/chip6653→patch/ILM/connfem/校准/RAM code/HCI Reset→反编译20H392原生UART/AppleBT传输选择与ABI→原始bluetoothd扫描/配对。本轮无新设备日志,不声称ROM_READY或蓝牙可用,不要求连续启动试探镜像。未来若有新日志,先`rg -n 'X9 BT:' ~/NNN.txt`核对状态,不能把注册/编译当作握手成功。

### 光感并行交接(HANDOFF 113-ALS,2026-11-10): 原生 ALS HID / MTK 数据入口已编译;物理 SCP 尚未接通

- **范围/提交**:本轮用户要求光线传感器初步开发及对接;代码 **`9323bcc`**(main,7个ALS相关文件),没有主动push/CI run、设备访问/写入/flash/reboot、插件重建或测试套件。不覆盖本节蜂窝主线及其他会话的手电筒/蓝牙/电池修改。
- **参考与观察**:UniXNU本地`ae687009`未找到ALS实现;hackios-xnu-kernel `c84b4634`的`userland/src/inject.c:544`仅跳过`BLControl waitForALSArrival`,不是驱动。现存设备DT(`.local/x9/modem/live.dtb`)明确`light_1=tcs3720`,SCP I2C0/0x39/EINT24,不能当AP现有I2C驱动使用。OPPO `kernel_device_modules-6.12`的`sensor/2.0`采用LIGHT=5、DATA_ACTION=0、40B sensor_info(gain@4)、36B share_mem_data(timestamp@4,value[0]@12);IPI CTRL out/in=29/30、NOTIFY out/in=31/32,本轮**只对接数据ABI,未实现该IPI传输**。
- **用户态对接证据**:实跑20H392 CoreBrightness的`-[BLControl startHIDSystemClient]`在`0x1af0a53bc/0x1af0a53c0`匹配Sensor/AmbientLight `0x20/0x41`;`-[CBALSNodeiOS copyEvent]`在`0x1af054fbc`请求type12。新`X9AmbientLight`复用现有IOHIDEventServiceUserClient/esuc队列,支持dispatch、copyEvent和按EventType的copyMatchingEvent,没有修改Apple用户态或跳过等待。
- **实现**:`bsd/dev/sweet_x9_als.c`+`X9ALS.h`提供线程上下文的MTK sensor-info配置/单条DATA接收/断连失效接口;按gain换算整数lux,校验长度、类型/action、负数/越界、timestamp重复/回退,互斥锁保存最新样本。HID使用接收时的mach_absolute_time而非直接拿SCP ns;5s无更新即无有效事件,不把无数据伪装成0lux。默认不发布ALS;`x9_als=1`等待真实MTK入口,**当前没有SCP生产者,不会产生物理读数**;`x9_als=2`为显式诊断,与真实MTK入口互斥。没有ADC/RGB/CCT/True Tone或背光硬件实现。
- **诊断/接口**:`debug.x9_als_test_lux`写入口同时要求root及`x9_als=2`;0..200000通过同一MTK字节解码器和HID发布路径,标为DIAGNOSTIC;写-1失效,不循环生成固定读数。IORegistry提供`X9ALSDataValid/X9ALSDiagnostic/X9ALSLastLux/X9ALSSequence`;ReportInterval可在20ms..1s配置,这里只控制HID轮询,不是SCP采样/批处理。`X9ALSPhysicalTransportReady`保持false,不能按服务存在或diagnostic dispatch验收自动亮度。
- **编译/产物**:三个编译单元`sweet_x9_als.o/KernelConfigTables.cpo/SweetHID.cpo`交叉编译通过,内核链接/strip与shim/boot打包exit0。为避开其他会话未完成硬件实现,候选复用**il已构建的其余overlay对象**,仅重编上述三个对象;链接清单取9323bcc,不含未提交的torch/BT对象。**`x9/out/x9-als-phase1-il-9323bcc/boot.img` = 13017088 B**,是il硬件基线+ALS诊断,不是新蜂窝DATA候选;完整il启动参数仅追加`x9_als=2`(598B),继续用32GiB root/extension manifest。打包工具走主checkout,并行torch ADT分支依赖`x9_torch=3`,本候选不传该令牌。插件275不变,**无需mailbox**。未上机,成功后未再次解包/重算hash/反汇编镜像。
- **原始证据/可复放输入**(不入git):`.local/x9/work/als-bringup/`包含`live.dts`、`als-matching.dis`、`corebrightness-events.dis`、首次重载歧义失败及修正后的编译日志、最终`build.stdout.log`、`recipe-input.json/link-order-input.txt/builder-input.py/local-config-input.json/boot-input.json`和`build-candidate.py`。重建需给该脚本新的唯一out路径;它依赖保留的il对象目录,不是从当前混合工作区全量重编。本次没有额外测试脚本执行。
- **下一步/验收**:不要求用户连续启动试探镜像。具备iOS shell时可用root依次`sysctl -w debug.x9_als_test_lux=10`、`=1000`、`=-1`,检查`SWEET HID: X9AmbientLight open`及`X9 ALS: dispatch ... source=DIAGNOSTIC`、有效性超时,再确认CoreBrightness实际收到事件;这仍仅验HID。真实光感还需SCP启动/固件与内存所有权、sensor list gain、SHARE_MEM配置/CRC8+ACK、LIGHT enable/batch、通知驱动的FIFO消费和断连恢复;拿到实际遮光/照明变化的lux以及CoreBrightness消费证据后才算物理采样通过。自动亮度最终还依赖尚未实现的面板背光控制。

### 前次交接(HANDOFF 112,2026-11-10): 174(il)已启动;优先反编译——PS-only信号清零与数据图标启用条件已定位;能力门跟随路由修正,未打包

- **用户要求**:仍无5G、信号条空;明确要求“优先反编译而不是让我不停测试镜像”。本轮没有打包/要求启动新的试探镜像,没有ADB设备操作、flash、重启、根盘写入、mailbox staging、CI或测试套件。
- **174.txt观察**:Mac `~/174.txt`,1459304B,末57.996s。il越过shim并正常进入iOS;3.352s `X9 CCGATE: already patched (dual-sim + nr gates, page hash)`。约25.96s SA注册、26.022s NR `RSRP=-69 dBm, SNR=27.5 dB`,NAS0x4f成功;45.946s转为LTE(`CEREG=1,C5GREG=0,AcT=7`),45.958s LTE/NR均有真实测量,0x4f仍成功。**未观察到新的CommCenter/SpringBoard崩溃**;mediaserverd/AegirPoster等旧崩溃仍在。不能把这份不到一分钟的日志说成持续SA或数据收发验证。
- **反编译方法/限制**:本机IDA Professional 9.1 `idat`报无有效license,未修改安装/许可证;改用Ghidra12.1.4 headless。全量分析遇Objective-C类型`b`和大函数pcode问题,已停止本人启动的全量进程;最终使用`-noanalysis`+明确函数入口,CommCenter边界取真实`LC_FUNCTION_STARTS`,定点导出C,结合既有Capstone/cache读取器与chained-fixup解析核对指令。SpringBoard抽取件缺共享stub islands,部分C输出有missing-memory/PIC警告;关键分支已按原始cache里的实际指令和已解析objc selector复核,不能把不完整C的推测参数当ABI证据。
- **信号清零(地址级确定)**:CommCenter `0x1008e6958`先调用模型vtable+0x30取得base bars,显示格数=min(base,personality+0xd8)。D22原构造器离线执行得到`+0xd8=4,+0x166=1`。`+0x166`使结果再走语音附着策略;`0x1008ed1f4`(查询回调)与`0x1008ee910`(通知回调)均在**CS未附着、controller+0x140开启、slot的PS附着为真**时写`{bars=0,displayBars=0,max=4}`并打印`Data-only attached, reporting zero bars`;PS也未附着则给1格,CS附着则保留模型结果。对应查询入口`0x1008ec854`。这解释172里已注册slot1为0、未插卡slot2为1的另一种原因:不能继续把“slot2=1”本身当信号被分到错误槽的证据。
- **真实NR输入链(确定范围)**:QMI parseInputs把0x17存入`(Technology=4,metric=7)=RSRP`及`(4,6)=SNR/10`,0x18为metric8。`NewRadioSignalStrengthModel` vtable在`0x1016292a0`;`0x100fd8f70`接纳Technology4的metric7/6,保存到+0x228/+0x238并置+0x230/+0x240有效;`0x100fd87a0`需要两种有效输入再走滤波映射;`0x100fd8e54`返回+0x184的模型格数。**未取得174的运行时模型格数/CS-policy输入,不能声称已经证明RSRP -69在现场算出了4格**。不要改TLV为假值或把PS注册伪装成CS/IMS注册。
- **5G隐藏条件(确定)**:SpringBoard `SBStatusBarStateAggregator::_updateDataNetworkItem:withData:primary:...`=`0x1c5e0d408`,从SIMInfo读取`dataNetworkType`,已有1..14的类型映射(包括5G),但最终item启用条件还与`isProvidingDataConnection`相与(0x1c5e0d5b8、0x1c5e0d62c)。172 logd明确该值NO,即使ST状态已有5GConnection。因此**注册成功/CT indicator=5G不等于该UI项可见**;应继续追数据连接状态及真实WDS/PDP,不是硬编码绘制5G。ST `0x1c55c2bf4`直接把CTSignalStrengthInfo.displayBars存到subscriptionInfo,并无重新按RSRP算格数。
- **已完成的源代码修正(非蜂窝修好)**:`kern_exec.c sweet_x9_ccgate`现在`x9_ccgate=1`按`x9_dual_sim==1`选择dual+NR,否则选择**single+NR**;`=2`仍恢复全部原始,`=0`仍关闭。接受original、NR-only、dual+NR三个完整状态,混合/未知拒绝,只写变化项,代码先写/页hash最后写,fsync+回读。NR-only的page3515 SHA256=`781e43dd2d2a057b3d852f26fb562071d2da3393d1e5da0c00c9b969319d7347`,从已核对原页仅应用8B NR补丁计算。**174确实仍加载dual+NR**,此前ik/il只关桥,单卡对照不完整;本修正避免下一次再发生,但未上机,不能保证其单独解决SIM NotReady。
- **编译**:只重放il的`kern_exec.c`交叉编译命令(替换工作区路径和私有输出,实际执行clang,exit0),对象`.local/x9/work/signal-174/kern_exec.o`;无新boot/内核链接。插件仍275,无需mailbox。其他会话正在修改`coachz_bb_pdp.cpp`、`X9DPMAIF.inc`、`X9QMIAT.inc`、`X9WDS.inc`,本轮未触碰/提交这些文件,**不能把它们混进本轮验证结论**。
- **证据/复放**(全部私有不入git):`.local/x9/work/signal-174/`下`quick/cc-*.c`、`sb/cc-*.c`,对应`cc-{base,request,compute,notify}-bars.asm`、`nr-model-*.asm`、`sb-{signal-bars,data-network}.asm`、`st-*.asm`;`d22-signal-policy.txt`为带PC链的离线构造器结果;`nr-only-page.py/.json`为原页hash核对/8B派生;`compile-command.json`与`compile-kern-exec.log`为单对象命令/日志。Ghidra本机数据库在`/tmp/x9-cc174-ghidra/{CC174Quick,SB174}.gpr/.rep`,不是设备盘;失败的全量CC174库不作为结论依据。
- **下一步(继续反编译,不先让用户刷)**:①追`controller+0x140`的写入与PS/CS回调,判断能否在D22的**数据专用显示策略**中保留真实LTE/NR模型格数而不改变注册/语音状态;不要全局改所有persona或无条件返回4格。②从ST `isProvidingDataConnection`及CC internetDataStatus反追数据开关/默认数据SIM/APN/WDS启动的第一道否决,与另一会话的PDP修改协调,不能凭WDS局部ACK声称上网。③SIM NotReady仍需完整单卡策略下的状态链证据;当前ADB无连接,174只有USB日志,172 logd不能当174的新状态。确需现场数据时优先**一次只读拉最新logd**,不是连续做试探镜像。

### 前次交接(HANDOFF 111,2026-11-10): 173(ik)停在shim——shim根盘表仍是16GiB;il修复

- **173.txt**(Mac `~/173.txt`,151行,只有shim日志)。`UFS READ-ONLY GPT VERIFIED`后`UFS native header status=0xfffffffffffffff8`(X9U_GEOMETRY)→`loader: native UFS root not verified/quiescent`,内核未启动。**设备未受影响**(shim只读)。
- **根因(本会话的遗漏)**:shim的根盘常量来自`ufs_root_manifest.header()`→`shim/ufs_root_config.h`,HANDOFF106只改了ADT(`add_native_adt`)没改`header()`,所以ij/ik的shim仍是`X9_ROOT_BYTES 17179869184`、709个extent,并要求HFS大小==16GiB。ij能启动是因为当时HFS还是16GiB(扩容在ij内核里才发生);扩容后所有这类镜像都会在shim失败。
- **il修复**(提交f5c3f38):`header()`改用`device()`(root‖extension,32GiB,4839 extent);shim的`native_root_verify`与内核一致:可写根盘允许HFS≤设备,只读根盘须相等。`x9-rev252il-md` = ik + 此修复(仍无`x9_dual_sim`,有`x9_md_lte=0`、ccgate、NR信号),`shim/ufs_root_config.h`已核对为34359738368/4839;boot.img 13008896B;产物`~/ios_plj110/x9/out/x9-rev252il-md/`,日志`.local/x9/work/pdp-activation/build-il.stdout.log`。
- **注意**:ij/ik/ii/ih/ig等在HANDOFF111之前构建的镜像现在都会停在shim,不要再用。
- 下一步由用户手动:`fastboot boot x9/out/x9-rev252il-md/boot.img`→`~/174.txt`(目的同HANDOFF110的ik:SIM保持Ready时5G图标/信号格)。

### 前次交接(HANDOFF 110,2026-11-10): 172 logd——iOS已认出5G SA(indicator=5G/NR),但SIM1被打回NotReady且bars=0;Android两卡均在;ik关双卡路由做对照

- **Android(观察,adb)**:`gsm.sim.state=LOADED,LOADED`,`persist.radio.multisim.config=dsds`,`gsm.operator.numeric=46015,46000`——**卡槽2有实体卡**。172里MD对SIM2回`+CME ERROR: 10`,所以是我们的MD bring-up没让SIM2上线(热插拔/上电/多卡模式),不是用户态问题。未查。
- **logd**:扩容后首次用新manifest拉取(`pull_ios_logs.py --paths private/var/db/diagnostics --recent-hours 2`,26文件51.8MiB,3分41秒,`.local/x9/logs/172-logd/`),说明root‖extension读取可用。`tracev3_strings.py`解出`strings.txt`(Persist 19b–19d + Special)。
- **SpringBoard `STTelephonyStateProvider`(观察,19c=172)**:slot1 NotReady→**Ready**→(约29s,`QMIClientPool: WDS::proxy`之后)**NotReady**;之后`registered`、`dataConnectionType=5GConnection`、bars 0、operatorName ""。slot2 NotInserted(能力门生效,出现第二槽),但bars常为1(信号被记到错误槽,推断)。19b(单卡构建)slot1一直Ready/registered/lte/bars0。CommCenter侧`kSimStateReady`→`kRatNR`/`kRegisteredHome`→`kSimStateDetected`。桥这边UIM 0x32指示每10s一次、内容不变(66B,SIM1 READY)。**推断**:NotReady由双卡分槽路由引起(单卡构建不出现)。
- **CoreTelephony(观察)**:`CTDataStatus attached=1, indicator=5G (8), radioTechnology=NR (10), reg.dataMode=NR (17)`——**5G SA已被iOS接受**。`CTSignalStrengthInfo bars=0, displayBars=0, maxDisplayBars=4`。CommCenter把0x17/0x18存入测量表(rat 4, metric 7=rsrp/6=snr÷10/8=rsrq,函数0x1f1ae9d3c起),bars映射在后续代码,未找到;ECSQ给的是rsrp −69 dBm、snr 24 dB。
- **x9-rev252ik-md**:= ij去掉`x9_dual_sim=1`(保留`x9_md_lte=0`、ccgate、NR信号、rootgrow默认开),Mac worktree基于d58d239,boot.img 12972032B,产物`~/ios_plj110/x9/out/x9-rev252ik-md/`,日志`.local/x9/work/pdp-activation/build-ik.stdout.log`。目的:SIM保持Ready时5G图标/信号格是否出现。
- 下一步:用户`fastboot boot x9/out/x9-rev252ik-md/boot.img`→`~/173.txt`;看完后若需要可再进Android拉logd(同上命令,把172换173)。并行待办:①MTK SIM2上线;②CommCenter bars映射(从0x1f1ae9d3c写入的测量表往下追)。

### 前次交接(HANDOFF 109,2026-11-10): 172(ij)根盘扩容与能力门安装成功;SA注册+NR信号已送达,但界面无信号/无5G;SIM2被MD报"未插卡"

- **172.txt**(Mac `~/172.txt`,1917688B,末112.8s,无panic)。用户:信号空、无5G、单卡。
- **根盘(观察)**:1.27s `HFS 16384 MiB on a 32768 MiB device`;3.27s `hfs_extendfs: extended "X9iOS16Full" to 8388608 blocks`;`X9 ROOTGROW: 16384 -> 32768 MiB errno=0`。**扩容完成,旧16GiB manifest的镜像从此无法挂载。**
- **能力门(观察)**:3.32s `X9 CCGATE: patched ... readback errno=0`;CommCenter全程一个pid(90),无CRASH、无cs_invalid_page——页哈希方案可用。
- **SA(观察)**:28.07s `NAS SIM1 registration ... C5GREG 1 CGATT 1 COPS-AcT 11`,`NAS SysInfo NR SA indication tlv 0x4d`;0x4e指示8次。
- **NR信号(观察)**:ECSQ只有一行NR:`+ECSQ: 65,88,-232,1,1,-44,-276,32768,96,41,32767,32767,32767`——SS组(10–12)为0x7fff,第二组有效→0x17 rsrp −69dBm snr 24.0dB,0x18 rsrq −11dB;0x4f `result 0`共10次,0x51指示6次。CommCenter收到了,但状态栏无信号格、无5G。0x5556=`nas::GetCellInfo`(QMICellMonitorCommandDriver),0x555b=Desense,0x5569=SmartDataMode/切数据卡——都与信号格无关。**原因需CommCenter os_log**,未查明。
- **SIM2(观察+推断)**:`X9 DUALSIM: UIM SIM2 present 0 CPIN class 0 ... via MIPC mask 2`。代码中present 0只在`+CME ERROR: 10`(未插卡)时出现,MIPC回包32B也符合→**MD认为卡槽2无卡**。BSP DsdsAllowed 0。需用户确认是否插了两张实体卡、Android下是否两卡都在;若是,下一步查MTK的SIM2上电/热插拔(169已见RPC `GPIO_SIM2_*`)。
- **工具**:`pull_ios_logs.py`改为按root‖extension读(扩容后新写入可能落在扩展文件),未实测。
- 下一步:用户进Android接ADB → 按memory流程拉logd(`pull_ios_logs.py --manifest <新manifest> --recent-hours 2`),grep CommCenter的signal/bars/RSRP/NR/5G/SA相关字符串;同时用户确认双卡硬件情况。

### 前次交接(HANDOFF 108,2026-11-10): ij = ih + 内核首启安装CommCenter双卡/NR能力门(含页哈希) + 5G信号

- **能力门(用户要求"放内核里第一次启动就修复")**:`kern_exec.c sweet_x9_ccgate()`,PID1在siri_off之前运行,默认开(`x9_ccgate=0`关,`=2`恢复原字节)。目标唯一文件`/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.31`(103677952B,未压缩才动)。改三处:双卡门28B@57593752(VM 0x1F036CF98)、NR门8B@57594288(VM 0x1F036D1B0)——字节与`prepare_dual_sim_cache.py`/`prepare_nr_registration_cache.py`完全相同;以及CodeDirectory第3515页SHA-256槽32B@103577665。离线分析(Mac根盘快照,scratch `ccgate_plan.py`):该文件只有一个CD(SHA-256,16KiB页),两处都在页3515,**存储哈希=原页哈希**已核对;新哈希离线预计算,内核不做哈希。旧哈希覆盖整页,所以匹配即证明整页原样;全部原样才写、全部已改则no-op、混合则拒绝;写后fsync并回读。CDHash随之改变:本系统没有trust cache/AMFI校验(缓存一直以"not in trust cache"警告方式映射),预期不影响;**未上机验证**。日志`X9 CCGATE: ...`。
- **5G信号**:`x9q_siginfo`逐行解析+ECSQ。LTE行→0x14(不变);NR行(Act 0x8000)→**0x17 {i16 rsrp dBm, i16 snr×10}**+**0x18 {i16 rsrq}**。格式与TLV号取自CommCenter:`NrSigInfo`解析器0x1f1952660、`NrSigInfoExt` 0x1f19526cc,0x4f响应0x1f1ae9b40与0x51指示0x1f1aea150都用0x14/0x17/0x18。MTK NR字段(libmtk-ril updateSignalStrength 0x8000分支):11/10/12=ss_rsrp/ss_rsrq/ss_sinr,6/5/8=第二组,均为1/4 dB,0x7fff无效;优先SS组,未测(全0)视为无效。日志`X9 QMI AT: ECSQ lte .. nr ..`(前8次,含原始回复)。
- **x9-rev252ij-md**:独立worktree基于origin/main 405f7c5构建,参数同ih(603B,`x9_dual_sim=1 x9_md_lte=0`,rootgrow/ccgate默认开),ADT 32GiB,boot.img 12972032B,warning 1417;产物`~/ios_plj110/x9/out/x9-rev252ij-md/`,日志`.local/x9/work/pdp-activation/build-ij.stdout.log`。ii(只含ccgate)也已构建,被ij取代。插件不变,无需mailbox staging。
- 下一步由用户手动:`fastboot boot x9/out/x9-rev252ij-md/boot.img`,`~/172.txt`(≥3分钟,两卡在机,几次底边上滑)。看`X9 ROOTGROW`、`X9 CCGATE: patched ... readback errno=0`、有无`cs_invalid_page`/CommCenter被杀、`X9 DUALSIM`、`NAS SysInfo NR SA`、`ECSQ lte/nr`、`X9 EXIT`。若CommCenter因能力门异常:下一镜像去掉`x9_md_lte=0`换成`x9_ccgate=2`即可恢复原字节(参数上限607B)。

### 前次交接(HANDOFF 107,2026-11-10): 三线合并为ih——main(ig根盘扩容/if/ie)+双卡uim3+NR SA nr1;双卡与SA默认开

- **背景**:用户说已合并并push双卡/5gsa;实际核对时两者只在Mac本地分支(`work/dual-sim` 71ce384/58a823b,`x9-nrsa` dde26eb/e8bb41f),各自单独成像(`x9-dualsim-uim3`、`x9-rev252nr1-md`,都用旧16GiB manifest),均未push、未互相合并;Mac main另有未push文档提交5d53837。用户选择"2"(合成一个镜像、双卡与SA都默认开)。确认两个codex CLI会话已退出、两分支均已提交后才合并。
- **合并**(独立worktree `~/ios_plj110-wt-if`,分支`merge-dualsim-nrsa`,已push为main,`work/dual-sim`与`x9-nrsa`也已push):origin/main 0eae168 ← work/dual-sim(无冲突)← x9-nrsa(冲突仅NOTES与`x9_modem.inc`的build字符串)← Mac main 5d53837(仅NOTES)。`X9QMIAT.inc`自动合并后人工核对:NR SA SysInfo(0x4b响应/0x4d指示,需C5GREG注册且COPS AcT=11)与LTE 0x19、ECSQ两行解析、双卡路由互不覆盖。
- **boot-args 607B上限**:ig已591B,加`x9_dual_sim=1 x9_md_lte=0`超限。改为**`x9_rootgrow`默认开**(设备不大于HFS时no-op;`x9_rootgrow=0`关闭),提交2407e0b;ih参数603B。
- **x9-rev252ih-md**:构建exit0,boot.img 12967936B,warning 1417(+5来自双卡/NR代码),ADT根盘32GiB,参数含`x9_dual_sim=1 x9_md_lte=0`;产物`~/ios_plj110/x9/out/x9-rev252ih-md/`,日志`.local/x9/work/pdp-activation/build-ih.stdout.log`。插件不变,无需mailbox staging。
- **已知限制(未做)**:(1)双卡UI与NR在CommCenter侧各有D22能力门(`GsmRadioPersonality+0x241` / `+0x121`),两个分支都只**离线准备**了dyld cache补丁副本,**未安装**——ih只提供内核/桥的双卡路由与NR SysInfo,设置里能否出现双卡、5G图标取决于这些门,预计仍不显示。(2)信号格:`x9q_siginfo`只取ECSQ的LTE行;驻留SA时NR行(Act 0x8000)不报→可能无信号格。(3)`x9_md_lte=0`关掉LTE-only后MD可能先注册SA(167: 24s SA,54s才LTE),168/171那种稳定LTE注册不保证;回退=去掉该参数重新打包。
- **根盘**:ih首次启动会把HFS扩到32GiB(HANDOFF106)。之后uim3/nr1/ig之前的所有旧manifest镜像都无法挂载根盘。
- 下一步由用户手动:`fastboot boot x9/out/x9-rev252ih-md/boot.img`,日志`~/172.txt`(≥3分钟,两卡在机,做几次底边上滑)。看`X9 ROOTGROW`、`X9 DUALSIM`/SIM2回复、`NAS registration`/`NAS SysInfo NR SA`、`ECSQ`、`X9 EXIT: SpringBoard`。

### 前次交接(HANDOFF 106,2026-11-10): 根盘扩容+16GiB——扩展文件已分配(无raw写),ig带x9_rootgrow=1待上机

- **用户批准**:"再扩 16g adb 已连接"(只批准扩容;**未批准**清崩溃报告,未做)。
- **设计**:根盘文件不动。新建第二个专用pinned+immutable文件`/data/local/tmp/x9-ios-pan92-20261027-1215/iosroot-extension-1.hfs`(16GiB),manifest新增`extension_policy`;`ufs_root_manifest.device()`把root‖extension拼成一个设备导出到ADT(`x9-ufs-root-bytes`/`write-contract`=32GiB,extents 709+4130=4839,上限16384)。内核`sweet_x9_ufs.c`对可写根盘允许HFS≤设备(只读根盘仍须相等);`x9_rootgrow=1`时PID1调用`x9_hfs_grow_root`(hfs_readwrite.c)→Apple原生`hfs_extendfs`(HFSIOC_RESIZE_VOLUME同一路径,journaled,扩bitmap,写新备用卷头),设备=文件系统时no-op。日志`X9 ROOTGROW: HFS a MiB -> b MiB errno=e`,以及`X9 UFS NATIVE ROOT: HFS ... on a ... device`。
- **执行(观察)**:`x9/tools/grow_root_extension.py --create --yes`:preflight通过→先把根盘前64KiB/最后64KiB(HFS卷头/备用卷头)保存到私有state→`pinfile create … 16384`+`chattr +i`。新文件4130个extent,其中2,141,696块(8.37GiB)落在manifest的`protected_extents`里,validate拒绝,manifest未生成。逐块溯源:全部属于4个历史文件——`x9-ios-full-20261026-222518/iosroot-full.hfs`(1,747,968)、`x9-ios-rw-20261027-010328/iosroot-full.hfs`(383,488)、`x9-pin-test-20261026-194306/test-8m.bin`(1,024)、`x9-iosroot-20261026-202513/iosroot.hfs`(9,216);adb确认四个路径都不存在,F2FS已把这些块分配给新pinned文件。按HANDOFF86先例`--resume --retired-reservation-manifest ×4`只把重叠部分从protected改归extension(protected 4262→2798条),校验旧排除并集仍被protected∪extension覆盖。之前/之后root/插件/日志/BBFW文件身份与FIEMAP一致,根盘前后64KiB未变,`preflight-after`通过(`HFS size: 17179869184 (not grown yet)`)。**没有任何raw写**,新块保留旧内容(HFS扩容后视为空闲)。
- 私有state(不入git,含序列号):`.local/x9/root-extension/20261110-16g/`(provisioning.json、operations.jsonl、两段64KiB头备份、existing-files-before/after、reservation-reclassification.json、新`root-manifest.json`、`local.json.before`)。
- **Mac `x9/build/local.json`的`root_manifest`已改为新manifest**(旧值备份在state目录)。**注意**:ig扩容成功后HFS=32GiB,任何用旧16GiB manifest打包的boot.img都会在挂载检查失败(EINVAL),包括之前所有候选和其他会话的镜像;新manifest+不含本提交sweet_x9_ufs.c改动的内核在扩容**之前**也会失败,扩容之后可用。
- **x9-rev252ig-md**:在独立worktree `~/ios_plj110-wt-if`(origin/main caa55be + 本次改动)构建,`--boot-arg x9_rootgrow=1`,其他参数同if;ADT核对`x9-ufs-root-bytes`=34359738368、extents 4839;boot.img 12959744B;warning 1412;产物已复制到`~/ios_plj110/x9/out/x9-rev252ig-md/`。日志`.local/x9/work/pdp-activation/build-ig.stdout.log`。插件不变,无需mailbox staging。Mac主checkout仍有codex会话未提交的双卡改动,未触碰、未pull。
- 下一步由用户手动:`fastboot boot x9/out/x9-rev252ig-md/boot.img`,日志`~/172.txt`。看`X9 UFS NATIVE ROOT`(32768 MiB)、`X9 ROOTGROW: HFS 16384 MiB -> 32768 MiB errno=0`、`hfs_extendfs:`行、之后`X9 BBFW: root HFS ... free`应约16GiB。if的`X9 EXIT`/ECSQ验证同样在这次看。之后的镜像可以不带x9_rootgrow(带着也是no-op)。若出错:不要用旧manifest的镜像(扩容前仍可用旧镜像,扩容后不行);根盘头备份在state目录。

### 前次交接(HANDOFF 105,2026-11-10): 171(ie)槽泄漏已基本解决;SpringBoard改为"底边上滑后静默退出";ECSQ是两行;if加退出原因日志

- **171.txt**(Mac `~/171.txt`,1588410B,末108.1s,无panic)。用户:崩得更频繁。SpringBoard pid 26→323→373→385→398(44.9/66.2/75.9/89.7s退出)。
- **CSG(观察)**:ie修复生效——之后`X9 RENDER CLOSE ... STOP/TERMINATE acknowledged`十余次都成功,只有62.2s一次`faults or AS1 busy (1ccc40000);retain`;`unresolved client teardown`只1次(170是6次);**没有**`no CSG slot`/`device refused`。
- **新的退出方式(观察)**:4次SpringBoard退出都**没有CRASH行**(不是mach异常),都发生在`SWEET HID: touch 0 down at x,2723–2743`(屏幕底边,上滑手势)之后约150ms;SpringBoard随即请求CoreImage `_blendWithAlphaMaskB0`/`_scaleClamp`(native-metal refused,EntryNotFound),约40ms后进程消失。170的82s那次相同。168里同样的底边触摸(57.36s)不触发CoreImage也不退出;从169(MG重建那次)起每次都这样。**推断**:MG缓存重建后SpringBoard的某些答案变了(旧缓存建于11-08 cn,早于`da04330`/`b108506`的414pt屏幕呈现),上滑走了新的CoreImage路径,随后被abort_with_reason/外部kill结束。未证实,原因需看退出原因。
- **磁盘(观察)**:根盘空闲 169 152MiB → 170 52MiB → 171 40MiB(`VeryLowDisk`,非root可用24MiB);170/171里CommCenter写`CellularUsage.db-journal`得到`errno=28`(ENOSPC)38/72次。很可能是mediaserverd每~5s崩溃写的崩溃报告(170里108次)。与SpringBoard退出的关系未知,但空间会继续缩小。清理需要用户确认(建议进Android清`/private/var/mobile/Library/Logs/CrashReporter`等)。
- **ECSQ(观察)**:原始回复是两行:`+ECSQ: 25,52,-325,1,1,-30,-355,4096,55,3`(LTE:RSRP −88.75dBm,RSRQ −7.5dB,RSSI −81dBm,SNR 13.75dB)和`+ECSQ: 87,126,-325,1,1,0,0,32768,0,0,0,0,0`(NR,未测)。x9q_line遇到重复tag就失败。**if**:逐行取Act为LTE的那一行。
- **if新增**:`kern_exit.c proc_exit`对SpringBoard/backboardd/CommCenter打印`X9 EXIT: name[pid] xstat (signal/status) reason ns code flags desc "..."`(EXIT_REASON_USER_DESC字符串)。用来确定上滑退出是谁、为什么结束的。
- **x9-rev252if-md**:Mac上**独立worktree** `~/ios_plj110-wt-if`(HEAD 62e304e…c066ba2 + 本次3文件,`.local`为软链)构建,产物复制到`~/ios_plj110/x9/out/x9-rev252if-md/`,boot.img 12926976B,参数与ie一致,kernel不含`x9_dual_sim`。原因:Mac主checkout里有另外的codex会话未提交的双卡改动(`x9_mipc_bridge.inc`/`x9_modem_bridge.h`/`X9WDS.inc`,`x9_dual_sim=1`门控),不能混进来;那三处文件未被本会话修改。Mac主checkout的`x9_modem.inc/kern_exit.c/X9QMIAT.inc`内容与本提交相同(scp过),但没有pull,留给那个会话自己处理。日志`.local/x9/work/pdp-activation/build-if.stdout.log`。插件275不变,无需mailbox staging。
- 下一步由用户手动:`fastboot boot x9/out/x9-rev252if-md/boot.img`,日志`~/172.txt`,做几次底边上滑。看`X9 EXIT: SpringBoard`行、0x4f是否`result 0`/信号格。

### 前次交接(HANDOFF 104,2026-11-10): 170(id)ECSQ解析为0;"注销"=GPU CSG槽泄漏后SpringBoard崩溃循环;ie修两处

- **170.txt**(Mac `~/170.txt`,1840702B,末166.6s,无panic)。用户:仍无信号格,设置里**出现了蜂窝**;这几版会不定时"注销"(黑屏转圈)。
- **ECSQ(观察)**:`AT+ECSQ`每次AT OK,但`ECSQ fields 0`,0x4f回16。id没记录原始回复,不知道是多于13个字段(旧代码遇到就清零)、URC重复行还是别的格式。**ie**:最多解析24个字段,不因尾部内容清零,前6次打印原始回复(`raw "..."`,`|`=CR,`/`=LF)。
- **注销(观察)**:SpringBoard pid 26→344(约82–87s)→401/409/419/425/429/435(140s起每~5s一次)。140s后的每次都是`X9 RENDER SLOT: no CSG slot for a new group` → PanVK 0xe00002be → `native-metal: device refused VK -3` → SpringBoard sig 11 pc 0(拿到nil device)。8个CSG槽被占满的原因是关闭时泄漏:`X9 RENDER: unresolved client teardown`×6(mediaserverd 294/349/356/371/373和SpringBoard 344),这些进程的group从未CLOSE。mediaserverd本身每~5s崩一次(`X9 GENERAL METAL: missing class=NBFunction selector=returnType`后abort,或nil device sig 11;170里108次,169里40次,165–168约12–28次),每次新建一个group。
- **根因(静态推断,和日志吻合)**:`render_destroy_locked`关闭已SUSPEND的group时先检查其他槽状态,有state 1/3就返回Busy且不重试;x9_gpu_park下backboardd的group几乎总是parked RUNNING,所以崩溃进程的关闭基本都失败,槽永久泄漏。成功关闭的几次(29/240/302/365/26)应是当时没有别的context parked。82s那次SpringBoard 26退出没有CRASH行,原因未查明(它的槽正常释放了)。
- **ie修复**:关闭前若另一个context有parked group,先`render_retire_parked("close of another context")`(与render_submit处理"other context"相同,持owner lock);每个拒绝关闭的分支都加了`X9 RENDER CLOSE: ... retain`日志,下次能看到具体原因。mediaserverd的NBFunction崩溃没修(插件/Metal侧问题)。
- **x9-rev252ie-md**:Mac构建成功,命令同ic/id(`--name x9-rev252ie-md`),boot.img 12922880B,参数与ic一致,warning 1412(未增加),插件275不变,**无需mailbox staging**。`X9 MODEM: build=rev252ie-md ...-ecsq-csgclose`。日志`.local/x9/work/pdp-activation/build-ie.stdout.log`。
- 下一步由用户手动:`fastboot boot x9/out/x9-rev252ie-md/boot.img`,日志`~/171.txt`,**尽量采集≥3分钟**。看:`unresolved client teardown`是否消失、`X9 RENDER CLOSE`里"parked"/"retain"行、140s后SpringBoard是否还循环;`ECSQ ... raw`原始回复。

### 双卡隔离分支指针(HANDOFF 107-DS,2026-11-10): uim3已编译;未上机,不等于双卡显示/上网完成

- **代码在独立分支,未合入main**:`work/dual-sim`,提交**`71ce384`**(MIPC/UIM/NAS/DMS/WDS分槽)+**`58a823b`**(BSP activation分槽/DsdsAllowed+离线D22能力门准备器)。worktree `.local/x9/worktrees/dual-sim`;完整证据、命令、未完成项见`git show 58a823b:NOTES.md`的106/107-DS。main另一会话未提交的三个代码文件保持原样,本次main只提交这段文档。
- **撤回103的双卡绝对断言**:20H392 arm64实际有双卡链,不是必须换arm64e IPSW。`GsmRadioPersonality+0x241`为D22关闭的第一层门;继续追到`ActivationController→ActivationModel→BSP 0xe2/0xb00c TLV0x53 DsdsAllowed`为第二层。旧BSP只缓存slot-one身份且不区分UIM session,双卡读回会串卡;uim3已补分槽缓存和真实ICCID对比后上报。尚无设备验证,不能只改门就宣称网络可用。
- **最新双卡候选**:`.local/x9/worktrees/dual-sim/x9/out/x9-dualsim-uim3/boot.img`,本地构建exit0,构建器报告12922880B;日志`.local/x9/work/dual-sim-20H392/build-uim3.stdout.log`。`x9_dual_sim=1`、`x9_dpmaif=2`、`x9_mipc=1`,插件275不变,**无需mailbox**。保留ie LTE-only/旧ECSQ,不含main尚未提交的171多行ECSQ/exitlog修复;不混NR候选。无CI run/测试套件/二次产物校验/设备操作。
- **用户下一步**:两卡留机,手动`fastboot boot .local/x9/worktrees/dual-sim/x9/out/x9-dualsim-uim3/boot.img`,约3分钟日志另存`~/dual-uim3.txt`。看`X9 DUALSIM: uim3`、两槽CPIN/EF_DIR、`distinct 1`、`BSP DsdsAllowed 1`、两卡NAS。**仍用原D22能力门,本轮只先证明读卡链,不应期待已出现双图标/上网。**
- **离线cache副本已准备,未安装**:脚本在该分支`x9/tools/prepare_dual_sim_cache.py`;副本`.local/x9/work/dual-sim-20H392/patched-cache/dyld_shared_cache_arm64.31`,`cache-patch.json`记录原/新28字节。只改D22(enum7)的+0x241谓词,组件偏移0x36ecf98,不改NR,不换机型。签名未重建、加载未验证。此偏移不是根盘/UFS偏移;先验证uim3读卡,再向用户单独确认可回退安装,不要自行写设备。DDS020d已追到slot→mask1/2,但getter/完成确认未证明,桥未放行真实DDS切换。最终仍需CC两卡显示/注册→切换数据卡→iOS实际收发。

### 隔离交接(HANDOFF 107-DS,2026-11-10): uim3补齐BSP分槽身份/DSDS上报;D22能力门28字节离线副本已准备,未安装

- **当前双卡候选改为uim3,取代106-DS的uim2**。分支仍`work/dual-sim`,worktree `.local/x9/worktrees/dual-sim`;首阶段代码提交**`71ce384`**。不混NR分支、main的171/exitlog改动。**没有新的实机日志,两卡显示/数据均未验证。**
- **新证据(当前20H392 arm64)**:CC原始chained fixup解析后,`ActivationInterface`实际为`ActivationController`,vtable 0x1015adf80的+0x90→0x10089ded4→dispatch任务0x1008a75a4→controller+0x70模型vtable+0x1c0。实际factory 0x10031b0a0→0x10084000c创建`ActivationModel`,getter 0x100844d74返回model+0x398(BoolWithUnknown)。0x100843ab4..0x100843b30由personality+0x241和`ActivationInfo+0x210/0x211`的值/有效位更新它;XPC回调0x10063e054将unknown0→capability4、false1→Disabled1、true2→Enabled2。**不是一个直接返回Enabled的函数。**
- **已追到线上包字段**:QMIMav13 ActivationRegister indication(BSP **0xe2 / 0xb00c**)在0x1f1a3243c先查+0x241,再解析**TLV0x53/u8 DsdsAllowed**,0x1f1a3248c/494写ActivationInfo+0x210/211。其0x46/47是真SIM2 ICCID/IMSI,0x51为SIM2 provisioned,0x58/59为是否embedded。原桥只有slot-one身份缓存,`cbd_uim_read_file`也不检查session;启用session2/7后,成功的SIM2 EF回包会覆盖slot-one缓存。这是uim2遗漏的桥层,不是“IPSW没有双卡代码”。
- **uim3补丁**:`coachz_baseband.cpp`把ICCID/IMSI缓存分槽;请求按session0/6→0、2/7→1,保存local client+generation+txn+file,非空AID/非零offset不作为身份观察,不覆盖在途owner。换ICCID清同槽旧IMSI,数据不进日志。X9完成两槽CPIN/ICCID后原子发布快照,只有真实ICCID不同才置`bb_dsds_read_ready`;失败会撤销快照。`coachz_bb_data.h`增加原生双槽activation envelope,53来自该快照而非强制true,46/47只取SIM2缓存。默认单卡保留原activation编码。**DsdsAllowed表示此实验适配器放行策略,不是DDS成功、注册/双待或联网证明。**
- **uim3构建exit0**:命令同106-DS,仅`--name x9-dualsim-uim3`。产物`.local/x9/worktrees/dual-sim/x9/out/x9-dualsim-uim3/boot.img`,构建器报告**12922880B**;日志`.local/x9/work/dual-sim-20H392/build-uim3.stdout.log`。插件275不变,**无需mailbox**。无CI run、无额外测试套件、成功后无二次产物校验。仍继承ie LTE-only/旧ECSQ解析,未包含main新修复。
- **离线能力门补丁(只准备,未安装)**:`x9/tools/prepare_dual_sim_cache.py`复用已有NR准备器的只读输入/UUID/原指令检查和exclusive-copy机制,但补丁本身完全独立。libCommCenterBase UUID `64cd5d55-eec1-3ee0-a697-cc0ecd0ee021`,VA **0x1f036cf98**,cache组件内偏移**0x36ecf98**,改28字节。原谓词`idx=hw-12; (uint8(idx)<45)&(0x1f021e078e07>>idx)`改为`idx=hw-7; (uint32(idx)<50)&(0x3e043c0f1c0e1>>idx)`;仅增加硬件enum7(D22),其他配置位不变,最后STRB+0x241不变。没有改设备身份/NR+0x121/CT返回值;还会拒绝带独立NR补丁的输入。原cache未改,签名未重建、设备加载未验证。
- **副本/复现**:在该worktree运行`python3 x9/tools/prepare_dual_sim_cache.py --source /private/tmp/x9root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.31 --output .local/x9/work/dual-sim-20H392/patched-cache/dyld_shared_cache_arm64.31`已exit0。输出已存在,重放须选新的输出路径;`cache-patch.json`保存原/新28字节及偏移。副本在`.local/x9/work/dual-sim-20H392/patched-cache/`,全部Apple内容gitignored。**组件偏移不是根盘/UFS偏移,禁止直接dd到设备;不得把副本与NR副本交叉覆盖。**
- **DDS新进展(只读逆向,未发命令)**:MTK `obtainMipcData(020d,preferredSlot)` 0x1c0730→`RfxMipcUtils::slotIdChangeToMipcSlotId` 0x1ad9e0,证实slot0/1→mask1/2(表0x129c44=[2,4,8])。前置`sendRfOccupyCommandIfNeed(false)`仅当handler+0x54发生变化才异步发00cb,TLV100/u8=2、TLV101字符串`MSPM_NDDS_DATA_VALIDATION_SESSION`;不是每次无条件必发。DDS getter/完成确认仍未定位,所以020d仍不在桥allowlist,不盲ACK Apple DDS请求。
- **下一步必须先取真实SIM2回复**:用户两卡留在手机,手动`fastboot boot .local/x9/worktrees/dual-sim/x9/out/x9-dualsim-uim3/boot.img`,日志另存`~/dual-uim3.txt`约3分钟。看`X9 DUALSIM: uim3`、两槽CPIN/EF_DIR、`distinct 1`、`BSP DsdsAllowed 1`和两卡NAS注册;失败按第一个失败阶段定位。**此次boot仍加载原D22能力门,无需也不要先安装cache副本**。读卡证实后再规划可回退的根盘补丁安装并向用户单独确认,然后验收CC两条线路、真实DDS/APN/PDP及iOS收发。无设备操作/根盘写入。
- **证据位置**:`.local/x9/work/dual-sim-20H392/`新增`cc-chain.py`(只读解析真实DYLD_CHAINED_PTR_64_OFFSET及LC_FUNCTION_STARTS)、`activation-{constructor,model-factory,gemini-update,capability-rest}*.asm`、`qmi-activation-register.asm`、`mtkmipc-{dds-routing,slotmask}.asm`、`d22-dual-gate.{s,encoding}`、`cache-patch.json`;普通反汇编器的未解析bind=0不是函数地址证据,旧字符串自动注释可能因寄存器失效而不准,以指令/已解析fixup链为准。

### 隔离交接(HANDOFF 106-DS,2026-11-10): 双卡UIM/MIPC路由补丁uim2已编译;未上机,尚非双卡显示/上网完成

- **分支/位置**:`work/dual-sim`,worktree `.local/x9/worktrees/dual-sim`,基于`758e464`。本次四文件修改只在此worktree;不覆盖main另一会话的`x9_modem.inc`/`kern_exit.c`/`X9QMIAT.inc`,不混`x9-nrsa`。曾发生main的QMIAT被另一会话重写,恢复快照在`.local/x9/work/dual-sim-20H392/concurrency-snapshot/`;旧`restore-uim-edits.py`比当前源码旧,不要重跑覆盖。
- **逆向证据→补丁**:原厂`libmtk-ril RfxSender::atSendCommandFullNolockAck` 0x23dcf4..0x23dd58走MIPC请求0003/回包0004、字符串TLV8100(含NUL,不含tty尾CR);slot0默认mask1,slot1经0x150d1c表得到mask2,与已有ttyCMIPC1对应。`x9_dual_sim=1`启用按SIM的只读AT;只放行既定查询与CRSM READ/GET RESPONSE,拒绝SIM映射/PIN/NV写和命令串联。CNF必须包含完整字符串和唯一末尾AT final;CNF到达不等于AT OK,超时取消仍保留迟到CNF所有权,绝不把SIM2 fallback到SIM1。
- **UIM**:0x2f/0x47依次读取两槽CPIN、真实ICCID、EF_DIR/AID,两张ICCID必须不同才发布双卡;仅CME10认定absent,其余错误不伪装成无卡。primary/secondary GW索引分开;session0/6→SIM1,2/7→SIM2,非空AID/逻辑通道仍拒绝。OPEN后安排一次只读CardStatus、SlotStatus、SIM1/2注册探针,不依赖D22创建第二client,不拨号。
- **NAS/DMS/WDS**:NAS45/DMS54按TLV1/u8(0/1)绑定查询路由,入队时快照SIM;WDS AF按TLV1/u32(1/2)绑定,B0读回。WDS bearer、CID冲突和EIF状态按SIM隔离,ACT/GET/DEACT/DISC和两套事件注册走对应mask;仍需真实ACT+GET+EIF/IFUP后才接DPMAIF。**订阅绑定不是DDS切换**;不假ACK默认数据卡请求,不解除显式APN/IPv4/noauth限制。
- **更正iPhone X IPSW断言**:当前arm64 `CTXPCClientHandler getDualSimCapability:` 0x10063d104读取`GsmRadioPersonality+0x241`,0则返回enum3=NotSupported;`subscriber::makeSimSlotRange` 0x1f0320bfc同样据此选择槽范围。D22构造器硬件enum7默认该字段0,但实际二进制保留双槽[1,2]及双卡调用链。非0后还查询`ActivationInterface` vtable+0x90(原CC chained bind 0x10151b018→typeinfo),该实现尚未追完。**本候选没有修改Apple能力门**,因此不能期待已经出现两个UI图标。
- **构建exit0**:在该worktree执行`python3 -I x9/build/build.py all --name x9-dualsim-uim2 --boot-config .local/x9/modem/boot-rev252hi-md.json --boot-arg x9_dpmaif=2 --boot-arg x9_mipc=1 --boot-arg x9_dual_sim=1`;产物`.local/x9/worktrees/dual-sim/x9/out/x9-dualsim-uim2/boot.img`,构建器报告**12931072B**。日志`.local/x9/work/dual-sim-20H392/build-uim2.stdout.log`;插件275不变,**无需mailbox**。无CI run、无测试套件、成功后无二次解包/hash/反汇编;无设备操作/根盘写入。uim1已被uim2替代。继承ie LTE-only及旧ECSQ解析,**不含main尚未提交的171多行ECSQ/exitlog修复**,区别看`X9 DUALSIM: uim2`而非旧MODEM build标签。
- **下一实机输入**:用户留两张卡,手动`fastboot boot .local/x9/worktrees/dual-sim/x9/out/x9-dualsim-uim2/boot.img`,约3分钟日志另存`~/dual-uim2.txt`。重点看`X9 DUALSIM:`两槽CPIN/EF_DIR、`distinct 1`、两卡NAS注册及`X9 MIPC AT: CNF ... rejected`;未知/超时不能当absent。没有此实测前不能称SIM2读通。
- **未完成/继续点**:MTK preferred-data实现0x2cb1a0使用MIPC020d,先调用`sendRfOccupyCommandIfNeed(false)` 0x2bc830;slot转mask入口`RfxMipcData::obtainMipcData` 0x1c0730待追。Apple WDS5569在`updateCurrentDataSIM_sync` 0x1f19d751c受personality+0x121门控,另有DSD4e;不能猜D22选哪条或直接发020d。下一步为ActivationInterface实际策略、DDS确认状态与两卡真实PDP。最终验收是两张真实卡显示/注册且切换主数据卡后**iOS实际收发**。地址级证据/只读脚本在`.local/x9/work/dual-sim-20H392/`,Apple原件与派生均不入git。

### 并行交接(HANDOFF 105-SA,2026-11-10): NR注册实验已编译;D22能力门离线求值为0;8字节cache补丁仅本地准备,未安装

- **隔离分支/代码提交**:`x9-nrsa` / **`dde26eb`**,worktree `.local/x9/work/ios_plj110-nrsa`。基于`c066ba2`(含`62e304e`的ie ECSQ/CSG修复),不含main另一会话后续MIPC/WDS修改。**不要覆盖main当前未提交代码**,后续按提交挑选合并。
- **更正104-SA的TLV说法**:GetSysInfo response(NAS0x4d)用NR status **0x4A** / info **0x4B**;SysInfo indication(NAS0x4e)用 **0x4C / 0x4D**。Apple indication在0x1f1b0f924明确传0x4D,与response共用解析函数不代表标签相同。旧桥把response标签0x4A用于indication也是bug。
- **D22门(离线观察,不是设备内存实测)**:20H392 `TelephonyRadios`模型映射0x1937c5368对D22得到product **0x40**(0x1937c7240分支);`getHardwareConfiguration`经0x1f036c11c得 **7**;按实际构造器字段/分支执行,0x1f036c650清零覆盖+0x121,直到返回仍为 **0**。指令级求值只显式建模分配/字符串等无关调用,未知调用会停止。脚本/完整PC链:`.local/x9/work/nrsa-20261110/{evaluate_d22.py,d22-evaluation.txt}`。当前证据是“原始D22构造器关闭NR门”,不推断以后没有对象写入。
- **桥接已改/编译**:`X9QMIAT.inc`修正response/indication标签;只有真实`C5GREG=1/5`且`COPS AcT=11`才生成29B NR System Info。NR只报PS域,来源CGATT;漫游来自C5GREG、PLMN来自COPS;未知CGATT及LAC/CID/TAC/reject均invalid。不把LTE/NSA当SA,不伪造RSRP或信号格。LTE打包逻辑未改。
- **用户态离线实验副本已准备,未安装**:`x9/tools/prepare_nr_registration_cache.py`(在上述分支)校验libCommCenterBase UUID及原指令,拒绝覆盖已有文件,仅复制原cache组件后改8字节。D22所在构造器case在0x1f036d1b0/1b4改`mov w9,#1; strb w9,[x19,#0x120]`为`mov w9,#0x101; strh w9,[x19,#0x120]`,保留+0x120=1并置+0x121=1;后续两个STRB仍写1。该case共用于硬件配置5–10,D22是7,不是全局换机型。**只针对NR注册门,不是完整SA功能补丁。**原cache未改;签名未重建、设备加载未验证。
- 副本:`.local/x9/work/nrsa-20261110/patched-cache/dyld_shared_cache_arm64.31`(103677952B,同原组件大小);精确改动/回退字节见`cache-patch.json`:文件偏移`0x36ed1b0`,原`2900805269820439`,新`2920805269420279`。全部Apple产物保持gitignored。此偏移是**cache组件内偏移**,绝不能当UFS/root盘偏移写入。
- **候选已构建**:`.local/x9/work/ios_plj110-nrsa/x9/out/x9-rev252nr1-md/boot.img`,构建器报告 **12926976B**;插件275未变,**无需mailbox**。在该worktree运行:`python3 -I x9/build/build.py all --name x9-rev252nr1-md --boot-config .local/x9/modem/boot-rev252hi-md.json --boot-arg x9_dpmaif=2 --boot-arg x9_mipc=1 --boot-arg x9_md_lte=0`。`x9_md_lte=0`只是跳过ERAT3、使用MD默认,**不强制SA**。日志`.local/x9/work/nrsa-20261110/build-nr1.stdout.log`。本地构建exit0,无CI run/测试套件,成功后没有解包/hash/二次产物校验。
- **当前不能宣称SA已通,也不要只boot这个镜像来验收SA**:cache副本不在boot.img里,只boot仍用原D22能力门。**没有设备操作/根盘写入**。下一步先确定可回退的cache加载/根盘8字节安装方式,具体写入需用户单独确认;验证加载和CommCenter接受NR后再扩展策略/信号/数据链。`getSupports5GStandalone`服务端已定位`CTXPCClientHandler 0x100f23f30`,其控制器vtable+0x1d0的策略仍待追。
- **尚未实现**:NAS0x33实际SA策略映射、NR信号、运营商/APN和数据连通。新确认0x33 `NRDisableMode`为TLV **0x2E/u32 LE**,`EnableNrUpdateReason`为 **0xA4/u8**(不是两个u8)。MTK E5GOPT读改写在`requestSetNrOption 0x3b84a0`、`requestSmartRatSwitch 0x3b6ed0`,取值/SA语义需继续核实;未猜值、未假ACK。最终仍需**CC NR接纳→WDS Start→MIPC→DPMAIF→iOS真实收发**,当前仅完成可复现的离线准备/编译。

### 并行交接(HANDOFF 104-SA分析,2026-11-10,与上条同日另一会话): 更正“iPhone X IPSW没有5G逻辑”;已定位SA解析、能力门及桥接缺项

- **本次仅分析,无设备操作/构建/CI/上机**。代码基线`137ba97`(本轮期间其他工作完成的id提交);最新实测仍169(ic-mg),候选仍id,插件275不变。未修改任何内核或Apple二进制。
- **更正旧结论**:实际20H392 **arm64** CoreTelephony有`getSupports5GStandalone:error:`(0x1881b8170,实际发XPC查询),QMI MCommandDrivers有`enable5GStandalone`(0x1f1afc288,构造NAS 0x33+NRDisableMode)及`NR5GSysInfo`解析。GetSysInfo响应0x1f1b08934和SysInfo indication 0x1f1b0f928共用NR解析函数,但标签分别为0x4B/0x4D(105-SA修正)。因此HANDOFF100/102的“CommCenter没有5G概念/换14 Pro才能认识5G”**撤回**,ERAT=3只是LTE-only实验策略,不是SA已解决。
- **已定位但未动态验证的能力门**:`processSystemInfo_sync`在0x1f191ce04取`Registry::getRadioPersonality()`,读对象+0x121,为0跳过NR;之后还检查NR输入presence。构造器按硬件配置设置能力。本次未读取设备对象、未完整求值D22分支,不能说该位的运行值已测。不能由代码存在直接说未修改的D22配置能用SA。
- **桥接缺项(确定)**:`x9q_nas`只输出NR 0x4A三字节service status,没有0x4B NR System Info;LTE有0x19完整信息。167日志的SA→LTE差异因此不能证明用户态缺NR支持。MTK的`C5GREG=1/AcT=11`只证明注册,不证明iOS数据通。
- **控制中心UI**:`CCUIConnectivityCellularDataViewController buttonTapped:`→`_toggleState`→CT Get/SetCellularDataIsEnabled;只翻转数据总开关,不是LTE/NSA/SA选择器。
- 证据:`.local/x9/work/cc-5gsa-20H392/`(本地Mach-O符号/反汇编、可复核脚本、provenance;不入git)。四处关键代码与根盘快照原cache一致;快照SystemVersion=16.7.16/20H392。正式结论与复核命令:`docs/2026-11-10_reverse-iphone-x-5gsa-report.md`。
- **下一步**:保留id的LTE信号验证计划;SA另做最小分支,先定位/观察personality NR能力位,补真实NR SysInfo/信号和NAS 0x33策略映射,核对运营商/APN,再解除LTE-only。必须以CommCenter NR注册→WDS Start→MIPC→DPMAIF→iOS实际收发为验收,不能只看5G图标。无需先换整套arm64e IPSW,也不承诺SA已经可用。

### 前次交接(HANDOFF 103,2026-11-10): 169(ic-mg)出现单卡信号;信号条空=NAS 0x4f被拒,id用真实AT+ECSQ回答;双卡判断仍待专项核验

- **169.txt**(Mac `~/169.txt`,2219165B,末150.8s,无panic)。`X9 MGWIPE: cache errno=0`(3.155s)生效。用户观察:状态栏出现**一张卡**的运营商/信号,但**信号条是空的**,也没有双卡。30.26s `CEREG 1 CGATT 1 COPS-AcT 7`。清缓存后CommCenter的请求明显变多(NAS 0x33/0x44/0x45/0x5562/0x5566/0x5567等,全94拒),仍无0xc8/WDS Start。
- **信号条(观察+修复)**:CommCenter在30.24–30.29s发NAS 0x4f Get Sig Info ×6,全部94拒;之后只每10s发0x5556(Apple私有)。0x20 Get Signal Strength它根本没发。**id**:0x4f改为真实`AT+ECSQ`。字段布局取自反汇编libmtk-ril `getMdSignalStrengthByAT`(0x39cf90)与`updateSignalStrength`(0x39d1a0)LTE路径:sig1,sig2,rssi_qdbm,rscp,ecn0,rsrq_qdbm,rsrp_qdbm,Act,sig3,band;LTE=Act 0x1000/0x2000,有效条件sig1≤34且sig2<98;rsrp/rsrq/rssi=qdbm/4,**SNR(0.1dB)=sig3*5/2**,sig3=0x7fff无效。回LTE TLV 0x14(rssi i8,rsrq i8,rsrp i16,snr i16)。收到0x6c的client每10s另收一次0x51 Sig Info indication(同一AT来源)。非LTE或无效值仍NOT_READY,不编造。日志`X9 QMI AT: ECSQ fields ...`前6次打印原始数值。
- **双卡旧结论已撤回(107-DS更正)**:D22默认单卡不能推出20H392 arm64用户态没有双卡实现,也不能推出必须换arm64e IPSW。已定位双槽能力门与BSP DsdsAllowed链,分槽适配补丁在独立`work/dual-sim`分支,尚未设备验证。SIM2 GPIO日志本身仍不是双卡可用证明。
- **x9-rev252id-md**:构建成功(Mac),命令同ic(`build.py all --name x9-rev252id-md --boot-config .local/x9/modem/boot-rev252hi-md.json --boot-arg x9_dpmaif=2 --boot-arg x9_mipc=1`),boot.img 12922880B,内嵌参数与ic逐项一致(不含mgwipe),warning数1412与ic相同,插件275不变**无需mailbox staging**。日志`.local/x9/work/pdp-activation/build-id.stdout.log`。注意:`X9 MODEM: build=`字符串仍是ic的,未改。
- 下一步由用户手动:`fastboot boot x9/out/x9-rev252id-md/boot.img`,日志`~/170.txt`(≥120s),看`ECSQ fields`行的act/数值、0x4f是否`result 0`、状态栏信号格。若ECSQ返回ERROR或字段数<10,按原始行再调整。蜂窝数据仍未拨号(无0xc8/WDS Start),logd取证待做(见HANDOFF102)。

### 前次交接(HANDOFF 102,2026-11-09): 168(ic)LTE-only生效、注册稳定,但CommCenter仍不发数据请求;需再取logd

- **168.txt**(Mac `~/168.txt`,1946100B,末114.57s,无panic,无board重置)。`build=rev252ic-md`;17.29s `LTE-only AT+ERAT=3 -> OK after 40 ms`;21.45s MIPC ready;23.66s SetOperatingMode 0;24.25s注册全0;**33.42s `CEREG 1 CGATT 1 COPS-AcT 7`**,之后到114s不再变化;LTE SysInfo每10s一次(33.4/43.4/53.4/63.4s…)。
- **观察**:LTE注册后CommCenter只做周期查询:NAS 0x5556×9(Apple私有,拒)、UIM 0x2a×8(ACK)、DMS 0x2f×6(GetTime,拒)、NAS 0x4f×3(拒)、NAS 0x6c×1(ACK)、WDS 0x30×1(拒)。**没有0xc8推送、没有WDS 0x20 Start**,MIPC拨号未触发。
- **推断**:注册这一环已经过了;现在不拨号是CommCenter数据侧的决定,候选:APN/运营商配置(PLMN 46015,iOS 16.7是否带该运营商bundle未知;163 logd里attach APN为空)、蜂窝数据开关/DataOff、或Apple私有NAS 0x5556。需要logd确认,不再盲改。
- **关于换iPhone 14 Pro身份**(历史判断,5G部分已由HANDOFF104更正):“换身份才认识5G、ERAT已解决5G问题”不成立;20H392本身有SA代码,ERAT仅限制LTE。14 Pro整套根盘仍涉及arm64e/cache、基带与GUI适配风险;现阶段不换,优先研究当前用户态的能力门和桥接缺项。
- 无新代码、无新镜像。下一步:用户进Android接ADB,按HANDOFF97流程取logd(`pull_ios_logs.py --recent-hours 2`;uuidtext已有,可只拉diagnostics),grep `FSM:|DataMode|Activat|APN|DataOff|0x5556|NOT_SUPPORTED`。

- **补充(用户观察,同日)**:设置里**完全没有"蜂窝数据"选项**。推断:MobileGestalt缓存过期。MG磁盘缓存只按OS build和build UUID校验,在X9上两者都是常量;最后一次清缓存是2026-11-08的`rev252cn-mgwipe`,而ADT的`/baseband`节点(class 3,mkadt在`x9_bb=1`时添加)是2026-11-09 `0c77622`才加的。所以缓存里蜂窝/基带能力很可能还是旧的"无",设置因此隐藏蜂窝面板,CommCenter也可能据此不启动数据。这和当初iPod问题同一机制(kern_exec.c `sweet_x9_wipe_gestalt_cache`注释)。
- **x9-rev252ic-mg**:内核与ic完全相同,只用`build.py boot --kernel`重新打包,多一个`x9_mgwipe=1`(内嵌参数41→42,其余一致)。`x9/out/x9-rev252ic-mg/boot.img`,日志`.local/x9/work/pdp-activation/build-ic-mg.stdout.log`(Mac)。**只启动这一次**;启动后MG按当前ADT重建缓存,之后回到`x9-rev252ic-md`(mgwipe不要常开)。
- 下一步由用户手动:`fastboot boot x9/out/x9-rev252ic-mg/boot.img`,日志`~/169.txt`(采集≥120s),看`X9 MGWIPE: cache errno=0`、设置里是否出现"蜂窝网络/蜂窝数据"、之后有无0xc8/WDS Start。

### 前次交接(HANDOFF 101,2026-11-09): 167(ib)证实先NR SA后LTE;LTE SysInfo发出后CommCenter立刻按已注册行事;ic强制LTE-only

- **167.txt**(Mac `~/167.txt`,1376123B/13346行,末61.95s,无panic)。`build=rev252ib-md`;22.16s MIPC ready;23.28s SetOperatingMode 0。
- **观察**:24.50s `NAS registration CREG 0 CEREG 0 C5GREG 1 CGATT 1 COPS-AcT 11`(**5G SA**);54.51s `CREG 0 CEREG 1 C5GREG 0 CGATT 1 COPS-AcT 7`(**LTE**),同一时刻`NAS SysInfo LTE registered domain 2 roam 0`,0x4e indication变为len 51。15ms内CommCenter发NAS 0x6c(Config Signal Info v2,16个阈值TLV)、NAS 0x4f GetSigInfo×2、WDS 0x30,表现得像已注册;采集在7s后结束,看不到之后是否有0xc8推送或WDS Start。
- **ERAT映射(反汇编libmtk-ril `RmcRatSwitchHandler::requestSetPreferredNetworkType` 0x3a9300起)**:表在0x190018,下标即ERAT rat值,值是Android RAF位掩码:[3]=0x84000(LTE|LTE_CA,**LTE only**),[15]=0x100000(NR only),[19]=0x184000(LTE+NR),[30]=0x1bfffe(全部)。另有下标31–33的特殊映射(rat 32/64/32,prefer 7/7/8)。prefer参数在含NR时为0x80。
- **ic修改**:(1)level-3 AT脚本在CFUN=4之后、CFUN=1之前加`AT+ERAT=3`(LTE only),结果总是打印`X9 MODEM: LTE-only AT+ERAT=3 -> OK/ERROR`;`x9_md_lte=0`可跳过。桥开始接收job的步骤号10→11。MD的NV写只进我们的RAM overlay,不改Android设置。(2)NAS 0x6c ACK(不产生signal indication)。NAS 0x4f仍拒:LTE信号TLV需要SNR,没有已验证的数据源,不编造。
- **ic构建成功**(未上机):同参数,`--name x9-rev252ic-md`,boot.img 12922880B,日志`.local/x9/work/pdp-activation/build-ic.stdout.log`(Mac)。无新warning,插件275不变,无需mailbox staging。回退:ib(不发ERAT)。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252ic-md/boot.img`,**采集至少120s**,日志`~/168.txt`。看`AT+ERAT=3 -> OK`、`NAS registration`是否直接CEREG 1/AcT 7、`NAS SysInfo LTE registered`的时间、之后有无0xc8/WDS 0x20 Start/MIPC 0x219。

### 前次交接(HANDOFF 100,2026-11-09): 166(ia)读完;LTE SysInfo未触发(采集只到47s,LTE尚未注册);ib加注册状态日志

- **166.txt**(Mac `~/166.txt`,1199596B/11816行,末47.17s,无panic)。`build=rev252ia-md`;21.63s MIPC dialler ready;21.67s SetOperatingMode 0 `(real AT)`;`resets 0`。NAS 0x4d两次`(real AT)`;0x4e indication在21.7/31.7/41.7s发出,全是len 19(只有service status),**没有出现`NAS SysInfo LTE registered`**,即这些时刻CEREG不是1/5。仍无0xc8/WDS Start。
- **历史推断(“没有5G概念”已撤回,见HANDOFF104)**:157(唯一一次91s长采集)里先是C5GREG=2,1(34.5s,CEREG=2,0,**5G SA**),52.4s才CEREG=2,1。160–166的采集都只有32–60s,可能还没等到LTE注册。当时未区分NR能力门与桥缺0x4B,误推为用户态不认识5G。LTE隔离实验使用MTK RIL的`AT+ERAT=%d,%d`(RAT模式),映射后来由HANDOFF101核实;`AT+E5GOPT=%d`(NSA/SA开关)取值尚未核实,不要猜。
- **ib修改**:SysInfo每次计算时,若注册输入有变化,打印`NAS registration CREG x CEREG y C5GREG z CGATT a COPS-AcT b`(只有数字)。
- **ib构建成功**(未上机):同参数,`--name x9-rev252ib-md`,boot.img 12922880B,日志`.local/x9/work/pdp-activation/build-ib.stdout.log`(Mac)。无新warning,插件275不变,无需mailbox staging。回退:ia/hz。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252ib-md/boot.img`,**采集至少120s**再停,日志放`~/167.txt`。看`NAS registration`的变化过程、`NAS SysInfo LTE registered`、之后有无0xc8/WDS Start。

### 前次交接(HANDOFF 99,2026-11-09): 165(hz)读完;基带重置循环消失;CommCenter认为未注册,ia补LTE SysInfo

- **165.txt**(Mac `~/165.txt`,1080224B/10833行,末42.9s,无panic)。`build=rev252hz-md`;22.00s MIPC dialler ready;22.22s `DMS 0x002e set operating mode 0`→22.26s `(real AT)`成功。
- **观察**:10s/20s摘要`resets 0`,全程没有board重置;pbl/Sahara各只打开一次。**重置循环解决。**这次CommCenter没有发attach APN推送(0xc8),也没有WDS Start。23s之后基本空闲,只有每10s一次NAS 0x5556(Apple私有,拒)和UIM 0x2a(ACK)。
- **原因(推断,有logd佐证)**:163的logd字符串中`cellularRegistrationStatus = unknown`(80次)、`kNotRegistered`(19次)、`kDataModeUnknown`(55次),说明CommCenter始终认为没注册,所以不启动数据。我们的NAS SysInfo(0x4d响应/0x4e indication)只带0x14 LTE和0x4a NR的3字节service status,没有sys info。
- **ia修改**:CEREG已注册时,SysInfo加LTE System Info TLV 0x19(libqmi布局,29字节):service domain/capability取自真实CGATT/CREG(PS或CS+PS),roaming取CEREG stat 5,MCC/MNC取COPS数字格式(ASCII,2位MNC第三字节0xFF);LAC/cell id/TAC/reject info标为无效,不编造。首次打印`NAS SysInfo LTE registered domain ...`。
- **ia构建成功**(未上机):同参数,`--name x9-rev252ia-md`,boot.img 12922880B,日志`.local/x9/work/pdp-activation/build-ia.stdout.log`(Mac)。无新warning,插件275不变,无需mailbox staging。回退:hz。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252ia-md/boot.img`,日志放`~/166.txt`。看`NAS SysInfo LTE registered`、状态栏有没有出现运营商/信号、是否出现0xc8推送和WDS 0x20 Start→MIPC 0x219/0x201。若仍不拨号,进Android取logd,grep `RegistrationStatus|DataMode|FSM:`看CommCenter的判断。

### 前次交接(HANDOFF 98,2026-11-09): 164(hy)读完;重置减为1次,但之后基带不再起来;hz实现DMS SetOperatingMode

- **164.txt**(Mac `~/164.txt`,1035051B/10265行,末31.96s,无panic)。`build=rev252hy-md`;23.13s MIPC dialler ready;24.30s `WDS 0x00c8 ... empty APN acknowledged`(2次)。
- **观察**:只剩一次board重置(25.49s,selector 0),但之后ABM每约1.04s做一次selector 1(上电)→selector 6(power cycle),resets 4→8,**不再打开pbl/Sahara/qmux**(163里每次重置后都马上重开)。桥对reset/power只重置Sahara状态,pbl通道一直在,所以不开pbl是ABM自己的决定,原因不明,需要logd。
- **线索(观察)**:重置前的DMS序列为:24.229 GetOperatingMode(0x2d,real AT)→**24.272 SetOperatingMode(0x2e) mode 0=ONLINE,被拒94**→24.465 SetOperatingMode mode 4=RESET,被拒→25.49 board重置。161–164每个boot都有1次mode 0和若干次mode 4,mode 4的次数等于当次的重置次数(3/4/4/1)。**推断**:ONLINE被拒是这条失败链的起点;mode 4只是CommCenter的重置手段。
- **hz修改**:DMS 0x2e实现真实AT:mode 0→`AT+CFUN=1`,mode 1/6→`AT+CFUN=4`;mode 3/4/5等仍unsupported(没有诚实的实现方式)。该AT超时放宽到60s(10s超时会把ttyC0隔离到下次boot)。会打印`DMS 0x002e set operating mode N`。
- **hz构建成功**(未上机):同参数,`--name x9-rev252hz-md`,boot.img 12922880B,日志`.local/x9/work/pdp-activation/build-hz.stdout.log`(Mac)。无新warning,插件275不变,无需mailbox staging。回退:hy。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252hz-md/boot.img`,日志放`~/165.txt`。看`set operating mode 0`→`response service 0x2 msg 0x002e ... (real AT)`,之后是否还有mode 4/board重置、是否出现WDS 0x20 Start。如果仍重置,进Android取logd(HANDOFF97流程)看FSM原因,以及ABM为什么不重开pbl。

### 前次交接(HANDOFF 97,2026-11-09): 从根盘取出163的logd,找到基带重置原因(attach APN推送被拒);hy已构建

- **取日志(只读)**:用户启动Android并接ADB(KernelSU root)。先跑`check_plaintext_root.py --manifest`(manifest见local.json),preflight全部通过。新工具 **`x9/tools/pull_ios_logs.py`**:只通过pinfile `rawreadat`读HFSX根盘,内存中重放dirty journal(本次17个transaction、381个block,jhdr_size 512),遍历catalog B-tree(855042条记录,256MiB),支持extents overflow和硬链接。拉取`private/var/db/diagnostics`(`--recent-hours 2`,36MB)和`uuidtext`(全量478MB,约1.5分钟)到Mac `.local/x9/logs/163-logd/raw`(不入git)。timesync里有163的boot UUID `a3b0a780…`(wall 1794235535)。
- **解码**:macOS 27的`log show`对这份手工组装的iOS 16 logarchive、甚至单个tracev3都报"corrupt or incomplete",读不出任何条目。构建Mandiant macos-UnifiedLogs被权限分类器拦截(外部代码),没有绕过。改用新工具 **`x9/tools/tracev3_strings.py`**:解开tracev3里的LZ4 chunkset(`bv41`),导出可打印字符串。只有内联参数,没有格式串;够用来搜原因。输出在Mac `.local/x9/logs/163-logd/strings-163.txt`。
- **根因(观察)**:CommCenter数据设置FSM进入`Pushing`,日志"trying to push 3GPP attach APN {apn:"", AllowedProtocolMask 1, TypeMask 1}",收到`QMI_ERR_NOT_SUPPORTED`,接着"FSM: handling Failure",随后`ResetInfo{Type=SoftReset/HardReset SubType=OtherErrors Reason='Resetting baseband after Failure to Push'}`。该字符串共4次,与163的4次board重置一致。时间上对应的是**WDS 0x00c8**(TLV `11/1=00 14/0 1f/1=00 90/1=00 91/4`,0x14为空APN),被我们回94。
- **hy修改**:WDS 0x00c8在APN为空时ACK成功(空APN=网络缺省,MD本来就是不配attach APN自动附着的,CGATT=1),不发AT命令;APN非空仍回NOT_SUPPORTED,只记录长度(真实MTK attach-APN命令尚未核实)。另外放开DF_TELECOM(`3F007F10`)下4Fxx/6Fxx的只读访问(每轮都读EF 6FE5)。
- **hy构建成功**(未上机):同参数,`--name x9-rev252hy-md`,boot.img 12922880B,日志`.local/x9/work/pdp-activation/build-hy.stdout.log`(Mac)。无新warning,插件275不变,无需mailbox staging。回退:hx。设备目前在Android,无设备写入。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252hy-md/boot.img`,日志放`~/164.txt`。看`board #.. selector 0`是否不再循环、`WDS 0x00c8 ... acknowledged`、是否出现WDS 0x20 Start和MIPC 0x219/0x201。若还有新的FSM失败,同样方法取logd:`pull_ios_logs.py --recent-hours 2 --out ...`,再`tracev3_strings.py`,grep "FSM:|Reset|NOT_SUPPORTED"。

### 前次交接(HANDOFF 96,2026-11-09): 163(hx)读完;CommCenter读完SIM后反复重置基带,原因只在它的os_log里;未出新镜像

- **163.txt**(Mac `~/163.txt`,1236286B/12412行,末43.1s,无panic)。`build=rev252hx-md`;22.08s MIPC dialler ready;UIM 0x24 GetFileAttributes多数`(real AT)`,0x21 record读成功。仍无WDS Start。
- **新发现(观察)**:CommCenter每1.2~5s对AppleBaseband做一次`board selector 0`(=重新上电),随后pbl→Sahara→qmux1/2重开→UIM读一轮,如此循环:163在24.68/25.88/28.07/32.91s,共4次。**这个循环161(5次)、162(4次)已经存在**,我在HANDOFF94/95分析时只搜了X9/WDS关键字,漏掉了。161是UIM读首次成功的那次;157/158摘要reset≤1。复位前没有固定的失败回复(有时正在连续成功读record),桥这边也没有原因可记。**推断**:CommCenter读完SIM后的某个阶段失败或超时,随即重置基带;具体原因只在CommCenter os_log里。
- 每轮的局部失败:UIM 0x24 EF 6FE5 path `3F007F10`(DF_TELECOM)被我们的白名单拒(48);两个较长path的0x20在GET RESPONSE得到CME 0(推断卡上没有该文件)。另外NAS 0x5566(Apple私有,TLV `10/2 11/9 12/1`)、私有服务0xe4 msg 0x1001、DMS 0x5557仍被拒。
- **决定**:不再盲补QMI消息出镜像。下一步先拿到CommCenter日志,方案:(a)改launchd配置,让CommCenter的os_log镜像到console:`OS_ACTIVITY_DT_MODE=1`+StandardErrorPath。iOS 16的daemon定义在launchd cache里,需要研究现有`launchd_unsecure_cache`的加载路径;属于根盘写入,需用户确认。(b)Android下只读取出根盘`/private/var/db/diagnostics`+`/private/var/db/uuidtext`,在Mac上解码tracev3,需用户重启进Android并接ADB。
- 无新代码、无新镜像。候选仍为hx(未出现回归);回退hw。

### 前次交接(HANDOFF 95,2026-11-09): 162(hw)读完;NAS SysInfo indication已送达但仍无WDS Start;hx补UIM GetFileAttributes

- **162.txt**(Mac `~/162.txt`,1389987B/13822行,末44.2s,无panic)。`build=rev252hw-md`;RAM 629739+25237页(约10.0GiB);15.2s HS2→READY;21.55s MIPC `IPv4 dialler ready`(同161)。
- **观察**:NAS 0x03被接受(首个请求`serving 0 system 1`,其余tag打印为never indicated)。传输trace显示indication 0x4e(SysInfo,len 19)在21.6/23.8/25.1/30.5/31.6/41.6s发给CommCenter的多个client。WDS/DMS事件注册已ACK。**全程仍无WDS 0x20 Start**,没有任何X9 WDS/MIPC拨号消息。
- 仍被拒(94):UIM 0x24 ×30(=GetFileAttributes,TLV `01/2 02/7`,CommCenter一直重试)、DMS 0x5557 ×30、NAS 0x5566 ×25、NAS 0x33 ×17(SET系统选择,写)、UIM 0x2a ×11(RefreshRegister)、NAS 0x45/0x4f/0x5556/0x5567/0x5562/0x5557/0x73/0x3b/0x34/0x2c、WDS 0x30/0xc8/0x5564、DMS 0x24/0x54/0x53/0x4b/0x45/0x2e/0x555c。
- **CommCenter自己的os_log不在USB日志里**(只有写stderr的SpringBoard/backboardd带`|name|`)。不知道它为何不发Start,目前只能从QMI流量推断。要拿原因需要:(a)让CommCenter的os_log镜像到stderr/console(launchd环境变量`OS_ACTIVITY_DT_MODE`+StandardErrorPath,需改launchd cache);或(b)Android下取根盘logd tracev3解码(参考HANDOFF81思路)。
- **hx修改**:UIM 0x24 GetFileAttributes走CPIN→CRSM GET RESPONSE(真实FCP),回TLV 0x10 SW + 0x11 {size,id,type,record size/count,5组安全属性,raw FCP}。**安全属性不是卡的ACL**:按本桥实际能力填read=ALWAYS(0),write/increase/deactivate/activate=NEVER(1)。UIM 0x2a RefreshRegister改为ACK(本桥不发refresh事件)。
- **hx构建成功**(未上机):同参数,`--name x9-rev252hx-md`,boot.img 12922880B,日志`.local/x9/work/pdp-activation/build-hx.stdout.log`(Mac)。无新warning,插件275不变,无需mailbox staging。回退:hw。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252hx-md/boot.img`,日志放`~/163.txt`。看UIM 0x24是否`(real AT)`、CommCenter是否停止重试、是否出现WDS 0x20 Start。若仍无Start,建议下一步先做CommCenter日志通路(a/b),不要继续盲补QMI消息。

### 前次交接(HANDOFF 94,2026-11-09): 161(hv)读完;MIPC握手完成、UIM读文件通;hw接受NAS/WDS/DMS指示注册

- **161.txt**(Mac `~/161.txt`,1158066B/11657行,末33.8s,无panic)。`build=rev252hv-md`;RAM 631247 free+23729 wired页(约10.0GiB);13.8s HS2→READY。
- **MIPC(观察)**:21.68s OPEN→21.96s `OPEN_CNF version 0`→stage 1 confirmed;0x307→0x308两次(mask 0x01,flag 0x02)→22.86s `v2 OPEN + DEACT/EIF subscriptions confirmed, IPv4 dialler ready`。**MIPC控制面首次就绪。**
- **UIM(观察)**:0x47、0x2f、多数0x20 ReadTransparent均`(real AT)`;txn 23得到SW失败(result 1 error 16,真实SW)。EF 4F34/4F36(path `3F007FFF7F665F30`)被路径白名单拒。hw:允许`3F007FFF`下子DF的4Fxx/6Fxx只读。
- **为什么不拨号(观察+推断)**:全程**没有WDS Start(0x20)**。CommCenter从不轮询NAS 0x24,只发NAS 0x03 Indication Register(36次,全被拒,因为带了很多0x13/0x18以外的tag:10 17 19 1e 23 25 26 29 2b a0 c0 d0 d1),NAS 0x4d只问了一次。推断CommCenter因此没有注册状态,不会发起数据呼叫。其余94拒绝:WDS 0x03(44)/0x01(8)、DMS 0x5557(24)/0x01(13)/0x24、UIM 0x24(18)/0x2a(6)、NAS 0x33(16,SET系统选择)/0x5566(15)/0x45/0x4f/0x73/0x34/0x2c/0x3b等。
- **hw修改**:NAS 0x03接受全部tag,0x13/0x18按请求开启,由真实AT快照每10s发indication 0x24/0x4e;其他tag接受但永远不发indication(打印一次)。WDS 0x01/0x03、DMS 0x01直接ACK,不产生indication(打印一次)。**这是放宽了此前"不ACK不存在的订阅"的原则**,理由同上;若因此出现错误行为,可单独回退这一块。
- **hw构建成功**(未上机):同hu参数,`--name x9-rev252hw-md`,boot.img 12922880B,日志`.local/x9/work/pdp-activation/build-hw.stdout.log`(Mac)。无新warning,插件275不变,无需mailbox staging。回退:hv。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252hw-md/boot.img`,日志放`~/162.txt`。看`rg -n 'build=rev252hw|NAS 0x0003 accepted|registration acknowledged|response service 0x3|X9 WDS|X9 MIPC: TX msg 0x0(219|201)|PDP activated' ~/162.txt`。期望:NAS 0x24/0x4e indication开始周期性发送;CommCenter随后发WDS 0x20 Start(会进入X9WDS的MIPC拨号:219→201→205+4302)。拨号即使成功,也会因DPMAIF无<4GiB内存而停在bind前;下一个工程项是physmap不映射空洞,以便拿到<4GiB内存,或改用LK carve-out。

### 前次交接(HANDOFF 93,2026-11-09): 160(hu)读完;RAM 10.0GiB已生效、UIM卡状态通;hv修MIPC版本检查与UIM读文件

- **160.txt**(Mac `~/160.txt`,1257059B/12417行,末49.9s,无panic,shim未报`X9 RAM rejected`)。`build=rev252hu-md`;13.3s HS2→READY。
- **RAM(观察)**:`vm_page_bootstrap: 630787 free pages, 24189 wired`,(630787+24189)×16KiB≈**10.0GiB**(159为8.7GiB)。新segmented表(base 0x100000000/top 0x37FA00000)通过live FDT核对,本次启动正常进入SpringBoard阶段。
- **UIM(观察)**:CRSM改为空data字段后,0x47 SlotStatus与0x2f CardStatus均`result 0 error 0 (real AT)`。CommCenter随即连续发UIM 0x20 ReadTransparent(约30次),全部在`x9q_read_args`被拒(48),当时没有原因日志。**推断**多为length 0(QMI表示整个文件)。hv:允许length 0,由FCP文件大小补全(仍限单次CRSM≤255B);record读length 0取record大小;拒绝时打印原因、session/aid长度、EF id、path、offset、length和TLV tag(不打印内容)。
- **MIPC(观察)**:OPEN_CNF的TLV为`0000=0 0100=0 0101=300000`(result 0、VERSION 0、TIMEOUT 300000)。被我们自己的`version==2`要求拒绝。反汇编libmipc `mipc_init` 0x133a0:VERSION/TIMEOUT只写日志(缺省-1/300000),CNF到达就返回0。hv:只要求result为0;version只打印不检查。
- DPMAIF仍在prepare失败(`no descriptor`),原因同HANDOFF92:XNU没有4GiB以下的页。DMS 0x23/0x25/0x2d成功。
- **hv构建成功**(未上机):同hu参数,`--name x9-rev252hv-md`,boot.img **12922880B**;日志`.local/x9/work/pdp-activation/build-hv.stdout.log`(Mac)。新增代码无新warning。插件275不变,无需mailbox staging。回退:hu。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252hv-md/boot.img`,日志放`~/161.txt`。看`rg -n 'build=rev252hv|X9 MIPC|X9 QMI AT: (UIM|response service 0xb)|X9 WDS' ~/161.txt`,预期MIPC `stage 1 confirmed`→stage 3 `IPv4 dialler ready`,UIM 0x20出现`(real AT)`或带原因的拒绝。

### 前次交接(HANDOFF 92,2026-11-09): 159(ht)读完;RAM扩到9.74GiB allowlist + MIPC/CRSM修复,hu已构建待上机

- **159.txt**(Mac `~/159.txt`,1326389B/13172行,末77.8s,无panic)。`build=rev252ht-md`;14.2s HS2→READY;Sahara finished;摘要`resets 1, PCIControl 0x0`不变。
- **MIPC(观察)**:OPEN的回复头为`84195424 00000000 mask 0xff flag 0x02 id 0x0302 txn 1 len 24`。可见hs是被byte9=0x02隔离的,ht的transport修复生效。但WDS侧`x9w_wire`仍要求`p[9]==0`且mask完全相等,所以`handshake stage 1 rejected/malformed`。hu修复:去掉这两项检查(同libmipc);拒绝时打印TLV tag和4字节值。
- **DPMAIF(观察+根因)**:`DMA alloc of 16384 bytes returned no descriptor`。**真正根因**:shim交给XNU的RAM从`X9_RAM_LO=0x140000000`开始,XNU根本不管理4GiB以下的页。hs是page_grab只能拿到>4GiB的页;ht的contig≤4GiB无页可给。HANDOFF91中"KMA_LOMEM导致"只说对了路径,没说到原因。hu仍然≤4GiB无页,DPMAIF会继续在prepare失败(预期)。另外Wi-Fi注释记录了EMI MPU只允许PCIe DMA到LK carve-out,DPMAIF可能同样受限(推断,未证实)。
- **UIM(观察)**:CPIN已过(进入stage 2/3),但`AT+CRSM`对EF_ICCID/EF_DIR都返回`AT result 1 cme 0`。0x2f的TLV 0x10值为0,已被接受。DMS 0x23(CGMR)现在成功(real AT)。108条reject日志里主要是NAS 0x03/0x33/0x34/0x45/0x4f/0x73、DMS 0x01/0x24/0x45/0x4b/0x53/0x54/0x5557、WDS 0x01/0x03/0x30/0x3e/0xc8/0xcd/0x5564,均为94未实现。hu:CRSM改用MTK RIL的空data字段格式`,,"path"`(libmtk-ril只有`AT+ECRLA=...,,"%s"`,不用CRSM;若hu仍CME 0,下一步改用ECRLA或`AT+ICCID?`)。
- **RAM(用户要求约10G)**:159的`vm_page_bootstrap`为550867空闲+21549 wired页,约8.7GiB。segmented表改为base **0x100000000**(新增0x100000000–0x13CC00000,DT中该段无mblock)、top **0x37FA00000**(到mblock-22 me_gpu_reserved),allowlist合计 **0x26F740000=9.74GiB**(原8.53)。**不再往下扩**:XNU的physmap把整个span(含空洞)按cacheable映射,4GiB以下的空洞含TEE/GZ/pvmfw等secure carve-out,推测会触发投机访问的MPU违规。要真正到10G+并拿到<4GiB页(DPMAIF),需要先在arm_vm_init里不映射空洞。shim启动时仍会扣掉live mblock;若FDT memory节点不覆盖新增段,shim会拒绝启动,并在USB日志开头打印`X9 RAM rejected`和memory tuples,此时回退ht。改动:两份`x9_ram_ranges.h`、`mkadt.py` segmented8g→(0x100000000,0x27fa00000)、`boot-xnu.py`上界。
- **hu构建成功**(未上机):`python3 -I x9/build/build.py all --name x9-rev252hu-md --boot-config .local/x9/modem/boot-rev252hi-md.json --boot-arg x9_dpmaif=2 --boot-arg x9_mipc=1`,boot.img 12918784B,产物元数据dram_base 0x100000000/size 0x27fa00000。插件275不变,**无需mailbox staging**。回退:ht(8.5G表)。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252hu-md/boot.img`,日志放`~/160.txt`。先看开头shim段有没有`X9 RAM rejected`,再看`vm_page_bootstrap`的free页数(预期约+75k页);然后`rg -n 'build=rev252hu|X9 MIPC|X9 QMI AT: (service|response)|X9 DPMAIF' ~/160.txt`,预期出现MIPC `handshake stage 1 confirmed`→stage 3,UIM 0x2f/0x47有`(real AT)`或新的CME号。

### 前次交接(HANDOFF 91,2026-11-09): 158(hs)读完;MIPC/DPMAIF/UIM三处本地错误已修,ht已构建待上机

- **git**:本机main原与origin分叉(本地70个旧提交=远端`ae882f6`已squash的内容),已建`backup/local-main-e0e5178`后`reset --hard origin/main`(2409f8b)。Mac仓库同一提交。
- **158.txt**(Mac `~/158.txt`,1016713B/10200行,末uptime 39.645s;私有副本 `.local/x9/logs/158-20261109-2211/158.raw.txt`(Mac),SHA256 `cd534b3a…c56e`)。446行`build=rev252hs-md`;11文件`same 22 external 1 complete 1 errno=0`;11.904s HS2→READY;19.57s Sahara finished;handshake 10/20s摘要`queries 190 accepted 20, resets 1, PCIControl 0x0`。无panic。
- **观察1 DPMAIF**:11.995s clocks未门控、CODA 0x20171111,随即`quarantined (private TX DMA allocation/range), phase prepare`,其后10/20s摘要ready0。**根因(读XNU源码确认,推断该次落在>4GiB)**:mask`0xffffc000`下16KiB(=page_size)的DRB不算contiguous,`IOKernelAllocateWithPhysicalRestrict`走`KMA_LOMEM`→`vm_page_grablo()`,arm64上`vm_lopage_needed=FALSE`直接`vm_page_grab()`,不受4GiB限制。修复:mask改`0xffff8000`(alignment 32KiB>page,强制`kmem_alloc_contig(max_pnum)`),失败时打印pa/segment。
- **观察2 MIPC**:19.321s TX OPEN 0x0301 txn1,2ms后`framing/event overflow, transport quarantined`;RX统计ch216=1,即MD确实回了一帧,被我们的解析拒绝。反汇编设备libmipc `mipc_init`RX循环(0x13480):READCOM(16)→READCOM(len@0xe),**不检查magic、byte9(flag)、CNF的SIM mask**,按`id>>14`分发(0 CNF按txid,1 IND按mask每位,2 MD命令)。hs要求byte9==0且mask bit0,比参考严格。修复:照libmipc分发;CNF只按txid+id匹配;IND 4202/4302需mask bit0;仅超长帧/事件溢出才隔离并打印原因+16B头;前64帧打印头(不含payload)。**具体是哪一项触发未知**(158没有头日志)。
- **观察3 QMI AT**:178个QMI请求只14条有日志。UIM 0x2f GetCardStatus两次都带1字节TLV(len 11),被`len!=7`静默回48(ARG),CommCenter始终拿不到卡状态。UIM 0x47每5s一次全部error16(NOT_READY),CPIN实际状态不可见(AT脚本日志被X9MD_LOG预算耗尽)。DMS 0x23(CGMR)error3,推断MTK回`+CGMR:`前缀行被`x9q_plain`跳过。DMS 0x25 IMEI、0x2d CFUN成功(real AT)。
  修复:0x2f接受单个TLV 0x10 len1(仍回legacy 0x10状态,**扩展状态格式是猜测**,首次打印该值);所有不进AT队列的拒绝打印`reject ... tlvs`(tag/len,1字节值);CGMI/CGMM/CGMR先认`+CGMx:`行再退回纯文本;AT失败打印stage/result/CME号,CPIN非READY打印分类(不打印AT正文)。
- **ht构建成功**(未上机):`python3 -I x9/build/build.py all --name x9-rev252ht-md --boot-config .local/x9/modem/boot-rev252hi-md.json --boot-arg x9_dpmaif=2 --boot-arg x9_mipc=1`,Image 28766208B,boot.img **12918784B**;日志`.local/x9/work/pdp-activation/build-ht.stdout.log`(Mac)。只有既有C99风格warning。插件275不变,**无需mailbox staging/固件重放**。无CI run。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252ht-md/boot.img`,日志放`~/159.txt`。先看`build=rev252ht-md`;`rg -n 'build=rev252ht|X9 DPMAIF|X9 MIPC|X9 QMI AT|X9 WDS|COACHZ pdp: .*s unit' ~/159.txt`。判断:DPMAIF应出现`initialized`或具体`out of range pa`;MIPC应出现`RX hdr ... id 0x0302`及后续307/308;UIM 0x2f应有`(real AT)`响应或CPIN分类/CME号说明为何NOT_READY。

### 前次交接(HANDOFF 90,2026-11-09): hs的MIPC IPv4 WDS/DPMAIF已编译;真实PDP激活/收发待上机

- 本轮代码/README/构建交接提交 **`0f3aff8`**,交接补记 **`e45ef66`**,均已push `origin/main`(push范围95eb409..e45ef66,含上轮f1048cc);本机main。该提交 `gh run list --commit` 返回 `[]`(证据 `.local/x9/work/pdp-activation/ci-0f3aff8.json`),没有CI run ID,采用上述直接本地构建。最终ADB枚举仍无device(原始私有 `adb-final-devices.private`);未做新设备写入/上机验证。本次push没有覆盖远端额外提交,设备验证仍未完成。
- 已完成提交 **`f1048cc`** (主动DPMAIF分阶段读回/失败保留DMA与WDS能力发现); `x9-rev252hr-md` 直接构建成功:53/53 overlay+6 prebuilt、link/strip/boot成功,Image28745728B、boot.img12910592B。命令 `python3 -I x9/build/build.py all --name x9-rev252hr-md --boot-config .local/x9/modem/boot-rev252hi-md.json --boot-arg x9_dpmaif=2`;日志 `x9/out/x9-rev252hr-md/build.log`、`.local/x9/work/dpmaif-active-157/build-hr.stdout.log`。未上机;最新实测仍hl/157。CI查询为空(本轮证据 `ci-f1048cc.json`),本轮fetch确认origin/main当时比HEAD少这一提交,现已随hs提交push。
- 用户刚接上ADB。本轮/前轮只读定点取证实际vendor RIL库、进程maps/fd/config;证据留在 `.local/x9/work/pdp-activation/`,不进git。运行时 `ro.vendor.mtk_mipc_support=1`, `mtkfusionrild`实际加载libmtkmipc-ril.so/libmipc.so/librilfusion.so,fd打开 **ttyCMIPC1**。不能拿未加载的AT版libmtk-ril或libmipc默认MIPC0代替。源契约MIPC1 queue5/RX216/TX217;DPMAIF channel=ccmni_idx,不是CID-1。
- 本轮实现:独立8槽MIPC mailbox/8槽event/4096B stream,非复用txn、队列5异常/oversized/framing隔离,不打印APN/SIM/IP正文。实际 `libmipc::mipc_init` 0x13324证明先OPEN301(maskff,VERSION100=2,NAME101,USIR102=1),CNF302读VERSION100;原生register API 0x14228证明REGISTER_IND307(mask1,MSG_ID100 uint16)/308(result0)。hs先完成OPEN与DEACT4202/EIF4302两订阅才放行拨号,不是直接发ACT。
- WDS链:GET_PDP_CID219→ACT201(IPv4、显式APN/no-auth、NOT_REUSE2、fallback0)→GET_CALL205 + 本次ACT后真实EIF4302/IFUP→DPMAIF bind。真实ID100 uint8独立于interface125 uint32;ACT12a transIntfId经当前decoder 0x2bd5ec确认,必须trans%100与interface一致,不使用CID-1或旧头netmask。EIF当前回调0x3ad8b0读trans100/cmd101/v4count104/fulladdr8105/v6count106,copy0x3ae170读fulladdr+4地址/+0 MTU;只接受单IPv4/no-v6。IFST/IFUP native configure0x39a4a0路径为本地netdev/ioctl/address/回调;没有凭EIPPORT名字造MD命令。缺匹配EIF/IFUP时不发布hostsuccess,30s后清理已owned call。Settings只给真实IPv4/DNS/MTU/family/APN,未知gateway/netmask/IPv6prefix省略。
- 生命周期:Stop先unbind,真实DEACT203/204确认;后台GET在飞先排空再deact。DEL_CLIENT/超时保留mutation token,lateACT只清理不迟到hostsuccess;原生partial-cause150202/150203/150204有call副作用,本IPv4版解码owned ID后清理。其他失败若含合法ID则隔离不盲deact可能外部context。DEACT IND/EIF接口变化立即关route;周期10s GET读回。malformed/framing/不明deact保留资源直到boot。DPMAIF双向按真实MTU挡包、partial RX route epoch复核,**每boot同channel只bind一次**,旧channel被MD复用时拒绝/清理,不是可任意反复拨号的完整实现。
- **hs直接构建成功**: `python3 -I x9/build/build.py all --name x9-rev252hs-md --boot-config .local/x9/modem/boot-rev252hi-md.json --boot-arg x9_dpmaif=2 --boot-arg x9_mipc=1`。首次52/53因hex token `0x810e+i`词法错误失败,已加空格;首次日志保留 `build-hs-attempt1.{stdout,build}.log`。最终53/53 overlay+6 prebuilt、link/strip/boot成功,Image **28749824B**、boot.img **12918784B**;最终日志 `x9/out/x9-rev252hs-md/build.log`、`.local/x9/work/pdp-activation/build-hs.stdout.log`。仍有XNU/控制面风格及既有窄化warning,不宣称无warning。未额外测试套件或二次解包/hash/反汇编镜像;没有CI build run。
- 本轮新连接检查ADB未列设备(保存 `adb-connected-devices.private`),所以没有新Android写入或新实机PDP证据;此前运行时maps/fd/config的MIPC1证据有效,不把当前离线当作旧候选没启动。插件275和完整11文件外置34MiB固件保持不变,**无需重新放/挂载固件或mailbox staging**。不发Android激活/断开命令、不改既有data session、不flash/reboot/写分区。
- **下一步由用户手动**: `fastboot boot x9/out/x9-rev252hs-md/boot.img`。新 `~/N.txt` 先确认 `build=rev252hs-md`、11文件/Sahara/QMUX/UIM/NAS,再DPMAIF各stage读回ready、MIPC301/302→307/308两次→219/21a→201/202→205/206+4302/IFUP→`PDP activated ... DPMAIF bound`。最后权威iOS地址/路由/DNS和实际TX完成/RX包,**不把中间CNF、逻辑bind、编译成功当联网成功**。最小检索 `rg -n 'build=rev252hs|X9 DPMAIF:|X9 MIPC:|X9 WDS:|X9 QMI AT:|COACHZ pdp:' ~/N.txt`。未实现IPv6/SLAAC/profile/auth/autoconnect/默认APN发现/语音IMS;Apple若不传显式APN或带额外未支持TLV将明确失败,不得硬编码SIM/APN。README已同步能力/验证门。

### 前次交接(HANDOFF 89,2026-11-09): DPMAIF实验后端/WDS前置路径已编译;hq仅探针,无真实会话成功

- 初版代码/构建交接提交 **`bb3903b`**,hq PIT修复提交 **`ccbad81`**,均已push `origin/main`;两次`gh run list --commit`均返回`[]`,本次无CI run(证据`.local/x9/work/dpmaif-wds-157/ci-{bb3903b,ccbad81}.json`)。用户要求继续DPMAIF收发与WDS数据会话。本轮从`3ef5398`/干净main继续,没有ADB、flash/reboot、设备读写/挂载,没有新实机结论。最新实测仍hl/157;hn保留。**本轮不是WDS/PDP或蜂窝网络已打通**,不能将编译成功或逻辑Bind成功视为数据面成功。
- 补全上一轮只读契约核对:原生`mtk_ccci_common.h:349–350`的LRO=bit4、2RXQ=bit5,故live cap0x7a确有两者;`clk-mt6993-ifrao.c`匹配live的`mediatek,mt6993-apifrbus_ao_mem_reg`,0x14126000的gate status/clear=0xd20/0xd28,main bit2、26MHz bit3。真实DPMAIF bases按`dpmaif_init_register`和`ccci_dpmaif_drv3_init`派生,MT6993 TX高地址写独立CON2、rdidx用UL SRAM0xe0高16bit/2。channel仍是ccmni索引,**现有源码/所存50KiB mtkfusionrild程序没有给出CID→ccmni/会话契约**,不得猜CID-1。参考根路径`~/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12/`。
- 新`X9DPMAIF.inc`接替send=-1 stub:网络线程仅向64槽/3072B mailbox复制,单bridge owner作MMIO、DMA发布/完成。独立`IOBufferMemoryDescriptor`物理连续分配,mapper-none物理segment长度/16KiB对齐/0x40000000..0xffffffff范围验证;不使用Wi-Fi保留池或编造Linux动态reserved节点地址。独立DRB2048、TX pool、BAT1024x2/PIT2048x2、1664/1920B缓冲;cache-line维护+barrier。
- **收发代码仅编码/编译,未硬件验证**:TXQ0 msg+payload DRB、word-count doorbell、高地址CON2、read-index/已发布范围回收及route epoch隔离;RX v3 sequence、h_bid、channel、header_offset×4、normal/fragment重组、地址/MTU检查、最终cbit0才回调。BAT共用、顺序补充同时要求硬件消耗+两路RX释放,不按每个PIT盲目重发。异常/超时/索引或所有权错误隔离保留全部DMA,不reset/force-assert/free可能在飞buffer。RX只支持关闭聚合后的单normal+fragments;若收到多个normal/LRO聚合就保留故障并隔离,**没有完整LRO/GRO实现**。
- `x9_dpmaif=0`默认关闭;**hq显式=1**,仅读时钟gate,门开才读DPMAIF状态,分配私有DMA,不写任何硬件寄存器/doorbell/SRAM、不publish描述符、ready保持0。=2代码为**未验证实验初始化**:等真实AT owner online后使能时钟、6993 P2/SRAM_INIT_v1 bounded wait、双PIT/BAT/TXQ0配置、关闭16路aggregation、INIT_DONE;不写旧AP bus-remap,两IRQ保持mask并poll。**本轮没有启用=2;后续启用SRAM/queue初始化需先用户确认,不能直接把hq改成active上机。** parent mux/MD-side DMA remap、队列读回/实际IRQ及非聚合双RX行为仍待实机验证。
- X9编译定义`COACHZ_NATIVE_DPMAIF`仅作用于此board:shared bridge不再创建内部IPA DPM/WDA配置client,不把Apple endpoint3/interface4改写成embedded4/interface1,不要求QMAP;原Qualcomm默认路径不变。host WDA/DPM仍明确不支持,没有伪造open-port或format成功;native的CBD_IDLE不是初始化成功。
- 新`X9WDS.inc`:支持WDS0xa2逻辑mux绑定(仅mux-only或shim的PCIe3/interface4)、0x4d family4/6;这是host意图,不会调用物理route bind。0x22串行真实`AT+CGACT?`、逐行CID/状态去重/范围解析,返回该client无自有bearer的disconnected,即使MD有active context也不冒充它。0x20 Start校验APN字符/长度、family、拒绝未实现profile/auth/凭据/autoconnect转换,真实CGACT preflight后在DPMAIF-ready或CID/ccmni未证明门失败,**不执行CGDCONT/CGACT/CGDATA激活、不生成handle或IP/DNS/MTU**。0x21未知handle返回9,0x2d无自有call返回15;starting锁、总deadline/取消/DEL_CLIENT generation与上一轮迟到final排空屏障保留,删除不对unowned MD context做CGACT=0。
- 源码复核又修了8位sequence绕回:2048项PIT的下一整圈会复用相同seq,不能只看旧seq;初始每项按(i^0x80)&0xff毒化(固定0xff会撞255/511等项),每完整64B cache line四项均消费后才xor0x80重新毒化、cache维护并归还credit,跨poll最多保留3项。避免碰相邻在飞DMA项,仍未硬件验证。ho初稿、hp开发构建均保留但不作为本轮上机候选;最终使用**hq**。
- **直接构建成功,未额外测试/二次校验镜像**: `python3 -I x9/build/build.py all --name x9-rev252hq-md --boot-config .local/x9/modem/boot-rev252hi-md.json --boot-arg x9_dpmaif=1`。53/53 overlay+6 prebuilt、link/strip/boot均成功;构建输出Image **28745728B**,boot.img **12906496B=12.30859375MiB**,20MiB guard不变。日志`x9/out/x9-rev252hq-md/build.log`及`.local/x9/work/dpmaif-wds-157/build-hq.stdout.log`。有XNU/原控制面风格警告及`x9d_bind`未使用警告(正是本版没有可信物理route producer),不声称无warning。没有手动触发CI run。插件275、完整11文件外置34MiB manifest保持不变,没有传revision或重建/stage插件,**不用再放/挂载固件或mailbox staging**。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252hq-md/boot.img`。新日志先看`build=rev252hq-md`、11文件/Sahara/QMUX/UIM/NAS;再`X9 DPMAIF:`的gate、私有DMA分配/范围通过与mode1/ready0/published0;`X9 WDS:`/`X9 QMI AT:`的0x22真实CGACT解析及Start失败门。TX/RX0是探针模式预期,不是成功。最小检索`rg -n 'build=rev252hq|X9 DPMAIF:|X9 WDS:|X9 QMI AT:|COACHZ pdp:' ~/N.txt`。不用重复156/157缓存取证。
- **剩余实现明确**:先取得真实MTK RIL/MIPC的PDP创建/绑定(CID、ccmni)、CGDATA/CGCONTRDP错误与地址/DNS/MTU语义证据;不能拿所存launcher替代实际RIL库。证明数据硬件INIT/内存地址可达后才扩展Start/Stop回滚、客户端删除/late-OK清理、自有handle、settings/断线indication和物理route producer,再放行iOS接口地址/路由/实际包收发。语音/IMS不在本版。

### 前次交接(HANDOFF 88,2026-11-09): AT→QMI控制面候选hn已编译;尚未上机,当时DPMAIF/语音未实现

- 代码与完整后续流程说明提交 **`91c897e`**,当前`main`。`x9/baseband/README.md`已替换过时eq/12文件/bindfs说明,记录hn能力边界及SIM→DPMAIF→WDA/DPM/WDS→iOS IP/路由/收发→语音/IMS的逐步成功门。
- 用户要求“把后续的流程全写完”,本轮将原error94 stub扩展为有界异步**真实AT控制面**并补齐后续流程说明。**不是全部蜂窝已完成**;DPMAIF、WDS数据会话、语音/IMS尚缺,没有伪造WDA/DPM成功。实测基线仍hl/157。
- 新增`x9_modem_bridge.h`/`x9_at_bridge.inc`和`X9QMIAT.inc`:单MD owner收发ttyC0,16槽AT邮箱,32槽QMI job(请求最多1024B,总deadline180s,单AT10s)。完整终止行才完成,2048B溢出继续排空至final后报错;缺final或TX失败隔离到下一次boot,不让迟到OK配错新请求。CCIF TX在邮箱mutex外;取消已发请求只标记,仍排空final。exception/offline清active,不重用stale slot。`x9_bb=1`持续监听,计时改为mach absolute elapsed;桥开启时统一串行level3初始化,CFUN1之后才接收job,AT正文及ch10包内容不进USB/pstore。
- **已编码、未实测**:UIM CardStatus(0x2f)用CPIN READY→CRSM GET RESPONSE真实EF_DIR/FCP→逐record解析真实AID;SlotStatus(0x47)读真实EF_ICCID。ReadTransparent/ReadRecord(0x20/21)先核对session/file/path/offset/length→真实FCP→CRSM读取→真实SW1/SW2+数据。仅empty-AID primaryGW/current USIM或card1 MF路径,不冒充slot2、不支持任意AID选择/非provisioning路由。未知PIN状态为not-initialized,固定retry字段为不可用而非宣称某个真实重试预算;未知personalization也不宣称READY。显式CME10可报告卡不存在。其余锁卡/未知错误不猜状态。
- NAS ServingSystem(0x24)/SystemInfo(0x4d)串行查询CREG/CEREG/C5GREG/CGATT/COPS;SignalStrength(0x20)另读CSQ,仅已知RSSI,不伪造RSRP/SNR/小区/PLMN。DMS manufacturer/model/revision/IMEI/CFUN/ICCID/IMSI来自真实CGMI/CGMM/CGMR/CGSN/CFUN/CRSM/CIMI。CPIN/CRSM/这些扩展AT命令的组合**尚未实机证明**,解析不匹配/不支持明确失败,不是拿157的旧CPIN观察替代新QMI成功。
- UIM event mask仅card/slot(bit0/4),NAS subscriptions仅serving/system;串行后台快照周期至少10s,前台队列忙时延迟。没有ACK不存在的signal/refresh/eSIM订阅。QMI TLV长度、重复tag及未知读参数被拒绝;SIM写/PIN/PUK/APDU/文件ACL属性、DMS mode-set、NAS网络选择、WDS/WDA/DPM仍不支持。原桥本地activation成功仍非真实激活/联网。
- QRTR client复用修复:`coachz_bb_qmux.h`为每次allocation保留64位generation;`bb_tx`/held UIM复制generation并丢弃过期job;DEL_CLIENT callback完成前release_pending维持2、不释放port。X9 owner取消该port全部job和subscription后才可复用,避免旧事务/迟到回复进入新client。
- **直接交叉编译成功**:首次hm通过后源码审查发现CFUN4误映射QMI RESET及后台失败indication问题,已修正;hm仅保留为开发产物,**不要上机**。最终**`x9-rev252hn-md`**: `python3 -I x9/build/build.py all --name x9-rev252hn-md --boot-config .local/x9/modem/boot-rev252hi-md.json`,53/53 overlay+6 prebuilt、link/strip/boot均成功;构建输出 **Image28745728B,boot.img12898304B=12.30078125MiB**,20MiB guard未改。构建日志`x9/out/x9-rev252hn-md/build.log`及`.local/x9/work/qmi-at-157/build-hn.stdout.log`;仍有XNU/风格及一处已界定为<=999的MNC整数窄化warning,不宣称无警告。无CI run、无额外测试套件、无镜像二次解包/hash/反汇编,无设备写入/ADB/启动。
- 完整11固件已独立34MiB stage,沿用原manifest/插件275;**无需放/挂载固件,无需mailbox staging**,没有传revision重建插件。hn沿用hi/hk/hl全参数。用户手动`fastboot boot x9/out/x9-rev252hn-md/boot.img`,回退hl路径不变。
- **下一步验证顺序**:新日志独立目录固定前缀,先确认hn marker/11文件/qdsp/Sahara/QMUX,再看UIM0x2f/0x47/0x20的`X9 QMI AT: response ... result 0 error 0 (real AT)`、真实AID/文件结果是否被CommCenter接受,再NAS/DMS。失败回到最早stage解析/MD响应,不自动反复尝试PIN/APDU。不以构建成功宣布SIM或蜂窝实测成功。后续DPMAIF必须按MT6993 v3真实DT/reserved-memory/DMA契约实现TX/RX/中断与回收,之后才允许WDA/DPM/WDS会话及iOS IP/路由/收发验证;语音还需IMS/音频链路。

### 前次交接(HANDOFF 87,2026-11-09): 157确认完整BBFW/qdsp和Sahara→QMUX推进;新阻塞为QMI不支持,真实PDP仍未通

- 用户手动启动后要求读`~/157.txt`。本轮仅读取日志及源码/本地参考,未ADB操作、flash/reboot/挂载/设备写入,未构建/CI/额外测试,未二次检查镜像。代码仍`20c64f2`,前轮文档`0f9d983`;仅更新本交接,原始日志不入git。
- **日志仍在追加,固定证据前缀而非假称整场结束**:20:30:36.842+0800捕获 **1361492 B / 13639行**,最后uptime **82.378 s**,完整末行。私有目录`.local/x9/logs/157-20261109-203036/`保存`157.raw.txt`、`capture.json`、带原行号`events.txt`、`qmi-events.json`;SHA256=`f9c004665c35a6854eeeab10fc1b971cf26473ca661e4a2b33945df54bedfc60`。下述行号均指此固定raw副本;后来新增内容不能冒充本次已分析的前缀。
- **完整固件落地已实测**:157:446/1.284s标记`build=rev252hl-md`;2713/10.344s=`files 11 in 2 paths written 1 linked 1 same 20 external 1 complete 1 result errno=0`。4683–4684/19.946–19.953s实际stat/open `qdsp6sw.mbn`均errno=0,不再是156的ENOENT。没有另行挂载固件,不用重stage固件或插件275。866/3.247s保留空间降到16MiB,当时free **131116KiB**,是安装qdsp之前的读数,不要当作启动末尾空闲空间。
- **决定性推进链**:20.108s匹配pbl;20.146s兼容桥接收并丢弃238208B的SendImage;21.046s Sahara HELLO;5011–5012/21.075–21.079s=`image 1 of 1 id0x15,10 requests,226532 bytes`并`finished/status0`。21.108/21.135s打开qmux1/qmux2。10–50s五次摘要均`protocol queries 95 accepted 10,user clients16 failed0,resets0,Sahara state6(CBS_FINISHED)`;156为queries0/requests0/reset最终15。**证明越过缺文件的启动前置门,不是Apple固件刷进MTK**;`PCIControl methods0x0`未变,不称PCIControl已有调用。
- **新窄闭环证明适配层错误**:5050–5052/21.196–21.204s内部DPM(0x2f)msg0x20 txn1发出→`X9 BB: QMI request to DPM`→同txn/id响应error0x5e;WDA(0x1a)msg0x20 txn2/3同样失败。5059/21.232s明确`DPM open error0x5e; WDA set data format error0x5e,link/uplink/downlink=-1`。NAS/UIM/DMS/WDS也实际请求/回复0x5e。当前`X9Baseband.cpp:59–71`的注册send回调确实无条件调用`x9bb_reply_error(...,94)`;94=0x5e,是本地X9适配stub返回,**不是原生MD报不支持QMI**。私有Apple QMI不支持另由shared bridge处理,不可混为同一个根因。
- **中间成功不能当数据面成功**:`data port step3`是`CBD_DONE`(尝试结束,含失败),`1 activations`是桥本地回答activation register,且ICCID/IMSI均`not read yet`。10–50s各PDP unit0..3摘要全`families0 blocked1 tx0 rx0`;qmux1到20s报告已收431帧41195B/发432帧8612B,qmux2收14帧228B/发14帧266B,其后到50s未再推进。QMUX流量和本地UDP计数不是蜂窝IP数据。
- **原生MD仍可用,但iOS未接到这些真实状态**:13.553s HS2→READY;19.797/27.699s CPIN READY;34.524s C5GREG=2,1而CEREG=2,0;52.458s CEREG=2,1而52.687s C5GREG=2,0;52.915s CGATT=1。仅是先后注册/packet attach观察,不证明iOS拿到地址/路由或可通话。该前缀没有`fPhases`/`BBUpdater error`文本且摘要reset一直0;**未读取157之后的落盘BBUpdater日志**,不要声称逐条复核了最终reset原因或所有error已消失。
- **下一步建议只做一个真实状态闭环**:先核对UIM slot/card status实际TLV与现有串行AT/CCCI读取,将真实CPIN/SIM状态传给CommCenter(必要时最小SIM文件只读,标识不打印/入git),再逐步NAS注册/信号和DMS;WDA/DPM/WDS需真实数据面契约,不能统一伪造成功。已先检索本地`.local/x9/work/references/{UniXNU,hackios-xnu-kernel}`相关baseband/modem文件,没有匹配的MTK QMI→AT现成实现。未修改适配stub,未生成新候选。根盘空间风险仍在;157有assetsd persistent-store open error及其他daemon崩溃,未证明与本次qdsp写入有因果关系,不因此自动删除根盘日志。
- 最小只读复核(仓库根):`rg -n 'X9 BBFW|qdsp6|handshake .*s:|sahara (image read|finished)|data port:|COACHZ pdp: .*s unit|QMI request to' .local/x9/logs/157-20261109-203036/157.raw.txt`。新的设备取证必须独立157目录/identity,不得复用156缓存。

### 前次交接(HANDOFF 86,代码20c64f2/构建交接0f9d983): 用户批准的34MiB独立BBFW已暂存并回读校验;hl小boot BUILT

- 用户回复“好”,明确批准前轮询问的专用文件创建和file-only extent写入。本轮只新建 `/data/local/tmp/x9-ios-pan92-20261027-1215/baseband-Mav17-20H392.pack`(**34MiB**,12个extent),F2FS pinned且immutable;未解除任何旧文件保护,未flash/reboot/挂载,未向原根盘/插件/磁盘日志写raw数据。
- 新增受控工具 `x9/tools/stage_baseband_firmware.py`:必须 `--create --yes` 或用准确捕获状态 `--resume --yes`,默认拒绝复用旧文件;每次raw写入前复核inode/保护/FIEMAP,范围最多256KiB,先读旧hash→pinfile比较后写→同范围读回hash,无效首块→payload→最后发布首块。完成后验证全包rawhash,原root/plugin/log文件inode/map及root首64KiB hash不变,再发布新私有manifest。操作用法及失败恢复约束见 `x9/BUILD.md`。
- **遇到的实际保护冲突及解决证据**:首次create已成功,在payload写入前发现4条旧protected记录重叠而停止。它们来自 `.local/x9/work/x9-full-root/20261026-222518/native-root-manifest.json` 对应的历史 `/data/local/tmp/x9-ios-full-20261026-222518/iosroot-full.hfs`;只读检查确认原路径不存在,历史manifest的device/partition身份一致。实时FW map与当前root/plugin/log map均无交集。用显式 `--retired-reservation-manifest` 将重叠 **2560×4096B=10MiB** 改归新firmware reservation,未重叠部分保留;旧protected从4265条变4262条,新FW另有12条。**覆盖检查证明所有原排除字节仍由残余protected或FW map保护,没有缩小旧排除区;原manifest文件未修改。** 不可把这个特例当作忽略未知/live重叠的许可。
- 首次失败前未持久化已分配inode的工具缺陷也已修正:现在先保存allocation identity再做overlap判定,resume同样重做全部ownership检查。本次失败分配通过只读捕获恢复,FIEMAP与首次冲突捕获完全相同,保存首次provisioning副本和恢复步骤,然后resume同一个新文件;没有重新分配/删除文件。
- **成功时间20:17:51+0800**:完整payload **34130291 B**,block padding后 **34131968 B**;raw SHA256=`d8a6a591b763f84d421123e78606d07c04e9dd566c5b5130b4d6013e421211b6`,与打包元数据相同。整包rawhash及新root+FW preflight通过。`initial_content_verified=true`,只是固件暂存成功,尚无新iOS运行结果。
- 私有状态/证据: `.local/x9/baseband/staging-20261109-rev252hl/` 的 `provisioning.json`、`operations.jsonl`、`existing-files-{before,after}.json`、`allocation-android-extents.json`、`retired-source-path-status.txt`、`reservation-reclassification.json`、`payload-rawhash.txt`、`preflight-{before,after}.log`及只读恢复脚本;含设备标识,均不入git。新权威manifest为同目录 **`root-manifest.json`**,旧pan220 manifest保留。
- `x9/build/local.json` 已切换 `root_manifest` 到该新manifest、`baseband_firmware_storage: external`,去除exclude/incomplete/large-boot开关。修改前后副本为同目录 `local-{before,after}.json`。仍使用原始Mav17 bbfw,插件allowlist **275未改**,**无需插件mailbox staging**,但本次独立BBFW staging已完成。
- 同步修正 `x9_modem.inc` 的硬编码build日志标记为 `rev252hl-md`(仅标签,没有改MD协议逻辑),避免像156那样把遗留hj标签误认为启动错镜像。
- **构建成功,代码提交20c64f2**: `python3 -I x9/build/build.py all --name x9-rev252hl-md --boot-config .local/x9/modem/boot-rev252hi-md.json`。没有传revision,没有运行插件构建;本机ccache交叉编译 **53/53 overlay对象(另6 prebuilt)**、link/strip及boot打包均成功。原38个boot-arg和options沿用hi/hk,固件描述与已stage manifest的payload大小/hash内置匹配检查通过。构建自己的输出=`Image 28708864 bytes; shim-boot.img 12877824 bytes; entry 0x1000; PSTORE MIRROR`,20MiB boot guard未放宽。未二次解包/重算hash/反汇编镜像,未运行额外测试套件,未手动触发CI。
- 构建日志:`x9/out/x9-rev252hl-md/build.log`及私有staging目录 `kernel-boot-build.stdout.log`;内核旁 `baseband-firmware.json` 是完整external描述,搬内核时必须一起保留。插件allowlist与git保持一致,没有重新打包/写mailbox。hk回退镜像保留。
- **下一步由用户手动**:`fastboot boot x9/out/x9-rev252hl-md/boot.img`。固件已stage,用户不用再放文件或挂载;候选PID1才会把缺少的qdsp写进HFS并为另一固件路径建硬链接,**当前Android状态不声称根盘qdsp已安装**。新日志需验证11文件/`external 1 complete 1 result errno=0`、qdsp可读、本次落盘reset原因以及PCI/protocol/Sahara是否推进;不得用暂存/构建成功代替基带功能成功。新启动采集必须新目录,不复用156缓存。

### 前次交接(HANDOFF 85,代码6f5c194/交接872dba5): qdsp 是启动必需项;外置完整固件代码已交叉编译

### 证据与修复方向

- 实机输入仍以156和84的定点采集为准:原10文件包缺 `qdsp6sw.mbn`(155/156 stat ENOENT);efs1.bin 已有1 MiB,本次15条reset全为 `fPhases.size()`。**纠正旧推断:加载阶段允许跳过,不等于启动阶段可省略。** 本地参考 UniXNU/hackios/QcomBaseband 没有可直接移植的 phase 修复;QcomBaseband 的完整 Apple 固件是独立输入,不能把其 iOS17/Mav16 分支补丁套到20H392。
- 当前 `libBBUpdaterDynamic.dylib` 的确定分支链(静态):
  1. `BBUEUR10UpdateSource` 目录构造函数 `0x1ee1ebcfc`:向量零寄存器由 `0x1ee1ebd4c` 设置,`0x1ee1ebda0` 清零 `this+0x108` 等字段。
  2. `init()`=`0x1ee1ec160`:字符串 `0x1ee2416f3` 是 `qdsp6sw.mbn`;`0x1ee1ecb10` containsFile,缺失在 `0x1ee1ecb20` 跳过,仅存在时在 `0x1ee1ecbb0` 填充 `this+0x108`。
  3. `validateBootupItems()`=`0x1ee1f1aac`:在 `0x1ee1f1af4/af8` 检查该字段,为0则返回 **35/0x23**;字符串 `0x1ee24162c`=`Baseband FW isn't complete. You are missing some images!`。EUR10/EUR13的vtable slot+0x50均指向此校验(提取dyld指针须去链元数据,不能直接用原始数值)。
  4. `bootupInternal()`=`0x1ee1731dc`:校验调用在 `0x1ee173210`,失败从 `0x1ee173218` 分支到错误事件,**早于** `handleBeginPhase`(`0x1ee173228`)和 first-stage query(`0x1ee173234`)。错误路径仍到 `0x1ee173294..2a4` 调反馈 endPhase;`handleError` 也有无条件 endPhase。
  5. `BBUConsoleFeedback::handleEndPhase()`=`0x1ee19e2a8`:空 phase 在 `0x1ee19e2d4/2d8` 转异常,行号93(`0x1ee19e594`),字符串=`Assertion failure ( fPhases.size() )`。
- **证据边界**:没有运行日志直接打印原始 error35/“missing images”;这是运行中缺文件＋当前二进制的必失败分支推导,与零PCI/Sahara及最终断言吻合。补齐文件后能否继续握手仍须新启动确认;不绕过断言、不替换Apple二进制、不声称蜂窝已可用。
- 汇编/脚本在 `.local/x9/work/bbu-phase-156/`(`update-source-ctor.s`, `update-source-init.s`, `validate-boot-items.s`, `bootup-internal.s`, `handle-error.s`等)。前轮补读 CrashReporter/Baseband、abm.workspace/pending 为空,本次启动的uncompressed子目录不存在;不要重复探这些空目录。补读增加32 KiB后,156的 `ranges/` 合计 **8577024 B**,仍未全盘读取,原有块未覆盖。

### 已完成的本地代码与编译

- `baseband_firmware.py`:新增 `--storage external` 和独立 `--header/--metadata`;完整11文件压缩包不进入 `.incbin`。内嵌包上限 **4 MiB**,排除文件必须显式 `--allow-incomplete`,外置模式禁止排除。保留原20MiB boot guard,**不能重试47MB的rev252eq黑屏路径**。
- 原始完整包已本地生成: **34130291 B**,11文件原始合计64543521 B;路径 `.local/x9/baseband/external-156/{Mav17.pack,x9_baseband_firmware.inc,source.json}`。按现有pinfile只接受偶数MiB,需独立 **34 MiB=35651584 B** 文件。原bbfw、旧10文件包和hk镜像保留。
- `sweet_x9_ufs.c` 新增只读 `sweet_x9_bbfw_read`:使用独立ADT extent表,核对容量/边界、自重叠及root/plugin/log/保护区冲突;不复用插件mailbox,无固件区写API。`ufs_root_manifest.py` 新增 `firmware_policy` 验证和ADT属性,固件extent自动加入root写入排除区。`check_plaintext_root.py` 对该可选区域核对live inode、pin+immutable、FIEMAP→raw map和整个payload SHA256。
- `x9_baseband_firmware.inc`:整包读取到RAM,先验证整包及所有解压文件hash;两处固件路径中优先复用已有正确文件,缺失时只写一份,第二路径hardlink并验证。不同现有文件/符号链接保留,link失败不复制第二份。**不挂载固件目录。** 目前空闲126.15MiB,qdsp一份59326752 B;仅按数据大小推算剩约69.57MiB(未计其他启动写入),而两份会只剩约13MiB,低于16MiB reserve。
- `build.py`:每个新内核旁保存 `baseband-firmware.json`,外置模式打包boot时必须与私有root manifest的payload大小/hash匹配;未真实分配的extent不能编造。用法见 `x9/BUILD.md`。
- **交叉编译结果**:external模式 `kern_exec.o`、`sweet_x9_ufs.o` 成功;embedded诊断模式 `kern_exec.o` 成功。首次编译发现 `kfree_data` 宏要求lvalue,已改为局部指针后重编成功;仍有XNU现存/风格警告,不是无警告构建。Python四文件语法编译成功,未运行额外测试套件、未完整link/打包、无CI run。
- 重放本地编译:`python3 -I .local/x9/work/bbu-phase-156/compile-prep.py`。日志 `compile-prep.log`(首次失败)、`compile-prep-2.log`(成功)、`compile-{external,embedded}/build.log`、`compile-results.json`。脚本结束恢复新版格式的旧10文件generated header;**设备manifest和local.json未改**,当前local.json仍排除qdsp,新的正常build会要求显式诊断开关或切到完整外置配置,不要默默加开关继续测缺文件包。

### 下一步(需用户确认后才操作设备)

1. 本轮从基线 `8c211b4` 开始;代码/构建说明提交 **`6f5c194`**。没有新的设备写入/flash/reboot/挂载,**没有可上机的新boot**。插件仍275,不重建/覆盖Metal mailbox。
2. 待用户确认:只新建 `/data/local/tmp/x9-ios-pan92-20261027-1215/baseband-Mav17-20H392.pack` 这一 **34MiB** 专用文件,保留root、插件mailbox、磁盘日志及所有分区备份。先现有root preflight;`pinfile create` 必须O_EXCL新建,随后设immutable并采集真实inode/FIEMAP,不解除任何旧文件保护。
3. **尚未编写/运行设备staging脚本**。下一代理需沿用 `pinfile rawreadat/rawwriteat` 的旧hash比较、分块回读流程,远端rawreadat带 `2>/dev/null`;只写新文件extent,先无效头→payload→最后发布首块。完整payload rawhash成功后,在**新的私有manifest副本**加入firmware_policy,旧root manifest保留。不能对Android普通文件写入来替代raw plaintext staging。
4. 新manifest通过root+固件preflight后,local.json改 `baseband_firmware_storage: external`,去掉exclude,用新manifest;保留hi/hk的完整boot配置、插件275,编内核/小boot。设备仍由用户手动启动,不自动reboot/flash。此阶段无新插件包,**不需插件mailbox staging,但需上述独立BBFW staging**。
5. 新日志需看到完整11文件、`external 1 complete 1 result errno=0`、qdsp实际可读及本次落盘reset原因;再看 `fPhases` 是否消失、PCIControl/protocol query/Sahara是否开始推进。全流程未验证前不说基带修好。新启动的定点采集必须新目录,不得拼接156缓存。

### 前次交接(HANDOFF 84,提交8c211b4): ADB 定点取证仅保存8.15 MiB;efs1.bin已有1 MiB

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
