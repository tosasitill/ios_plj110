## HANDOFF 2026-11-08 (56): 103.txt(rev252cq 探针)= 123 命中正确、动作正确, 但 setKeyplaneName 后键盘面不变; rev252cr(探针第二轮, plugin 271)构建中

- 实测 103.txt: KBDHOOK 在 Spotlight[274] 36.5 s 装好(其余含 UIKeyboardLayoutStar 的进程也装上, 无副作用)。
  Spotlight 键盘布局 418.7x243 pt, 面 `small-letters-small-display`, More-Key `it=11 dt=18 visible=1 state=2
  frame=2.0,175.0 51.7x49.0`。所有 16 次 123 按压: `hit 'More-Key'`、`downActionFlags=0x24`、`sendDown
  actions=0x24 infos=1` —— 命中/可见/状态/计数全部满足, 但 `after sendDown plane='small-letters-small-display'`
  不变。表情键切 `17613_PortraitTruffle_iPhone-Emoji-Keyboard_Letters` 正常, 地球键 0x1085 正常。
  → 104 之前的推断(命中失败/不可见/禁用/计数)全部**证伪**; 问题在 More 分支内部或 setKeyplaneName: 自身。
- 实测(反汇编 setKeyplaneName: 0x188ff97c0): 第一道门 `_isTrackpadMode`(BOOL, ivar 偏移指针 0x1e1d8e9cc)非零
  → 直接返回。trackpad 由 setTrackpadMode:animated: 写, 调用者: willBegin/didEndIndirectSelectionGesture:
  (来自 _UIKeyboardTextSelectionInteraction panningGesture:/oneFingerForcePress:/oneFingerForcePan:)、
  will/didEndTrackpadModeForServerSideDictation(UIDictationController clearKeyboardTrackpadModeIfNeeded /
  UIDictationView finishReturnToKeyboard 调 didEnd)、didRotate。推断(未验证): trackpad 卡在 YES 会让切面静默
  失败; 但 performHitTestForTouchInfo 也读这个标志而命中正常, 所以也可能是 More 分支里 alternateKeyplaneName/
  subtreeWithName/ShiftRendering==31→showGestureKeyboardIntroductionIfNeeded 这几步。
- rev252cr 探针(commit 8e198db): More 分支打印 trackpad/rebuilding/isShiftKeyPlaneChooser/keyboard 名/alt 名/
  alt 子树是否存在; 每次 setKeyplaneName: 打印参数与前后值; setTrackpadMode:animated: 打印参数与 UIKitCore
  去 slide 返回地址(可用 ~/x9-kbd-dis/dis.py 符号化)。字母键不再打 downActionFlags; 行数上限 800。
- 构建: Mac `/tmp/x9-rev252cr.sh`(bash 数组 + Framework python + homebrew llvm PATH), `build.py all --revision 271
  --name x9-rev252cr`, 参数同 rev252cq。plugin 变了 → 先 mailbox 重写再 boot。
- **BUILT(Mac, 未上机)**: x9/out/x9-rev252cr boot_sha256 **31cc737ff3694bb211d36f594f11c2ec3256bcf99cca6849bca75fa3805d340b**,
  payload b924f3cb99e30a79...; allowlist(plugin 271)已提交。

## HANDOFF 2026-11-08 (55): 123 键诊断探针 x9kbdhook=1 (plugin 270) → rev252cq 构建中

- USER(102.txt 后): 按住 123 **不变暗/不高亮**; 123 键不是灰的; 地球键能切、删除能删、旁边表情键能点;
  **所有语言的数字面都打不开**。
- 推断: 不高亮 = touch-down 时 UIKit 没把这次按压当作 More 键的动作: 要么命中测试没落在 More 键
  (邻近表情键正常, 不像整体坐标偏), 要么 More 键 visible=NO / state==1 → downActionFlags=0。反汇编排除:
  stateForKeyplaneSwitchKey: 只返回 2/4; shouldMergeKey: 只管 interactionType 38/9/5(More=11);
  _shouldSwapGlobeAndMore 需两键都 visible; setKeyplaneName: 的 setVisible 循环只置 visible=1。静态到此为止。
- 新探针(commit 7dce1e5, x9/metal/General/X9KeyboardProbe.inc, 由 X9GeneralMetalFactory.mm include): bundle
  constructor 在含 UIKeyboardLayoutStar 的进程里(boot-arg 令牌 x9kbdhook=1)换 3 个 IMP, 先调原实现、结果不改:
  `X9 KBDHOOK <proc>[pid]: touchDown at x,y (layout WxH) plane=... infos=N hit '<key>' it= dt= visible= state= frame=`,
  `  More-Key ...`, `downActionFlags=0x.. key ...`, `sendDown actions=0x.. infos=N key ...`, `after sendDown plane=...`。
  直接写 /dev/console, 400 行上限。注意: bundle 是首次 Metal 使用时才加载(102.txt Spotlight 59.6 s), 之前的触摸
  不会被记录 → 测试时先在 Spotlight 里打几个字母、等几秒再点 123。
- 构建(Mac, 后台): `build.py all --revision 270 --name x9-rev252cq` + rev252cp 的 16 个令牌(去掉 x9_mgwipe)+
  x9kbdhook=1。plugin 版本变了 → **必须先 mailbox 重写**: `X9_SERIAL=<serial> python3 x9/mailbox/update.py
  x9/out/x9-rev252cq`, 再 `fastboot boot x9/out/x9-rev252cq/boot.img`。
- **BUILT(Mac, 未上机)**: x9/out/x9-rev252cq — boot.img **64a73e39a64c12692efc3b313f424eeac1ceda62ba9892ab4b547cd81551c93a**,
  kernel f97394422348d619..., payload b5c461a6ef18b8f9...; 嵌入参数核对 = 上述 16 令牌 + x9kbdhook=1; X9GeneralMetal270
  内含 `X9 KBDHOOK` 串。allowlist 头随 b334009 提交。构建坑: zsh 不拆 `$A`(用 bash 数组脚本 /tmp/x9-rev252cq.sh);
  boot 步需 `PATH=/Library/Frameworks/Python.framework/Versions/Current/bin:/opt/homebrew/opt/llvm/bin:$PATH`
  (all 步因缺 llvm-objcopy 停在 boot, 用 `build.py boot --kernel x9/out/x9-rev252cq/kernel.development.sweet` 补)。

## HANDOFF 2026-11-08 (54): 101.txt(rev252cp) 相册/时间/机型 USER 确认修好; 键盘 123 仍打不开 → UIKit 门控已定位(静态), assetsd 假说证伪

- 实测 101.txt(rev252cp): `X9 RTC: 2026-11-08 05:37:56 UTC`、`X9 MGWIPE: cache errno=0`、`tccd issued extension`
  均出现; CRASH 只剩 mediaserverd/AegirPoster ×104 行、syncdefaultsd 64、CommCenter 44、driverkitd 4,**assetsd 0**。
  USER: 相册、时间等修好, 键盘 123 仍打不开 → handoff 47/51 的 "123 = assetsd 崩溃循环" 推断**证伪**。
- 实测 101.txt 键盘段(Spotlight, 77.8 s 下拉): 81.4–97 s 在 (80-100, 2440-2496) 连点 123 十余次, HID down/up
  全部成对(99/99, 只有 finger 0), 无 Spotlight/kbd 崩溃, 帧持续 ~10 fps。另见 hfs `set LowDisk` (freeblks≈262k,
  低于 warninglimit 262144, 约 1 GiB 空闲), 尚未处理。**/tmp/101.txt 已随服务器卡死重启丢失**, 需用户重新放。
- 实测(Mac 反汇编 dsc-20H392 UIKitCore, 工具 ~/x9-kbd-dis/{dis.py,callers.sh,ivars.py}, 不在 git):
  123(More 键)在 **touch-down** 切键盘面: `-[UIKeyboardLayoutStar completeSendStringActionForTouchDownWithKey:
  withActions:executionContext:]` 0x1897ebabc: action bit 5 → **`[_extendedTouchInfoMap count] == 1` 才**
  `setKeyplaneName:[keyplane alternateKeyplaneName]`, 否则静默跳过(ivar 偏移指针 0x1e1d8e950 = UIKeyboardLayoutStar
  `_extendedTouchInfoMap` NSMutableDictionary)。字母键(touch-up 出字)和 shift(bit 4/34 路径)不查这个计数——
  正好解释"字母能打, 123 没反应"。条目在 generateInfoForTouch:(touchDown) 加入, 在 clearInfoForTouch:forResting:
  删除(调用者: completeRetestForTouchUp ×2, completeHitTestForTouchDown, continueFromInternational…, pinchDidConsumeTouch,
  touchCancelled:forResting:)。
- 推断(未验证): 计数≠1 = 有触摸条目泄漏(某次触摸的 up/cancel 没走到 clear), 或键盘任务队列滞后(主线程被
  dictation 链拖慢, 见 30-42.txt)使 down/up 处理错序。我们的 x9_kbd_dictation 种子(每次开机把 cfprefsd 写回的
  bplist 换成 XML, 原键丢弃, 备份 .x9-before-seed.plist)与 x9_siri_off(launchd.plist 32 处 Mach 服务改名)
  都不直接碰这条路径, 但种子会抹掉用户键盘设置(含 QuickPath 等), 列为待排除项。
- **102.txt(USER: 键盘弹出后第一下点 123 也打不开)**: 实测 Spotlight 30.4 s exec, 34-36 s 点搜索条, 39.9 s 首次点
  (71,2442) 无效, 之后 45-73 s 在 (50-135, 2395-2463) 连点 ~35 次; HID 依旧单指成对。→ "先前触摸条目泄漏" 排除。
- 实测(反汇编 `-[UIKeyboardLayoutStar downActionFlagsForKey:]` 0x1897f3c10): More 键 = interactionType **11** → 固定
  flags 0x24(含 bit 5 切面)。唯一例外在函数开头: `[key visible]==NO` 或 `stateForKey:==1`(disabled)→ flags 0。
  所以 123 无效的剩余可能: (a) 命中测试没落到 More 键; (b) 该键 visible=NO / state=disabled; (c) 按下时
  `_extendedTouchInfoMap` 计数≠1(非泄漏型, 如 down/up 同批处理)。下一问 USER: 按 123 时键是否变暗高亮(高亮=命中且
  未禁用 → 查 c), 地球键/删除键是否有效。
- 下一步(无需构建的设备判别): (1) 键盘弹出后**先不碰任何键**, 第一下就点 123 —— 能开 → 泄漏型; 不能 → 别的门。
  (2) 按住 123 不放 ~1 s 看是否在按住时切到数字面。 (3) 换 App(备忘录/设置搜索)试。 结果决定: 泄漏型 → 查
  哪类触摸没被 clear(候选: 键盘出现动画期间按下的触摸、系统手势取消); 否则抓 Spotlight 主线程样本
  (x9_sample_procs=Spotlight)。

## HANDOFF 2026-11-08 (53): rev252cp 统一镜像 BUILT = 相册+机型+时间三修复同内核; 未上机; 既有镜像缺口核验

- 任务: 用户要"最新最全"(三修复齐备)的镜像并 push 仓库。
- **既有镜像缺口核验(观察; 内核 strings + boot.json 哈希逐项对过)**: 没有任何一个现成镜像三修复齐备。
  - `x9-rev252cn-mgwipe/boot.img` (9f0c6278..., 打包内核 = `x9-a6fbe37-dirty` 03490aa9..., boot.json
    kernel_sha256 相符): 有 `X9 RTC:`×5 + `X9 MGWIPE:`×3, 但**缺相册修复**——无 `issued extension`/
    `SWEET Sandbox` 串, 即 46e1d4c 不在其构建源码里(该内核构建先于 sandbox 提交落地)。
  - `x9-rev252co/boot.img` (40ae2357..., 内核 d3959493...): 三修复代码全在(三串全命中), 但嵌入参数集只有
    x9dc=1 x9if=1(dc-if 组), **没有 x9_mgwipe=1** → MG 缓存清理不会触发, iPod 缓存照旧。
  - `x9-rev252cn-rtc/`: 只有时间。名字带 a6fbe37-dirty 的构建目录命名与实际源码内容不符, 判镜像以
    strings+哈希为准。
- **rev252cp 构建(本次, 从 HEAD 6edb086 全量)**: `python3 -I x9/build/build.py all --revision 269 --name
  x9-rev252cp` + 14 个 Wi-Fi/GPU token(x9_wifi_power=6 x9_wifi_real=1 x9_gpu_khz=1716000
  x9_gpu_floor_khz=1100000 x9_gpu_park=1 x9_submit_async=1 x9_wifi_xo0..6 x9_cache_scope=1) +
  `x9dc=1 x9if=1 x9_mgwipe=1`。流程 plugins 269 重建 → kernel(51/51 overlay objects, ccache)→ boot。
  - **三修复验证(观察, 内核 strings)**: `X9 RTC:`×5 / `X9 MGWIPE:`×3 / `issued extension`×1 全命中。
  - **嵌入参数(payload strings)**= cn 基线 29 token 逐字节一致 + `x9dc=1 x9if=1 x9_mgwipe=1`。
  - 哈希: boot.img **48e7c81155d4037f099071e4d8f18e1ca6606b461e9cdd60762b2216b7478cb4** (11730944 B);
    kernel **5c75c3c61b670f27f7e3ffc480714fde1bd6b691bfab968289b9fcea8603578b**;
    payload **0286d71c90d22a6e2bd5cdb00dbf3515d64d5006ded86fb115a198cbbc713c65**。
  - allowlist: plugins 重建非确定性按预期只动了 X9GeneralMetal 哈希 → identity 13a7b368... →
    **64a12d08170fff48ed097d24944276e65f308fa147db2360096b3afb1bf04a01**,头已随本条提交;新内核只与本镜像
    payload 配对。**mailbox 必须先重写**: `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252cp`,
    然后 `fastboot boot x9/out/x9-rev252cp/boot.img`。
- 上机判读(52 三线合一): (1) 早期 `X9 RTC: 2026-11-08 HH:MM:SS UTC = N s`,1970 窗口消失;失败形态
  `map failed`/`PMIF transfer failed`/`implausible ...` 原样报。 (2) `X9 MGWIPE: cache errno=0`(errno=2 =
  缓存本就不在, 另找 iPod 来源)且 UI 机型不再显示 iPod。 (3) assetsd 不再崩(16 次/122s → 0)+ 搜
  `issued extension` 行 + 键盘 123 + 点相册看 MobileSlideShow exec 走多远(仍无 exec → 拒绝在
  SpringBoard 内部, 下一轮 x9_sample_procs=SpringBoard)。 (4) 三项验证后 reboot 不带 x9_mgwipe(缓存重建
  一次即可, 勿每 boot 清);本镜像带 x9dc/x9if(101.txt 第四轮已证 armed+stable)。

## HANDOFF 2026-11-08 (52): 相册打不开 → 查到 hackios 生态的 assetsd TCC 修复并移植 (ops 5/6 真 token), rev252co BUILT 未上机

- **任务**: 用户报相册(Photos/MobileSlideShow)打不开,并指路查 UniXNU 和 hackios 的相关修复。
- **101.txt 证据链(观察)**: 全部近期日志(95-101)中 MobileSlideShow 一次都没 exec 过——无 `SWEET AKS:
  MobileSlideShow`(exec 校验)、无 CRASH;对照: Safari 在 98.txt exec 后崩(异常路径与 assetsd 同款)。
  101.txt 117.768s 有一次 tap(555,1138):有 21 帧 UI 响应(动画)、**SpringBoard 没发 AKS sel 26/27
  (assertion)、没有任何 exec**——若这是相册 tap,拒绝发生在 SpringBoard 内部、launchd 之前;但也可能
  没点在相册上(87 次 tap 大多在日历内交互;日历 74.2s 被点开并渲染 74-86s,说明点图标启动 app 本身是通的)。
  内核侧排除项: 本轮无 x9_lite/sweetnoexec,trust-cache 失败会有 CS_KILLED CRASH 行,均不成立。
- **UniXNU 上游: 无相册/TCC 相关修复**(四个 checkout 的 git log --grep 和 docs 全空)。
- **hackios 生态: 找到已上机验证的修复**。同根因 `assetsd requires TCCPhotosService access` 在三个移植
  都被同一个内核补丁修掉:
  - **ios-asteroids 0116** (nothing-sm7635-devs/ios-asteroids, commit 5f7ff1e "sandbox extensions
    issue/consume succeed (fixes Photos/assetsd)";后续 commit bffdfb3 "**Photos works (0116)**"):
    tccd 每次 TCC 授权都要 `sandbox_extension_issue_generic("com.apple.tcc.kTCCServicePhotos")`;
    op 5 返回 ENOENT 时 tccd 报 kTCCErrorDomain 6 "Failed to issue Sandbox Extension",assetsd 崩。
    修复 = op 5 写回真 token、op 6 回写非零 handle。docs/CSOPS.md 有完整诊断+上机验证步骤
    (`SWEET Sandbox: tccd issued extension type 3 'com.apple.tcc.kTCCServicePhotos'` 为通过标志)。
  - **ios-b0s 0313** (JeyKul/ios-b0s): 同修复,token 伪成 Apple 序列化格式(40hex;00;…;class;01;…;path),
    并记录了参数布局。**ios-popsicle 0329**: 同修复(顺带修 appex 容器路径)。beyond2lte-ios: 记录了同
    崩溃,未修。
  - 0116/0313 恰好回答了 handoff 51 里 op-5 dump 要探的问题: `__sandbox_extension_issue` 7×u64 =
    [0] class/path 串指针, [1] extension type, [2] 目标 pid 或 path, [3] flags, **[4] 2048B token
    输出缓冲**, [5]/[6] 未用; consume = {token 串, 含 NUL 长度, int64 *handle_out}。无沙箱时扩展本就
    不授予任何东西,消费者不校验格式(libsystem_sandbox 的 consume 只拒绝 "invalid" 前缀)。
- **已移植(commit 46e1d4c, x9/kernel/src/bsd/kern/sweet_amfi.c)**: op 5 写 opaque token
  `SWEET-<serial>;<type>;<class>`,op 6 回写非零 handle;0x43 保持 ENOENT(iOS 16.7 容忍;
  asteroids 0136 是 iOS 18 版语义);删 x9_sandbox_ext_ok(token-less 路径已被 40.txt 证伪——
  syncdefaultsd 在 ENOENT 和 token-less 成功下都崩,与其无关);op-5 64B dump 保留(x9_sandbox_ops=1,
  验证我们 16.7.16 用户态的布局)。仅内核改动,插件不变,allowlist identity 不变,mailbox 载荷不受影响。
- **BUILT(仅本机, 未上机)**: x9/out/x9-rev252co —— kernel d39594930c161fab...,boot.img
  40ae235719baed25...(11730944 B, 参数集 x9dc=1 x9if=1,同 101.txt 第四轮)。
- **上机判读**: (1) assetsd 不再崩(16 次/122s → 0),搜 `issued extension` 行;
  (2) 键盘 123 是否修好(assetsd 活了数字盘才装得上); (3) 点相册:若 exec 出现 → 看它跑多远;
  若仍无 exec/AKS sel 26 → 拒绝在 SpringBoard 内部,与 assetsd 无关,下一轮加
  x9_sample_procs=SpringBoard 在 tap 时刻抓栈; (4) 顺带回收 handoff 51 的诊断包
  (COMMIT WORK/GOV stack/op-5 结构/scope-hold/PARK retire)。**mailbox 要按 51 的提醒先重写**
  (plugin 269 载荷与设备邮箱不配对):`X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252cn-dc-if`,
  然后直接 boot rev252co 的 boot.img。

## FINDING 2026-11-08: MobileGestalt 机型读取链已反汇编核验(20H392 实机 dylib); 内核/ADT 供给全对; "还是 iPod" 首要嫌疑 = MG 磁盘缓存陈旧答案

- 对象: `.local/x9/work/dsc-20H392/usr/lib/libMobileGestalt.dylib`(设备实跑的 iOS 16.7.16 拷贝, 有符号)。
  读取链: **ProductType ← sysctl `hw.product`**(copy_productType 0x19c58b0cc → hash key `h9jDsbgj7xIVeIQ8S3/X3Q`;
  native 0x19c571680 直接把 hw.product C 串转 CFString); **DeviceClassNumber ← hw.product 前缀匹配**
  (0x19c55f5c8: iPhone→1, iPod→2, iPad→3, AppleTV→4, iFPGA→5, Watch→6, AudioAccessory→7, iBridge→8,
  AppleDisplay→0xa, ComputeModule→0xc; **前缀不匹配或 hw.product=NULL → 默认 2 = iPod**, 结果缓存在
  [0x1e1d7c740]); HWModelStr ← sysctl `hw.target`; 其余读 `IODeviceTree:/product`(product-id/product-name/
  artwork-device-idiom/subtype/scale-factor/gamut/display-corner-radius/side-button-location)、
  `/buttons home-button-type`、`/chosen display-scale`。MG 二进制内**没有**内置机型表(全库只有 "iPhone" 等
  前缀字符串)→ 答案完全由 DT/sysctl 决定。
- 缓存(为什么 iPod 不死): MobileGestaltCache.c → `/private/var/containers/Shared/SystemGroup/
  systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`, 校验只有
  OS build("cache %@ doesn't match OS %@")+ build UUID("cache UUID %@ doesn't match build UUID %@")。
  命中即返回, native 只在 miss 时调("MGCache: Invoking %@")。我们 build 恒 20H392 + unique-chip-id
  合成 0x5839000000000001 恒定 → **改 ADT/内核永不失效**; 与老 sweet 项目 6854 的失效模式相同。
- X9 供给已核验全对: staged `x9/out/x9-rev252cn-dc-if/x9.adt` root `model=iPhone10,3`、compatible
  `D22AP|iPhone10,3|...`、`/product` 完整(product-id fd079b...=真 D22AP 值, artwork phone/2436/3x/sRGB)、
  `/buttons home-button-type 2`、serial/model-number=PLJ110; 101.txt:79 `X9 IDENTITY: product=iPhone10,3
  board=D22AP; description=iPhone20 model=PLJ110`(kernel: hw.product=IODT getMachineName←ADT root model,
  hw.target=第一个 compatible, SweetPlatformExpert.cpp:130 注释; kern_mib.c HW_PRODUCT→PEGetProductName,
  SWEET 已解 kludge)。→ 若 UI 仍显示 iPod, 首选删上面的缓存 plist(boot 时 MG 重建走 native)再验证;
  x9_update_install 目前只能 publish 文件不能删 var 下的文件, 删除需 boot-arg 门控 PID1 unlink
  (仿 08b386d 模式)或重刷根。删完仍 iPod 再查 UIKit 侧(status-bar device-name item 的具体 key)。
- **x9_mgwipe 已实现(commit 6613ddb, kern_exec.c sweet_x9_wipe_gestalt_cache)**: PID1 exec 路径
  (update-install 块后)在 `x9_mgwipe=1` 时对两个路径各 unlink 一次——systemgroup 缓存 plist 与 legacy
  `/var/mobile/Library/Caches/com.apple.MobileGestalt.plist`;ENOENT 算成功,其它错误只记录不拦 boot
  ("X9 MGWIPE: cache/legacy errno=N")。**BUILT**: 内核 `x9/out/x9-a6fbe37-dirty`(269 allowlist identity
  13a7b368... 已在内核,与 handoff 51/52 的 269 staging 规则一致),boot 镜像
  `x9/out/x9-rev252cn-mgwipe/boot.img` sha256 9f0c6278bd2c70e959875d1cd9bb8da906def102734d8efffd9b0955cd852a67,
  embedded args = rev252cn 基线逐字节一致 + `x9_mgwipe=1`(已 diff 核对);payload(邮箱用)
  sha256 ae1bc3aab684f14b75243728351d036a115c911bea0063ce6295009fc5c3a2fb。**269 plugin-package 已拷入
  candidate**(boot 步骤带 --kernel 不产 plugin-package;来源 x9-de3702b-dirty 的诊断包,identity
  13a7b368...,10 个文件大小+SHA256 与内核 allowlist 逐一核对全等,插件二进制非确定性所以不能重跑 plugins,
  必须用与 committed 头同批的那份)。上机: 若设备邮箱还是旧载荷,
  先 `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252cn-mgwipe`,再
  `fastboot boot x9/out/x9-rev252cn-mgwipe/boot.img`;日志看 `X9 MGWIPE: cache errno=0`(errno=2=文件本就不在,
  说明缓存此前不存在,需另找 iPod 来源)。验证后去掉该 boot-arg 恢复正常缓存(每次 boot 都删会每次重建,
  不损坏但浪费一次 rebuild)。

## HANDOFF 2026-11-08 (52): 开机 1970 修复 = PMIF/SPMI 读 MT6687 RTC (getGMTTimeOfDay), BUILT x9-rev252cn-rtc; 未上机

- 实现(commit 0b74af5, `SweetPlatformExpert.cpp`): X9 的 PE 覆写 `getGMTTimeOfDay`——经 PMIF/SPMI 只读
  MT6687 PMIC RTC。协议逐寄存器对照 Android 树移植:
  - PMIF(`spmi-mtk-pmif-core.c` mt6993_pmif_arb): MMIO **0x1c01a000**(DTS "pmif", path 0), SW 通道
    soc_chan=2 → SWINF_2(ACC 0x0880/WDATA 0x0884/RDATA 0x0894/VLDCLR 0x08A4/STA 0x08A8); 传输 = 等 IDLE
    (顺带排空陈旧 WFVLDCLR)→(写 WDATA)→ ACC = cmd<<30|sid<<24|bc<<16|addr → 等 WFVLDCLR → 读 RDATA →
    写 1 到 VLDCLR, err 位 (v>>18)&1; 16 位 PMIC 地址用 EXT_REG_LONG(cmd=3); 每次传 2 字节(bc=1)。
  - MT6687 = **USID 9、path 0**(mt6687.dtsi `reg=<0x9 SPMI_USID>`; spmi-dev-mask 0x55a8 bit9=0)。
  - RTC(rtc-mt6685.c 的 MT6685 布局, base 0x580): PDN_CLR 0x516←0x30 上电 MCLK+32k → IOSleep 1ms →
    读 BBPU 0x588 → 写回 |KEY 0x4300|RELOAD 0x20 → WRTGR 0x5C2←1 → 轮询 BBPU.CBUSY(bit6)清零 →
    读 7 个 u16: TC_SEC 0x592 起每 +2(SEC MIN HOU DOM DOW MTH YEA), 掩码 3f/3f/1f/1f/7/f/7f,
    年=raw+1968、月 1-based → 带进位防护(sec2>=sec 才接受, 最多 8 轮)→ PDN_SET 0x515←0x30 断电。
  - 防御: 年 2024-2099/月日时分秒范围校验, 不合理或 PMIF 传输失败 → 记 `X9 RTC:` 行并返回 0(=今天
    的 epoch 行为, mobiletimerd 恢复路径与 NTP 纠正不受影响)。锁 = IORecursiveLock(开机 1 次 +
    IOPM 睡眠路径调用)。`setGMTTimeOfDay` 只打一次日志(clock.c:826 每次 settimeofday 都会调它,
    写路径留给上机验证后的下一轮)。
- 挂接链确认: bsd_init:688 `IOKitInitializeTime` → IORTC 资源(PE start 发布, 无 30s 等待)→
  `clock_initialize_calendar` → PEGetUTCTimeOfDay → 我们的读。uptime 大于 RTC 值时内核会把日历钳到
  uptime(clock.c 的 sys>secs 分支)——RTC 失败回 0 时即今天的告警路径, 无新行为。
- **BUILT `x9/out/x9-rev252cn-rtc/`(kernel+boot 一起构建, 基线 cn 完整 29-token 参数集已验证嵌入)**:
  boot.img sha256 **59ada17e79deb90a606903539f30f4ca97f6ca068e8352dccc117f2e3e783d8d**,
  kernel.development.sweet sha256 **b3903226d26c918c7a75a2041e8892d14f91b93cde1577aeaad8356e74389848**。
- ⚠️ 归因注意: 构建时工作区里有一个并发会话未提交的 `kern_exec.c` 改动(x9_mgwipe=1 时 PID1 清
  MobileGestalt 缓存), 已被编进这个内核但**完全惰性**(镜像参数集没有 x9_mgwipe, 函数首行即 return 0)。
  该改动落地后如需纯净镜像可重跑 `python3 -I x9/build/build.py --name <n> <14 个 Wi-Fi/GPU extra token>`。
- 测试(用户执行): mailbox **不用重写**(allowlist identity f4b2aa27 在本内核与已装 268 bundle 配对),
  直接 `fastboot boot` x9/out/x9-rev252cn-rtc/boot.img。看日志: 期望早期出现
  `X9 RTC: 2026-11-08 HH:MM:SS UTC = N s` 且之后 launchd/daemon 时间戳开机即真实日期(1970 窗口消失);
  失败形态三种各有行号: `map failed` / `PMIF transfer failed` / `implausible ...`——任一失败都原样报告,
  并同时看 `X9 MGWIPE:`(并发会话的惰性门, 应无输出)。成功后下一轮: 写路径(setGMTTimeOfDay → BBPU
  解锁+TC 写+WRTGR, 参考 mtk_rtc_set_time)使 NTP 校时写回 RTC。

## HANDOFF 2026-11-08 (51): 开机 1970 根因查清(未改代码)= 内核从无 RTC 读取路径, 日历恒从 epoch 0 起步; 修复 = pwrap 读 MT6687 RTC

- 用户报告: 每次开机时间跳回 1970。结论(代码证据, 全部可复查): XNU 日历初始化链
  `clock_initialize_calendar()` (.local/dxb/xnu/osfmk/kern/clock.c:1032) → `PEGetUTCTimeOfDay()`
  (iokit/Kernel/IOPlatformExpert.cpp:1363) → `IOPlatformExpert::getGMTTimeOfDay()` **基类默认 return 0**
  (IOPlatformExpert.cpp:393)。X9 的 SweetPlatformExpert 没有覆写它; 271 个补丁谱系 + kernel/src overlay
  中 grep `getGMTTimeOfDay|getUTCTimeOfDay|setUTCTimeOfDay` 0 处实现——开机时间自移植第一天起就是
  1970-01-01 00:00:00 UTC + uptime。bsd 侧 `inittodr(0)` (kern_time.c:243) 只做负值校验, 从不设时。
- SweetPlatformExpert.cpp:40 的 `publishResource("IORTC",this)`(dali0210)只是 readiness:
  IOStartIOKit.cpp:70 `IOKitInitializeTime` 会等 IORTC 资源最长 30 s, 发布它只是消掉等待; 它不是 RTC 驱动。
- 101.txt 实测时间线(观察, 非猜测): uptime 7 s launchd 打 `1970-01-01 08:00:39`(=0+uptime, +8 时区本身
  是对的); ~99.4 s 被设为 2026-01-01 08:00:00(mobiletimerd 下限值); ~101.8 s → 2026-11-07 08:00:00
  (mobiletimerd 从盘上恢复的上次已知时间; 猜测——未反汇编 mobiletimerd); ~103.8 s → 2026-11-08 08:00:01
  (联网纠正)。即"跳回 1970"= 每次开机前 ~100 s 的窗口 + 无网时停在恢复值。
- 硬件事实: 真 RTC 在 PMIC **MT6687** 里(mt6993.dts:23925 `&mt6687_rtc`; mt6687.dtsi:60
  `base = <0x580>`, compatible "mediatek,mt6687-rtc"), Android 侧由 drivers/rtc/rtc-mt6397.c 经
  pwrap 持续维护(树在 ~/HelioDarwin/oppo-source/modules/kernel/kernel_device_modules-6.12)。RTC 自走且
  Android 一直校准, 所以**只需要"读"**: iOS 开机读一次即可拿到正确时间; 写回仅校漂移, 非必需。
- 修复方向(下一步, 未实施): SweetPlatformExpert 覆写 getGMTTimeOfDay/setGMTTimeOfDay(或独立 SweetX9RTC),
  pwrap 读 MT6687 RTC_TC_SEC/MIN/HOU/DOM/MTH/YEA(参考 rtc-mt6397.c 语义+写触发 WACS; 年份基
  RTC_MIN_YEAR=1968)。前提待验证: LK/ABL 交棒 shim 后 pwrap/PMIC wrapper 的 MMIO 状态(Android 每次开机
  读 RTC、LK 也读, 大概率已初始化)。备选: shim 阶段读好把 epoch 放 DTB chosen, 内核 PE 读 DTB——同样要
  pwrap 读路径, 不省事。注意 mobiletimerd 的恢复路径与 IORTC 用户态探测如何交互, 实施时先只做读、观察
  boot log 时间戳是否开机即正确。

## HANDOFF 2026-11-08 (51): rev252cn 诊断包 (plugin 269) - 四项推进的结果与下一步

- **#1 per-draw 诊断(已落地,待上机)**: GOV 行加 stack=%u kHz(MCU 频域跟随 stack 还是固定,一次上机见分晓);
  前端 commit 时节流输出 `X9 COMMIT WORK: commit=%u draws=%u passes=%u pixels=%llu dispatches=%u blits=%u`
  (encodedWork 本来就有,只是没打日志)——与内核 `X9 RENDER timing` 的 wait_us 按时间离线相关,直接量出
  per-draw GPU 成本。
- **#4 assetsd(键盘 123)根因已钉死**: 堆扫描抓到完整异常 `NSInternalInconsistencyException: 'assetsd
  requires TCCPhotosService access'`,下层 `auth_error_string="Failed to issue Sandbox Extension for
  kTCCServicePhotos"`——tccd 活着也响应了,死在**签发沙箱扩展**。我们的 stand-in 把 op 5(issue)/6(consume)
  保持 ENOENT(rev252q 让它们无 token 成功过,syncdefaultsd 10 秒一崩所以回退)。已加 op-5 参数 dump
  (x9_sandbox_ops=1 时打一次 64 B hex):下一次设备跑拿到结构后,就能写回真 token 让 tccd 的 Photos 授权走通。
  这才是键盘 123 的修复路径,和 GPU 无关。
- **#2 scope 真相(推翻了早前的判断)**: cache-uncovered 只有 ~30 KB——前端 scope 其实覆盖了 imports
  (pre_skip 57-63 MB);真正维持的 13-18 MB 是**"范围已注册但被 pending command buffer 引用(refs>0)"**
  的部分——scope 设计上对被引用范围保守维护(防 CPU 写),而前端 acquire 不区分 GPU 读/写。新遥测
  `X9 RENDER cache-scope-hold: pre_avg/post_avg` 量化它。真正的修复 = acquire 区分读写用途(渲染目标标记
  GPU-write-only,pre-cache 可安全跳过),是前端+内核的设计决策,不是 bug。
- **#3 P1-A 未动,但有了计数**: PARK 行加 retire_map/retire_unmap/retire_other(101.txt 第四轮 cold 仍占
  ~55%,map/unmap churn 是主因)。BO pool(Mesa 侧 + 插件)仍是正解,工作量另估。
- BUILT: plugin 269(`X9 COMMIT WORK` 日志)+ 三支内核逐字节一致(35a7d364...)+ boot: cn 087fa246... /
  cn-dc 919c78d1... / cn-dc-if c63a1384...。**mailbox 必须重写**(插件二进制变了):`X9_SERIAL=<serial>
  python3 x9/mailbox/update.py x9/out/x9-rev252cn-dc-if`。
- 下一轮上机建议:dc-if 镜像 + `x9_sandbox_ops=1`,一次拿齐:COMMIT WORK vs RENDER timing 相关性、GOV stack
  频率、op-5 参数结构、scope-hold 体量、PARK retire 分类。全是诊断,无行为改动风险。

## HANDOFF 2026-11-08 (50): rev252cn = GPU 帧管线两优化 BUILT (x9dc 脏区间 CIVAC + x9if 双帧在飞), plugin 268; 3 images; 未上机

- 背景: 97.txt 审计(会话前半)结论——显示通路/GPU 频率都不是瓶颈;帧管线完全串行(1 job/present,join 行
  shown==enqueued==completed 全程成立),卡顿 = 复杂场景下 GPU job 执行(wait_us)5→20+ ms 与用户态翻译同涨。
  串行根因: 每帧 `vkQueueSubmit` 先 `pan_kmod_flush_bo_map_syncs` → `X9RenderCache` 选择器要拿 context+owner 锁,
  而异步 worker 在整个 fence poll 期间持有它们。实施优化 2(脏区间)+优化 4(双帧在飞),commit 12af59f。
- **x9dc**(boot-arg, 默认关): 插件纯 CPU→GPU clean(`PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH`)改发
  `X9RenderClean|X9RenderDefer`(caps bit 128)记录到 per-context 列表(`X9PanVKMemory.c` cache_call,记录路径
  不持 context/owner 锁——这是客户端不再串行的关键); 下一次 submit 的 publish 阶段精确维护这些范围并对覆盖的
  资源跳过全量映射并集 fallback。submit 入队时在 c->lock 下把列表搬进按 submission id 奇偶的 pending 槽
  (render_dirty_stage),所以 publish(S) 不可能吃到 S+1 的记录;溢出(>64 条)/句柄失效只损失优化、不损正确性。
  只 defer 纯 clean;invalidate(GPU→CPU 读回)仍内联。遥测: 新 `X9 RENDER cache-dirty` 行(entries/bytes/
  skipped_res)。预期 pre_cache 1-2 ms → <0.5 ms。
- **x9if**(boot-arg, 需 x9_gpu_park=1 + x9_submit_async=1): render_submit 在 doorbell 处拆成
  render_submit_publish/reap(状态进 RenderJobState,两份静态实例,worker 独占);worker 把 FIFO 里下一个
  同 context+group 的 job 配成一对——先 publish job1,再经 parked-resume 路径 publish job2(不做 AS/CSG 编程;
  worker 先 `render_parked={c,g,root1}` 触发检测),然后按 FIFO 顺序 reap1、reap2。每流第二 fence 槽在 storage
  +0x8300(marker 区 0x8080-0x8288 之后、render_page_sync 的 16 KB 页内),publish 只 bzero 自己的槽,所以 job1
  的 fence 值活到 reap;depth=1 地址恒 +0x8000,与今天逐字节一致。producer 改为 publish 时提交(原来 reap 时;
  否则 job2 的 expected 会与 job1 撞包),reap 不再提交。user-io insert 检查在 pipeline 下放宽为 >=(在流内顺序
  消费下与 == 等价)。c->lock+owner 锁跨整对持有: map/unmap/v1 选择器照旧"绝不在 job 中途",x9dc 记录路径是
  唯一在 job 期间保持无锁的客户端调用。**设备验证点**: reap1 的 flush_gpu 与 post-cache IVAC 在 job2 运行中执行
  (clean-only 写回与 IVAC 丢弃不会盖掉 GPU 写,但 FLUSH_CACHES 中途语义未在 Mali 上验证过);新遥测 ASYNC window
  加 inflight_max(期望到 2)。同步路径/depth-1 行为逐字节不变(fence 槽 0 = 今天的 +0x8000)。
- ABI(两份头同步已 diff 验证): `X9RenderDefer=0x100u`(修饰位,旧 immediate 路径 `operation&~3u` 会拒绝它)
  + `X9RenderDeferredCache=128u`。plugin 268(`X9PanVKMemory.c`): caps 有位且 `X9_DEFER_CACHE=1` 未设时 defer;
  `X9 PANVK SUBMIT` 行加 `cache=deferred clean (x9dc)`;`X9 PANVK CACHE RANGE` 行尾改注 deferred。allowlist 头
  重生成为 268 并随 12af59f 提交。
- **BUILT 3 images(全参数集,cm4 的完整 --boot-arg 组;最终轮,内核三支逐字节一致 c912d585...)**:
  x9-rev252cn boot bcc37a7b66f064b8...(基线,两特性关,验无回归)/ x9-rev252cn-dc bf57d99c7ef2ce578...(x9dc=1)/
  x9-rev252cn-dc-if cdb2f5665500ae7c...(x9dc=1 x9if=1)。**plugin 构建非确定性**(Rust,每跑一次 plugins 二进制
  哈希就变),而 allowlist 编译进内核——最终统一:三支内核都按同一份 allowlist(identity f4b2aa27,commit 在树)
  重编,所以**只需一次 staging**:`X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252cn-dc-if`
  (设备邮箱现在还是第一轮的 d735c1b3 载荷,与新镜像不配对,必须重写),然后三支镜像随便 boot。往后再出镜像:
  只跑 `plugins` 一次 + 三次 `kernel`/`boot`(带 --panvk skip),别每支都 `all`。101.txt 第一次卡 logo 是没写
  mailbox;第二次写好了但内核换轮了——装机 `install result errno=0`、`active plugin=X9GeneralMetal268`、
  discovery armed 那次都是配对的。
- **101.txt 修复(commit dcec9a1)**: 第二次上机首个 GPU job(submission 1)在 `user-io-prepare` uncertain
  ——我把 `g->producer` 提交放在了 user-io 校验之前,而该校验比较的是"user-io producer 字 == g->producer
  (本 packet 替换前的旧值)",0≠32 必失配。修正:producer 提交挪到 `stream-publish`(与 user-io 写入同一步),
  reap 不提交;parked-resume depth-1 流程已人工核对自洽(va 与 producer 都等于上一个 job 的 expected)。这影响
  全部三支镜像(split 无条件),所以三支都重建了,哈希以上面为准。
- **本机 .local/dxb/xnu 树的机器本地改动**(不在 git,重建环境需重做): (1) EXTERNAL_HEADERS/corecrypto/ccmd5.h
  从 .local/x9/work/legacy-import/xnu_x9/refer/corecrypto-2023/ 恢复——cm6 会话曾复制过它,后来的树刷新把
  untracked 文件洗掉了,导致 SweetX9NetworkStack 从头编译报 corecrypto/ccmd5.h not found(cm2-cm4 的日志能编
  是因为当时文件还在;cm5 之后全靠 prebuilt 对象掩盖); (2) libkern/crypto/sweet_aes.c 加了
  sweet_aes_ecb_decrypt(ccmode_ecb,BearSSL 无 ECB 入口,用 CBC-dec 零 IV 逐块实现)+ ccaes_ecb_decrypt_mode()
  访问器——cm6 最终代码改用了 CBC-as-ECB 内联(handoff 49),该 ECB 目前无引用,留着无害。
- **101.txt 第四轮(桌面起来了,x9dc/x9if 全程 armed,0 uncertain/0 DeviceLost),但流畅度没变,数据说明**:
  (1) cache-dirty 每帧只覆盖 ~200-285 KB,而 fallback 真实体量 pre_bytes_avg=77.6 MB(重窗口)——deferred
  clean 只盖插件自己的 BO 同步,CIVAC 大头是 IOSurface imports(客户端进程写,插件不知道脏区间)。x9dc 要真
  省就得让前端 scope 覆盖 IOSurface(老审计里 cache-uncovered ~41 MB 就是这缺口); (2) inflight_max 61 个窗口
  里只出现过一次 2——客户端(Metal runtime/CA)仍是一帧一等,FIFO 里没第二个 job 可配对;内核深度没问题
  (证明能到 2)。x9if 要生效必须改 metal2vulkan runtime 的逐批等待(.local 里的 963f checkout);
  (3) dirty 记录列表爆发期超 64 条(cap 已提到 256); (4) 键盘 123 按不开 = assetsd 崩溃循环(本日志 x4,
  sig 6,与渲染无关,handoff 47 就有;cm5 的 STR 堆扫描已抓到现场,单独查)。结论:rev252cn 的两项工程上是
  对的且已稳定落地,但按 97.txt 审计的瓶颈排序,真正的大头还是 #5 per-draw GPU 成本(MCU 时钟核实+per-draw
  计数,一直没做)和客户端侧流水线,这两个才是 fps 的主乘数。
- **101.txt 第三轮: install error file=5 errno=17 (EEXIST)** — rootfs 上已有第一轮装的 268 bundle(哈希旧),
  新载荷 identity 校验通过但装不进去:x9_update_install 从不替换已存在文件。已修(x9_update_install.inc):
  先对已发布文件做哈希比对——一致则跳过(幂等重装),不一致则 unlink 重发布;PID1 时序与"装后全面验证"不变。
  三支内核重编为逐字节一致 d6f4d031...,boot: cn 222f417a... / cn-dc 54f89bbe... / cn-dc-if f0255876...。
  **mailbox 不用重写**(identity f4b2aa27 已在设备、与内核配对,第三次日志的 install error 前置校验已过)——
  直接 boot 新的 x9/out/x9-rev252cn-dc-if/boot.img,boot 自身会替换旧 bundle。cm6 的旧 boot.img 仍作废。
- Test 顺序(由用户执行): (1) 基线 cn 上机,跑 97.txt 同款操作,确认 X9 RENDER/METRICS 量级不变、无 DeviceLost
  (plugin 268 + 内联 flush ≈ 267 行为); (2) cn-dc: 看 cache-dirty 行 entries/bytes ≈ 原 pre_bytes、
  `X9 RENDER timing` 的 pre_cache_us 降幅、像素正确性(控制中心/图标网格/滚屏); (3) cn-dc-if: ASYNC window
  inflight_max≥2、重场景 presented_fps 与 latency 改善、`X9 RENDER GPU ... uncertain`/WC CHECK/MARK 行有无;
  任何 uncertain 都先退回单令牌 A/B。观测重点: 控制中心与 app 过场帧率(97.txt 里 12-26 fps 的窗口)。
- 下一步(未做): x9dc 的 GPU 侧验证若通过,再评估把 post-cache(渲染目标 IVAC)纳入脏区间(当前渲染目标全帧
  写、收益小); x9if 稳定后 P1-A BO pool(消灭每帧 map/unmap→retire,97.txt 里 cold≈40-70% job)。

## HANDOFF 2026-11-07 (49): rev252cm6 = **WPA2 stage 2 BUILT**: in-driver 4-way handshake (PRF/MIC/GTK unwrap), INSTALL_KEY_V3, protected TX/RX; sources + Android 1X capture in hand

- Evidence base: gen4m source `~/HelioDarwin/oppo-source/modules/vendor/mediatek/kernel_modules/connectivity/wlan/core/gen4m`
  (wlan_oid.c wlanSetAddKeyImpl, nic_uni_cmd_event.c nicUniCmdInstallKey, privacy.c secPrivacySeekForBcEntry,
  nic_txd_v3.c protection, nic.c BSSINFO SEC) and **android-join-2 is a RUC-Mobile 802.1X capture** (logcat: Security
  type 3, Supplicant COMPLETED, IP 10.47.253.191): M1 0x008a -> M2 0x010a -> M3 0x13ca -> M4 0x030a, GTK M1 0x1382 ->
  GTK M2 0x0302, all MIC version 2 (HMAC-SHA1-128); add_key pairwise cipher fac04 keylen 16 -> wlanIdx = the AP's
  staIdx; group key -> "Add key to wlanIdx 0" + ucBMCWlanIndexS=0 (BC WTBL entry); qmCheckRxEAPOLM3 holds M4 until
  the PTK is in.
- Formats locked from gen4m + the kmsg: UNI_CMD_STAREC_INSTALL_KEY3 = STAREC (0x03) header {bss, widxL} + TLV tag
  **0x27 len 68** {AddRemove, TxKey, KeyType(1 pairwise/0 group), IsAuth, peer[6], BssIdx, AlgorithmId=4
  (CIPHER_SUITE_CCMP), KeyId, KeyLen(16 = TK only, not the PTK), WlanIndex, MgmtProtection, KeyMaterial[32],
  KeyRsc[16]}; TXD DW3 b1 = PROTECTED_FRAME (EAPOL never protected), b4 = BMC + b0 = NO_ACK for multicast, DW1 widx
  = AP STAREC (ours 1) unicast / 0 (BSSINFO u2BcMcWlanidx, already 0 in our builder) for multicast; BSSINFO SEC
  AuthMode 6 = WPA2 / 7 = WPA2-PSK, EncStatus 6 = CCMP (Android kmsg 55.25 pre-join + 55.31 connected), 0/1 open;
  GTK KDE 00-0F-AC-01 info byte bits0-1 KeyID bit2 Tx (gen4m wpa_supp/src/rsn_supp/wpa.c gd.keyidx/gd.tx).
- rev252cm6 implementation (all in X9WifiNet.inc): crypto = corecrypto HMAC-SHA1 (PRF-512 "Pairwise key expansion",
  MIC v2); **this corecrypto has no MD5 and links CBC only** -> MIC version 1 logged unsupported, AES-ECB primitive =
  one CBC block with a zero IV (P = D(C) xor IV), used by the RFC 3394 GTK unwrap; corecrypto headers carry no C++
  guards -> wrapped in extern "C". M1 -> store + SNonce(read_random) + PTK(KCK/KEK/TK) + M2; M3 -> MIC verify ->
  INSTALL_KEY pairwise (AP widx, TK) -> M4 -> GTK KDE unwrap (KEK) -> INSTALL_KEY group (BC widx 0, keyid, RSC);
  GTK M1 rekey -> install + GTK M2. kCipherKey: 32 B = PMK -> arm the handshake (whKick; a stored M1 runs it on the
  radio thread); 64 B = full PTK -> ext mode: install only, M1/M3 forwarded to userspace (it answers). TXD protection
  per gen4m; unkeyed drops are per-direction (unicast waits PTK, multicast waits GTK); protected RX no longer dropped
  (HEADER_TRAN = FW-decrypted Ethernet; raw path strips CCMP hdr 8 + MIC 8); EAPOL-Key frames intercepted (not for
  the stack), EAP (type 0) still delivered to userspace; leave/join clear the key state; BSSINFO SEC set from the
  AP's RSN AKM (parsed before the pre-join BSSINFO now).
- BUILT kernel + boot: x9/out/x9-rev252cm6/boot.img sha256 45278d7c4ef61b277b190cc6dffecc0faca2183d8effffdfe9b72c5bc2cad81e
  (kernel 40f3f93964ef5b3137108e011a591abb83412df53e584aaa6e263b70c1d3d28b), FULL arg set embedded (cm4's, verified).
- Test: (1) PSK network first - the PMK comes with the associate request, no EAP needed: expect "X9 WIFI KEY: M1 ->
  PTK", "M3 ok", "install PTK", "M4", "GTK ... installed", then traffic. (2) RUC-Mobile: iOS runs EAP (watch "EAPOL:
  RX type 0 -> userspace" and any new X9 WIFI REQ types from wifid/eapolclient - the EAPOL delivery ioctl may still
  need mapping, that is the open unknown); after EAP success the kCipherKey PMK arrives -> "PMK arrived after M1,
  starting the 4-way handshake". (3) Report an "install ... NO CMD_RESULT" if the firmware rejects tag 0x27.

## HANDOFF 2026-11-07 (48): 100.txt = my rev252cm5 boot **WITHOUT the Wi-Fi boot args** (toggle grey, en0 never created); fixed image x9/out/x9-rev252cm5

- 实测 100.txt (my first cm5 packaging): zero `X9 WIFI` lines at all - configd opened IONetworkStack at 7.857 s but
  `x9_wifi_maybe_start()` (X9WifiNet.inc) returned because it requires `x9_wifi_fake=1` OR `x9_wifi_real=1`, and the
  boot.json base args carry neither. No en0 -> iOS shows a greyed Wi-Fi switch. Everything else (GPU, touch, UFS,
  SpringBoard) booted normally.
- Cause: `build.py boot` uses x9/build/boot.json as the base, and the Wi-Fi experiment args were always passed as
  `--boot-arg` extras per boot; I packaged with no extras. My first image (x9/out/x9-79d6e0d-dirty/boot.img, sha256
  e88f7adc9c843ac16bd2e8e647d71c3d4ad89de7828626ea951ee0d4ddbd17af) also OVERWROTE the cm4 image that lived at that
  path (same out-dir naming rule) - do not boot it.
- Fix: the full cm4 command line is recoverable from x9/out/x9-rev252cm4/shim/embedded-payload.bin (`strings | grep
  '^x9_metal_render'`; the x9_wifi_xo0..6 calibration words are deliberately not in git). Rebuilt as
  **x9/out/x9-rev252cm5/boot.img, sha256 aa2629548123b72a7a1eb3de6edc690eec3b1aad88db41e141222d908585dbc5** =
  cm5 kernel (x9/out/x9-79d6e0d-dirty/kernel.development.sweet, sha256
  dba60756c92182226c88d0d2e395012a257c2238d3e64b62b91fe891ead6f902) + the full cm4 arg set
  (x9_wifi_power=6 x9_wifi_real=1 + GPU clocks/park/submit_async + 7 XO words + x9_cache_scope=1; the exact tokens are
  in the embedded payload of both cm4 and cm5, verify with strings before booting).
- Rule going forward: a Wi-Fi test boot must pass the full --boot-arg set (recover per above) - boot.json alone boots
  a no-radio system. Verified cm5's embedded payload carries x9_wifi_real=1 and the XO words.

## HANDOFF 2026-11-07 (47): 99.txt = rev252cm4: link up, DHCP + DNS + hijacked HTTP all work, **but the captive probe never runs**; keyboard stuck on letters = assetsd crash loop suspected; rev252cm5 (probe/exception instrumentation) BUILT

- Log /Users/tosasitill/99.txt (19540 lines, 284 s). Timeline: joined RUC-Web 66.1 s, DHCP -> 10.46.12.100;
  **AP deauthed us at 121.9 s (reason 3, genuine: A1=us A2=a4:be:2b:b1:df:b1)**; iOS rejoined 146.6 s, user/app
  disconnect 162.4 s, rejoined 169.0 s, no kick until log end. So the link flaps early but came back on its own.
- **The captive-portal sheet never appeared and the probe never ran**: during 66-121 s and 169-224 s (all of it inside
  the packet-log budget) there is ZERO DNS or TCP traffic, only 1 Hz gateway ARP + DHCP + mDNS. The only web traffic of
  the whole session is one burst at 232-233 s right after the user touched the screen: 4 DNS answers from 10.3.6.6,
  TCP 124.238.121.40:80 -> SYN ACK, 297 B, FIN (classic hijacked-HTTP 302), then TLS to 10.21.8.35:443 (4x1434+738 B,
  FIN from the portal server). That shape is a manual/WebKit page load (Safari), not the CNS probe (which is
  HTTP-only, no redirect follow). rev252cm4's network grant works: no "WebKit.Ne privilege 10001 denied" line, and the
  TLS load actually moved data.
- Sheet-absence proof: the only WebKit process all run is com.apple.WebKit.GPU pid=349 (spawned 90.1 s right after
  MobileSafari AKS lines at 89.3 s). A SpringBoard-hosted portal sheet would spawn its own WebKit children
  (new pid) - none. (Observation, mechanism of the CNS trigger still a guess: configd's CaptiveNetworkSupport plugin
  exists in the system image, but we cannot see configd logs; the probe may be gated on SC state we don't provide.)
- **Key instrumentation bug found**: x9_pkt_log had ONE 160-line budget per direction; the 1 Hz gateway ARP ate the TX
  budget by 224 s, hiding every later TX (that is why 99.txt shows DNS answers but no DNS queries). RX cap hit during
  the 233 s burst, so anything after 233.5 s is invisible too.
- **Keyboard (cannot switch to the number plane)**: assetsd crashed 4x (17.3/17.4/45.9/105.7 s, sig 6) and
  syncdefaultsd 16x, both uncaught-exception aborts (host symbolication of 99.txt vs dsc-20H392: thrower frames are
  below dispatch_async_redirect_invoke, only the abort path is in the 10-frame dump). assetsd hosts the keyboard
  layout/input-mode service, so its death while the keyboard is up is the prime suspect for the dead "123" key
  (letters plane works, a plane LOAD would need the service). WirelessRadioMan 28x is QMI XPC misuse
  (qmi::sFixupServerConnection) - no modem on this port, background noise.
- rev252cm5 (this build, kernel+boot only): (1) x9_pkt_log: ARP gets its own 40-line/direction budget, IPv4 budget
  400/direction; UDP:53 questions log the QNAME (`X9 WIFI DNS TX: captive.apple.com ...`); TCP:80 payloads that look
  like HTTP log the first 160 B (`X9 WIFI HTTP RX: HTTP/1.1 302|Location: ...` style, \n as |). (2) sweetscan default
  list is now "backboardd,SpringBoard,assetsd,syncdefaultsd,com.apple.Safari" and each scanned name is heap-scanned on
  its FIRST crash only (crash loops no longer flood USB) - the assetsd scan should print the NSException reason.
- BUILT kernel + boot.img locally: x9/out/x9-79d6e0d-dirty/, boot.img sha256
  e88f7adc9c843ac16bd2e8e647d71c3d4ad89de7828626ea951ee0d4ddbd17af (kernel
  dba60756c92182226c88d0d2e395012a257c2238d3e64b62b91fe891ead6f902). Committed the plugin-267
  x9_render_external_manifest.h the previous session left uncommitted.
- Test: fastboot boot the image, join RUC-Web, stay on it 3 min, save the console log. Read in order: (a) is there a
  `X9 WIFI DNS TX: captive.apple.com` within ~30 s of the join? YES -> the probe ran and SpringBoard-side presentation
  is the problem; NO -> configd/CNS never probes (SC-state suspect). (b) `X9 WIFI HTTP RX` shows the hijack 302 target
  (http vs https, portal IP). (c) STR scan of assetsd's first crash gives the keyboard exception. Then drive the fix;
  if the probe is confirmed missing, next lever is giving configd a proper primary-service/SC state rather than more
  packet logs.
- WPA2 stage 2 (4-way handshake, key install) still not implemented (handoff 45).

## HANDOFF 2026-11-07 (46): 97.txt = rev252cm2: **DHCP OK** (10.46.12.100) on RUC-Web, then deauth + FW hang; rev252cm3 BUILT

- Log copy .local/x9/logs/x9-97.txt. Join as before; `first packet on RX5`, rx 16 in 10 s (bad 0), rings 0/16/0, en0
  10.46.12.100 at 47.8 s. So the RX5 fix works: the data path is up in both directions.
- 49.08 s: deauth/disassoc reason 3 detected from the BSSID (frame not dumped; genuine or not is unknown). Our
  teardown: STAREC state 0 answered, BSSINFO (disconnected, BSSID 0, StaRecIdxOfAP 0xfe) -> event 0x77
  (UNI_EVENT_ID_UPDATE_LP, harmless TX-delay tag) then no CMD_RESULT and no RX7 traffic ever again; every later scan
  empty, iOS rejoin at 66 s failed (not found). 96.txt had the same hang on a rejoin's pre-join BSSINFO. The portal sheet
  never had time to appear (IP at 47.8 s, link gone at 49 s).
- Android disconnect (android-join-2 kmsg 54.29): deauth, remove keys, BSSINFO ConnState 1 with BSSID = AP and
  u2StaRecIdxOfAP = AP widx, then cnmStaFreeAllStaByNetwork (STAREC remove), then nicDeactivateNetwork.
- rev252cm3: x9_radio_bssinfo(mode 0 pre-join / 1 connected / 2 disconnecting: BSSID + widx kept);
  x9_radio_leave = deauth, BSSINFO mode 2, STAREC REMOVE (tag 0x25, action 0); a leftover STAREC (failed join) is
  removed before the next pre-join BSSINFO; deauth/disassoc frames are logged in full (A1/A2/seq/reason).
- BUILT kernel only (plugin 267): boot_sha256 7220e0a2c4ac84d57683a1c7b1be1604122786b43887f84abe2c694ec8c14740.
- WPA2 stage 2 (4-way handshake, key install) still not implemented (handoff 45).

## HANDOFF 2026-11-07 (45): 96.txt = rev252cm: **ASSOCIATED** (RUC-Web 5180), TX ok, RX 0; rev252cm2 BUILT

- Log copy .local/x9/logs/x9-96.txt. Auth resp seq 2 status 0, assoc resp status 0 aid 2, `'RUC-Web' CONNECTED`.
  Data TX: 47 frames all ok (IPv6 ND, DHCP DISCOVER 342 B ...), rx 0 for 160 s -> en0 169.254.x (no DHCP), so the
  captive portal never appeared: no network, not a rendering problem. Beacons (every ~100 ms) still arrive on RX7 as
  RXD type 7 (`connected event eid=0x00 len=297`, frame at +0x58).
- Cause (from gen4m mt6653RxRingSwIdx2HwIdx): RX4 = band 0 data, RX5 = band 1, RX6 = band 2. With DBDC on, 5 GHz is
  band 1, so the data went to RX5, which we never gave descriptors.
- After iOS left (191 s) and rejoined (197.6 s) with no teardown, the first BSSINFO got an eid 0x77 event then no
  CMD_RESULT, and RX7 delivered nothing ever again (every later scan timed out). Possibly RX5 filling up blocked the RX
  DMA, possibly the missing leave.
- rev252cm2: (1) RX4/5/6 all set up (0x11000 each from pool +0x10000), polled round robin by x9_r4_try/done; log
  `first packet on RXn`, ring counts in the DATA line. (2) x9_radio_leave before a rejoin, on iOS disassociate and on an
  AP deauth: deauth frame (reason 3), STAREC state 0, BSSINFO not connected. (3) WPA2 stage 1: an RSN BSS (privacy +
  IE 48) is joinable; the Assoc gets privacy cap + RSN IE (iOS's from the associate request if found after +0x88, else
  built: AP group, CCMP, AP's first AKM); TX only EAPOL (0x888e) until keys exist (`unkeyed drops`); EAPOL TX/RX logged
  (`X9 WIFI EAPOL:`); PSK PMK from the associate request (cipher 6, 32 B) and the 802.1X PMK from kCipherKey (cipher 6)
  are stored and logged (lengths only).
- WPA2 stage 2 not done: 4-way handshake in the driver (PRF-SHA1 PTK, MIC, AES key unwrap GTK), key install through
  STAREC tag 0x27 INSTALL_KEY_V3 (pairwise widx 1; group key needs a BC WTBL entry, gen4m secPrivacySeekForBcEntry),
  protected TX/RX.
- BUILT kernel only (plugin 267): boot_sha256 ed29b242413d48337027a2707e81fe6f46ed0a17a134eb7eb018f93ac4c1a085.
- Test: RUC-Web first (expect `first packet on RX5`, rx > 0, DHCP, portal). Then RUC-Mobile with the campus account:
  see whether EAP frames flow (`X9 WIFI EAPOL: RX type 0`) and whether `PMK received` appears.

## HANDOFF 2026-11-07 (44): rev252cm BUILT = rev252cl + first Wi-Fi data path (untested)

- Not yet run on the device: rev252cl (Auth seq 1) itself is still unverified; rev252cm contains it.
- TX: if_output -> sta_tx queues Ethernet frames (64 x 1600 B ring under fake::lock, only while STA_UP). The radio thread
  (sole owner of the WFDMA rings) converts each to 802.11 data (FC 08 01 ToDS, A1 BSSID, A2 our MAC, A3 DA, HW SN,
  LLC/SNAP + ethertype) with the same TXD / ring as the ACKed Auth (TX15, MCU_Q0|ALTX, COMMAND, HF 802.11, fixed OFDM
  6M, DIS_MAT, WIDX 1, TGID from the grant), type DATA, no TX status. First 6 logged `X9 WIFI DATA: TX ok|FAILED ...`.
- RX: while STA_UP the loop polls every 4 ms (200 ms otherwise). RX4 and non-event RX7 packets go through x9_rx_data:
  RXD type 2 parsed per nic_rxd_v3.c (32 B + groups 4,1,2,3 = 16 B, 5 = 96 B, + HEADER_OFFSET x2); DW2 b7 HEADER_TRAN
  -> Ethernet as is, else 802.11 FromDS data (QoS/HTC handled, A-MSDU / protected dropped, LLC/SNAP stripped). Other RXD
  types are searched for a FromDS data header with A2 = BSSID. Result -> events->rx -> on_rx -> ifnet_input. First 6
  RX logged `X9 WIFI DATA: RX len ... -> eth N: <RXD words>`; non-beacon unknown rx4 packets `rx4 other` (first 8).
- Deauth/disassoc from the BSSID -> STA_IDLE + link_lost. Counters every 5 s when changed:
  `X9 WIFI DATA: tx N (fail, queue drops) rx N (bad, protected, other)`.
- Not done (Android does after association): EDCA_SET 0x04, RX_HDR_TRAN, SMART_GEAR, power-save config. Data rides the
  command ring at 6 Mb/s fixed rate: a first working path, not the HIF data rings.
- BUILT kernel only (plugin 267 unchanged): boot_sha256 82eb9e42389128365539d951f44e5c2221e1a9ea976f3227466b24729212a736.
- Test: join RUC-Web; check Auth resp seq 2 status 0 -> Assoc -> CONNECTED, then the DATA lines and whether DHCP gives
  an IP (Settings, or `X9 WIFI DATA: rx` > 0 with type 0800/0806 traffic).

## HANDOFF 2026-11-07 (43): 95.txt = rev252ck: **AUTH ACKed** (DBDC on); AP answered status 14 = our seq 0; rev252cl BUILT

- Log copy .local/x9/logs/x9-95.txt (52 s, no panic, DeviceLost 0).
- `X9 WIFI DBDC: enable -> answered` (UNI 0x28). 5 GHz grant now dbdc=1. Auth on RUC-Web 5180: TX_DONE status 0
  SUCCESS tx_count 1 delay 9 ms; AP answered `auth resp seq=1 status=14`. Retry on 2432 MHz (grant dbdc 0): three Auths
  all SUCCESS (tx_count 6/1/4), answer again seq 1 status 14. So DBDC was the missing piece for 5 GHz TX acceptance
  (2.4 GHz band-0 TX also ACKed now). Status 14 = transaction sequence out of expected sequence: our Auth body never set
  the transaction sequence number (left 0; the comment claimed 1), and the AP replied with 0+1.
- rev252cl: fr[26] = 1 in the Auth body. Kernel only (plugin 267). boot_sha256
  86358dcc1af3785da08db9ada1b0edbede799aee322fe316f197c8aedf3f4827. Expect Auth resp seq 2 status 0, then Assoc.
- Still no data path after association (see handoff 41).

## HANDOFF 2026-11-07 (42): 94.txt = rev252cj: GPU poisoned by foreground check + alias NoMemory; Auth still unACKed; rev252ck BUILT

- Log copy .local/x9/logs/x9-94.txt (166 s, plugin 267). DeviceLost x26591.
- 38.285 s `X9 PANVK CALL: op=5 (Unmap) errno=13 IOKit=0xe00002c1` -> X9_FAIL line 55. 0xe00002c1 = kIOReturnNotPrivileged
  from externalMethod, which required kIOClientPrivilegeForeground on EVERY call (process not identified in the log).
  59.846 s `submission failed kr=e00002bd` with no `X9 CSF TILER OOM` line -> the other NoMemory in submit:
  x9_render_memory_cache createMappingInTask(kernel_task) of an import failed.
- rev252ck GPU: externalMethod / clientMemoryForType check only the owner task per call (foreground still at
  initWithTask); a non-foreground caller is logged (`X9 RENDER CLIENT: <proc> pid op without foreground privilege;
  allowed`, first 8). Import alias failure now logs `X9 RENDER CACHE: kernel alias of import handle bytes failed` and
  maintains through phystokv per physical segment (getPhysicalSegment, wired by prepare), 64 KiB preemption-off runs.
- Wi-Fi 94: DIS_MAT + TGID (grant dbdc 0 -> TGID 0) + tx 30: still MPDU_ERROR (tx_count 23/29/15), CTS again on try 3
  (rsp_rate 11). All joins since the FEM fix were 5 GHz (iOS gets all -55 dBm and picks 5180). Android joins with DBDC
  enabled (cnmUpdateDbdcSetting Enable, grant band 1, TGID 1); ours never enables DBDC.
- rev252ck Wi-Fi: x9_radio_dbdc() before a join (once per bring-up): UNI_CMD_ID_MBMC 0x28 {4 pad, tag 0 len 8, en 1,
  aa 0, rfband 0, reason 0}, ACK, 20 ms wait; log `X9 WIFI DBDC: enable -> ...`; boot-arg x9_wifi_dbdc=0 skips. If the
  join fails, one retry on an open BSS of the same SSID on the other band (`X9 WIFI JOIN: retry on the other band`):
  separates band/DBDC problems from frame problems in one run.
- BUILT kernel only (plugin 267): boot_sha256 9b8ae411a3552d5763d5f6828162b1092acef4902bd5ecafd7957e00a35cde75.

## HANDOFF 2026-11-07 (41): Android join command sequence mapped; Assoc rates fixed; rev252cj BUILT

- From android-join-2 (RUC-Web join at kmsg.clean 37700-38600), Android's UNI commands in order:
  pre-join: 0x02 BSSINFO len 8, 0x02 BSSINFO full len 260, 0x03 STAREC len 16 (free old), 0x01 DEVINFO activate, 0x02
  BSSINFO activate (PhyMode 0x39 = A|GN|AN|AC), 0x27 CH request; after grant: 0x03 STAREC len 112 (BA offload ampdu 1/1,
  amsdu 1/1 3328, ba 64/64; Bfee; MLR); Auth; Assoc (cap 0x1101 RUC-Web / 0x1111 RUC-Mobile, listen 2); after success:
  0x21 SMART_GEAR, 0x02 BSSINFO full (connected), 0x04 EDCA_SET len 52, 0x03 STAREC state 2 with response, 0x11
  RRM_11K, 0x0C ROAMING, 0x27 CH abort, 0x02 len 12, 0x0E CHIP_CONFIG len 336, 0x63, 0x0C, 0x02 len 48, 0x2C
  POWER_LIMIT, 0x23 GET_STATISTICS. Payload bytes are not in the capture (lengths only).
- Of those, only the join path matters for Auth/Assoc; ours covers BSSINFO pre/post, CH request/abort, STAREC 0/2.
  Not sent (power/QoS/roaming/stats, or HT/VHT): SMART_GEAR, EDCA_SET (we associate legacy, no WMM IE, isQBSS 0),
  RRM, ROAMING, CHIP_CONFIG, POWER_LIMIT, GET_STATISTICS, per-join re-activation with PhyMode 0x39, StaRec Bfee/MLR/
  HT/VHT tags.
- Bug fixed: our Assoc request carried a fixed 2.4 GHz rate set with CCK 1/2/5.5/11 as basic, invalid on 5 GHz. rev252cj:
  cap = ESS | (AP short preamble / short slot), listen 2, Supported/Extended Rates copied from the AP's beacon (OFDM
  default on 5 GHz if none kept); logs `X9 WIFI JOIN: assoc request cap .. rates from ..`.
  Kernel only (plugin 267). boot_sha256 ec084245726187b62dbc2b7f84400e7700bd5f19926a5585766c8365eab7e12e.
- After a successful association there is still NO data path (TX data ring/TXP, RX data frames, 802.11<->802.3 into en0,
  DHCP). That is the next large piece; join success alone will not give an IP address.

## HANDOFF 2026-11-07 (40): Android TXD capture (android-join-2); Auth TXD differences found; rev252ci BUILT

- Capture: Android booted, adb on the Mac. x9/tools/android_wifi_manual_capture.sh (new; gen4m dbgLevel 0xFF:0x13F +
  `set_cfg DataTxDone 3`, 240 s kmsg) while the user joined RUC-Mobile then RUC-Web by hand. The script stalled after
  kmsg when the ssh session ended; remaining steps (debug restore, status, logcat) done by hand. Files (gitignored):
  .local/x9/android-join-2/{kmsg.txt,kmsg.clean,logcat-wifi.txt,status-*.txt} on both machines. Four joins, all
  5240 MHz: RUC-Mobile x2, RUC-Web x2, Auth -> Assoc -> CONN_SUCCESS; final state connected to RUC-Web 11ac.
- Android join on bss 0 / OMAC 0 / AP WTBL #3 (BMC #0 own mac). ChReq dbdc=255 -> ChGrant band=1 (DBDC on: scan uevent
  "DBDC:ENABLE"). BSS activate PhyMode 0x39 (ours 0). StaRec update has BA offload, Bfee, MLR, HdrTrans(skip) tags.
- Android's Auth TXD (kmsg.clean ~38000, connac3x_dump_tmac_info): TxByteCnt 62, EtherTypeOffset 20, pkt_ft 2, QueID
  0x10, MLDID 3, TGID 1, HdrFmt 2, HdrLen 12, TID-mgmt 0, own_mac 0, FixedRate 1, subtype 11, RTS 1, life 30,
  remain_tx_cnt 30, sn_vld 1, pid 44, tx_status_2_mcu 1, DA_source_selection 1, **DIS_MAT 1**, MSDU_count 1,
  Timestamp_offset_index 1, Fixed_rate_index 1, Bandwidth 8, Valid_TXD_arrival_time 0, txd len 0.
- Differences vs ours: DIS_MAT (DW6 b3) missing (gen4m sets it with MSDU_CONTROL_FLAG_FORCE_TX: no MLD-to-link address
  translation of the 802.11 header; fits 92.txt: CTS to our RTS (RA from WTBL) but no ACK to the Auth MPDU), TGID 0 vs 1,
  remain_tx 15 vs 30. (DS_VER 3600: our RTS in DW7 b13 is right; the dump just lists it under TXD_2.)
- rev252ci (kernel only, plugin 267 = rev252ch): DW6 |= DIS_MAT, TGID = grant ucDBDCBand (g[14], 0/1), remain tx 30.
  boot_sha256 0bf7f624b0b3345188e9075a74e75e774ee36f11e4bff2dbc721ee47225d85d0. Stage rev252ch's package if not done
  (`X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252ch`), boot x9/out/x9-rev252ci/boot.img.
- Not done (Android does): DBDC enable at init, BSS activate PhyMode 0x39, StaRec BA/Bfee/MLR tags, Assoc cap/IEs as
  Android (cap 0x1101 RUC-Web, listen 2, HT/VHT IEs).

## HANDOFF 2026-11-07 (39): 93.txt = rev252cg: no tiler OOM; device lost at 96.4 s = unmap 0xe00002c2; rev252ch BUILT (plugin 267)

- Log copy .local/x9/logs/x9-93.txt. Big tiler heap active (backboardd 64 chunks / 128 MiB, others 16); no `X9 CSF TILER
  OOM`, no submission failure. backboardd ~500-540 MiB. Desktop rendered (up to 32 fps windows).
- 96.439 s `X9 PANVK FAILED: line=418 errno=5 detail=unmap native resource: IOKit=0xe00002c2` (x9_pan_kmod_unmap_cpu),
  then every alloc refused and DeviceLost x1376. Preceded by a burst of 14.6 MB allocations every ~7 ms. Same signature
  as the open 67.txt issue. Cause (XNU IOKit behaviour, inferred, not traced on device): IOUserClient::mapClientMemory64 ->
  IOMemoryDescriptor::createMappingInTask without kIOMapUnique reuses a compatible existing mapping in the same task, so a
  second map of a BO returns the same address while the client's mapping OSSet holds it once; the first unmap removes it
  and the second unmap finds nothing -> kIOReturnBadArgument. The 963f runtime (free thread, more map/unmap) hits it
  more often.
- rev252ch (plugin 267): X9PanVKMemory.c x9_bo gains cpu/cpu_maps (zeroed at alloc and host import); map_cpu returns
  the existing pointer with cpu_maps++; x9_pan_kmod_unmap_cpu decrements and calls IOConnectUnmapMemory64 only at the
  last map. boot_sha256 fa0acfcef65b9dcc070a85c0eeb99df372b3c83a76def1612ec11e044db2c3f6. Kernel = rev252cg (tiler OOM
  log, Wi-Fi queue 16 + RTS). Stage `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252ch`.
- Wi-Fi 93: same as 92 (Auth MPDU_ERROR x15, rsp_rate 0 this time, delay 267-319 ms). Android TXD capture still the next
  step (needs Android booting) plus dumping our TXD+frame bytes.

## HANDOFF 2026-11-07 (38): 92.txt = rev252cf: device lost again at 57 s = tiler heap OOM; Auth: RTS/CTS ok, MPDU unACKed; rev252cg BUILT

- Log copy .local/x9/logs/x9-92.txt. backboardd memory now 531-618 MiB (spare cap works) but 56.998 s
  `X9 RENDER ASYNC: submission failed id=990 kr=e00002bd` again -> DeviceLost x7886. So NoMemory was not the budget.
  Source found: X9CSFRenderQueue.cpp x9_csf_group_health returns kIOReturnNoMemory on a CS OOM event (REQ^ACK bit 26) =
  tiler heap exhausted; we have no heap-grow handler (kbase adds chunks). Heap: X9 TILER HEAP log shows every process
  with Mesa's fixed 5 x 2 MiB = 10 MiB (the 128/32 MiB variant needed boot token x9tb/x9vt; boot-args near the 608 B cap).
  The 963f runtime (lowering worker, batching) evidently bins more per pass. 91's failure was most likely the same.
- rev252cg (plugin 266): X9PanVKQueueBridge.inc big heap BY DEFAULT (backboardd 64 chunks = 128 MiB, others 16 = 32 MiB;
  exact token x9ts restores 5); kernel logs `X9 CSF TILER OOM: slot stream REQ ACK` (first 8). boot_sha256
  ea8a82ce42d6c9e360aa743ba81ae7b310220f1931e8c851452f7c9c669917ed. Stage `X9_SERIAL=<serial> python3
  x9/mailbox/update.py x9/out/x9-rev252cg`. Budget: backboardd +118 MiB on ~600 MiB, under 1 GiB.
- Wi-Fi 92 (queue 16 + FORCE_RTS): still TX_DONE status 3 MPDU_ERROR x15 on 5180 MHz ch 36, but attempt 1 shows
  rsp_rate=14: a response (CTS to our RTS) was received, i.e. the AP hears us and we receive it; only the Auth MPDU gets
  no ACK. RTS RA comes from the WTBL (STAREC peer), the MPDU header from our buffer. Re-audited vs gen4m: TXD long 32 B,
  padding 0, PSE header 16, MAC_TX_RESERVED_FIELD 0, header format / length / TID-mgmt / OMAC / TGID / subtype / type /
  lifetime / tx count / PID+TXS / DW6 (msdu 1, DA_SRC_SEL, fixed rate idx 1, FR_BW 8) / DW7 RTS all match. No
  difference left found by reading. Next: capture Android's real Auth TXD (`set_cfg DataTxDone 3`,
  x9/tools/android_wifi_connect_capture.sh) if Android still boots, and dump our TXD+frame bytes as sent.

## HANDOFF 2026-11-07 (37): 91.txt = rev252cd: desktop up, device lost at 70 s (spares); Auth unACKed; rev252ce/cf BUILT

- Log copy .local/x9/logs/x9-91.txt (169 s, no panic, plugin 264).
- Desktop rendered 26-78 s (up to 73 presents / 2 s); import refusals 2 (was 2618), SpringBoard no longer crashing.
  GPU scope now skips 83-93 MB pre / 35-45 MB post per job (ranges ~231 with no-copy registration; hold 0, refused 0).
- 70.329 s: `X9 RENDER ASYNC: submission failed group=1 id=1253 kr=e00002bd` (kIOReturnNoMemory) -> PanVK errno 5
  -> backboardd DeviceLost on every command buffer / import after it (2040 lines), presents 0 after 78 s. The only
  NoMemory in the submit path is the import cache alias (createMappingInTask in x9_render_memory_cache). backboardd
  native bytes grew to 892 MB (RENDER LIVE 850 MiB, old30s 381 MiB) vs ~430-660 MiB on the pan205 runtime: the new
  runtime's spare image blocks have no byte limit and go only on age (checked lazily) or memory pressure (never
  delivered here). Kernel NoMemory path itself not instrumented yet.
- rev252ce (plugin 265, 963f tree c29b31aa, 0100 regenerated): native devices age spares on every keep and keep at
  most NATIVE_SPARE_BYTES 64 MiB (oldest freed first). boot_sha256 4c11214241fe0f55384c72aa4d14be093557a694baa382e7d8fa16b23ad04206.
- Wi-Fi 91: scans keep working with 5 GHz networks (RUC-Web/RUC-Mobile on 5180-5825 MHz, 23 networks). Join RUC-Web
  a4:be:2b:b1:df:b1 5180 MHz: BSSINFO (ch 36, rates 3fc0, basic 0540) answered, CH grant ch 36 band 2 status 0, STAREC
  answered, 3 Auth: TX_DONE status 3 MPDU_ERROR tx_count 15 rate 6M txpwr 44. Firmware probe requests ARE answered, so
  RF works; our Auth TXD is the suspect. Audit vs gen4m nic_txd_v3_compose / auth.c: all fields match except queue
  (gen4m Auth sets FORCE_TX | MGNT_2_CMD_QUE -> MCU_Q0 | ALTX = 16; we used 17 = MCU_Q1 | ALTX) and FORCE_RTS
  (nicTxSetForceRts -> DW7 b13 for DS_VER >= 3500; we had none). TGID 0 (grant dbdc 0), TXD_LEN 1 page 0 match.
- rev252cf (commit after these notes' parent): queue 16 + DW7 FORCE_RTS_CTS. Kernel only, plugin 265 (rev252ce's).
  boot_sha256 c355d073953b736b374fa3cd4138bdd264aeb9d35b9da293cb0d553f846356a4. Stage rev252ce's package
  (`X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252ce`), then boot x9/out/x9-rev252cf/boot.img.

## HANDOFF 2026-11-07 (36): 90.txt = rev252cc: **Wi-Fi TX WORKS** (probe responses to our MAC); no desktop (runtime merge bug, fixed in rev252cd)

- Log copy .local/x9/logs/x9-90.txt (116 s, no panic, plugin 263 = metal2vulkan 963f0e64).
- Wi-Fi (kernel = rev252cb features): PMIC query answered (pmic_id 18e8); COM_FEM (618 B) answered status 0 in 4 ms;
  CAL FORCE (816 B) answered event 0x81 status 0 in 1299 ms, EMI result f9610000/442844; PHY + RAM start ok; DOMAIN
  passive + domain answered; NVRAM 37/37. Scans 1-3: fw beacons 34/22/48, our parser beacons 28/23/40, **probe
  responses 19/11/26 with DA = our MAC 02:58:39:57:46:01**, networks 17/14/26 (was 4-7). So the radio now transmits on
  air: the missing piece was PHY_ACTION COM_FEM (external FEM QM42655/QM45655) + CAL. 19 scans; no join attempted (no
  desktop, so no Settings). RX4 dump: 291-byte packets, rxd0 38010123.
- No desktop: backboardd "surface import refused: no-copy buffer: Unsupported(HostMemory, NotRepresentable)" x2618 and
  "buffer refused: OutOfMemory(Device)" x285; SpringBoard crashed x15 (sig 11). Cause: my merge (handoff 35) used
  upstream's free memory::candidates() in upstream's new module-level import_host and in place(), which do not pass
  the negotiated submission-cache-ownership flag, so only HOST_COHERENT types qualified; PanVK on Mali has none for host
  imports. Our Memory::candidates (ownership-aware, x9) was bypassed there. Both now call memory.candidates(...);
  other x9 submission_cache_atom uses are all present (same per-file counts as pan205).
- rev252cd (commit after c804e84): 963f tree 2a844520, 0100 patch regenerated, stamp updated; `build.py all --revision
  264 --name x9-rev252cd` + base args + x9_cache_scope=1. boot_sha256
  1f0c8b49247a073604e3a5bc960f6eca1ba08ef05256a41e515e2419d2bf72f3. NEW PLUGIN (264):
  `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252cd`. Fallback with a known-good runtime: rev252cb
  (plugin 262, same kernel features incl. the Wi-Fi fix).

## HANDOFF 2026-11-07 (35): metal2vulkan upgraded to 963f0e64 (UniXNU 642c4fef series); rev252cc BUILT (plugin 263); commit b16f334

- User chose the full upgrade. New runtime tree: .local/x9/work/x9-metal-render/metal2vulkan-963f-source (git; branches:
  x9-963f = 963f0e64 -> 4209371f AdrenoMetal ce63701 0002..0030 (25, all applied clean) -> 2d6d9ac9 our changes ->
  2c59fa14 merge fixes; am-old-219f / pan205-local / x9-only kept for reference). pan205 tree untouched (fallback).
- How our changes were isolated: pan205 = 219f0136 + AdrenoMetal 4599584 0001-0004 (the version that applies clean to
  219f) + ours. x9-only = pan205 crates (metal2vulkan, -runtime, -air) over 219f+AM0001-0004: 103 files +3529/-600
  (vertex capture, sample-position replay, raw linear shadows, native split depth/stencil, depth/stencil resolve +
  scratch, counter samples, sampled-source copies, memoryless resolve backing, submission cache atom, x9_timing, ...).
  Cherry-picked onto 4209371f: 23 files / 50 hunks resolved by hand (scratchpad resolve.py logic):
  * taken from upstream/AM where they supersede ours: canon prune_entry (our entry_refusal dropped), mtlb Core Image
    libraries, growing memory blocks + spare image blocks + free thread, import_host as module fn, in_place/policy docs.
  * kept ours on top: submission_cache_atom (DeviceOptions, Debug, equality), allocation alignment to the cache atom,
    native release_kept reclaims every pool and every spare, commit_ns/x9_timing (commit_as + submit), raw shadow
    access in shadow uses + upstream clear early-return, raw_texel checks in texture views, vertex_capture /
    sample_replay_safe fields (codec, pipeline, interface; rebound.rs Command gets vertex_capture: None), vertex
    capture ordering check before upstream's reaching/prefix note, sample loop around upstream's recording (+ settled:
    None, through_record), A1BGR5Unorm write mask applied to upstream's write_mask, SamplePositionReplayDraw error.
  * cache versions: SEMANTICS 3001973, ARTIFACT 3000489, RUNTIME 3001775 (old device shader caches invalid).
- Repo: x9/build/third_party/metal2vulkan-963f/{adrenometal-ce63701/ (25 patches, SERIES, MIT LICENSE),
  0100-x9-local-changes.patch (git diff 4209371f..2c59fa14, 99 files)}; verified: pristine 963f0e64 + SERIES + 0100
  == the tree (diff -r clean). build-general-metal.py: default --runtime-source is the 963f tree; for it, applies the
  series once and keeps DIR/.x9-patches-applied (sha256 per patch); pan205 still builds with --runtime-source.
  x9/build/scripts/prepare-metal2vulkan-963f.py clones pristine 963f0e64 for a fresh machine.
- Bridge (NativeMetalObjects.rs, cfg x9_runtime_latest which the build always sets): as AdrenoMetal ce63701 -
  options.policy.render_lowering = Worker, PipelineRecords file (pipeline-records-<program>.bin in the shader cache dir)
  + device.warm(earlier, MAX), nmb_command_buffer_commit -> commit_and_submit. Compiles with 0 warnings (1.98.0).
- BUILT (Mac): `build.py all --revision 263 --name x9-rev252cc` + base args + x9_cache_scope=1 (kernel = rev252cb:
  Wi-Fi PHY_ACTION/domain/NVRAM, GPU scope). boot_sha256 64a02a25f3cfa61e9d9c3e62c935dd26ee33aec447b33597743b9a3245fc4f1e.
  Plugin bundle 34.7 MB (262: 32.9 MB). NEW PLUGIN (263): `X9_SERIAL=<serial> python3 x9/mailbox/update.py
  x9/out/x9-rev252cc`. Not device-tested. Fallback: rev252cb (plugin 262, pan205 runtime), same kernel features.
- Risks to watch: first boot recompiles every shader (cache versions changed: slower first UI), render lowering on a
  worker changes error timing, commit_and_submit changes submit thread; any rendering regression -> compare with
  rev252cb.

## HANDOFF 2026-11-07 (34): UniXNU 642c4fe (metal2vulkan -> 963f0e64) assessed; not merged yet (scope decision pending)

- UniXNU commit 642c4fef is a pin bump: steelbrain/metal2vulkan development 963f0e64 (Nov 5) + AdrenoMetal ce637016
  with its translator patches ported (30 -> 25 entries: dropped 0001, 0008, 0010, 0012, 0028; 0027 renamed
  bounded-image-reuse). AdrenoMetal's own bridge change for the bump was small (NativeMetalObjects.rs +97 lines,
  6416a76..ce63701: lowering thread, commit submission, warm pass).
- Our runtime: .local/x9/work/x9-metal-render/metal2vulkan-pan205-source = metal2vulkan 219f0136 (master, Oct 29) +
  AdrenoMetal 0001-0004 (.applied markers) + our pan194-pan205 local runtime/translator changes + repo patches 0023,
  0025, 0026. Diff vs pristine 219f0136 (Mac reference copy): crates/metal2vulkan 44 files / ~1.5k changed lines,
  metal2vulkan-runtime 57 files / ~2.7k lines, metal2vulkan-air 5 files (validation crates also differ, not built).
  219f0136..963f0e64 = 763 commits (624bab39..963f0e64 = 805); runtime crate alone +74k/-7k lines upstream.
- Merging means rebasing all of the above onto 963f0e64 and re-qualifying on the device (PanVK, not Turnip): the
  AdrenoMetal-origin patches come ported, our Mali-specific local changes must be separated from them and re-ported by
  hand. Waiting for the user's choice (full migration vs. selected patches) before starting.

## HANDOFF 2026-11-07 (33): rev252cb BUILT = Wi-Fi domain + GPU scope for no-copy buffers (plugin 262); commit c72e4fa

- Wi-Fi (on top of rev252ca PHY_ACTION): x9_radio_domain() before the NVRAM fragments, as gen4m rlmDomainSendCmd
  (non single-SKU): UNI 0x15 passive-scan table (arSupportedRegDomains_Passive default: 52-64, 100-144 passive), then
  domain (NVRAM map 0 = by country, country 0 -> default group: 1-13, 36-48, 52-64 DFS, 100-144 DFS, 149-177; no 6 GHz),
  bw2g CONFIG_BW_20M, bw5g 20/40, ACK requested (`X9 WIFI DOMAIN:`). Boot-arg x9_wifi_domain=0 skips.
  Deliberately not sent: ANT power (Android "ANT Config #0" = empty table), country power limit (EMI type; limits only
  lower power; Android ucCountryPwrLimitCtrl 0), cnmPowerControl, kalSyncTimeToFW (CCIF). Later Android sets country CN.
- GPU: 88 skipped only 14-16 of ~63 MB because most of the ~140 imports are not IOSurfaces. Frontend now registers
  newBufferWithBytesNoCopy imports (page-rounded base/bytes, unregistered in NBBuffer dealloc) and acquires textures
  over a registered buffer even without a surface (rev252bx/by skipped those textures: they were never acquired, but
  they were also never registered, so nothing was wrongly skipped). Kernel `X9 RENDER cache-uncovered:` counts imported
  CPU-exposed BOs no registered range covers (imports_avg, bytes_avg, largest).
- BUILT (Mac): `build.py all --revision 262 --name x9-rev252cb` + base args + x9_cache_scope=1. boot_sha256
  98bcc55fc84cfe151d9e2c168bd10746328e646ab53fb6e8448d6b5c450ac00e. NEW PLUGIN (262): stage
  `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252cb`. Not device-tested; rev252ca (kernel-only, 261)
  is superseded unless the user wants the Wi-Fi PHY change alone.

## HANDOFF 2026-11-07 (32): 89.txt = rev252bz (NVRAM accepted, RX changed); external FEMs found; rev252ca BUILT

- 89.txt (rev252bz, copy .local/x9/logs/x9-89.txt): NVRAM 37/37 fragments answered (CMD_RESULT 2e .. 03 01 / 00).
  After it rx4 frames per scan 4-7 -> 36-49 but beacons 0, and the FIRMWARE's own SCAN_DONE "fw beacons" 0 (was 5-7):
  RX genuinely changed, not our parser. No join possible (no networks listed).
- Root-cause candidate (strong, from data): the device has EXTERNAL FEMs. Live DT (Mac .local/x9/work/x9-battery/
  captured.dts line 23039, copy in .local/x9/wifi-nvram/connfem.dts, gitignored): connfem node, production node `sku`
  (connfem reads `sku-mtk` only when connfem_is_internal()) = hw-0 "qm42655_qm45655_was": QM42655 (2.4 GHz, pins 3 2 4 14),
  QM45655 (5 GHz, pins 10 11 12 1), 4 layouts (g-wf0/wf1 qm42655, a-wf0/wf1 qm45655), no spdt, fe-ant-cnt 0x85.
  The firmware only drives them after PHY_ACTION COM_FEM, which we never sent.
- Android pre-RAM sequence (android-pull dmesg 14.539-14.590, after the patch): kalSyncTimeToFW (MT6653 = CCIF, skipped),
  TX CMD 0x22 LEN 72 (QUERY_INFO PMIC), 0x08 LEN 682 (PHY_ACTION COM_FEM, wlanSendPhyActionV2), 0x08 LEN 880
  (PHY_ACTION CAL, wlanSendPhyAction V1 with CONNFEM: TLVs CAL, NVRAM ePA/eLNA, COM_FEM v1 zero, LAA zero).
  Verified our encodings reproduce those lengths: ePA/eLNA = NVRAM tags 7,8,9,21,0x81 = 632 B -> CAL 816+64;
  connfem SKU tag (gen4m _AddConnfemSkuTag, 2 FEMs x 4 layouts) = 606 B -> COM_FEM 618+64.
- rev252ca (commit 9a17da5): x9_wifi_pre_phy() after the patch, before PHY RAM code: PMIC query, COM_FEM V2, CAL V1
  FORCE_CAL (0x01; Android may have used USE_BACKUP 0x02 with its EMI cal backup). Event 5 decoded (`X9 WIFI PHYACT:`
  ... status, emi, temp). Boot-arg x9_wifi_phyact=0 skips. x9_wd_cmd waits 10 s for cid 0x08. RX4: first 2 frames of
  the first 3 scans dumped (`X9 WIFI RX4:`). Data: x9/tools/wifi_connfem.py (DTS -> tag) used by wifi_nvram.py, output in
  gitignored generated/x9_wifi_nvram.inc. NVRAM fragments (rev252bz) still sent after RAM start.
- BUILT (Mac): `build.py all --name x9-rev252ca` (kernel only, plugin 261) + base args + x9_cache_scope=1.
  boot_sha256 076552cfcf865748ab3f4b29b3aa38f10fa2e16ada08c8c270472491def1c648. Not device-tested.
- Still NOT done from Android's flow: SET_DOMAIN_INFO (passive scan info + domain V2 active channel list, UNI 0x15),
  power limit (EMI type) + ANT power setting, cnmPowerControl, set_country_code. Next if TX still dead.

## HANDOFF 2026-11-07 (31): 88.txt = rev252by by mistake (user flashed the wrong image); rev252bz still untested

- Log copy .local/x9/logs/x9-88.txt. No `X9 WIFI NVRAM` line at all (rev252bz always logs one) -> rev252by kernel;
  user confirmed the wrong image. Wi-Fi unchanged: probe responses 0, Auth TX_DONE MPDU_ERROR x15 (x3).
- GPU, scope fix WORKS: hold_jobs=0, table used=9 registers=51 refused=0 unreg_miss=0; pre/post skip 14-16 MB per job.
  pre_cache 3.8 -> 3.2-3.3 ms, post 1.7 -> 1.0 ms; wait 3.2-6.8 ms (87: 11.6-15.3); presented 36 fps in busy windows
  (87: 19-23.5). Only 14-16 MB of ~63 MB skipped: the rest are imports the frontend does not register (not IOSurface
  textures of this process: next to identify which BOs they are).
- Next: user boots rev252bz (NVRAM). Full Android init (domain V2, power limit EMI, ANT power, PHY_ACTION COM_FEM/CAL,
  PMIC query, time sync, cnmPowerControl) still to do; gen4m entry points: common/wlan_lib.c wlanAdapterStart (1375,
  ready bit at 1566), os/linux/gl_init.c 5141 (wlanLoadManufactureData), wlanOnPostAdapterStart (8405).

## HANDOFF 2026-11-07 (30): Wi-Fi TX not on air -> missing NVRAM; rev252bz BUILT (NVRAM + rev252by scope fix); commit 0caf603

- Evidence TX never reaches the air: 86 Auth TX_DONE MPDU_ERROR tx_count 15 (x3), 87 probe responses 0 in every scan
  while beacons arrive. Our own MAC 02:58:39:57:46:01 (locally administered, valid).
- Android init steps we never did (android-pull-20261106-132120/dmesg.txt, grep -a; the file has binary bytes):
  cnmPowerControl, rlmDomainGetDomainInfo/oplus{Passive,Active}DomainInfo ("no matched country code, default
  domain"), wlanLoadManufactureData ("TLV is Null, last index = 37" = 37 NVRAM fragments), kalSyncTimeToFW,
  rlmDomainSendPwrLimitCmd (EMI type) + rlmDomainSendAntPowerSetting, later set_country_code CN. Earlier notes also list
  PHY_ACTION 0x08 (COM_FEM 682 B, CAL 880 B) and QUERY_INFO 0x22 (PMIC) as skipped.
- First experiment = NVRAM (calibration/TX power, most likely to gate TX). gen4m (common/wlan_lib.c
  wlanLoadManufactureData, MT6653 is_support_nvram_fragment=TRUE): fragment 0 = file offset 0 .. end of first TLV
  (ucTypeID0 at 256), then one TLV {id, u16 le len, data} per fragment until a zero tag; each sent as
  UNI_CMD_ID_NVRAM_SETTINGS 0x2E {4 pad, tag 0 len 1504, 1500 B buffer}. Our split of the 11266 B record gives exactly
  37 fragments (max 719 B), matching Android.
- Data handling: x9/tools/wifi_nvram.py (committed, no data) writes x9/kernel/generated/x9_wifi_nvram.inc (gitignored)
  from .local/x9/wifi-nvram/WIFI (copied from the Android pull; also on the Mac, gitignored); stub when absent;
  build.py runs it next to wifi_firmware.py ("wifi_nvram" in local.json overrides the path).
- Kernel: x9_radio_nvram() before x9_radio_activate (after RX4 setup), ACK requested per fragment, logs first 2 /
  last / failures + `X9 WIFI NVRAM: N/37 fragments answered`. Boot-arg x9_wifi_nvram=0 skips.
- BUILT (Mac): `build.py all --name x9-rev252bz` (kernel+boot; plugin 261 from rev252by) with base args +
  x9_cache_scope=1. boot_sha256 5893c01bf609ca2a76d80da8df185a9554632e97cc082f36786e2b7cd327d4ee.
  Needs plugin 261 on the device (stage rev252by's package if not done). Not device-tested.
- If NVRAM alone does not bring probe responses / Auth ACK: next are SET_DOMAIN_INFO (UNI 0x15, V2 active channel list),
  power limit (EMI) + ANT power setting, then PHY_ACTION COM_FEM/CAL.

## HANDOFF 2026-11-07 (29): 87.txt = rev252bx (scope ON) - no effect: HoldAll on ~every job; rev252by BUILT (fix)

- Log copy .local/x9/logs/x9-87.txt (user flashed only the ON image; "no difference" by feel). Scope clients opened
  (backboardd 17.3 s, SpringBoard, mediaserverd). cache-frontend-scope: 22.7 s skip 24 MB pre/post with hold 8/69;
  from 26.8 s hold_jobs ~= jobs (62/82, then 80/87 .. 85/85) and pre/post skip 0. So nothing was skipped.
  Cause (inferred from code, not logged): HoldAll was set by commits whose surface had scopeBytes==0 (registration
  failed). Registrations fail once the 1024-entry table is full; rev252bx matched Unregister/Acquire by address AND
  bytes, so any length mismatch leaked entries (and a leaked entry could even cover a later unregistered import).
- Same log: pre_cache 4.4-4.8 ms, post 1.9-2.5 ms, wait 11.6-15.3 ms per job (40-58 jobs/2 s); presented 19-23.5 fps.
  wait is now the largest item.
- Wi-Fi 87: every scan has probe responses 0 (beacons 4-7 only). With 86's Auth MPDU_ERROR x15 this points at the
  radio not transmitting on air at all (or the firmware scan being passive). TX path (PA/FEM/TX calibration/power) is the
  next Wi-Fi suspect; not investigated yet.
- rev252by (commit e8811c4): kernel table matches (task, address) only, regs/refs counted separately, Acquire of an
  unknown address creates a placeholder (regs 0) so Release balances, only regs>0 ranges can justify a skip, any refusal
  (table full) disables skipping for the boot; log adds table used/registers/refused/unreg_miss/placeholders/holds.
  Frontend acquires each referenced surface by its own base address (IOSurfaceGetBaseAddress + page-rounded alloc)
  even if its registration failed; HoldAll only for heaps, unmatched surface buffers, or a failed acquire.
  BUILT `build.py all --revision 261 --name x9-rev252by` (+x9_cache_scope=1), boot_sha256
  0b4931c1052d6283a179b19bf4a4fb445bd9f724213ade0862795702b1a6904b. NEW PLUGIN (261): stage
  `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252by`. Not device-tested.

## HANDOFF 2026-11-07 (28): rev252bx BUILT = frontend cache scope (plugin 260); A/B images; commit 4f26005

- Goal: 60 fps (16.7 ms/frame). 84-86 busy windows: ~1 backboardd CB per frame, kernel FIFO serial, run_avg 10-19 ms/job
  of which cache maintenance 5.3-5.7 ms (pre 3.5-3.9 on 63 MB + post 1.6-1.8 on 31 MB). 86 measured each CB references
  ~13.9 MB of IOSurfaces (heap_cbs=0). Cutting maintenance to the referenced surfaces is the largest single lever.
- Mechanism (no change to PanVK/runtime):
  * Kernel: X9RenderResource.host_address (ImportHost). Type-1 user client = scope-only (no engine context, no memlimit),
    op X9RenderScope=14 {abi, op, address, bytes}: Register/Unregister/Acquire/Release/HoldAll/ReleaseAll in a
    1024-entry table keyed by task (SweetX9RenderService.cpp); entries of a task go when its scope client closes.
    Submit takes one snapshot per job; pre and post loops skip an imported cpu-exposed BO only if x9_cache_scope=1, no
    hold, a registered range covers it and no overlapping range has refs. Log `X9 RENDER cache-frontend-scope:
    active pre_skip_avg post_skip_avg ranges_avg hold_jobs`.
  * Frontend: surfaceBuffer registers (base, page-rounded alloc); import release unregisters; commit (before submission)
    acquires every registered import referenced (textures via .buffer, views share it); HoldAll if a referenced
    surface/buffer has no registered range or a heap is referenced; finish releases only on safeRelease (completed /
    failed locally); uncertain ownership keeps them held (more maintenance, never less). Seam: nmb_cache_scope in
    bridge/NativeBackendProbe.mm -> x9_render_scope in X9RenderTransport.c (boundary check clean).
  * Not covered (risk): runtime-internal submissions that touch an IOSurface outside a frontend CB (none known).
    Wrong skip would show as stale/partial layer content, not a crash.
- BUILT (Mac): `build.py all --revision 260 --name x9-rev252bx` (base args + x9_cache_scope=1), boot_sha256
  f1d80801f29ce9611f02c363935b0ba56545bee1ff7b35d45c67388714885f22; and `build.py boot --name x9-rev252bx-off` (same
  kernel, no x9_cache_scope), boot_sha256 4d3215fdad6be0fc987ec5ebaa173395ba9fd640f04f9e8e45a1897c459bb310.
  Kernel includes rev252bw Wi-Fi diagnostics. NEW PLUGIN PACKAGE (260):
  `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252bx` before booting either image. Not device-tested.
- What to read: cache-frontend-scope pre/post skip (expect ~45-50 MB pre, ~17 MB post skipped), RENDER timing pre/post
  (expect ~1 ms each), METRICS fps/latency vs the -off image, any stale UI content (user visual check).

## HANDOFF 2026-11-07 (27): 86.txt = rev252bv run (plugin 259); Auth never ACKed; GPU scope measured; rev252bw BUILT

- Log copy .local/x9/logs/x9-86.txt. 46 s, no panic.
- GPU measurement (backboardd, 2 s windows 27-44 s): per CB 1-7 IOSurfaces (avg 1-2 after 31 s), per_cb_kib_avg
  13.6-14.2 MB (one full-screen surface ~13.8 MB + small ones), window union 2-10 surfaces / 27-42 MB,
  heap_cbs=0 and buf_refs=0 in every window (list looks complete: no heaps, no surface-backed buffers).
  Same windows kernel: import_pre_avg 63 MB (pre_cache 3.5-3.9 ms), import_post_avg 31 MB (post 1.6-1.8 ms);
  kernel jobs/2 s (35-84) ~= backboardd CBs (39-77). So ~4.5x more pre bytes and ~2.2x more post bytes than referenced.
  Estimate (proportional, unproven): scoped maintenance -> pre ~0.8 ms, post ~0.8 ms, saving ~3.5-4 ms/job.
- Wi-Fi: grant ok (ch 5), STAREC answered, 3 Auth attempts: every TX_DONE status=3 MPDU_ERROR (no ACK), tx_count=15,
  rate 0x004b (OFDM 6M), txpwr 20, delay 87-156 ms. So the frame is attempted 15 times and the AP never ACKs (or our
  receiver never sees the ACK). DEVINFO own-MAC layout checked against gen4m: matches. rx4 still 0 during waits.
- rev252bw (kernel only, plugin 259 unchanged): scan line also counts probe responses (fc 0x50) and their first DA;
  join line logs our own MAC. Probe responses with DA = our MAC => radio TX works on air and the Auth problem is
  specific; none => TX path not on air (calibration/FEM/channel) suspect.
  boot_sha256 eee78494bd1d0681fe7577358cf4728a382612f23c15fc02786a07c6a3daa402. No staging (259 already on device).

## HANDOFF 2026-11-07 (26): rev252bv BUILT = plugin 259 (IOSurface reference measurement) + rev252bu kernel; commit 95d26ee

- GPU cache maintenance is the biggest CPU cost per job (84/85, backboardd busy windows): pre_cache 3.5-4.0 ms on ~63 MB,
  post_cache 1.6-1.8 ms on ~31 MB, every job; cache-reach shows import_pre_avg == import_pre_full_avg (no reduction):
  every imported IOSurface mapped in the process (~140, 60 MiB live) is CIVAC'd per job. ACE-Lite is shelved (64.txt),
  so maintenance stays; the lever is a narrower per-job scope. Not changed yet.
- Measurement (frontend, logging only): at commit (before submission) each CB's _resourceOwners are scanned for
  IOSurface textures and surface-backed buffers. Every ~2 s with the commit->submit line:
  `X9 FRONTEND surfaces: window_ms command_buffers with_surfaces per_cb_avg per_cb_max per_cb_kib_avg per_cb_kib_max
  window_union window_union_kib tex_refs buf_refs heap_cbs live_imports live_import_kib` (per process).
  Compare per_cb_kib_avg with kernel `X9 RENDER cache-reach import_pre_avg` (~63 MB). heap_cbs > 0 = list may be
  incomplete for those CBs (heap members not enumerated).
- BUILT (Mac): `build.py all --revision 259 --name x9-rev252bv` + rev252bs boot args (park/async). Kernel includes
  rev252bu (TX_DONE decode, 3 auth tries). boot_sha256 3d3f36c306d3e064db38cbc2ec89bfa130f3bd8ceb241d2cdf7af70c122aeb35.
  NEW PLUGIN PACKAGE: `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252bv` before booting (not run).
  rev252bu (kernel-only) is superseded by this. Not device-tested.

## HANDOFF 2026-11-07 (25): 85.txt = rev252bt run; rev252bu BUILT (TX_DONE decode, 3 auth tries); commit 6a39d0c

- Log copy .local/x9/logs/x9-85.txt. 43 s, no panic, park+async on.
- Join RUC-Web a4:be:2b:7d:1c:41 2432 MHz (33.72 s): BSSINFO pre-join answered (rates 3fcf, basic 0003, dtim 1);
  CH grant (real parse now) tag 0 status 0 ch 5 band 1 width 0 dbdc 0 interval 4000 = as asked; STAREC answered;
  Auth queued; 17 ms later rx7 eid 0x2d (UNI_EVENT_ID_STATUS_TO_HOST = TX_DONE for our PID) len 80, payload not logged.
  No auth response in 1.5 s (rx4 packets 0). CH abort sent. Scans 8-10 after the join (41-42 s) still return 6-9 beacons:
  radio no longer looks wedged after a failed join (83 had 0-frame scans).
- rev252bu: decodes the TX_DONE tag (`X9 WIFI TXDONE: status=N (SUCCESS / MPDU_ERROR(no ACK) / ...)`, tx_count, rate,
  delay) and sends Auth up to 3 times (600 ms each). Kernel only (plugin 258 unchanged, no staging).
  boot_sha256 0b66f3807b587866304a339be8967a69a234259887db1963a0808cd541c66982. Not device-tested.
  Reading: SUCCESS + no response -> our RX path drops the Auth reply; MPDU_ERROR -> AP never ACKed (TX side).

## HANDOFF 2026-11-07 (24): rev252bt BUILT = Wi-Fi join commands aligned with gen4m/android-join-1; commit 3acdb30

- gen4m source IS local: /home/ubuntu/android_kernel_modules_and_devicetree_oppo_mt6993/vendor/mediatek/kernel_modules/
  connectivity/wlan/core/gen4m (handoff 21's "no local source has the abort encoding" was wrong).
- Found by comparing with android-join-1/dmesg.txt 11890-12104 + gen4m:
  (1) our CH grant parse read UNI_EVENT_CNM padding (ev[46]/ev[47]); 83/84 "CH grant status=0 ch=0" meant nothing.
      Grant tag is at ev[48] (Android got ChGrant ch=6 for ch 6).
  (2) Android's Auth TXD uses a fixed rate (Rate mode 1 = MANUAL_DESC, RateIdx 1 = FIXED_RATE_INDEX_OFDM_6M, FR_BW 8)
      and PID 32 + TXS_TO_MCU (-> EVENT_TX_DONE, seen 50 ms later status=0). Ours had neither, so the frame used the
      WTBL rate of widx 1 that our partial STAREC set (guess: possibly never transmitted / bad rate).
  (3) STAREC rates were wrong: basic 0x00ff and desired 0x0fff set PBCC 22M/33M bits; phy type 0x07 had 5G OFDM on 2.4G.
  (4) Android sends a full BSSINFO (nicUpdateBssEx: BASIC, RLM+PROTECT+IFS, RATE, SEC, QBSS, MLD...) before the CH request
      and after the association (BSSID, ConnState 0, StaRecIdxOfAP); we sent none. Android releases the channel
      (CH_PRIVILEGE_ABORT) after every join, success included.
- rev252bt changes (X9WifiNet.inc): TXD DW1 b31 fixed rate, DW5 PID 0x20 | b9 TXS_TO_MCU, DW6 rate idx 1 + FR_BW 8, b28
  cleared (MT6653 CFG_WIFI_TX_FIXED_RATE_NO_VTA=1); full grant log + "NOT as asked" warning (join continues, for data);
  x9_radio_ch_abort (cid 0x27 tag 1 len 12 bss 0 token 3 dbdc 0xff, send-only) after every attempt; x9_radio_bssinfo
  pre-join (disconnected, BSSID 0) and post-assoc (connected); STAREC rates/basic/DTIM from AP IEs, PHY 11BG, AID,
  + BA_OFFLOAD and UAPSD tags; WAIT logs the first 6 rx7 packets (len, pkt type, eid) so EVENT_TX_DONE (0x0f) shows.
  Not yet sent (Android does): EDCA/WMM params, MLR_INFO tag, re-activation per join, HT tags.
- BUILT (Mac): `build.py all --name x9-rev252bt` (kernel+boot only, no --revision; manifest stays plugin 258) with the
  rev252bs boot args. boot_sha256 3b9ca23342a602238e13248e4da82e602952f1f9c0ab6e07646e947ba819c459.
  NO new plugin package: device already has 258 from rev252bs. Not device-tested.
- What to read in the next log: `X9 WIFI JOIN: BSSINFO pre-join`, `CH grant tag=.. status=.. ch=..`, `STAREC state 0 ...`,
  `X9 WIFI WAIT: rx7 ... eid=0x0f` (TX done -> Auth left the radio) and whether rx4 packets > 0 after the grant.

## HANDOFF 2026-11-07 (23): 84.txt = rev252bs device run (plugin X9GeneralMetal258, park+async on)

- Log copy: .local/x9/logs/x9-84.txt (gitignored). 50.6 s, no panic, no device lost. Boot args match rev252bs.
- FRONTEND commit->submit now populated (fix confirmed). backboardd steady state (2 s windows, 27-48 s):
  41-82 command buffers, avg 0.7-6.1 ms, max 1.3-33 ms. Highest when the GPU is busiest (27-35 s: avg 3-6 ms, max ~29 ms,
  same windows as RENDER wait_us 14-16 ms); lowest when idle (42 s: avg 0.67 ms, max 1.25 ms). SpringBoard: few CBs,
  avg 1-7 ms. Cause of the multi-ms commit->submit (scheduler waits on earlier work vs. vkQueueSubmit cost) NOT known.
- Display busy windows (28-34 s): 31-41 fps presented, 0 dropped, swap->present avg 24-30 ms, max 44 ms, stutter 0.
  Lighter windows (40-46 s): 20-28 fps, avg 12-20 ms.
- GPU per job (async run_avg 8-20 ms): wait 2.4-16 ms (largest under load), pre_cache 1.4-4.0 ms, post_cache 1.0-1.8 ms,
  start ~0.15 ms; FIFO queue_avg 16-20 us; max job 16-39 ms. pre_cache rose 1.4 -> 3.8 ms after 35 s at similar job counts.
- MESA "WSI pitch not properly aligned" x123 (also x190 in 83.txt; pre-existing, not investigated).
- Wi-Fi join RUC-Web a4:be:2b:b1:df:a1 2432 MHz at 39.27 s: CH grant status=0 ch=0 (39.560, after eid 0x76/0x89/0x27),
  STAREC answered (39.569), auth queued on TX15 (DMA consumed). WAIT subtype 11 timed out 1500 ms: rx4 packets 0,
  rx7 frames 1, rx7 events 0. Scans 7-10 just before got 8-11 beacons/scan on rx4, so after the grant NOTHING arrives on rx4,
  not even beacons. Guess (unproven): the radio is not receiving on the BSS channel after the grant (grant reports ch=0),
  or a BSS activate / BSS info update UniCmd that gen4m sends before STAREC is missing. No scan after the join in this log.
- Crashes: syncdefaultsd x6, assetsd x2, driverkitd, WirelessRadioMan (as before).
- Next: Wi-Fi - compare our join command sequence with android-join-1 (BSS_ACTIVATE / BSS_INFO / channel fields) before
  STAREC; GPU - find where commit->submit waits (split commit->scheduler pick vs. vkQueueSubmit duration).

## HANDOFF 2026-11-07 (22): rev252bs BUILT = plugin 258, commit->submit timing audited and fixed

- Audit of the rev252bq/br frontend timing (runtime patch 0026) against the Mac runtime source found:
  (1) a command buffer split into several batches was recorded once per batch (group.rs members loop and
  submit.rs both fired per batch), so counts were inflated and avg pulled down by early batches;
  (2) group.rs recorded even when vkQueueSubmit failed; (3) frontend label still said "group submissions only",
  window was printed as a fixed 2 s, and the static window timestamp was raced by several committing threads.
- Fix: record only for the batch that ends the command buffer (`Submitted::last`; new `Submission::ends_buffer()`),
  only on Ok, with `commit_ns.swap(0)` so each buffer counts at most once. Frontend line is now
  `X9 FRONTEND commit->submit: window_ms=<real> command_buffers= avg_us= max_us=` (per process; atomic CAS window).
  Other GPU timing lines (X9 METRICS frames/latency/join, X9 RENDER timing, X9 RENDER ASYNC window) checked in
  83.txt and look correct; unchanged.
- Mac runtime tree: old 0026 reverse-applied, new 0026 applied by the build. Pushed origin main (f1f8766); Mac repo
  fast-forwarded eac9383 -> f1f8766 (scp copies were identical to HEAD; backup /tmp/ios_sync_backup_20261107/files.tar on
  the Mac). Mac keeps its local edit of x9/kernel/generated/x9_render_external_manifest.h (generated, untouched).
- BUILT (Mac): `build.py all --revision 258 --name x9-rev252bs` + rev252br boot args (park/async on); the boot step
  failed first on a python without lz4, re-run with /Library/Frameworks/.../3.13/bin/python3 `build.py boot`.
  boot_sha256 575cf2c5dfeefaa540281fa48255dab6035c825274bb0c95ce7012e54c5e2a10. Kernel identical in function to rev252br
  (Wi-Fi diagnostics included). Note rev252br's build.log shows a first failed attempt (E0433 x9_timing) then a good rerun.
  NEW PLUGIN PACKAGE: `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252bs` (not run; device write).
  Not device-tested. rev252br is superseded (never staged).

## HANDOFF 2026-11-06 (21): rev252br BUILT = plugin 257 (submit-site timing) + Wi-Fi diagnostics; commit 1df4281

- BUILT (Mac, via ssh): `build.py all --revision 257 --name x9-rev252br` (args as bq + park/async). Output x9/out/x9-rev252br.
  NEW PLUGIN PACKAGE: stage before booting, serial from the user: `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252br`.
  Not run (device write). Not device-tested.
- (a) Frontend timing now also covers the submit-alone path: runtime patch 0026 hooks submit.rs (own vkQueueSubmit return,
  after group.submit). Patch regenerated; forward-applies to pristine, reverse-applies to the patched tree. Mac runtime tree
  is patched by build-general-metal.py (0026 wired in; script copied to the Mac this time).
- (b) X9WifiNet.inc: `X9 WIFI WAIT: subtype N timed out ...` (rx4 packets, rx7 frames, rx7 events, last eid) on mgmt wait
  timeout; `X9 WIFI JOIN: auth queued on TX15 (DMA consumed the descriptor; air transmission not confirmed)`;
  scan CMD_RESULT (eid 0x01) now logged like other scan events.
- (c) Found: join never releases its channel privilege. Android (android-join-1/dmesg.txt:12046-12047) on join timeout runs
  aisFsmReleaseCh -> cnmChMngrAbortPrivilege -> nicUniCmdChAbortPrivilege. The abort UniCmd bytes are not in the capture and no
  local source has them, so NOT implemented (guessing a firmware command could wedge the radio). Logged as
  `X9 WIFI JOIN: failed; CH privilege token 3 is not aborted`. Next: recover the abort encoding (gen4m source) or confirm
  with the user before trying one on the device.
- Wi-Fi 83.txt follow-up still needed: next RUC-Web run should show the WAIT summary and the auth-queued line.

## HANDOFF 2026-11-06 (20): 83.txt = rev252bq device run; RUC-Web join reaches Auth, no response; park/async A/B

- Log: /tmp/83.txt (plugin X9GeneralMetal256, park+async on). Copy to .local/x9/logs/x9-rev252bq-83.txt before cleanup.
- Wi-Fi join RUC-Web (open) at 36.316 s: CH grant status=0 ch=0 (36.605), STAREC answered (36.613), Auth sent via
  x9_tx_mgmt (no 'auth TX stuck' log), no auth response in 1.5 s -> 'no auth response', station result 6 (38.117).
  STATS `tx=0/0` is fake::stats (fake radio counter), NOT the real radio: it does not prove the Auth never left.
  Open: did the Auth TX complete, and what did firmware events eid 0x76/0x89/0x27 (36.604) mean.
- Scans before the join: 1-9 OK (beacons 1-6, RUC-Web/RUC-Mobile/eduroam-like SSIDs). After the join: scan 10 (52.6 s) 4 frames 0
  beacons, scans 12 and 13 0 frames (timeouts). Radio likely wedged after the failed join (unproven).
- FRONTEND commit->submit: command_buffers=0 in every window, so the group submit path was never hit. The rev252bq
  timing covers nothing; submit_alone (and any other submit path) must be timed too.
- Park+async vs sync (82 vs 83, 2 s windows): presented fps 19.0 -> 23.0, dropped 31 -> 11, stutter 1 -> 0,
  but avg swap->present latency 9.6 -> 20.5 ms (frames queue behind the fence), max 99 -> 49 ms.
  Busy windows us/job: pre_cache 3411 -> 3714, wait 6594 -> 7695 (sync-style measure), post_cache 1813 -> 1744.
  Async worker: queue_avg 16-24 us, run_avg 0.7-13.6 ms per job.
- Next (not done): (a) time submit_alone path; (b) log auth TX completion and the firmware events after STAREC;
  (c) check radio state after a failed join.

## HANDOFF 2026-11-06 (19): rev252bq BUILT = plugin 256 with commit->vkQueueSubmit timing + park/async

- BUILT (Mac, via ssh): `build.py all --revision 256 --name x9-rev252bq` with rev252bp args (x9_gpu_park=1 x9_submit_async=1).
  Output x9/out/x9-rev252bq (boot.img, plugin-package). Not device-tested. boot_sha256 in x9/out/x9-rev252bq/boot.json.
- NEW PLUGIN PACKAGE: stage before booting, serial from the user: `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252bq`.
  Not run yet (device write, needs the user's go-ahead).
- Runtime change: metal2vulkan-runtime patch x9/build/third_party/metal2vulkan-pan205/0026-x9-commit-submit-timing.patch
  (added to build-general-metal.py list). It adds commit_ns to CommandBufferShared (set at commit_as), crate module x9_timing
  (now_ns/record/take), and records commit -> vkQueueSubmit return at the group submit sites (group.rs queue_submit).
  Forward-applies to pristine, reverse-applies to the patched tree. Coverage: group submissions only; submit_alone is not timed.
- Frontend: NativeBridgeMetalFrontend.mm logs `X9 FRONTEND commit->submit: window_s=2 command_buffers= avg_us= max_us=` every 2 s
  via nmb_x9_submit_timing (NativeMetalObjects.rs/.h).
- Wi-Fi RUC-Web failed (user). Still no disk/console log of that attempt. Needed before any Wi-Fi diagnosis.

## HANDOFF 2026-11-06 (18): metal2vulkan source found on the Mac (not in the server checkout)

- Location (Mac): .local/x9/work/references/metal2vulkan-219f01365dc4c8eb346816c405b5b17403793764/crates/metal2vulkan-runtime
  (pinned ref 219f0136...). Also a patched copy in .local/x9/work/ios_plj110-gpu-integration/x9/build/third_party
  (01-metal2vulkan-x9.patch) and x9-metal-render/pan194-original-local-diffs.
- Commit path: NativeMetalObjects.rs:1733 nmb_command_buffer_commit -> runtime src/command_buffer/mod.rs:2424 commit()
  -> commit_as (returns after queuing; actual submit deferred to the scheduler group) -> submit.rs:340 submit_batch
  -> VkQueue submit in PanVK -> X9PanVKSync.c:378 x9_pan_kmod_submit_async (kernel enqueue).
- Commit->enqueue timing needs: a commit timestamp carried on the command buffer (commit_as), and a log at submit_batch
  or at the PanVK submit. That edit goes into the vendored runtime and therefore the 01-metal2vulkan-x9.patch, plus a
  new plugin revision and mailbox staging. Not done yet; awaiting decision.

## HANDOFF 2026-11-06 (17): frontend commit->enqueue timing BLOCKED on source; Wi-Fi RUC-Web failed (no log yet)

- Frontend commit path: NativeBridgeMetalFrontend.mm commit -> nmb_command_buffer_commit (NativeMetalObjects.rs:1733)
  -> metal2vulkan_runtime `commit()`. That crate is NOT in this repo (no Cargo.toml path, no source under x9/ or .local/),
  so backend commit -> PanVK queue -> x9_pan_kmod_submit_async (X9PanVKSync.c:378) cannot be instrumented here.
- Only the synchronous handoff (time inside nmb_command_buffer_commit) could be timed in the .mm now. That is not commit->enqueue.
  A plugin change also needs a new plugin revision (kernel allowlist) and a mailbox stage on the device.
- Wi-Fi: user says joining RUC-Web (open) failed on the rev252bn-era build. No new log in /tmp or .local/x9/logs.
  Need: disk sidecar + console pulled after that attempt, and which build was on the device.

## HANDOFF 2026-11-06 (16): rev252bp BUILT = park+async + kernel FIFO timing; commit 1f6f03c

- BUILT (Mac via ssh): x9/out/x9-rev252bp/boot.img, boot_sha256 a875f20b41a023d2f104b35d8bf4e367454fbc1d149ee9d79d2457233d25d9e8.
  Args: rev252bo set + x9_gpu_park=1 x9_submit_async=1. Plugin on device was X9GeneralMetal255 in 82.txt (async-capable).
  Not device-tested.
- Added `X9 RENDER ASYNC window:` every 2 s (SweetX9RenderEngine.inc): queue_avg_us (enqueue -> worker pop), run_avg_us
  (pop -> render_submit return), enqueue_to_done_max_us, jobs, failed. Kernel FIFO only.
- NOT done: frontend commit -> enqueue timing lives in the plugin (NativeBridgeMetalFrontend.mm ctc, status line only,
  not logged). Needs a plugin change and a new plugin package to stage. Swap -> present timing is already in X9 METRICS.
- Wi-Fi: user reports still cannot connect on rev252bn-era build. No new log yet (only 82.txt in /tmp). rev252bn implements
  open-network join only (RUC-Web); WPA2 networks (RUC-Mobile, eduroam) are not implemented, so those fail by design.
  Need the disk sidecar and console from that attempt before any diagnosis.

## HANDOFF 2026-11-06 (15): 82.txt = rev252bo device log: metrics work; sync GPU path (park/async OFF)

- Log copy: .local/x9/logs/x9-rev252bo-82.txt (gitignored). 65 s boot, no kernel panic, boot-args from launchd: no x9_gpu_park,
  no x9_submit_async (logs: "resident group ... off (x9_gpu_park)", "async submission off").
- Display, steady state 24-64 s (presents on a 120 Hz target, software counters): presented 15-28 fps (avg ~20);
  swap->present avg 6-10 ms, max 41 ms; 0 frames >= 50 ms; dropped 0-13 per 2 s (13 at 52 s). Presented tracks swaps,
  so the rate is set by the client's swap rate, not by the display path.
- First presents at 16 s: 2 frames, avg 56 ms, max 99 ms (first-use cost). Before UI: tick lateness up to 234 ms (4.4 s), 85 ms (8.4 s).
- GPU per job, sync path: pre_cache 3.4-4.5 ms (largest), wait 2.5-11.5 ms (rises with load), post_cache 1.6-2.3 ms,
  start 0.21 ms, retire 0.2 ms; max job 9-36 ms. Cache maintenance (pre+post) is about 5-6.5 ms per job.
- Join fields gpu_enqueued/completed/shown_gpu_seq are all 0 here because async is off, so the join is untested.
- Crashes (24 CRASH lines): syncdefaultsd sig6 x8, WirelessRadioMan sig5 x7, assetsd sig6 x4, MusicWidgets sig11 x3,
  driverkitd sig9 x1, PosterBoard sig10 x1. No SpringBoard crash in this log. IONVRAM serializeVariables ret 0xe00002d8 at 18.2 s.
- Metrics gaps seen: no interaction marker (one touch at 65.158 s), so idle and static screens look the same.
- Next (not done): A/B with x9_gpu_park=1 x9_submit_async=1 to populate the join and compare pre/post cache costs.

## HANDOFF 2026-11-06 (14): rev252bo BUILT = P3 metrics (logging only); commit f93f7b0

- Added X9 METRICS to the 2 s display cadence block (SweetFramebuffer.cpp, commit f93f7b0): per window swaps,
  presents, presented_fps, dropped (replaced before shown), swap->present latency histogram (<8/<17/<34/<50/<100/>=100 ms),
  avg/max, stutter_ge50, and the GPU fence seq of the last shown frame joined with async enqueued/completed.
  Latency is measured per shown frame from swap arrival, so idle desktop does not inflate it.
- BUILT (on Mac, via ssh): x9/out/x9-rev252bo/boot.img 11,685,888 B, boot_sha256
  48742b6a0bda2d1bbb654d59f8d354c13f75a77957eeab9c57331403d947abfc. Same args as rev252bn. Kernel only, no mailbox.
  Not device-tested. Build used the SweetFramebuffer.cpp copied via scp (commit f93f7b0 is NOT pushed to origin).
- NOT done from P3: GPU-timestamp -> stage join (no GPU timestamp source wired), per-interaction A/B markers,
  static-desktop vs lock-screen rows (need a user-driven marker), cache-build-id/warmup progress (no shader priming
  exists, P1-C), RX event retention/dropped count (Wi-Fi side, not measured here).
- Next: device run of rev252bo; read `X9 METRICS` lines in the disk log to get the first real frame/stutter numbers.

## HANDOFF 2026-11-06 (13): rev252bn BUILT via ssh (Mac eac9383); GPU optimization list

- Mac tunnel was blocked by a stale sshd session (pid 750636) holding 127.0.0.1:2222 on the server; killed it,
  then autossh reconnected (user, 2026-11-06).
- Mac repo fast-forwarded 752ac58 -> eac9383 (x9_render_external_manifest.h left locally modified, untouched).
- BUILT: x9/out/x9-rev252bn/boot.img sha256 bf0e1c17c403edcc506286d058f02a05d5faed021e4271068477cf9335795600
  (11,685,888 B), kernel only, no mailbox. Args: x9_wifi_power=6 x9_wifi_real=1 x9_gpu_khz=1716000
  x9_gpu_floor_khz=1100000 + x9_wifi_xo0..6 as in rev252bm (x9_metal_render=1 is already in boot.json, do not pass it).
  Not device-tested.
- GPU optimization headroom (docs/graphics/gpu-optimization-headroom.md section 4), ranked:
  P0-A decouple submit from completion (wait ~51%: IRQ/event fence instead of polling, 2 jobs in flight; -3..5 ms/job, high risk);
  P0-B cache maintenance by proof (~38%: coherency probe, then drop CIVAC/reach walks; -2..4 ms/job, top correctness risk);
  P0-C stop per-job AS teardown (ranged FLUSH_PT/FLUSH_MEM, keep AS bound; high risk, needs PT correctness test);
  P1-A BO pool with aging (no per-alloc full-buffer scrub; also removes the 1,024-mapping / 512 MiB ceilings);
  P1-B batch bind, 2 MiB pages, O(1) handles; P1-C shader pre-warm (stutter, not throughput);
  P2-A transient/memoryless attachments; P2-B damage-aware present / direct scanout (one 13.9 MB copy per frame);
  P2-C worker priority, shorter CSG suspend waits, DVFS from real utilisation; P3 metrics first.
- Device state from earlier notes: ACE-Lite shelved (x9_gpu_acelite off); park+async are the stable GPU path.
  Known open GPU issue: 67.txt IOConnectUnmapMemory64 refused (0xe00002c2) -> device lost at 63.95 s, not investigated.
- Next: device test of rev252bn (open-network join, see handoff 12); GPU work only after P3 metrics exist.

## HANDOFF 2026-11-06 (12): **SETTINGS SHOWS REAL NETWORKS** (81.txt = rev252bm); join not implemented

- USER: Settings Wi-Fi scans and lists the real networks; joining fails (expected: sta_connect returns
  RES_UNSUPPORTED in real mode). 实测 81.txt: 13 scans; scan 1: 3 frames, 3 beacons, 3 networks:
  RUC-Mobile (caps 0431, privacy), RUC-Web (caps 0421, OPEN), eduroam (0431) - all 2437 MHz.
- RX7 scan frame layout (hex dumps): 802.11 header at packet offset 0x58; RXD groups before it; RCPI bytes at
  0x44 (e.g. 69 54 ff ff, i.e. header - 20): dBm = (rcpi - 220) / 2 (not yet used; signal fixed -55).
- Join source reading (gen4m): management frames (Auth/Assoc) are sent on the COMMAND ring as TXD + 802.11 frame
  (nic_tx.c COMMAND_TYPE_MANAGEMENT_FRAME -> HAL_WRITE_TX_CMD, TXD long/short per nic_txd_long_format_op); join =
  aisFsmStateInit_JOIN (bssCreateStaRecFromBssDesc, cnmStaRecChangeState STA_STATE_1, auth type) after a CNM
  channel-privilege grant (aisFsmRunEventChGrant), then saa_fsm Auth/Assoc. Android debug knobs (adb root):
  `echo 0xFF:0x13F > /proc/net/wlan/dbgLevel` (all modules, ERROR..INFO + DEBUG, no LOUD) and
  `echo "set_cfg DataTxDone 3" > /proc/net/wlan/driver` (connac3x_dump_tmac_info of every mgmt TXD).
  Capture script: x9/tools/android_wifi_connect_capture.sh OUTDIR (run on the Mac with bash; streams /proc/kmsg
  while `cmd wifi connect-network RUC-Web open`).
- Next: open-network join (RUC-Web) = channel privilege, BSSINFO/STAREC updates, host Auth/Assoc mgmt frames,
  then the data path (TX ring + RX data) and DHCP; WPA2 after that.

## HANDOFF 2026-11-06 (11): 80.txt = rev252bl: frames still classified as events; rev252bm

- 实测 /tmp/80.txt (rev252bl): radio up 23.9 s, scans done in ~600-680 ms, fw beacons 3-10 per scan, but the
  252-371 B RX7 packets still went down the event path ("scan event eid=0x00"): they have RXD type 7 like events.
- rev252bm: every RX7 packet during a scan except eid 0x0e (SCAN_DONE) / 0x01 (CMD_RESULT) goes to x9_scan_take
  (which validates the 802.11 mgmt header itself); first 3 dumped. BUILT via ssh (source 752ac58): Mac
  x9/out/x9-rev252bm/boot.img sha256 e5c44bc02cbdf6b73e212f0b7de19ec6c8885fce1d1b70256bf6b334bd071887 (usual args +
  x9_wifi_real=1 + XO args, wlan ADT node), kernel only, no mailbox.

## HANDOFF 2026-11-06 (10): 79.txt = rev252bk: scans complete; frames come on RX7; rev252bl

- 实测 /tmp/79.txt (rev252bk): en0 up at 6.3 s (MT6653 radio label), wifid scans answered with 0 networks until
  the radio is up; FW READY 23.9 s, NIC cap ok, RX4 ring e2710000/32, **DEVINFO + BSSINFO answered (CMD_RESULT)**,
  radio up 23.95 s. Scans: **SCAN_DONE after ~590-670 ms, channels 26** (Android 0.6 s), fw beacon counter 2-7,
  wifid re-scans every ~0.6 s. But rx4 frames 0: the scan results arrive on **RX7** as packets of 252-362 B that
  the loop logged as "scan event eid=0x00 opt=00" (i.e. not RXD type 7 events; frames).
- rev252bl (commit after 1bca7b9): RX7 packets with (rxd0 >> 27) != 7 go to x9_scan_take(); first 3 dumped
  (`X9 WIFI RADIO: rx7 frame type ...` + hex rows). Between scans such frames are dropped. BUILT via ssh (source
  3fc4296): Mac x9/out/x9-rev252bl/boot.img sha256 2a446118d25847f865a497379d78de828dce22475e780db933d4b9587c98aa98
  (usual args + x9_wifi_real=1 + XO args, wlan ADT node present), kernel only, no mailbox.

## HANDOFF 2026-11-06 (9): 78.txt = RAM-mode commands work; rev252bk = real radio for Settings (goal: real SSIDs)

- 实测 /tmp/78.txt (rev252bj): UNI_CMD_ID_CHIP_CONFIG NIC_CAPABILITY answered in 1 ms (UNI_EVENT 0x43, 588 B,
  29 TLVs). MAC tag = 00:00:00:00:00:00 (eFuse empty; Android takes the MAC from NVRAM WIFI); tag 0x14 a-die
  8a016653; tag 0x25 sw-sync EMI 0x1c8000 and tag 0x1f host-suspend 0x200607c4/c8 match Android's dmesg.
- Android dmesg also shows the pre-PHY init: QUERY_INFO 0x22 (PMIC), PHY_ACTION 0x08 x2 (682 B COM_FEM, 880 B CAL
  with EMI cal backup) - we skip those (likely why our RAM WIFI_START takes 1.7 s vs Android 154 ms).
- Android scan (dmesg SCANLOG): BSS0 activate DevInfo[OMAC=0,DBDC=255] BssInfo0[WMM=3, ConnType=65537,
  ConnState=1, BcIdx=0, PhyMode=0]; ScanReqV2 ScanType=1, SSIDType=1 Num=0, ChannelType=4 Num=26 (1-13,
  36-64, 149-165), Func=0x11, random MAC, BSSID 0 -> SCAN_DONE ~0.6 s, 22 BSS.
- iOS side already works with the fake radio (29.txt: Settings toggle + list). rev252bk (commits a few after
  cc886de, last d3d6c50): boot-arg x9_wifi_real=1 = real backend behind the same station interface:
  en0 + wlan ADT node also for x9_wifi_real=1 (boot-xnu.py); sta_scan -> radio thread when realUp (else 0 networks
  via the fake timer); join -> RES_UNSUPPORTED for now. Stage 6 after FW READY + NIC cap calls
  x9_wifi_radio_loop() and never tears down: HW RX ring 4 (32 x 2 KiB at LK pool +0x10000, WFDMA 0x540..),
  DEVINFO (cid 1) + BSSINFO BASIC/MLD (cid 2) with ACK, then per scan request UNI_CMD_ID_SCAN_REQ 0x16 (opt 0x06)
  tags SCAN_REQ/BSSID/CHANNEL/MISC; collects RX4 packets (beacon/probe-resp found by searching for the mgmt
  header, RCPI not decoded: signal fixed -55), waits for UNI_EVENT_ID_SCAN_DONE 0x0e (8 s cap), fills rbss[32],
  calls scan_done -> wifid reads results. Logs `X9 WIFI RADIO: ...` incl. every network found. EMI text scan now
  opt-in (x9_wifi_emitext=1). BUILT via ssh (source abae90a): Mac x9/out/x9-rev252bk/boot.img sha256
  8265dfeeb0ce7ff6daae8ff28a6af348fdafcf24bafeddc3a2ed91d55bc41638 (usual args + x9_wifi_real=1 + XO args; ADT has the
  wlan-pcie,mt6653 node), kernel only, no mailbox.
- Unknowns/risks: whether FW delivers scan frames on HW RX4 (else look at RX5/6/8 or RX7 pkt type), whether
  DEVINFO/BSSINFO need more tags, whether SET_DOMAIN_INFO is needed before 5 GHz active scan.

## HANDOFF 2026-11-06 (8): **Wi-Fi FIRMWARE RUNNING, READY BIT SET** (77.txt = rev252bi)

- 实测 /tmp/77.txt (x9-rev252bi, calibrated XO via x9_wifi_xo0..6): PLATCFG XO reads back ca9f0019 000095bf 0
  80800000 00008080, custom 00158000. Patch downloaded (161 ms). PHY image: 4 regions mode 80000019 status 0
  (147008 B in 6 ms), **WIFI_START override 8 -> status 0 "PHY STARTED"** (8 ms, same as Android). RAM: 6 regions
  status 0 (21 ms), **WIFI_START {1, 0091c000} answered status 0 after 1.73 s -> "RAM CODE STARTED"**, then
  **ep cfg 0x48c = 30cfbc2e (sync 3), conn_on_misc = 3 -> FW READY after 0 ms**. RX7 didx 1 at that point.
- EMI after start (11 blocks changed, no assert text): +1a8000 = fw log header (00000040 0000bf80 0000c000 00003f80;
  Android: WF_WM base 0x1a8040 len 0xbf80, WF_MCU 0x1b4000 len 0x3f80), +1b0000.. fw log data, +1c0000..+210000 new
  data, sw-sync +1c8000 area.
- Root causes, in order: (1) MCU EMI base/size, (2) conninfra sysram plat cfg (XO NVRAM + connv3 option) -> patch;
  (3) PHY RAM code + WIFI_START(CRC_CHECK) before N9 RAM code -> RAM init completes.
- rev252bj (commit ad70f2a): x9_wd_unicmd() = connac3 UNI command (TXD 32 + 16-byte UNI header {u2Length=total-32,
  u2CID, 0xA0, seq, option b1 UNI/b2 SET/b0 ACK}, payload at 48; events: RXD 32 + WIFI_UNI_EVENT 12, data at 44,
  skips unrelated events up to 32) and x9_wifi_nic_cap(): UNI_CMD_ID_CHIP_CONFIG 0x0E opt 0x02 payload {4 rsv, tag 3
  len 4} -> UNI_EVENT_ID_CHIP_CAPABILITY 0x43, logs every TLV (`X9 WIFI CAP:`; tag 7 = MAC). Boot-arg x9_wifi_cap=1.
  BUILT via ssh: Mac x9/out/x9-rev252bj/boot.img sha256 bd15a62d1ae75fbd331404bdc8ec4658865a5041e633bcff98ce218ef41824cb
  (same args incl. calibrated x9_wifi_xo0..6).
- Next: talk to the running firmware (RAM-mode commands: connac3 WIFI_CMD / UNI_CMD on TX15, events RX7): first
  NIC capability query (MAC, caps) as gen4m wlanQueryNicCapabilityV2, then keep the chip up instead of powering
  it off at the end of stage 6, then a real driver (interrupts, scan, data path; hakios-mt6768 redwood_mtkwifi.c is a
  CONNAC1 reference for that layer).

## HANDOFF 2026-11-06 (7): Android pull -> PHY firmware goes BEFORE the N9 RAM code; rev252bi

- Android pull (user connected adb to the Mac; on the server in .local/x9/android-pull-20261106-132120/, gitignored:
  CONNXO 29 B (calibrated XO NVRAM, differs from the vendor default), WIFI 11266 B, WIFI_CUSTOM, dmesg.txt (+
  dmesg.clean), logcat-all.txt). Pull script: zsh on the Mac does not word-split, run it with bash.
- 实测 Android dmesg (gen4m at DEBUG, 2nd probe at 12.63 s): wlanAccessCalibrationEMI Offset 0x1610000 Size 0x6c1dc
  backup(1); **WIFI_MT6653_PHY_RAM_CODE_dx5_2g2a_1.bin: 4 regions, TX CMD ID 0x01 x4, ID 0x02 -> "FW_START EVT
  success" (8 ms)**; then WIFI_RAM_CODE: 6 regions ID 0x01 x6, ID 0x02 -> FW_START success after 154 ms;
  "Ready bit asserted" immediately; fw log EMI headers WF_MCU 0x1b4000 / WF_WM 0x1a8040; SwSync EMI offset 0x1c8000
  (matches the 10ff8a55 seen in 74.txt); NIC capability query etc. No PHY_ACTION (ID 0x08) TX line visible in that
  probe. So my "PHY_FWDL off for MT6653" reading of Kbuild was wrong.
- PHY image tailer: 4 regions 86400000 59216, 86600000 23504, 86800000 43472, 86a00000 20816, all feat 01, no entry.
  gen4m wlanDownloadPhyFw: mode = wlanGetDataMode(PHY) = 80000009 | WORKING_PDA_OPTION b4 = 80000019; WIFI_START
  {override START_CRC_CHECK b3 (| b0 if entry), addr 0}.
- rev252bi (commit after 9fa16d6): x9_wifi_dl_image() shared region downloader; PHY image + WIFI_START(8, 0)
  (boot-arg x9_wifi_phyfw=1) before the N9 RAM + WIFI_START; XO NVRAM words overridable via boot-args
  x9_wifi_xo0..x9_wifi_xo6 (build with the calibrated CONNXO words, generated from the .local pull - values not in
  git). BUILT via ssh (source 9c08d40) with the calibrated XO args (confirmed in shim/embedded-payload.bin
  boot-args): Mac x9/out/x9-rev252bi/boot.img sha256 bd1fd63967899c0e77efb7dee37de84edc7ea703e587c59bbf9e494d7cc560e3,
  kernel only, no mailbox. Build: `python3 -I x9/build/build.py --name x9-rev252bi --boot-arg
  x9_wifi_power=6 --boot-arg x9_gpu_khz=1716000 --boot-arg x9_gpu_floor_khz=1100000` + the 7 x9_wifi_xo args.

## HANDOFF 2026-11-06 (6): 76.txt = rev252bg: RAM code accepted, WIFI_START unanswered, N9_DONE only; rev252bh diag

- 实测 /tmp/76.txt (x9-rev252bg): patch downloaded again (161 ms). RAM: all 6 regions DOWNLOAD_CONFIG status 0, data
  1624992 B in 22 ms. WIFI_START (cid 2 {1, 0091c000}) **no event in 10 s** (RX7 cidx=7 didx=11: 12 free
  descriptors, nothing written; events stay on HW RX7 in RAM mode per mt6653RxRingSwIdx2HwIdx). Afterwards
  conn_on_misc 1 -> 2, EP cfg 0x48c = 20cfbc2e (b29:28 = 2), i.e. WIFI_FUNC_N9_DONE (b1) set, WIFI_FUNC_INIT_DONE
  (b0) missing (ready = BITS(0,1) = 3). EMI +0 header: after patch 00000008 0 0 0 0 00001ad0 00000007 "_ROM" ->
  after start 0000000a 00000002 0 0 00002726 00001ad0 00000007 "_ROM". rom_sram dead1234.
- 推断: RAM code runs (N9 up) but its init does not complete / does not send the START result. Skipped gen4m
  pre-RAM steps: time sync (MT6653 = CCIF notify, not an init cmd), asicConnac3xQueryPmicInfo, wlanPhyAction
  (PHY_ACTION TLV with NVRAM ePA/eLNA + connfem data, then CAL FORCE/BACKUP) - phyAction is the main suspect.
- rev252bh (commit after 90510a3): after WIFI_START polls for x9_wifi_ready_ms (default 20000) ep cfg 0x48c
  (sync b30:28, own b14), conn_on_misc, EMI +0 words 0-7 and WFDMA RX didx 0-13 (0x50c + n*0x10), logs every change
  (`X9 WIFI RAM: +N ms ...`); then scans the whole EMI for ASSERT/assert/xception/panic/FATAL/fail text
  (`X9 WIFI EMI after WIFI_START: text ...`). BUILT via ssh (source 325c123): Mac x9/out/x9-rev252bh/boot.img
  sha256 9d4e2a79f14b50cd34e352eeeef5887ecaec16b93777a3cf953ea07402467882, same args, kernel only, no mailbox.

## HANDOFF 2026-11-06 (5): **ROM PATCH DOWNLOADED** (75.txt = rev252bf); next = RAM code

- 实测 /tmp/75.txt (x9-rev252bf): PLATCFG before = 16 zero words; XO NVRAM default written (reads f8980019
  0000a8c1 0), custom option 0x7c05b2c8 = 00158000. sem get 2, both sections, then **`PATCH_FINISH answered
  status=0 ... 386240 B in 161 ms -> ROM PATCH DOWNLOADED`**, sem release status 3, `after patch
  rom_sram=dead1234 romcode_index=dead1234`. EMI after FINISH: only 2 blocks changed: +000000 = 00000008 0 0 0 0
  00001ad0 00000007 4d4f525f ("_ROM") and +010000 = 7c000000 00000008 7c000010 00000008 ... (table of chip
  addresses).
- So the missing pieces were (rev252bd/bf): MCU EMI base/size (mt6653SetupMcuEmiAddr) + conninfra sysram plat cfg
  (wlanCopyPlatCfgToSysram: CONNXO XO NVRAM + connv3 custom option). Which of XO/custom was essential is not
  isolated (x9_wifi_xonv=0 / x9_wifi_platcfg=0 would tell).
- rev252bg (commit after b9db309, X9WifiNet.inc): after a successful patch (and sem release) downloads the RAM code:
  tailer of WIFI_RAM_CODE_MT6653_dx5_2g2a_1.bin = chip 0x19, fmt 2 flag 1, 6 regions: 0091c000 15760 feat 21
  (VALID_RAM_ENTRY|ENC), 02212800 352208, 0040c500 47824, e005d800 384976, e00bc800 666576, e0161800 157648 (all
  feat 01 = AES key0 -> mode 80000009), data back to back from 0, 160 B release-info after. Per region cid 0x01
  DOWNLOAD_CONFIG {addr,len,mode} + data on TX16; then cid 0x02 WIFI_START {1, 0x0091c000} (10 s wait); polls EP
  cfg 0x48c b29:28 == 3 (MT6653 CFG_MTK_WIFI_ON_READ_BY_CFG_SPACE) up to 5 s; EMI walk "after WIFI_START".
  Boot-arg x9_wifi_ram=1. gen4m also does kalSyncTimeToFW, queryPmicInfo and phyAction (PHY_ACTION cal) before the
  RAM code - skipped for now. PHY firmware: CFG_MTK_WIFI_SUPPORT_PHY_FWDL unset for MT6653 -> 0 (not downloaded by
  gen4m). BUILT via ssh (source 90510a3): Mac x9/out/x9-rev252bg/boot.img sha256
  11ac4f532b29f0de672f0dce2691816095a11237701eb03c52344a5cb4589ada, same args, kernel only, no mailbox.
- Next: RAM code (WIFI_RAM_CODE, 1625428 B) = gen4m wlanConnacFormatDownload (tailer regions, DL_TO_EMI regions via
  wlanDownloadEMISectionViaDma) + wlanConfigWifiFunc (WIFI_START), then the PHY firmware.

## HANDOFF 2026-11-06 (4): 74.txt = rev252bd: EMI set -> chip writes host EMI after FINISH, still no event; rev252be

- 实测 /tmp/74.txt (x9-rev252bd, no panic): EMI regs set (before emi_addr=4000), 167/192 EMI blocks nonzero as LK
  left them (Android leftovers). sem get 2, both PATCH_START ok, data sent, PATCH_FINISH no event in 10 s
  (RX7 cidx=15 didx=3, rom_sram/romcode_index 1d1e, R2A dmawr_probe 100c0000, PCIe/AER/AXI clean), sem release no
  event. **After FINISH 7 of 192 EMI blocks changed**: +000000, +010000, +1a0000, +1b0000 became all zero;
  +1c0000 (first nonzero now +1c8000 = 10ff8a55 = MT6653_WIFI_OFF_MAGIC_NUM ...), +230000 (+230004 = 0x24),
  +250000 (+255c00 ...). Nothing on the AP owns that memory -> 推断: the chip (patch code) now runs after FINISH
  and touches its EMI, but then does not send the FINISH event. (Unknown whether changes happened in the data
  phase or after FINISH; snapshot is only before/after.)
- Source: gen4m wlanAdapterStart calls wlanCopyPlatCfgToSysram right before patch download ("should be called before
  patch download"); CFG_SUPPORT_XONVRAM=y for MT6653 (configs/MT6653/mobile/defconfig). It writes conninfra sysram
  0x7c05b28c (64 B, bar 0x1ab28c): XO NVRAM blob from offset 0 (Android userspace writes it via wlanXonvBufHandler;
  content/length unknown to us), and connv3_get_plat_config() (4 B) at the end = 0x7c05b2c8: u16 ext_32K_ticks
  (connv3 DT 0x8000), byte2 = 1 | co_clock<<1 (DT 0) | uvlo 1<<2 | rising uvlo 1<<4 = 0x15 -> word 0x00158000.
  (OPLUS_FEATURE_CUSTOMER_CLK may change co_clock detection - unverified.)
- rev252be (commit 946a7bb): logs `X9 WIFI PLATCFG before:` (16 words) and writes 0x00158000 to 0x7c05b2c8
  (boot-args x9_wifi_platcfg=1 default, x9_wifi_platcfg_val=<word>). Post-data alive probe renamed to
  x9_wifi_aliveprobe (x9_wifi_probe already meant x9_wifi_probe_hw). BUILT via ssh (source 1a97156): Mac
  x9/out/x9-rev252be/boot.img sha256 3552e789b9171721921069dd955d9f7ed091ec187be94b7c3e469c30bdb5f634, args
  x9_wifi_power=6 x9_gpu_khz=1716000 x9_gpu_floor_khz=1100000, kernel only, no mailbox.
- rev252bf (commit 6a01e7d): also writes the XO NVRAM = CONNXO NVRAM record at 0x7c05b28c (boot-arg x9_wifi_xonv=1
  default). Source: Mac ~/output = extracted vendor partition (user, 2026-11-06): lib64/libwifinvram.so sends it as
  "WR-BUF:XONV" via /dev/wmtWifi (-> gen4m wlanXonvBufHandler); lib64/libcustom_nvram.so stConnxoCfgDefault
  (27 B @ file 0x6e758) = 19 00 98 f8 c1 a8 + zeros, iFileCONNXOLID = 0x53. Default used (phone's calibrated
  /mnt/vendor/nvdata/APCFG/APRDEB/CONNXO not read yet). BUILT via ssh: Mac x9/out/x9-rev252bf/boot.img sha256
  42c267fac80d8a568ff2396c5a8286f187234063508edcd799a01846f51c3a44 (same args; boot this instead of be).
- TODO: get the XO NVRAM blob from Android (dmesg "Copy N bytes from xo nvram", the HAL's source file under
  /mnt/vendor/nvdata) and write it at 0x7c05b28c too; diff EMI per word between data phase and FINISH.

## HANDOFF 2026-11-06 (3): rev252bc PANICKED (my mapping bug); rev252bd built

- 实测 rev252bc (user booted it; USB log incomplete). Disk ring pulled on the Mac with
  `python3 -I x9/tools/pull_disk_log.py --manifest x9/out/x9-rev252bc/shim/ufs-root-manifest.json --serial <adb
  serial> --out .local/x9/work/x9-disk-log/x9-rev252bc-20261106-121242` (adb in /opt/homebrew/bin); it stops at
  21.264 s (logger lag). Android pstore (`adb exec-out su -c cat /sys/fs/pstore/console-ramoops-0`, saved on the
  Mac in .local/x9/work/x9-disk-log/pstore-20261106-121302/) has the end:
  `X9 WIFI EMI: before emi_addr=00004000 emi_size=0 emi2_addr=0 emi2_size=0`, `set emi_addr=0000e100
  emi_size=00c00000 emi2_addr=0000e260 emi2_size=0009a100` (writes stick), then
  `panic: kmem_alloc(..., 12582912, 0x100021): failed with 3` = my 12 MiB ml_io_map_wcomb (io_submap is 8 MiB
  and KMA_PERMANENT; same class as rev247-lo). The patch was never sent in that boot.
- rev252bd (commits 1b184be, cee0134): EMI checksum walk through 1 MiB windows with ml_io_map_unmappable(pa, 1 MiB,
  0x6 = VM_WIMG_WCOMB) (kernel_map, KMA_DATA) + ml_io_unmap. BUILT via ssh: Mac x9/out/x9-rev252bd/boot.img
  9,887,744 B sha256 b0e4e35e5a088bec6e94158af0638a2e515559d2a78de7a1bad13b9fae81f791, same args
  (x9_wifi_power=6 x9_gpu_khz=1716000 x9_gpu_floor_khz=1100000), kernel only, no mailbox.
- RULE: never ml_io_map/ml_io_map_wcomb anything big or temporary (permanent 8 MiB io_submap); use
  ml_io_map_unmappable + ml_io_unmap.

## HANDOFF 2026-11-06 (2): rev252bc = tell the MCU its EMI (mt6653SetupMcuEmiAddr) before the patch; built

- Source finding (gen4m, this session): host-side patch flow matches ours exactly (wlanDownloadPatch ->
  wlanGetPatchInfoAndDownloadV2 per region PATCH_START+data, then wlanPatchSendComplete {0,0,0,0}; BT/ZB patch
  options are only on for MT7961/MT7902, so ucType stays 0; mt6653 asicEnableFWDownload = NULL; no dyn mem map).
  BUT before any download gen4m halHifSwInfoInit calls mt6653SetupMcuEmiAddr: CONN_ON_EMI_ADDR 0x20060d1c
  (bar 0xe0d1c) = EMI pa >> 16, MT6653_EMI_SIZE 0x7c05b2e0 (bar 0x1ab2e0) = size, EMI2 0x7c05b2ec/0x7c05b2e4
  (bar 0x1ab2ec/0x1ab2e4) = misc pool pa >> 16 / sum of mt6653_wifi_misc_rsv_mem_info (0x9a100). mt6653
  rEmiInfo.type = EMI_ALLOC_TYPE_LK = wifi node conninfra-emi-addr/size (mt6993.dts: addr 0 filled by LK, size
  0xc00000) = LK mblock-52 consys_wifi_emi 0xe1000000/0xc00000 (connsys-emi-config layout region 0 = 0xc00000,
  MPU region 0x3e). EMI2 = mblock-54 wifi-misc 0xe2600000. We never wrote any of these.
- Hypothesis (guess): FINISH with both sections makes the ROM start the patch, which uses the EMI at base 0 ->
  hang; section-0-only fails verification first (0x11). Tag-ctrl (CFG_PCIE_TAG_ID_SUPPORT) is not in the 6653
  block -> skipped.
- rev252bc (X9WifiNet.inc): boot-arg x9_wifi_emi (default 1) writes the four CRs after rings up, logs
  `X9 WIFI EMI: before ... / set ...`, maps the 12 MiB EMI (Normal NC) and checksums it per 64 KiB, and after
  PATCH_FINISH logs `X9 WIFI EMI after PATCH_FINISH: block +xxxxxx changed ...` (= did the chip write host EMI).
  The post-data sem probe is now opt-in (x9_wifi_probe=1). Build: Mac tunnel (ssh -p 2222) was refused this
  session at first; then BUILT via ssh (source 1b2db7b): Mac x9/out/x9-rev252bc/boot.img 9,887,744 B sha256
  9561e812b05a81a8c7716f113801e37118c1ccc6671683656fbccf20a918ef8a. Built with the usual args: `python3 -I x9/build/build.py --name x9-rev252bc --boot-arg
  x9_wifi_power=6 --boot-arg x9_gpu_khz=1716000 --boot-arg x9_gpu_floor_khz=1100000` (kernel only, plugin 255
  already staged, no mailbox).
- If FINISH still hangs: next gen4m pre-download inits to compare: sw_wfdma init, SW EMI ring init
  (CFG_MTK_WIFI_SW_EMI_RING), wifi_coredump/EMI clearing, CCIF; also poll RX rings other than 7 after FINISH.

## HANDOFF 2026-11-06 (1): 71/72.txt = rev252bb / rev252bb-s0: section 1 is what blocks PATCH_FINISH; hakios review

- 实测 /tmp/71.txt (x9-rev252bb, all sections): sem get status 2, both PATCH_START ok, all data sent (2 + 8 ms),
  probe sem-get right after the last data chunk ANSWERED (eid 4 status 0) => ROM alive after the data. Then
  PATCH_FINISH (cid 7 seq 5) no event in 10000 ms again (rom_sram/romcode_index 1d1e; at timeout slpprot0 back to
  0041f004, R2A dmawr_probe 00100000).
- 实测 /tmp/72.txt (x9-rev252bb-s0, x9_wifi_nsec=1, section 0 only): probe answered, **PATCH_FINISH answered in 1 ms
  with status 0x11 (17)**, sem RELEASE answered status 3. => FINISH handler works; sending section 1 (e0010000,
  315264 B, AES) is what turns FINISH into a hang. Guess (unverified): with both sections the ROM verifies/
  decrypts section 1 and jumps into the patch, and the patch hangs or answers somewhere we do not listen
  (other RX ring / needs something host-side). Status 0x11 meaning not looked up yet (gen4m/ROM code table).
- Next candidates: look up 0x11 in gen4m; nsec=2 but skip/replace section 1 data with zeros (does FINISH then give
  a status != 0x11?); after FINISH poll all RX rings (not only RX7) and romcode_index / CONN MCU PC for ~10 s;
  compare mt7925 (e0002800 section, same PATCH_START flow) post-FINISH expectations.
- hakios review (github.com/oosexgang/hakios-xnu-kernel @2631f5d, + oosexgang/hakios-mt6768; clones only in my
  scratchpad, not in git). Observations:
  * GPU: main tree is Adreno-only (PM4/a6xx own Metal driver "hakios-metal" 100-118 fps; UniXNU metal2vulkan+Turnip
    50-57 fps). Not portable to our Mali CSF. Ideas that transfer: `fbscale=2` render at 750x1624 and scale to panel;
    `metal_defer` (record CBs on own thread ~ our async); `wallpaper_freeze`/`ui_lite` cheats; userptr ioctl 0x7d
    (wired task pages, CPU cache pushed by PA per submit - same cost model as ours, no trick there);
    tools/ca-metallib.py (QuartzCore variants by pipeline label). mt6768 = Mali-G52 (JM, panfrost GL + AIR->NIR
    Metal); their async path had "reuse while GPU reads" corruption, fully sync mode was clean (docs/metal-x6826b-plan.md).
  * Wi-Fi: POCO runs Linux ath11k unmodified on FreeBSD LinuxKPI + their wifi/lx glue (mk-ath11k.sh) - a path to run
    gen4m/mt76 code later. Darwin side reusable for us: drivers/wifi/hakios.apple80211.c (SIOCGA80211/SIOCSA80211
    ioctls for wifid, events class 6), hakios.wlan.cpp (IO80211Controller registry + IO80211InterfaceRole +
    device-tree wlan local-mac-address so MobileGestalt/lsd do not wait 5 s each), hakios.sta.c (scan/join thread),
    wifi/lx/src/lx_wpa.c (own WPA2 4-way, MIT). mt6768 (CONNAC1 AXI, gen4m) reached WPA2+DHCP+HTTPS+Safari:
    kernel/bsd/dev/redwood_mtkwifi.c + docs/wifi-x6826b.md (lessons: mgmt frames on cmd ring TX15 with TXD; EAPOL as
    security frame at lowest basic rate; BMC WTBL for broadcast; POWER_SAVE_MODE CAM after join; WIFID=1 needs
    configd, mDNSResponder, trustd, securityd, nsurlsessiond, CFNetworkAgent). Their RAM code regions with
    FW_FEATURE_DL_TO_EMI (bit 7) are memcpy'd into EMI, not DMA'd - applies to RAM code, NOT our ROM patch
    (our e0010000 matches mt7925's e0002800 style chip address).

## HANDOFF 2026-11-05 (6): 70.txt: sem works (status 2) but ROM silent after the data; rev252bb probes

- 实测 /tmp/70.txt (rev252ba): PATCH_SEM_CONTROL get answered eid 4 status 2 (so cid 0x10 is understood), both
  PATCH_START ok, data sent, PATCH_FINISH no event 10 s, and the sem RELEASE afterwards also no event 3 s =>
  ROM stops answering anything after the section data (alive after section 0 data: it answered section 1 START).
- rev252bb: (a) probe sem-get right after the last data chunk (ROM alive?), (b) boot-arg x9_wifi_nsec=N sends only the
  first N sections. Images: x9-rev252bb (all sections) and x9-rev252bb-s0 (x9_wifi_nsec=1: section 0 only then
  PATCH_FINISH - does FINISH answer when section 1's 315 KB at e0010000 is not sent?).
- BUILT via ssh (source 9cd92fa, args x9_wifi_power=6 x9_gpu_khz=1716000 x9_gpu_floor_khz=1100000, plugin 255):
  x9-rev252bb sha256 640d5f7c3d469f0c5e6ea8acc04ccdcf2478cbf7c374a0eaf29d6ac0c2a31496;
  x9-rev252bb-s0 sha256 61f6de46b84fed8344e930896769360b7b98934896eaec6bc566fa585be33f50.

## HANDOFF 2026-11-05 (5): 69.txt; rev252ba = PATCH_SEM_CONTROL (x9_wifi_sem=1 default)

- 实测 /tmp/69.txt (rev252az): sleep bits (slpprot0 b28, CB slp_status 2b) are ALREADY set right after section 0's
  data and the ROM still answers section 1's PATCH_START => benign idle state, not the cause. PATCH_FINISH still
  no event in 10 s; PCIe/AER/ATR clean; dmawr_probe varies (10480000, 10700000, 10080000).
- Checked mt76 (connac2_load_patch / get_data_mode): for info=01000000 mt76 computes mode = NEED_RSP|ENCRYPT|
  RESET_SEC_IV|KEY_IDX(0) = 80000009 = ours => candidate 2 (mode bits) is NOT the difference. mt76 also uses
  PATCH_START (0x05) only for addr 0x900000 / (7925,7927: 0xe0002800); other addrs use TARGET_ADDRESS_LEN_REQ
  (0x01) - ours e0010000 is not in mt76's list (guess: gen4m uses PATCH_START for all; ROM answered it).
  mt76 CB patch (cid 0x30/0x32) is mt7928 only - not applicable (no CB patch file for MT6653 here).
- rev252ba: PATCH_SEM_CONTROL get (cid 0x10 {1,0,0,0}, expect event eid 4 status 2) before the patch, release
  after; boot-arg x9_wifi_sem=0 disables. Look for `PATCH_SEM_CONTROL get answered status=N`.
- BUILT via ssh (source e935585): Mac x9/out/x9-rev252ba/boot.img sha256 92db29e8f0a202117cb6f895d7a11ba985d5e97717031ebeb5115866d6a16102,
  args x9_wifi_power=6 x9_gpu_khz=1716000 x9_gpu_floor_khz=1100000, kernel only, plugin 255.
- If sem changes nothing: try cid 0x01 (TARGET_ADDRESS_LEN_REQ) for section 1, chunk size.

## HANDOFF 2026-11-05 (4): 68.txt = rev252ay: PATCH_FINISH STILL no event in 10 s; rev252az diag (not built)

- 实测 /tmp/68.txt (x9-rev252ay): identical to 67: cid5 seq1/2 answered in 1 ms each, section data 2 ms / 6 ms
  (all chunks consumed on TX16), PATCH_FINISH (cid 7 seq 3) `no event in 10000 ms`, rom_sram/romcode_index
  still 1d1e, RX7 cidx=15 didx=2, slpprot0 1041f004 (b28), CB slp_status 2b (b5), R2A dmawr_probe=10080000.
  => it is NOT a too-short wait. ROM never answers FINISH.
- Unknown (guess): ROM may have stopped/slept after the data (b28/b5), or data of the AES section 1 was not
  accepted. rev252az (source only, NOT built; commit follows): dumps x9_wifi_rc_diag right after each section's
  data ("after data s0/s1") to see whether b28/b5/dmawr_probe flip during the data phase or only on FINISH.
- BUILT 2026-11-05 via ssh (Mac ~/ios_plj110 at 54c8184): x9/out/x9-rev252az/boot.img sha256
  dd2c3c9106442059990d8f1a729d3211b3a752850fe24162f39ac369b031247e, boot args x9_wifi_power=6 x9_gpu_khz=1716000
  x9_gpu_floor_khz=1100000 (my reconstruction of rev252ay's args), kernel only, plugin 255.
- Build on the Mac: `python3 -I x9/build/build.py --name x9-rev252az --boot-arg x9_wifi_power=6 ...` (same
  boot args as rev252ay; kernel only, plugin 255 already staged, no mailbox needed).
- Next candidates if the flip happens in the data phase: try PATCH_SEM_CONTROL (mt76 does), smaller/larger
  chunks, key-index/mode bits for the AES section.

## HANDOFF 2026-11-05 (3): Wi-Fi DMA WORKS with the LK pool; PATCH_FINISH timeout; rev252ay

- 实测 /tmp/67.txt (x9-rev252ax, includes rev252am x9_wifi_rsvmem=1): `X9 WIFI DMA: using LK
  wifi-reserve-memory pool pa=e2700000`; rings TX15 e2700000 TX16 e2700100 RX7 e2700200. **First command
  answered** (cid 0x05 seq 1 -> event eid 1 seq 1 status 0), section 0 (addr 00900000 len 70976 mode 80000009)
  and section 1 (addr e0010000 len 315264) PATCH_START answered status 0, all 386240 B of data consumed on
  TX16 (6 ms). => The 50-60.txt stall was the DMA target memory: the PCIe/Wi-Fi master may only access LK's
  MPU-opened wifi pool (rev252am hypothesis confirmed).
- Then PATCH_FINISH (cid 7 seq 3, 4 zero bytes) got no event in 1000 ms: RX7 didx stayed 2, rom_sram and
  romcode_index still 1d1e, R2A dmawr_probe=10080000 dmard_probe=00000400, WFDMA slpprot0 0041f004 ->
  1041f004 (b28), CB slp_status 0b -> 2b (b5). gen4m flow on connv3 is identical (CFG_ROM_PATCH_NO_SEM_CTRL=1,
  no dyn mem map, V2 sections with one data mode, CID-0 raw data on the FWDL port, PATCH_FINISH
  {ucCheckCrc=0,rsv[3]}) but waits CFG_DEFAULT_RX_RESPONSE_TIMEOUT = 3000-10000 ms.
- rev252ay (commit 0bf180d): init-command event wait 3 s, PATCH_FINISH 10 s, logs `event after N ms` and on
  timeout rom_sram/romcode_index. Image x9/out/x9-rev252ay/boot.img sha256
  360248d1471ac71cbff454441a984cd7ad6636e449f4a1f0d6848433bee8361b = x9_wifi_power=6 x9_gpu_khz=1716000
  x9_gpu_floor_khz=1100000 (no park/async: stable GPU), kernel only, allowlist 255 (staged).
- If PATCH_FINISH still gets nothing in 10 s: look at the b28/b5 sleep-protect bits (did the ROM put WFDMA
  AXI into sleep protect after the patch?) and the outstanding R2A write (event write to e2709000 stuck?).
- GPU side status (separate): 67.txt park+async+ready-frame+1716 MHz: copies 52-65 /2 s early (best so far),
  then 63.95 s `X9 PANVK FAILED: line=418 ... unmap native resource: IOKit=0xe00002c2` (IOConnectUnmapMemory64
  refused in x9_pan_kmod_unmap_cpu) -> device lost -> flicker/freeze. Not investigated yet.

## 66.txt = x9-rev252aw (park+async, plugin 255): stable, GPU side faster, display slower; rev252ax (2026-11-05)

- 实测 /tmp/66.txt (718,749 B): 0 FAILED/uncertain; `X9 RENDER ASYNC: jobs=1200 failed=0 queue_full_waits=0
  drain_waits=0`; park hits=411 cold=775 retires=774 (retire reasons: map mostly -> backboardd maps new BOs
  between most submits). Same boot window 30-40 s vs 60.txt (sync): jobs/2 s 74-84 vs 56-75, retire_us 19 vs
  213-312, start_us 134-269 vs 207-481, but display copies/2 s 28-39 vs 46-51 with gpu_defer 71-179 vs 3-14.
  Interactive 56-72 s: copies 20-42 /2 s (10-21 fps), per job pre_cache ~5.5 ms, wait 9-14 ms, post ~3 ms;
  import cache traffic 66 MB pre + 43 MB post per job; governor 858-1300 MHz at util 15-39%, 40-42 C.
- Cause of the display drop: the global present fence waited for the newest swap; with work always queued,
  each new swap moved the target, so complete frames were dropped unseen.
- rev252ax (commit 65bcb05): ready-frame fallback (superseded swap whose fence passed is kept and shown while
  the newest still renders; `ready_shown=` in X9 DISPLAY cadence). New boot-arg x9_gpu_floor_khz (governor
  floor; OPPs 390..1716 MHz). Image x9-rev252ax = x9_wifi_power=6 x9_gpu_park=1 x9_submit_async=1
  x9_gpu_khz=1716000 x9_gpu_floor_khz=1100000, sha256 2dd0b62515d15fa9d14fe96e50b47a7ff451f73e31a3323431cae3d0c4f237af,
  kernel only (allowlist 255 = staged).
- Remaining big costs (per job): GPU wait ~12 ms, CPU cache maintenance ~8.5 ms (imports 66+43 MB), per-frame
  full-screen BO churn (maps -> park retires, memory bursts). Next candidates: exact resource sets (design
  decision 3/4) to cut import maintenance; BO reuse in the frontend; map-without-retire (AS UPDATE only).

## 65.txt = x9-rev252av (park+async, plugin 254): kernel side works; PanVK sync-only bug; plugin 255 (2026-11-05)

- 实测 /tmp/65.txt (349,361 B): async worker started, PanVK mode=async, kernel async jobs completed in order
  (jobs=4 failed=0), parking works: `retired group=1 (map) in 332 us`, `(unmap) in 185 us`, hits=2 by the
  4th job; CSG close path (resume empty -> STOP/TERMINATE -> AS1 detached) fine. No GPU fault.
- Failure: `X9 PANVK SYNC FAILED: line=381 errno=17` right after the 4th job = the async branch of
  submit_sync_common for a SYNC-ONLY op: ret kept its -1 initialiser (errno 17 stale) -> device poisoned ->
  `X9 PANVK FAILED: line=97` (free_bo of a still-mapped BO during teardown) -> backboardd/SpringBoard lose the
  device repeatedly -> logo. My bug from rev252aq.
- Fix plugin 255 (commit 1efb451): ret=0 before the async branch. Images: x9-rev252aw = all --revision 255 +
  x9_wifi_power=6 x9_gpu_park=1 x9_submit_async=1; x9-rev252aw-safe = kernel only, x9_wifi_power=6 (allowlist
  255). NEW PLUGIN -> stage x9/out/x9-rev252aw with mailbox/update.py before booting either. Built (source
  ea767dd): x9-rev252aw boot.img sha256 28c33dfe697fdf367c6744660da1ebd7017d53b4ba4804b826cb9f4d0aca3679,
  x9-rev252aw-safe sha256 b5397556257dff7612d439429a4f54c1462c4767e09689639c27460a197e0d7d (both 9,883,648 B,
  both kernels contain X9GeneralMetal255).

## 64.txt = x9-rev252au-probe (plugin 254): ACE-Lite breaks GPU data even WITH full CIVAC -> ACE-Lite shelved (2026-11-05)

- 实测 /tmp/64.txt (6,186,997 B): plugin 254 staged (X9 UPDATE errno=0, active X9GeneralMetal254), boot-args
  x9_gpu_acelite=1 x9_gpu_coherent=0. Snoop probe coherent=483 stale=0. At 35.255 s (user interaction, same
  moment as 62.txt 34.7 s): AS1status=00c202c2 AS1addr=0x40 (translation fault, read, zero pointer + 0x40) ->
  submission uncertain -> `X9 PANVK FAILED: line=50 errno=5 detail=native operation: IOKit=0xe00002ca` (the
  submit's kIOReturnIOError) + `X9 PANVK SYNC FAILED: line=394` -> backboardd and SpringBoard device lost ->
  frozen lock screen.
- Conclusion: the 0x40 fault happens with complete CPU cache maintenance, so it is NOT stale CPU-written data
  from the CIVAC skip. Making GPU traffic shareable (SMMU SHCFG=incoming + AMBA ACE-Lite + outer-shareable
  leaves) itself corrupts what the GPU reads under load (GPU->GPU data via the ACP path, 推断). Missing platform
  setup candidates (unverified, done by closed pKVM/GPUEB/SLBC on Android): DVM_MI <-> MCUSYS SYSCO connect,
  L3 ACP partition (slbc mtk_l3c_set_acp_part), ACP QoS/ostd. 63.txt's silent poisoning was most likely the
  same fault with the log line lost.
- Decision: keep x9_gpu_acelite OFF. Code stays opt-in (SMMU SHCFG switch, AMBA, outer-shareable leaves, snoop
  probe) for later research.
- Next images (kernel only, allowlist = plugin 254 already staged; both kernels contain "X9GeneralMetal254"):
  x9-rev252av = x9_wifi_power=6 x9_gpu_park=1 x9_submit_async=1, boot.img sha256
  20cb8392c4da39704755d5f3510df7de35388a1e737529f6b51abb1e88f614de; x9-rev252av-safe = x9_wifi_power=6, sha256
  55f7b6791ce999beca8de3a7ea590508b64e61fc952ab0a1b599f1fc8160166e. Neither park nor async has been exercised
  past a first job on hardware yet (61.txt died on the ACE-Lite fault first).

## Wi-Fi: DMA stall FIXED by LK's Wi-Fi pool (EMI MPU); PATCH_START status 14 = our 68-byte header; rev252av (2026-11-05)

- 实测 61/62/63.txt (rev252am+ kernels, x9_wifi_rsvmem default 1): `X9 WIFI DMA: using LK wifi-reserve-memory
  pool pa=e2700000`, then `X9 WIFI CMD: cid 0x05 seq 1 -> event len=44 rxd0=3800002c ... type=0007 eid=1 seq=1
  data=0e 00 00 00`. **First ever completed host<->MT6653 command round trip**: TX15 consumed, RX7 event. So the
  58-60 stall was the EMI MPU (PCIe/CONN master only allowed into LK's MPU region 47 carve-out), as predicted.
- Then `PATCH: section 0 addr=00900000 len=70976 info=01000000 mode=80000009 PATCH_START answered status=14
  cid=00` -> stage stops, rings stopped, fw own, power off (clean).
- SOURCE: event layout ok (RXD 32 = MT6653_RX_INIT_DESC_LENGTH, INIT_WIFI_EVENT 8 bytes, CMD_RESULT
  {ucStatus, ucCID}); status codes 0..6 only (nic_init_cmd_event.h); connv3 builds use CFG_ROM_PATCH_NO_SEM_CTRL=1
  (no PATCH_SEM, our flow right), CFG_DOWNLOAD_DYN_MEMORY_MAP=0, patch via wlanImageSectionConfig PATCH_START.
  **Bug: gen4m wlanSendInitSetQueryCmdImpl sizes commands as cmd + sizeof(INIT_HIF_TX_HEADER) 32 +
  sizeof(INIT_HIF_TX_HEADER_PENDING_FOR_HW_32BYTES) 32 -> payload at 64** (INIT_WIFI_CMD = CID, 0xA0, rsv, seq,
  u4Reserved, au4D3toD7Rev[5]); mt76 connac2 MCU TXD is 64 too. Ours used 68 -> ROM read {addr,len,mode} 4
  bytes late (status 14, CID echoed 0). FW data chunks (cid 0, ring 16) are raw: unchanged.
- rev252av (commit 4c9d281): payload at 64, len 64+plen. Image x9-rev252av = all --revision 254 (plugin 254
  failure logging) + x9_wifi_power=6 x9_gpu_acelite=1 x9_gpu_coherent=0. NEW PLUGIN: stage before booting.
  Expect PATCH_START status 0, then patch data chunks on ring 16, PATCH_FINISH (CID 7) status 0, then the RAM
  firmware stage (not implemented yet beyond what stage 6 does).

## 63.txt = x9-rev252at-probe: GPU->CPU snoop mostly works; backboardd device poisoned silently at 41.4 s; plugin 254 (2026-11-05)

- 实测 /tmp/63.txt (4,238,619 B), boot-args x9_gpu_acelite=1 x9_gpu_coherent=0 (CIVAC kept). SMMU SHCFG 0->1
  ok, AMBA ACE-Lite enabled, civac_skip=0.
- `X9 RENDER SNOOP: GPU->CPU fence probe coherent=1003 stale=2` (first stale between 32.6 and 34.6 s): GPU
  write-backs of outer-shareable lines DO invalidate CPU lines almost always; 2 stale reads are unexplained
  (race of flush completion vs snoop, or real misses) -> not proven reliable.
- No AS1 fault and no submission uncertain this time. At 41.404 s backboardd's PanVK allocations failed with
  used_failed=1 (device already poisoned) -> Metal DeviceLost flood (the "lots of logs"). Nothing logged
  between the last good call (41.397) and 41.404: PanVK has silent poisoning paths (reply validation).
  backboardd native bytes were 603 MB at that moment (kernel budget 1024 MiB, no budget refusal logged).
- Memory bursts of full-screen BOs (13.9/12.8/16 MiB) during interaction also occur without ACE-Lite
  (57.txt peak 562 MB, then released) -> not an ACE-Lite effect by itself.
- Plugin 254 (commit 92e35fb): X9_FAIL macro logs the first poisoning per device with source line, errno and
  last kernel error (`X9 PANVK FAILED: line=`), sync layer `X9 PANVK SYNC FAILED: line=`. Image
  x9-rev252au-probe = all --revision 254, boot-args x9_wifi_power=6 x9_gpu_acelite=1 x9_gpu_coherent=0
  (same as 63) -> NEW PLUGIN: stage with mailbox/update.py before booting. Built (source a7177cc):
  x9/out/x9-rev252au-probe/boot.img 9,887,744 B sha256 b16284a2db46f2be4ed410b112c4b53666e6c44182906641fa247b5706abe54c,
  plugin-package X9GeneralMetal254/X9PanVK254. CAUTION: each kernel embeds the plugin allowlist it was built with,
  so rev252aq/ar/as/at kernels accept only plugin 253; after staging 254 use rev252au-probe (or rebuild others
  with `all --revision 254`).

## 62.txt = x9-rev252as-acelite: reached lock screen, then GPU read VA 0x40 -> stuck; snoop probe rev252at (2026-11-05)

- 实测 /tmp/62.txt (2,388,946 B): `GPU SMMU CR0=0 GBPA 00000000 -> 00001000 (SHCFG 0 -> 1) ok`; ACP line:
  mfg_acp_slpprot=000000d4 gals_rx=0f gals_tx=07 emi_acp chi0-3=1c/1c/1c/1c mfg_dvm_prot=05 tcu_efp=09
  dvm_si=0001e001 pctrl con0=00005190 (b4 ACP_SWITCH_LOCK_EN=1) con2=0000f0f0 (b11 SSYS3 mask=0) -> pKVM's
  pctrl_enable condition would be true. AMBA 1f -> 00, acelite=1 civac_skip=1.
- ~170 jobs ran (lock screen visible), then at 34.696 s backboardd job id=117 (3 streams): AS1status=018202c2
  AS1addr=0x40 (translation fault, read, VA 0x40 = zero pointer + 0x40), extract 3664 of 3680 -> submission
  uncertain -> frozen lock screen. None of 54-60.txt (no ACE-Lite) has any AS1 fault.
- Reading (推断): page tables were CPU-cleaned in this build, so the stale read is CLIENT DATA whose CIVAC was
  skipped -> GPU reads of CPU-dirty lines are not (reliably) snooped. Possibly on MT6993 only the GPU SMMU TCU
  uses the ACP path (devapc name MFG_S_TCU_ACP_SNOC), not GPU data. Unproven.
- rev252at (commit 2529073): GPU->CPU probe on the outer-shareable render fence page (plain cached load vs
  invalidated load after render_page_sync) -> `X9 RENDER SNOOP: GPU->CPU fence probe coherent= stale=` every
  2 s. Image x9-rev252at-probe = x9_wifi_power=6 x9_gpu_acelite=1 x9_gpu_coherent=0 (CIVAC kept -> rendering
  should stay correct; also shows whether shareable traffic alone hurts or slows anything). Kernel only.
  Built: x9/out/x9-rev252at-probe/boot.img 9,887,744 B sha256
  730bc8651215bf2bf4126bb6327253240f6d1bf6f9c49d7140670bb80894a1f2.
- Decision rule: stale>0 -> GPU writes do not snoop either -> ACE-Lite unusable on this boot path without the
  missing platform setup (keep x9_gpu_acelite off). coherent only -> GPU->CPU works; CPU->GPU (what 62 broke)
  still needs its own test.

## ACE-Lite root cause found: GPU SMMU bypass forces non-shareable (GBPA.SHCFG=0); rev252as (2026-11-05)

- 实测 (every log): `X9 CSF HW: SMMU_CR0=0 CR0ACK=0 GBPA=00000000` is the GPU SMMU 0x48600000 (CSF code maps
  it, SweetX9CSF.cpp sweet_x9_csf_boot). SMMUv3 GBPA.SHCFG b13:12 = 0b00 = force Non-shareable in bypass, so
  every GPU transaction left the GPU non-shareable: the outer-shareable PTW/leaf accesses of x9_gpu_acelite
  could never snoop -> 61.txt translation fault on stale page tables.
- SOURCE: Android runs this SMMU (dts gpu_smmu "mediatek,mt6993-gpu-smmu") under pKVM: hyp/arm_smmuv3.c
  builds stage-2 STEs (CFG 0b110, S2SH0 inner) -> effective shareability keeps the incoming one. Also found
  (not touched): EMI PCTRL 0x1102c000 (CON0 b4 ACP_SWITCH_LOCK_EN, CON2 b11 SSYS3_SWITCH_MASK, read by
  pkvm_smmu_host.c pctrl_enable for TCU DVM), DVM_MI 0x1102a000 (MCUSYS SYSCO connect for DVM), MFG ACP path
  regs (gpufreq_reg_mt6993.h: MFG_TOP 0x48500834 ACP slpprot idle bits, ACP GALS 0x4b420320/364, EMI ACP CHI
  0x1102b174..198, RSI 0x11037080..). gpufreq only reads them.
- rev252as (commit 955f6e0): x9_gpu_acelite first sets GPU SMMU GBPA.SHCFG=0b01 (UPDATE protocol, only with
  SMMUEN=0, verified; restored if mismatched), then AMBA_ENABLE=ACE-Lite. bypass() accepts exactly 0x1000
  after that. Logs `X9 RENDER coherency: GPU SMMU CR0= GBPA a -> b (SHCFG 0 -> 1)` and a read-only
  `X9 RENDER ACP:` line. Render page tables are CPU-cleaned again even with ACE-Lite (a broken path now shows
  as stale pixels, not a stopped boot); the CIVAC skip on CPU-exposed client data stays the ACE-Lite win.
- Images (kernel only, plugin 253 already staged; source 4c1d543): x9-rev252as-acelite = x9_wifi_power=6
  x9_gpu_acelite=1 (sync, no park: isolates ACE-Lite) sha256 72c9b1469020f806b10d0fd36417b11f51f704a3840c9abbcc6ebd258a4886cd;
  x9-rev252as = + x9_gpu_park=1 x9_submit_async=1 sha256 4a2586f4d5ccc27ea3ae6a08125a98581f28b343a084bf1809c5cb09b85edb63. Also built earlier:
  x9-rev252ar (park+async, no acelite) sha256 e6f4e589..., x9-rev252ar-safe (baseline) sha256 b5c116c1....
- Possible remaining failure (推断): once transactions really are shareable they go via the ACP path; if that
  path is not open on our boot (no pKVM pctrl/DVM setup), GPU accesses could stall -> completion timeout ->
  `submission uncertain`. The ACP line gives the state to compare.

## 61.txt = rev252aq: stuck at logo; ACE-Lite page-walk snooping does NOT work as configured; rev252ar (2026-11-05)

- 实测 /home/ubuntu/61.txt (323,740 B), user staged mailbox (X9 UPDATE errno=0, active plugin
  X9GeneralMetal253), boot-args incl. x9_panel_hz=120 x9_gpu_acelite=1 x9_gpu_park=1 x9_submit_async=1.
- Panel: `X9 PANEL as LK left it: TE periods us=8297/8297/8297/8299 (~120.5 Hz) frame push DSI busy 7816 us
  (fits one refresh)`; after the switch identical. **LK already runs the panel at 120 Hz**; the lag was never
  the panel. (One push 7.8 ms at 120 Hz: little headroom.)
- ACE-Lite enabled (AMBA_ENABLE 1f -> 00, L2 off). Async worker started; PanVK mode=async (caps 0x7f).
  **First render job (backboardd group 1 id 1, cold path) faulted:** AS1 UPDATE cfg=62000006, MMU fault
  AS1status=401002c0 AS1addr=10000000 = translation fault level 0, read, at the group's ring VA. Marks all 0,
  insert=32 extract=0 -> `submission uncertain` -> device refused -> SpringBoard crash loop -> logo forever.
- Cause (推断, strong): with x9_gpu_acelite the render page tables were not CPU-cleaned (PTW_SH outer);
  the GPU walked stale DRAM. So on this boot path GPU shareable reads are NOT snooping CPU caches, despite
  AMBA_FEATURES ACE-Lite and the dts. Missing piece unknown: MTK ACP/DSU path enable (kbase options
  CONFIG_MALI_MTK_ACP_DSU_REQ / ACP_SVP_WA exist; GPUEB/MFG side not researched). Until a snoop self-test
  exists, x9_gpu_acelite must stay OFF (it also makes the CIVAC skip unsafe).
- Park/async were not exercised beyond this first job.
- rev252ar (kernel only, plugin 253 stays staged): x9-rev252ar = x9_wifi_power=6 x9_gpu_park=1
  x9_submit_async=1 (no acelite, no panel switch); x9-rev252ar-safe = x9_wifi_power=6 only (sync, no park;
  plugin 253 runs sync without the cap).

## GPU step 2: asynchronous submission (x9_submit_async=1) + rev252ap fix; rev252aq (2026-11-05, built, not device-tested)

- rev252ap BUG (found while doing step 2, do NOT flash x9-rev252ap): a parked context kept c->exposed=true;
  x9_render_memory_cache(_spans) refuse exposed contexts (Busy), so the very next same-group submit failed in
  pre-maintenance -> PanVK marks the device failed. Also clientMemoryForType/Map/Release/Cache refuse exposed.
  Fix (06f152b): parked => exposed=false (GPU has no work, AS1 still bound, vm stays attached so page-table
  changes still retire first); resume sets exposed=true right before the hardware phase.
- Kernel async (06f152b), opt-in boot-arg x9_submit_async=1 -> caps bit X9RenderAsyncSubmit=64:
  selector 12 SubmitAsync (X9RenderSubmission in, accepted reply status 0/completed_id 0), selector 13
  FenceWait (X9RenderWaitRequest, timeout_ns 0 = poll, <= 10 s). One global FIFO (depth 16, backpressure)
  + one kernel worker (BASEPRI_PREEMPT) running the unchanged render_submit under c->lock + owner lock.
  Per-context completion ring (64) + async_done_id under render_async_lock (leaf lock). FenceWait never
  holds c->lock. Unmap/Release/DestroyGroup/v1 Submit drain the context first (drain drops/retakes c->lock);
  render_close drains before the owner lock. Display: swap stores fPendSeq = global enqueue seq; tick presents
  only when completed >= fPendSeq, without the owner lock (counts as gpu_defer while waiting). Logs:
  `X9 RENDER ASYNC: worker started`, `X9 RENDER ASYNC: jobs= failed= queue_full_waits= drain_waits=`,
  `X9 DISPLAY: present without the GPU owner lock (... async submission on: present fence ...)`.
- PanVK plugin 253 (same commit): async iff kernel advertises it and no X9_SUBMIT_SYNC env. Sync.c: submit
  enqueues, pending signal fences go to a per-provider completion thread (FenceWait in order, 10 s x3, then
  done); Fence.queued/ordered: a submit's wait list accepts dependencies already in the kernel FIFO; sync-only
  marker waits for the newest enqueued id; x9_sync_drain before transport close. Memory.c: submit_async,
  fence_wait (no device lock, ETIMEDOUT not fatal), wait_bo/group_check use FenceWait. Queue.c: CALL buffers
  retired until async_completed >= id, released on a later submit (pool not touched by completion thread).
  Transport: ops up to FenceWait, kIOReturnTimeout -> ETIMEDOUT. Both X9RenderABI.h copies identical.
- Known limits (推断): global FIFO means one slow client delays all; display fence is global (conservative);
  CPU readers of another process's GPU output outside the display are no longer implicitly synchronized.
- Built (source 9adf5b3; two compile fixes: literal priority 92, extern kernel_thread_start_priority):
  `build.py all --revision 253 --name x9-rev252aq` + boot-args x9_wifi_power=6 x9_panel_hz=120
  x9_gpu_acelite=1 x9_gpu_park=1 x9_submit_async=1 -> x9/out/x9-rev252aq/boot.img 9,883,648 B sha256
  4b9f4386dad2761408ea96da163857b4fe5f00de732e32a9172801fe9e7b27c0; plugin-package X9GeneralMetal253 /
  X9PanVK253 (X9PanVK sha256 c4df6194...). NEW PLUGIN PACKAGE: stage before booting:
  `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev252aq`.
- Fallbacks without restaging: rebuild boot.img (kernel only, no --revision) without x9_submit_async (plugin
  253 sees no X9RenderAsyncSubmit cap and runs sync as before), then without x9_gpu_park / x9_gpu_acelite.

## GPU step 1: resident (parked) render group, opt-in x9_gpu_park=1; rev252ap (2026-11-05, built, not device-tested)

- Per submit before (SweetX9RenderEngine.inc render_submit): flush_gpu + install_as(AS1, 2 idle_as polls) + CSG
  START/RESUME (notify + 2 wait_ack, 100 us sleep quanta) ... doorbell, poll fence ... CSG SUSPEND (firmware
  saves CSG state) + flush_gpu + remove_as(AS1). Stock kbase keeps groups resident with AS bound.
- rev252ap (commit 55d000f): with x9_gpu_park=1 a finished submit flushes GPU caches only and parks
  {context, group, root}: group stays RUNNING idle on its slot, AS1 stays installed, c->exposed stays true.
  Next submit on the same context+group (and AS1 TRANSTAB == parked root): skips publish/install_as/START,
  checks x9_csf_group_health, publishes the packet, rings the doorbell. Everything else retires first via the
  unchanged SUSPEND -> flush -> remove_as -> detached (render_retire_parked): submit on another group/context,
  map/unmap/create/destroy/close/quiesce on that context, resident_end. sweet_x9_render_display_try_enter
  treats a parked AS1 as idle; GPU desktop staging skipped while parked (sweet_x9_render_parked());
  sweet_x9_csf_user/desktop return busy while parked. ParkRestore guard re-tracks the parked group if a submit
  returns before the first hardware step. Kept every submit: flush_gpu before work and after completion.
- Logs: `X9 RENDER PARK: resident group between submits ON`, every 2 s `X9 RENDER PARK: hits= cold= retires=`,
  first 8 retires with reason and duration. Expect start_us/retire_us in X9 RENDER timing to drop on hits.
  Hit rate depends on how often backboardd/SpringBoard alternate contexts or map between submits (unknown).
- Risks (推断): firmware behaviour with a long-idle running CSG (idle notifications are not checked by
  group_health); a CS that did not return to idle would make the next doorbell behave unexpectedly -> symptom
  would be a completion-wait timeout -> `submission uncertain` (no reset). Boot without x9_gpu_park to compare.
- Built (source e5562ac): x9/out/x9-rev252ap/boot.img 9,879,552 B sha256
  619c711dcc4b01d66977814ce351a10fd2ee8bd29424fbf989c33e4ec56d740d, boot-args + x9_wifi_power=6 x9_panel_hz=120
  x9_gpu_acelite=1 x9_gpu_park=1, kernel only, no mailbox staging. Control: x9-rev252ao-acelite (no park).

## Display refresh: panel rate measured + 120 Hz switch; rev252ao / rev252ao-acelite (2026-11-05, built, not device-tested)

- Facts (SOURCE + logs): panel ac287 is a COMMAND-mode panel (oplus panel-ac287-p-7-a0020-dsi-cmd.dtsi): it
  refreshes from its GRAM at the LK-chosen rate; we push content with mutex+DSI_START after a fresh TE
  (x9_lk_gui_step). Timings in the dtsi: sa_fhd_120 (pll 553), sa_fhd_90 (pll 414), sa_fhd_60 (pll 414);
  stock switch = oplus,dsi-timing-switch-command: 39 FF 5A A5 00, 15 60 xx, 15 7D yy (120: 00/00, 90: 03/01,
  60: 04/01). LK's rate was never measured before this build.
- 实测 (58-60.txt): our vsync/submit cadence is the software 60 Hz tick (sweetfb_hz=120 existed, opt-in);
  real pushes peak at 21-26/s (copies=42-52 per 2 s, copy 1.6-1.9 ms, TE wait avg 3.9-4.6 ms, 0 TE
  timeouts). So the ~10-25 fps feel is the GPU submit chain (~13 ms/job, ~2.3 jobs/frame), not LK; 120 Hz
  panel + cadence removes the display side as a cap but will not by itself raise content fps.
- rev252ao (commit 6f12536): AppleCLCD start logs `X9 PANEL as LK left it: TE periods us=... median=...
  (~N Hz) frame push DSI busy X us`; x9_panel_hz=120|90|60 sends the stock switch (LP, between frames,
  CMDQ0-2/CON/RX restored; new x9_lk_send_raw/x9_lk_panel_rate/x9_lk_panel_measure in x9_lk_console.h),
  re-measures (`after switch`), reverts to the rate nearest the original if one push >= one refresh
  (`reverted`). Without sweetfb_hz the cadence becomes 120 when the panel measures < 9.1 ms. PLL/PHY/DSC
  untouched (stock uses pll 553 for 120; if the push at 414 does not fit, the revert path triggers).
- Built: x9/out/x9-rev252ao/boot.img sha256 1ad40f9d635ce4e8765edef9e03b0eb105265f1ad1410b875c974d53afec3bf0
  (x9_wifi_power=6 x9_panel_hz=120) and x9/out/x9-rev252ao-acelite/boot.img sha256
  519d796d4291b4e7795f9915c294dbbc0562bda72953ada4e57cac506af4b294 (+ x9_gpu_acelite=1), 9,879,552 B each,
  kernel only, no mailbox staging.
- Device check: the three X9 PANEL lines; `X9 DISPLAY cadence: target=120`; TE wait avg should drop toward
  ~4 ms at 120 Hz with no TE timeouts; any tearing/garbage = PLL too low -> boot without x9_panel_hz.

## GPU cache maintenance: ACE-Lite IO coherency per stock kbase; rev252an / rev252an-acelite (2026-11-05, built, not device-tested)

- Cost being attacked (15.txt/18.txt, rev246-248): per job ~66-82 MB CIVAC-cleaned + 31-47 MB invalidated, all
  backboardd imports (cacheable, so rev247's WC skip matched 0); ~6.7 ms of ~13 ms/job.
- SOURCE (oppo a16w kbase r54p1 + mt6993.dts): mali node `system-coherency = <0>` = COHERENCY_ACE_LITE, no
  `dma-coherent`. For arch >= 12.0.1 kbase_pm_init_hw -> kbase_cache_set_coherency_mode writes AMBA_ENABLE
  (0x304) b4:0 = 0 (ACE-Lite) after reset, before L2 up; kbase_mmu_hw_configure sets TRANSCFG.PTW_SH = outer
  (b29:28 = 2) when coherency != NONE; kbase_mmu_sync_pgd_cpu skips the page-table clean unless NONE; userio
  pages are CPU-cached + GPU SHARE_BOTH (PTE 2<<8) + MEMATTR idx6 (0x08 SHARED). Upstream panthor
  (panthor_gpu_coherency_set, 2024-10 "Explicitly set the coherency mode") also writes the protocol before L2
  power-up; ACE_LITE=0, ACE=1, NONE=31.
- CORRECTION to the 2026-11-03 headroom note: CONFIG_MALI_MTK_COHERENT_DMA_BUF only acts when
  kbase_device_is_cpu_coherent() (system_coherency == ACE, full) and for a special coherent heap with memory
  group 1; on MT6993 (ACE-Lite) it is inactive. The vendor does NOT make ordinary user/import memory coherent;
  it relies on ACE-Lite snooping only for page walks, userio and MCU-shared regions, and keeps dma_sync for user
  BOs (most Android Mali BOs are CPU-uncached anyway).
- 实测 (logs): AMBA_FEATURES=00000001 (ACE-Lite only, no shareable-cache bit), AMBA_ENABLE=0000001f (= none,
  as LK leaves it). Our old gate read 0x300/0x308 as COHERENCY_NUM_GROUPS/GROUP_0 (pre-arch-12 layout), so it
  logged groups=1 type=0 and could never enable.
- rev252an (commit 467a4a0, on top of rev252am Wi-Fi pool): opt-in `x9_gpu_acelite=1`: AMBA_ENABLE -> ACE-Lite
  (only if AMBA_FEATURES b0 and L2 off; readback-checked, restored on mismatch), as_transcfg() adds PTW_SH outer
  (0x62000006), render-VM leaves outer shareable (2<<8, MEMATTR idx0 0x88 kept so GPU L2 still caches; GPU L2
  is flushed/invalidated around every job), no CPU clean of render page tables, existing coherent path skips the
  per-submit CIVAC of CPU-exposed client bytes (x9_gpu_coherent=0 keeps CIVAC while ACE-Lite stays on).
  install_as refuses if AMBA_ENABLE loses ACE-Lite. CSF-private memory unchanged (non-shareable + explicit
  cleans). 推断/risk: leaf SH=outer for user/import memory goes beyond what the vendor does (they keep dma_sync);
  it is the panthor model for dma-coherent ACE-Lite parts. If the interconnect does not snoop GPU shareable
  traffic, expect stale/mosaic surfaces -> boot without the token.
- Built (Mac): x9/out/x9-rev252an/boot.img sha256 96171f861e0b9d21f88597e8db45e0fc936a45d4507c7081c21320e0453e4cc6
  (behaviour = rev252am) and x9/out/x9-rev252an-acelite/boot.img sha256
  8b85ed8d6aafc020ca2a4f65332c91cc4bd65388e34674e7c5a6d80e20a6447c (+x9_gpu_acelite=1), both 9,875,456 B,
  x9_wifi_power=6, kernel only, no mailbox staging.
- Device check for -acelite: `X9 RENDER coherency: ... AMBA_ENABLE 0000001f -> 00000000 ... acelite=1 (enabled)
  civac_skip=1`; `X9 RENDER cache-coherent: ... pre_skip_avg/post_skip_avg` large; `table-cache cleaned_avg=0`;
  per-job pre_cache/post_cache near 0 in X9 RENDER timing; no mosaic/stale content when swiping (compare with
  the plain rev252an image).

## HANDOFF 2026-11-05 (2): SMMU ruled out; Android DMAs only from LK's Wi-Fi pool -> rev252am built, not device-tested

- SMMU (item 1 below) effectively ruled out: mt6993.dts pcie0/pcie1 have no `iommus`; the only SMMU on that
  side is soc_smmu @0x14900000 (UFS SID 0x63/0x77), and every log shows `X9 CSF HW: SMMU_CR0=0 CR0ACK=0 GBPA=0`
  = SMMU disabled, global bypass, no abort. Not read at the stall (nothing suggests it matters).
- **New lead (SOURCE, gen4m + dts + LK DT):** gen4m on mt6993 never DMAs from ordinary kernel memory.
  hif_mem.c halAllocHifMem -> halInitHifMem(WIFI_RSV_MEM_WFDMA) dma_alloc_coherent from the wifi node's
  memory-region; ALL TX/RX descriptor rings, TxCmdBuf, TxFwdlBuf come from it. LK carves it out as
  mblock-55 shared-dma-pool_wifi-reserve-memory_dma = 0xe2700000 size 0x1900000 (same in 50..60.txt), misc pool
  mblock-54 0xe2600000/1 MiB. dts connsys-emi-config: layout idx 11 "WFDMA1" 0x1900000, emi-mpu-region
  <6 11 47>, i.e. LK sets EMI MPU region 47 for it ("MPU-in-lk" path in hif_mem.c). plat/mt6993/plat_priv.c
  kalSetDrvEmiMpuProtection opens such regions only for DOMAIN_AP + DOMAIN_CONN -> the PCIe/Wi-Fi master is
  probably a non-AP EMI domain and may be refused on normal DRAM (0x1_45aa0000). 推断: refused access = our
  silent stall (MTK EMI MPU can bypass/clear violations without a report). mt6653 u4DmaMask 34 (non-WMT).
- Upstream match: LKML "dt-bindings: PCI: mediatek-gen3: Allow memory-region for restricted DMA buffer"
  (wenst@chromium.org, 2026-05, patchew 20260508063633.3894348-1-wenst@chromium.org): "On some SoCs without an
  IOMMU behind the PCIe controller, the PCIe controller memory access could be limited to a small region by
  the firmware configuring a memory protection unit." (rtw88 there; conclusion memory-region, not dma-ranges.)
- rev252am (commit abac34b): boot-arg `x9_wifi_rsvmem` (default 1) maps 0xe2700000 + 64 KiB with
  ml_io_map_wcomb (Normal NC; XNU RAM starts at 0x140000000 so the pool is not XNU memory) and uses it for the
  whole WFDMA area (d.pa = 0xe2700000, no cache maintenance); log line `X9 WIFI DMA: using LK wifi-reserve-memory
  pool ...`. `x9_wifi_rsvmem=0` = old IOBufferMemoryDescriptor path. Built (source d5ca54a):
  ~/ios_plj110/x9/out/x9-rev252am/boot.img 9,875,456 B sha256
  5118b8360af345f2673707cabec92915fd3348e749daee567933edcff7022e01, 0 errors, x9_wifi_power=6, no mailbox.
- Expect on device: descriptor DMADONE=1 / TX15 didx moves / RX7 event if this was the block. If it still
  stalls: next are EMI MPU violation registers (find EMI MPU base in dts emi-mpu node) and the rev252ac ack bits.

## HANDOFF 2026-11-05 (user out of quota): 60.txt = the EP read reaches the RC and dies on the SoC side

- 实测 60.txt (400,738 B, /tmp/60.txt) = rev252al. RC inbound ATR (as LK left it, unchanged): win0 t0 and win1 t0
  src 0 size 2^64 IMPL=1 trsl 0 param 00000004 (TRSL_ID 4 = AXI4 master 0, Linux pcie-plda.h), t1-t7 off.
  **At the stall RC p_adt_win0/1 = 00000008/00000008 (were 0 before DMA) = DOORBELL/hit: the MT6653's TX15
  descriptor read crossed the link and passed the RC's inbound ATR to the RC AXI master.** RC istat_local 0,
  pend_adt 0, err_addr/err_info 0, tx_credit 00010309/00000049 (same before/after), RC AXI0 err 0 (earlier logs).
  EP: istat_local 88000000 as ROM left it then 0; DEVCAP2 0011081f, DEVCTL2 0 (completion timeout enabled,
  default range) yet EP AER stayed clean in 56.txt (unexplained). Descriptor still DMADONE=0.
- Path status for the first WFDMA read (all verified on device): WFDMA issues AXI read (R2A dmard_probe) ->
  chip PCIe MAC outbound ATR hit (a_adt_slv0 doorbell) -> link -> RC inbound ATR hit (p_adt_win doorbell) ->
  **RC AXI master -> SoC interconnect / EMI: the read never completes, no error anywhere.**
- Next (not started), in order of my suspicion (推断):
  1. An SMMU / IOMMU in front of the PCIe0 AXI master left by LK in stall (not abort) mode, or translating.
     x9 UFS code already checks an SMMU "DMA bypass" for UFS (bsd/dev/sweet_x9_ufs.c: cr0 +0x20, ack +0x24, gbpa
     +0x44). Find the SMMU instance that covers pextp/PCIe0 on mt6993 (dts smmu nodes, mtk-smmu / iommu
     drivers in the oppo kernel tree, LK) and read its CR0/GBPA/fault registers at the stall.
  2. EMI MPU / bus protection rejecting PCIe-master access to the kernel region 0x1_45aa0000 (MTK EMI MPU
     domains; Android's gen4m allocates normal kernel DMA memory, so a default policy should allow it).
  3. The DDREN/APSRC "wait ack" bits rev252ac set before link-up (RESOURCE_CTRL 0xd2c bit10, PEXTP_REQ_CTRL
     bit12): if SPM never acks for this master the AXI access would wait forever. The stall predates rev252ac
     (50.txt), but test once with those two bits left at LK values.
- Current build tree: x9_wifi_power=6 path = stage 5 + RC pre_init/votes + pref window disabled + mcu_init writes
  + mt76 WFSYS reset + DMASHDL bypass + diag dumps (RC/CHIP/WFDMA/R2A/EPMAC/ATR/PEND/RCIN). Last image
  x9-rev252al (commit 16e2a90). Each experiment is undone at power-off; the phone has rebooted cleanly every time.
- Practical: logs arrive at /tmp/NN.txt on the server (54.txt was /home/ubuntu/54.txt). Server pushes to GitHub
  over SSH (origin git@github.com:tosasitill/ios_plj110.git). Mac build: ssh -p 2222 tosasitill@localhost,
  `export PATH=/Library/Frameworks/Python.framework/Versions/Current/bin:/opt/homebrew/opt/llvm/bin:$PATH`, then
  `python3 -I x9/build/build.py --name x9-revNNN --boot-arg x9_wifi_power=6` (tunnel drops often; poll it).
  mt76 reference clone lives only in the session scratchpad (re-clone openwrt/mt76 if needed: mt7925/pci.c,
  mt792x_dma.c, MT7927 = PCI id 0x6639, same BAR layout as MT6653).

## 59.txt + PLDA decode; rev252al = does the read reach the RC? (2026-11-05)

- 实测 59.txt (389,652 B, /tmp/59.txt) = rev252ak. pend_adt 00010000 (R=1) at the first BAR access, after driver
  own, after WFSYS reset, right after and 1 ms after bus master on, before DMA and at the stall: static, does
  not count our read -> not a stuck read. a_adt_slv0 0 -> 00000008 only at the stall; p_adt_win0 00000008
  from the first access. Same stall otherwise.
- Both bridges are PLDA XpressRICH (Linux drivers/pci/controller/plda/pcie-plda.h, same ATR layout 0x600 inbound
  / 0x800 outbound, 32-byte entries; MTK gen3 RC uses the same offsets 0x184 ISTATUS_LOCAL, 0x1d4 PENDING_ADT,
  0x800 trans tables). PLDA ATR event nibble: b0 POST_ERR, b1 FETCH_ERR, b2 DISCARD_ERR, b3 DOORBELL -> the 8s are
  DOORBELL/hit, not errors: our read passed the chip's outbound ATR (t0 identity 2^64, TRSL_PARAM 0 =
  PCIE_TX_RX_INTERFACE as Linux plda_pcie_setup_window uses). Linux MTK gen3 never programs the RC's inbound
  ATR_PCIE_WIN tables (HW default).
- rev252al (commit 16e2a90, read-only): `X9 WIFI RCIN <when>:` RC istat_local 0x184, pend_adt, p_adt_win0/1
  0x1d8/0x1dc, a_adt_slv0, err_addr 0xe44:0xe40, err_info 0xe48, tx_credit 0xd30/0xd34, EP istat_local (chip MAC
  0x184), EP DEVCAP2/DEVCTL2 (cap 0x80+0x24/0x28); plus one line per enabled RC inbound ATR entry (win0/1 t0-t7)
  or "no enabled RC inbound ATR entry". Built: ~/ios_plj110/x9/out/x9-rev252al/boot.img 9,875,456 B sha256
  b6eecf1e39a84f592f9c50dedb52a12dfc9d0501b5be151d99e6d200fbaf13e9, 0 errors, x9_wifi_power=6, no mailbox.

## rev252ak: trace of the pending outbound read (built 2026-11-05, not device-tested)

- Commit 060ae16, read-only additions: `X9 WIFI PEND <when>: pend_adt (R= W=) a_adt_slv0 p_adt_win0 dma_pend
  r2a_rd_probe r2a_sts ep cmd/sta devsta rc int_sta` at "first BAR access (EP cmd MEM only)", "after driver own",
  "after WFSYS reset", "right after bus master on", "1 ms after bus master on" (plus the existing EPMAC lines
  before DMA / at the stall). Earliest possible point is the first BAR access (the chip MAC is only reachable
  through BAR0).
- Built: ~/ios_plj110/x9/out/x9-rev252ak/boot.img 9,875,456 B sha256
  ee47ccff3b5ac96d6995622210fa0d0b33661f9ec14598568e82528ea7c60e72, 0 errors, x9_wifi_power=6, no mailbox.

## 58.txt: outbound ATR is identity 2^64 on all four AXI slaves; slave 0 flags our read; a read was already pending (2026-11-05)

- 实测 58.txt (383,903 B, /tmp/58.txt) = rev252aj. Chip PCIe MAC (bar 0x1d0000) as ROM left it / before DMA:
  slv_conf 55554343/55054353/55054363/55054373, mst_conf 00333346/00333356, **pend_adt 00010000** (PENDING_ADT_R
  field b27:16 = 1, W = 0), p_adt_win 00000008/0, a_adt_slv 0/0/0/0, istat_dma_pend 0, if_ctrl 02040000,
  basic_sta 20000401. ATR_AXI_SLV0-3 t0 on every slave: src 0, size 2^64, IMPL=1, trsl 0, param 0 (id 0) ->
  identity pass-through of the whole address space; t1-t7 IMPL=0 (mask ffffffff/fffff000). ATR unchanged at the
  stall. **At the stall a_adt_slv0 = 00000008** (was 0); pend_adt still 00010000; R2A dmard_probe 24000000 again.
- 推断 (not confirmed, bit meanings undocumented): (1) the outbound ATR covers 0x1_45aa0000, so translation
  is not the block; (2) slave 0 recorded an event (bit 3) for our read; p_adt_win0 also shows bit 3 for the
  inbound BAR path that works, so bit 3 may be a "transaction seen" flag rather than an error; (3) a read was
  already pending in the MAC before we started any DMA (pend_adt R=1 "as ROM left it"), which could block later
  reads if the outbound read path is in order. When it appears (before/after link-up, before/after EP bus
  master) is not known yet.

## rev252aj: read-only dump of the MT6653 PCIe MAC ATR/ADT state (built 2026-11-05, not device-tested)

- Commit eec2e12, `x9_wifi_power=6` (WFSYS reset of rev252ai still in). Every diag point ("as ROM left it",
  "before DMA", "stuck") also logs `X9 WIFI EPMAC <when>: slv_conf x4 mst_conf x2 pend_adt p_adt_win x2 a_adt_slv x4
  istat_dma_pend if_ctrl basic_sta` (bar 0x1d0000 + 0x34/3c/44/4c, 0x30/38, 0x1d4, 0x1d8/1dc, 0x1e0-1ec, 0x1d0,
  0x1a8, 0x18) and one `X9 WIFI ATR <when>: slvS tT src= size=2^n impl= trsl= param= (trsf= id=) mask=` line per
  non-empty ATR_AXI_SLV entry (0x800 + 0x100*S + 0x20*T, S 0-3, T 0-7), or "all ... entries are zero".
- Built: ~/ios_plj110/x9/out/x9-rev252aj/boot.img 9,875,456 B sha256
  82c22e7236615bbfd8d8ec29e753586aa55f4883f9e43b9b80fc711cd4732c40, 0 errors, no mailbox staging.

## 57.txt: the WFDMA read IS issued and hangs inside the MT6653; chip PCIe MAC has outbound ATR tables (2026-11-05)

- 实测 57.txt (444,746 B, /tmp/57.txt) = rev252ai. WFSYS: before emi_ctl0 18100004, rgu_wf_rst 00010340,
  mcu_own 00000007, romcode 1d1e, lpctl 0; after the pulse "DONE, ROM idle after 0 ms", rgu 00010340, emi_ctl0
  18100006, lpctl 00000004 (driver own lost) -> "driver own again taken". Probably not a full WF reset (ROM never
  left 1d1e). Same stall. R2A (wrapper 0x500..) ctrl0 ffff0c54 ctrl1 5ffe7ff8 ctrl2 8effff1f, sts 0 throughout,
  **dmard_probe 00000000 -> 24000000 at the stall** (outstanding AXI read), dmawr_probe 0, wr_dbg0 02aa0000,
  txfifo_dbg0/1 0f0f0f0f/0f0f0fff.
- 推断: WFDMA issued the AXI read; no RRESP error; EP AER clean (56.txt) -> the read is lost between WFDMA's AXI
  master and the PCIe link, i.e. in the MT6653's own PCIe MAC (PCIE_MAC_IREG, chip 0x74030000 = bar 0x1d0000).
- pcie_mac_ireg.h (gen4m coda mt6653) shows a PLDA-XpressRICH-like bridge: AXI_MST0-3_CONF 0x30.., AXI_SLV0-3_CONF
  0x34.., ROUTING_RULES 0x200.., ISTATUS_PENDING_ADT 0x1d4 (R b27:16, W b11:0), ISTATUS_P_ADT_WIN0/1 0x1d8/0x1dc,
  **ISTATUS_A_ADT_SLV0-3 0x1e0-0x1ec**, inbound ATR_PCIE_WIN0/1 0x600.. and **outbound ATR_AXI_SLV0-3 at 0x800,
  0x900, 0xa00, 0xb00 (8? entries x 0x20)**: +0x00 SRC_ADDR_LSB[31:12] | ATR_SIZE[6:1] (2^(n+1)) | IMPL[0],
  +0x04 SRC_ADDR_MSB, +0x08 TRSL_ADDR_LSB[31:12], +0x0c TRSL_ADDR_MSB, +0x10 TRSL_PARAM (TRSF_PARAM[27:16],
  TRSL_ID[3:0]), +0x18/+0x1c TRSL_MASK. gen4m never writes these (ROM/HW default). Not yet dumped.

## rev252ai: mt76 MT7927 WFSYS reset before DMA + R2A bridge dump (built 2026-11-05, not device-tested)

- Commit 9156ca9, `x9_wifi_power=6`. Inside the driver-own window, before bus master + stage 6:
  `X9 WIFI WFSYS: before reset emi_ctl0 rgu_wf_rst mcu_own romcode lpctl`, then CONN_CFG_EMI_CTL_0 (bar 0x101100)
  |= bit1, CB_INFRA_RGU (bar 0x1f8600) bit4 set 1.5 ms / clear / 8 ms, CB crypto MCU_OWN_SET (bar 0x1f5034) = 1,
  poll ROMCODE_INDEX == 0x1d1e <= 200 ms -> `X9 WIFI WFSYS: reset DONE, ROM idle|TIMEOUT ...`; re-takes driver own
  if lpctl bit2 came back (`driver own again`); DMA skipped if ROM not idle or no driver own; EMI_CTL_0 restored
  at the end. Every diag dump now also logs `X9 WIFI R2A <when>: ctrl0-2 sts dmawr/dmard probe wr_dbg0/1
  txfifo_dbg0/1` (R2A STS: b11 RRESP_ERR, b10:3 RRESP_ERR_STS, b0 RD_AXI_ERR_DET_INT).
- Built: ~/ios_plj110/x9/out/x9-rev252ai/boot.img 9,871,360 B sha256
  701ca03719327564c329a190f46aec81b309b4bd979f94c428216ce314362f38, 0 errors, no mailbox staging.

## 56.txt + mt76 comparison (2026-11-05)

- 实测 56.txt (382,528 B, /tmp/56.txt) = rev252ah. RC pref window as LK left it 00010001/0/0 (64-bit, base 0,
  limit 0 -> first 1 MB only; never covered 0x1_45...); set to 0001fff1/0/0 (low nibbles RO). EP AER cap @0x200:
  unc_sta 0, unc_mask 00400000 (only internal error masked), cor_sta 0, header log 0 as ROM left it, before DMA
  and at the stall. RC MAC 0x204/0x210/0x230 = f5/ffff00f1/f5 constant from LK on. Same stall, DMADONE=0.
  -> EP saw no completion timeout/UR/CA: the read either never left the MT6653 or the stall is after a good read.
- mt76 (openwrt/mt76 0936617, shallow clone in the session scratchpad, read only) mt7925/pci.c + mt792x_dma.c:
  - MT7927 (PCI ids 0x6639/0x7927/0x0738) register map = our MT6653 BAR layout: WFDMA0 0xd4000, EXT_CSR 0xd7000,
    ROMCODE_INDEX 0xc1604 (idle 0x1d1e), CB crypto-top MCU_OWN_SET 0x1f5034, CB RGU WF reset 0x1f8600 bit4.
  - Descriptor high bits identical to ours (TX w3 bits 3:0, RX w2 bits 3:0). mt76 uses a 32-bit DMA mask for
    MT7927 (34-bit only for MT7928); our DRAM is all above 4 GiB, so ADDR_EXT is mandatory for us.
  - MT7927 probe before DMA: fw_own -> drv_own, CONN_CFG_EMI_CTL_0 (0x7c011100, bar 0x101100) bit1 SLPPROT_EN
    = 1, mt7927_wfsys_reset (RGU 0x1f8600 |= bit4, 1-2 ms, clear, 5-10 ms, MCU_OWN_SET 0x1f5034 = 1, poll
    ROMCODE_INDEX == 0x1d1e <= 200 ms) = gen4m mt6653_mcu_reset (skipped in WMT builds), irq mask, rings,
    "prefetch" (0xd7030 rewrite, 0xd70f0-fc = 660077/1100/30004f/542200: on MT6653 these are HOST_CONFIG and
    MSI_INT_CFG0-3, i.e. MSI routing), GLO_CFG |= ADDR_EXT | FW_DWLD_BYPASS_DMASHDL, clear LBK_RX_Q_SEL_EN,
    EXT1 bit28, INT_RX_PRI |= 0xf00, INT_TX_PRI |= 0x7f00. MCU events on RX ring 6 for MT7927 (gen4m mt6653: 7).
  - Not seen before: WFDMA wrapper AXI0 "R2A" bridge 0x20027500-0x2002751c (bar 0xd7500..): CTRL_0..2,
    STS (RRESP_ERR b11, RRESP_ERR_STS b10:3, RD_AXI_ERR_DET_INT b0, BRESP b20/b19:12), DMAWR/DMARD probes,
    WR_DBG_AXIM_OUT0/1. gen4m never writes it; its STS would show an AXI read error inside the chip.

## 55.txt: DMASHDL ruled out; rev252ah = RC prefetchable window disabled + AER (2026-11-05)

- 实测 55.txt (493,031 B, /tmp/55.txt) = rev252ag. Bypass took effect: `ext0 28c004df -> 28c0049f, sw_ctrl
  00000000 -> 10000000`; DMASHDL stuck dump shows sw_ctrl 10000000. Same stall: TX15 cidx=1 didx=0, desc
  45aa1000 40500000 0 1 DMADONE=0, packet correct, 0 events. **DMASHDL is not the block.**
- Ruled out so far: RC pre_init/votes/sleep protect/res acks (51), CB slp_prot + access_detect (52), WFDMA AXI
  slpprot (53), CONN_HIF_RST + descriptor/command format (54), DMASHDL (55).
- rev252ah (commit d867c1c): RC type-1 prefetchable window 0x24/0x28/0x2c (never programmed before) read and set
  to Linux's "disabled" 0x0000fff0/0/0 at link-up (`X9 WIFI EP: RC windows: io/sec X, pref A/B/C -> ...`),
  restored in undo; `X9 WIFI AER before DMA|stuck:` = RC MAC 0x204 unc_sta, 0x210 cor_sta, 0x230 rp_sts, pref
  window, EP AER capability (ext-cap walk from 0x100) unc_sta/unc_mask/cor_sta/header log. Built:
  ~/ios_plj110/x9/out/x9-rev252ah/boot.img 9,871,360 B sha256
  dbd1553c96ce3229df34a817ef20071c948fdf82ca998e1b0082abf10245ec50, 0 errors, x9_wifi_power=6, no mailbox.
  Not device-tested.

## 54.txt: TX15 packet correct but never completed; rev252ag = DMASHDL bypass test (2026-11-05)

- 实测 54.txt (378,260 B, /home/ubuntu/54.txt, not /tmp) = rev252af. hif_rst 00000030 (logic + DMASHDL out of
  reset). DMASHDL before/stuck identical: sw_ctrl 0 (no bypass), opt 700c8fff, page 003f1000, refill fffe0000,
  ctrl_sig 00151000, pkt_max 00180010, grp0/grp1 01ff0010 (max 0x1ff, min 0x10), qmap0-3 0, dbg0/1 0.
  TX15 desc[0] after stall = 45aa9000 40500000 00000000 00000001 (addr 0x1_45aa9000, len 80, LastSec0, DMADONE=0);
  buf 01000050 00004000 ... | 00000030 0100a005 ... 00900000 (TXD, INIT header len 48 / CID 5 / A0 / seq 1,
  payload addr 0x900000): exactly as intended. GLO_CFG bit 6 = TX_WB_DDONE (confirmed in header) is on, so
  WFDMA never finished the packet. EP devsta 0 (no completion timeout / UR / CA seen).
- gen4m DMASHDL config (hal_dmashdl_mt6653.h): groups 0-5 max 0x490 min 0x10 refill on, group 15 max 0x30, queue
  map AC*3 -> group 4, ALTX -> 5, PLE max page 1 / PSE 0x18, HIF_ACK_CNT_TH 5, GUP_ACT_MAP 0x800F. Group 15
  quota register (0x2002605c) never dumped.
- rev252ag (commit d551ece): before the rings, GLO_CFG_EXT0 (0x200242b0) bit 6 TX_DMASHDL_EN off and DMASHDL
  SW_CONTROL (0x20026004) bit 28 bypass on, log `X9 WIFI DMASHDL: bypass test: ext0 A -> B, sw_ctrl C -> D`,
  restored in the teardown. Built: ~/ios_plj110/x9/out/x9-rev252ag/boot.img 9,871,360 B sha256
  82e116dca6a80867e9fa1c072f1181ac4d8af56221a7a74dd26e54f442523802, 0 errors, x9_wifi_power=6, no mailbox.
  If the patch downloads -> port mt6653DmashdlInit properly; if still stuck -> DMASHDL ruled out, next is the
  RC bridge prefetch window (0x24-0x2c) and AER on both sides.

## 53.txt + source trace: WFDMA AXI slpprot off; connv3 has nothing DMA-relevant; rev252af read-only probe (2026-11-05)

- 实测 53.txt (461,293 B, /tmp/53.txt) = rev252ae. WFDMA wrapper: axi_slpprot 00000004 (only AXI0 IDLE, EN=0;
  nothing cleared), slpprot0 0041f004, slpprot1 0, msi_cfg 40000000, hif_misc 001c2000 -> 001c2001 at the stall
  (HIF_BUSY). DBG_PROBE before -> stall: 01 00445420->034440a0, 02 00550010->00550050, 05 00000080->100000a4,
  0a 0020fc00->0020f80f, 0c 0->16 (others unchanged). Same `TX15 stuck cidx=1 didx=0`.
- Source trace (USER asked to trace before more builds):
  - connv3 power-on on mt6993 (connv3_core opfunc_power_on_internal -> connv3_hw_pwr_on): vsel no-op;
    common_power_ctrl = pmic-en-set + 20 ms + (co-clock only) MT6688 UDS_EN regmap writes (0x0609/0x0629 04->05,
    0x0301 30->10, clock-buffer sleep control) + faultb pull-up; pinctrl setup_pre/done = combo UART + DFD pins;
    connv3_ext_32k_on is a no-op (mt6993 pinctrl ops have no pinctrl_ext_32k_ctrl). Nothing DMA-relevant missing.
  - gen4m: mtk_pci_probe = pci_set_master, MSI, dma mask 36. halHifSwInfoInit before FW DL: mt6653DmashdlInit
    (hal_dmashdl_mt6653.c: PLE/PSE max page, group refill + min/max quota, 32 queue->group maps, priority,
    slot arbiter, optional control, ERROR_FLAG_CTRL 0xEFF), HOST_CSR bus timeout, setupMcuEmiAddr. hifRst
    (CONN_HIF_RST 0x20024100: 0 then 0x30) is only reached via connac1 pdmaConfig, not for mt6653.
  - Correction: "RC saw no error" does not prove the EP never read; DIDX only advances after the packet is done.
- rev252af (commit 20cd366, read-only additions): `X9 WIFI DMASHDL before DMA|stuck:` hif_rst (0x20024100),
  DMASHDL 0x20026000 (bar 0xd6000) sw_ctrl/opt/page/refill/ctrl_sig/pkt_max/grp0/grp1/qmap0-3/dbg0/dbg1/rd02;
  on a stall `X9 WIFI DMA: TX15 desc[0] after stall = w0 w1 w2 w3 (DMADONE= len= last=) buf ...` re-read from
  memory (GLO_CFG bit 6 TX_WB_DDONE set, so DMADONE=1 = WFDMA finished reading the packet). Built:
  ~/ios_plj110/x9/out/x9-rev252af/boot.img 9,871,360 B sha256
  0c001fdad41a302ed81a9d6874de2b99e15cc44817ef77377027f6046e9853f8, 0 errors, x9_wifi_power=6, no mailbox.
  Not device-tested. Next if DMADONE=1: data reached WFDMA, look at the MCU side / DMASHDL (port mt6653DmashdlInit);
  if DMADONE=0: the fetch itself is stuck (inbound path).

## 52.txt: CB sleep protect not the cause; rev252ae dumps WFDMA wrapper + debug probe (2026-11-05)

- 实测 52.txt (549,248 B, /tmp/52.txt) = rev252ad. `CHIP as ROM left it` (52.txt:2421): cb slp_prot hw=fffbffff
  **sw=00000000 already**, slp_status 0000000b, pcie_slp_cfg0 4000002d, cfg1 ffffffff, access_detect 1,
  EP-side AXI_PCIE_IF_CTRL 02040000 (gen4m "HW default" 0x2040004; bit2 = flr_mode_sel, irrelevant), remap_wf
  74037001 (= def). access_detect 1 -> 0 took effect. Same stall `TX15 stuck cidx=1 didx=0 glo=5430b857`;
  `CHIP stuck` slp_status 0000000b -> 0000002b (bit5 set during the stall; meaning unknown).
- So neither the host RC (51.txt) nor CB-infra sleep protection is the block. gen4m never writes
  WFDMA_AXI_SLPPROT_CTRL for mt6653 (only dbg_mt6653.c reads it).
- rev252ae (commit 891aaf6): before DMA and on a stall logs `X9 WIFI WFDMA <when>: hif_misc (0x20027044, bit0
  HIF_BUSY) axi_slpprot (0x20027050: AXI0 EN b0 RDY b1 IDLE b2 ERR b3 VIO_RD b4 VIO_WR b5; AXI1 same at b16..)
  slpprot0/1 (0x20027078/7c) msi_cfg (0x2002702c)` and `probe` = DBG_IDX 0x20024124 <- 0x100+{0,1,2,3,30,5,7,a,b,
  c,62}, DBG_PROBE 0x20024128 (bar = chip - 0x20020000 + 0xd0000). If AXI0/AXI1 SLPPROT_EN is set it clears
  them (`AXI sleep protection was ENABLED ... cleared`) and restores after. Built:
  ~/ios_plj110/x9/out/x9-rev252ae/boot.img 9,871,360 B sha256
  2807fd91ab65b4526db999d94a92a0cad0059e1e048c5f000d048ba24884930e, 0 errors, x9_wifi_power=6, no mailbox.
  Not device-tested.

## 51.txt: host PCIe side clean, DMA still stalls -> chip side; rev252ad = mcu_init writes (2026-11-05)

- 实测 51.txt (368,569 B, /tmp/51.txt) = rev252ac. LK state (51.txt:2388-2390): pextp pwrctl6/8 = 0 (HW mode
  already off), slpprot_rdy = 0, res_req 00055455, req_ctrl c4; RC res_sta ef (all acks), res_ctrl 3, wcpl 0x80,
  no AXI error. Votes "released after 0 ms". Before DMA / at stall (2431-2442): res_ctrl 40a, req_ctrl 10d5,
  int_sta 0, axi0_err 0/0, RC cmd/sta 00100006, secondary status 0 (no master/target abort), EP cmd/sta
  00100006 (no error bits), EP devctl/sta 00002910. Same stall: `TX15 stuck cidx=1 didx=0 glo=5430b857`.
- 推断: the RC never saw a request -> the MT6653 never issued the descriptor read. The RC pre_init/votes did
  not matter (left in place; harmless). Suspect chip-internal gating.
- mt6653_mcu_init (WMT build) first writes CB_INFRA_SLP_PROT_SW_CTRL 0x70025214 = 0 and
  CONN_HOST_CSR_TOP_CSR_AP2CONN_ACCESS_DETECT_EN 0x20060128 = 0, then checks ROM idle. gen4m's HAL_MCR_WR to
  0x70025214 goes through the STATIC table entry 0x70020000 -> bar 0x1f0000 (labelled "(RO)" but used for
  writes); the earlier assumption "static window RO, skip" was wrong. L1 remap alternative (not used):
  CB_INFRA_MISC0_CBTOP_PCIE_REMAP_WF 0x70026554 (bar 0x1f6554, def 0x74037001, low 16 = target >> 16) ->
  window bar 0x1c0000.
- rev252ad (commit ad6042b): inside the driver-own window, before bus master: logs `X9 WIFI CHIP as ROM left it:`
  (cb slp_prot hw 0x1f5210 / sw 0x1f5214, slp_status 0x1f500c, pcie_slp_cfg0/1 0x1f5014/8, access_detect
  0xe0128, EP-side AXI_PCIE_IF_CTRL 0x1d01a8, lpctl, conn_on_misc) and remap_wf, writes both to 0, logs
  `slp_prot_sw X -> Y, access_detect X -> Y`, restores after stage 6; `X9 WIFI CHIP stuck:` on a stall.
  Built: ~/ios_plj110/x9/out/x9-rev252ad/boot.img 9,871,360 B sha256
  3735c737c1666c3987474a76ef108348e2763852a2bd1b7ff9a839eafdca02c8, 0 errors, x9_wifi_power=6, no mailbox.
  Not device-tested.

## 50.txt: stage 6 DMA stalls on the first descriptor fetch; rev252ac adds Linux RC pre_init + votes (2026-11-05)

- 实测 50.txt (405,545 B, /tmp/50.txt on the server) = rev252ab. Link L0 in 15 ms, EP 665314c3, ROM idle,
  driver own in 5 ms, bus master on (EP cmd 00100006), firmware present (sha prefixes match). DMA buffer
  pa=0x145aa4000. Rings programmed as intended: tx15=45aa4000/00010010 (count 16, addr bits 32-35 = 1),
  GLO_CFG 5430b85d (TX/RX EN, ADDR_EXT). After CIDX=1: `TX15 stuck cidx=1 didx=0 glo=5430b857` (50.txt:2528):
  TX_DMA_BUSY set, DIDX never moved, 0 events. Teardown, SET_OWN, undo and power-off all clean; link stayed L0.
- 推断: the EP's read of host memory (descriptor fetch) never completed -> host inbound path (RC master side),
  not the descriptor/command format. Not yet checked: RC AXI error registers, sleep protection, SPM resource acks.
- Linux (pcie-mediatek-gen3.c) on mt6993 does before link start: mtk_pcie_pre_init_6993 (RESOURCE_CTRL 0xd2c
  APSRC_ACK bit10 + SYS_CLK_RDY 0xa, WCPL_TIMEOUT 0x340 = 4, PEX_SPC2 0xd8 bit0, PEXTP_REQ_CTRL bit12
  DDREN_ACK_SEL, PEXTP_PWRCTL_0 |= 0xff00, MAC_CTR1 0x74 bit28, PEXTP_CLOCK_CON low-power clock sel, AXI_IF bits
  we already set), clkbuf 26M force (bit0, already), and control_vote_v2: HWMODE_EN (bit0) of PEXTP_PWRCTL_6
  (RC, who=0) / PWRCTL_8 (Wi-Fi, who=1; gen4m pcie.c votes it off while busy), then waits for PEXTP_SLPPROT_RDY
  0x264 bits 2|3 (port0 MAC/PHY sleep protect) to read 0.
- rev252ac (commit 3146171, `x9_wifi_power=6`): with stage >= 6, before link start logs `X9 WIFI RC as LK left
  it:`, applies the pre_init writes above (not MAC_CTR1/CLOCK_CON), clears HWMODE_EN in PWRCTL_6/8, waits up
  to 10 ms for SLPPROT_RDY & 0xc == 0 (`X9 WIFI RC: votes held ... released|STILL PROTECTED`); restores all
  of it after PERST. `X9 WIFI RC before DMA:` / `stuck:` / `no event:` dump pextp 0x20/0x34/0x40/0x58/0x74/
  0x7c/0x264, RC int_sta/axi_if/misc/res_sta 0xd28/res_ctrl/AXI0_ERR_ADDR 0xe00/ERR_INFO 0xe08/wcpl/spc2,
  RC cmd/sta + secondary status, EP cmd/sta, EP devctl/sta.
- BUILT on the Mac: ~/ios_plj110/x9/out/x9-rev252ac/boot.img 9,871,360 B sha256
  dae30dbcc1e8bbc7db7175ef40aaf463c4febc4b5b38538fd3772bf47f2da27e, EXIT=0, 0 errors, 0 X9WifiNet.inc warnings,
  x9_wifi_power=6, no plugin-package/ -> no mailbox staging. Not device-tested.

## rev252ab: firmware embedded in the kernel; stage 6 = ROM patch download over WFDMA (2026-11-05)

- USER (this session): "boot 分区够不够放 够的话就放不够就走 mailbox". Answer: **boot**. boot.json of rev252aa:
  `boot_partition_size` 67108864 (64 MiB), boot.img 9,478,144 B (lz4 kernel), `max_image_size` 129,499,136;
  firmware patch 386,464 + RAM 1,625,428 + PHY 147,332 = 2,159,224 B. No mailbox staging for Wi-Fi firmware.
- Delivery: `x9/tools/wifi_firmware.py` writes `x9/kernel/generated/x9_wifi_firmware.inc` (gitignored, ~7.8 MB
  decimal text) from `.local/x9/wifi-fw/` (local.json `wifi_firmware` overrides); build.py runs it before every
  compile; content-compare so the object only rebuilds when firmware changes; stub (X9_WIFI_FW_PRESENT 0) when absent.
  Included by X9WifiNet.inc via `../../../generated/` (iokit-cxx-1 has no -I{GEN}). Server copy of the three
  dx5_2g2a files now also in ~/ios_plj110/.local/x9/wifi-fw (sha256 patch aef1eb67..., ram b88ecead..., phy b7599c6c...).
- Patch file facts (parsed): PATCH_FORMAT_V2 (ver ffffffff), date `20260722133306a`, platform ALPS, swhw 108a108a,
  2 sections: sec0 off 0xe0 len 0x11540 -> 0x00900000, sec1 off 0x11620 len 0x4cf80 -> 0xe0010000, both
  sec_info 0x01000000 (AES, key 0) -> datamode 0x80000009.
- CORRECTIONS to the 49.txt stage-6 design (from gen4m source this session): init commands go on **TX ring 15**
  (connac3x asicConnac3xCapInit: u2TxInitCmdPort = TX_RING_CMD, mt6653 tx_ring_cmd_idx 15), firmware data on
  TX ring 16 (raw bytes, no TXD, 2048 B per packet). Command header: TXD DW0 = len | 2<<23, DW1 = 1<<14, then
  u16 len-32, **u16 PQ 0** (connac3x leaves it 0), CID, 0xA0, 0, seq, 28 zero bytes, payload (68 B overhead).
  **No PATCH_SEMAPHORE_CONTROL**: Kbuild.main sets CFG_ROM_PATCH_NO_SEM_CTRL=1 for MT6653+WMT. PATCH_FINISH
  payload {0,0,0,0} (CFG_SUPPORT_WIFI_DL_BT_PATCH off for 6653). Events: RX ring 7, 32-byte RXD + INIT_WIFI_EVENT
  (u16 bytes, u16 type, EID, seq, 2 rsv, payload); EID 1 = CMD_RESULT, payload[0] status.
- Stage 6 code (`x9_wifi_power=6`, commit 1b63934, X9WifiNet.inc x9_wifi_stage6): runs inside the stage-5
  driver-own window; EP cmd MEM|MASTER; 64 KiB IOBufferMemoryDescriptor (contiguous, 36-bit mask, cacheable,
  flush_dcache64 by PA); GLO_CFG 0x5030b850|ADDR_EXT, rings TX15/TX16/RX7 (16 entries), manual prefetch table,
  RST_DTX/DRX, EN, EXT1 0x9c800404, EXT2 0x1404; PATCH_START per section, data, PATCH_FINISH; then DMA off,
  rings cleared, GLO_CFG back, bus master off, SET_OWN, power-off as before. RX descriptors recycled per 64-byte
  line. Log prefixes: `X9 WIFI FW:`, `X9 WIFI DMA:`, `X9 WIFI CMD:`, `X9 WIFI PATCH:`; success line
  `PATCH_FINISH answered status=0 ... -> ROM PATCH DOWNLOADED`.
- BUILT on the Mac (commit c9635d8, cast-align warnings fixed): ~/ios_plj110/x9/out/x9-rev252ab/boot.img
  9,871,360 B sha256 5de64a48f6e3a457e745aba5d6077394288964499e9fa6e226431d0bad8c36b6, kernel 18,475,576 B
  (+~0.4 MB stripped vs rev252aa after lz4: boot.img only +393 KB), payload 18,599,792 B. 0 `error:`,
  0 X9WifiNet.inc warnings, payload has x9_wifi_power=6, no plugin-package/ -> **no mailbox staging**.
  Not device-tested. Boot args = rev252aa + x9_wifi_power=6 (replaces 5).
- Git sync fixed: server origin is now `git@github.com:tosasitill/ios_plj110.git` (the server's GitHub SSH key
  works; https had no credentials). Pushed through c9635d8.
- Mac build gotchas: build.py must run under the python that has `lz4` with `-I`: the Framework python
  `/Library/Frameworks/Python.framework/Versions/Current/bin/python3` (homebrew 3.15 only has lz4 in the user
  site, which -I hides). Non-interactive ssh: `export PATH=/Library/Frameworks/Python.framework/Versions/Current/bin:/opt/homebrew/opt/llvm/bin:$PATH`.
- Expected log (after the stage-5 lines): `X9 WIFI FW: patch ... 386464 B`, `X9 WIFI EP: bus master on`,
  `X9 WIFI DMA: buffer pa=...`, `rings up ...`, per section `PATCH_START answered status=0` and `data sent`,
  then `PATCH_FINISH answered status=0 ... -> ROM PATCH DOWNLOADED`. Failure points to read: `TX15/TX16 stuck`
  (DMA not fetching: bus master/36-bit/inbound path), `no event on RX7` (ROM not answering or event ring wrong),
  status != 0 (2 = CRC, 3 = decrypt).
- Not done / next: RAM code (wlanConnacFormatDownload: tailer, DOWNLOAD_CONFIG CID 1 per region, WIFI_START
  CID 2), PHY RAM code path, connv3_ext_32k_on, MSI.

## 49.txt: MT6653 ROM idle and driver own works; stage 6 (first WFDMA round trip) designed, not written (2026-11-05)

- 实测 49.txt (558,743 B) = rev252aa stage 5. Link L0, EP 665314c3 again (49.txt:2687-2690).
  `rom_sram=00001d1e romcode_index=00001d1e` (MCU_IDLE) lpctl=00000004 before; **driver own TAKEN
  after 5 ms** lpctl=00000000 irq_stat=00000001; while owned WFDMA0 GLO_CFG=1010b850,
  CONN_ON_MISC=00000001, HCR=03040001; SET_OWN gives lpctl=00000004 back (49.txt:2706-2709).
  Clean power-off at 24.787 s.
- Firmware found by the user in the Mac's ~/output/firmware (vendor image dump); copied to
  ~/ios_plj110/.local/x9/wifi-fw/ (gitignored, verified): WIFI_MT6653_PATCH_MCU_dx5_2g2a_1_hdr.bin
  (386,464 B), WIFI_RAM_CODE_MT6653_dx5_2g2a_1.bin (1,625,428 B),
  WIFI_MT6653_PHY_RAM_CODE_dx5_2g2a_1.bin (147,332 B), TESTMODE variants, conninfra.cfg.
  This device is the `dx5_2g2a` variant.
- Stage 6 design (SOURCE: gen4m mt6653.c, hal_pdma.c, cmm_asic_connac3x.c, wlan_oid.c,
  wf_wfdma_host_dma0.h), to be implemented by the next session; the code-writing attempt in this
  session was interrupted twice and nothing was written (tree clean):
  - Rings: init commands on HW TX ring 16 (u2TxInitCmdPort = TX_RING_FWDL); events on RX_RING_EVT =
    HW RX ring 7, each preceded by a 32-byte RXD. WFDMA0 host base 0x20024000 = BAR0 + 0xd4000:
    TX ring n CTRL0..3 = +0x300+n*0x10, RX ring n = +0x500+n*0x10, TX EXT_CTRL +0x600+4n, RX EXT_CTRL
    +0x680+4n, INT_STA +0x200, GLO_CFG +0x208, RST_DTX_PTR +0x20c, RST_DRX_PTR +0x280, EXT0/1/2
    +0x2b0/+0x2b4/+0x2b8.
  - Order (halWpdmaInitRing + mt6653): GLO_CFG = 0x5030b850 | bit26 (ADDR_EXT) without EN; per ring
    CTRL0 = base[31:0], CTRL2 = cidx (TX 0, RX count-1), CTRL1 = count | base[35:32]<<16, EXT_CTRL 4;
    then mt6653WfdmaManualPrefetch EXT_CTRL values: RX4/5/6 = 00000008/00800008/01000008, RX7/8 =
    01800004/01c00004, TX16 = 02000004, TX0..3 = 02000010/03000010/04000010/05000010, TX4/5 =
    06000004/06400004, TX15 = 06800004; RST_DTX_PTR = RST_DRX_PTR = ffffffff; GLO_CFG |= bit0|bit2;
    EXT1 = 0x9C800404; EXT2 = 0x1404. EP config cmd |= bus master (bit 2) for the duration.
  - Descriptor (TXD_STRUCT/RXD_STRUCT, 16 B): w0 SDPtr0, w1 SDLen0 bits 16-29 | LastSec0 bit 30 |
    DMADONE bit 31, w2 SDPtr1, w3 SDPtr0Ext (TX) / RXINFO (RX).
  - Command (asicConnac3xFillInitCmdTxdInfo + wlanSendInitSetQueryCmdImpl): 32-byte TXD (DW0 =
    total | 2<<23 PKT_FT_CMD | queue 0; DW1 = 1<<14 HEADER_FORMAT_COMMAND), INIT_HIF_TX_HEADER 32 B
    (u16 total-32, u16 PQ 0x8000, CID, 0xA0, rsv, seq, then 24 reserved bytes), payload.
    PATCH_SEMAPHORE_CONTROL = CID 0x10 payload {1,0,0,0}; reply EID 4, payload[0] status
    (0 no sema, 1 already patched, 2 got sema need patch, 3 released). Release = payload {0,...}.
  - DMA memory: contiguous, below 4 GiB, cacheable with DC CIVAC around device access
    (render_wc_scrub pattern in SweetX9RenderService.cpp).
  - After that: patch download = PATCH_START (CID 5, {addr,len,mode} from the _hdr.bin header,
    load 0x00900000), data on ring 16 with CID 0 (PKT_FT_PDA_FWDL 3, no init header), PATCH_FINISH
    (CID 7), release sema; then RAM code regions (DOWNLOAD_CONFIG CID 1 per region, WIFI_START CID 2).
    Firmware delivery to the kernel is still open (embed at build time from .local, or mailbox/root).

## 48.txt: MT6653 enumerated (14c3:6653, Gen4 x1, BAR0 2 MiB), chip registers readable; rev252aa (2026-11-05)

- 实测 48.txt (539,796 B) = rev252z stage 4, 48.txt:3016-3923. Link L0 in 15 ms again. RC id
  68a014c3 class 0604 (bridge). After bus numbers 00/01/01: **EP id=665314c3** class/rev=02800000
  (network controller, other) subsys 665314c3, cap list 0x80 PCIe v2 endpoint, linkcap 0141dc14,
  **negotiated Gen4 x1**, devcap 10008fe2; caps 0xe0 MSI (id 05), 0xf8 PM (id 01). BAR0 64-bit
  prefetch-less mem, sizing ffe00004/ffffffff = **2 MiB**. tt0 = 50000035/0/50000000/0/0,
  BAR0 = 50000004, EP cmd MEM. BAR0 reads: CB_TOP 0x70000000/4 = 0, **CB_INFRA1 0x70010200 =
  00006653**, 0x70010204/8 = 00008a00, **CONN_CFG IP version 0x7c011000 = 03040001**, WF_TOP_CFG IP
  version 0x80020010 = 03040001, 0x88000000 = 03040001, 0x88000004 = 00008a00, CONN_ON_MISC
  0x200600f0 = 0. int_sta 0 throughout, link still L0 after the MMIO, clean undo + power-off.
  03040001 = MT6653_CONNINFRA_VERSION_ID / MT6653_WF_VERSION_ID (gen4m include/chips/mt6653.h).
- rev252aa stage 5 (`x9_wifi_power=5`, commit 4925ad5), after the stage-4 MMIO and before the undo:
  ROM state sram 0x7c05b160 (bar 0x1ab160) + WF_TOP_CFG_ON_ROMCODE_INDEX 0x81021604 (bar 0xc1604)
  (mt6653_mcu_check_idle wants 0x1D1E), CONN_HOST_CSR_TOP LPCTL 0x20060010 (bar 0xe0010) /
  IRQ_STAT 0xe0014 / WAKEPU_TOP 0xe01a0; driver own = write bit1 (HOST_CLR_OWN), poll bit2 to 0 for
  200 ms; then ROM state, WFDMA host DMA0 GLO_CFG 0x20024208 (bar 0xd4208), CONN_ON_MISC, HCR again;
  SET_OWN (bit0) back. Not done (Android WMT path skips them or they need the CB dynamic remap):
  CB_INFRA_SLP_PROT_SW_CTRL 0x70025214 = 0 (CB infra2 static window is RO), mcu_reset, ext 32k.
- Built: ~/ios_plj110/x9/out/x9-rev252aa/boot.img 9,478,144 B sha256
  a62b23573260c0d9b1cbd6c6ab80835c2ea05da8128f724b57a94ebdff76f96c, no `--revision` (no
  mailbox staging), EXIT=0, 0 errors, 0 X9WifiNet.inc warnings. Boot args as rev252z with
  x9_wifi_power=5. Not device-tested.
- Firmware needed for the next step (not on the server or the Mac; only on the phone's vendor
  partition): gen4m mt6653.c:1469-1637 names mt6653_wifi.bin / WIFI_RAM_CODE_MT6653_*.bin /
  WIFI_RAM_CODE_6653.bin, WIFI_MT6653_PATCH_MCU_*_hdr.bin / mt6653_patch_e1_hdr.bin (load
  0x00900000), WIFI_MT6653_PHY_RAM_CODE_*.bin, *_idxlog.bin, plus TxPwr/NVRAM. Asked the user to pull
  /vendor/firmware from Android into .local/x9/wifi-fw/ (gitignored, never commit).
- Next after stage 5: firmware download needs WFDMA TX ring 16/FWDL ring + RX event ring in DMA
  memory (IOBufferMemoryDescriptor, 36-bit), bus master on, polling (MSI later), patch semaphore,
  PATCH_START_REQ / FW_SCATTER / PATCH_FINISH, then RAM code (gen4m nic/fw_dl.c + connac3x).

## 47.txt: globe key fixed; PCIe0 LINK UP to L0; EP config read ffffffff (no RC bus numbers); rev252z (2026-11-05)

- 实测 47.txt (608,088 B) = rev252y. USER: globe key works now. `X9 KBD DICT: ... binary plist (449
  bytes); replaced ... bytes=1727` (47.txt:672-673). No Preferences/Spotlight crash.
- 实测 Wi-Fi stage 3 (47.txt:3012-4161): faultb=1 again. Before the reset release every MAC reg read
  0; after `pextp0cfg +0xc = bit0` **base_conf=14110f01 setting=00007081 rst=00088708**
  (47.txt:3020), PHY rx_30=00800432 glb_4c=010f0207 misc=18000002 -> the MAC/PHY were held in reset
  by LK, as inferred. After the RC start: LTSSM 0x00 -> 0x01 -> 0x02 -> 0x0e -> 0x0c -> **0x10 (L0)**,
  `LINK UP after 16 ms` link=00001901 (47.txt:3109), still up after 1 s. EP config read at bus 1 =
  ffffffff for all dwords (47.txt:3110). Clean undo, power-off.
- 推断: ffffffff because the root port (bus 0 devfn 0, type-1) had secondary/subordinate bus 0,
  so the type-1 request for bus 1 was never forwarded (Linux enumeration programs 0x18 first).
- rev252z Wi-Fi stage 4 (`x9_wifi_power=4`, commit 0257af8), inside the link-up window:
  AXI_IF_CTRL 0x1a8 |= bit12 (AXI0_SLV_RESP_MASK) | bit28 (SW_CPLTO_DATA_SEL) first (pre_init_6993:
  completion timeout returns zeros, no slave error); RC id/class, bus 0x18 = 00/01/01, mem window
  0x20 = 0x5000-0x57f0, cmd |= MEM|MASTER; EP id/class/header/subsys/cap walk (PCIe cap: Gen/width);
  BAR0(/1) sizing; if 2..128 MiB: translation table 0 (0x800: cpu 0x50000000 -> pci 0x50000000,
  128 MiB, MEM), BAR0 = 0x50000000, EP cmd = MEM only (no bus master); read-only BAR0 MMIO per
  mt6653_bus2chip_cr_mapping: CB_TOP 0x70000000/4 (bar+0x1e0000), CB_INFRA1 0x70010200/4/8
  (0x1c0200), CONN_CFG IP version 0x7c011000 (0x101000, expect 0x03040001), WF_TOP_CFG IP version
  0x80020010 (0xb0010), TOP_HCR/FVR 0x88000000/4 (0x140000), CONN_ON_MISC 0x200600f0 (0xe00f0).
  Everything is undone (EP cmd/BAR, tt0, RC cmd/bus/window, AXI_IF_CTRL), then PERST and MAC/PHY
  resets re-asserted and power-off as before. No conn_infra wakeup, no DMA, no firmware.
- Built on the Mac: ~/ios_plj110/x9/out/x9-rev252z/boot.img, 9,478,144 B, sha256
  6bb75cc85a9a8dc225c6612fa3b96f27d84536b6c90cc5aba12720355dcdc56b, no `--revision` (allowlist
  17ed5f20, no plugin-package/ -> no mailbox staging), EXIT=0, 0 `error:`, 0 X9WifiNet.inc warnings.
  Boot args as rev252y but x9_wifi_power=4. Not device-tested.
- Next: flash rev252z; read `X9 WIFI EP:` (want id 665314c3 or 0x6653 in it, BAR0 size >= 2 MiB)
  and `X9 WIFI MMIO:` (non-zero CB_TOP/IP version = chip registers reachable). Then: conn_infra
  wakeup + driver-own handshake, firmware download (mt6653 patch/RAM from Android /vendor/firmware),
  WFDMA rings, MSI -> SPI 369.

## 46.txt: typing fixed; globe-key crash = my string KeyboardLastUsedForLanguage; Wi-Fi faultb=1; rev252y (2026-11-05)

- 实测 46.txt (1,093,670 B, ends 55.6 s) = rev252x. USER: typing is fine now (Offline Dictation
  Status seed worked: `X9 DICTATION ... bytes=314`, 46.txt:660), but tapping the globe key to switch
  language makes the input method "crash".
- 实测 crash: `CRASH Preferences[285] sig 6` at 43.678 s (46.txt:6037-6040), slide 0x51cc4000, bt
  symbolized: __pthread_kill <- abort <- libc++abi demangling_terminate_handler <- std::__terminate
  <- __cxa_rethrow <- _objc_terminate <- objc_exception_rethrow <- CFRunLoopRunSpecific+780 <-
  GSEventRunModal <- -[UIApplication _run] = uncaught ObjC exception. No exception text in the log
  (Preferences is not in sweetnull/sweetscan). Live keyboard prefs were rewritten by UIKit to a 751-B
  bplist between 30 s and 40 s (46.txt:5802).
- SOURCE (20H392 UIKitCore/TextInput disassembly, not device):
  -[UIKeyboardPreferencesController setLanguageAwareInputModeLastUsed:] (0x1897572ac) reads a
  preference via valueForPreferenceKey: (0x189757620), sends it objectForKey: (0x18975766c), later
  mutableCopy + setObject:forKey: and passes it to -[TIPreferencesController updateLastUsedKeyboards:]
  (0x18fa44d08), which is setValue:forPreferenceKey:@"KeyboardLastUsedForLanguage" (CFString
  0x1d64e4ab0 in TextInput). rev252v-x seeded that key as the **string** `en_US` -> objectForKey: on
  an NSString -> NSInvalidArgumentException exactly when the input mode changes. 推断 (strong, the
  exception text itself was not captured): this is the globe crash, introduced by my rev252v seed.
- Fix (rev252y, commit 176091d): KeyboardLastUsedForLanguage = dict {en_US:
  en_US@sw=QWERTY;hw=Automatic, zh_Hans: zh_Hans-Pinyin@sw=Pinyin-Simplified;hw=Automatic} (same shape
  as the 767-B original); the "already present" marker is now that dict entry, and an older XML seed
  (has `<key>zh_Hans_CN</key>` but not the marker) is replaced whole instead of merged
  (`X9 KBD DICT: older XML seed ...; replaced`). The live file is currently a bplist, so the bplist
  replace path runs on the next boot anyway. Boot arg `sweetscan=backboardd,SpringBoard,Preferences,
  Spotlight` added so the next abort in those apps prints heap `STR` lines incl. the exception reason.
- 实测 Wi-Fi stage 2 (46.txt:3418-4162): with the GPIO65 pull-up programmed (pupd 6fdf->6fdd, r1
  0->2) **faultb reads 1** right after PMIC_EN (46.txt:3434) and stays 1 for the whole 3.6 s =
  MT6653's MT6376 PMIC up without fault. PCIe0: every MAC register reads 0 before and after our
  writes (setting/cfgctrl/rst/ltssm/link/base_conf = 0, writes do not stick), PHY trx_64/rx_30/
  glb_4c = 0; req_ctrl 0xc4 (writes there did stick and were restored). Link down after 500 ms.
- SOURCE: pextp0ao_rst is ti,syscon-reset (mt6993.dts:1306): assert = write 1 to pextp0cfg +0x8,
  deassert = write 1 to +0xc, MAC bit0 / PHY bit1, STATUS_NONE -> the "rst=0" we read at +0x8 never
  meant released. mtk_pcie_power_up: reset_control_deassert(mac) -> pm_runtime (MTCMOS, already on:
  ap_pd sta 0x1c052900 bit16/17, clk-mt6993-pd.c) -> clocks (already ungated) ->
  deassert(phy) -> phy_init (efuse calibration from nvmem, not done by us).
- rev252y Wi-Fi stage 3 (`x9_wifi_power=3`): stage 2 plus, before the RC start, write 1<<0 then
  1<<1 to pextp0cfg +0xc (MAC then PHY reset release) with base_conf/setting/PHY readback, and
  re-assert both via +0x8 at the end. Still no PHY efuse calibration, no PMIC/clkbuf writes.
- Built on the Mac: ~/ios_plj110/x9/out/x9-rev252y/boot.img, 9,474,048 B, sha256
  76949c9b8110ac70c5a3b85440cdbd1e0cddcdd881abb40f2147055e44146c34, `build.py all` without
  `--revision` (no plugin-package/, allowlist 17ed5f20 present -> no mailbox staging), EXIT=0,
  0 `error:`, no new warnings (X9WifiNet.inc 0; kern_exec.c 7351/7353 are the pre-existing
  declaration-after-statement ones shifted). Boot args: boot.json + x9_wifi_fake=1 x9_wifi_probe=1
  x9_wifi_power=3 x9_siri_off=1 x9_lite=1 x9_sandbox_ops=1
  x9_sample_procs=SpringBoard,Spotlight,kbd,Preferences sweetscan=backboardd,SpringBoard,Preferences,
  Spotlight. Not device-tested.
- Next: flash rev252y, switch input modes with the globe key a few times, pull the log. Read:
  `X9 KBD DICT` line (replaced), no Preferences/Spotlight sig 6 (else the `STR` lines), and
  `X9 WIFI PCIE: MAC reset released: base_conf=` (non-zero = MAC alive) then the LTSSM trail.
  If the MAC is alive but LTSSM stays in Detect: PHY efuse calibration (nvmem pcie0_calibration) and
  the BBCK1 refclk (MT6687 clkbuf) are next.

## 45.txt: rev252w-wifipwr boots; dictation loop root-caused to a nil offline-status cache; rev252x (2026-11-05)

- 实测 45.txt (822,821 B) = rev252w-wifipwr (user flashed it directly, not rev252w-kbdseed).
  `RENDER EXTERNAL: active ... native discovery armed` (45.txt:653-654), 0 SpringBoard crashes,
  desktop reached; user: 「键盘还是卡」.
- 实测 keyboard seed held: `X9 KBD DICT: XML seed already present; left alone` (45.txt:740), live
  file 1578 B mtime 34 in every pass. Spotlight[301] t0 (6 passes 70-120 s, slide 0x1b2ec000,
  symbolized on the Mac with dsc-20H392 + scratch symb.py; report not in git) shows **no
  updateEnabledDictationLanguages:** any more. All 6 are the availability loop:
  postUIKeyboardDictationAvailabilityDidChangeNotification -> updateDockItemsVisibility (+712/+792/
  +1044) -> dictationIsFunctional+284 -> -[UIDictationConnection dictationIsAvailableForLanguage:]
  -> -[AFDictationConnection dictationIsAvailableForLanguage:synchronous:]+696 ->
  forcedOfflineDictationIsAvailableForLanguage:synchronous:+56 -> AFPreferences init/dealloc. The
  truncated caller frame 0x1a44af… = UIDictationConnection dictationConnnectionDidChangeAvailability:
  (0x1891c3140+slide).
- SOURCE (host disassembly, 20H392 AssistantServices, not device):
  forcedOfflineDictationIsAvailableForLanguage:synchronous: (0x19bf007a4) caches
  `[[AFPreferences sharedPreferencesWithInstanceContext:] offlineDictationStatus]
  keysOfEntriesPassingTest:` in ivar +0x18; whenever that ivar is nil it recomputes and
  dispatch_asyncs block_invoke_2 (0x19bf7d0ec) = `_availabilityChanged` ->
  `_tellSpeechDelegateAvailabilityChanged` -> UIKit notification -> dock -> query again.
  `-[AFPreferences offlineDictationStatus]` (0x19beff910) = `_AFPreferencesValueForKeyWithContext(
  "Offline Dictation Status", "com.apple.assistant.support")`, returned only if it isKindOf
  NSDictionary, else nil (CFStrings 0x1d71f28d8 / 0x1d71f1f18 read from the dylib). The key is
  absent on this device -> nil -> empty cache forever -> self-sustaining loop. Only other
  `_availabilityChanged` caller is networkAvailability:isAvailable: (rare).
- Fix (rev252x, commit 5b4db56): sweet_x9_seed_assistant_dictation also writes
  `Offline Dictation Status = <dict/>` (expect `X9 DICTATION ... bytes=` ~316). 推断: ivar becomes an
  empty NSSet (non-nil), the change fires once, dictation stays unavailable, mic key hidden.
- 实测 Wi-Fi round 1 (45.txt:3553-3923): GPIO250/251 read back 1, faultb 0 for the whole 3 s,
  LTSSM/link/rst_ctrl 0, clean power-off. faultb had no pull-up (connv3 selects
  `connsys-pin-pmic-faultb-enable` = GPIO65 input + bias-pull-up R1R0_10 right after pmic-en-set),
  so the 0 is not evidence either way.
- rev252x Wi-Fi stage 2 (`x9_wifi_power=2`, same commit): faultb pull-up via iocfg_lm0 0x13020000
  (IES +0x50, PUPD +0x90 = 0, R0 +0xb0 = 0, R1 +0xc0 = 1, bit 1; pinctrl-mt6993.c, mt6993.dts reg
  index 11), then PCIe0 RC start per pcie-mediatek-gen3.c mtk_pcie_startup_port/mt6993: RC mode,
  pextp0cfg +0x7c bit0 (26M force-on) + bit4 (BBCK1 PMRC bypass), clear 0x84 bit2 if set, assert
  0x148 bits 0-3, 50 ms, release 0-2, 50 ms, release PERST, poll 0x154 bit 8 for 500 ms logging
  each LTSSM state change; EP config read (bus 1, `X9 WIFI PCIE: EP id=`) only on link up; then
  PERST re-asserted, 0x7c restored, faultb back to pull-down, power off as round 1. PHY regs only
  read. Not device-tested.
- Built on the Mac: ~/ios_plj110/x9/out/x9-rev252x/boot.img, 9,474,048 B, sha256
  938af3b6e2b2a32651ed9d1465d5cc7e6778f68edd746b1b64eae4038457ca54, `build.py all` without
  `--revision` (no plugin-package/, kernel still carries allowlist digest 17ed5f20 -> **no mailbox
  staging**), EXIT=0, 0 `error:`, only the pre-existing kern_exec.c:1036/1099/1105 warnings.
  Boot args: boot.json + x9_wifi_fake=1 x9_wifi_probe=1 x9_wifi_power=2 x9_siri_off=1 x9_lite=1
  x9_sandbox_ops=1 x9_sample_procs=SpringBoard,Spotlight,kbd,Preferences.
- Next: flash rev252x, type in Spotlight ~60 s. Read: `X9 DICTATION`
  bytes, Spotlight t0 passes free of the dictation chain, `X9 WIFI POWER: faultb pull-up on:
  faultb in=` (1 = PMIC up, no fault), `X9 WIFI PCIE:` LTSSM trail / LINK UP / EP id.
  If link stays in Detect (0x00/0x01): refclk BBCK1 (MT6687 clkbuf, SPMI) or MT6376 PMIC writes are
  the next suspects.

## Handoff 2026-11-05: test order rev252w -> rev252w-wifipwr

- Git: origin/main had the user's merge b2b3fd0 (NOTES conflict already resolved, no markers left);
  the Mac session's uncommitted 44.txt notes committed from the Mac as 91b3373. Server and Mac both
  at that commit before this note.
- Re-checked the 44.txt diagnosis from the server: 43.txt:668-669 `RENDER EXTERNAL: active ... native
  discovery armed` vs 44.txt:621/623 `missing/mismatched ... discovery disabled`; rev252v boot.img
  holds the restaged digest 235f5a9c, rev252w holds 17ed5f20; Mac
  x9/kernel/generated/ is clean. rev252w sha256 462402ba... re-confirmed on the Mac.
- Wi-Fi B1 round 1 was never device-tested (the ed0466f worktree image predates the keyboard work).
  Built a variant reusing the rev252w kernel, `build.py boot` only (no `--revision`, no recompile):
  ~/ios_plj110/x9/out/x9-rev252w-wifipwr/boot.img, 9,474,048 B, sha256
  095e0349c624ad4a7de116500bc37a24a2e7349a066055edd901ed5728493f70, EXIT=0, boot args = rev252w +
  `x9_wifi_power=1` (verified in build.log and payload.bin). Not flashed, not hardware-verified.
  Non-interactive ssh needs `export PATH=/opt/homebrew/opt/llvm/bin:$PATH` or boot-xnu.py fails
  with "llvm-objcopy missing".
- Order: (1) rev252w-kbdseed: logo must clear (`RENDER EXTERNAL: active`), then type in Spotlight
  ~60 s -> keyboard seed verdict. (2) only then rev252w-wifipwr: first Wi-Fi hardware write
  (GPIO250/251 high ~3 s, then off), user already approved it on 2026-11-04. Read
  `X9 WIFI POWER:` lines: faultb (GPIO65) going to 1 = PMIC up without fault; LTSSM likely stays 0.
- Mailbox: **no staging needed** for rev252w / rev252w-wifipwr. kern_exec.c:8212 calls
  x9_update_install (the mailbox) only when x9_render_external verification of the bundles already
  on the root volume fails; both images carry the committed 252 allowlist (17ed5f20) that matched
  in 43.txt:668-669 with no X9 UPDATE line. Neither dir has plugin-package/, so update.py would
  refuse anyway. Do NOT stage x9-rev252v-kbdseed/plugin-package (the restaged 235f5a9c payload).

## 44.txt: rev252v stuck the phone at the Apple logo because my build restaged the Metal allowlist (2026-11-04)

- 实测 USER: 「一直卡 logo」 with 44.txt (406,799 B, log ends 137.5 s). Kernel and root come up
  normally; the visible failure is SpringBoard crash-looping: 22 `CRASH SpringBoard reason ns 2
  code 0xb` lines, one every ~5.8 s from 14.4 s (44.txt:2173 …), and the display counters never
  copy a frame (`X9 DISPLAY cadence ... copies=0 submits=0` all the way to 136.9 s).
- 实测 cause, first lines that differ from 43.txt: 44.txt:621 `X9 RENDER EXTERNAL:
  missing/mismatched path=/System/Library/Extensions/X9GeneralMetal252.bundle/X9GeneralMetal
  errno=22; discovery disabled` and 44.txt:623 `X9 RENDER EXTERNAL: not armed errno=22`, where
  43.txt:668-669 said `active plugin=X9GeneralMetal252 from verified allowlist` / `exact signed
  Metal/PanVK files verified on root volume; native discovery armed`. With no accelerator the
  sampled SpringBoard dies in the CoreImage software path (slide 0x266e0000, sig 11, **pc=0x0**):
  `-[UIImage _imageWithStylePresets:tintColor:traitCollection:]` (UIKitCore 0x1896ead5c) ->
  `+[CUIShapeEffectStack sharedCIContext]` (CoreUI, dispatch_once) -> `+[CIContext
  contextWithOptions:]` -> `-[CIContext initWithOptions:]` -> `+[CIContext(Internal)
  internalContextWithEAGLContext:options:]` -> `CI::GLContext::GLContext` -> `CI::GLContext::init()
  +204` calling NULL. So "stuck at logo" = no GPU plugin -> CoreImage GL path -> jump to NULL ->
  SpringBoard relaunch loop; it is not the keyboard seed.
- 实测 why the allowlist moved (my mistake, build invocation): x9/build/build.py:279 runs the
  **plugins** step whenever `--revision` is passed together with step `all`, and rev252v was built
  with `all --revision 252`. 44's build.log:35-36 shows `package-plugins.py --revision 252` ran and
  printed "kernel allowlist x9/kernel/generated/x9_render_external_manifest.h updated" (rev252u's
  build.log has no such line and no plugin-package dir). It rehashed the bundle payload: X9GeneralMetal
  sha256 17ed5f20... -> 235f5a9c..., and x9_render_external.inc:13-27 verifies size + full sha256 of
  the files **already installed on the root volume**, so the kernel no longer matched the device copy
  and disabled discovery. Byte-checked in the artifacts: rev252v kernel/boot.img contain the 235f5a9c
  digest, rev252w contain 17ed5f20.
- 实测 the rev252v keyboard seed itself worked and is worth keeping: 44.txt:626-627 `X9 KBD DICT:
  com.apple.keyboard.preferences is a binary plist (693 bytes); replaced with XML seed` + `errno=0
  bytes=1578`, and the live file stayed `size=1578 mtime=34` in **all 11** preference passes up to
  130 s - cfprefsd did not rewrite it, unlike 42.txt where the seed was replaced by a 224-byte file
  at 50 s. The 767-byte `.x9-before-seed` backup is still the untouched original.
- 实测 the lag experiment could not run this boot: only SpringBoard (4 sample events, because the
  process kept dying) and kbd were sampled; Spotlight and Preferences never came up, so there is no
  t0 evidence about `updateEnabledDictationLanguages:` yet.
- rev252w (built, not flashed): same kernel source as rev252v, allowlist restored with
  `git checkout -- x9/kernel/generated/x9_render_external_manifest.h` so it again matches the bundles
  installed on the device; the restaged package and header copy are kept aside in
  x9/out/x9-rev252v-kbdseed/ (plugin-package/, x9_render_external_manifest.h.restaged-252) in case we
  ever want to install the new payload - installing it means deleting the existing bundles on the
  root volume, a device write that needs the user's explicit OK. Built with
  `build.py all --name x9-rev252w-kbdseed --boot-arg ...` **without** `--revision`, EXIT=0, no
  `error:` lines; boot.img 9,474,048 B, sha256
  462402bad7264783633b02b8323ac0567e00e1819a4ae555c69d8b25d502a1a7, host-built, not
  hardware-verified. Boot args unchanged from rev252v.
- Test: flash rev252w and read the first lines for `X9 RENDER EXTERNAL: active plugin=
  X9GeneralMetal252 from verified allowlist` + `native discovery armed`; the logo should clear and
  SpringBoard should stop crashing. Only then type in Spotlight for ~60 s and pull the log, which is
  what gives the rev252v/w seed verdict (`X9 PREF` still 1578 bytes and no
  `updateEnabledDictationLanguages:` frame in the Spotlight t0 passes).
- 教训 for the next session: **never pass `--revision` with step `all`** unless the plugin payload is
  going to be reinstalled on the root volume; `all` without `--revision` keeps the committed
  allowlist (this is the "no restaging" note in project memory).

## 43.txt: rev252u never seeded (gate left at 0); the miss is only part of the cost (2026-11-04)

- 实测 43.txt (723,496 B) is the rev252u boot, same boot args, sampler on
  (`X9 SAMPLE: enabled delay=30 interval=10 count=12 targets=SpringBoard,Spotlight,kbd,Preferences`
  at 43.txt:118) and the typing lag is unchanged (user: 「还是卡」).
- 实测 **there is not one `X9 KBD DICT` line in the whole log**: the rev252u seed gate had been
  left at `sweet_x9_arg("x9_kbd_dictation",0)` while its comment claimed default 1. So 43.txt
  measured the *unseeded* state and rev252u was diagnostics-only.
- 实测 live `com.apple.keyboard.preferences.plist` is a 487-byte bplist00, mtime 155, unchanged on
  every pass 30 s → 130 s (43.txt:4322, 4750, 4906, 5201 …). Its top-level keys are
  `KeyboardLastUsedForLanguage`, `DidShowContinuousPathIntroduction`, `KeyboardsCurrentAndNext`,
  `DictationLanguagesLastUsed`, `DictationLanguagesEnabled`, `KeyboardLastUsed` and the byte dump
  (43.txt:4326 `X9 PREF +288:`) shows **no language string inside either dictation dict** — both
  are stored empty — while `KeyboardsCurrentAndNext` holds `en_US@sw=QWERTY;hw=Automatic` and
  `zh_Hant-Zhuyin@sw=Zhuyin;hw=Automatic`.
- 实测 regression from rev252t: our seeded Pinyin entry is gone from the live file and has become
  **Zhuyin**; the 767-byte `.x9-before-seed` backup (43.txt:4329, mtime 994) still contains
  `zh_Hans-Pinyin@sw=Pinyin-Simplified;hw=Automatic`, `zh_Hans`, the `KeyboardAutocorrection*`
  keys. So the whole-file replacement left the user with the wrong keyboard layout — rev252v seeds
  `KeyboardsCurrentAndNext` back with QWERTY + Pinyin, plus `KeyboardLastUsed` and
  `KeyboardLastUsedForLanguage`.
- 实测 assistant.support is still seeded and its backup is 153 B (43.txt:4319).
- 实测 Spotlight[274] t0, slide 0x113e4000, 11 passes (30 s … 130 s); 7 of 11 touch the dictation
  machinery, and the two expensive branches are *different*:
  - t=30 the typing path (`-[UIKeyboardTaskQueue continueExecutionOnMainThread]` →
    `updateForChangedSelectionWithExecutionContext:` → `completeUpdateForChangedSelection:`).
  - t=40 the recompute: `-[UIKeyboardInputMode dictationLanguage]+808` →
    `-[UIKeyboardInputModeController isDictationLanguageEnabled:]+144` →
    `updateEnabledDictationLanguages:]+120` →
    `updateEnabledDictationAndSLSLanguagesWithCompletionBlock:]+268` →
    `activeDictationSupportedInputModeIdentifiers]+244` → `supportsInputMode:error:]+56` →
    `supportsDictationLanguage:error:]+32` → `+[UIDictationConnection
    dictationIsSupportedForLanguageCode:error:]+136` → `+[AFDictationConnection
    dictationIsSupportedForLanguageCode:error:]+100` → `+[NSError errorWithDomain:code:userInfo:]+32`
    → `nanov2_allocate_outlined`.
  - t=90 and t=120 the same recompute entered from `activeDictationLanguages]+92` and from
    `suggestedDictationLanguagesForDeviceLanguage]+116` → `TIGetSuggestedDictationLanguagesFor
    DeviceLanguage` → libicucore `ualoc_localizationsToUse`.
  - t=100, t=110, t=130 the availability branch: `-[UISystemKeyboardDockController
    updateDockItemsVisibility]+1044` → `+[UIDictationController dictationIsFunctional]+284` →
    `-[UIDictationConnection dictationIsAvailableForLanguage:]+96` → `-[AFDictationConnection
    dictationIsAvailableForLanguage:synchronous:]+696` → `forcedOfflineDictationIsAvailable…+52/56`
    (+ at t=130 `dictationConnnectionDidChangeAvailability:` →
    `postUIKeyboardDictationAvailabilityDidChangeNotification` → `-[SPUISearchHeader
    updateDictationButtonEnabledStatus]`, i.e. the answer re-enters the same path).
  - t=50 a CFPreferences read (`CFPrefsSearchListSource alreadylocked_copyValueForKey:`) and t=80
    `-[TIPreferencesController valueForPreferenceKey:]+108` from
    `updateDockItemsVisibility]+112` → `globeKeyDisplaysAsEmojiKey` → `enabledInputModeIdentifiers:`.
- 实测 (host disassembly of 20H392 UIKitCore/AssistantServices, not the device) the control flow
  that makes a stored false only a partial win:
  - `-[UIKeyboardInputModeController isDictationLanguageEnabled:]` (0x1890eb5b0): reads
    `DictationLanguagesEnabled` through `valueForPreferenceKey:`, `[dict allKeys]
    containsObject:lang`; **absent → `updateEnabledDictationLanguages:` (0x1890eb63c)**; present →
    `boolValue`, false returns NO at 0x1890eb6a4 (cheapest), true calls
    `[[[dictationController sharedInstance] dictationConnection]
    dictationIsAvailableForLanguage:]` at 0x1890eb690.
  - `-[UIKeyboardInputMode dictationLanguage]` (0x1890eb180): `x19 = defaultDictationLanguages:
    ([self languageWithRegion])`; `x26 = valueForPreferenceKey:@"DictationLanguagesLastUsed"` and
    the fallback at 0x1d5c42408 = **`LastUsedDictationLangauge`** (Apple's typo). If
    `[x19 count]==0` it jumps straight to 0x1890eb488 with x24 = `languageWithRegion`. Otherwise
    `x23 = x26[languageWithRegion]`; if x23 is non-nil **and** enabled → early return at
    0x1890eb2bc. Anything else falls to 0x1890eb2c0 = the `suggestedDictationLanguages
    ForDeviceLanguage` scan, then 0x1890eb488 re-checks x24, then 0x1890eb4b4 tries the
    `LastUsedDictationLangauge` value, and **when nothing answers YES it lands at 0x1890eb4fc →
    `-[UIKeyboardInputModeController enabledDictationLanguages]` (0x18915f4e8)**, which walks
    `activeDictationLanguages` (0x18915f610) → `activeDictationSupportedInputModeIdentifiers`
    (0x18915f794), and that getter has **no cache at all**: it rebuilds an array from
    `activeInputModes` and runs one `supportsInputMode:error:` (0x18915f884) per mode, i.e. one
    AF capability query + one NSError allocation per keystroke.
  - `-[UISystemKeyboardDockController updateDockItemsVisibility]` (0x1890eab7c) calls
    `+[UIDictationController dictationIsFunctional]` at 0x1890eaf8c on **both** sides of the
    `shouldShowDictationKey` branch (0x1890eaf34 only gates the `enabledDictationLanguages`
    shortcut, and that selector is `+[UIDictationController
    fetchCurrentInputModeSupportsDictation]`, not a preference). So the dock probe cannot be
    turned off by any keyboard preference.
  - `-[UIDictationConnection dictationIsAvailableForLanguage:]` (0x1890ebab8) rewrites `_` to `-`
    (`componentsSeparatedByString:`/`componentsJoinedByString:`) before asking AssistantServices,
    while the preference dict is keyed by the underscored `languageWithRegion`.
- 推断 (from the two disassembly facts above, not measured on device): no preference value can make
  `dictationLanguage` cheap on this port, because the only early exit needs a stored **true** whose
  `dictationIsAvailableForLanguage:` also answers yes, and `_AFDictationCapable()` is false here
  (that is why every `dictationIsSupportedForLanguageCode:` frame ends in NSError code 400/401).
  The seed therefore removes only the `updateEnabledDictationLanguages:` recompute and its
  cfprefsd write-back — which is exactly the 42.txt 540→224-byte rewrite and 43.txt's empty dicts,
  i.e. the loop where an empty computed dict makes the next lookup miss again. rev252v is the
  single-variable test of that: gate back to 1, 26 spellings covering all four query sources
  (underscore `en_US`/`zh_Hans`/`zh_Hant` forms included, the two full input-mode identifiers),
  `LastUsedDictationLangauge` seeded, Pinyin restored, `sweet_sample.c` buffer 1024 → 2048 so the
  ~1 KB seed dumps whole. Read the next log for: (1) `X9 KBD DICT` present and the live file
  still ours at 30/40/50 s; (2) whether `updateEnabledDictationLanguages:` disappears from the
  Spotlight t0 passes while the availability/supported-modes frames stay; (3) whether the user
  still perceives lag. If (1) holds and the lag is unchanged, the fix has to move from the
  preference to the capability itself (make the AF service/prefix check succeed so UIKit's own
  recompute converges to a non-empty enabled dict) — which would light up the mic key and then
  fail when tapped, and must be disclosed before doing it.
- Output x9/out/x9-rev252v-kbdseed/boot.img: kernel.development.sweet 18,065,544 B, Image
  18,821,120 B, boot.img 9,469,952 B, sha256
  f15c4b8b53783ae8adc62ca1d98622b2ee6b87522755919c39db14780ca6d8f8. Built on the host, EXIT=0,
  zero `error:` lines (only the pre-existing kern_exec.c warnings); **not flashed, not
  hardware-verified** — the user flashes. Boot args unchanged: boot.json plus
  `x9_wifi_fake=1 x9_wifi_probe=1 x9_siri_off=1 x9_lite=1 x9_sandbox_ops=1
  x9_sample_procs=SpringBoard,Spotlight,kbd,Preferences`.
- 实测 kbd[162] and the other sampled targets: Spotlight t0 was the only thread showing this chain;
  kbd's main thread stayed in CFRunLoopRun (as in 42.txt).

## 42.txt: rev252t seed landed, lag unchanged; dictationLanguage always scans (2026-11-04)

- 实测 42.txt is the rev252t boot (same boot args as 40/41.txt, log ends 119.914 s) and the typing
  lag is unchanged (user: 「还是卡」).
- 实测 the seed ran this time: 42.txt:652-653 `X9 KBD DICT: com.apple.keyboard.preferences is a
  binary plist (767 bytes); replaced with XML seed` + `... replaced binary plist with XML seed
  (new file) errno=0 bytes=540`. 42.txt:4305-4311 byte-dump the live 540-byte XML at 30.136 and it
  is exactly our seed (en/en-US/en_US/zh-CN/zh_CN/zh-Hans/zh-Hans-CN/zh_Hans_CN all `<false/>` plus
  `DictationLanguagesLastUsed</key><dict/>`); still 540 bytes at 40.140 (42.txt:4852).
- 实测 cfprefsd/kbd rewrote the file at ~50 s: 42.txt:5190 `size=224 mtime=72` and the same 224
  bytes through 110.169 (42.txt:6935). Only pass 0 was byte-dumped, so the keys of that 224-byte
  file — which are the spellings UIKit itself considers authoritative — are still unknown.
- 实测 the 767-byte original is now in the log (42.txt:4312-4319): top-level keys
  KeyboardAutocorrection_approxDateOfBuildInstall, KeyboardLastUsed, KeyboardAutocorrection_analyzedAt,
  KeyboardAutocorrection_buildAtLastAnalysis, DictationLanguagesEnabled, KeyboardLastUsedForLanguage,
  KeyboardsCurrentAndNext, DidShowContinuousPathIntroduction; value strings `en_US`, `zh_Hans`,
  `en_US@sw=QWERTY;hw=Automatic`, `zh_Hans-Pinyin@sw=Pinyin-Simplified;hw=Automatic`, NonASCII,
  ASCII, Capable. rev252t's whole-file replacement dropped KeyboardsCurrentAndNext (the enabled
  keyboard list) from the live copy.
- 实测 Spotlight[275] t0 (run=1) is inside the dictation chain in 12/12 passes (slide 0xd548000):
  t=30/40/100 suggestedDictationLanguagesForDeviceLanguage -> TIGetSuggestedDictationLanguagesForDevice
  Language -> +[NSBundle preferredLocalizationsFromArray:forPreferences:] ->
  _CFBundleCopyPreferredLanguagesInList -> libicucore ualoc_localizationsToUse; t=50 and t=90 show the
  full 12-frame chain from the app: -[SPUISearchHeader updateDictationButtonEnabledStatus] (t=50) and
  -[UISystemKeyboardDockController updateDockItemsVisibility] (t=90) -> +[UIDictationController
  dictationIsFunctional] -> -[UIKeyboardInputMode dictationLanguage]+672/+496 ->
  -[UIKeyboardInputModeController isDictationLanguageEnabled:]+144 -> updateEnabledDictationLanguages: ->
  updateEnabledDictationAndSLSLanguagesWithCompletionBlock: -> activeDictationLanguages ->
  activeDictationSupportedInputModeIdentifiers -> -[UIDictationController supportsInputMode:] ->
  +[UIDictationConnection dictationIsSupportedForLanguageCode:error:]; t=60 updateDockItemsVisibility ->
  dictationIsFunctional+284 -> -[UIDictationConnection dictationIsAvailableForLanguage:] ->
  -[AFDictationConnection dictationIsAvailableForLanguage:synchronous:]+696 -> forcedOffline...+56 ->
  +[AFPreferences sharedPreferencesWithInstanceContext:] -> initWithInstanceContext:+208 (posts
  notifications); t=70 -[UIKeyboardPreferencesController forwardInvocation:] ->
  -[TIPreferencesControllerClient forwardInvocation:] -> -[NSXPCConnection _sendInvocation:...];
  t=80 __63-[AFDictationConnection _tellSpeechDelegateAvailabilityChanged]_block_invoke ->
  postUIKeyboardDictationAvailabilityDidChangeNotification -> updateDockItemsVisibility again, i.e. the
  availability notification feeds the same per-layout path.
- 实测 kbd[162] main thread stays in CFRunLoopRun in every pass (the keyboard process itself is idle);
  42.txt has 52 `CRASH syncdefaultsd` lines (13 aborts, down from 14) and 8 `CRASH assetsd` lines
  (2 aborts at 17.2/17.4 s); still zero SiriTTSSynthesizer/MacinTalkAUSP/KonaSynthesizer lines.
- 实测 host disassembly of 20H392 UIKitCore (not the device): -[UIKeyboardInputMode
  dictationLanguage] (0x1890eb180) computes defaults = -[UIKeyboardInputModeController
  defaultDictationLanguages:]([self languageWithRegion]) and returns early at 0x1890eb488 when that
  array is empty; otherwise it reads DictationLanguagesLastUsed (selref table slot 0x1d5bdbd80) with
  -[TIPreferencesController valueForPreferenceKey:], and only when
  DictationLanguagesLastUsed[languageWithRegion] exists **and** -[... isDictationLanguageEnabled:]
  answers YES for it does it return that language (0x1890eb2bc); otherwise it falls into
  suggestedDictationLanguagesForDeviceLanguage + the enabled-language scan at 0x1890eb2c0.
  isDictationLanguageEnabled: (0x1890eb5b0) = valueForPreferenceKey:@"DictationLanguagesEnabled" ->
  allKeys containsObject:lang; missing key -> updateEnabledDictationLanguages: (0x1892a569c, which
  runs the whole updateEnabledDictationAndSLSLanguagesWithCompletionBlock: scan); present key ->
  objectForKey:/boolValue, and a stored false returns NO at 0x1890eb6a4 with **no** availability
  query. updateEnabledDictationAndSLSLanguagesWithCompletionBlock: keys its result from
  activeDictationSupportedInputModeIdentifiers x -[... defaultDictationLanguages:].
- 推断 (from the two disassembly facts, one measured contradiction): the rev252t seed cannot
  short-circuit dictationLanguage by construction, because DictationLanguagesLastUsed was seeded empty
  and every Enabled value false, so the scan runs on every call. The contradiction is that with the
  540-byte seed on disk the scan's candidates still missed (updateEnabledDictationLanguages: ran in
  every pass), so at least one queried spelling is not among the eight we seeded — or
  TIPreferencesController never served our XML. The 224-byte rewrite is the authoritative answer and
  is what rev252u now dumps every pass.
- rev252u (built, not flashed): exception.c sampler walks 28 instead of 12 frame-pointer records and
  the line buffer is 1024 instead of 512, so the callers below isDictationLanguageEnabled: stay
  visible; sweet_sample.c x9_prefcheck byte-dumps both *live* preference files on every pass (the
  .x9-before-seed backups stay first-pass only); kern_exec.c adds `zh_Hans` and the two full
  input-mode identifiers (`en_US@sw=QWERTY;hw=Automatic`,
  `zh_Hans-Pinyin@sw=Pinyin-Simplified;hw=Automatic`) to the DictationLanguagesEnabled seed and
  re-seeds KeyboardsCurrentAndNext with those identifiers, so the enabled keyboard list survives the
  replacement. x9_kbd_dictation still defaults to 1.
- Output x9/out/x9-rev252u-prefdump/boot.img: kernel 18065544, Image 18821120, boot.img 9469952
  bytes, sha256 892117873db60edcc788b5e4504e06c1e882ae342f8f79597075395db06306d7. Host-built only,
  not hardware-verified. Boot args as in rev252t (boot.json + x9_wifi_fake=1 x9_wifi_probe=1
  x9_siri_off=1 x9_lite=1 x9_sandbox_ops=1 x9_sample_procs=SpringBoard,Spotlight,kbd,Preferences).
- Test: flash rev252u, type in Spotlight between ~35 s and ~140 s, pull the log. Read: (1) the
  every-pass `X9 PREF +N:` lines of the live keyboard preferences — the 224-byte file's key spellings
  tell us exactly what isDictationLanguageEnabled: asks for; (2) whether the deeper Spotlight t0
  stacks show updateEnabledDictationAndSLSLanguages... returning an empty dictionary
  (activeDictationSupportedInputModeIdentifiers -> supportsInputMode failing) and who calls it;
  (3) whether the Pinyin layout is still selectable.
- Next (rev252v, decide from 43.txt): seed DictationLanguagesLastUsed[languageWithRegion] = <a
  language whose Enabled entry is true> plus Enabled true for the dictation languages and
  com.apple.assistant.support `Offline Dictation Status` = {<lang>: {Installed = true; High Quality =
  true}} so the availability query answers YES without assistantd. Trade-off to state plainly:
  +[UIDictationController dictationIsFunctional] then becomes true, so the mic button lights up and
  tapping it will fail on this port.

## 41.txt: rev252s seed declined on the binary plist; rev252t replaces it (2026-11-04)

- 实测 41.txt is the rev252s boot (same boot args as 40.txt; log ends 136.296 s) and the typing lag
  is unchanged.
- 实测 41.txt:688 `X9 KBD DICT: com.apple.keyboard.preferences is a binary plist (767 bytes);
  left alone`: the rev252s seed never applied. 41.txt:4389-4392 `X9 PREF: ...keyboard.preferences
  .plist size=767 mtime=994 mode=0100600 uid=501` with a bplist00 header; 41.txt:4393 the backup
  lookup ended errno=2, so no .x9-before-seed existed yet.
- 实测 the assistant.support seed still survives: 41.txt:4385-4387 dumps the XML body with
  Assistant Enabled=false and Dictation Enabled=false.
- 实测 the rev252s op-4 budget change worked: 41.txt:792-879 names 8 op-4 callers (launchd, mount,
  mount_hfs, seputil, mobile_obliterat, mount, MSUEarlyBootTask, cc_fips_test) and ops 5/6/0x43
  stay unanswered; the 19 op-5 lines 41.txt:1767-5514 are all ret=2, and no op 6/0x43 reached the
  table. syncdefaultsd still aborts 14 times (56 CRASH lines), so the ENOENT answer did not fix it.
- 实测 sampling: 41.txt:119 `X9 SAMPLE: enabled delay=30 interval=10 count=12
  targets=SpringBoard,Spotlight,kbd,Preferences`; 12 passes 30.137..130.173 s. kbd[153] main
  thread sits in CFRunLoopRun in every pass (the keyboard process is idle), while Spotlight[267]
  t0 is inside the dock/dictation chain in 12/12.
- 实测 the 12 Spotlight t0 stacks rotate through the recompute path: t=30 updateEnabledDictation-
  Languages -> supportsDictationLanguage XPC; t=40 AFPreferences dealloc/stringWithFormat; t=50
  CFPrefs copyAppValue; t=60 dock image/traits; t=70/80 TIGetSuggestedDictationLanguagesForDevice
  Language + NSBundle; t=90 globeKey/enabledInputModeIdentifiers array compare; t=100
  MKBGetDeviceLockState IOKit; t=110 NSXPCConnection send; t=120 availability -> forcedOffline ->
  kevent. Host symbolication of the 20H392 cache, slide 0x2a178000.
- 推断 (corrected): 40.txt's missing-en_US diagnosis was not tested by rev252s; 41.txt is
  consistent with it, because the seed was refused before any write.
- 实测 host disassembly of 20H392 UIKitCore (not device): -[UIKeyboardInputModeController
  isDictationLanguageEnabled:] (0x1890eb5b0) reads TIPreferencesController
  valueForPreferenceKey:@"DictationLanguagesEnabled" (CFString object 0x1d5c43ee8, next table slot
  0x1d5c43f08 is DictationLanguagesLastUsed), then allKeys/containsObject: language. When the key
  is present it does objectForKey:/boolValue and returns that bool (stored false -> return NO at
  0x1890eb6a4) without updateEnabledDictationLanguages: or dictationIsAvailableForLanguage:.
  updateEnabledDictationLanguages: keys the dictionary with -[UIKeyboardInputMode
  languageWithRegion] (en_US form).
- rev252t (built, not flashed): the kbd seeder replaces a bplist00 file with the XML seed (hardlink
  backup .x9-before-seed first; temp 0600 501:501 + rename), seeds DictationLanguagesEnabled=false
  for en, en-US, en_US, zh-CN, zh_CN, zh-Hans, zh-Hans-CN, zh_Hans_CN and an empty
  DictationLanguagesLastUsed, and skips when the `zh_Hans_CN` marker is already present. XML files
  still merge before the last </dict>. x9_prefdump now has a 1024-byte buffer and the first pass
  also dumps the .x9-before-seed backups, so the next log carries the original 767-byte binary
  contents (key names and stored values).
- Output x9/out/x9-rev252t-kbdseed/boot.img: image 18821120 / boot.img 9469952 bytes, sha256
  6778b2f47b8027807ef8f1995ed887c0677d716c4a29259722f0ccb0ec2d69ee. Host-built only, not
  hardware-verified. (build.py `all` stopped because x9_metal_render=1 is already in boot.json;
  the kernel had linked, so only `build.py boot` was rerun with the six extra args.)
- Test: flash rev252t, type in Spotlight between ~35 s and ~140 s, pull the log. Read:
  (1) `X9 KBD DICT: ... binary plist (767 bytes); replaced with XML seed` and
  `... replaced binary plist with XML seed ... errno=0 bytes=...`; (2) `X9 PREF:` for the live
  keyboard prefs (XML, not bplist00) and the full .x9-before-seed dump (the keys UIKit had);
  (3) whether the 12 Spotlight t0 stacks leave updateEnabledDictationLanguages/TIGetSuggested/
  NSBundle and shrink to a CFPrefs lookup; (4) whether the typing lag changes. Known risk: the
  live file loses its other keys (KeyboardsCurrentAndNext, autocorrection stats); the original
  stays in the backup hardlink.

## 40.txt: the stall is a per-frame dictation recompute; rev252s seeds the keyboard pref (2026-11-04)

- 实测 40.txt is the rev252r boot: 40.txt:1069 boot args `x9_metal_render=1
  launchd_unsecure_cache=1 launchdsuffix=release x9_wifi_fake=1 x9_wifi_probe=1 x9_siri_off=1
  x9_lite=1 x9_sandbox_ops=1 x9_sample_procs=SpringBoard,Spotlight,kbd,Preferences`; log ends 140.558 s.
- 实测 the appex/TTS storm is still gone: 0 lines naming SiriTTSSynthesizer/MacinTalkAUSP/
  KonaSynthesizer/IntentsExtension/assetsd in the whole log (38.txt had 12 each). WirelessRadioMan
  and syncdefaultsd both continue (56 CRASH lines each = 14 aborts). So rev252r's ENOENT answer
  for ops 5/6/0x43 did not stop syncdefaultsd; the rev252q 推断 that a success answer without a
  token/handle caused it is falsified.
- 实测 `SWEET Sandbox op:` wrote exactly 24 lines and every one is op 4 (container path), so ops
  5/6/0x43 were never named: the shared budget is consumed in arrival order. rev252s gives op 4
  its own 8-line budget so the other ops get the 24.
- 实测 the assistant.support seed survives: 40.txt:4148 `X9 PREF: ...assistant.support.plist
  size=268 mtime=34 mode=0100600 uid=501` with the XML body at 40.txt:4149-4151, unchanged at
  40.140/50.143. So cfprefsd did not rewrite it; X9 JOBS 40.txt:793 `already disable=7 lite=1 size=601`.
- 实测 all 12 Spotlight[274] passes (30.136..140.178 s) have t0 (run=1, main thread) inside the
  dictation availability chain. Host symbolication of the 12 t0 stacks against the 20H392 cache
  (slide 0x255cc000): UISystemKeyboardDockController updateDockItemsVisibility ->
  +[UIDictationController dictationIsFunctional] -> -[UIKeyboardInputMode dictationLanguage] ->
  -[UIKeyboardInputModeController isDictationLanguageEnabled:] ->
  -[UIDictationConnection dictationIsAvailableForLanguage:] ->
  -[AFDictationConnection ...synchronous:] -> forcedOfflineDictationIsAvailableForLanguage: ->
  +[AFPreferences sharedPreferencesWithInstanceContext:] (init + Darwin observers + CFPreferences
  read -> sync cfprefsd XPC, kevent_id/_dispatch_kq_poll) -> _tellSpeechDelegateAvailabilityChanged
  -> UIKeyboardDictationAvailabilityDidChange -> dock update again.
- 实测 host disassembly of the same binaries (not device): -[UIKeyboardInputMode dictationLanguage]
  calls isDictationLanguageEnabled: up to 5x per invocation; isDictationLanguageEnabled: reads
  "DictationLanguagesEnabled" through -[UIKeyboardPreferencesController valueForPreferenceKey:] ->
  TIPreferencesController, whose domain is com.apple.keyboard.preferences, and only calls
  updateEnabledDictationLanguages: and dictationIsAvailableForLanguage: when the current language
  is absent from that dictionary. +[UIDictationController dictationIsFunctional] also calls
  dictationIsAvailableForLanguage: unconditionally after its locked/telephony/passcode gates.
- 实测 the kernel/compositor are not the limiter: 42 of the 2-s `X9 DISPLAY cadence` windows report
  copies=20 submits=20 with target=60 (the app commits 10 frames/s while the kernel presents every
  tick); 14 windows report 0/0.
- 推断: the missing en_US entry is what makes every dock update recompute, so seeding
  DictationLanguagesEnabled={en_US:false} and DictationLanguagesLastUsed={} into
  com.apple.keyboard.preferences should reduce the per-frame path to dictionary lookups. It cannot
  remove dictationIsFunctional's own availability call, so the lag may only shrink. If the on-disk
  plist is bplist00 the merge is declined (logged), which the new probe reports.
- rev252s (built, not flashed): `sweet_x9_seed_kbd_dictation` in kern_exec.c merges the two keys
  before the last `</dict>` of
  /private/var/mobile/Library/Preferences/com.apple.keyboard.preferences.plist (creates the file
  when absent; hardlink backup .x9-before-seed.plist; temp 0600 501:501 + rename; refuses binary,
  >8 KiB and </dict>-less files), and `x9_prefcheck` also dumps that plist plus its backup (size/
  mtime each pass, bytes on the first). Boot args identical to 40.txt.
- Output x9/out/x9-rev252s-kbddict/boot.img: image 18821120 / boot.img 9469952 bytes, sha256
  2edd2fdc72524e35c70f7a46d6cc33832ea15cd2d4c468e2b023bf689cd5e248. Host-built only,
  not hardware-verified.
- Test: flash rev252s, type in Spotlight between ~35 s and ~140 s, pull the log. Read:
  (1) `X9 KBD DICT:` (new file / merged / binary / no </dict>); (2) `X9 PREF:` size + first bytes
  for com.apple.keyboard.preferences (bplist00 vs XML) and whether size/mtime change between
  passes; (3) whether the 12 Spotlight t0 stacks are still deep in AFPreferences/cfprefsd;
  (4) `SWEET Sandbox op:` lines for ops 5/6/0x43.

## 39.txt: rev252q fixed the appex storm; syncdefaultsd regressed; lag still unexplained (2026-11-04)

- 实测 39.txt is the rev252q boot (boot args 39.txt:742: x9_wifi_fake/probe=1 x9_siri_off=1 x9_lite=1;
  `SWEET Sandbox: launchd op 4 container path` 39.txt:737; JOBS `already disable=7 lite=1 size=601`
  39.txt:661; SIRI `entries=32 already=32 changed=0` 39.txt:659-660). Log ends at 146.516 s.
- 实测 the appex crash storm is GONE: zero SiriTTSSynthesizer/MacinTalkAUSP/KonaSynthesizer/
  IntentsExtension/assetsd/widget crashes in the whole log (38.txt had 12-12 each). Only
  WirelessRadioMan 14 (fake radio, sig 5 code 0x5, every 10 s 10.5..137.2), syncdefaultsd 13,
  driverkitd 1 (also 1 in 38.txt). The op-4 container answer (PKService bootstrap) worked.
- 实测 NEW regression in rev252q: syncdefaultsd aborts sig 6 (52 CRASH lines / 4 = 13 crashes) at
  11.0, 12.4, 12.9, then every ~10 s to 114.1 (launchd throttle), 0 occurrences in 34..38.txt.
  Same innermost chain as the appex aborts: pc 0x1ea5a3198 slide 0x27f8c000 -> unslid
  0x1c2617198 = libsystem_kernel `___pthread_kill+0x8` (identical unslid pc in 35/38), then
  libc++abi terminate + libobjc `_objc_exception_throw`, but the outer frames are libdispatch
  workers (unslid 0x18db98848/0x18db6b65c/0x18db735d4/0x18db6c1f8), not PlugInKit. The only
  userland-visible change from rev252p is the sandbox answer: op 5/6/0x43 went from ENOENT to 0.
- 推断 (not proven): returning success for extension issue (5) without a token, or consume (6)
  without a handle, is what syncdefaultsd trips over. rev252r restores ENOENT for 5/6/0x43 and
  keeps rev252q's op-4 container path (x9_sandbox_ext_ok=1 restores 0 without a rebuild).
- 实测 the lag is not kernel input and not the GPU: every `SWEET HID: touch ... down/up` has its
  `X9 TOUCH ... HID dispatched` line in the same millisecond; cadence every 2 s has missed=0,
  TE timeouts 0, ticks 120/121, copy_avg ~1.6 ms; render jobs 20-21/2 s, max_job_us 10-20 ms,
  GPU util 2-29%. Largest timestamp gap in the log is 0.92 s (console stitch, not a freeze).
- 实测 the GUI presents at 10 Hz: swap_id 485 at 60.272, 585 at 70.272, 685 at 80.272 (exactly
  100/10 s), copies=submits=19-21 per 2 s window. Two typing bursts in the keyboard area
  (touch y 2060-2260 of 2760): 61.874-~65 (two-finger typing from 63.7) and 80.865-90.98; the
  present rate did not rise during either (no frame per keystroke). Only when Spotlight/searchd
  work starts at 84.5-90.6 s does it rise 7 -> 21 -> 26 -> 32 per window. searchd AKS burst
  86.9-87.6 and 116.99-117.02; corespeechd/siriactionsd/voiced/siriinferenced are alive,
  assistantd/sirittsd absent.
- 推断 the keystrokes produce no frames because the process hosting the text input is stalled on
  its main thread - the same shape as 30/32.txt (dictation-availability recompute), but this boot
  typed in Spotlight, so that is a guess until sampled. Nothing in this log shows the blocking
  call itself.
- rev252r (built, not flashed): x9_sandbox_ops=1 adds a bounded (24-line) once-per-(process,op)
  line `SWEET Sandbox op: name[pid] op N ret= len=` for ops 4/5/6/0x43, so the next log names
  every extension-op caller (and says whether syncdefaultsd still crashes); boot args add
  x9_sample_procs=SpringBoard,Spotlight,kbd,Preferences (30 s, every 10 s, 12 passes, named
  targets only - not the all-process scan that panicked in rev252m/n). Commit 9bdeb94.
  Output x9/out/x9-rev252r-lagprobe/boot.img: image 18804736 / boot.img 9469952 bytes,
  sha256 7a3225e5567d2de1ca04521888d8541bc5b4f8c32cae82c8818a55e77dec5a9e. Host-built only.
- Test: flash rev252r, open the place you type in (Spotlight / Settings / Notes), keep the
  keyboard up and type between ~35 s and ~140 s so all 12 samples land, then pull the log.
  Read: (1) `X9 SAMPLE` stacks of the app main thread during typing - is it in the dictation/
  input-mode chain or another blocking call; (2) `SWEET Sandbox op:` lines for ops 5/6/0x43 and
  whether syncdefaultsd disappears from CRASH; (3) swap_id should still advance 100/10 s at idle.

## 35.txt: the launchd patch persisted, but every sampled target was idle (2026-11-04)

- 实测 the root copy of launchd.plist stayed patched across the reboot: 35.txt:763
  `X9 SIRI: disable size=796447 magic=bplist00 entries=32 already=32 changed=0 errno=0`
  and 35.txt:776 `readback mismatches=0`. assistantd/sirittsd again never appear in the boot.
- 实测 the churn is unchanged: by 143.8 s SiriTTSSynthesizer/MacinTalkAUSP/KonaSynthesizer have
  12 sig-6 crashes each (34.txt had 6 each by 79.5 s - same ~10-12 s rate) and locationd has 42
  sig-5. So disabling assistantd/sirittsd did not stop the crash source, as in 34.txt.
- 实测 sampler `[35.txt:371] ... targets=SpringBoard,Preferences,MobileSafari,MobileNotes,kbd`,
  12 passes from 30 to 140 s (35.txt:4340..5865). Only SpringBoard[26] and kbd[155] were ever
  found: there is no `Preferences[`, `MobileSafari[` or `MobileNotes[` line in the entire log.
  SpringBoard t0 and kbd t0 are both `run=3` (TH_STATE_WAITING) in mach_msg in all 12 passes, with
  unchanging pc/lr/bt (SpringBoard 35.txt:4354..5855, kbd 35.txt:4342..5852). The log ends at
  147.9 s.
- 推断 the app the user types in was neither SpringBoard nor kbd (it was probably not any of the
  four named apps), so this boot still has no stack of the stalling process. Sampling by name keeps
  missing it; the next build identifies it by thread state instead (rev252m).

## rev252q: the appex crash storm is our sandbox shim's container-path answer (built 2026-11-04, not flashed)

- 实测 38.txt (rev252p) the policy ran: 38.txt:675-676 `X9 SIRI: disable size=796447
  magic=bplist00 entries=32 already=32 changed=0 errno=0` + `readback mismatches=0`; 38.txt:677-679
  `X9 JOBS: original kept as disabled.x9-orig.plist errno=0`, `disable=7 lite=1 added=7 kept=0
  size=601 errno=0`, `readback mismatches=0`. Boot args at 38.txt:1081 include `x9_lite=1`.
- 实测 locationd is gone (0 occurrences in the whole log) and Wi-Fi is untouched (wifid 301 lines,
  fake scans at 143.521/144.329). The stall the user still saw ("还是卡") is therefore not fed by
  locationd and not by the pruned jobs.
- 实测 crash census up to 209.186 s: SiriTTSSynthesiz/MacinTalkAUSP/KonaSynthesizer 12 each, in
  triplets about every 5 s from 26.364 to 143.795 and none afterwards; assetsd 3;
  IntentsExtension/WeatherIntents/StocksDetailInte/ScreenTimeWidget/MobileTimerInten 1 each at
  18.6-19.7 s; MusicWidgets 3 (sig 11, separate); WirelessRadioMan 21x sig 5 code 0x5, exactly one
  every 10.0 s from 13.457 to 208.210. The TTS churn stopping at ~144 s does not mean the boot
  became healthy.
- 实测 every sig-6 appex and assetsd die with the same pc/lr/far and a 10-frame backtrace:
  pc 0x20a097198 lr 0x2196265f8 far 0x2268f2e28. With slide 0x47a80000: libsystem_kernel
  `___pthread_kill+0x8`, libsystem_pthread `_pthread_kill+0xd0`, libsystem_c `abort+0x7c`,
  libc++abi terminate frames, libobjc `_objc_exception_throw+0x168`, CoreFoundation+0xeb704,
  PlugInKit+0x18d5c. 35.txt (slide 0x43d0c000) resolves to the same unslid addresses, so the
  chain is boot-stable.
- 实测 disassembly: 0x186ed3698 is `+[NSException raise:format:]` (it formats with
  `_CFStringCreateWithFormatAndArguments`, calls `exceptionWithName:reason:userInfo:` then
  raises), so 0x186ed3704 is its return address; the raiser is PlugInKit itself. PlugInKit
  0x1aaf39d58 calls the objc_msgSend stub 0x1aaf53880 with x0 = NSException (classref 0x1dda93f68,
  CoreFoundation `__objc_data`), x2 = an imported NSString constant, x3 = CFSTR("%@"), and builds
  the reason from the CFString at 0x1d8339d00 = "plugin has invalid sandbox or container: error %d
  path %s". Nearby strings: "Bootstrapping; failed to get my own sandbox container", "Failed to
  start plugin; could not consume sandbox service extensions; %@", NSExtensionSandboxProfile,
  com.apple.private.pluginkit.sandbox-override. That is `-[PKService checkEnvironment:]`.
- 实测 the cause is our own shim: sweet_sandbox_syscall returned ENOENT for op 4 (container path
  of a pid/audit token), 5/6 (sandbox extension issue/consume) and 0x43. launchd is the only op-4
  caller visible (38.txt:1077) because the shim logs only the first use of each op number.
- 实测 reference: .local/x9/work/references/hackios-xnu-kernel/xnu/files/bsd/kern/kern_minimac.c
  (c84b463) implements request 4 for exactly this failure: PlugInKit pre-fills the buffer with "?"
  and wants a longer path back, else it raises "plugin has invalid sandbox or container"; the path
  is CFFIXED_USER_HOME, which the launcher (RunningBoard for an app, pkd for an extension) got from
  containermanagerd and left among the strings exec placed at the top of the process stack. A
  process without the variable gets success and an untouched buffer.
- rev252q ports that: `sweet_sandbox_container_path()` in x9/kernel/src/bsd/kern/sweet_amfi.c
  (proc_find + pidversion check, copy the p_argslen bytes above p->user_stack the way
  sysctl_procargsx does, task_reference before proc_rele, scan for `CFFIXED_USER_HOME=`, bounded
  copyout). Non-container ops now return 0 as in the reference instead of ENOENT; that also
  changes op 5/6 from rev252p and is the one judgement call in this patch.
- Build: `.local/build-venv/bin/python3 -I x9/build/build.py all --name
  x9-rev252q-sandbox-container --boot-arg x9_wifi_fake=1 --boot-arg x9_wifi_probe=1 --boot-arg
  x9_siri_off=1 --boot-arg x9_lite=1`, PATH with /opt/homebrew/opt/llvm/bin. Output
  x9/out/x9-rev252q-sandbox-container/boot.img: image 18804736 / boot.img 9469952 bytes.
  Host-built only, not hardware-verified. Commit b022e56.
- Test: flash rev252q, boot to the desktop, type past ~150 s, pull the log. Expect the TTS triplet
  storm and the 18.6 s IntentsExtension/assetsd/widget sig-6 crashes to disappear from `CRASH`;
  `SWEET Sandbox: ... op 4 container path` should be logged for the first process that asks. If
  the sig-6 storm persists, the appex environment has no CFFIXED_USER_HOME and the buffer still
  answers "?" - the next step is a boot-arg-gated log of the op-4 result (length/errno/pid), not
  more job pruning. 推断 only: the keyboard/IME appex launch path goes through the same PKService
  bootstrap, so this churn is the best candidate for the input lag seen so far; that stays a
  guess until the crash count drops and typing is retested on the device.

## rev252p: launchd disabled.plist policy, on-device jobs only (built 2026-11-04, not flashed)

- Code: `sweet_x9_seed_disabled_jobs()` in x9/kernel/src/bsd/kern/kern_exec.c, called from
  `load_init_program_at_path()` right after the siri_off seed. Boot args: `x9_disable_jobs=<label>,...`
  (default empty) and `x9_lite=1` (default 0 -> off). Before launchd starts it writes
  /private/var/db/com.apple.xpc.launchd/disabled.plist with `<key>label</key><true/>` for every
  label: the pristine file is copied once to disabled.x9-orig.plist, each rewrite starts from that
  backup, the keys are spliced in front of the closing `</dict>` (the existing dhcp6d/bootpd entries
  survive), the file goes out through temp + rename, root:wheel 0644, fsync, then a read-back prints
  `readback mismatches=`. `disabled.migrated` is created if missing. A label already present in the
  base is not added twice; when every label is already there the file is not rewritten
  (`X9 JOBS: already ...`).
- Lite list is deliberately on-device only (7 labels): locationd, photoanalysisd, mediaanalysisd,
  coreduetd, mobile.obliteration, assistantd, sirittsd. sheng's radio/network members of the cycle
  (identityservicesd, CommCenter, bluetoothd) and the other network jobs are NOT in lite because
  Wi-Fi is under bring-up on this board (user constraint, 2026-11-04); a test that wants the full
  sheng cycle passes them through `x9_disable_jobs=` explicitly. locationd stays because it is the
  daemon that traps every ~3.3 s in our logs. rev252o was built with the 22-label list (sheng's,
  incl. the radio jobs) first; it was not flashed and is withdrawn in favour of rev252p.
- Build: `.local/build-venv/bin/python3 -I x9/build/build.py all --name x9-rev252p --boot-arg
  x9_wifi_fake=1 --boot-arg x9_wifi_probe=1 --boot-arg x9_siri_off=1 --boot-arg x9_lite=1` with
  `/opt/homebrew/opt/llvm/bin` on PATH. Output x9/out/x9-rev252p/boot.img: image 18804736 / boot
  9465856 bytes. Host-built only.
- Test: flash rev252p, boot to the desktop, keep the keyboard up past ~130 s, pull the log. Expect
  one `X9 JOBS: disable=7 lite=1 added=7 kept=0 size=... errno=0` + `readback mismatches=0`
  (later boots print `already disable=7`), locationd gone from the boot, and Wi-Fi untouched. If the
  churn stops and the keyboard does not stall, this is the fix; if churn stops but the keyboard
  still stalls, the loop is not fed by these daemons. If the stall survives, the next test can add
  the radio members explicitly: `x9_disable_jobs=com.apple.identityservicesd,com.apple.CommCenter,com.apple.bluetoothd`
  on top of lite (both are merged).

## UniXNU upstream check: no dictation patch, but a job-pruning mechanism for the XPC wait cycle (2026-11-04)

- 实测 (checked UniXNU, UniXNU-20261030, UniXNU-gpu/UniXNU-sheng and UniXNU-gpu-parity-793d6b
  checkouts): there is no patch about dictation, assistantd, Siri, PlugInKit or the keyboard.
  `git log --grep` and the docs trees have no hit.
- 实测 the related mechanism is `sheng-lite=1` / `sheng-disable-jobs=<label>,...`, implemented in
  UniXNU-sheng/package/BootWatchdogPolicy.c (`sheng_boot_watchdog_policy`) and called from
  `load_init_program()` right after the Data mount, before the launchd exec (the call is added by
  UniXNU-gpu-parity-793d6b/patches/xnu/zzz-boot-progress.patch:215). It writes
  `/private/var/db/com.apple.xpc.launchd/disabled.plist` (and an empty `disabled.migrated`) with
  `<key>label</key><true/>` for every job to disable, then fchown 0:0, chmod 0644, fsync and
  fstat-verify. The file comment states (引 upstream text): "hardware stacks show identityservicesd,
  contextstored and locationd in a cycle of synchronous XPC waits. A disabled job makes its Mach
  service lookups fail instead of hang." The lite list starts with identityservicesd, locationd,
  CommCenter, bluetoothd; docs/devices/asteroids.md:58 says "the lite list holds the XPC wait
  cycle". This is the same failure shape as our 33.txt (sync XPC waits), and it disables jobs by
  launchd label in a data file instead of editing Apple bytes.
- 实测 this mechanism is live on our root: the pan172 image already has
  /private/var/db/com.apple.xpc.launchd/disabled.plist (dhcp6d=true, bootpd=true,
  ftp-proxy-embedded=false) plus disabled.migrated. Our kernel never touches it.
- 实测 another related upstream patch, `native-work-interval-soft-fail`: its comment says "UIKit
  aborts on a refused CA_CLIENT interval; soft-fail it as macOS does". Our tree has only 0064
  (PRIV_WORK_INTERVAL); osfmk/kern/work_interval.c:1068 still returns KERN_FAILURE when a second
  CA_CLIENT interval is created, i.e. the soft-fail is missing. That would be an app abort, not the
  keyboard stall, but it is the same port family problem.
- 推断 our 32-byte launchd.plist patch only removed assistantd/sirittsd, while the churn kept
  running (34.txt/35.txt), because the cycle has other members - locationd crashes every ~3.3 s in
  every log. Porting the disabled.plist seed (boot-arg `x9_disable_jobs=`, optional `x9_lite=`)
  gives one mechanism to disable any label, and can splice the keys into the existing file (backup
  first) so dhcp6d/bootpd entries survive. No code written yet.

## 36.txt: the all-process stall finder panicked the kernel on its first pass (2026-11-04)

- 实测 rev252m was flashed (`x9_stall_scan=1` in 36.txt:859, `[36.txt:119] X9 SAMPLE: enabled
  delay=20 interval=10 count=12 scan=1`), the launchd patch ran normally again
  (`X9 SIRI: ... already=32 changed=0 errno=0` 36.txt:737, readback 0 36.txt:752), the GUI took
  over at 16.654 s, and the last normal line is `[20.085] X9 GPU GOV`. Then the console holds the
  first `X9 SCAN:` lines (mediaserverd[29] t16, sharingd, identityservices, thermalmonitord,
  mobiletimerd, DumpPanic, amfid, remoted, keybagd, wifid, logd, runningboardd, UserEventAgent[23],
  SpringBoard[26] t0 run=1, launchd[1] t0) and the phone reset; the user could not reach the
  desktop (「直接重启了 进不去桌面」).
- 实测 panic tail: `Kernel data abort. at pc 0xfffffe00075349e0, lr 0xfffffe00075349c8`,
  `esr 0x96000005 far 0x0`, `panic(cpu 0 caller 0xfffffe0007c1653c)`,
  `handshake structure not initialized`.
- 符号化: pc -> `_machine_thread_get_state + 0x1e0` (lr +0x1c8), return address ->
  `_sleh_synchronous_sp1 + 0x0`. Disassembly at fffffe00075349dc is `ldr x8, [x21, #0x110]`
  then fffffe00075349e0 `ldr w9, [x8]` with no NULL check: the thread state pointer is NULL, so
  our own `thread_getstatus(ARM_THREAD_STATE64)` dereference faults (far=0). `cmp w8,#0x44` seen
  nearby is ARM_THREAD_STATE64_COUNT (0x44=68), not an exception flavor.
- 推断: walking every process reaches threads that are mid-teardown / crash-looping (the TTS
  appexes and locationd die every few seconds), whose task/thread has no valid arm state; a kernel
  read of another task's thread state is not safe for those. This route is closed.
- Consequence: the scan code was removed; sweet_sample.c and exception.c are back to af3d654.
  rev252m and rev252n (the budgeted-scan rebuild) must not be flashed. rev252l
  (x9/out/x9-rev252l/boot.img, commit af3d654) is the last image that boots to SpringBoard and is
  the one to flash to get the desktop back.

## Voice-appex crash chain, symbolized (36.txt/35.txt slides) (2026-11-04)

- 实测 35.txt: SiriTTSSynthesizer[264] at 26.523, MacinTalkAUSP[262] at 26.121,
  KonaSynthesizer[263] at 26.370, all sig 6, reason ns 2 code 0x6; slide 0x43d0c000.
  pc 0x206323198, lr 0x2158b25f8, far 0x222b7ee28. bt `0x1d19684b8 0x2157eaf54 0x2157dba4c
  0x1c3ddeafc 0x2157ea424 0x2157ecda0 0x2157ecd60 0x1c3dd9d58 0x1cabdf704 0x1eec45d5c`.
- 符号化 (runtime - slide, mapped through the extracted 20H392 dyld cache at .local/x9/work/dsc-20H392):
  pc libsystem_kernel `___pthread_kill + 0x8`; lr libsystem_pthread `_pthread_kill + 0xd0`;
  then libsystem_c `_abort + 0x7c`; libc++abi `demangling_terminate_handler + 0x12c` and
  `__ZSt11__terminatePFvvE + 0xc`; libobjc `_objc_terminate + 0x7c`; libobjc
  `_objc_exception_throw + 0x168`; CoreFoundation `___CFDictionaryCreateGeneric + 0x0`; then a
  PlugInKit frame (symbols sparse there, function identity uncertain).
- 推断: the voice plugins die of an uncaught ObjC exception raised inside a CoreFoundation call
  made from PlugInKit during plugin load - not from the assistantd XPC. Disabling assistantd/
  sirittsd (rev252k) therefore cannot stop this churn, which matches 34.txt/35.txt exactly.
  The next step is offline: why the PlugInKit load of these appexes throws on this root image
  (extension-point Info.plist, NSExtension attributes, dyld-cache/dylib mismatch), not another
  device probe.

## 34.txt: rev252k patch applied and verified, but the keyboard still stalls (2026-11-04)

- 实测 the patch ran on the phone exactly as designed (34.txt:667-701): the on-device original
  bytes were saved errno=0, all 32 single-byte edits printed (`off=303802..304527 0x63->0x7a` for
  the 30 assistantd MachServices keys and com.apple.sirittsd, `off=305003 0x61->0x7a`,
  `off=344864 0x73->0x7a` for the two daemon paths), then
  `X9 SIRI: disable size=796447 magic=bplist00 entries=32 already=0 changed=32 errno=0` and
  `readback mismatches=0`. assistantd and sirittsd then never appear in the whole boot: no
  `SWEET AKS: assistantd`, no sample for the assistantd target, no sirittsd line of any kind. So
  launchd read the patched plist and neither job was spawned.
- 实测 the churn did not stop: SiriTTSSynthesizer/MacinTalkAUSP/KonaSynthesizer still abort at the
  same rate (6 each by 79.5 s, one every ~10-13 s; 33.txt had 11 each by ~130 s) and locationd
  sig 5 22 times by 79.5 s (33.txt: 38). 推断: those .appex processes are not launched by
  assistantd/sirittsd, and their crashes do not re-post the availability notification through
  those daemons. The user reports the keyboard still stalls.
- 实测 34.txt has no `Preferences[...]` line and no `SWEET AKS: Preferences` line at all - this
  boot was not tested in the Settings app (targets were Preferences,kbd,assistantd; only kbd was
  ever found, 5 passes / 25 lines from 30 to 70 s). The log stops at 79.5 s with the last
  `X9 DISPLAY cadence ... missed=0` at 78.573, i.e. it was pulled a few seconds later and the
  display path was healthy up to the cut. The frozen process was therefore something else
  (Spotlight search runs inside SpringBoard, which was not sampled) and we have no app-side stack
  for this stall.
- Consequence: sampling only named daemons is too narrow. The next build widens the target list
  (SpringBoard,Preferences,MobileSafari,MobileNotes,kbd) instead of guessing the app.

## rev252l: same launchd patch, wider sampler (built 2026-11-04, flashed, results in the 35.txt section)

- Kernel code unchanged from rev252k. Only the boot args differ:
  `x9_sample_procs=SpringBoard,Preferences,MobileSafari,MobileNotes,kbd` (30 s delay, 10 s
  interval, 12 passes), `x9_siri_off=1` (the plist is already patched, so the seed should print
  `already=32 changed=0` + `readback mismatches=0`), `x9_wifi_fake=1 x9_wifi_probe=1`.
- Build: same two commands. Output x9/out/x9-rev252l/boot.img: image 18804736 / boot 9461760
  bytes, allowlist 252, no restaging, boot.img sha256
  fb0cd5bd0b2ba9480830c2b890cdf52128b4d318a63ebf37bf66b381031ce94d, kernel sha256
  25d5d3c1803c68d3f30bb772cde519d20631234c7c00cb8b88d0dd6fab03e523 (unchanged), payload sha256
  dd256ccbe5268b295a3a8f4e163009f1d7009a3c1d3e026b82341d6579136ba8. Host-built only.
- Test: reproduce in the same app as 34.txt (if that is not Spotlight/main-screen search, say
  which app it was), keep the keyboard up past ~130 s so all 12 passes land, then pull the log.
  Read: the SpringBoard/app main-thread stacks. If it is still the
  UIDictationController/UIDictationConnection recompute chain even though the XPC now fails fast,
  the loop is self-sustaining and the fix has to move to the UIKit side (or to whatever still
  triggers the notification). If the stack is a different chain, that chain is the new lead.
- The root copy of launchd.plist stays patched across boots; a `--boot-arg x9_siri_off=0` build
  restores the original 32 bytes (and prints `restore ... readback mismatches=0`).

## rev252k: route B delivered as a same-length launchd.plist patch (built 2026-11-04, not flashed)

- Why: 33.txt (实测) keeps the app main thread in the dictation/input-mode recompute in 11/11
  sampled passes while assistantd[30] t0 is parked in
  xpc_connection_send_message_with_reply_sync; the Siri TTS voice plugins still abort every ~10 s
  (SiriTTSSynthesizer/MacinTalkAUSP/KonaSynthesizer sig 6, 11 each) and locationd every 3.3 s.
  User chose route B (make assistantd + Siri TTS unreachable) and delivery by boot.img, as a
  same-length in-place edit of `/System/Library/xpc/launchd.plist` (no root reflash; the only local
  root image is pan172-era).
- Layout (实测): the local pan172 `edited-root.hfs` copy of `/System/Library/xpc/launchd.plist` is
  byte-identical in the probed region to the phone - size=796447, `bplist00`, numObjects=22269, and
  the 33.txt:5135-5146 needle offsets (221907 / 303962 / 304100 / 308486) match. assistantd job:
  `ProgramArguments[0]` path string object at 304941 (this exact path occurs once in the file), 30
  `MachServices` key string objects in 303802..304935; sirittsd job: path string at 344805, its one
  service key at 344781. Each of those 32 strings is referenced exactly once in the plist (plistlib
  walk over all 533 jobs), so renaming them cannot touch another job.
- Code (x9/kernel/src/bsd/kern/kern_exec.c, `sweet_x9_siri_off()`, called in the pre-launchd seed
  block right after the dictation seed; boot-arg `x9_siri_off`, default 1):
  - guards: file size 796447 + `bplist00` magic, then for every target the exact bplist string
    object (`0x5F 0x10 len` header + full string) at its recorded offset. All 32 are classified
    before any write, so a different image is left untouched (prints the failing offset); it also
    refuses if the edited byte is neither the original nor the patched value.
  - edit: service keys `com.apple.*` -> `zom.apple.*` (first byte), daemon paths basename
    `assistantd` -> `zssistantd`, `sirittsd` -> `zirittsd` (first byte). Client lookups of the old
    MachService names then fail with bootstrap unknown-service instead of queueing a message for a
    job that cannot spawn, and launchd's own spawn of the daemons fails with ENOENT.
  - safety: on the first patch the on-device original bytes are written to
    `/System/Library/xpc/launchd.x9-siri-orig.txt`; the file is fsynced and read back, printing
    `X9 SIRI: readback mismatches=N`. `x9_siri_off=0` restores the recorded original bytes and
    needs no other state (the patch itself persists on the root until then).
  - 推断: the jobs stay in the plist (structural removal is impossible with a same-length edit);
    launchd's LaunchEvents may still try to start them, but each attempt is a cheap ENOENT spawn
    failure and launchd throttles failing jobs. The detached `.sig` is already bypassed on this
    port (`launchd_unsecure_cache=1` in every image since 0039u).
- Build: `.local/build-venv/bin/python3 -I x9/build/build.py all --name x9-rev252k --boot-arg
  x9_wifi_fake=1 --boot-arg x9_wifi_probe=1 --boot-arg x9_sample_procs=Preferences,kbd,assistantd
  --boot-arg sweetiolog=assistantd --boot-arg x9_siri_off=1` with `/opt/homebrew/opt/llvm/bin` on
  PATH. Output x9/out/x9-rev252k/boot.img: image 18804736 / boot 9461760 bytes, allowlist 252, no
  restaging, boot.img sha256
  307db489f3f06f616a820e05bf34ec9e0e23a7481d4180c2666f386282de70e9, kernel sha256
  25d5d3c1803c68d3f30bb772cde519d20631234c7c00cb8b88d0dd6fab03e523, payload sha256
  f1f7b68b38e94dcd9c78852f3727e3b94d389fa473bb6891fa78d91ea3af49bc. Host-built only.
- Test: flash x9-rev252k with `fastboot boot`, keep the keyboard up past ~130 s (12 passes). Expect
  `X9 SIRI: disable size=796447 magic=bplist00 entries=32 already=0 changed=32 errno=0`, 32
  `X9 SIRI: off=<n> 0x63->0x7a ...` / four `0x61|0x73->0x7a` path lines, then
  `X9 SIRI: readback mismatches=0`; assistantd absent from `X9 SAMPLE:` and from the AKS lines;
  the TTS sig-6 churn gone. Read: does the Preferences main thread go idle (run=0) and does the
  keyboard stop stalling? If it still stalls with these two jobs unreachable, the remaining trigger
  is not assistantd/sirittsd (locationd's sig 5 loop is independent) and needs the UIKit-side gate.
- Restore: build a candidate with `--boot-arg x9_siri_off=0` and boot it once; it writes the
  original bytes back and prints `X9 SIRI: restore ... errno=0` / `readback mismatches=0`.
- Unfinished: no device run yet; if the patch is kept, the root copy of launchd.plist stays patched
  across later boot.img revs.

## rev252j: read-only launchd.plist layout probe for route B (built 2026-11-04, not flashed)

- Why: route B (make the assistantd/Siri TTS launchd jobs disappear) must be delivered as a
  same-length in-place byte edit of `/System/Library/xpc/launchd.plist`, because the only local root
  image is pan172-era and NOTES says it is NOT the current phone state, so a root reflash would roll
  the device back. A same-length edit needs the live byte layout first: recovery has no hfsplus, so
  there is no dd-by-name, and the recorded v5 offset 0x1e025f000 / 1456980 bytes is stale after the
  v8 and v13 plistlib rewrites. Decision (user): probe first, then patch with verified offsets.
- New code (x9/kernel/src/bsd/dev/sweet_sample.c, read-only, boot-arg `x9_xpcscan=1`): at the first
  sampler pass (t=30 s) open the plist, read up to 2 MiB in 256 KiB chunks, and print
  `X9 XPC: <path> size=... got=...`, `X9 XPC: magic=bplist00 head=..`, the bplist trailer
  (`offSize refSize numObjects top table`), and for each needle in
  com.apple.assistantd / com.apple.siriactionsd / com.apple.SiriTTSService / SiriTTSSynthesizer /
  MacinTalkAUSP / KonaSynthesizer / /usr/libexec/assistantd the total count plus up to 4 hits as
  `X9 XPC: <idx> #<n> off=<byte offset> ctx=[16 bytes before]<needle>[16 bytes after]`
  (non-printables as '.'). Nothing is written; `x9_prefcheck` from rev252i still runs alongside.
- Build: same two commands as rev252i (`all`/`boot`, PATH needs /opt/homebrew/opt/llvm/bin) with
  `--boot-arg x9_wifi_fake=1 --boot-arg x9_wifi_probe=1
  --boot-arg x9_sample_procs=Preferences,kbd,assistantd --boot-arg sweetiolog=assistantd
  --boot-arg x9_xpcscan=1`. Output x9/out/x9-rev252j/boot.img: image 18804736 / boot 9461760 bytes,
  allowlist 252, no restaging, boot.img sha256
  570694c9ccc442b36c9cf2f065ca3e9820f7b9d62c380fb14404f9474f6677c0, kernel sha256
  767fdb476f7b0bbeba4dcee863b99f71b0be8f430e60de8b02f6d67071ce1a24. Host-built only.
- Test: flash x9-rev252j, keep the keyboard up past ~130 s (12 passes) so the pref read-back and the
  XPC scan both land, then pull the log as usual. Then: use the printed offsets/counts to pick the
  single-byte target (prefer the `Program`/binary path string, which only that job uses, over the
  label string that other jobs may reference) and build the patcher with a byte-window check before
  each write plus a backup file and a restore switch.

## rev252i: plist read-back from the sampler + assistantd sampled (built 2026-11-04, not flashed)

- 实测 correction to the section below: the "SWEET AKS" lines are the AppleKeyStore IOKit
  user-client trace, not assistant-services output. assistantd appears there twice per boot
  (32.txt:1846-47 at 11.74 s, 31.txt:1852-53 at 13.11 s, sel 0 then sel 7 -> 0x0) and has no CRASH
  line in either log, i.e. it starts, opens AppleKeyStore, and then stays quiet in the log while the
  app keeps re-asking. Crash churn counts confirmed by header lines: SiriTTSSynthesiz/MacinTalkAUSP/
  KonaSynthesizer 10 each (sig 6), locationd 40 (sig 5).
- New kernel code (boot.img only, no root change, 实测 compile+link clean):
  - x9/kernel/src/bsd/dev/sweet_sample.c: `x9_prefcheck()`/`x9_prefdump()` run at the start of every
    sampler pass. vnode_getattr on com.apple.assistant.support.plist prints
    `X9 PREF: <path> size=... mtime=... mode=... uid=...`; on pass 1 (t=30 s) it also dumps the
    first 256 bytes as escaped ASCII (`X9 PREF +0: ...`), and the `.x9-before-seed.plist` backup is
    size-only. `x9_prefcheck=0` disables. If the seed survived: size=268 with an `<?xml` header and
    both `<key>...Enabled</key> <false/>` pairs inside the dump (they end near byte 253; the last
    ~15 bytes are outside the 256-byte window). If cfprefsd rewrote it: size != 268 and/or a
    `bplist00` header. XNU field name is `va_modify_time`, not `va_mtime` (first compile failed).
  - x9/kernel/src/bsd/kern/kern_exec.c: the seed line now prints `bytes=%d` (268) so write size and
    read-back size are comparable inside one log.
- Build: `.local/build-venv/bin/python3 -I x9/build/build.py all --name x9-rev252i --boot-arg
  x9_wifi_fake=1 --boot-arg x9_wifi_probe=1 --boot-arg x9_sample_procs=Preferences,kbd,assistantd
  --boot-arg sweetiolog=assistantd`. The boot/packaging step needs `/opt/homebrew/opt/llvm/bin` on
  PATH or boot-xnu.py dies with "llvm-objcopy missing: install LLVM or ziglang" (the kernel step
  itself is unaffected). Output x9/out/x9-rev252i/boot.img: image 18804736 / boot 9457664 bytes
  (same as rev252h), allowlist 252, no restaging, boot.img sha256 dca68511a98afc46a23bc91c4cb4ca4b9f6915dc97d7611ebdda72cf80e1f49c,
  kernel sha256 b2f121f3ae46d68c28ccc7e25973937fe55ad49487d9967c1bebe444692bbc7b. Host-built
  candidate only - no device write, no boot result.
- Test: flash x9/out/x9-rev252i/boot.img with `fastboot boot` (user does this), bring the keyboard
  up and keep it up past ~130 s so all 12 passes run. Read: (1) X9 PREF at pass 1 vs pass 12 - a
  size/header change mid-run means cfprefsd rewrote it; (2) if size stays 268 and the XML is intact
  while Preferences' main thread is still run=1 in the dictation chain, the preference is ignored
  rather than lost -> the fix has to be route B (drop the assistantd/Siri TTS jobs in the root, like
  tools/sweet-root-tweaks.py does for CommCenter) or a UIKit-side gate; (3) assistantd's sampled
  stacks show whether it answers the synchronous availability query or is parked.
- New tool: tools/x9-sample-report.py renders `X9 SAMPLE:` records into symbolicated stacks (takes
  the same symbol table file as tools/sweet-crash.py style runs); it splits records the console
  truncated mid-line. Reports for the two rev252h boots: /tmp/report32.txt, /tmp/report31.txt.

## 31/32.txt: dictation-off seed did NOT stop the keyboard stall; cause is availability churn (2026-11-04)

- 实测 31.txt and 32.txt are two rev252h boots and both reproduce it. The seed ran
  (32.txt:744 "X9 DICTATION: ... errno=0"), yet Preferences' main thread is run=1 in 9 of
  its 10 sampled passes (32.txt:4547,4717,4951,5062,5167,5284,5423,5547,5663) and in 10 of
  12 in 31.txt, and every one of those stacks is the same dictation/input-mode recompute.
  kbd[149] t0 sits in __CFRunLoopServiceMachPort/mach_msg in all passes except 32.txt:5290,
  so the keyboard daemon is idle; X9 DISPLAY cadence keeps missed=0. The stall is in the app
  process main thread, not kbd, not the GPU/display.
- 实测 chain (symbol table /tmp/symb32.out, stacks rendered by the new tools/x9-sample-report.py;
  reports /tmp/report32.txt, /tmp/report31.txt):
  -[UIDictationController postUIKeyboardDictationAvailabilityDidChangeNotification] ->
  _CFXNotificationPost -> -[UISystemKeyboardDockController updateDockItemsVisibility] ->
  +[UIDictationController dictationIsFunctional] -> -[UIKeyboardInputMode dictationLanguage]
  -> UIKeyboardInputModeController enabledDictationLanguages / activeDictationLanguages /
  activeDictationSupportedInputModeIdentifiers / activeInputModes / enabledInputModeIdentifiers:
  -> two expensive sinks: (a) TextInput TIGetSuggestedDictationLanguagesForDeviceLanguage ->
  +[NSBundle preferredLocalizationsFromArray:forPreferences:] -> CFBundle localization math ->
  ICU ualoc_localizationsToUse/uloc_minimizeSubtags (passes 5,7) and
  CFPrefsSearchListSource copies (pass 8); (b) -[UIDictationConnection
  dictationIsAvailableForLanguage:] -> -[AFDictationConnection
  dictationIsAvailableForLanguage:synchronous:] (pass 4) -> xpc_connection_send_message_with_reply_sync
  -> _dispatch_mach_send_and_wait_for_reply (pass 3 shows the send side, _dispatch_mach_msg_send).
  So each availability flip = full input-mode/locale re-enumeration on the main thread plus a
  blocking XPC round trip. Pass 9 (32.txt:5423) frames 0x1b34/0x1b35/0x1b36/0x1b37e6e94 are
  unsymbolicated (same UIKit keyboard range, 推断 same path).
- 实测 churn is still there: SiriTTSSynthesizer/MacinTalkAUSP/KonaSynthesizer sig 6 x10 each
  (first 32.txt:2517, ~every 10 s) and locationd sig 5 x40, every 3.3 s from 11.2 s on; assistantd
  itself runs (SWEET AKS lines 32.txt:1846). 推断: those daemon state changes are what re-post
  the notification. "Dictation Enabled"/"Assistant Enabled" in com.apple.assistant.support is read
  by assistantd, not by UIKit, so UIKit still asks -> 30.txt's guessed follow-up is now confirmed.
- Local RE inputs: research/dsc/ is gone and `ipsw` is not installed any more, but the shared
  cache copy is at /tmp/sysos20H392/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64
  (+ .01/.02/.03). Reinstall ipsw to disassemble +[UIDictationController dictationIsFunctional]
  and find the UIKit-side gate.
- Next options: (A) boot.img only - kernel re-reads the seeded plist after ~20 s to prove cfprefsd
  kept it, and add assistantd to x9_sample_procs/sweetiolog -> DONE, built as rev252i (section
  above); (B) root reflash -
  drop the assistantd + Siri TTS jobs from /System/Library/xpc/launchd.plist the way
  tools/sweet-root-tweaks.py drops CommCenter, so the sync availability query fails fast and stays
  "unavailable" (also kills the TTS crash churn). locationd's sig 5 loop is a separate bug.
  -> DONE without a reflash, as the same-length boot.img patch in rev252k (section above).
## Wi-Fi B1 round 1 (GPIO power-on test) built (2026-11-04)

- USER: keyboard/language settings normal (English keyboard) -> keyboard parked; USER approved the Wi-Fi
  power-on (first hardware write).
- SOURCE order (connv3_core.c opfunc_power_on_internal, connv3_hw.c, mt6993_pmic.c, gen4m
  cmm_asic_connac3x.c/plat_priv.c): PRE_PWR_ON -> wlan_pre_pwr_on -> wf_rst_on (GPIO250 high,
  RST_PIN_MIN_WAIT_TIME 200 ms) -> connv3_hw_pwr_on: vsel no-op, mdelay(30), pmic-en-set (GPIO251 high),
  mdelay(20), faultb pull-up (GPIO65), combo UART pre-on pull-ups -> gen4m connsys_power_on mdelay(10)
  "pcie phy ready" -> halMawdPwrOn -> PTA_UART_ON. Off: mdelay(20), pmic-en-clr, wf_rst_off.
- Commit ed0466f: x9_wifi_power=1 thread (20 s after IONetworkStack start) does wf_rst_on, 30 ms, PMIC_EN
  high, 30 ms, logs GPIO65 + PCIe0 LTSSM/link/rst_ctrl changes for 3 s (50 ms poll), then off in reverse.
  Only GPIO250/251 mode/dir/out bits written (RMW). No pulls, PMIC registers or PCIe RC writes.
- Built in a SEPARATE worktree because ~/ios_plj110 on the Mac carries another session's unpushed work
  (6 commits rev252i..m keyboard investigation + uncommitted sweet_sample.c/exception.c): worktree
  ~/ios_plj110-wt-wifi (detached ed0466f, .local and x9/build/local.json symlinked). Artifact:
  ~/ios_plj110-wt-wifi/x9/out/x9-wifipwr-ed0466f/boot.img (x9_wifi_fake=1 x9_wifi_probe=1
  x9_wifi_power=1), allowlist 252, 9,457,664 B, sha256 2b3d9f02... Not device-tested.
- Expect: `X9 WIFI POWER: armed`, at ~20 s `WF_RST high`, `PMIC_EN high`, t=... lines (faultb rising to 1
  = PMIC up without fault is the hoped-for sign; LTSSM likely stays 0 because the RC is not started),
  then `off again`.

## 32.txt (rev252h-nowifi): keyboard freeze is NOT Wi-Fi related (2026-11-04)

- USER: still freezes with no fake radio (0 X9 WIFI lines, no wifid/en0). Preferences[277] t0 again in
  the dictation loop (UIDictationConnection dictationIsAvailableForLanguage -> UISystemKeyboardDockController
  updateDockItemsVisibility -> UIKeyboardInputModeController activeDictationLanguages /
  suggestedDictationLanguagesForDeviceLanguage / CFPrefs reads -> NSNotification ...).
- SOURCE (UIKitCore/AssistantServices 20H392): UIDictationConnection only forwards to AFDictationConnection;
  -[AFDictationConnection dictationIsAvailableForLanguage:synchronous:] keeps a per-language dict
  (CheckLanguageAvailable, LanguageAvailableOverNetwork, HQ/LQAssetsAvailable), consults
  AFNetworkAvailability isAvailable and forcedOfflineDictationIsAvailableForLanguage:synchronous:.
  AFNetworkAvailability addObserver path calls _startObservingAvailability then _availabilityChanged
  (0x19bf085bc) -> every new observer gets an immediate change callback.
- 推断 (unverified), two candidates: (1) the dock creates a fresh dictation connection per update, each
  new observer gets "changed" -> next update (self-sustaining); (2) keyboard/language preferences never
  set (Setup skipped by x9_skip_setup) -> enabledInputModeIdentifiers recomputed/written each pass ->
  prefs change notification -> recompute. User check requested: Settings > General > Keyboard > Keyboards.

## 31.txt (rev252h): dictation seed not enough; B1/B2 probe result; A/B build (2026-11-04)

- 实测 B1/B2 probe (read-only): GPIO251 PMIC_EN in/0 (MT6653 OFF: LK never powers it), GPIO248 POR
  out/0, GPIO250/65/243/244 in/0 mode 0; pextp0 cg_sta 0 (all PCIe0 clocks ON), rst 0, pd 0x3ce7f
  (MAC0 + PHY0 powered); pcie0 MAC rst_ctrl/ltssm/link = 0 (idle, no link). => LK leaves PCIe0 powered
  and clocked; only the Wi-Fi chip is off. Next B step = first WRITE: GPIO power-on (PMIC_EN high etc.,
  connv3 order) + LTSSM watch -> needs explicit user confirmation.
- Keyboard: `X9 DICTATION ... errno=0` but USER: still freezes. Preferences t0 still in the same loop
  (dictation languages / dock visibility / AFPreferences / NSXPC to keyboard prefs). AssistantServices:
  -[AFDictationConnection _availabilityChanged] -> _tellSpeechDelegateAvailabilityChanged, fed by
  AFNetworkAvailability (Darwin notify token; AFNetworkAvailabilityDidChangeNotification). 推断: network
  availability changes (fake radio / en0 / wifid) may drive the loop; 31.txt Wi-Fi churn is low though
  (no join, ~4 wifid req/10 s after 37 s). assistantd is running.
- A/B build: x9/out/x9-rev252h-nowifi/boot.img = rev252h kernel, no x9_wifi_fake (no wlan node, no
  wifid, no en0), x9_sample_procs=Preferences,kbd. Test keyboard in Settings' search field.

## 30.txt keyboard freeze root-caused; Wi-Fi B1/B2 read-only probe; rev252h built (2026-11-04)

- 实测 30.txt (rev252g-kbd, keyboard held up): Preferences[276] t0 run=1 in samples 3-12, all in a
  dictation-availability loop: AFDictationConnection forcedOfflineDictationIsAvailableForLanguage:
  synchronous: -> UIDictationConnection dictationConnnectionDidChangeAvailability: -> UIDictationController
  postUIKeyboardDictationAvailabilityDidChangeNotification -> UISystemKeyboardDockController
  updateDockItemsVisibility -> UIKeyboardInputModeController activeDictationLanguages -> ... (sample 2 =
  password prompt, WFNetworkListController -> SFPasswordSharingService passwordSharingAvailability).
  Speech daemons crash-loop (SiriTTSSynthesizer/MacinTalkAUSP/KonaSynthesizer x12 sig 6).
- Fix (commit 33b90c0): kern_exec seed sweet_x9_seed_assistant_dictation writes
  /private/var/mobile/Library/Preferences/com.apple.assistant.support.plist {Assistant Enabled=false,
  Dictation Enabled=false} each boot (x9_dictation_off=0 disables; backup ...x9-before-seed.plist).
  Key/domain from AssistantServices 20H392 (AFPreferences dictationIsEnabled). 推断: may not be enough
  if UIKit queries availability even with dictation disabled -> rev252h samples Preferences/kbd again.
- Wi-Fi B1/B2 facts from MTK source (SOURCE): connv3 MT6993 power-on = vsel no-op, pinctrl
  pmic-en-set (GPIO251 high; co-clock 0 => MT6688 UDS_EN regmap skipped), faultb (GPIO65) pull-up,
  combo UART GPIO243/244 pre-on pull-up then CONSYS UART function, POR reset GPIO248 30 ms pulse,
  WF_RST GPIO250 (wf_rst_on high). GPIO base 0x1002d000 (DIR/DO/DI +0/+0x100/+0x200 per 32 pins, MODE
  0x300+(pin/4)*0x10, 8-bit stride, 5 bits). pextp0cfg_ao 0x169b0000 CG sta +0x14 (1=gated) MAC0_TL 0
  REF 1 PHY0_MCU_BUS 6 PHY0_REF 7 AXI_250 12 AHB_APB 13; reset +0x8/+0xc (MAC bit0, PHY bit1).
  ap_pd 0x1c040000+0x12900 (1=on) PEXTP_MAC0 bit16 PHY0 bit17.
- x9_wifi_probe=1 reads all of the above (no writes); PCIe0 MAC (0x148/0x150/0x154) only if MAC0 power
  bit set and MAC clocks ungated.
- Built x9/out/x9-rev252h/boot.img: x9_wifi_fake=1 x9_wifi_probe=1 x9_sample_procs=Preferences,kbd,
  allowlist 252 (no restaging), sha256 44827af1...

## 29.txt: Wi-Fi iOS side WORKS with the fake radio; keyboard freeze found; rev252g-kbd built (2026-11-04)

- USER: Settings Wi-Fi toggle works, both fake networks listed. 实测: wifid[43] up (ADT /arm-io/wlan
  fix confirmed), 359 requests by 96 s (top types: 103 current network x143, 19 power x23, 11 scan
  result, 383 set x16 [unknown, refused], 254, 10 scan), refused=46, fake join of X9-FAKE-OPEN at
  42.1 s -> state UP, en0 169.254.53.56 (no DHCP answer, expected), tx 36 frames.
- USER: keyboard / input method nearly freezes the UI wherever it appears (so the WPA2 password
  could not be typed). Not Wi-Fi specific; never recorded before in NOTES (keyboard likely never
  exercised). Frames keep flowing (swap_id 3 -> 1382); SpringBoard main thread idle in all samples.
- Commit 8323a09: x9_sample_procs=a,b,c (sampler from 30 s, every 10 s, 12x). Built
  x9/out/x9-rev252g-kbd/boot.img: x9_wifi_fake=1 x9_sample_procs=Preferences,kbd,SpringBoard,backboardd
  sweetiolog=Preferences,kbd. Test: open Settings > Wi-Fi > X9-FAKE-WPA2 and keep the keyboard up
  from ~30 s to ~150 s. Next: symbolicate Preferences/kbd threads (slide from the sample header).
- Open Wi-Fi follow-ups: type 383 set (16x refused) unknown; fake radio has no DHCP.

## 28.txt: wifid never ran -> missing DT node "wlan"; rev252f-wifi built (2026-11-04)

- 实测 28.txt (rev252e-wifi): all 4 Apple80211 requests were configd[36] (IPConfiguration via
  IO80211.framework: user client refused -> ioctl bind, card caps, SSID). No `IOK wifid` line and wifid
  absent from all 6 sampler passes -> wifid not running.
- SOURCE: com.apple.wifid.plist (RunAtLoad, KeepAlive) has _LimitLoadToDeviceTree = [wlan,
  marconi-wifi]; launchd 20H392 per-name check 0x10003d1f0 = IOKit MIG 2887
  io_device_tree_entry_exists_with_name. Our ADT had no node named wlan.
- LK DT copy complete (16 KiB, 272 lines): pcie0 @0x16910000 status "okay", SPI 0x171 (369); pcie1
  "disabled"; phy@16900000 "okay"; wifi node emi-addr still 0 (address only via reserved-memory
  mblock-52-consys_wifi_emi 0xe1000000), emi-size 0x1900000; wifi-tx-cma
  emi-size 0x5400000; wifi-misc 0x100000.
- Commit 150dfdb: mkadt.py adds /arm-io/wlan (device_type wlan, compatible wlan-pcie,mt6653, fake MAC)
  when the boot line has x9_wifi_fake=1. Built x9/out/x9-rev252f-wifi/boot.img (x9_wifi_fake=1 x9bl
  sweetiolog=wifid,configd), allowlist 252, no restaging. Expect: wifid in sampler + IOK wifid lines +
  wifid's X9 WIFI REQ (power/scan); Settings Wi-Fi toggle enabled, fake networks listed.

## 27.txt (rev252d-wifi) analysed; rev252e-wifi diagnostic built (2026-11-04)

- 实测 27.txt: LK DT copy works (160 lines, cut at pcie@16910000 by the 8 KiB limit). Wi-Fi EMI from
  LK, all no-map: consys_wifi_emi 0xe1000000/12 MiB, consys_emi 0xe1c00000/10 MiB, wifi-misc pool
  0xe2600000/1 MiB, wifi-reserve (WFDMA) pool 0xe2700000/25 MiB. connv3 co-clock 0, radio-off-mode 0,
  drdi-detect 1, oplus,pcb_map <8 1>.
- en0 up at 5.556 s; 7.788 SIOCGIFDEVMTU (unhandled), 7.793 IO80211Interface user client type 0
  refused, 7.795 REQ type 12 (card caps) get -> 0, 7.798 type 1 (SSID) get -> ENOENT; nothing after.
  USER: Settings Wi-Fi grey. No wifid crash.
- SOURCE (20H392 RE on the Mac): wifid matches {IOPropertyMatch:{IO80211InterfaceRole:Infrastructure}}
  (objc dict literal 0x100238c38; the "IO80211SkywalkInterface" text is only in a log string) ->
  ours matches. IO80211.framework Apple80211BindToInterface: __Apple80211FindService (by
  IOInterfaceName) -> Apple80211BindToInterfaceWithService -> IOServiceOpen fails -> falls back to
  Apple80211BindToInterfaceWithIOCTL (0x1d3ed54f4) -> RawGet type 12 len 13 = our REQ #1. So the
  bind succeeds; the stall is after it. Unknown which process bound (no names logged).
- Commit 73a185b: process names on REQ/ioctl/user-client logs, SIOCGIFDEVMTU answered, wifid added to
  the X9 sampler, LK DT copy 16 KiB without no-map lines. Built x9/out/x9-rev252e-wifi/boot.img with
  x9_wifi_fake=1 x9bl sweetiolog=wifid,configd (allowlist 252, no restaging). Next: symbolicate
  wifid's sampled threads (wifid binary: /tmp/os20H392/usr/sbin/wifid on the Mac).

## Wi-Fi track A (fake radio) + B0 (LK DT copy) built, not device-tested (2026-11-04)

- Commits 948e1f1 + 5a9cd4e. Built x9/out/x9-rev252d-wifi/boot.img (boot line + x9_wifi_fake=1,
  allowlist 252 -> no mailbox restaging), 9,453,568 B, sha256 796e11b6...; host test
  x9/tools/test_shim_wifi_dump.py OK.
- A: iokit/Kernel/X9WifiNet.inc (ported from UniXNU-sheng WifiNet.cpp @516a780, BSD-2-Clause-Patent,
  attributed) included by SweetX9NetworkStack.cpp. x9_wifi_fake=1: configd's first IONetworkStack
  open -> thread call -> ifnet en0 (02:58:39:57:46:01, NX_NOAUTO, bpf), IO80211Controller
  "X9WLANController" + IO80211Interface "X9WLAN" under the platform expert, SIOCGA80211/SIOCSA80211
  answered, KEV class 6 events; fake radio = "X9-FAKE-OPEN" 2437 MHz open + "X9-FAKE-WPA2" 5180 MHz
  RSN/PSK, scan done after 800 ms, join after 300 ms (WPA2 needs >= 8-char key), TX swallowed.
  Naming requests accepted once en0 exists. Logs: `X9 WIFI REQ: n type t set/get len val -> err`
  (first 300 + first of each type), `X9 WIFI: ...`, `X9 WIFI STATS` every 10 s.
- B0: shim/fdt_wifi_dump.c -> ADT /chosen/x9-lk-wifi-dt (8 KiB, mkadt.py) -> kernel prints
  `X9 LK DT: R <reserved child> reg=/size=/no-map` and `X9 LK DT: N <path> <key>=<value>` for
  pcie@169*/phy@169*/connv3*/wifi*/connsys-emi* nodes at IONetworkStack start (also in builds
  without x9_wifi_fake). Never fatal; shim prints its result only on the (unlogged) shim console.
- Expected on device: `X9 LK DT:` block early (~0.1 s); after configd opens IONetworkStack:
  `X9 WIFI: en0 is up (FAKE radio)`, then `X9 WIFI REQ` lines if wifid talks to en0 (the key A4
  question); Settings > Wi-Fi shows the two fake networks if the request layouts match iOS 16.
  If no REQ lines appear: wifid not started / not finding the interface -> next step investigate.

## Brightness DONE (default-on); Wi-Fi plan written (2026-11-04)

- USER-verified rev252b-bl: slider + fade work, no stall. Commit cad84a1 publishes AppleARMBacklight
  by default on X9 (sweetbl=0 off; x9bl = sampler only). Built x9/out/x9-rev252c/boot.img (no tokens,
  allowlist 252 -> no restaging), sha256 a0d22420..., not yet device-run.
- Wi-Fi plan: docs/network/wifi-plan.md. Key new facts: Wi-Fi on pcie0 @0x16910000 SPI 369, PHY
  0x16900000/0x16920000, MEM window 0x50000000/128 MiB, no iommus on pcie nodes (SMMU likely not
  needed), connv3 node + pins, Wi-Fi EMI allocated at boot (emi-addr 0, MPU via ATF) -> must reuse
  LK's region. iOS side templated by UniXNU WifiNet.cpp (IO80211Controller/Interface stand-ins,
  ifnet en0, SIOCG/SA80211, KEV class 6) over nw_abi.h station API. Next: track A with a fake radio
  + read-only dump of LK FDT reserved-memory/connv3/pcie0/wifi nodes.

## rev252b-bl: IODisplayParameters fix for the backlight-service stall (2026-11-04)

- Root cause (SOURCE CONFIRMED, CoreBrightness 20H392): the display setup reads IODisplayParameters
  from the backlight service (IORegistryEntryCreateCFProperty, no recursion) and on NULL jumps to
  the failure exit (0x1af0d87b0 -> 0x1af0ddb18): no CB display, no libEDR server
  (CBEDRServerStartServer/AddMainDisplay), no brightness ever sent. QuartzCore meanwhile waits on
  libEDR every vsync (25.txt t5). UniXNU sheng recorded the identical failure (int-236) in
  package/AppleARMBacklight.cpp @516a780: CB needs backlight-control + IOObjectConformsTo
  "AppleARMBacklight" (ours is that class) + IODisplayParameters{brightness{min,max,value}}, and
  drives it with setProperties {brightness, FadePeriod 16.16 s}. Their page /zh/porting/display
  has no EDR detail; acceptance there = min/mid/max readback + visible, fade completes, timeout
  counter stable; controller writes and system UI verified separately.
- Commit 677a50d: AppleARMBacklight publishes IODisplayParameters {min 0, max 3515, value = DBV},
  setProperties takes brightness (+FadePeriod <= 5 s) -> x9BacklightRequest (never waits, latest
  wins, fade stepped every 16 ms on fBlCall, value republished); IOMFBBrightnessLevel stops moving
  the panel once brightness arrived. Log: `X9 BL STATS: requests= level= fade_ms= sent= fail=`.
- Built: x9/out/x9-rev252b-bl/boot.img (kernel+boot only, allowlist still 252 -> the staged
  X9GeneralMetal252 mailbox payload is reused, NO restaging), boot line + x9bl (service +
  sampler). boot.img sha256 6a2dba33..., 9,433,088 B. Not device-tested.
- Expect: no stall (swap_id like 24.txt), `SWEET BL: backlight set brightness/FadePeriod`,
  `X9 BL STATS`, slider moves the panel. If it still stalls, the X9 SAMPLE stacks show where.

## rev252-x9bl (25.txt): backlight-service stall root-caused to CoreAnimation's libEDR client (2026-11-04)

- 实测: rev252-x9bl boots (install errno=0, X9GeneralMetal252), LK banner blanked with NO token
  (`X9 LK overlay ... queued (clear)` / `zeroed in 4896 us`) -> default-on switches work.
  Stall reproduced: swap_id 0,0,2,4,5,6,8,...,24 at 10 s steps (~2 swaps / 10 s). USER: still laggy.
- Sampler (6 samples, 40-115 s), symbolicated against .local/x9/work/dsc-20H392 (slide 0x3f884000,
  backboardd mh 0x100d1c000; tooling: per-image __TEXT range table + `atos -o <dylib> -l <vmaddr>`).
  ALL backboardd threads are WAITING (run=3), none running -> not a CPU/GPU-cost problem.
  Persistent in samples 2,3,4,6:
  `t5 CA render: vsync_callback -> Server::render_for_time -> IOMFBServer::set_enabled ->
   IOMFBServer::register_libedr_callbacks -> CA::EDRClient::client() -> pthread_mutex_lock (wait)`;
  `t4 CA server_thread: render_display_context_list / set_next_update -> mutex wait` (behind t5).
- QuartzCore RE (20H392): IOMFBServer::edr_client() exists only if display flags (halfword at
  display+0x251) bit 0x200 is set; the ONLY writer is `CA::WindowServer::Server::set_sdr_nits(float)`.
  So once CoreBrightness pushes SDR nits to CA (CBBrightnessProxyCA path, enabled by having a
  backlight service), CA creates a libEDR client every vsync. EDRClient::client(): lock, mark
  creating, unlock, call libEDR (EDRCreateClient...WithCallback); the completion block takes the lock
  and calls a libEDR getter (blr via 0x1dcdf8d98) WHILE HOLDING it -> render thread waits behind it.
  No backboardd thread shows the libEDR server side.
- CoreBrightness side: libEDR server = CBEDRServerStartServer (0x1af095398, called from 0x1af0964ec
  and 0x1af0e0ff8) + CBEDRServerAddMainDisplay(display->[0x3088]) at 0x1af0dd848, gated by
  __DisplayCheckEDRSupport (0x1af0e0e2c): false only for DeviceClassNumber 6/7 or
  (display[0x129]&1 && display[0x3088]==0) -> logs "Not supporting LibEDR, uses CBEDR instead".
  CB os_log is not in our console, so which branch ran is NOT observed.
- Red herrings: t1 dispatch_group_wait = BKButtonHapticsController waiting for "_BRController ready"
  (no haptics HW); t14 = PLClientLogger sync XPC to PowerLog stuck in every sample (powerlogd
  state unknown; not on the render path).
- 推断 / next: either CB never registers an EDR display, or CA's client asks for a different display
  ID than CB registers. Next RE: libEDR EDRCreateClientForDisplay* rendezvous (server lookup, display
  id, timeout) vs EDRServerAddMainDisplay; check com.apple.libEDR naming. Possible levers: make the
  rendezvous succeed (official), or keep CB off the CA SDR-nits path (ignore-IOMFB sets CB
  display[0x88]=0 - meaning unverified).

## Boot line freed: X9 feature switches default-on in the kernel (2026-11-04)

- Commit 2bf1891. `sweet_x9_arg(name, def)` (osfmk/arm64/sweet_early.c): absent -> def on X9,
  0 on other boards; `name=N` overrides, so `name=0` turns any of them off.
  Default-on now: x9_statusbar_provider(=2) x9_pmic_keys x9_gpu_power_probe x9_csf_prepare
  x9_csf_boot x9_csf_queue_probe x9_csf_indirect x9_cs_compute x9_network_stack x9_touch_lbp
  x9_skip_setup x9_watchdog_prefs sweetfb_stayon x9_migration_seed x9_usb_log x9_cryptex_aliases
  x9_ufs_hs x9_init_cache x9_bootstrap_dirs x9_gpu_coherent. x9_ram_8g defaults to the packaged
  segmented span (pmap; vm_resident follows pmap via x9_ram_is_segmented()); boot-xnu.py now only
  rejects an explicit x9_ram_8g=0. LK orange-state blank default-on (x9lo=0 keeps the banner,
  x9lo=2 / x9ob observe). x9_gpu_dependencies was parsed nowhere -> dropped.
- Kept in boot.json: x9_metal_render=1 (X9GeneralMetalFactory.mm reads kern.bootargs for that
  literal token) and all upstream/userspace args. boot.json boot line 602 -> 215 bytes.
- BUILT on the Mac (commit c638056 + allowlist 28030e5): `x9/out/x9-rev252/boot.img` (default) and
  `x9/out/x9-rev252-x9bl/boot.img` (boot line + ` x9bl`), same kernel sha256 d35c24d9..., both
  9,433,088 B; plugin-package X9GeneralMetal252/X9PanVK252 copied into both dirs (one mailbox
  payload). First link failed: `_sweet_x9_arg` undefined because sweet_early.o is PREBUILT
  (x9/kernel/README: six objects have no compile command, editing their sources does nothing) ->
  helper now in pexpert/arm/pe_serial.c.
- Mac build env that worked: `build.py all` (plugins + kernel) with system python3 -I, then the boot
  step separately with `X9_OBJCOPY/X9_NM=/opt/homebrew/opt/llvm@22/bin/llvm-{objcopy,nm}` and the
  interpreter `.local/build-venv/bin/python3 -I x9/build/build.py boot --kernel ... --name ...`
  (system python lacks lz4). Mac checkout sits at c638056 with the generated allowlist modified;
  its content is byte-identical to 28030e5 (hash 5b6e63eb), so a pull only needs that file reset.
- Device test order: x9-rev252 first (expect 24.txt behaviour with no orange banner and no token),
  then x9-rev252-x9bl (purple/stall reproduced on purpose; X9 SAMPLE lines at 40 s + 6 x 15 s).
- Older note kept for history: the first build attempt was blocked by Mac ssh. Expected device result for the default
  build: identical to 24.txt (banner blanked without any token). Build plan:
  `build.py all --revision 252 --name x9-rev252`, then
  `build.py boot --kernel <x9-rev252 kernel> --name x9-rev252-x9bl --boot-arg x9bl` and copy
  x9-rev252/plugin-package into x9-rev252-x9bl so both share one mailbox payload. Stage with
  mailbox/update.py before fastboot boot.

## Brightness: DCS path verified on device; slider route = CoreBrightness RE (2026-11-04)

- 实测 rev251 (24.txt): `[40.794] X9 BL: DBV 1433 (0x599) r=1 retries=0 sent=1 fail=0
  cmdq0=002c3909 con=00008001`, `readback 0x52 di=22 dbv=0598, 0x0A di=21 mode=9c`; swap_id
  176 -> 403 -> 567 after the write; USER: no problem. So the LP 0x51 write between frames,
  with CMDQ restore, works and the panel reports the DBV back.
- RE material (Mac, gitignored): dyld cache extracted with Xcode dsc_extractor.bundle to
  `.local/x9/work/dsc-20H392/` (tool .local/x9/work/dsc-tool/ex.c; SystemOS.dmg mounted ro at
  /tmp/sysos20H392). iPhone X ADT: `.local/x9/work/adt-d22/` (DeviceTree.d22ap.im4p, LZFSE
  payload; decode with `compression_tool -decode -a lzfse`). Analysis scripts used this session
  (macho.py/xref.py/ann.py/adtdump.py) were in the session scratchpad only.
- SOURCE CONFIRMED (CoreBrightness 20H392 disassembly, display setup ~0x1af0d8600-0x1af0da740):
  reads from the backlight service (IORegistryEntryCreateCFProperty, options 3 = recursive
  parents): IODisplayParameters/brightness{value,min,max}, new-backlight-architecture,
  backlight-calibration-parameters{current-for-max/mid-backlight, hardware-max-current-limit,
  lMax/Mid/MinProduct, lMax/Mid/MinPanel, milliAmps2NitsScaleFactor} (absent -> 5/140/400 nits
  then slider curve), digital-dimming-min-nits (default = hw min), user-accessible-max-nits
  (default = hw max), backlight-marketing-table. DigitalDimmingSupported = hwMin > digMin,
  ExtrabrightEDRSupported = hwMax > userMax -> BOTH OFF with our minimal service. `max-nits` is
  never read (our invention). Setter: __DisplaySetBLDriverProperty("brightness-nits") ->
  SetIOMFBBrightness -> SetWiggledDigitalDimmingScalar; 23.txt never reached it.
- Real iPhone X /device-tree/backlight: DBVMin 0x8b, DBVMax 0x7ff, max-nit-value 750,
  backlight-update-policy 4, use-AAB-architecture, use-AAP, use-Nits2DACTable, LmaxProduct...,
  backlight-marketing-table (404 B), no digital-dimming key.
- 23.txt new backboardd lines only with the service: NightModeControl / SunriseSunsetProvider /
  "Night mode init". Stall = ~1 swap per 5 s, GPU idle. Cause still unknown (digital dimming
  ruled out); hypothesis: a CA display colour/EDR transform composited off the GPU path.
- Commit d2fba88: bare boot token `x9bl` (5 bytes, use INSTEAD of x9lo - banner returns) publishes
  AppleARMBacklight and starts the X9 thread sampler (40 s, then 6 x 15 s; "X9 SAMPLE: name[pid]
  slide= mh= ... bt"). NOT BUILT (Mac ssh dropped). Next: build rev252 with --boot-arg x9bl,
  stage mailbox, boot, symbolicate backboardd threads against dsc-20H392.

## rev250-bl device result: purple + near-frozen UI from publishing AppleARMBacklight; service off again (2026-11-04)

- 实测 (23.txt, 4977 lines to 100 s): `X9 BL: DCS 0x51 via inherited LK DSI0` and
  `AppleARMBacklight published` at 0.18 s; payload staged fine (`install result errno=0`,
  `active plugin=X9GeneralMetal250`). backboardd `backlight-control` -> AppleARMBacklight
  (8.216 s), SpringBoard too. USER: purple screen, very laggy.
- **No `X9 BL: DBV` and no `SWEET BL` line at all**: CoreBrightness never set brightness-nits or
  IOMFBBrightnessLevel, so not a single DCS 0x51 was sent. The DSI change is not implicated.
- Damage, measured: GUI heartbeat swap_id=13 at 100.2 s (15.txt: 985 at 80 s); cadence copies
  0-1 per 2 s window; GPU GOV util_avg 0-1%, commits=27 by 99 s; backboardd allocates extra
  full-screen BGRA surfaces (ids 56,58,59,63,65,66 at 20.6-35.0 s); new abort loops
  AegirPoster x28 / GradientPosterExtension x16 (sig 6). IOMFB selector set identical to 15.txt.
- 推断: this is R3 / topaz patch 0110 again - with a backlight service CoreBrightness/CA turns
  on digital dimming (offscreen colour-filter pass); purple = that path mis-rendered on our stack.
  Not proven which property triggers it.
- Fix (next commit): X9 does not publish AppleARMBacklight unless sweetbl=1, and
  IOMFBBrightnessLevel on AppleCLCD is only set when a service is published (= rev249 userspace
  view). Transport check instead: first 10 s GUI heartbeat with swap_id>=60 enters fBlCall ->
  one `X9 BL: DBV 1433` + readback line. Expected: rev249 picture/speed back, plus a visible
  brightness step to 35% if the LP write lands mid-session.
- Slider: needs a different route than AppleARMBacklight (open question: what makes
  CoreBrightness skip digital dimming - e.g. AAB/"DisplayDigitalDimming"-style properties - not yet
  researched).

## Active: brightness (DCS 0x51) code committed, NOT BUILT, NOT TESTED (2026-11-04)

- Commit 9b63862 on feat/G1Ultra (origin/main's docs fast-forwarded in first, incl.
  docs/brightness/porting-difficulty.md). Mac ssh (localhost:2222) was refused all session, so
  there is no kernel build yet; only the new host test ran:
  `python3 -I x9/tools/test_kernel_lk_dcs.py` -> OK.
- Source confirmed this session (/home/ubuntu/android_kernel_modules_and_devicetree_oppo_mt6993):
  oplus_display_dsi_primary.c:448 oplus_panel_set_backlight_cmdq writes `level>>8, level&0xff`
  into the `51 00 00` template, clamps to hw_max (4094) and DROPS level 1 -> pass-through is no
  longer an inference. mtk_dsi.c:8576 rewrites CMDQ0=0x002c3909 + SIZE=1 before every cmd-mode
  frame trigger; mtk_dsi_host_send_cmd (11354) = wait idle, CMDQ, START 0->1->0, wait idle.
- Code: x9_lk_console.h `x9_lk_dcs_long()` (under x9_lk_submit_lock, refuses while BUSY,
  LP long write, restores CMDQ0/1/2, +0x44, +0xa0, acks only CMD_DONE). AppleCLCD: fX9Bl set when
  the LK path is active; AppleARMBacklight published on X9 by default (sweetbl=0 disables);
  nits 0..500 -> DBV 2..3515 linear; sent from fBlCall with up to 100 x 1 ms busy retries.
- Expected log: `X9 BL: DCS 0x51 via inherited LK DSI0 ...` at start; backboardd matching
  `backlight-control` -> found; `SWEET BL: backlight set brightness-nits`; `X9 BL: DBV n r=1`
  and for the first two sends `X9 BL: readback 0x52 di=.. dbv=.... 0x0A ... mode=9c`.
- Risks to watch: (R3) CoreBrightness digital dimming may slow composition now that a service
  exists - compare copies/s and swap_id against 22.txt; (R1) frames stop or garbage after the
  first DBV line = CMDQ restore not enough -> rollback to x9-rev249-lo.
- Next: on the Mac `git pull`, `build.py all --revision 250 --name x9-rev250-bl --boot-arg x9lo`
  (same env workaround as rev248/249), then BEFORE fastboot boot stage the payload:
  `X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/x9-rev250-bl`.
  Test: slider min/mid/max visibly changes brightness, panel stays alive.

## Brightness (backlight) porting assessed: difficulty 中低, lowest open subsystem; doc landed (2026-11-03)

- Read-only assessment written to `docs/brightness/porting-difficulty.md`; no production code
  changed, no build, no device touched.
- 实测 from source: X9 brightness is **not** the I2C/GENI path the unixnu display page
  demonstrates. The board DT (`oppo-source/modules/vendor/oplus/kernel/devicetree/oplus-display/`,
  panel `ac287`, board `mt6993-zhujiang-24203`) sets it with a MIPI DCS `0x51` long write,
  range 1..4094, and `display_backlight_on()` at x9/shim/display.c:497 already lit the glass
  in LP mode (pan36, recorded in that file's own comment; pan35 HS failed with SoT errors).
- Remaining work is small: swap the QCOM `dsiSend()` register layout for the MT6993 LP long
  write, change the nits->DBV constant 0x7ff -> 0xffe, and un-gate `AppleARMBacklight` (it is
  currently blocked on `fDsi`, which only the QCOM branch maps). Top risk is injecting a DCS
  write into the per-frame fresh-TE/DSI_START loop, and CoreBrightness digital dimming.
- Not verified (stopped per the user's no-disk-scan rule): the `mtk_drm_setbacklight()` body is
  not in the two oplus dirs, so "level is passed straight through" stays 推断, listed as open.

## GPU 优化空间评估 vs UniXNU `porting/gpu` + 厂商 Avalon DDK (2026-11-03, docs only)

- 取证 (read-only; nothing built, nothing flashed): report at
  [docs/graphics/gpu-optimization-headroom.md](docs/graphics/gpu-optimization-headroom.md). Inputs =
  the UniXNU Adreno740 page, our kernel/userspace tree, and
  `~/HelioDarwin/oppo-source/modules/vendor/mediatek/kernel_modules/gpu` — MT6993 builds
  `gpu_mali/mali_avalon/a16w` = Arm DDK **r54p1-11eac0**, CSF (`fragment/mt6993.fragment:5`).
- 结论: 那篇页面的映射微优化对我们只部分适用 — (a) WC 分配和 (d) 的"只填无效叶子"我们已有，
  (b)/(c) 没有对应物因为我们 SMMU 是 bypass、Mali AS 没有 per-page TLBI。真正的余量在
  实测 12,011 µs/job 的三块: `wait` 51% (同步 submit-and-wait + 每轮 poll 重发缓存命令)、
  `pre_cache+post_cache` 38% (59.2+33.5 MB/job 软件缓存维护)、`start+retire` 7%
  (每次提交 2× `FLUSH_CACHES` + `install_as/remove_as` 4 次 `idle_as` 轮询)。
- 结构性根因 (取证): `map()/unmap()` 在 `vm->attached` 时直接返回 busy
  (`X9RenderPageTable.cpp:96,104`)，而 `attached` 只有在提交末尾 detach 时才为假 —
  所以映射无法与执行重叠。厂商在同一 GPU 上用的是 per-AS、VA-range、`FLUSH_PT|FLUSH_MEM`
  且 arch>11 只做 TLB-invalidate、且**不在提交路径** (`mali_kbase_mmu_hw_direct.c:543-608`)。
- 厂商另外证明了两件事: MT6993 唯一启用 `CONFIG_MALI_MTK_COHERENT_DMA_BUF=y` +
  `of_dma_is_coherent` → 他们几乎不做 CPU 缓存维护 (池页只在分配时同步一次);
  以及 `CSG_SUSPEND_TIMEOUT` 从 3100M 砍到 150M cycles。我们的 `x9_gpu_coherent=1`
  是**未验证就跳过** CIVAC，同一个东西既是最大性能项也是最大正确性风险 (CACHE-3)。
- 完全缺失 (取证): pipeline 预热 (`__primelist`/prime 线程/pipeline pack 一个都没有，
  `NativeMetalCompiler.inc:6-18` 是调用者 join 的同步编译)、client BO 池与 spare 老化、
  batch map (PanVK 只接受 IMMEDIATE，一个 op 一次 IOKit call)、O(1) handle 索引
  (5 处 O(4096) 扫描/提交)、transient/memoryless 附件、damage present、任何线程优先级。
- 度量缺口 (必须先补): 全仓库没有 FPS/stutter 指标、GPU 时间戳未与 CPU 阶段链对齐、
  `wait` 内队列 vs GPU 执行仍无法拆分; `first-frame-experiment-plan.md` 仍是设计稿。
- Next (待用户选): 1) 先落 P3 度量并刷机取一轮日志; 2) P0-B 一致性实测探针 (读回 L2/SMMU
  snoop + CPU/GPU 竞态回读), 有证据后再扩大 coherent 路径; 3) P0-C 常驻 AS + 范围失效原型。
  No production code changed by this assessment; CC platter bug remains unresolved per spec §3.2D.

## MT6993 DSP/audio port feasibility assessed against the UniXNU `porting/power` page (2026-11-03, docs only)

- 源码取证 (read-only pass over `~/HelioDarwin/oppo-source`, nothing built, nothing flashed):
  the assessment landed as [docs/audio/porting-difficulty.md](docs/audio/porting-difficulty.md).
  Verdict 中高, staged: A = AFE `0x1a110000` (GIC_SPI 384) → ADDA → MTKAIF v4 → MT6681 over I2C,
  no DSP at all; B = ADSP offload; C = capture; D = SCP voice (not scheduled).
- The UniXNU page is Qualcomm-only (GLINK/SMEM, AudioReach/GPR, q6apm, WCD938x/SoundWire).
  取证: `adsp|audio_ipi|audio_dsp|audio_scp` give **0** hits for `audio_reach|gpr|apm|tplg|topology`
  and **0** for `request_firmware|iova|iommu|dma_map_`. What we CAN reuse is their OS-side shape
  only: `hal/ShengAudioPlugIn.c` (703 lines, a plain `AudioServerPlugIn` bundle),
  `package/ShengAudioService.cpp` (143 lines, `IOService` + `clientMemoryForType` rings +
  `NativeSandboxGrantProcess = audiomxd,mediaserverd`) and the host-clock 24 MHz fix.
  MTK needs no graph objects: opening a path is 4 messages (`OPEN/HWPARAM/PREPARE/START`),
  81 message IDs, 16-byte header + 96-byte payload (`MAX_IPI_MSG_BUF_SIZE 112`), 10 ms queue timeout.
- Two blockers are cheap to settle and are NOT resolved:
  1. this build reads AFE IRQ status through `arm_smccc_smc(SMC_SC_NBL_VHM_REQ, 0x90000000, hwirq…)`
     (`mt6993-afe-pcm.c:7049`) instead of `regmap_read` - test a direct MMIO read from XNU first;
  2. nothing in-tree loads the ADSP image ("Reserved memory allocated from lk") - probe whether the
     carve-out still carries firmware when we boot our shim instead of Android.
- 推断 (not measured): A ≈ 3-6 周, B +4-8 周, C ≈ 2-3 周. No audio behaviour is DEVICE VERIFIED anywhere
  in this document; the sweet `VirtualAudio.plugin → HAL.disabled` move only masked the symptom.
- Next: capture the live board DTB (the attached dts is SoC-level; audio-json/board wiring is absent),
  then the A0/A1 read-only probe list. No production code changed by this assessment.

## Wi-Fi assessment landed (read-only, 2026-11-03) — MT6653 over PCIe, no code written, P0 pending

- New handoff doc: `docs/network/wifi-porting-assessment.md` (assessment + difficulty table +
  P0 collection checklist). Nothing implemented; nothing here authorises a device action.
- 实测 from the user-supplied `oppo-source`: the Wi-Fi function is **MT6653 / CONNAC 3.0 with
  PCIe HIF** (`wlan/core/gen4m/Kbuild.6993_6653:5-13`, module
  `wlan_drv_gen4m_6993_6653`), not the gen4-mt79xx path. RC base used by the driver is
  `ioremap(0x16910000, 0x2000)` (`chips/mt6653/mt6653.c:3564`), and the host glue is MediaTek
  gen3 (`android_kernel_oppo_mt6993/drivers/pci/controller/pcie-mediatek-gen3.c`:
  RST_CTRL `0x148`, LINK_STATUS `0x154`, LTSSM `0x150`, INT_ENABLE `0x180`, trans-table
  `0x800`, MSI sets `0xc00+i*0x10`), 1 MSI vector, 36-bit DMA mask, EP BAR ≥2 MiB.
- 实测: MTK ships an OS abstraction — `os/linux/include/gl_os.h` (2217 lines) plus existing
  `os/uefi`, `os/windows`, `os/none` implementations, so a `os/darwin/` KAL is the sanctioned
  route. Coupling is at the edge: `net_device` appears 1287 times with only 24 outside `os/`,
  and the core makes 3 direct cfg80211 calls and 0 `netif_*` calls.
- 实测 contrast with `https://unixnu.com/zh/porting/network`: that page's asset is a Qualcomm
  prebuilt OS-neutral library reached via `nw_abi.h`/`lx_platform`/`nw_board`. We have no such
  library, so its step 3 becomes "compile MTK's core against our own KAL". Its 4-step shape
  and its gotchas still transfer — notably MAC-zero: `CFG_TC1_FEATURE` is off here, so the MAC
  comes from the NVRAM cal file, i.e. a missing cal reproduces their zero-MAC case.
- 实测 XNU side today: BSD stack is already linked (`-DINET` 11 sites, `-DNETWORKING`,
  `-DSOCKETS`, `-DNECP`, `-DPRIVATE_WIFI_ONLY=1`, `if.o`/`dlil.o`/`bpf.o`/`ndrv.o` in
  link-order) but there is **no INET6**, **no PCIe host**, **no conninfra code**, **no
  MediaTek coprocessor mailbox**, and `SweetX9NetworkStack.cpp:8-10` admits there is no
  IONetworkInterface producer. The old `ue0` USB-ECM win belongs to SM7150, not X9.
- Difficulty verdict: hardest subsystem so far. P1 conninfra power = L/highest correctness
  risk; P2 PCIe RC in XNU = XL (new substrate, needs `mkadt.py` nodes + built-in class since
  there is no kext loader); P3 `os/darwin` + firmware download = L but mechanical; P4 own
  ifnet + WPA2 = M (real IP, no system UI); P5 wifid/Apple80211 ABI = XL research. Do not make
  P5 a prerequisite for P4.
- Open question that most affects the estimate: **the supplied tree has no MT6993 device
  tree**, so PCIe/conninfra GIC SPI numbers, `memory-region` and whether the connectivity RC
  sits behind the SMMU are unknown. P0 item 2 (`cat /sys/firmware/fdt` on stock Android, then
  `dtc -I dtb -O dts` locally — `/opt/homebrew/bin/dtc` exists) closes it. All P0 steps are
  read-only and stage under the gitignored `x9/.local/x9-wifi-probe/`.

## rev249-lo device-verified: banner blanked AND picture restored once the 249 payload was staged into the mailbox (2026-11-03)

- 实测 (22.txt, 7234 lines to 102 s, user flashed after running the staging command):
  `[4.510] X9 UPDATE: payload installed and verified from reserved mailbox` →
  `[4.512] X9 UPDATE: install result errno=0` →
  `[4.512] X9 RENDER EXTERNAL: active plugin=X9GeneralMetal249 from verified allowlist`.
  Discovery is armed again, which is exactly what 20/21.txt lacked.
- Banner: `[10.647] X9 LK overlay: i=3 addr=fd6a8000 1256x2760 pitch=5024 13877248 B queued (clear)`,
  `before ... ink=29 opaque_black=231 transparent=65276 bbox=0..397,1380..1433 checksum=56b1fc1b in 3589 us`
  → `after ... ink=0 transparent=65536 checksum=00000000` → `13877248 B zeroed in 3125 us`,
  `1 rejected layer buffer(s) queued (x9lo=1)`. Same content signature as the obs run, so the
  A/B conclusion stands: the blank is harmless and costs ~3.1 ms once per boot.
- Picture (this is the part 14/20/21.txt never had): slot0 `index=0 pa=fc910000 sample=256
  nonblack=253` at 36.722 s then `nonblack=256` held for every later probe; heartbeats climb
  `swap_id=0 → 6 → 66 → ... → 1183`; `X9 GPU GOV` commits climb to 270 with DVFS reaching
  1300000 kHz; 382 MESA WSI lines (17.txt had 337). **0** `CRASH SpringBoard` events, versus
  10 in 14.txt and 6/5 in 20/21.txt. User confirms the screen shows the desktop with no
  orange-state banner.
- 推断 (the operational rule this fixes): `build.py all --revision N` renames the plugin bundles
  and regenerates the kernel allowlist, but the bytes reach the device only through
  `python3 x9/mailbox/update.py <candidate-dir>`. Any new revision must ship as boot.img **and**
  a mailbox staging step, or Metal discovery stays `not armed errno=2` and the screen is black
  for reasons unrelated to the display code.
- Not covered by this run: `locationd` still crash-loops (pre-existing), GPU coherency reports
  `capable=0`, and the optional band-only blank (rows 1380..1433, ~1 MiB instead of 13.9 MiB)
  remains unimplemented - the whole-layer blank is proven harmless so there is no reason to
  rush it.

## rev249 A/B verdict: blanking is innocent; the black screen is unstaged Metal plugins (mailbox still holds the rev247 payload) (2026-11-03)

- 实测 (20.txt = x9-rev249-lo, 21.txt = x9-rev249-obs, both user-reported black): the two runs
  behave identically, so writing the LK layer is not what removes the picture.
  `X9 LK overlay before/observe: i=3 pa=fd6a8000 1256x2760 grid=65536 ink=29
  opaque_black=231 transparent=65276 other_alpha=0 bbox=0..397,1380..1433
  checksum=56b1fc1b` appears in both with the same checksum; after the lo blank everything is
  transparent (`ink=0 transparent=65536 checksum=00000000`, 13877248 B zeroed in 4394 us).
  So that stale layer is 99.6% transparent and carries ink only in a 54-row band - it never
  carried the desktop. Sampling cost measured ~21 ms (20884/20865 us lo, 25150 us obs).
- Real cause, measured in the same two logs:
  `[0.986] X9 RENDER EXTERNAL: missing/mismatched path=/System/Library/Extensions/
  X9GeneralMetal249.bundle/Info.plist errno=2; discovery disabled` →
  `[0.991] X9 UPDATE: install result errno=2` → `[0.997] X9 RENDER EXTERNAL: not armed errno=2`.
  14.txt (rev248-lo) shows the same trio for `X9GeneralMetal248`. 17.txt (rev247, the run that
  produced a picture) instead shows `[2.536] X9 UPDATE: payload installed and verified from
  reserved mailbox`, `[2.539] install result errno=0`, `[3.382] active plugin=X9GeneralMetal247
  from verified allowlist` and then `X9 RENDER DISCOVERY ... MetalPluginName=X9GeneralMetal247`
  for backboardd/mediaserverd/SpringBoard.
- Why: `x9/build/build.py all --revision N` renames the bundles to X9GeneralMetalN/X9PanVKN and
  regenerates the kernel's built-in allowlist, but the payload is not in boot.img - it lives in
  the reserved mailbox and is staged by `python3 x9/mailbox/update.py <candidate-dir>` (see
  x9/mailbox/README.md). I bumped 247 -> 248 -> 249 and never staged the new payload, so the
  kernel looks for bundles that are neither installed on the root volume nor present in the
  mailbox, leaves discovery disabled, and every Metal client dies on the null path
  (SpringBoard sig 11 pc=0x0, 6 loops in 20.txt / 5 in 21.txt; swap_id stuck at 6/4; only 5/3
  copies ever; 0 MESA lines vs 337 in 17.txt). This is my error, not a device regression.
- Prepared (local only, no device touched): copied `x9/out/x9-rev249/plugin-package` into both
  `x9-rev249-lo/` and `x9-rev249-obs/` so each directory is a candidate `update.py` accepts
  (10 files, payload_end 0x30fc000, identity af930ccd1d1573d9..., all package hashes match).
- Next, both are the user's actions:
  1. cheap confirmation without any device write: flash `x9/out/x9-rev247/boot.img` - its
     allowlist matches what the mailbox/root already has, so the picture should return with the
     orange banner still present.
  2. real fix: with Android running, `X9_SERIAL=<serial> python3 x9/mailbox/update.py
     x9/out/x9-rev249-lo` (root preflight + mailbox slices only, no boot partition), then flash
     `x9/out/x9-rev249-lo/boot.img`. Expect `install result errno=0`,
     `active plugin=X9GeneralMetal249`, frames again and no banner.
- Banner follow-up (only after the picture returns): a band-only blank (rows ~1380..1433) would
  touch ~1 MiB instead of 13.9 MiB; the current whole-layer blank is proven harmless.

## rev248-lo: banner gone, black screen is SpringBoard's null-call crash loop; rev249 adds read-only x9ob (2026-11-03)

- rev248-lo 实测 (14.txt, 2689 lines to 88.6 s, no panic, no kmem_alloc this time):
  `[10.362] X9 LK overlay: i=3 addr=fd6a8000 1256x2760 pitch=5024 13877248 B queued`,
  `before sampled=256 nonblack=1 checksum=fffffc03` → `after clear ... nonblack=0
  checksum=00000000 zero wrote in 3258 us`, and the user confirms the orange-state banner
  disappeared. So the target address and the mechanism are both right.
- That cleared buffer is NOT the picture: only 1 of 256 grid samples had any RGB bits set, i.e.
  banner ink over a black/transparent background. A buffer carrying a picture samples ~256/256
  (17.txt slot0 at 34.573 s: `nonblack=255`, then 256 and held there to 80 s).
- The picture is missing in 14.txt because XNU produced no frames at all: cadence shows
  copies=1 then copies=2 and then copies=0 forever (last non-zero window 12.208 s), heartbeat
  `swap_id` stuck at 5 from 20 s to 80 s, slot0 (0xfc910000) samples stay nonblack<=2 the whole
  run. 17.txt (rev247 default, x9lo off) in the same wall clock reaches copies 3..50 per 2 s
  window, swap_id 889 and a full-content slot0.
- Reason, measured: SpringBoard segfaults repeatedly - 10 events, `sig 11 code 0x100000001`,
  `pc 0x0 lr 0x1d7c5c58c`, each ~4 ms after `IOK SpringBoard: matching1 ... IOAcceleratorES ->
  IOAcceleratorES`, every ~7 s from 23.068 s to 84.713 s (PIDs 26,257,274,286,296,...). In
  17.txt SpringBoard (pid 26) survives to 66 s, never calls matching1 for the accelerator
  (only the iterator `matching` -> 1 found at 29.128 s) and goes on to create IOSurfaces.
  14.txt also has 0 `MESA: error` lines where 17.txt has 337: the GPU/WSI path was never
  reached because the client kept dying. locationd/WeatherIntents/... crash loops are much
  heavier in 14.txt too (104 vs 88 locationd, 44 vs 4 IntentsExtension).
- 推断, not proven by one run: the overlay write is not what kills SpringBoard (it lands 12 s
  earlier on a different path). rev249 makes this testable with a single binary:
  `x9lo` (bare, which XNU parses as -1, or `=1`) blanks the queued layer; `x9ob` (bare) and
  `x9lo=2` sample it and write nothing. The 607-byte boot-arg budget fits " x9lo"/" x9ob"
  exactly (607 used) but not " x9lo=2", hence the separate short token.
- rev249 code: pe_serial.c keeps the token level (`x9_lk_overlay_mode`) and now also exports
  each queued layer's pitch and height through `x9_lk_overlay()`; AppleCLCD got
  `sweet_x9_report_lk_overlay()`, a 256x256 grid that classifies every sample as ink /
  opaque_black (0xff000000) / transparent (alpha 0) / other_alpha, reports the ink bounding
  box, the rolling checksum and the measured read time, printed before and after the store
  loop plus the zeroing duration. Descriptor direction changed to kIODirectionInOut, which
  removes rev248-lo's `warning: creating writable mapping from IOMemoryDescriptor
  (kIODirectionOut)` and its Backtrace line; the mapping route itself (withPhysicalAddress +
  kIOMapWriteCombineCache) is unchanged and demonstrably safe (the whole write stayed inside
  LK's reservation: 0xfd6a8000 + 0xD3C000 = 0xfe3e4000 < 0xfff70000).
- Artifacts: x9/out/x9-rev249-lo and x9/out/x9-rev249-obs, same kernel, boot.img 9,428,992 B
  each, plugins X9GeneralMetal249/X9PanVK249. Kernel compile emitted no new warnings (only the
  pre-existing kern_exec.c -Wdeclaration-after-statement list). Build still needs the env
  workaround: X9_OBJCOPY/X9_NM from /opt/homebrew/opt/llvm@22/bin and boot-xnu.py run without
  -I with X9_CC=<out>/build-tools/x9-clang, X9_LD=lld@22, SDKROOT/DEVELOPER_DIR from Xcode.
- Next: flash both; whichever one shows SpringBoard surviving is the display baseline. If the
  crash loop appears in both, the overlay write is exonerated and the black screen is the
  IOAcceleratorES open path (null function pointer at pc=0 right after matching1) - the same
  family as the older "no Apple logo" reports, and the ink bounding box from x9ob then tells us
  whether a targeted banner-only blank would have been enough anyway.

## rev247-lo panic root-caused; rev248 moves the x9lo clear into AppleCLCD (2026-11-03)

- rev247-lo (x9lo) did panic, and the reason is now measured, not guessed. The 8 MiB disk ring
  (.local/x9/work/x9-disk-log/x9-rev247lo-20261103-200934.log, header CRC valid, seq 11) stops
  mid-line at [10.236] "X9 LK engine: i=7". Android pstore went further:
  .local/x9/work/x9-disk-log/pstore-20261103-201049/console-ramoops-0.raw (heavily bit-flipped,
  fields not reconstructed) reaches engine i=12 at [10.275], then
  [10.281] `kmem_alloc(0xfffffe000ec22668, 14139392, 0x100020): failed with 3 @vm_kern.c:178` and
  [10.287] `panic: (cpu 7 caller 0xfffffe0007bf8ca0)`, then [10.291] the shim prints "log ready"
  again = reset. So x9_lk_clear_overlays() never printed a single "X9 LK overlay" line.
- Cause (measured): 14139392 = round_page(5120*2760) at 16 KiB pages, i.e. the exact
  X9_LK_MAP_FB()/ml_io_map_wcomb() size. flags 0x100020 = KMA_NOFAIL|KMA_PAGEABLE|KMA_PERMANENT,
  which in xnu-8796.141.3 is io_map()'s permanent 8 MiB io_submap (same class as the earlier
  ml_io_map-per-submit panic in this file). A frame-sized request can never be satisfied there,
  and KMA_NOFAIL turns that into a panic instead of a NULL. pexpert has no IOMemoryDescriptor, so
  the clear cannot live in pe_serial.c at 10 s; the identical 14 MiB map only works early because
  fbcon_probe() does it before that budget is committed.
- Orange-state banner root cause (实测, from the clean 17.txt engine dump, not pstore): EXDMA
  i=3 pa=32970000 en=00000011 layer=00000001 size=1256x2760 fmt=00000002 **pitch=5024**
  addr=fd6a8000 accepted=0, while the accepted boot layer i=2 is the same geometry at pitch=5120.
  5024 = 1256*4 (no padding), so x9_lk_layer()'s `pitch == 5120` term is what rejects it; every
  other predicate passes and 0xfd6a8000 is exactly one 14,254,080-byte slot inside the reservation.
  buffers=1 in both rev247 runs: LK keeps blending that stale layer over every frame we push and
  nothing ever writes it. rev248 keeps it rejected (mirroring our 5120-stride frame into a
  5024-stride buffer would shear the image) and only zeroes it.
- rev248: pe_serial.c x9_lk_clear_overlays() -> x9_lk_scan_overlays(), which discovers and queues
  (address/rounded bytes/engine, max 4) and maps nothing; new x9_lk_overlay(index,...) accessor.
  AppleCLCD got sweet_x9_clear_lk_overlays() (SweetFramebuffer.cpp), called right after both
  x9_lk_gui_probe() sites, using IOMemoryDescriptor::withPhysicalAddress + map
  (kIOMapWriteCombineCache) - the same route the boot scanout surface already uses, NULL-checkable
  instead of panicking - then a store loop with sweet_watchdog_pet() every 256 KiB. It samples a
  256-point strided grid before and after ("X9 LK overlay: i=.. before/after ... nonblack=.. checksum=..
  zero wrote in .. us"), so the run proves whether the banner pixels really live at 0xfd6a8000.
  Engine dump now also prints f4c (the only register x9_lk_layer() tests that was not printed).
- Why zeroing is safe rather than screen-blackening: our content is visible everywhere except the
  banner strip, so that layer's blend honours per-pixel alpha; an opaque layer would already have
  hidden the console. 推断, to be confirmed by the after-clear screenshot.
- 18.txt "not smoother" is a separate, unchanged problem: rev247's WC host-import skip matched
  nothing (imports=832 wc=0) and per-job cache traffic is the same as rev246 (65.9 vs 67.5 MB
  cleaned, 31.0 vs 36.1 MB invalidated). The 17-vs-15 delta is GPU completion wait
  (median 6.82 -> 10.93 ms), with the display path identical; still needs an x9iw control run.
- Build env on this Mac broke for the documented `python3 -I x9/build/build.py`: PATH had no
  llvm-objcopy and -I hides ~/Library/Python/3.15/lib/python/site-packages, where lz4 now lives,
  so packaging failed after the kernel linked. Workaround used here (kernel itself unchanged):
  export X9_OBJCOPY/X9_NM=/opt/homebrew/opt/llvm@22/bin/... and run the same boot-xnu.py command
  line taken from `--dry-run` without `-I`. Worth fixing properly (install lz4 into the brew
  python's own site, or stop using -I in build.py) before the next agent burns time on it.
- Artifacts: x9/out/x9-rev248 (default) and x9/out/x9-rev248-lo (same kernel, extra boot-arg
  x9lo), boot.img 9,428,992 B each, plugins X9GeneralMetal248/X9PanVK248, allowlist regenerated.
  Kernel binary contains 8 "X9 LK overlay" strings. Device test is the user's; expect boot to
  survive past 10.3 s with x9lo this time.
- Next if the banner survives a successful clear: the buffer was the wrong target, and the 13
  EXDMA nodes are exhausted (dump shows only i=2/i=3 enabled), so the remaining suspects are the
  BL1/OUTPROC/DLO/PQ/RSZ blocks, which this kernel has never read.

## rev246 results; rev247: WC host-import skip, LK layer dump / x9lo (2026-11-03)

- 15.txt default rev246: status bar OK (DynamicSplitMedium errno=0). per job pre 4.0 + wait 6.6 + post 2.3
  = 12.9 ms, 26.1 jobs/s, display 8.9 copies/s, gpu_defer 5.2/s.
- 16.txt x9qd: USER: severe screen jitter, touch uncontrollable. per job 7.3 ms but display 2.2
  copies/s. Reading: with a userspace submit thread, CoreAnimation's swap can reach the kernel
  before the GPU job does; the lock-free present then copies incomplete/old surfaces. Async submit
  needs per-surface present fences (design 3.4) first. x9qd stays opt-in, not to be used.
- Cache cost source (15.txt cache-reach): import_pre_avg 82,440,691 B cleaned and import_post_avg
  47,055,047 B invalidated PER JOB = every import of backboardd (LIVE: imported=202/93MiB), "kernel
  mapping bounds, not dirty tracking"; WC BOs already skip ~480 MB/job.
- rev247 kernel: X9RenderImportHost whose every physical page has pmap WIMG == VM_WIMG_WCOMB
  (SweetSurface IOSurfaces map WC) is marked write_combined -> no per-job CIVAC, like WC BOs.
  Boot token x9iw disables. Log "X9 RENDER IMPORT WC: imports wc wc_bytes cached_bytes".
  x9_render_memory_reassert_wc now skips imports instead of failing.
- LK orange-state banner (user: always shown at the top, "another layer not refreshed"): kernel
  x9_lk_layer accepts only the 1256x2760/5120-pitch layer (buffers=1). rev247 logs every scanned EXDMA
  engine once ("X9 LK engine: i pa en layer size fmt pitch addr accepted"); boot token x9lo zeroes the
  buffer of other enabled layers inside the LK reservation (memory only, no register writes,
  "X9 LK overlay: ... cleared"). LK RE material on the Mac: ~/Downloads/03-安全研究/LK-C17,
  .local/x9/work/lk-preserve-dali (references/legacy-notes/wiki/MT6993-Display.md).

## Active: rev246 built, awaiting device test (2026-11-03)

- x9/out/x9-rev246 (default) and x9/out/x9-rev246-qd (boot-arg x9qd), same kernel and plugin package
  (X9GeneralMetal246/X9PanVK246), boot.img 9,424,896 B each; allowlist fa4ecfb. Kernel contains
  "DynamicSplitMedium" and "X9 DISPLAY: present"; GeneralMetal contains "x9qd".
- Default image: InPlace host textures (no mosaic), latency governor, padding, Dynamic Island status
  bar (boot.json x9_statusbar_provider=2 -> _UIStatusBarVisualProvider_DynamicSplitMedium; =3
  DynamicSplit; =1 LegacyPhone). Provider class names read from the 20H392 SystemOS cryptex
  dyld_shared_cache_arm64.* (mounted read-only, detached). Expect "X9 STATUSBAR: UIKit provider ...".
- x9qd image: plus Mesa threaded submit and lock-free display present.
- DRM question (user): iOS has no DRM/KMS; the equivalent is direct scanout through the MT6993
  display controller (OVL/RDMA/DSI) instead of the CPU copy + LK DSI push. Copy cost is 0.6-1.7 ms
  per frame; the measured limits are synchronous submits (~2.3 x 13-17 ms per frame) and the display
  lock coupling. Decide after the x9qd result.
- Mac stashes: rev242/243/245/246 allowlists kept (all but 242/245 identical to commits).

## rev245 results + x9qd lock-free display A/B (2026-11-03)

- 13.txt (default) vs 14.txt (x9q1, "vk_queue mode=threaded" in backboardd+SpringBoard; threaded mode
  worked, no init failure): jobs/s 23.3 -> 27.3, per job 13.1 -> 14.2 ms, display copies/s 3.7 -> 5.1,
  gpu_defer/s 2.9 -> 10.1. USER: async "very laggy". Reading: the display tick TryLocks
  metal_service_lock (sweet_x9_render_display_try_enter), held for every whole synchronous job; with
  a submit thread the GPU lock is almost always taken, so presents are deferred.
- Governor (latency thresholds): during swipes util 34-39% -> index 33 (1300 MHz); idle at 858 MHz.
- Steady-state present is a CPU copy: GPU desktop staging stops after 3 frames
  (SweetX9DesktopWorker.inc desktop_frame_presented>=3); 14.txt has 0 "X9 DESKTOP FRAME".
  The presented surface is protected by fShowHold/fPendHold IOSurface holds, not by the GPU lock.
- rev246 change: token x9qd = frontend threaded submit + kernel display tick skips
  sweet_x9_render_display_try_enter (log "X9 DISPLAY: present without the GPU owner lock (x9qd)");
  GPU staging requires fX9OwnerHeld, so it is skipped without the lock. Default unchanged.
- Status bar: kern_exec.c sweet_x9_seed_uikit_statusbar writes UIStatusBarVisualProviderClassName =
  _UIStatusBarVisualProvider_LegacyPhone (x9_statusbar_provider=1). User wants the iPhone 14 Pro
  (Dynamic Island) style; exact iOS 16 class name must be read from 20H392 UIKitCore on the Mac first.

## Async submission step 1: x9q1 = Mesa threaded vk_queue submit (source only, 2026-11-03)

- Finding: the blocking point is userspace PanVK vkQueueSubmit -> X9PanVKSync.c submit_sync_common ->
  x9_pan_kmod_submit (synchronous kernel call); the frontend's commit does not wait afterwards
  (completion handlers fire from fence signals). Mesa's vk_queue has a threaded submit mode
  (MESA_VK_ENABLE_SUBMIT_THREAD) that moves driver_submit to a per-queue thread; our sync provider
  implements DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT. NOT verified against the Mac Mesa tree whether
  the X9 sync type advertises WAIT_PENDING (threaded mode requires it) - check the first log.
- Frontend: boot token x9q1 -> setenv MESA_VK_ENABLE_SUBMIT_THREAD=1 before nmb_device_open; log
  "X9 SUBMIT: vk_queue mode=...". This is a userspace stand-in for decision 1 (kernel worker); the
  kernel queue, display lock decoupling and exact resource sets (decisions 3/4) follow if it helps.
- Next build (rev245): host-texture default InPlace, latency governor, x9q1 token.

## DECISION 2026-11-03: user accepted all five submission-decoupling defaults

docs/graphics/submission-decoupling-design.md section 10: (1) kernel worker-thread queue,
(2) fence = (group, submission_id), (3) exact resource sets tracked in PanVK, (4) bound 64 ranges per
submission with reachability fallback, (5) first candidate behaviour-neutral: x9_submit_async=0 and
ExactSet off by default, A/B by boot token.

## Active: mosaic root cause = 353ae1c InPlaceWrites; performance breakdown; rev245 (2026-11-03)

- rev244 x9h0 (/tmp/12.txt, "X9 HOST TEXTURES: policy=InPlace" in backboardd+SpringBoard): USER: NO
  mosaic, but very laggy. => swipe mosaic is caused by host-page textures rendering directly into
  imported IOSurface pages (353ae1c, HostPageTexturePolicy::InPlaceWrites). Exact missing cache
  handoff for in-place render targets not yet located.
- Performance (job-weighted averages from X9 RENDER timing, both runs ~22-23 jobs/s):
  11.txt InPlaceWrites: pre_cache 3.9 + wait 7.9 + post_cache 2.2 = 14.0 ms/job, display 10.4 copies/s
  12.txt InPlace:       pre_cache 4.2 + wait 10.0 + post_cache 2.5 = 16.6 ms/job, display 8.9 copies/s
  (gpu_defer 3-5/s). ~2.3 synchronous jobs per frame, nothing overlaps -> ~9-10 frames/s on screen.
  Shadows cost ~+19%/job; CPU cache maintenance of imports ~6.7 ms/job (~40%); GPU wait 10 ms is
  per-job latency (governor util 12-25% is misleading for a serialized pipeline).
- UniXNU porting/gpu (zh, read 2026-11-03): WC pageable BOs without CPU maintenance, batch maps with no
  TLB invalidation at submit, completion thread instead of blocking callers, spare BO/image reuse,
  shader prewarm; "per-call overhead decides smoothness". No numbers; Adreno/Turnip, not Mali.
- rev245 changes: (1) default host-page policy is the safe InPlace; token x9h1 re-enables
  InPlaceWrites (env X9_HOST_TEXTURE_INPLACE_WRITES); x9h0 removed. (2) governor latency-oriented:
  up 4 indices at util >= 20% per 50 ms window, down 2 below 5% (thermal limits unchanged).
  NOT built (tunnel down).
- Biggest remaining levers (needs user decisions in docs/graphics/submission-decoupling-design.md
  section 10): asynchronous kernel submission queue (removes the 10 ms wait from the Metal call path,
  lets CA encode/display overlap); exact per-submission resource sets to cut the 6.7 ms of import
  cache maintenance; then fix in-place render-target coherence to drop the shadows.

## Swipe mosaic regression bisect: rev244 tokens x9h0 / x9r1 / x9r3 (2026-11-03)

- rev243 result (/tmp/10.txt x9c0, /tmp/11.txt control): mosaic in BOTH -> CRC hypothesis
  FALSIFIED. Governor works: GPUEB temperature is in degrees C (37..43), util_avg only 12-25%
  during use, so it stays at the 858 MHz floor (0 and 3 commits) - not GPU-bound.
- USER: mosaic started with commits after 2026-11-01 14:15 +0800 (06:15 UTC); before that only
  the missing triangle. Candidates that change behaviour (commit dates mix +0000/+0800):
  efef710 WC flag on user views; 0efed90 x9_gpu_clean + batched range sync (more cleaning);
  40ed1cf reassert WC (off by default since d15598c -> excluded); 353ae1c (21:32 +0800,
  mailbox214) host-page textures InPlaceWrites: GPU renders directly into imported IOSurface
  pages with a linear layout instead of a shadow (strongest suspect: imported memory relies on
  the kernel cache plan with write_begin/write_end ranges and 256-byte atoms).
- rev244 tokens (one per boot; same kernel/plugins):
  x9h0  frontend sets X9_HOST_TEXTURE_SHADOW -> bridge keeps the runtime default InPlace policy
        (log "X9 HOST TEXTURES: policy=...").
  x9r1  kernel: user views of WC BOs without kIOMapWriteCombineCache (pre-efef710).
  x9r3  kernel: x9_gpu_clean -> clean_dcache64 (pre-0efed90 API; batching kept).

## Active: rev243 built (CRC A/B + governor + padding), awaiting device test (2026-11-03)

- Swipe mosaic hypothesis: PanVK transaction elimination (CRC). Mesa's own comment in
  panvk_should_checksum: images the host can modify must not use CRC; X9 imports IOSurfaces
  written outside PanVK. Patch 12 (12-panvk-crc-ab.patch, generated against the live tree,
  dry-run clean): boot token x9c0 -> no CRC on any image (as PANVK_DEBUG=no_crc); otherwise
  logs the first 32 CRC images per process ("X9 CRC IMAGE: proc WxH vkfmt usage flags tiling").
- Built on the Mac (login shell): x9/out/x9-rev243-crc (boot-arg x9c0) and x9/out/x9-rev243-ctl
  (same kernel, no extra token), boot.img 9,424,896 B each; same plugin-package
  (X9GeneralMetal243/X9PanVK243). Verified strings: X9PanVK "X9 CRC", GeneralMetal "X9 PAD",
  kernel "X9 GPU GOV" (governor first build, untested on device). Allowlist rev243 = b4221f0.
- Mac stashes kept: stash@{1} rev242 allowlist from the user build, stash@{0} rev243 (identical
  to b4221f0). G1UltraMetal main c3b239c has patch 12.
- Test: mailbox stage x9/out/x9-rev243-crc once; boot -crc, swipe; boot -ctl, swipe. Mosaic gone
  only with x9c0 -> CRC tracking misses writes; then find which writes (X9 CRC IMAGE lines) and
  invalidate CRC there instead of disabling it. Also check "X9 GPU GOV" lines (temperature units).

## G1UltraMetal repo split (2026-11-03)

- Pushed git@github.com:tosasitill/G1UltraMetal.git main 75439c9 (fresh history, from
  ios_plj110 4abe1e3): metal/ (= x9/metal), build/third_party (all Mesa + metal2vulkan patches),
  build/scripts (13 plugin-chain scripts; build-render-cs and collect/import-build-inputs stay
  here), build/prepare-local.py (kernel inputs dropped from its required list),
  build/build.py (plugins only), build/mailbox-manifest.json, docs/graphics,
  docs/partial-vertex-group.md, README, MIT LICENSE. Paths rebased (parents[3] -> [2],
  x9/metal -> metal); package-plugins writes the allowlist header to <out>/ or --header.
- NOT build-verified (tunnel down). Expected setup on the Mac: clone to ~/G1UltraMetal,
  `ln -s ~/ios_plj110/.local .local`, copy build/local.example.json -> build/local.json.
- ios_plj110 x9/metal is still the live copy; until G1UltraMetal builds identically, keep
  changes in both or switch ios_plj110 to a submodule.

## GPU load governor (source only, 2026-11-03)

- User chose a simple load governor. sweet_x9_gpu_dependencies.c: the old one-shot commit
  (fastest OPP <= 900 MHz = index 44, 858 MHz) is now the FLOOR; gov_thread samples every 50 ms
  GPU busy ns (sweet_x9_gpu_busy_ns, accumulated in SweetX9RenderEngine.inc from doorbell to
  fence of each synchronous submit), util > 80% -> 4 OPP indices faster, < 30% -> 2 slower,
  clamped to [ceiling, floor]. Ceiling = fastest OPP <= x9_gpu_khz (default now 1300000).
  Thermal: GPUEB shared `temperature` >= 80 -> floor, >= 90 -> slowest OPP; values outside
  1..150 (unknown units) never allow more than the floor. Only runs while the resident render
  session holds power (render_active_refs >= 0). x9_gpu_gov=0 or x9_gpu_khz=0 -> fixed floor.
- All GPUEB exchanges now serialized by x9_gpu_ipi_lock; governor commits are quiet (no TX/RX
  lines). Logs: "X9 GPU DVFS: shared temperature=..." once, "X9 GPU GOV: start ..." and every
  2 s "X9 GPU GOV: util_avg temp idx gpu commits limiter ceiling floor".
- NOT compiled (tunnel down). Check the temperature units from the first log before trusting
  the thermal thresholds.

## Active: rev242 padding FIXES the CC split (user confirmed); next: swipe mosaic, GPU clock, repo split (2026-11-03)

- rev242 (X9GeneralMetal242, log /tmp/9.txt): "X9 PAD: partial vertex group padding on",
  first pad 6->12. USER: the Control Center diagonal split is gone; static frames correct.
- REMAINING (user): mosaic while swiping, disappears on release and when static. No capture of it yet.
  HYPOTHESIS (unproven): Mali transaction elimination (tile CRC). PanVK v14 enables
  crc_read/crc_write on CRC render targets (csf panvk_vX_cmd_draw.c ~1583-1731, CRC header
  "valid" flag, cmd_invalidate_crc). Writes that are not CRC-tracked render passes (blits/copies,
  host/IOSurface writes, other processes) without CRC invalidation would leave stale tiles
  during motion. Test: a boot token that disables CRC (needs the Mac Mesa tree; tunnel down).
- GPU clock (observed 9.txt): "X9 GPU DVFS: GPU OPPs 390000..1716000 kHz ... cap 900000 kHz ->
  index 44 (858000 kHz)", stack 572 MHz, volt 61500/58000, ceiling=0 floor=56. Cap is
  x9_gpu_khz (default 900000 in sweet_x9_gpu_dependencies.c gpu_select_performance, one fixed
  dual-commit, no governor, no thermal input; battery temperature is a fixed estimate).
  boot-args are 602 B; x9vt makes 607 of 608, so a larger cap needs a code default change.
- PanVK upstream: the bug is not yet localized inside PanVK (count passed unchanged); a report
  needs a minimal non-CA repro (non-indexed triangle list, count 9, last vertex distinct).

## Active: rev241 confirms partial final vertex group; rev242 frontend padding workaround (2026-11-03)

- rev241 log /tmp/8.txt (backboardd 128 MiB heap, X9GeneralMetal241). Picks: n1 6-vertex ffid66
  (290,494,259x258), n2 CC card 9-vertex ffid24 (80,228,516x516).
- OBSERVED: padded variants 13/14/15 (12/24/48 vertices) give ONE hash each over all 10
  passes: n1 ae1f81b5... (3857 uncovered), n2 b3fb843a... (257 uncovered = exactly the 1-column
  + 1-row box border, i.e. geometrically exact). Unpadded v0/v7: n2 mostly 234 (23 samples
  drawn OUTSIDE the card -> a vertex overshoots) and v0 r3 910; n1 mostly nothing written
  (4225), v0 r3 4224. Padding to 12 == 24 == 48.
- CONCLUSION (replay level): non-indexed triangle lists whose count is not a multiple of 4 get a
  wrong position for the last vertex (deterministically wrong or intermittently wrong);
  padding with copies of the last vertex makes the draw exact. PanVK passes INDEX_COUNT =
  vertex count unchanged (csf launch_draw), so masking of the extra lanes in the last group is
  hardware/firmware or shared lowering; BIR vs Kraid (rev228) made no difference.
- rev242 change (NativeBridgeMetalFrontend.mm): x9PadDraw, default ON, boot-arg x9p0 disables.
  Non-indexed MTLPrimitiveTypeTriangle, vertexStart 0, 1 instance, count%12 != 0, count<=96,
  all attributes per-vertex in CPU-readable buffers/inline bytes, padded size <= 4 KiB: copy
  count vertices + copies of the last vertex up to a multiple of 12 into inline vertex bytes,
  draw, restore the app's bindings. Replay passes (traceFrameReplaying) are NOT padded, so the
  replay keeps showing the raw bug. Encoder now mirrors vertex bindings (_padBuf/_padOff/
  _padBytes in NBEncoder); pipelines keep padLayout. Logs "X9 PAD: ... on" once and
  "X9 PAD: padded=N skipped=M" at 1 and every 16384. NOT compiled (tunnel down).
- Test: build rev242 with --boot-arg x9vt (capture + 128 MiB backboardd heap + padding).
  Visual A/B without capture: --boot-arg x9p0 (padding off). If the CC split is gone with
  padding, keep the workaround and then hunt the root cause (indexed draws are not covered).

## Active: rev240 = heap ruled out; last vertex of 9-vertex draws misplaced; rev241 padding probe (2026-11-03)

- rev240 log /tmp/7.txt (3.1 MB): "X9 TILER HEAP: proc=backboardd ... linked=64 bytes=134217728"
  (others 32 MiB). USER: flicker NOT reduced vs 32 MiB.
- Replay n2 = CC card, 9 vertices, box 659,234,510x509, ffid24 (same card as rev237):
  v0=v3=v4=v6=BR, v1=v5=v8=TL, v2=TR, v7=BL -> tri0 (BR,TL,TR), tri1 (BR,BR,TL) zero-area,
  tri2 (BR,BL,TL). Good passes hash c3c16d6b50ee546b (485 uncovered); bad: v0 r2 and v11 r2
  (4263, d6f837e2...), v12 r0 (1172). So non-determinism persists with a 128 MiB heap:
  HEAP SIZE IS NOT THE CAUSE (neither of the replay error nor of the visible flicker).
- MAP of bad passes: tri2 is drawn with its LAST vertex v8 (TL) displaced by a varying amount
  (v0 r2: to about (736,483); v12 r0: ~2 grid columns right). Other vertices correct.
- Pattern across runs: 12-vertex card (rev238/239) and 24-vertex corners always exact; 9- and
  6-vertex draws flaky. HYPOTHESIS: the last valid vertex in a partially filled final IDVS
  vertex group gets a bad position (PanVK / HW / masked-lane store in the position shader).
- rev241 change (X9FrameCapture.inc): variants 13/14/15 = owned copy padded with copies of the
  last vertex to 12/24/48 vertices; 7 = owned unpadded copy as control. Schedule 24 passes:
  0 x6, 7 x4, 13 x6, 14 x4, 15 x4 (readback factor 24). Heap setting unchanged from rev240.
- Reading: 0/7 fail sometimes but 13..15 never -> partial final vertex group confirmed; a
  frontend workaround (pad non-indexed draws to a group multiple) becomes the next step,
  then locate the exact PanVK/HW cause. 13..15 also fail -> not group padding.

## USB log tool restored (2026-11-03)

- The user's log capture tool usb_log.py (formerly
  /Users/tosasitill/HelioDarwin-x9-lk-preserve/work/x9-usb-live/usb_log.py) was in x9/legacy,
  removed in 81069e5; the old workspace is deleted. It is now maintained at
  x9/tools/usb-live/ (usb_log.py, tests, log_protocol.h, kernel-prototype/build-kernel.py).
  SHA-256 b2b0509a... identical to .local/x9/work/x9-usb-live/usb_log.py (still present).
  Usage: python3 x9/tools/usb-live/usb_log.py --output /tmp/N.txt
- USER (rev240, backboardd 128 MiB): flicker NOT reduced vs 32 MiB.

## Active: rev239 result (32 MiB heap real) + rev240 backboardd 128 MiB (2026-11-03)

- rev239 log /tmp/6.txt (3.7 MB, X9GeneralMetal239). "X9 TILER HEAP ... requested=5 linked=16
  bytes=33554432" for backboardd, mediaserverd, SpringBoard, chronod, SBRendererService:
  the heap enlargement is in effect for the first time. 5 x 32 MiB allocations logged.
- USER: flicker seems reduced (not gone).
- Replays deterministic: n2 = CC card (12 vertices, 83,230,513x514, ffid24) identical hash
  d1530d05c867e713 for all 13 whole-draw passes, full coverage; n1 (27 vertices, 578x578,
  ffid65) identical 0473e8590a4f2730; its v12 (RT1 cleared to 1) is consistently different
  (2097e3f1daece45f) -> ffid65 reads attachment 1, deterministically.
- Memory: backboardd peak X9 RENDER MEM used=544,391,168 with 32 MiB heaps; kernel client
  budget defaults to 1024 MiB (x9_gpu_client_mb), per-BO limit 128 MiB (inclusive).
- rev240 change (X9PanVKQueueBridge.inc): with x9tb/x9vt, backboardd links 64 chunks
  (128 MiB = upstream Mesa max_chunks = per-BO limit), all other processes 16 (32 MiB).
- Build: build.py all --revision 240 --name x9-rev240-heap128 --boot-arg x9vt (Mac, login
  shell). Expect "X9 TILER HEAP: proc=backboardd ... linked=64 bytes=134217728".
- Reading: flicker gone at 128 MiB -> capacity (no OOM servicing) is the mechanism; then a
  real fix is TILER_OOM servicing / incremental rendering, or a fixed upstream-sized heap.
  Unchanged vs 32 MiB -> residual flicker has another cause (chunk recycling or non-heap).

## Active: rev239 = first build with the repo PanVK bridge (heap A/B now real) (2026-11-03)

- Toolchain: user ran `sudo xcode-select -s /Users/tosasitill/Downloads/Xcode.app/Contents/Developer`
  (revert: `sudo xcode-select -s /Library/Developer/CommandLineTools`). Fresh PanVK configure
  needed two script fixes: ed9fa79 (cross rust_args -isysroot iPhoneOS SDK) and 77c2055
  (native rust_args -isysroot Xcode MacOSX.sdk); rustc links through clang without
  -isysroot and clang otherwise picked CommandLineTools MacOSX27.0.sdk.
- Fresh build-panvk-native-ios: 868/868 OK; build.ninja and meson-private have 0 references
  to HelioDarwin-x9-lk-preserve; dylib contains "X9 TILER HEAP", "x9vt", "X9 PASS SERIALIZE".
  Old build dir kept at .local/x9/work/x9-metal-render/build-panvk-native-ios.bak-20261103.
  c04cc5d: 00-mesa-panvk-x9.patch text and collect-build-inputs.py default now point at
  ~/ios_plj110/.local (live tree already did).
- Built on the Mac: x9/out/x9-rev239-heap (build.py all --revision 239 --boot-arg x9vt).
  boot.img 9,420,800 B; plugin-package X9GeneralMetal239 + X9PanVK239 (X9PanVK has
  1 x "X9 TILER HEAP"); kernel allowlist rev 239 committed. The boot step needs the login
  shell (Homebrew llvm-objcopy in PATH, anaconda python3 with lz4); /usr/bin/python3 lacks lz4.
- This is also the first X9PanVK with the repo X9PanVKQueueBridge.inc (only diff vs the old
  copy: the x9tb/x9vt heap switch + log). X9PanVKMemory.c (4096 mappings) was already built.
- Test: mailbox/update.py x9/out/x9-rev239-heap, boot it. Expect "X9 TILER HEAP: ...
  linked=16 bytes=33554432" lines. Then report whether CC split/flicker changes. A control
  boot (x9vf, 10 MiB) can be packed from the same kernel with build.py boot.
- DELETED 2026-11-03 with user confirmation: /Users/tosasitill/HelioDarwin-x9-lk-preserve (13 GB),
  build-panvk-native-ios.bak-20261103, x9-failed-fresh-configure-20261103. Prior audit
  (x9/out/old-workspace-audit/result.json on the Mac): 181,667 files byte-identical under
  .local/x9; no-copy/different files were build caches, old scripts (git history ca5b8d5),
  the old MaliMetal bridge copy, and Mu-Silicium-X9 .git stores (upstream
  Project-Silicium/Mu-Silicium @ 6c67550). Its 25 uncommitted local changes are identical in
  .local and were also exported to .local/x9/work/legacy-import/Mu-Silicium-X9.local-changes/
  (top-level.patch, sub/Mu_Basecore.patch, base commit, status, untracked list). After deletion
  `ninja -n` in build-panvk-native-ios resolves all inputs.

## Build-path migration attempt (Mac, 2026-11-03)

- SSH to the Mac works (key added by user; repo ~/ios_plj110, fast-forwarded to 81069e5).
- Old workspace diff (MaliMetal/X9/General/panvk vs repo): only X9PanVKMemory.c (repo raises
  native mappings 1024 -> 4096, 2026-11-01 DeviceLost fix) and X9PanVKQueueBridge.inc
  (repo adds x9tb/x9vt heap switch + X9 TILER HEAP log) differ.
- Live mesa-render-source meson.build already lists the six X9 .c files via the repo
  symlink .local/x9/MaliMetal/X9/General -> x9/metal/General, so the .c files (incl. the
  4096 mappings) ARE built from the repo. But build-panvk-native-ios/build.ninja carries
  750 x -I/Users/tosasitill/HelioDarwin-x9-lk-preserve/MaliMetal/X9/General/panvk from the
  OLD cross file baked into meson coredata, so csf/panvk_vX_gpu_queue.c includes the OLD
  X9PanVKQueueBridge.inc. That is why x9tb/x9vt never existed in any binary.
- `meson setup --reconfigure` keeps the stored cross file (verified: -I unchanged, dylib has
  0 x "X9 TILER HEAP").
- Fresh configure (old dir moved aside) FAILS before any compile: rustc sanity link uses
  Xcode 15.0.1 clang/ld against /Library/Developer/CommandLineTools/SDKs/MacOSX27.0.sdk
  ("tapi error: unknown architecture arm64e.x1-macos"); xcode-select -p is
  /Library/Developer/CommandLineTools. Old build dir restored unchanged (build.ninja present);
  failed attempt kept at .local/x9/work/x9-metal-render/x9-failed-fresh-configure-20261103.
- Old workspace is still required by the build dir: -I panvk headers/.inc, and the regenerate
  rule's panvk-native-ios.cross/.native under HelioDarwin-x9-lk-preserve/work/x9-metal-render.
  Do NOT delete it until a fresh configure works.

## Cleanup + build-path finding (2026-11-03)

- Mac check by user (rev238): /Users/tosasitill/HelioDarwin-x9-lk-preserve/MaliMetal/X9/General
  is a REAL directory (not a link to this repo); its panvk/X9PanVKMemory.c and
  X9PanVKQueueBridge.inc differ from x9/metal/General/panvk; csf/ has no X9PanVK* copy;
  the built x9/out/x9-rev238-heap/plugin-package/X9PanVK238.bundle/X9PanVK contains
  0 x "X9 TILER HEAP" and 0 x "x9vt". CONFIRMED: repo edits under x9/metal/General/panvk
  have not been compiled into X9PanVK (x9tb in rev223 and x9vt in rev238 were no-ops).
  Likely mechanism: the six .c files come from 00-mesa-panvk-x9.patch:399-404 (old absolute
  paths) and the configured build-panvk-native-ios dir still carries the old -I path
  (build.py only runs incremental ninja once build.ninja exists). Not yet seen on the Mac.
- User approved: (a) delete the old Mac workspace AFTER the build is migrated to the repo
  and a full PanVK rebuild is verified; (b) delete x9/legacy. Declined: server-side old
  copies (/home/ubuntu/agsbx, old session scratchpads) stay.
- Done (b): git rm x9/legacy (145 files); last tree at ca5b8d5. BUILD.md,
  EXTERNAL-DEPENDENCIES.md and prepare-local.py comment updated.
- Pending (a): needs SSH; the server key is not authorized on the Mac (publickey denied).
  Steps: diff old vs repo panvk sources, point meson files()/libdrm/.rsp and -I at repo
  paths, `build.py plugins --panvk full`, verify strings, then list and delete the old
  workspace with the user's confirmation.

## Active: rev238 x9vt run; CORRECTION: heap enlargement very likely never built in (2026-11-03)

- User flashed only the x9vt image (log /tmp/5.txt, boot-args contain x9vt, plugin 238
  active). User: screen STILL flickers / splits.
- Replays in 5.txt are fully deterministic: n1 (ffid66, 259x258) 13/13 identical hash
  36ef2c39eca31e33 (nothing written), n2 (CC card, 12 vertices, 83,230,513x514, ffid24)
  13/13 identical hash d1530d05c867e713 with full coverage (512 uncovered = border).
- CORRECTION to the rev238 entry below: this determinism can NOT be attributed to the heap.
  Evidence: no "X9 TILER HEAP" line exists in ANY log ever (incl. the rev223 x9tb test),
  although x9_panvk_tiler_create in x9/metal/General/panvk/X9PanVKQueueBridge.inc logs it
  unconditionally; 5.txt still has 10 x "X9 PANVK ALLOC: size=10485760" (= 5 x 2 MiB) and no
  32 MiB allocation. So x9tb/x9vt most likely never changed the heap, and the rev223 "x9tb
  no visible change" result is void too. Different picks (12- vs 9-vertex card) and run
  state may explain rev237 vs rev238; the cause of the replay non-determinism is still OPEN.
- Build-path facts (source, not verified on the Mac):
  - 00-mesa-panvk-x9.patch meson.build lists X9RenderTransport.c and X9PanVK{Memory,Sync,
    TilerHeap,SubmitPlan,Queue}.c by absolute path under the OLD workspace
    /Users/tosasitill/HelioDarwin-x9-lk-preserve/MaliMetal/X9/General/ (also known in
    x9/EXTERNAL-DEPENDENCIES.md:20-21). Repo edits to those .c files are not compiled
    unless that path is a symlink to this repo.
  - csf/panvk_vX_gpu_queue.c does #include "X9PanVKQueueBridge.inc": a quoted include looks
    in the csf/ directory first, so a stale copy there would shadow the repo's file (the -I
    path .local/x9/MaliMetal/X9/General/panvk is a prepare-local.py symlink to the repo).
  - prepare-panvk-native-budget.py pins Mesa max_chunks = initial_chunks = 5 (10 MiB).
- Needed from the Mac before any heap conclusion: whether that old path is a symlink to the
  repo, whether csf/ holds a stale X9PanVKQueueBridge.inc, and whether the built X9PanVK
  binary contains the strings "X9 TILER HEAP" / "x9vt".

## Active: rev237 reproduces the CC split in replay; rev238 heap A/B token x9vt (2026-11-03)

- rev237 log /tmp/4.txt (2.9 MB, X9GeneralMetal237 active). Pick n2 = CC card,
  cb=117, 9 vertices, box 659,232,511x511, ffid24; vertices exactly
  v0=v1=v3=v6=TL, v2=v5=v7=BR, v4=TR, v8=BL; tri1=(TL,TR,BR), tri2=(TL,BR,BL).
  Geometry matches the CPU projection here (no mirror).
- OBSERVED (14 passes in one CB, one encoder each, box-sized fresh targets), in pass order:
  uncovered 765, 490, 459, 255, 255, 255, 255 | 7431, 7250, 8256, 6909, 6741, 6586
  (v0/v11/v12 interleaved; last pass v1 alone = 16384). MAP: early passes full card,
  from pass 8 on the lower-left triangle tri2 (the LAST triangle) is missing. Frozen inputs
  (v11) behave identically. Every box hash differs (also between early full passes).
  This IS the CC diagonal split, reproduced inside one command buffer.
- Pick n1 (cb=112, ffid70, 6 vertices, 424x424): mostly nothing written with a stable hash
  7025d5873cfddd83; sporadic deviations (v11 r1, v12 r1, v0 r4 = 11035).
- v12 (attachment 1 cleared to 1,1,1,1) is no different from v0/v11 -> no evidence ffid24
  depends on attachment 1. RT1 hypothesis not supported for this pipeline.
- LIVE same=0 even for the exact 432-byte vertex range: CA rewrote its ring obj=29 between
  pick and completion (completion took 2.6-5.2 s). Not the cause (frozen v11 identical);
  note it for the real frames.
- No TILER OOM / fault / X9 TILER HEAP line in the log.
- HYPOTHESIS (best fit): pass-order dependence + last triangle lost points to the CSF
  tiler heap (IDVS writes position/varyings into it). X9PanVKTilerHeap.c is a fixed
  pre-linked chain (Mesa's 5 x 2 MiB), no growth, no OOM servicing. Chunk recycling at
  FINISH_FRAGMENT or exhaustion would show exactly "fine for N passes, then the tail of
  the draw is lost".
- rev238 change: boot token x9vt = x9vf capture + 16-chunk (32 MiB) heap (x9vf + x9tb
  do not fit the 608-byte bound). Frontend: out.frame = x9vf || x9vt; PanVK bridge:
  big heap for x9tb || x9vt. Kernel-side x9vf logging is not enabled by x9vt. NOT compiled.
- Test: build all rev238 --boot-arg x9vt, then a second boot.img from the same kernel with
  --boot-arg x9vf (control). One mailbox staging serves both. Reading: with x9vt the
  flip moves later or disappears -> heap capacity/recycling confirmed; unchanged -> not
  heap size (look at chunk recycling / heap context state next).

## Active: rev236 = non-determinism is GPU/driver side; rev237 attachment-1 probe (source only) (2026-11-03)

- rev236 log /tmp/3.txt (2.98 MB, X9GeneralMetal236 active). Picks cb=54: both the same
  6-vertex ffid66 draw, box 290,494,259x258; v0=v1=v5=TR, v2=v3=BR, v4=BL ->
  one real triangle (BR,BL,TR).
- OBSERVED: most passes write nothing (4225/4225). Non-empty: n1 v0 r3 4102, n1 v11 r2 4102,
  n1 v10 r1 3924, n1 v1 3100, n2 v8 4214. Frozen inputs (v11) vary like live (v0).
  => input mutation is NOT the cause; the result varies with identical frozen inputs.
- OBSERVED MAP: the written pixels lie on the TL->BR main diagonal (1 px line, sometimes
  wider), while the CPU-expected hypotenuse is BL->TR. Together with rev235's lower-left
  half, GPU geometry = horizontal mirror of the CPU projection every time. Likely a CPU
  projection mismatch (vfid41 is more than M*pos); systematic, not the variance.
- LIVE same=0 for every binding is uninformative: all bindings live in ring obj=29
  (262144 B) and the 4 KiB windows include space later draws write after the pick.
- HYPOTHESIS (unproven): ffid66 is FBF with attachment_count=2, memoryless_index1=2; the
  output varies at edge pixels, so attachment 1 tile reads (PanVK FBF / ROA for RT1) may be
  unreliable. That would also give a triangle-dependent split in real CC passes.
- rev237 change (X9FrameCapture.inc):
  - schedule: 0 x6, 11 x3, 12 x3, 10, 1. Variant 12 = live whole draw with colour
    attachment 1 cleared to (1,1,1,1).
  - RESULT gains hash= (FNV of the whole box readback): exact repeat comparison.
  - MAP prints only rows containing '#'.
  - LIVE compares only the vertex range (position slot) / first 256 bytes (others).
  - Second pick per process must use a different fragment function.
  - NOT compiled (tunnel down).
- Reading rev237: v0 hashes differ across repeats -> confirmed GPU non-determinism with
  one encoder per pass. v12 differs from v0 consistently -> shader reads RT1 (then test
  PanVK FBF of RT1). A second pipeline that repeats exactly -> non-determinism is
  pipeline-specific (FBF), not general.

## Active: rev235 shows GPU replay output is NOT repeatable; rev236 frozen-input A/B (source only) (2026-11-03)

- rev235 log /tmp/3.txt (1.95 MB, plugin X9GeneralMetal235 active at 3.360 s).
  Picks: cb=47 (log 35.29 s, not a captured frame, so no DRAW records), 2 picks of the same
  9-vertex draw, box 339,529,222x221, ffid66 (fixed_frag_lph_cph, framebuffer_fetch=1,
  texture_function 0x0e, specialized at 35.224 s), vfid41 (texcoord_count0/1=1).
- Vertices (VTX, observed): v0=v2=v5=v6=BR(558.25,747.57), v1=v3=v4=v8=TR(558.25,531.19),
  v7=BL(341.87,747.57). tri0/tri1 zero-area; tri2=(BR,BL,TR) = lower-RIGHT half.
  a1@16 = (-1,-1)/(-1,0)/(0,-1); texcoord1@24 = pixel-like (437.25,436.57) etc.
- OBSERVED: same draw, same CB, separate box-sized targets, different coverage per repeat:
  n1 v0 r0/r1 = 1641/1641, n1 v10 r0/r1 = 2392/1641, n1 v6 2249/3136, n2 v0 1641/3095,
  n2 v8 3136/2832 (box_uncovered of 3136). Covered colour is 00000000.
- OBSERVED (GRID): n1 v0 r0 wrote exactly the lower-LEFT half (TL,BL,BR), not the expected
  lower-right half. TL is no vertex of the data (x of v7, y of v8). n1 v10 r0 wrote a thin
  band along the TL-BR diagonal instead. n2 v0 r1 wrote ~nothing.
- READING: GPU output for identical work is not deterministic, and the written geometry is
  not the CPU-projected triangle. Either live inputs (CA uniform/vertex ring memory, or the
  X9 inline-bytes upload) change between encode and execution, or post-transform positions
  are corrupted in the GPU/driver (PanVK IDVS position output / tiler / kernel). Caveat:
  a mirrored CPU projection was not excluded, but it would not explain the variance.
- rev236 change (X9FrameCapture.inc, x9vf only):
  - at pick, each stage-1/2 buffer binding is snapshotted (offset .. max(4 KiB, stride*
    (start+count)), clipped) and logged: X9 FRAME REPLAY BIND (kind=buffer|bytes, obj,
    offset, len, FNV hash).
  - variant 11 = whole draw with every buffer binding replaced by a fresh buffer of that
    snapshot (offset 0). Schedule: 0,11 alternating x4, then 10 x2, then 1,5,6,8 once
    (14 passes/pick; readback budget factor 14).
  - X9 FRAME REPLAY MAP: 32x32 ASCII coverage map ('#' written, '.' clear kept) per pass.
  - X9 FRAME REPLAY LIVE at completion: re-hash of each live binding vs the pick snapshot.
  - NOT compiled (tunnel down).
- Reading rev236: v11 repeats identical while v0 varies -> live inputs mutate (LIVE same=0
  names the binding). v11 also varies with all inputs frozen -> GPU/driver side; next
  instrument PanVK IDVS position buffer for one draw. MAP shape vs CPU triangle tells
  whether the projection itself is mirrored.
- Test: build rev236, run mailbox/update.py, boot. Picks need not be CC.

## Active: rev234 ran but picked no CC draw; rev235 pick filter (source only) (2026-11-03)

- User re-staged the mailbox; /tmp/3.txt (2.96 MB) shows "active plugin=X9GeneralMetal234"
  at 3.430 s. The earlier errno=2 line at 0.982 s is from before the plugin root was ready.
- Log timestamps are ~32 s behind uptime (FRAME CB f=1 log 32.612 = uptime 64.477 s).
- Replay ran cleanly: f=51 (uptime ~69 s), 2 picks, ffid66, both the same 6-vertex strip,
  box 1151,0,105x834 (vertices off the right edge, clipped). 24 passes, GRID printed, no failure.
  All variants including 10 are ~all magenta. Not CC; no evidence about the triangle.
- The capture windows (uptime 64/80/125 s) held no 9-vertex CC card draws, so CC was not
  open during them.
- rev235 change (X9FrameCapture.inc): a replay pick needs the projected draw fully inside
  the target and at least 200x200. Earlier CC picks (82.8..593.7, 511 px) still qualify.
  NOT compiled (tunnel down).
- Test: build rev235, run mailbox/update.py, boot. Open CC as soon as the desktop appears
  and keep it open and static for ~2 min (picks are the first 2 qualifying draws after
  uptime 45 s).

## Active: rev234 first boot failed: plugin package not staged (2026-11-03)

- Log /tmp/3.txt (2,683 lines, to ~94 s). Kernel is rev234: at 0.844 s
  "X9 RENDER EXTERNAL: missing/mismatched path=/System/Library/Extensions/
  X9GeneralMetal234.bundle/Info.plist errno=2; discovery disabled", then
  "not armed errno=2". No X9 GENERAL METAL / RENDER DISCOVERY lines, so the
  Metal plugin never loaded and the a3517e9 probe code never ran.
- Consequence: SpringBoard sig 11 pc=0 x11 restart loop (first at 24.8 s),
  no desktop. The locationd/Intents/TTS crashes are secondary.
- Cause (observed): boot.img expects the rev234 bundles, but the device mailbox
  still holds an older package (ENOENT on the 234 path). This is a staging step,
  not a compile error.
- Fix: python3 -I x9/mailbox/update.py x9/out/x9-rev234-uv --serial <serial>
  (from Android, needs "Mailbox committed ... read back"), then fastboot boot
  the matching x9/out/x9-rev234-uv/boot.img.

## Active: rev233 replay re-read + UV-gradient probe (source only) (2026-11-03)

- Log /tmp/2.txt on the analysis server (rev233, x9vf, 49,308 lines, to uptime 1020 s).
  Replay picks: ONE CB only (cb=81 = capture f=5, uptime ~92 s), 4 picks =
  draws n=93 (vfid25/ffid24, A8 122x62 mask at slot 3), n=94 (vfid27/ffid35,
  507x507 surface 79 at slot 3), n=95 (24-vertex nine-patch corners, ffid68),
  n=102 (second card at x=660, ffid24). All non-indexed, stride 48,
  a0=Float4@0, a1=Float2@16; 9-vertex order: (BR,TL,BR) zero-area, (TL,TR,BR), (BR,BL,TL).
- OBSERVED: whole-draw replays (variants 0 and 7) cover the "lost"
  triangle (BR,BL,TL) 4/4 for n=93/94 (3/4 for n=102). So in this pipeline set the
  triangle is rasterized. Every 3-vertex replay (1..6, 8, 9) covered 0 of its
  sample points, including the vertically/horizontally mirrored copies.
- OBSERVED harness flaw: rev233 is pre-6c80fa7 (shared target per pick).
  n=93 variant 5 is a zero-area triangle alone yet left 836 non-magenta
  samples (box_uncovered 15805/16641), and identical variants 1/2 disagree
  (16641 vs 15257). So 3-vertex results in this log are not trustworthy.
- OBSERVED, not proof: n=94 whole draw, tri1 samples vary
  (af8425/ae8425/b68f44/b0862e), tri2's four samples (uv y 0.5..0.83) are all
  ~bd9e5e on a 507x507 real texture. Consistent with tri2 getting a constant/wrong
  texcoord varying instead of not being rasterized.
- INFERENCE: "covered = pixel != magenta" cannot tell "not rasterized" from
  "rasterized but shader output == destination". For the FBF darkening shaders
  (fid76 in 225), a wrong colour/uv varying would look exactly like a lost
  triangle. The earlier "triangle never rasterized" readings (225-228) are
  therefore not proven; varying/IDVS output is the better-supported lead.
- Merged main 6c80fa7 into this branch (77a9811): per-pass box-sized
  targets, variants 0,1,5,6,8 x2 repeats, 2 picks/process, 2D draws only.
- New (this commit, X9FrameCapture.inc only, x9vf only): variant 10 = whole
  draw with every fragment texture slot bound to a 256x256 BGRA8 gradient
  (B=0x40, G=y, R=x). Printed bgra = 40 GG RR ff -> sampled uv ~ (RR+0.5)/256,
  (GG+0.5)/256. Run twice like the others. X9 FRAME REPLAY GRID lines: 16x16
  box-relative grid (cell centres) for variant 0 and 10, repeat 0.
  Readback budget factor 10 -> 12 passes/pick. NOT compiled: tunnel down
  (ssh -p 2222 refused, 2026-11-03).
- Reading the next log: variant 10 grid smooth and continuous across the
  diagonal and matching vertex texcoords -> varyings fine, look at the FS/
  FBF path; tri2 flat or discontinuous at the diagonal -> varying (IDVS
  generic varying buffer / attribute fetch for vertices 6..8) wrong while
  position is right -> next instrument PanVK varying buffer for that draw.
  If GRID equals magenta in tri2 for variant 10 too -> really not rasterized.
- Build: python3 x9/build/build.py all --revision 234 --name x9-rev234-uv
  --boot-arg x9vf; open CC before ~45-60 s and keep it static (picks are the
  first 2 eligible draws after 45 s in backboardd).

## Active: rev233 logging build (2026-11-02)

- Branch feat/G1Ultra, HEAD 0f9745a. Built and packaged locally; NOT flashed,
  NOT booted. Candidate artifacts:
  x9/out/x9-rev233-log/boot.img (9420800 B),
  x9/out/x9-rev233-log/kernel.development.sweet,
  x9/out/x9-rev233-log/plugin-package/ (10 files, payload 51,347,456 B,
  X9GeneralMetal233.bundle + X9PanVK233.bundle with SHA256SUMS).
  Kernel overlay compiled 51/51 (6 prebuilt). Boot args now 607/608 bytes:
  the base 602-byte line plus the single umbrella token x9vf (4 chars).
- Build: python3 x9/build/build.py all --revision 233 --name x9-rev233-log
  --boot-arg x9vf. PanVK patch 11 (11-panvk-fbd-log.patch) is registered in
  both build-panvk-native-ios.py and build-panvk-incremental.py; applying it to
  the pre-patch source reproduces the working tree byte-for-byte. The CSF
  object was compiled alone before the full build.
- What x9vf now emits (all logging only; no compositing, submission or cache
  decision changes):
  - X9 FRAME COVER (frontend): per-draw CPU projection of the position
    attribute through fragment buffer slot 2, bbox plus scissor clip and
    clipped_nonempty; unavailable=<reason> when a binding is not CPU readable.
  - X9 FRAME TEX: per fragment texture slot tex/parent/surface/format/size.
  - X9 FRAME FBF: per colour attachment bound also as fragment texture, with
    load/store and pipeline write mask.
  - PASS line extended with surface=<IOSurface ID> and parent=<traceID> for
    colour, depth and stencil attachments.
  - Kernel (SweetFramebuffer.cpp): under x9vf only, the panel sampler grid
    rises 32x32 to 64x64, and X9 DAMAGE (union of presented layer dst rects,
    layer mask, full-panel coverage) plus X9 LAYER lines are logged on change
    or every 240 identical frames.
  - PanVK (patch 11): first X9 FBD RT: line per distinct render-pass shape
    (bounded 64 signatures) with fb/render/tiling area, tile size, samples,
    per-RT format, in_bounds/border load op, load.always, store, store.always,
    iview presence, colour_write_enables and write mask; X9 FBD ZS for
    depth/stencil; X9 FBD totals at 256/512/... and every 65536 passes.
- Trigger schedule is now three 20 s windows at 60/80/100 s after start
  (3 CBs each, 400 draws/CB). To capture Control Center, open and manipulate
  it inside one of those windows; backgroundd only.
- The Timed::record() histogram fix that was already in the working tree is
  included in 0f9745a. The rev233 manifest replaced the earlier rev232 one.
- Runtime observed (device log /Users/tosasitill/2.txt, 45,247 lines, ends at
  uptime 100.05 s inside f=8): boot-args end with x9vf; "X9 DIAG: x9vf active"
  at 17.964; sampler sample=4096 (64x64) confirmed; X9 FRAME COVER 679,
  TEX 810, FBF 1360, DRAW 680, DAMAGE 187, LAYER 184, FBD PASS 135 (backboardd
  64 distinct, mediaserverd 28, SpringBoard 21, chronod 18, SBRendererService 4;
  FBD ZS 11). Render-area emulation confirmed: fb=64x64 with
  render=0,0-56,56 tiling=0,0-63,63 (log 4895) and 64x64 with render 0..31.
- Observed limits, not guesses: (a) FBD signature budget (64/proc) is exhausted
  by backboardd at passes=256 (log 5226), so the 59-60 s window (uptime 91-92 s)
  produced no FBD lines; (b) frontend capture is process-agnostic but only
  backboardd produced records - SpringBoard printed the capture init and no CB;
  (c) /dev/console writes from other processes interleave into some X9 lines;
  (d) COVER is a vertex-projected bbox, not fill; path_blit draws (no a0) report
  a meaningless bbox while clipped still follows the scissor.
- New evidence from that log: damage/present is always the whole 1256x2760
  panel, one layer, surface ids 2/3 alternating, so no partial-damage path
  exists at display level. All 1360 FBF rows have bound_as_texture=0: no
  captured draw reads its own colour attachment. Per-draw write masks are c0=15
  and c1=0 except the 1064x450 offscreen pass at f=4 n=1..4, where c0=0 and
  c1=15 with both attachments load=clear/store - the inverted-mask shape Task 10
  predicted, now directly observed. Texture-to-present mapping works: tex 23
  aliases surface 3 (the presented layer id), and tex 59 -> surface 71 ->
  offscreen 80 -> composited into surface 3 at (96,288) 1064x450.
- Still unresolved: the Control Center platter diagonal split. The 59-60 s
  capture does contain a CC-like chain (1064x450 backdrop composite at
  (96,288), 316x692 blur reads of surface 3, 57x57/64x64 and 110x69/355x36
  passes) with full pass/attachment/pipeline/vertex-input records, but nothing
  in this log proves the diagonal's cause and the FBD data for those passes is
  missing.
- Next (needs a new build): make the FBD table rotate instead of dropping new
  signatures, and extend/segment the capture windows so the CC window lands
  inside an incomplete trigger (the log stops mid f=8). Path_blit coverage
  should be marked unavailable rather than printing the projection garbage.

## Previous: Task 9 rev231 audit + Task 10 first-frame audit (2026-11-02)

- Branch feat/G1Ultra, HEAD 681f64e. READ-ONLY audit; no production code,
  kernel, PanVK, runtime or build input was modified, built or flashed by this
  task. Unrelated working-tree edits were already present and were left alone:
  x9/kernel/generated/x9_render_external_manifest.h now names rev 232, and
  NativeBridgeMetalFrontend.mm has a Timed::record() histogram fix. They are not
  part of this commit and their state is whatever the earlier rev 232 work left.
- Task 9 deliverables (earlier session): docs/graphics/rev231-runtime-audit.md,
  rev231-metal-latency.md, rev231-wsi-pitch.md, rev231-buffer-lifecycle.md,
  rev231-submission-performance.md. Input log /Users/tosasitill/2.txt.
- Task 10 deliverables (this session): docs/graphics/first-frame-rendering-audit.md,
  damage-tracking-audit.md, cross-frame-attachment-audit.md,
  control-center-vs-folder-rendering.md, first-frame-experiment-plan.md.
  architecture-inventory.md (Module I/J + cross-module note) and
  refactor-issues.md (MEM-7, DISP-6) updated.
- Captured facts (rev 211 folder / rev 212 swipe logs):
  - All panel-sized colour-0 passes are Load+Store (17 in rev 211, 43 in rev 212).
    No panel-sized pass uses a Clear load. The `attachment_clear` pipeline
    (path_blit_vert + attachment_clear_frag, 4-vertex strip) logs write mask 0 on
    colour 0 and 15 on colour 1 in every occurrence, so it does not zero colour 0.
  - The only Clear actions are full-panel blur intermediates after the swipe
    (rev 212 ids 134/135/138/139/140/141 at 157.652-162.335 s), followed by a
    Load pass (id 142). No composite was captured (6-draw per-pair cap).
  - No damage region exists at any layer: WSI ignores VkPresentRegionKHR,
    render area is full-attachment, PanVK incremental rendering is tiler-OOM only.
  - Display/GPU completion are separate events; the kernel's completed_id already
    implies cache visibility (clean/invalidate before reply), so CPU cache is not a
    mechanism for mixed pixels.
- Control Center screenshot (only pixel evidence; no CC runtime log): each rounded
  card is split by one constant-x+y diagonal with different mean colours on the two
  sides (measured L1 3.6-39.3). It matches the path_blit_vert strip diagonal, is not
  tile-aligned, and is deterministic, so it is not cross-frame reuse or a tile-clear
  boundary. Per-triangle cause (interpolation/attributes/FBF order) is UNKNOWN.
- NOT IMPLEMENTED (by instruction): the four experiments in
  first-frame-experiment-plan.md are design only. No flash, no build, no boot.
- NOT PROVEN: that a swipe redraws the exact corrupted pixels; that the presented
  surface is the captured colour 0; any root cause. The folder/App Library
  "inherited surface bytes" explanation stays a hypothesis.
- Next: user review of the five reports. If an experiment is approved, start with
  experiment 3 (m2v_no_recycle, build-cfg only) then experiment 1; experiments 2
  and 4 need new instrumentation/barriers and an explicit go-ahead.

## Previous:230 Task 7 Display backend boundary (2026-11-02)

- Branch feat/G1Ultra. Tasks 4-7 and the rev 230 allowlist are committed
  together. Full rev 230 build ran: PanVK incremental + X9PanVK230 + 
  X9GeneralMetal230 (boundary clean, 27 sources, two pre-existing
  X9FrameCapture warnings), kernel 51/51 objects linked and stripped
  (kernel.development.sweet 17995904 B), boot.img packaged (9420800 B,
  SWEET-XNU candidate, entry 0x1000). Artifacts under
  x9/out/x9-5b3caad-dirty/ (name carries the pre-commit hash). Pushed as
  origin/feat/G1Ultra, commit 681f64e.
- DEVICE VERIFIED (log /Users/tosasitill/2.txt, 72.5 s session): rev 230 boot,
  allowlist accepted X9GeneralMetal230/X9PanVK230, factory opened in
  backboardd and mediaserverd, desktop rendered through the CPU copy path,
  display cadence ticks 120/121 per 2 s with missed=0, copies 27-45 per 2 s,
  copy_avg 1.3-1.8 ms, TE frames with timeouts=0, touch/SpringBoard input
  working, no graphics panic. Kernel X9 RENDER timing moved: wait_us window
  average max 10,952 us, single job max 35,092 us.
- GAP (source-confirmed): the Task 6 userspace rows G/T/Q never appear because
  native_bridge_metal_telemetry has no caller in the kernel overlay, frozen
  XNU tree or any repo tool; the bundle only exports it. Userspace baseline is
  NOT COLLECTED. Docs corrected in submission-baseline.md and phase-4 report.
  The reader is implemented in rev 231 (see below); steps 2-6 stay blocked
  until a rev 231 log with the rows is captured.
- Host tooling note for repro: boot packaging needs llvm-objcopy/llvm-nm from
  /opt/homebrew/opt/llvm@22/bin on PATH and the lz4 module visible under
  `python -I`; used .local/build-venv (gitignored) for the latter. build.py
  itself did not need a change.
- Task 7 step 1 (partial), steps 3/4 groundwork: new
  x9/kernel/src/iokit/Kernel/arm/X9DisplayBackend.inc holds
  x9_display::Surface/Scanout, format/blend/row/rows/scaled copy primitives,
  validate() and mapped_addresses(). It encodes exactly the pre-mapping
  acceptance conditions directPresent() used inline (scanoutPhys, 4-byte
  elements, >= panel size, 16-byte aligned fetch, below 4 GiB) plus a
  diagnostic Reject reason. SweetFramebuffer.cpp:1474 includes it;
  directPresent() :1482-1535 uses validate(), then calls kernelAddress() and
  mapped_addresses() in the original order (kernelAddress can map lazily);
  x9_layer_* remain thin wrappers :1556-1590 so present() call sites and
  behavior are unchanged.
- BUILD VERIFIED end to end (rev 230 full build): SweetFramebuffer.cpp
  compiled without new warnings, the kernel linked 51/51 overlay objects and
  boot.img packaged; the extraction is exercised on device through the copy
  presenter.
- NOT IMPLEMENTED (deliberate): true client-surface direct scanout (current
  directPresent copies into the flip buffer to avoid half-drawn frames);
  show/retire hold enrollment for the direct surface; separate display fence;
  three-buffer queue object; modifier/IOMMU/display-DMA-domain verification.
  Steps 5-8 remain open; device test (step 10) is the user's.
- Task 6 (submission baseline, step 1 only): NativeBridgeMetalFrontend.mm now
  reports commit-to-completion P50/P95/P99, submit-call duration, commit to
  scheduled queue latency and in-flight depth (rows G ctc, T submit, T queue,
  Q inflight). BUILD VERIFIED with build-general-metal.py --revision 230:
  boundary clean (metal, 27 sources), bundle linked/signed; the two
  pre-existing X9FrameCapture.inc:147,148 warnings. On device the rows are
  absent (no consumer; see GAP above), so the instrumentation itself is
  NOT TESTED.
- Task 6 steps 2-6 (lock split, submission queue, resource set, cache
  precision, fence wait) are NOT IMPLEMENTED: they need a measured baseline
  first and steps 3+ need the ABI v2 async path from the Task 2 design.
- Docs: docs/graphics/submission-baseline.md, phase-4-report.md,
  display-backend.md, phase-5-report.md; Module F/G evidence updated in
  architecture-inventory.md.
- Control Center platter triangles remain UNRESOLVED; neither task changed
  rendering.
- Rev 231 (BUILD VERIFIED, not flashed): the plugin now reads the telemetry
  table itself when the boot args contain the exact token x9_telemetry=1 or
  its 3-byte alias x9t (the payload has ~5 spare bytes), once every 120
  completions, on a utility queue so ctc samples are not distorted. Off by
  default; boot.json is unchanged and the candidate carries x9t via
  --boot-arg. Artifacts: x9/out/x9-rev231-telemetry/boot.img (9420800 B,
  sha256 6d914c7a...c826e6), plugin-package, allowlist regenerated to 231,
  kernel 51/51 linked. Build needed the same llvm@22 PATH + build-venv lz4.
- Next: user flashes rev 231 boot.img + rev 231 plugin package, leaves the
  desktop idle ~60 s, opens Control Center once, captures the X9 METAL G/T/Q
  rows. Then fill the baseline and start steps 2-6; afterwards (a)
  direct-scanout A/B with hold enrollment, (b) Task 2 async submission.

## Previous:230 Task 5 Buffer Manager (2026-11-02)

- HEAD 5b3caad, branch feat/G1Ultra. FOUR files changed (2 new sources, 1 edited
  source, 1 build script) plus docs; NO ABI header, kernel, PanVK, manifest or
  display change; NO allocation/mapping/cache/fence behavior change; NO device run.
- Task 5 (P3): added a userspace Buffer Manager metadata/lifecycle registry in
  the Metal bundle. New x9/metal/General/X9BufferManager.{h,c} — keyed by
  NmbBuffer*/NmbTexture*, X9BufferDesc (kind IOSurface/HostLinear/Shadow/
  GPUOwned/Display, ownership, usage bits, format domain, <=3 planes) and
  X9BufferLifecycle (separate GPU and display state + refs, use counters).
  Lifecycle transitions are registered explicitly; GPU state is tracked
  separately from display state, and unregister tombstones while refs are held
  (reclaim-last semantics).
- Integration (NativeBridgeMetalFrontend.mm): root registration for IOSurface
  (:267), heap buffer/texture (:1388/:1401), device buffers (:1571/:1579/:1588),
  device texture (:1596), planar IOSurface with 2 planes (:1625-1634) and
  buffer views (:359); unregister in dealloc (:226/:288); GPU refs acquired at
  encode (:1211-1212) and released in finish (:1172-1177).
- All calls are ADVISORY: a failed register/ref cannot refuse a Metal resource;
  no Metal object retention changed; no allocation, map, cache or fence change.
- BUILD VERIFIED (userspace bundle only): build-general-metal.py --revision 230
  -> boundary check clean (metal, 27 sources), compiled/linked/stripped/signed;
  only the two pre-existing X9FrameCapture.inc:147,148 warnings. nm audit:
  bundle defines all eight _x9_buffer_* symbols; frontend object references
  register/unregister/gpu_acquire/gpu_release/describe; no new IOKit or
  x9_render_* references from the Buffer Manager object.
- NOT IMPLEMENTED (deliberate): shadow (X9BufferKindShadow) and display
  (X9BufferKindDisplay) registration — metal2vulkan exposes only aggregate
  shadow counters and scanout state is not visible to the process; no
  registration site was invented. Modifier fields are zero. NOT TESTED at
  runtime: no lifecycle observation on a device.
- NOT done: PanVK 230 build, package-plugins.py, kernel allowlist (still 229),
  kernel build, flash. Bundle 230 remains non-deployable until a user-approved
  candidate. Control Center platter triangles remain UNRESOLVED.
- Docs: docs/graphics/buffer-manager.md, docs/graphics/phase-3-report.md, plus a
  Module L entry in docs/graphics/architecture-inventory.md.
- Next: WAIT for direction — (a) 230 package+allowlist+kernel candidate,
  (b) Task 6 submission optimization, (c) Task 7 Display Backend / direct
  scanout, or (d) rework of Task 2 design into implementation.

## Previous:230 Task 4 PanVK / XNU backend boundary (2026-11-02)

- HEAD 5b3caad, branch feat/G1Ultra. THREE files changed (1 new, 2 edited); NO
  kernel, PanVK, ABI header, manifest or display change; NO device run.
- Task 4 (P2): audited the native integration and made the PanVK <-> XNU
  boundary build-enforced. New x9/build/scripts/check-native-boundary.py with
  two policies: --panvk forbids IOKit/IOSurface/display/mach_vm/libkern and
  limits the transport + x9_render_* to x9/metal/General/panvk; --metal allows
  IOKit/transport only in X9RenderTransport.c and bridge/NativeBackendProbe.mm.
  Wired at build-panvk-native-ios.py:26 (after patches, before budget/meson)
  and build-general-metal.py:40 (before Rust compile).
- Audit (SOURCE CONFIRMED): PanVK core has no direct XNU/IOKit/display
  dependency (only comments mention them); X9 integration includes only the POD
  X9RenderABI.h, X9RenderTransport.h, pan_kmod and panthor uapi; kernel owns
  CSF/queue/doorbell (X9CSFRenderQueue.cpp), GPU VM/PTEs (X9RenderPageTable.cpp),
  BOs (SweetX9RenderService.cpp:194-277) and the cache span plan
  (X9RenderCachePlan.inc); userspace mappings[4096] is bookkeeping only. Fence:
  timeline signals only after validated synchronous completion; no fixed sleep;
  kernel render_wait is a 64-entry history lookup.
- BUILD VERIFIED (userspace only): build-general-metal.py --revision 230 ->
  check clean (metal, 25 sources), bundle linked/signed; only the two
  pre-existing X9FrameCapture.inc:147,148 warnings. build-panvk-native-ios.py ->
  check clean (panvk, 109 sources), dylib linked (no-op rebuild). Negative
  self-test: a temporary panvk file including IOKit made --panvk exit 1.
- NOT done: kernel build, PanVK packaging, allowlist regen, flash. Bundle 230
  still not deployable (manifest pins 229); that needs a user-decided candidate.
- Findings not fixed: patch 00 (third_party/00-mesa-panvk-x9.patch) embeds
  /Users/tosasitill/HelioDarwin-x9-lk-preserve absolute paths and no script
  applies it, so a fresh tree is not reproducible; BO wait is connection-wide,
  not per-BO; the check is direct-dependency only (no transitive/object audit).
- Control Center platter triangles remain UNRESOLVED; no rendering path touched.
- Reports: docs/graphics/panvk-backend-boundary.md (audit) and
  docs/graphics/phase-2-report.md (13-section). Next: WAIT for direction —
  (a) 230 package+allowlist+kernel candidate, (b) Task 2 implementation,
  (c) Task 5/6, or (d) Task 7 display.

## Previous:230 Task 3 Metal/native boundary split (2026-11-02)

- HEAD 5b3caad, branch feat/G1Ultra. FOUR files changed (2 new, 2 edited); NO
  kernel, PanVK, ABI header, manifest or display change; NO device run.
- Task 3 (P1) moved the Metal runtime's last cross-layer reach into a bridge
  boundary: x9/metal/General/bridge/NativeBackendProbe.{h,mm} now owns the
  IOKit SweetX9Render probe, the x9_render_open/close liveness check, the
  accelerator-port -> registry-ID lookup and X9_PANVK_BUNDLE_PATH.
  X9GeneralMetalFactory.mm keeps only the boot-arg gate, cache path, logging and
  calls nmb_backend_{probe,loader_path,open}. build-general-metal.py compiles and
  links the new .mm.
- BUILD VERIFIED (userspace bundle only): python3
  x9/build/scripts/build-general-metal.py --revision 230 -> compiled, linked,
  stripped, ad-hoc signed (pagesize 16384). Two pre-existing warnings in
  X9FrameCapture.inc:147,148. nm audit: factory/frontend objects have 0 IOKit
  and 0 x9_render_* undefined symbols; NativeBackendProbe.o is the only object
  with IOKit (3) / transport (1) refs.
- Behavior: same gate, same probe order, same nil results, same device open
  arguments. Log-only deltas: (a) "begin" now prints before a failed registry
  lookup (before: silent nil); (b) second log prints port=%u, not registry=%llu.
- NOT done, deliberately: PanVK 230 build, package-plugins.py, kernel allowlist
  (still pins X9GeneralMetal229/X9PanVK229), kernel build, flash. Bundle 230 is
  therefore not deployable yet; that needs a new flash candidate = user's call.
- Control Center platter triangles remain UNRESOLVED; this refactor does not
  touch and does not explain them.
- Next: report written to docs/graphics/phase-1-report.md. WAIT for direction:
  (a) package+allowlist+kernel for a 230 candidate, (b) Task 2 implementation,
  or (c) further Task 3 boundary work (a build-time check that the Metal objects
  never re-import IOKit/transport).

## Previous:230 Task 1 audit + Task 2 submission design (2026-11-02)

- HEAD 5b3caad, branch feat/G1Ultra. NO source/build/ABI changes; NO build and NO
  device run. Working tree only adds docs/graphics/ and this handoff.
- Task 1 wrote docs/graphics/{architecture-inventory,call-graph,refactor-issues,
  abi-map,phase-0-report}.md. No GPU submission, cache, HSR, DCD,
  framebuffer-fetch or display behavior was touched.
- Task 2 wrote docs/graphics/submission-decoupling-design.md (design only,
  awaiting approval). It proposes: M3 front-end / M4 kernel queue / M5 backend /
  M6 fence / M7 display split; ABI v2 additive selectors 12-15
  (SubmitAsync/FenceWait/FenceQuery/SubmissionRelease) gated by caps bit 64;
  BO/VM refs per submission; lock order fLock->conn->queue->bo->vm->hw->fence;
  hw_lock only around AS/doorbell/halt; resource-set cache plan with
  reachability fallback; build matrix (stages D1-D2 kernel-only, D3-D4 need
  plugin + allowlist revision, metal2vulkan unused).
- Findings (source-confirmed unless noted):
  - Submit is synchronous end to end: render_submit polls the fence and
    x9_pan_kmod_submit requires completed_id == submission_id; render_wait is a
    64-entry history lookup, not a blocking wait.
  - render_submit holds metal_service_lock across cache maintenance, AS install,
    doorbell and the completion poll; display uses TryLock and skips ticks.
  - Cache maintenance is per-mapping span-exact with holes excluded and
    WC/coherent/never-exposed skips; the plan doc's "whole-BO CIVAC" premise is
    outdated. Remaining imprecision is the reachability scope.
  - Display is CPU composition + LK GUI submit with fresh TE; GPU result is
    copied back and memcmp-verified. No GPU direct scanout; GPU/display do not
    share an IOVA domain. Hardware capability for either is UNKNOWN.
  - PanVK has no submit-time user-resource list (only panvk_priv_bo refs);
    metal2vulkan does track used_resources() but it cannot cross Vulkan.
  - externalMethod rejects descriptors and caps input at 96 B; it also holds the
    per-context lock for the whole selector, so FenceWait needs its own path.
- Not re-verified: build or runtime. Per rule since 2026-10-28, compile/package
  only, and this session ran neither.
- Next: WAIT for explicit approval of the Task 2 design (5 decisions in §10).
  Do not start implementation; 1-3 touch submit/cache semantics.

## Previous: main 6c80fa7 race-free repeated replays, merged into feat/G1Ultra 2026-11-03 (main called it "230"; unrelated to the branch rev230 boundary build)

- 229 RUNTIME (.local/analysis-mailbox229/2_229.txt): picks were 3D/
  perspective CC transition strips (box 124x830), not 2D platter quads. Same
  draw at variant 1 and variant 2 (identical for count 9) gave 6448 vs 496
  uncovered samples (n=3), rotations differed too. Either GPU
  non-determinism or a harness artefact: all variants of one pick shared one
  target texture, so a pass's readback could race the next pass's clear.
- USER OBSERVATION: Today-view (negative-one screen) blur is completely fine;
  the problem is transparency/alpha compositing (CC/App Library platters use
  framebuffer-fetch darkening shaders), not blur itself.
- 230: every replay pass uses its own box-sized targets (viewport shifted
  by the box origin); variants 0,1,5,6,8 each run twice (repeat=0/1); picks
  limited to 2 per process and to non-perspective draws (clip w 0.99..1.01).
  Determinism question: same variant, different repeat -> different coverage
  means the GPU result itself is not repeatable.

## Previous:229 owned-buffer / mirrored replays of the lost triangle (2026-11-02)

- 228 RUNTIME: x9bf, x9bv, x9ba all still split/flicker (user). x9ba log
  .local/analysis-mailbox228/2_228_x9ba.txt shows compiler A/B mode=3 in
  backboardd/SpringBoard. => NOT a Kraid codegen bug (BIR identical result).
  Excluded so far: CA inputs, metal2vulkan translation, GPU-visible memory,
  index ranges, HSR, FPK, pass overlap, heap size, zero-area cull, provoking
  vertex, compiler. The lost triangle is the lower-left half (BR,BL,TL) of
  framebuffer-fetch platter quads (fid68/fid76), even drawn alone.
- 229: replay variants 6..9 draw from a buffer the capture owns at offset 0:
  6 lost triangle moved to vertex 0..2, 7 whole draw copied, 8/9 lost triangle
  mirrored horizontally / vertically in object space. Replay picks per process
  reduced to 4 (readback memory). Not syntax-checked unless noted.
- Reading: 6 covered -> depends on vertex index/offset in the original buffer
  (attribute fetch / descriptor); 7 still lost but 6 covered -> vertex index>=3;
  8 or 9 covered -> geometric (edge orientation) rasterizer/tiler behaviour.

## Previous:228 Kraid-vs-BIR compiler A/B (10); source only (2026-11-02)

- 227 RUNTIME (.local/analysis-mailbox227/2_227.txt): 8 CC picks replayed
  (46 passes). fid68/fid76-type pipelines (fixed_frag_lph_cph with
  framebuffer_fetch, no texture, vertex colour 003c x4 = opaque white for ALL
  vertices): the triangle (BR,BL,TL) is never rasterized/written in isolation,
  in any vertex order (variants 3/4 rotate provoking vertex) -> not provoking
  vertex, not input data, not other draws. Some CA draws also carry duplicated
  (degenerate) vertices by design (e.g. n=6 v1=v2=v3=BR). 3D-perspective CC
  draws (w=0.25) render both halves.
- Only fragment-shader execution distinguishes the two triangles now.
  pan_compiler.c selects Kraid (Rust, new) vs BIR via PAN_USE_KRAID (default
  all Kraid). 10-pan-compiler-bir-ab.patch: tokens x9bf/x9bv/x9ba -> BIR for
  external FS / VS / both; cache flags follow. Zero-fuzz/idempotence checked
  on upstream 300b5fd pan_compiler.c (no earlier patch touches it).
- Test: build 228, boot with --boot-arg x9bf (then x9bv/x9ba if needed);
  CC static split gone with x9bf -> Kraid fragment codegen bug.

## Previous:227 replay picks from any backboardd CB (2026-11-02)

- 226 RUNTIME (/tmp/2_226.txt, plugin226): no REPLAY records: frames at
  60/80 s uptime held only a simple 16-draw scene (CC not open then). Not a
  fix; user confirms still flickering/split.
- 227 (commit 0baa33e): replay candidates (non-indexed CC-sized triangle
  lists, float4 position) are picked from ANY backboardd command buffer after
  45 s (or x9_metal_geometry_start), process budget 8, and replayed at that
  CB's commit; PICK logs the fragment function and fid. Variants/VTX as 226.
  Not syntax-checked (tunnel down).

## Previous:226 replay logs raw vertices + rotated lost triangle (2026-11-02)

- 225 RUNTIME (.local/analysis-mailbox225/2_225.txt): 16 picks (CC-sized
  non-indexed 6-vertex quads, vertex fid25 texcoord_count0, fragment fid76:
  framebuffer_fetch1, blend_function0x29, no textures, blending OFF). Isolated
  replay on a magenta target: triangle 0 (TL,TR,BR) -> a000a0ff = magenta*0.63
  (shader darkens the destination by the vertex colour); triangle 1
  (BR,BL,TL) -> ff00ffff unchanged, in 14 of 15 draws, ALSO when drawn alone
  (variant1). One draw (n=179) had tri1 covered in the whole draw but not
  alone. OBSERVATION: the loss is intrinsic to that triangle, not to other
  draws, not "last triangle", not pass overlap. Positions are a clean
  axis-aligned rectangle; v3=v2, v5=v0 positions/texcoords.
- 226 (commit 1a438f1): replay also logs each picked vertex's 48 raw bytes
  (X9 FRAME REPLAY VTX) and adds variants 3/4 (triangle 1 alone with rotated
  vertex order via a 16-bit index buffer = other provoking vertex) and 5
  (triangle 0 alone). Not syntax-checked (tunnel down).
- Reading: VTX colour of v3..v5 zero/transparent -> CPU input differs per
  triangle (then explain why Apple draws it); colours equal but rotation
  changes coverage -> flat/provoking-vertex varying handling on v14 (PanVK
  provoking vertex / metal2vk flat inputs); no variant ever covers tri1 ->
  fragment shader result for that triangle (varyings/derivatives) or raster.
- This clone now has local git user.name/email set to the repo owner identity
  (matching existing commits) so rebases stop failing.

## Previous:225 isolated per-triangle GPU replay of CC draws (2026-11-02)

- 224 RUNTIME: log .local/analysis-mailbox224/2_224.txt, CULL ZERO AREA
  mode=1 active. User: CC static still split diagonally, swipe still flickers
  -> zero-area culling is NOT the cause (09 harmless, kept as no-op candidate).
- 225 (commit 1dc0ebd, frontend only): with x9vf, up to 8 non-indexed
  triangle-list draws (6..48 verts, float4 position, box >150x150, BGRA8
  full-screen pass) per captured CB are re-drawn after the original passes
  with the same pipeline/buffers/textures/samplers into a magenta-cleared
  target: variant0 whole draw, variant1 last triangle alone, variant2 without
  the first two triangles. Box read back; logs X9 FRAME REPLAY PICK/ENCODED/
  RESULT/TRI (covered samples per real triangle) /END. Fragment buffers and
  textures/samplers of all slots now tracked in x9vf mode; inline bytes kept
  up to 4 KiB. NOT syntax-checked (tunnel down).
- Reading: variant0 last tri covered=0 but variant1 covered -> intra-draw loss
  in PanVK/HW; all covered in isolation -> loss depends on preceding work in
  the original pass (state/ordering), not the draw itself.

## Previous:224 v14 Cull Zero Area (09); source only, NOT built (2026-11-02)

- 223 RUNTIME (user, no logs): x9s1 (no render-pass overlap) and x9tb (32 MiB
  heap) both NO visible change -> not cross-pass concurrency, not heap size.
- Correction to 222 reading: the five count54 nine-patch draws (fids 41/72)
  are a small rotated ~70x60 px shape, NOT the CC modules. The real CC module
  draws in f=6 (CC frame at 102 s) are n=139..142 / n=130..133 (505x505 at
  (89,236) and (662,236)). n=140/141 are NON-indexed triangle lists of 12
  vertices: tris (BR,TL,BR) and (TL,TL,BR) are zero-area, then (TL,TR,BR) and
  (TL,BR,BL). Clip coords valid (z=0,w=1.0033), inside scissor 89,236,505,505.
  The photo shows exactly (TL,TR,BR) present and the LAST triangle (TL,BR,BL)
  missing. Full-screen count6 quads without degenerate tris do not split.
  Non-indexed -> all index-buffer hypotheses irrelevant here.
- SOURCE: csf/panvk_vX_cmd_draw.c sets cfg.cull_zero_area only #if
  PAN_ARCH < 14 (introduced in refactor 305dd36, no stated reason); v14.xml
  DCD Flags 0 still has Cull Zero Area bit 26. Gallium does not use it.
  HYPOTHESIS: on v14 uncull'd zero-area triangles disturb the following
  primitive(s) of the draw (last triangle lost / pulled to a stale vertex).
- 224: 09-panvk-v14-cull-zero-area.patch sets the bit on v14 by default;
  token x9z0 = upstream. Zero-fuzz/idempotence checked; not compiled here.
- Build: build.py all --revision 224 --name mailbox224-zero-area (+ optional
  --boot-arg x9z0 control). If not fixed, next: frontend strips zero-area
  triangles from non-indexed triangle lists before encoding (A/B).

## Previous:223 PanVK-vs-X9 split switches x9s1 / x9tb; source only (2026-11-02)

- User chose option B. 08-panvk-serialize-passes.patch: token x9s1 -> at
  CmdBeginRendering (not resuming) the VT subqueue waits FRAGMENT syncobj >
  progress_seqno+relative_sync_point (same wait form as PanVK barriers), i.e.
  no render-pass overlap. X9PanVKQueueBridge.inc: token x9tb -> 16 heap
  chunks (32 MiB) instead of 5. Both default off; logs X9 PASS SERIALIZE and
  X9 TILER HEAP. 08 zero-fuzz apply/idempotence checked on reconstructed
  upstream+00+03..07; NOT compiled here.
- Build/test (Mac): build.py all --revision 223 --name mailbox223-split; then
  boot images with --boot-arg x9s1 and --boot-arg x9tb (one token each fits
  the 607-byte bound). Same CC static + swipe check on three boots.
- Reading: x9s1 fixes -> cross-pass overlap (shared heap/descriptor ring/
  sync between passes: X9 backend or PanVK CSF sync); x9tb fixes -> heap
  capacity despite no OOM event; neither -> single-draw execution on v14
  (PanVK/HW), next step a per-draw GPU readback inside the platter pass.

## Previous:222 result: CC platter shader translation is per-vertex trivial (2026-11-02)

- 222 RUNTIME: .local/analysis-mailbox222/2_222.txt (plugin222, x9vf). 98 X9 FC
  FUNC records; FC C rows of 12 constants are split by the USB console at
  ~256 chars, losing a few names/values (fix: shorter rows next time).
- CC platters in the framebuffer: nine-patch count54 draws, vertex fid41
  (fixed_vert_lph_spc: generic0,texcoord_count0/1=1,vertex_layout0) and
  fragment fid72 (fixed_frag_lph_cph: attachment_count2, blend_function54,
  framebuffer_fetch1, framebuffer_sample0, memoryless_index0/1=0,
  has_image_0A1, image_function1=9, texture_function5, lod_bias1, ...).
  Two values lost to line splits and assumed: has_asg_77_weights=false,
  image_count=1. Args saved as fc72.full.
- Mac translation (lavapipe profile) of fid41+fid72: spirv-val PASS
  (cc72.vert/frag.spv + cc72.json saved). Vertex shader is exactly
  position = M*pos (UBO slot2 + push bind offset), uv = scale*tc+bias; fragment
  samples one texture, no input attachment, no flat inputs, one colour output.
  => metal2vulkan's translation of this pipeline has no per-triangle
  dependency; it cannot drop one triangle of a quad. Layer is GPU execution:
  PanVK v14 and/or the X9 PanVK backend/kernel. (INFERENCE from SPIR-V,
  lavapipe image replay not run.)
- Checked: X9 tiler heap is fixed 10 MiB (5x2 MiB, prepare-panvk-native-
  budget.py), kernel has no OOM servicing, but x9_csf_group_health returns
  NoMemory on any CS TILER_OOM toggle and no such failure is logged -> heap
  exhaustion not indicated; enlarging it is NOT expected to help.
- Server note: leftover local rustc from the aborted translator build caused
  VPS lag; killed. All builds now on the Mac only.

## Previous:222 capture CA function constants for offline lavapipe replay (2026-11-02)

- User: all builds/compiles now run on the Mac (reverse SSH -p 2222 to
  tosasitill@localhost when the tunnel is up); this server only analyses.
- Mac: iOS 20H392 QuartzCore default.metallib extracted read-only from
  .local/heliodarwin/work/ios16-20H392/OS.dmg (detached) to
  .local/analysis-shaders/. metal2vulkan CLI built from the pan205 runtime
  source into .local/analysis-shaders/m2v-target (separate target dir).
  Offline translation for lavapipe profile works (spirv-val PASS) for
  downsample_blur_vert_lph+narrow_blur_7 and path_blit+attachment_clear.
- fixed_vert_lph_spc/fixed_frag_lph_cph (CC platters) are QuartzCore's uber
  shader (~60 function constants incl. fc_framebuffer_sample,
  fc_memoryless_index0/1); cannot be translated without CA's live values.
- 222 change (commit ac7f0e6): with x9vf, log every specialized function's
  constants (X9 FC FUNC/X9 FC C, keyed by fid) and per captured draw the
  vertex/fragment fids (X9 FRAME FUNCS); draw cap 400/CB, 2 triggers
  (60/80 s), GPU byte compare off. Not syntax-checked (tunnel dropped).
- Next: build revision 222 with --boot-arg x9vf, open CC before 60 s, keep
  static to ~90 s. Then translate the exact CC pipelines on the Mac and
  replay the 316x692 platter pass on lavapipe here: correct there -> PanVK/X9
  side; wrong -> metal2vulkan translation.

## Previous: three fix hypotheses falsified; need GPU-output evidence (2026-11-02)

- 221 RUNTIME (user): still flickers; control image not flashed. Log
  .local/analysis-mailbox221/2_fpk.txt, plugin221, FPK policy mode=1.
- OBSERVED: CA's 2-RT draws already had upstream_kill=0 (e.g. rt_mask=3
  written=3 effective=2 can_fpk=1 upstream_kill=0): roa_color (rasterization
  order access, enabled by metal2vulkan for framebuffer fetch) blocks FPK in
  every 2-RT pass. FPK killed only rt_mask=1 draws (legit). => 07 is a no-op
  for CA; FPK hypothesis FALSIFIED. 04 (HSR) and 06 (index range) also showed
  no visible effect. Keep them only as harmless; do not present as fixes.
- Kraid tile-access waits compared to BIR va_insert_flow.c at 300b5fd:
  LD_TILE waits Barrier(7) -> encode_flow emits full .wait, same as BIR;
  BLEND/ST_TILE likewise; ATEST/ZS_EMIT -> wait0126. No discrepancy found.
- Established so far: CA inputs correct (217/219), GPU-visible vertex/index
  bytes == CPU (219), no index OOB, HSR/FPK/index-range toggles no effect.
  Per photo the LAST triangle of nine-patch/quad draws is lost and wedges
  share one apex. Mechanism still UNKNOWN; layer not yet isolated.
- Proposed next evidence (await user choice):
  A) device: copy colour0 of the CC platter offscreen pass (316x692, stored)
     after the pass and after each split point, compare with a CPU raster of
     the captured draws -> first wrong draw/pass.
  B) desktop: replay a captured frame through metal2vulkan on lavapipe (needs
     the CA shader AIR from the Mac) -> correct on lavapipe = PanVK/X9 side;
     wrong = metal2vulkan.

## Previous:221 PanVK FPK honours colour write masks (07) (2026-11-02)

- 220 RUNTIME: logs .local/analysis-mailbox220/2_{widen,exact}.txt. plugin220,
  IB policy widen=1 resp. 0 confirmed, IB WIDEN e.g. requested=12 bound=261712.
  User: both still flicker, no visible difference. => tight index-range
  hypothesis FALSIFIED. 06 is harmless and kept for now; can be dropped.
- Re-read of 219 frame (observation): in the 316x692 platter pass each
  nine-patch draw (c0 blend, c1 mask0) is followed by path_blit_vert/
  attachment_clear (c0 writeMask 0, c1 mask15) and mask quads with c0 mask 0.
  HSR logs show these as rt_mask=3 written=3 effective=2.
- SOURCE: upstream FPK rule uses rt_written = shader outputs (write masks not
  considered; blend setup skips zero-mask RTs so any_dest_read stays 0). So
  those draws get allow_forward_pixel_to_kill=1 and may kill earlier colour0
  fragments they never write. FPK kills fragments still queued, i.e. the
  most recently submitted triangles of the previous draw = its LAST triangle
  -> matches nine-patch centre lower-left and quad (2,3,0) loss, timing-
  dependent flicker, and HSR-off only helping a bit (x9h0 never touched FPK).
  INFERENCE until hardware confirms.
- 221 change: x9/build/third_party/07-panvk-fpk-write-masks.patch: FPK
  requires effective (really written) RTs to cover rt_mask; x9f0 token =
  upstream. Zero-fuzz apply/idempotence with 04/05 checked; helper+block
  compiled with gcc stubs (CA case written=3 eff=2: upstream 1 -> 0).
- Build on Mac:
  python3 -I x9/build/build.py all --revision 221 --name mailbox221-fpk
  python3 -I x9/build/build.py boot --kernel x9/out/mailbox221-fpk/kernel.development.sweet --name mailbox221-fpk-upstream --boot-arg x9f0
  commit regenerated manifest header; mailbox update with 221 package.
- Check: X9 FPK policy mode=1, X9 FPK DCD with written=3 effective=1/2
  upstream_kill=1 kill=0; CC static platters whole? swipe flicker? Then x9f0
  image as control.

## Previous:220 widen tight index-buffer range (PanVK 06) (2026-11-02)

- 219 RUNTIME (user): plugin219, x9vf active. Log .local/analysis-mailbox219/
  2_219.txt, photo control-center-219.png (CC static, badly broken).
  7 FRAME CBs (f=4 hit the 160-draw cap; backboardd then produced no CB from
  80 s to 253 s, cause not examined). 580 GPU CHECKs.
- OBSERVED: every vertex/index range a draw fetches, copied by GPU blit after
  the passes, equals the encode-time CPU snapshot, EXCEPT 14 ranges, all of
  path_blit_vert_lph/attachment_clear draws: that shader reads one float4 rect
  (vertex 0 = 0,0,1256,2760), not 4x48-byte vertices, so the next draw's data
  at base+16..80 legitimately follows; gpu_diff==cpu_changed there. Capture
  artifact, NOT a memory fault. => X9 kernel cache/mapping and CPU ownership
  are excluded for these draws.
- OBSERVED: no X9 PANVK IDX OOB. backboardd indexed kinds: prim4 isize2
  count6 bound=12 (exact), isize4 count3 bound=256. metal2vulkan binds the
  index buffer with exactly count*isize via vkCmdBindIndexBuffer2.
- OBSERVED (data+photo): CC platters are 4x4 nine-patch count54 draws whose
  last six indices are 5,6,10,10,9,5 (centre cell); the photo shows the centre
  cell's upper-right triangle present and the last triangle (10,9,5) missing,
  the same "last triangle" of count6 quads (2,3,0) seen earlier; thin wedges
  converge on one apex, consistent with a vertex-0 substitution.
- INFERENCE (not proven): the tail of a tight INDEX_BUFFER_SIZE is read as
  zero by the v14 index fetch (coarser bounds granularity than bytes), making
  the last triangle degenerate (idx 0,0) or pulled to vertex 0. Alignment-
  dependent, which fits "only some draws" and motion-dependent flicker.
  Layer: PanVK/HW contract for INDEX_BUFFER_SIZE, exposed by metal2vk's exact
  sized binds. Also seen: CA draws path masks with MTLPrimitiveTypeLine 1-px
  spans (count 68..654) into colour1; not yet examined.
- 220 change: x9/build/third_party/06-panvk-widen-index-range.patch on the
  COMMON src/panfrost/vulkan/panvk_vX_cmd_draw.c (upstream 300b5fd file, no
  earlier patch touches it; reconstructed, not the Mac tree): explicit-size
  index binds widened to the VkBuffer end. Token x9i0 = exact size (A/B).
  Zero-fuzz apply + idempotence checked; helper gcc-stub compiled only.
- Build on Mac:
  python3 -I x9/build/build.py all --revision 220 --name mailbox220-ib-widen
  python3 -I x9/build/build.py boot --kernel x9/out/mailbox220-ib-widen/kernel.development.sweet --name mailbox220-ib-exact --boot-arg x9i0
  commit regenerated x9/kernel/generated/x9_render_external_manifest.h; mailbox
  update with 220 package. Boot default image first (fix), then x9i0 (control).
- Check: X9 PANVK IB policy widen=1 and IB WIDEN lines; CC static platters
  whole? diagonal/wedges gone? blur halves? flicker while swiping?
  If fixed with widen and back with x9i0 -> confirmed.

## Previous:219 whole-frame geometry + GPU-visible byte compare (2026-11-02)

- 218 RUNTIME (user, three boots, one mailbox): logs preserved (gitignored)
  .local/analysis-mailbox218/2_{allrt,off,upstream}.txt + control-center-218.png.
  All three show plugin218 and the expected X9 HSR policy mode per process.
  backboardd totals at65536 draws: mode1 upstream_can_cull=20156 can_cull=4190
  blocked=15966; mode2 18739/18739/0; mode0 17196/0/17196 and can_be_culled=0.
  DCD signatures confirm CA uses rt_mask=3 with written=3 effective=1/2
  (write-masked RTs) and read=1/2/3 LD_TILE draws.
- USER OBSERVATION: x9h0 (HSR fully off) somewhat less flicker/errors but
  NOT fixed. Most errors appear while swiping (flicker, alpha errors, diagonal
  triangles scattered); static scenes mostly fine EXCEPT Control Center, which
  is badly broken statically. Mode of the photo not stated.
- PHOTO (observation): every CC module platter is split along its TL->BR
  diagonal; the upper-right triangle (v0,v1,v2) has the dark material, the
  lower-left (v2,v3,v0) is missing it. Long thin dark wedges run from one
  module's diagonal to a corner of another module (e.g. camera -> rotation
  lock -> module1 icons). Interpretation (INFERENCE): the triangle that alone
  uses v3 is drawn with a wrong v3 position, often another quad's vertex.
  That is a vertex-fetch/index/position problem, not HSR culling (HSR cannot
  create triangles outside a primitive). HSR is at most a secondary flicker
  contributor; 04 default fix kept.
- 217 CPU data for its captured draws was correct, but the CC platter draws
  were NOT among the 18 captured (quota). Their CPU inputs are unverified.
- 219 changes (source only; no Mac build here, no compile of ObjC/Mesa):
  * x9/metal/General/X9FrameCapture.inc + hooks in NativeBridgeMetalFrontend.mm
    / X9BlurCapture.inc: x9vf whole-frame capture (see x9/BUILD.md) with GPU
    blit copy of each draw's vertex/index bytes compared after completion.
    tracksGeometry() = geometry||frame for the existing tracking hooks.
  * x9/build/third_party/05-panvk-index-fetch-audit.patch: logs indexed draws
    whose index range exceeds INDEX_BUFFER_SIZE + distinct index kinds.
    Verified zero-fuzz apply/idempotence on reconstructed upstream+00+03+04;
    block compiled with gcc stubs only.
- Build on Mac:
  python3 -I x9/build/build.py all --revision 219 --name mailbox219-frame
  python3 -I x9/build/build.py boot --kernel x9/out/mailbox219-frame/kernel.development.sweet --name mailbox219-frame-x9vf --boot-arg x9vf
  commit regenerated x9/kernel/generated/x9_render_external_manifest.h;
  mailbox update with 219 package; boot the x9vf image.
- Replay: open Control Center BEFORE 60 s uptime and keep it static until
  ~130 s (triggers 60/80/100/120 s). Save 2.txt + photo. Then (same boot or
  not) swipe for flicker data is optional.
- Decision table: FRAME GPU CHECK gpu_diff>0 on the platter draws -> memory
  visibility/mapping (X9 kernel/PanVK BO path). gpu_diff=0 and CPU vertices
  correct but photo still split -> GPU vertex fetch/IDVS (PanVK/hardware);
  IDX OOB lines -> metal2vk index binding size. CPU vertices themselves wrong
  (v3 already off) -> frontend/CA input path. cpu_changed>0 -> CPU rewrote
  bytes before GPU completion (ownership/completion ordering).

## Previous:218 PanVK HSR all-RTs cull fix + A/B tokens (2026-11-02)

- User approved implementing the hsr_can_cull fix below and asked to push,
  collecting all needed evidence in one run.
- New maintained x9/build/third_party/04-panvk-hsr-cull-all-rts.patch,
  applied after03 by build-panvk-incremental.py and build-panvk-native-ios.py.
  Verified locally only: zero-fuzz forward apply and "Already applied" reverse
  check on upstream Mesa300b5fd panvk_vX_cmd_draw.c + 00 + 03 hunks (that file
  only; reconstructed, not the Mac's mesa-render-source). Helper code compiled
  with gcc -Wall -Wextra -Werror against stubs. NOT compiled in Mesa/iOS.
- Behaviour: per-process mode from exact kern.bootargs token (same sysctl the
  frontend already uses successfully for x9vg):
  default 1 = hsr_can_cull also requires effective_written != 0 and
  !(rt_mask & ~effective_written); effective excludes RTs with colour write
  disabled or write_mask 0. x9h2 = unmodified upstream (217 behaviour).
  x9h0 = hsr_can_cull=0 and hsr_can_be_culled=0. Only removes culling.
- Logs: "X9 HSR policy: proc= mode=", <=48 "X9 HSR DCD" distinct signatures
  per process (rt_mask/written/effective/read/dest_read/ld_tile/coverage/
  upstream_can_cull/can_cull/can_be_culled), "X9 HSR totals" at power-of-two
  draws >=256 and every65536 (blocked = upstream would cull, now not).
- build.py gained --boot-arg TOKEN (repeatable; rejects whitespace and
  duplicates). Normal boot args are602 bytes; one4-byte token fits the607 bound.
- Exact build on the Mac (normal boot.json, no x9vg):
  python3 -I x9/build/build.py all --revision 218 --name mailbox218-hsr-allrt
  then commit the regenerated x9/kernel/generated/x9_render_external_manifest.h.
  Variant images reusing the same kernel/plugins (boot step only):
  python3 -I x9/build/build.py boot --kernel x9/out/mailbox218-hsr-allrt/kernel.development.sweet --name mailbox218-hsr-off --boot-arg x9h0
  python3 -I x9/build/build.py boot --kernel x9/out/mailbox218-hsr-allrt/kernel.development.sweet --name mailbox218-hsr-upstream --boot-arg x9h2
  One mailbox update (218 plugins) serves all three boot images.
- Next replay, one session if possible, same Control Center/App Library
  interaction for each boot, keep each 2.txt separately plus a photo/clip:
  (a) default fix, (b) x9h0, (c) optional x9h2 control. Read X9 HSR policy
  mode, DCD signatures with rt_mask&~effective!=0 and upstream_can_cull=1,
  totals blocked>0. Interpretation: (a) fixes -> RT-mask HSR gap confirmed;
  (a) fails but (b) fixes -> other HSR culling (can_be_culled/hierarchical)
  involved; (b) also fails -> HSR excluded entirely, look at tiler/IDVS/
  varyings next. blocked=0 in (a) means the gap is not reached.

## Previous:217 runtime decoded; CPU geometry clean; PanVK v14 HSR can_cull gap (2026-11-02)

- User ran217 (plugin X9GeneralMetal217 at3.302s, capture inputs0/pixels0,
  x9vg active) reproducing Control Center/App Library blur defect: diagonal
  half-quad missing blur + flicker. Log preserved (gitignored) at
  .local/analysis-geometry217/runtime217.raw.log; decoder decode_geom.py and
  output geometry-decoded.txt in the same directory.
- OBSERVED: 18 GEOM draws over3 windows incl. count54x5/48/24/18 multi-quad
  draws. Every draw INDICES complete=1 readable=1, no SKIP/OOB/truncation;
  all quads canonical (n,n+1,n+2,n+2,n+3,n) or a consistent 4x4 nine-patch;
  positions form exact rectangles/parallelograms, UVs affine with positions,
  w=1, viewport/scissor match attachments, cull0. CA packs uniforms,vertices
  and indices contiguously in one256KiB ring (e.g. verts3408+36*48=idx5136).
  8 COMMIT/COMPLETE pairs, all status4. => Apple input + X9 frontend encode
  are correct for these draws. (CPU encode snapshot; GPU fetch not observed.)
- OBSERVED: kernel flush_gpu (FLUSH_CACHES0x23304,L2+LSC clean/inv) before
  and after every job; coherent skip inactive (group0_type=0); CIVAC kept for
  cacheable exposed; jobs complete before return.
- OBSERVED (log, 03 patch probe): CA tile-reading shaders read RT1 (read=2)
  with write=1 or3, i.e. memoryless color1 (load0/store0) is a tile-local
  scratch carried BETWEEN draws of one pass.
- SOURCE (upstream Mesa300b5fd csf/panvk_vX_cmd_draw.c ~2510 vs ~2605): legacy
  FPK requires !(rt_mask & ~rt_written) (draw must write EVERY bound RT before
  killing earlier fragments); v13+ hsr_can_cull omits that term (only !ld_tile,
  rt_written!=0, !(read&written), !any_dest_read). A later opaque draw writing
  only RT0 can thus be marked able to cull an earlier draw whose RT1 output a
  later LD_TILE draw needs. Culling follows the later primitive's triangles
  and in-flight timing -> matches diagonal halves + flicker.
- INFERENCE, NOT PROVEN: layer most likely PanVK (v14 HSR DCD flags), not
  metal2vk (deterministic per-vertex translation cannot intermittently drop
  one triangle of a static quad) nor X9 kernel cache path. Non-prepass HSR
  semantics on G925 remain unverified; no per-draw can_cull log for RT0-only
  writers yet. User previously rejected an HSR rollback; this is a DIFFERENT,
  narrower change. (User approved; implemented as 218 above.)
- Proposed single-variable candidate218: add !(rt_mask & ~out->rt_written)
  to hsr_can_cull (mirrors FPK), optional bounded log of can_cull for draws
  with rt_mask&~rt_written. Only removes culling; no shader/sync changes.
  No build, device action or commit of code this turn.

## Active:217 vertex/triangle observation built; NOT staged;216 CANCELLED (2026-11-01)

- User explicitly rejected the HSR direction and now asks to first locate the
  vertex/corner defect.216 is CANCELLED; its local output remains archival,
  NOT a deployment candidate. No216 mailbox update or device boot occurred.
  Device's last verified run remains215 + confirmed renderer task-limit fix.
- Restored215 source patch03, build entrypoints/helper and215 allowlist from
  930123a^; active local Mesa compile.rs/panvk_vX_cmd_draw.c restored from the
  saved pre216 copies. Keep2cb9897 task-limit repair. Restoration itself did
  not build or touch a device; subsequent217 build is recorded below. The
  historical216 deployment instructions are superseded in full.
- Current narrow investigation: original draw indices/full vertex inputs,
  buffer offsets/lifetimes and vertex-to-raster state. Prior six-index
  captures do NOT validate count54 draws; viewport/scissor were not captured.
  Scheduled/group/display and performance investigation are deferred at the
  user's priority, not established root causes. Do not retry deindex210 or
  relabel unused half-color/HSR/cache hypotheses as confirmed geometry bugs.

- Baseline restoration committed49a0e08. Source inspection confirms frontend
  indexed arguments reach runtime and CmdDrawIndexed unchanged; live vertex
  binding memo includes buffer+offset+stride, index memo includes buffer+
  offset+type+size. This rules out those specific source omissions, NOT GPU
  fetch/output correctness. No evidence yet identifies the corrupt triangle.
- Implemented separate opt-in x9_metal_geometry CPU capture to close the
  count54/late-window gap, without GPU waits/readbacks/deindex/shader changes.
  Starts uptime45s, three30s windows, independent two-draw quotas for multi-
  quad blur/ordinary blur/non-blur composition (max18/process). Full effective
  indices up to96, unique referenced shared vertices, layout+buffer offsets,
  matrix/UV/EDR state, viewport/scissor/raster, CB commit/completion identity.
  Private/missing/instance-stepped/OOB/truncated inputs are marked, not
  silently validated. Fixed diagnostic offset-after-setBytes to mirror active
  runtime's zeroing; did NOT change runtime offset semantics.
- Geometry mode is independent of old inputs/graph/pixels; normal boot.json
  remains unchanged. Separate exact build input:
  .local/analysis-geometry217/boot.json. Added explicit --boot-config to the
  normal builder.217 is an OBSERVABILITY candidate, not a visual/FPS repair.
  Exact normal build command (final input uses short alias x9vg):
  python3 -I x9/build/build.py all --revision 217 --name mailbox217-geometry --boot-config .local/analysis-geometry217/boot.json
  No extra tests/device operations.

- First normal217 attempt: PanVK/Metal and51/51 kernel compilation/link
  succeeded; boot packaging FAILED because normal boot args already602 bytes
  and long diagnostic tokens produced649 > existing607-byte payload bound.
  No device operation. Did not remove any original boot arg or enlarge ABI.
  Added exact short alias x9vg, using default45s; separate final boot input
  now607 bytes. Retried the same normal build; final result follows.

- Implementation/build/217 allowlist commit:f25f87b; baseline restoration
  commit:49a0e08. No CI run; local normal build only.
- FINAL217 normal build SUCCEEDED exit0. Rust release1.21s; frontend,
  transport/factory compiled+linked+signed;10-file plugin payload51290112
  bytes; exact217 kernel allowlist generated;51/51 overlay objects+6 prebuilt
  linked+stripped; shim packaged. Build reports boot.img9420800 bytes.
  No CI run,additional tests/suites,artifact secondary inspection/hash/
  disassembly/rebuild comparison. Both attempts remain in candidate build.log
  and .local/analysis-geometry217/build-console.log. No217 runtime evidence.
- Candidate:x9/out/mailbox217-geometry/boot.img. Plugins in the same directory.
  LOCAL ONLY; device last verified215 + renderer-limit. Do NOT boot217 image
  with215 mailbox. No ADB/root-preflight/mailbox update/flash/boot/reboot/root
  write was performed this turn. Normal authorized updater,after user reports
  Android/ADB ready, must run with private serial and preserve readback log:
  python3 -I x9/mailbox/update.py x9/out/mailbox217-geometry --serial <private>
  Follow existing root preflight+protected-extents+per-range-readback gates;
  do not write boot/root or use cancelled216. User manually boots matching217.
- Exact next replay: from kernel boot uptime45..75,75..105,105..135 seconds,
  open/close Control Center/folder/App Library to reproduce polygon defect.
  Preserve a fresh complete2.txt and,if possible,visual clip of the same time.
  Confirm plugin217,capture inputs0/pixels0 and X9 GEOM capture line; gather
  GEOM DRAW/ARGS/BASE/PASS/VIEWPORT/SCISSOR/RASTER/INDICES/INDEX/ATTR/BIND/DATA/
  END plus matched COMMIT/COMPLETE. For count54 require complete1,all54
  effective indices and no relevant SKIP/OOB/truncated records before claiming
  CPU input validation. Evaluate original AIR for each referenced vertex and
  triangle,then distinguish wrong CPU geometry from GPU fetch/vertex/raster
  or sampled-texture artifacts. The capture is at encode time,not GPU output
  or necessarily the later Shared-buffer contents. No root-cause/visual/FPS
  repair claim is warranted. Do not rate FPS during diagnostic log bursts.

## Historical:216 HSR rollback built but CANCELLED, never deployed (2026-11-01)

- Implementation/build/216 allowlist commit:930123a. Runtime-evidence handoff
  commit:8857ca6. No CI run; the successful build was local.
- Normal build SUCCEEDED exit0:
  python3 -I x9/build/build.py all --revision 216 --name mailbox216-hsr-rollback
  x9/out/mailbox216-hsr-rollback/boot.img (build reports9420800 bytes).
  Log:x9/out/mailbox216-hsr-rollback/build.log. PanVK rebuilt Kraid+draw objects
  and linked; Metal Rust release1.50s,frontend/transport/factory compiled;
  both216 plugins signed+packaged (10 files,payload51273728 bytes),kernel
  exact216 allowlist regenerated,51/51 overlay objects+6 prebuilt linked,
  stripped and shim packaged. No CI,extra tests/suites or secondary artifact
  inspection/hash/disassembly/rebuild comparison. Compile success is NOT
  hardware or visual/FPS success.
- Build log confirms Retired (reversed):03-kraid-hsr-metadata.patch and
  Already applied:02-panvk-host-linear-images.patch. Both full/incremental
  entrypoints now retire03 before applying02; helper uses zero-fuzz reverse
  for patched snapshots or zero-fuzz forward applicability for original ones,
  refusing unmatched drift. Historical03 is preserved in third_party/retired.
  Pre-change local compile.rs and panvk_vX_cmd_draw.c are preserved separately
  in .local/analysis-mailbox216/before-hsr-rollback/. No global HSR override,
  extra sync or Apple shader/executable byte edits. Returning to214 metadata
  is a narrow REGRESSION COMPARISON,not a source-level correctness claim.
- adb get-state returned error:no devices/emulators found. No root preflight,
  mailbox update,device write,flash,boot or reboot was performed. Device remains
  last-tested215 + renderer-limit kernel.216 is LOCAL ONLY; MUST update its
  plugin package via the normal authorized updater with root preflight and
  per-range readback BEFORE user manually boots its matching boot.img.
  Once Android/ADB reconnects:
  python3 -I x9/mailbox/update.py x9/out/mailbox216-hsr-rollback --serial <private>
  Preserve update output at that candidate/mailbox-update.log. Do not print
  or commit serial. Do not touch the root image,protected extents or boot.
- Next replay: same Control Center/folder/App Library interaction after>40s,
  preserving a fresh2.txt and visual feedback/clip. Confirm active plugin216,
  task limit134217728 and no recurring code7 renderer kills; compare visible
  flicker/polygon/color errors and aggregate timing to215-limit. If unchanged,
  the isolated HSR rollback did not resolve the observed defect; do not repeat
  it or re-label old WSI/BLEND/cache speculation as new proof.

- User confirms flicker is still obvious and rendering errors remain after
  the renderer-limit candidate. Do not report the visual/FPS objective fixed.
- Latest /Users/tosasitill/2.txt contains TWO boots. Full capture preserved at
  .local/analysis-sbrender-limit/runtime-20261101-225011.raw.log (1570353 bytes).
  The second Darwin header begins at raw line7428; the old215 prefix includes
  the previously recorded15 renderer crashes and is NOT a new regression.
  New boot alone: .local/analysis-sbrender-limit/current-run.log (763920 bytes),
  ends681.390s. Selected evidence and boot identity are in the same directory.
- Runtime proves the 2cb9897 repair executes: plugin215 at1.305s;
  37.748s SBRendererService pid283 inactive16->128MiB; task limit134217728
  persists through native_bytes34291712/footprint30228880 at38.468s and
  Metal factory initialization. This boot contains0 SBRendererService,
  backboardd or SpringBoard CRASH lines,0 DeviceLost and0 panic. This confirms
  disappearance of the captured initialization code7 kill loop, NOT pixels.
- Active display windows record38..55 copies per~2s (roughly19..27 software
  updates/s,NOT measured panel FPS),missed0 and TE timeouts0. Late job averages
  still pay cache~4..5.5ms and GPU wait~12..15.5ms; neither cost is fixed by the
  task limit. No weakened cache/coherency/completion guard is justified.
- At96.058s and679.574s frame records explicitly clear all four surfaces
  (set=clear=80000007);675.642s surfaces restore and render/display submissions
  resume. Long black/idle windows do NOT prove GPU hang; reason for upper-layer
  clear is unresolved. Capture remains inputs0/pixels0; old WSI pitch fallback
  and _blendWithAlphaMaskB0 lookup refusal are not newly established causes.
- Candidate216 ONLY retires 03-kraid-hsr-metadata.patch, including
  reverse migration of already-patched local Mesa, while keeping215 diagnostic
  removal,214 host-pitch imports/QoS and confirmed renderer task-limit repair.
  This is a single-variable visual-regression comparison, not proof HSR caused
  the corruption. Apple shader/executable bytes remain unchanged. Normal build
  only; no extra tests/artifact inspection, no automatic device boot/flash.

## 215 renderer task-limit fix built; subsequent runtime recorded above (2026-11-01)

- User reports215 has MORE rendering errors, still flickers/low FPS. Latest
  /Users/tosasitill/2.txt preserved separately as
  .local/analysis-mailbox215/runtime215-followup.raw.log (689585 bytes,
  ends323.673s). Earlier215 capture remains runtime215.raw.log (251.341s).
  Active plugin215,capture inputs0/pixels0 and actual tile-reading DCD
  can_cull0/can_be_culled0 are all runtime-confirmed. The visual regression
  cannot yet be attributed to HSR vs diagnostic removal; no shader rollback
  has been performed. Do NOT claim these HSR fields repaired hardware pixels.
- NEW decisive failure beyond the truncated214 log: SBRendererService dies
  15 times,38.535..188.196s,roughly every10.65s,at Metal/PanVK initialization.
  At38.531..38.533s its first3 one-stream submissions complete; second10MiB
  allocation at38.535s brings native_bytes21676032,followed immediately by
  SIGKILL,reason namespace1/code7. Close footprint24871392 exceeds its
 16777216-byte task limit. Repeat60.201s has footprint24838544, same limit
  and reason. Local XNU kern_memorystatus.h defines7 as
  JETSAM_REASON_MEMORY_PERPROCESSLIMIT, and on_ledger_footprint_exceeded
  creates exactly that reason. This is a confirmed finite-task-limit restart
  loop, NOT DeviceLost, a measured shader bug, or proof of flicker's cause.
- Native PanVK utrace_context_init unconditionally allocates10MiB even
  without enabled tracing. It explains a source-level initialization cost;
  second10MiB allocation's exact producer is not resolved. Do not label both
  allocations tiler heap, whose native queue reserves128MiB separately.
- Consulted local UniXNU ae687009348296e97e49ce6e925795044f363ce6 and
  hackios c84b4634277a70beac73921029b1fb6afc2530af: no exact renderer
  per-process-limit repair found. hackios xnu/hakios.patch uses sane_size
  for global pressure percentages; that is NOT this code7 fatal task-limit
  failure and was not ported. Existing X9 mediaserverd inactive floor is the
  local integration precedent; retain it unchanged for mediaserverd.
- Implemented a bounded128MiB floor for positive active/inactive limits ONLY
  on X9, x9_metal_render=1 and exact p_name SBRendererService. Preserve
  <=0 default/unlimited limits,higher limits,fatal attributes,priority,
  memory accounting and global pressure kills. x9_sbrender_mb=0 restores
  original limit policy;32..256 adjusts floor, invalid values use128.
  Both SET limit paths preserve it across later updates; real render-client
  open repairs pre-exec launchd-installed limits before native BO mappings,
  using a held proc reference,owner/task match and proc_list_lock.
- Normal KERNEL ONLY build succeeded exit0:
  Implementation/build commit:2cb9897.
  python3 -I x9/build/build.py --name mailbox215-sbrender-limit
  Compiled51/51 overlay objects (6 prebuilt), linked+stripped, shim packaged;
  build reports boot.img9420800 bytes. Log:
  x9/out/mailbox215-sbrender-limit/build.log. No CI,test suite,secondary
  artifact inspection/hash/disassembly/rebuild comparison or device operation.
  Candidate:x9/out/mailbox215-sbrender-limit/boot.img, reusing already-staged215
  allowlist/plugins. This isolates the confirmed task-limit defect without
  simultaneously changing HSR,texture policy,Metal diagnostics or Apple
  shader bytes. NO new mailbox update needed if installed mailbox remains215.
  User manually starts this candidate; no automatic flash,boot or reboot.
  Next manual runtime: same interactions through>40s; check X9 JETSAM floor
  log and live task limit134217728,absence of recurring code7 renderer kills,
  then compare visible errors and cadence. If visual regression remains,
  HSR rollback remains a separate comparison, not a substitute for this
  confirmed process failure.128MiB is headroom,not a measured steady budget.
- Remaining observed constraints: interaction still pays several ms per job
  on all-import pre/post cache maintenance plus GPU wait; this scoped task
  floor is not a full FPS fix. Repeated WSI pitch-alignment errors still
  accompany fallback for smaller textures; main1256x2760,row5120 explicitly
  imports. No mapping protection,alignment,cache or completion guard weakened.
  Later copies/submits0 do not prove a GPU hang. Actual late display producer
  and photographed polygon corruption still need runtime correspondence.

## Mailbox215 built: Kraid HSR metadata and normal-path capture removal (2026-11-01)

- Subsequent user readiness message: adb get-state confirmed device. Normal
  mailbox215 update succeeded exit0 with serial from private root manifest:
  python3 -I x9/mailbox/update.py x9/out/mailbox215-hsr-clean-render --serial <private>
  Root preflight verified device,pin+immutable,inode/map/partition,protected
  extents,current HFS UUID/geometry. Mailbox committed51269632 bytes;
  each written range read back. Log:
  x9/out/mailbox215-hsr-clean-render/mailbox-update.log. Only the approved
  mailbox updated; no boot write,flash,reboot or iOS-root-file modification.
  Supersedes LOCAL ONLY below:215 is now STAGED, manual boot/runtime pending.
  Matching boot:x9/out/mailbox215-hsr-clean-render/boot.img. No215 visual/FPS
  success evidence; next action remains user's manual boot+blur interaction.

- User's new /Users/tosasitill/2.txt still reports flicker and low FPS. Preserved
  separately at .local/analysis-mailbox214/runtime214.raw.log; ends mid-line
  at34.987s, NOT a complete late-interaction capture. At3.345s active plugin
  is verified214 (the earlier missing-file line precedes mailbox installation).
  At16.508s backboardd's1256x2760,row5120 surface has direct1,pitched1,shadow0;
  subsequent matching surfaces increase pitched to4. Thus214's explicit-pitch
  route WORKS on hardware; do not diagnose this run as modifier negotiation
  failure. Other formats/pitches still shadow. No215 hardware evidence yet.
- Captured display copies are sparse during startup, with no missed software
  ticks or TE timeouts; cannot calculate interactive FPS or identify the later
  photographed producer from this truncated log. Last aggregate render batch
  at33.631s is9 jobs: pre241us,storage289us,start525us,wait498us,retire353us,
  post107us. These startup jobs are not a measurement of later blur load.
  GPU coherency remains capable0,active0; explicit cache maintenance retained.
- Implementation/build commit e33cb0a. Maintained local PanVK patch
  x9/build/third_party/03-kraid-hsr-metadata.patch now fills Kraid's omitted
  fragment fs.hsr from final scheduled/lowered instructions. Mirrors current
  retained BIR valhall/va_gather_hsr_info.c: LD_TILE (both NIR load variants),
  generic/special centroid and rasterizer coverage, generic varyings and
  WAIT/WAIT_RESOURCE/tile work before ATEST or ZS_EMIT. Flow classification
  follows Kraid encode_flow, not all message-slot waits. Gather precedes
  pan_shader_update_info, which preserves hsr; no shader instruction, Apple
  AIR, attachment identity, ROA flag, global HSR policy or prepass enable
  substitution. Full/incremental native builds consume03 after02 via the
  existing zero-fuzz/already-applied source gate. No upstream submission.
- This repairs a SOURCE-PROVEN metadata contract omission, NOT a proven
  hardware flicker root cause: prior notes left non-prepass HSR consumption
  uncertain. Existing PanVK DCD logic now rejects tile readers for can_cull
  and can_be_culled. First4 tile-reading DCD builds/process log X9 HSR tile-read
  with read/write masks,safety bits and actual packed-field decisions. No
  per-draw dumps or added GPU synchronization. Need215 log+visual result.
- Detailed Metal capture is now explicitly opt-in via exact whitespace-delimited
  kern.bootargs tokens, read once/process. Default inputs0,pixels0 bypasses
  diagnostic buffer/texture tables,setBytes copies,pipeline/sampler/pass string
  formatting,blur/graph logs,pixel readback encoders and asynchronous dumps.
  x9_metal_capture=1 restores bounded CPU input/graph capture;
  x9_metal_pixels=1 also enables intrusive pixel readbacks. These are absent
  from normal215 bootargs. Real runtime bindings/resourceOwners/IOSurface
  lifetime,completion ordering and sampled surface/aggregate counters unchanged.
  Removes definite instrumentation work; no measured sustained FPS gain claim.
- First normal build stopped in Kraid at Rust E0502: gather used Shader's
  immutable NIR borrow after pan_shader_update_info's mutable FFI coercion.
  Fixed ordering to gather before the update (same preserved hsr fields);
  failed console retained .local/analysis-mailbox214/mailbox215-build-first-failed.log.
  C DCD object compiled in that attempt; later incremental build reused it.
- Normal build then succeeded exit0:
  python3 -I x9/build/build.py all --revision 215 --name mailbox215-hsr-clean-render
  Kraid compiled/native
  PanVK linked; Rust Metal release1.50s,frontend/transport/factory compiled,
  plugins signed+packaged215; exact allowlist regenerated,51/51 kernel overlay
  objects compiled,linked/stripped,shim packaged. Build reports boot.img
  9,420,800 bytes. Logs:x9/out/mailbox215-hsr-clean-render/build.log and
  .local/analysis-mailbox214/mailbox215-build.log. No CI,test suite,secondary
  artifact inspection/hash/disassembly/rebuild comparison or device operation.
- 215 is LOCAL ONLY. Device remains last-tested214. Normal mailbox update must
  precede user manual boot of x9/out/mailbox215-hsr-clean-render/boot.img;
  retain all root preflight/readback. No auto flash,reboot or partition write.
  Next: capture215 through actual Control Center/folder/App Library interaction,
  confirm capture inputs0,pixels0 and tile-read DCD can_cull0/can_be_culled0,
  compare visual corruption and render/display aggregate timings. If flicker
  persists despite corrected DCD, do not re-label the old metadata omission
  as established cause; full faulty-draw inputs/EDR/actual GPU output remain open.

## Mailbox214 updated: host-pointer explicit-pitch texture repair (2026-11-01)

- User reconnected adb and requested the prepared mailbox update. The normal
  updater completed with exit0 using the serial from the private root manifest
  (not recorded here):
  python3 -I x9/mailbox/update.py x9/out/mailbox214-host-linear --serial <private>
  Mutable-root preflight verified device,pin+immutable,inode/map/partition,
  protected extents,current HFS UUID/geometry before writes. Log reports
  Mailbox committed:51273728 bytes; each written range read back.
  Log:x9/out/mailbox214-host-linear/mailbox-update.log. Only the authorized
  mailbox was updated; no flash,reboot,boot-partition write or root-file change.
  Matching boot:x9/out/mailbox214-host-linear/boot.img. User must manually
  start214; no214 runtime/visual/FPS evidence yet, flicker root cause still open.
  Implementation/build commit353ae1c (iOS QoS portbf1a007 included).

- User asks to fix the explicit-pitch/direct-rendering blocker described in
  the preceding turn. Implemented the runtime+X9 native integration path,
  preserving213's iOS QoS fix. No Apple shader substitution or Kraid/HSR tweak.
- Maintained native integration patch:
  x9/build/third_party/02-panvk-host-linear-images.patch. Under X9_NATIVE_KMOD
  it enables EXT_image_drm_format_modifier while leaving fd,dma-buf,foreign
  queue and WSI features disabled. Existing native host image format query
  now accepts ordinary LINEAR or DRM_FORMAT_MODIFIER_EXT only when its queried
  modifier is DRM_FORMAT_MOD_LINEAR, host handle is HOST_ALLOCATION_EXT and
  type is2D. General format/usage/extent queries still run before this check;
  native external memory stays importable only, no dedicated-only claim.
  Applied to the two existing X9 integration sections of mesa-render-source;
  no upstream compiler/rasterizer change, no Mesa upstream submission.
- Maintained runtime patch0025-host-pointer-linear-textures.patch:
  device/create.rs enables the modifier name independently where host import
  is enabled and the driver lists it (dma-buf route already adds it where
  present). Enables::push_name already deduplicates names. Adopted devices
  are checked against their actual enabled extension list. host_pitched_image
  now checks host importer availability+Device::enables_extension, instead of
  requiring Importer::makes_dma_buf_images. Existing modifier format features,
  external-importability, memory-type, dedicated requirement, reported layout,
  alignment and allocation-bound checks retained. Existing explicit-plane
  driver-defect guard retained. Unsupported cases still return a shadow.
- Added consumer opt-in HostPageTexturePolicy::InPlaceWrites based on current
  UniXNU AdrenoMetal0014 policy; global default InPlace and exact policy Shadow
  unchanged. X9 bridge opts in under x9_runtime_latest. This bypasses the
  read-only qualification/write-usage policy gate, NOT image/binding checks.
  It targets host-page textures; no owned-buffer padded-row relaxation or
  arbitrary Turnip16x4 rule copied. Native linear-row sizing/framebuffer bounds
  were traced earlier; actual G925 edge behavior still requires runtime proof.
- Build integration: shared apply-source-patches.py handles forward or
  already-applied source with zero-fuzz applicability, rejecting drift.
  Both incremental/full PanVK entrypoints apply native02 before compilation;
  Metal entrypoint applies0023 then0025. x9/BUILD.md documents the patch stack
  and the source lock as migration baseline. Source locks were not rehashed.
- Minimal observability: nmb_device_texture_import_stats gives six counters
  (host textures direct,pitched images,shadows,pitch/image/placement refusals).
  Existing sampled IOSurface log cadence (first8,then every128) also prints
  X9 TEXTURE routes. Counts are cumulative per process/device, not an exact
  per-texture verdict; early first-surface deltas can demonstrate the route.
  No new pixel/vertex/shader capture, sync, per-draw logging or forced delays.
- Normal build succeeded with exit0:
  python3 -I x9/build/build.py all --revision 214 --name mailbox214-host-linear
  Native incremental5 Ninja steps compiled both modified source units and
  linked; Rust release53.38s; Metal frontend/bridge linked and signed; both
  bundles packaged214; kernel allowlist regenerated, kernel linked and boot
  packaged. Build reports boot.img9,420,800 bytes under
  x9/out/mailbox214-host-linear/. Logs: that directory/build.log and
  .local/analysis-blur212/mailbox214-host-linear-build.log. No CI run.
- No tests/suites or secondary artifact inspections/hash/disassembly/rebuild
  checks, per user preference. At build completion213 and214 were LOCAL ONLY;
  subsequent214 mailbox update succeeded as recorded above. No automatic boot
  or reboot; hardware/visual/FPS success NOT established by build or update.
- Next runtime evidence: reproduce same Control Center/folder/App Library
  blur after paired214 update+manual boot; preserve new2.txt separately. Match
  X9 SURFACE layout1256x2760,row5120 with increasing direct/pitched counts,
  inspect refusal counters if absent, then compare visible polygons/colors
  and cadence. This validates the repaired route; it is not yet a claim that
  the photographed flicker has the same cause.

## Texture/blur continuation after QoS build: explicit pitch is blocked (2026-11-01)

- QoS port is committed bf1a007; local mailbox213-ios-qos build succeeded,
  device remains212 (no install or boot). Continued static texture review,
  no second build or tests, no texture policy/driver/shader changes.
- Concrete NEW optimization blocker beyond HostPageTexturePolicy's write
  rejection: actual native Mesa tree mesa-render-source/panvk_vX_physical_device.c
  X9_NATIVE_KMOD override sets EXT_image_drm_format_modifier=false,
  KHR_external_memory_fd=false,EXT_external_memory_dma_buf=false and
  EXT_queue_family_foreign=false. Read actual built-source override, not
  base mesa-source's upstream extension table. Same code in maintained
  00-mesa-panvk-x9.patch around1097. Host-pointer import remains supported.
- Current runtime texture/in_place_image.rs::in_place_unbound accepts an
  ordinary linear host image only if reported offset0 and row_pitch exactly
  match Metal's (array pitch too for arrays). Otherwise host_pitched_image
  requires Importer::makes_dma_buf_images; device/create.rs::dma_buf_image_names
  unnecessarily groups modifier+foreign with external-memory-fd+dma-buf,
  and host_memory.rs further requires dma_buf.is_some(). Thus this route is
  unavailable under X9's enabled capabilities, even after a hypothetical
  InPlaceWrites port. cfg(unix) includes iOS; this is NOT a Linux-only cfg bug.
- Captured main IOSurface texture1256x2760 BGRA8 has row5120,offset0,
  allocation14139392. Native linear layout path pan_mod_linear_init_slice_layout
  aligns BGRA8 rows to64: align(1256*4,64)=5056, NOT5120. Explicit layout
  would pass row5120 through WSI constraints; ordinary host linear image
  cannot represent it. Therefore flipping the policy alone still shadows this
  concrete main-surface layout. This is a proven source-contract performance
  obstacle, not proof of an actual bad pixel producer or measured copy cost.
- Consulted current Vulkan primary spec: image_drm_format_modifier depends on
  Vulkan1.2 (or its core/extension prerequisites), not external-memory-fd or
  dma-buf, and explicitly permits other external memory handle types.
  https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_drm_format_modifier.html
  A future host-pointer explicit-pitch route should negotiate modifier support
  independently, keep current fd/dma-buf features disabled, and retain external
  format/importability/dedicated-requirement/plane-layout/memory-bound checks.
  No driver extension advertised without implementation review this turn.
- Downloaded reference base624bab's three relevant runtime sources under
  .local/analysis-blur212/unixnu-current/runtime624 and its companion
  UniXNU/patches/mesa/turnip-host-import-images.patch. Current reference still
  uses the same dma-buf grouping. Companion patch relaxes dedicated-image
  requirements specifically for host-pointer imports; it is a distinct
  prerequisite from padded-row/tile-edge handling, not generic Mali code.
- PanVK native linear image contract read: pan_layout.h gives64-byte BGRA8
  row/offset alignment; pan_mod.c sizes a linear surface by row_stride*actual
  mip height, rounded to row alignment (not a16x4 tile-height pad). pan_desc.c
  pan_emit_linear_color_attachment emits LINEAR with actual base,row_stride,
  surface_stride; pan_fb.c emits framebuffer width/height and clamps bounds
  to framebuffer extent. Explicit/ordinary non-WSI linear images disable CRC
  in panvk_should_checksum, avoiding untracked external writes behind CRC.
  These source contracts differ from Turnip's16x4 GMEM block blits; actual
  G925 edge-store behavior is not measured or licensed by those contracts.
- Blur color round-trip check: current BGRA8/RGBA8 shadows can select
  shadow_merge::with_unorm_view (one mip/layer, sampleable optimal format).
  bind/shadow_merge_image.rs uses OpImageFetch (no sampler/filter), RoundEven
  after *255 and ConvertFToU; BGRA uses order[2,1,0,3] before packing bytes.
  sRGB is excluded from that path. No direct extra filtering, sRGB conversion
  or R/B packing omission found. Native copy-back dispatch cost facts apply
  to Intel in current measured table, not a demonstrated Mali fast path.
  Source check does not prove which kernel corrupted the live output.
- InPlaceWrites targets imported/buffer-backed surfaces; surface0 in graph
  does not establish small blur intermediate allocation/storage kind. Keep
  that optimization separate from sampled same-image10-mip feedback and
  the photographed per-triangle artifact. Original narrow7 still normalizes
  alpha conditionally and scales RGB by EDR slot6; exact live slot6 remains
  uncaptured. No new root-cause/visual improvement claim.

## Mailbox213 built: upstream0023 iOS QoS port (2026-11-01)

- User explicitly asks to port0023, then continue texture/blur source review.
  Maintained patch x9/build/third_party/metal2vulkan-pan205/
  0023-quality-of-service-on-ios.patch ports AdrenoMetal831a9dc's Apple QoS
  cfg changes to the current retained runtime. Local source's intervening
  lower_to_utility function differs from newer reference context; adapted
  hunks at the same six implementation/fallback gates, with zero-fuzz apply.
  Submission/lowering/completion threads now request USER_INTERACTIVE on iOS;
  driver compile threads request at least USER_INITIATED. Warm-thread utility
  behavior, pipeline-wait policy and texture policy unchanged.
- build-general-metal.py applies this maintained patch before compilation,
  accepts an already-applied source via reverse applicability, and rejects
  unrelated source drift. x9/BUILD.md records upstream source and behavior.
  Actual retained .local runtime was patched by this build, not just a loose
  patch saved without a consuming build step. No Mesa source edits.
- Initial unadapted reference patch was rejected by source applicability
  before Metal compilation (one of four hunks); no partial source mutation.
  Adapted normal build succeeded:
  python3 -I x9/build/build.py all --revision 213 --name mailbox213-ios-qos --panvk skip
  Build process exit0, Rust release24.94s; existing PanVK repackaged213,
  Metal compiled/linked/signed, plugin package+exact kernel allowlist generated,
  kernel compiled/linked and boot packaged. No CI run.
  x9/out/mailbox213-ios-qos/boot.img: build reports9,420,800 bytes.
  Normal build log x9/out/mailbox213-ios-qos/build.log; console copy
  .local/analysis-blur212/mailbox213-ios-qos-build.log.
- No extra tests, build-artifact inspections, hash checks, disassembly or
  second verification rebuild. No device operation/update/reboot; device
  mailbox212 unchanged.213 is built locally, NOT installed/hardware verified.
  QoS effect on X9 FPS and polygon flicker remains unmeasured. Texture/blur
  review continues separately; no speculative InPlaceWrites/Exact switch.

## Fresh UniXNU upstream references: iOS QoS and pipeline waits (2026-11-01)

- User supplied github.com/UniXNU/AdrenoMetal and /Adreno740. Fetched current
  public main heads, preserved separate source snapshots and API comparisons:
  .local/analysis-blur212/unixnu-current/{AdrenoMetal,Adreno740}.
  AdrenoMetal831a9dcbd6ac7de89a11bdaf031e07b9aadfa3c8;
  Adreno740a9a2cbb43b89d672e2921e19721a6438b738b17f.
  Prior acquisition pins ce5e19e/5c8c145 are stale: API comparison reports
  161/90 commits ahead and50/47 changed paths respectively. These counts
  include merged branch history, not161/90 distinct fixes. Current bridge
  has24 patches, not the prior16. Read current AGENTS, READMEs and official
  component pages/catalog. No reference tests/builds or device operations.
- Also pinned UniXNU build repository6790e7ce6e77f67a9174fba1255e64eacf1c41ad,
  saved relevant build scripts, dependency inputs and two Mesa patches under
  unixnu-current/UniXNU. Its current translator pin is624bab3918de5b978e1101517abca2b4fa426565,
  Mesa c823a83e8f373a5c36c2b9ed1d7dc98a4f3360ee. X9 active runtime remains
  older219f013 base plus local patches; patch filenames alone are insufficient
  to compare current behavior. No automatic version bump or patch application.
- NEW portable scheduling difference: bridge patch0023-quality-of-service-on-ios
  expands exit_guard.rs Apple QoS cfgs from macOS to macOS+ios for
  raise_to_user_initiated,run_at_user_interactive,at_least_user_initiated and
  their fallbacks. X9 current source still has macOS-only implementations;
  iOS run/raise are empty and at_least returns true. Real call sites include
  scheduler/mod.rs633,render/lowering_worker.rs93,completion.rs262,
  pipeline/pool.rs473 and device/compiles.rs311. Thus a real missing iOS
  scheduling request is established in source. Reference build script reports
  default-class iPad wake delays up to50ms; that is reference observation,
  NOT a measured X9 delay, FPS gain, or flicker explanation. This is a focused
  candidate for a later portable fix, independent of Mali instruction encoding.
- NEW pipeline omission difference: current reference NativeMetalObjects.rs189
  sets PipelineWaitPolicy::Exact. X9 nmb_device_open leaves default Bounded,
  whose policy has4500ms creation/commit bounds. Current X9 pending.rs
  record_once calls drop_late_draw on timeout and records no draw; mod.rs
  drop_late_draw counts draws_dropped_still_making and announces degradation.
  Source route exists, but current212 log does not establish a timeout. Sink
  stores counts/last warning for telemetry; absence of a console string is not
  a no-timeout proof. Exact changes compile/wait latency, so no speculative
  policy switch made. Whole-draw omission alone does not prove a half-quad bug.
- NEW current texture policy: reference185 sets InPlaceWrites; patches0014/15
  permit rendered/written padded surfaces in place,0020 removes host-page
  tile-fit restriction. Inspected actual companion
  turnip-render-mode-for-attachment-memory.patch: tu_attachment_unpadded
  recognizes linear explicit-layout/host-import attachments not whole GMEM
  blocks; load uses draw path and store uses unaligned path over texels alone.
  It can KEEP GMEM rendering.0020's inline comment about rendering without
  tile memory is not the current companion implementation; do not repeat it
  as fact. Build bridge script explicitly names this dependency. X9 defaults
  InPlace but in_place rejects render/write usage; shadows remain expected.
  No proof PanVK offers equivalent edge-store behavior, so do not copy this
  opt-in or infer a missing generic Vulkan barrier from the Turnip-specific fix.
- Reference0012 adds rounded-model fallback for unsupported fast extended-range
  sRGB blending (comments identify Astronomy BGRA10_XR_sRGB). Captured X9
  ordinary blur targets are format80 linear BGRA8. The patch is not evidence
  for those polygons or color regression; wallpaper use of that format not
  established here. Patches0001/0002 retain old semantics;0004 differences
  mostly rebase positions, not a newly missing constant-folding repair.
- Current A740 kernel changes include actual-page noncacheable classification,
  bounded cache-maintenance runs, memory allocation and two-ring preemption.
  Hardware/context/cache contracts differ from X9 CSF; no ring/preemption or
  import-maintenance shortcut transplanted. X9 native submission remains
  synchronous; new reference timing figures are not X9 runtime measurements.
- No render/runtime code change, new build, tests, installation or shader
  substitution. Mailbox212 unchanged, polygon flicker root cause unresolved.
  Best NEW portable lead is iOS QoS; source-proven pipeline omission route
  needs late-draw counters or concrete warning before causal attribution.

## Flicker continuation: BLEND encoding, flat UV gate and native heap contract (2026-11-01)

- Finished the pending BLEND format check. Kraid encode_v9.rs::OpBlend uses
  op_encode_sr_read to carry the color's DataType. proc/isa/encoder.rs maps
  register_format to sr_src.data_type.scalar_type() and vecsize to comps().
  v14 ISA uses F32=2/F16=3; its count expression yields four/two registers
  for vec4 respectively. Thus omission of a manual register_format field in
  OpBlend::encode is not an omitted hardware format. PC-relative return
  offset is encoded in eight-byte units, with 16-byte blend return prologue.
- Ordinary MRT outputs use BlendCall; ra.rs::instr_clobbered_regs lists
  r0-r15 plus BlendReturnAddr, protecting still-live later attachment values.
  nir.rs records typed blend_types before any untyped-output conversion;
  v14 does not use the old pre-v9 descriptor register_format field. No direct
  first-attachment clobber/half4-vs-float4 mismatch demonstrated.
- Traced an additional diagonal-color hypothesis: translator can deliberately
  turn a primitive-uniform varying flat. Original narrow7 UV metadata is
  air.center + air.perspective; original downsample UV is texcoord*scale+bias.
  ABI parser preserves both keys; fragment_input_deco leaves float smooth
  unless per-primitive/uniform analysis proves otherwise. Uniformity treats
  VertexInput as divergent; arithmetic propagates that fact through UV.
  All21 captured212 layouts have buffer1,stride48,PerVertex1,rate1 for each
  of attributes0-3. So this source route does not classify these UVs uniform
  through instance/constant stepping. No emitted specialized SPIR-V inspected;
  this is scoped contract evidence, not proof of actual interpolation.
- Native completion/ownership chain read beyond runtime documentation:
  X9PanVKSync.c leaves output fences pending until x9_pan_kmod_submit returns.
  SweetX9RenderEngine.inc requires each active stream's submission fence,
  extract pointer and guards, then CSG halt, GPU flush and AS removal, then
  client post-cache acquisition before returning completion. Frontend finish
  follows runtime completion. No simple early-completion vertex/index-buffer
  reuse defect found here; caller writes concurrent with work remain unobserved.
- New native tiler-heap audit: X9PanVKTilerHeap.c initializes zero context,
  64-byte headers and next=(GPU_VA | chunk_size>>12). Compared with upstream
  Linux panthor_heap.c (raw torvalds/linux/master, accessed this turn): opaque
  context zeroing and next-address/4KiB-unit chunk encoding agree; context is
  separately16KiB-aligned. PanVK sets HEAP_SET to context, initial TILER_HEAP
  bottom=base+64/top=base+size. Native header format not shown defective.
  Source: https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/panthor/panthor_heap.c
- Concrete capacity difference: native bridge passes max_chunks to heap_create,
  allocating all64*2MiB=128MiB plus16KiB context per queue. Ordinary PanVK
  requests initial_chunks5 (10MiB) with max64, growing through its KMD. X9
  keeps the full initial chain to avoid depending on absent dynamic growth.
  This identifies memory overhead, not heap exhaustion, a measured FPS cause,
  or polygon corruption. Do not just reduce capacity without growth handling.
- Root cause remains unresolved.212 late interaction151-160s is still outside
  pixel/vertex captures; first6 indices do not cover count54. Existing HSR
  metadata gap remains unproven at the faulty draw. No rendering patch, test,
  build, device action or shader substitution. Mailbox212 unchanged. Further
  checks should resolve a new concrete state/producer discrepancy, not repeat
  BLEND format, uniform-UV gate or early native completion as new findings.

## Flicker continuation: local vertex capture vs native v14 IDVS (2026-11-01)

- Examined X9 local vertex execution modifications as a separate possible
  source of polygon corruption. compile::lower_inputs selects capture only
  through simd::needs_vertex_capture: unsupported vertex subgroup features,
  lane/quad indices, group/SIMD-matrix/AGX ops or qualifying SIMD barriers.
  Original downsample_blur_vert_lph has only matrix/UV arithmetic, no such
  builtins or calls. Thus this shader does not select local compute capture
  by these source conditions. Ordinary vertex state defaults capture None;
  link copies capture only if the compiled interface explicitly has it.
  No IndexedVertexCapture notice found in212 log; absence alone is not proof.
- Native v14 path: pan_nir_lower_vs_outputs separates position/attribute/
  generic varying buckets by load_shader_output_pan, emits each bucket's
  stores under matching output-mask bit. Unified IDVS clones body per bucket
  and substitutes the local output mask before constant/DCE cleanup.
  Kraid ShaderOutput FAU page3,index4 matches Valhall special-FAU definition.
- Kraid IDVS output-buffer index uses InternalId r59; BIR uses same r59.
  VertexId r60,InstanceId r61,DrawId r62 also match. Generic varyings write
  hardware buffer1 on v12+, with linked layout byte offset; position FIFO
  buffer0. Fragment generic descriptors target buffer1. No simple wrong
  preload register/buffer-number difference found between backends.
- Kraid secondary_enable for unified VS is set iff outputs include anything
  outside position/extended-FIFO outputs, so plain float2 UV qualifies.
  PanVK secondary_shader is enabled iff secondary_enable and FS present;
  VS/FS/IDVS changes participate in tiler-flags dirty calculation.
- Correct DVS hypothesis scope: v14 direct and indirect launch sites explicitly
  pass MALI_IDVS_SHADING_MODE_EARLY to RUN_IDVS2; enum distinguishes Early0
  from Deferred1. Do not assume generic G925 deferred-small-triangle replay
  is enabled merely from hardware model or the term IDVS. Exact emitted
  command state still not captured, no new GPU execution proof.
- No demonstrated vertex-route defect. No tests/build/device action, Apple
  shader substitution or driver patch. Mailbox212 unchanged. Root cause
  still unresolved; keep full multi-quad GPU input and actual specialized
  instruction/descriptor evidence gaps open rather than rechecking these
  same route-selection conditions next turn.

## Repository push (2026-11-01)

- User requested push. git push origin main succeeded, updating remote
  ca5b8d5 -> b3c843d, including the accumulated rendering investigation commits.
- Rendering root cause remains unresolved; mailbox212 unchanged. No build or
  device action. Untracked1.txt,sys.txt and input tarball were not staged.

## Flicker continuation: texture fusion and AIR sample operand contract (2026-11-01)

- Switched from repeatedly reviewing barriers to texture lowering. Read
  pan_nir_lower_tex.c va_try_lower_tex_dual/va_opt_var_tex, Kraid nir.rs
  texture result reconstruction, and AIR intrinsic/sampler selector sources.
- TEX_DUAL requires same opcode/source count, identical SSA for every
  non-handle operand including coordinate/LOD, same dimension/result type,
  array/compare/components. It combines handles, not unrelated coordinates.
  Narrow7's uv-offset0, uv+offset0, uv-offset1, uv+offset1 are different SSA
  coordinates, so no direct conflation of those taps through this match.
  If prior optimization proves taps identical, combining them alone is not
  wrong. No generated variant available to establish actual selected ops.
- VAR_TEX fusion requires backend source directly be load_var[_buf], with
  only one use,32-bit interpolation result, no explicit offset/LOD, center
  or a verified centroid source. An arithmetic uv+offset result does not
  pass that source-shape check. Thus do not assume every blur sample fuses
  the interpolator or blame that fusion without actual compiled evidence.
- Kraid TEX result reconstruction separates two destinations at whole-register
  boundaries and picks packed-half lanes according to write masks. No
  demonstrated simple half4/float4 or second-result lane mix found here.
- Correct AIR operand interpretation: in original narrow7 call the first
  i1 true after float2 coord is OFFSET PRESENT, with zero integer offset;
  following i1 false is implicit LOD/Bias, value0 and min-lod0. It is NOT a
  normalized-coordinate flag. Sampler normalization is separate descriptor
  state. Intrinsic texture.rs binds both flags separately; sampler/select.rs
  makes fragment Bias0 implicit and Level explicit. Pan lower_tex and Kraid
  carry computed/explicit/zero/gradient LOD modes distinctly. No reversal seen.
- Captured ordinary narrow-chain input textures have one mip. An implicit
  LOD error cannot select another undefined mip within those single-level
  image views; texture filtering/coords/helper execution still matter. The
  variable blur10-mip feedback chain remains a separate case and cannot
  alone explain all ordinary narrow-blur artifacts.
- No new proven rendering fault or patch. No tests/build/device action;
  mailbox212 unchanged. Further source-only global texture-fusion searches
  are low yield without the actual faulty draw's specialized shader. Keep
  unresolved full54-index inputs, EDR slot6 and HSR metadata/hardware-state
  boundaries distinct; none has yet been established as photographed flicker.

## FB order continuation: fragment retirement and descriptor lifetime (2026-11-01)

- Extended the prior same-pixel read/BLEND chain to render-pass retirement:
  issue_fragment_jobs emits RUN_FRAGMENT2 on v14, followed by FINISH_FRAGMENT
  under cs_iter_sb_update. Finish waits current iterator and signals next;
  cs_iter_sb_update_end sets indirect wait/stream masks excluding conflicting
  direct reuse. Descriptor ring release, when used, is deferred after finish.
  Current local code includes the ordering mechanism for the upstream
  FINISH_FRAGMENT ordering issue; version/release-note exposure alone is not
  a missing patch. Did not infer hardware success from comments.
- Actual runtime primary submission and inline-first render commands use
  ONE_TIME_SUBMIT; SIMULTANEOUS_USE descriptor-ring allocation/release is a
  separate PanVK branch. No evidence this draw accidentally reuses the
  simultaneous ring. Ordinary descriptor pools remain command-buffer-owned
  until completion (earlier lifecycle trace). No early descriptor release
  found in this static path.
- Reconfirmed luma outputs lower through ATEST then BLEND; skip_atest applies
  only internal blit shaders without ZS output, not ordinary luma. Both
  load_tile variants preserve pixel/sample/coverage/conversion operands into
  Kraid; is_resource selects Resource pre-wait vs Barrier. They share one
  hardware LD_TILE opcode, so the flag not appearing in opcode encoding is
  intentional; do not claim a missing LD_TILE_RES opcode bit.
- Kraid BlendCall lowering happens after wait/flow assignment and preserves
  the original BLEND flow; adds fixed return/jump prologue for blend-shader
  descriptors. No evidence BGRA8 replacement luma uses a shader-blend
  descriptor, so this exceptional branch cannot be treated as its active bug.
- Targeted upstream primary-source searches did not find a matching Kraid
  HSR metadata repair. Historical PanVK input-attachment memory-dependency,
  BY_REGION, and FINISH_FRAGMENT fixes are not newly missing merely because
  they appear in search results; current relevant mechanisms were traced.
  No broad version update or workaround justified from those titles.
- No confirmed new rendering defect. Remaining strong evidence gap is actual
  specialized draw's DCD/FBD and generated tile-read/store operations versus
  the source contract; startup212 pixels miss late interaction. Do not keep
  recycling the same wait/barrier source checks as further root-cause progress.
  No tests/build/device operation. Mailbox212 unchanged.

## FB read/write order: same-pixel chain traced (2026-11-01)

- User asks specifically for framebuffer read/write order. Followed one
  narrow chain: earlier color write -> input-attachment tile read -> luma
  shader's use -> color BLEND writeback. No device operation/tests/build.
- Runtime binds WriteSource::Attachment input descriptor from tables.pass's
  color.target via render_target::viewed/color_view, not the original caller's
  IOSurface texture. Uses/layout resolve the selected stand-in before binding.
  Subpass input references use the SAME attachment reference as color outputs,
  in GENERAL with COLOR aspect. No separate old-image read binding demonstrated.
- Every pipeline of key.pass.fetch_inputs sets color-blend ROA, even ordinary
  writers that do not themselves fetch. The subpass sets matching ROA. Common
  Vulkan state copies rasterization_order_access into bound dynamic state.
  Thus prior ordinary writer -> later fetching pipeline is covered in API
  state, not only fetch -> fetch. No omitted prior-pipeline flag found.
- PanVK lowers input load to load_tile_pan (read/write attachment) or
  load_tile_res_pan (read-only attachment). Kraid translates both to OpLdTile;
  required_waits inserts Barrier for color R/W or Resource for read-only.
  These are hardware dependency waits, not a CPU fence. Result consumption
  waits its message slot. Output lowering emits blend_pan, translated to
  OpBlend/BlendCall, preceded by ZS+Barrier; actual BLEND conversion/write mask
  determines attachment store. load/store data dependencies remain in IR.
- Kraid scheduler classifies LD_TILE as memory Read and BLEND/ST_TILE as Write;
  its dependency tracker covers RAW/WAW and reverse WAR. message-slot pass
  additionally waits result-register hazards. No plain reordering of color
  store ahead of a consumed tile load demonstrated. This source trace cannot
  establish ordering across fragment invocations on actual v14 hardware.
- Pipeline/state-switch audit: prepare_draw calls prepare_blend BEFORE DCD;
  any_dest_read is fresh when DCD FPK/HSR conditions are computed. FS changes,
  color write enables/count/masks are covered by fs_user_dirty; blend equations,
  enables, input map and render state also participate. Initial suspicion that
  CB_COLOR_WRITE_ENABLES was absent in DCD's local list is false: macro includes
  it. No proven stale write-mask or FS-switch FPK state at these sites.
- FPK direction matters: allow_forward_pixel_to_kill is false for roa_color;
  all pipelines above are ROA, so later writers cannot kill earlier fragments
  through this FPK eligibility bit. allow_forward_pixel_to_be_killed may stay
  true, but it is not by itself permission for any later draw to kill it.
  Do not describe that lone bit as confirmed mis-culling.
- Remaining concrete discrepancy: Kraid zero fs.hsr vs BIR LD_TILE safety
  classification still changes hsr_can_be_culled on fetched-color shaders.
  Hardware consumption outside enabled prepass remains unknown. No direct
  Turnip FLUSH_PER_OVERLAP -> Mali primitive_barrier equivalence established;
  injecting a barrier draw between API draws cannot fix same-draw overlapping
  primitives by itself. Do not transplant it as a proven order repair.
- Outcome: no demonstrated missing runtime attachment identity, API ROA flag,
  tile pre-wait, result wait, blend pre-wait or DCD invalidation in this chain.
  Must identify actual compiled instructions/DCD or the first bad framebuffer
  producer to go beyond these source contracts. Mailbox212 unchanged; flicker
  root cause not yet confirmed.

## UniXNU GPU comparison: patch inventory and backend ordering (2026-11-01)

- User explicitly requests careful UniXNU GPU source comparison. Read local
  AdrenoMetal and Adreno740 AGENTS/README, official component docs/catalog,
  the16 AdrenoMetal bridge patches, relevant frontend/Rust bridge, A740
  submit/cache publication code, and local Turnip ROA implementation.
  No tests required by reference instructions were run: user explicitly
  forbids extra tests and requested static work. No reference code edited.
- Provenance from existing unixnu-gpu-20261030-inputs.json (not outer git HEAD):
  AdrenoMetal main inventory ce5e19e33e05e1873d38d818e23cabfe0ac708c3;
  A740 main5c8c1453c67d6ef6b10ec43ab1f8fc570a2f33e1; parity branches differ.
  Vendored dirs lack independent.git; inventory is acquisition provenance,
  not a fresh verification of exact local-file revision. Component build
  entrypoint scripts/Cargo manifest were not present in this partial export;
  do not claim full resolved UniXNU build inputs reconstructed. X9 active
  build-general-metal.py points pan205 source; old prepare script720ef pin is
  NOT authoritative active source. Recorded pan201 provenance base219f013...
- Private comparison artifacts: .local/analysis-blur212/unixnu-objects.diff,
  unixnu-frontend.diff, unixnu-gpu-comparison.json. JSON lists all16 patches
  with applicability, equivalent mechanism vs unmatched status. No new report.
- Correctness patches already represented in X9:0001 optional constant output
  leafwise SCCP,0002 overlapping host-range/heap resource waits,0004 constant
  global field load folding. Frontend draw count/baseVertex/index offset,
  vertex descriptor, blend factors/write masks and pass dimensions match
  reference forwarding. X9 has additional linked-function/pipeline fields;
  removing them to match older reference would lose capabilities.
- Cache/allocation changes0003/0009/0010/0011/0012/0014 have current mechanisms
  (PrimingCache, KeptRenderPasses, FieldBytes/ViewportList, instantiate_draw_into,
  command_uses_into, KeyMap); exact old patch equality is not asserted.
  0005 uses a different decode/profile cache arrangement;0013 unchanged-rebind
  optimization not fully matched. Neither is a demonstrated flicker repair.
  0006 null-pipeline skip is failed-compile handling, not blur correctness.
- Explicit deltas: UniXNU0008 enables AliasWrites for padded texture rows,
  0016 guards A74016x4 store overreach; X9 holding() refuses such alias and
  creates a shadow. Do NOT transplant16x4 geometry to Mali or bypass shadow
  safety blindly. Live main IOSurface1256x2760 BGRA,row5120,plane0/0 is padded,
  so extra-copy cost is relevant; no evidence this causes diagonal flicker.
  0007 _RB4 two-plane RGB and0015 uninitialized two-plane shadow optimization
  are not a match for that main single-plane BGRA source. X9 planar bridge
  exists in NativeMetalExtras.rs; absence in Objects.rs was not a missing ABI.
- Kernel publication differs intentionally: A740 publishes mapped BOs,
  reasserts WC and audits IB/cache vs DRAM before ring submission, retaining
  job handles through completion. X9 negotiates submission cache ownership
  and delegates cache handoff to synchronous KMD; frontend does not repeat
  UniXNU flushSurfaces. Old X9 WC audit observed no drift (see earlier notes).
  Do not add unsynchronized user cache flushes during uncertain DMA ownership.
- Most useful backend-specific distinction: local Turnip tu_pipeline.cc
  tu6_emit_prim_mode_sysmem uses FLUSH_PER_OVERLAP_AND_OVERWRITE for ROA or
  feedback, and parse_rasterization_order uses FLUSH_PER_OVERLAP in GMEM.
  PanVK normal draws implement tile loads/Barrier waits and FPK restrictions;
  primitive_barrier appears only in explicit cmd_fb_barrier. Both advertise
  rasterizationOrderColorAttachmentAccess, so UniXNU does NOT automatically
  use a storage-interlock route while X9 uses input attachments. Turnip snippet
  is local reference backend source, not a proven exact UniXNU Turnip pin.
- This makes framebuffer feedback ordering/FPK/HSR a concrete backend boundary
  to compare further; fields differ by architecture, no direct field port is
  justified. No confirmed root cause, no tests/build/device changes or shader
  substitution. Mailbox212 remains staged.

## Flicker continuation: Arm's fetch/prepass contract and actual ROA flags (2026-11-01)

- Read Arm primary documentation, not inference from descriptor names:
  https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/immortalis-g925-the-fragment-prepass
  The article distinguishes fragment prepass from other HSR (FPK can remain
  active when prepass is disabled). Its incompatibility case5 specifically
  covers helper-lane tile reads propagated across lanes, including tile-read
  values used as coordinates of implicit-LOD sampling. This is the precise
  dataflow in original luma_boost_lph: d0 -> luminance -> LUT coordinates ->
  air.sample_texture_2d. Therefore Kraid's absent LD_TILE classification
  violates the conservative eligibility policy implemented in the BIR path
  for an actually used shader family. It still does NOT prove that prepass
  executes with current defaults, nor give register-level meaning for FBD
  hierarchical enable with prepass disabled. Do not call it confirmed flicker.
- Also read Arm's framebuffer-fetch implementation requirements:
  https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/framebuffer-fetch-in-vulkan
  Ordered input-attachment feedback requires the color-blend pipeline flag
  and, for render-pass objects, the matching subpass flag. Current runtime
  has BOTH: pipeline/graphics.rs:1057 (also output-library creation1248) and
  render_target.rs:1771. Thus an omitted API-level ROA flag is not supported
  by source. Lower fragment/color.rs creates SubpassData at attachment index;
  PanVK lowers it to tile loads. This does not prove actual device execution.
- Rechecked actual mesa-render-source Kraid compile.rs and pan_compiler.c:
  no alternate HSR writeback there; lack of metadata is not merely reading
  the host reference rather than iOS build source. Existing frontend log
  lacks FBD/DCD bit values and compile-backend identity per draw, so cannot
  demonstrate active non-prepass hardware consumption from this capture.
- Main constraints remain: latent compiler metadata gap is concrete;
  missing API raster-order flags and simple lost intermediate DontCare are
  unsupported. Runtime defect was not captured by212's startup-only pixel
  windows, so no pixel comparison establishes the first bad producer yet.
- No tests/build/device action or speculative behavior change. Mailbox212
  unchanged. Source/document review has reached a hardware-state boundary
  on this lead; do not repeat generic HSR searches or declare it excluded
  from only the default prepass toggle.

## Flicker focus: framebuffer-fetch HSR safety remains open (2026-11-01)

- User explicitly prioritizes flicker again; no performance behavior edits.
- Inspected color1/memoryless preservation across internal pass splitting:
  render_target::load_at uses LOAD after first instance, store_at forces STORE
  before last instance even for memoryless originals (the memoryless DontCare
  selection only affects final-store policy). No simple intermediate-mask
  loss through original DontCare established at this site.
- Important narrowing from ORIGINAL luma_boost_lph AIR: both half4 color and
  float2 UV inputs are air.arg_unused, as is position. Its RGB uses current
  render_target0 color d0 and a luminance-derived LUT sample, alpha from d0.
  A wrong interpolated UV/half-color cannot directly change this shader's
  value. Geometry, input attachment contents/order and compiled fetch/LUT
  execution are separate possible causes. Live211 graph contains this pass.
- Correct the weight of the earlier HSR exclusion: prepass default OFF does
  NOT prove that all HSR behavior is disabled. v14 fragment flags separately
  encode prepass11, interleaving12, filter13, hierarchical optimizations14;
  csf/panvk_vX_cmd_draw.c:1489-1490 enables the latter two unconditionally.
  DCD hsr_can_cull/be_culled is populated regardless of prepass setting.
  No authoritative semantics found proving whether those fields matter in
  the non-prepass path; therefore retain this as OPEN, not excluded or causal.
- Reachable compiler discrepancy: default Kraid leaves fs.hsr zero while
  BIR va_gather_hsr_info marks LD_TILE. For a plain framebuffer-read luma
  shader (no discard/memory side effects/coverage), build_dcd_flags would
  permit hsr_can_be_culled with zero metadata; BIR's ld_tile would forbid it.
  rt_read & rt_written independently blocks hsr_can_cull but does NOT block
  hsr_can_be_culled. This is a concrete source safety-contract mismatch for
  a live shader family, not proof of active hardware mis-culling or that an
  HSR change repairs the photographed polygons.
- Also noticed readonly_color_mask's final loop references ial color_map
  while looping cal array; current runtime uses identity locations for these
  MRTs, so no reachable remap error demonstrated. Do not chase as live root.
- Consulted upstream primary Mesa mirrors for HSR semantics; indexed results
  did not establish non-prepass behavior. No reliance on unrelated results,
  no third-party hardware anecdote used as proof. No repeat of failed probes.
- No tests/build/device operations, shader substitution or speculative HSR
  patch. Mailbox212 unchanged. Next decisive evidence is actual DCD+FBD HSR
  state for the framebuffer-fetch luma draw / non-prepass semantics; existing
  frontend pipeline names and Completed status cannot settle this boundary.

## Static continuation: indexed forwarding, EDR and measured driver costs (2026-11-01)

- Followed indexed draws frontend -> nmb_render_draw_indexed -> runtime
  draw_indexed -> bind/graphics.rs -> vkCmdDrawIndexed -> PanVK launch_draw.
  Count54 is forwarded intact; index bytes are bound at index_buffer_offset,
  first_index0 and base_vertex as vertex_offset. PanVK emits INDEX_COUNT,
  INDEX_OFFSET, VERTEX_OFFSET separately and maps2/4-byte indices to UINT16/32.
  No simple count6 cap or double application of byte offset demonstrated in
  this route. Diagnostic cap6 remains separate from actual draw count.
- EDR setters: frontend scalar/range/bytes/offset setters all forward arbitrary
  fragment index, Rust maps FRAGMENT to matching runtime setters. No slot6
  special case dropping it. Exact live slot6 value and setter sequence absent.
- Concrete diagnostic mismatch: runtime ArgumentTable::set_buffer_offset
  after inline setBytes replaces that slot with zero upload at any offset;
  X9BlurCapture.inc offset override merely modifies the old NSData snapshot's
  offset. A capture of that call sequence would disagree with runtime binding.
  Current log does not record setter sequence, so reachability is UNKNOWN;
  do not label this the rendering root cause or change valid-call semantics.
- Re-read actual212 timing at the151-162s blur interaction: display timer
  ~120 ticks/2s with missed0, only20-33 submits/2s and gpu_defer1-21. Copies
  ~1.65-1.83ms average, TE timeout0. These are submission/software counters,
  NOT measured panel FPS. Render job averages at158.128s: pre-cache3354us,
  storage353us,start536us,wait12500us,retire374us,post-cache1768us. At160.128s
  wait18682us. These intervals contain diagnostics; do not extrapolate normal
  performance or equate wait_us to shader time.
- Current poll helper already has pan219 short-spin/backoff:64*16us spins,
  then20*100us deadline sleeps, then1ms sleeps. Old NOTES describing no spin
  are historical, not current. Completion wait also acquires active private
  fence pages via render_page_sync (v14 PA-range FULL0x7308, completion poll),
  every poll during spin and at >=1ms thereafter. Thus wait_us includes
  explicit GPU cache commands, CPU observation and scheduling, not shader
  timing. No counter splits these costs in current log.
- Source still serializes submission under RenderOwner while running cache,
  AS, CSG and fence handling; display TryLock failure increments gpu_defer.
  This establishes a direct contention mechanism, but does not prove whether
  GPU work or fence/cache observation dominates. Preserve fence publication,
  guards and completion checks; no speculative removal or new build made.
- No tests/device operation/render behavior change. Mailbox212 unchanged.
  Next useful evidence must distinguish actual GPU completion from fence-page
  cache-acquire/sleep latency and record full54-index/EDR data. Source-only
  review has not yet established the polygon or color root cause.

## Static/runtime-input continuation: exact blur vertex route (2026-11-01)

- Reused already extracted original AIR in .local/analysis-blur-root/
  downsample_blur_vert_lph.ll and narrow_blur_7_frag_lph.ll; no binary changes,
  new disassembly, builds or tests. Important scope correction: downsample
  vertex declares half4 color as air.arg_unused and does NOT output it. Its
  only outputs are float4 position and float2 UV. Mixed half-color varying
  packing is therefore not reachable in this vertex/fragment blur pair;
  fixed/std/luma paths still have separate contracts.
- Decoded mailbox212 CPU capture and evaluated the actual original AIR:
  clip[r] = sum(matrix[c*4+r]*position[c]); uv = input_uv*texmat.xy+texmat.zw.
  Private derived .local/analysis-blur212/vertex-route.json contains19 draws
  using downsample_blur_vert_lph. Excludes15/16 variable_blur_vert_lph, whose
  source formula has not been established. Captured four corner positions
  yield finite clip/UV and w1, so no perspective singularity at those inputs.
  This evaluates CPU snapshots, not GPU vertex output or later buffer bytes.
- Resolved the old8x8 alpha-footprint lead quantitatively: captured narrow27
  draw8 clip x=-1.03125..-0.75, y=1.03125..0.75 maps through a64x64 viewport
  to pixel rectangle[-1,8] in both axes; clipped coverage0..8 is compatible
  with the readback's8x8 alpha bbox. Draw7's preceding geometry maps to1..10
  on114x73. Thus the final texture's small footprint is already present in
  the submitted geometry; cannot call it a broken full64x64 rasterization.
  Matching a particular readback window still needs draw identity/viewport
  correspondence; no claim of captured polygon defect or repair.
- Exact narrow7 fragment source reads float2 offsets at0/8, half weights at64,
  divide half at80, and EDR scale half from fragment buffer6. Uses four samples,
  optional division by max(alpha,~0.001), then scales RGB only, preserving alpha.
  Live19/20 have weight0=0.5 and other weights0, near-zero first offsets and
  divide0/1. Tiny offset alone is consistent with a near-identity blur; no
  evidence to label it corrupted. AIR layout/GEP uses element alloc_size and
  explicit datalayout vector alignment: float2 stride8, weights64, divide80,
  struct alloc88. No demonstrated std140-style stride16 mistranslation.
- Critical evidence limits: X9BlurCapture.inc caps indices at MIN(count,6).
  count54 draws1/4/6 cover only their first quad; remaining48 indices and
  vertices have NOT been validated. Historical input/deindex conclusions
  must not exclude the whole multi-quad draw. Fragment capture only stage2
  slot1; EDR scale slot6 is absent. Base bridge forwards arbitrary buffer
  index, so missing capture is NOT evidence that slot6 is unbound.
- No tests/build/device action or speculative patch; mailbox212 unchanged.
  Root cause unresolved. Next concrete source/evidence scope: remaining
  multi-quad inputs and EDR binding, not unused half-color blur varyings.

## Static continuation: PanVK cache flush and meta state restoration (2026-11-01)

- Completed source trace of global barrier -> CmdPipelineBarrier2 ->
  add_cs_deps/normalize_dependency -> collect_cs_deps -> emit_barrier_csf.
  vk_synchronization.c expands MEMORY_READ into stage-specific reads and
  SHADER_READ into sampled/storage reads, so generic masks do not omit the
  texture-unit read class. collect_cache_flush_info filters per subqueue;
  add_memory_dependency invalidates RO L1 via others for sampled, transfer,
  input-attachment and color reads. L2/LS only flush for host interactions
  under the source's stated coherent-cache model. Hardware coherence itself
  is not proven by this source assumption.
- emit_barrier_csf waits source iterator scoreboards, issues cs_flush_caches
  with others INVALIDATE, waits IMM_FLUSH, signals subqueue sync objects and
  inserts destination cross-subqueue waits. COPY maps to BOTH fragment and
  compute subqueues in panvk_get_subqueue_stages; CmdCopyImage2 chooses gfx
  for AFBC (or COPY_GFX debug) and otherwise compute. Thus transfer mapping
  does not simply miss one internal copy engine. Source flush/wait code is
  the same in mesa-source and actual mesa-render-source (only native CSIF
  lookup/include differ in that file). No device trace of these instructions.
- CmdEndRendering2KHR flushes tiling and issues fragment jobs before resetting
  render.tiler and render.flags. Barrier after pass end is therefore not
  classified as an in-pass by-region dependency by those state checks.
  issue_fragment_jobs waits tiling; v14 uses RUN_FRAGMENT2. No source support
  for the hypothesis that end-pass merely clears state without submitting
  outstanding attachment stores.
- Checked CRC complication: global barriers carry no image CRC invalidation,
  but compute CmdCopyImage2 explicitly invalidates destination CRC if copied
  mip0 overlaps its CRC plane; AFBC writes go through gfx. Lack of image
  barriers alone is not evidence of stale CRC for this copy route. References
  have same structure; do not invent a blanket CRC-reset repair.
- Inspected meta_gfx_start/end as a post-copy geometry-corruption lead:
  save/restore shader descriptors, push constants, vb0, vertex-input and sample
  state plus all dynamic state; restore clears VS/FS resource-table addresses
  and dirties VS, FS, VB, DESC_STATE, RENDER_STATE and all set dynamic states.
  vk_meta_draw_rects changes vb0 only and uses nonindexed CmdDraw, so absence
  of saved index-buffer state is not by itself a bug. Compute meta restores
  CS descriptors/push data and marks CS/DESC_STATE/PUSH_UNIFORMS dirty.
  No demonstrated missing restoration yet.
- No tests, builds, device changes or speculative patch. Mailbox212 unchanged.
  Root cause remains unproven. Default barrier and meta-restoration routes now
  have source consistency evidence; actual specialized shader output / GPU
  vertex data and hardware cache behavior remain the main evidence gaps.

## Static continuation: Kraid RA/flow and shadow-copy execution (2026-11-01)

- Kraid ra.rs::RegAlignConstraint::for_op_dst reserves a full register even
  for a sub-word staging result, with even-register alignment for >32-bit
  staging results. The allocation request carries this widened ra_bytes;
  later rewrite keeps staging destination lanes at ssa_bytes. Hardware
  last_use is assigned only to killed32-bit SSA sources, not packed halves.
  No simple half-result sharing/last-use omission demonstrated by this read.
- opt_flow::merge_waits can combine Resource with another wait in one direction;
  encode_v9::encode_flow explicitly promotes that combination to full Wait.
  Barrier also encodes full Wait; ZS plus regular slots becomes Wait0126.
  Thus combined flow bits are not silently truncated to one slot. dce_flow
  removes only Resource/ZS/Barrier waits already satisfied in CFG state;
  generic message slots0..2 are not removed there. Actual generated program
  and v14 execution still unobserved; this is not a compiler correctness proof.
- Followed original constant values through frontend snapshot/indexed+named
  extraction -> Rust bytes.to_vec and owned FunctionConstantValues. GraphicsKey
  and DrawKey::is include SpecializationKey, topology, pass and strides.
  No missing specialization in this cache key found. Exact FC4/FC5 etc values
  of the live fixed_frag variants remain unknown; shader name alone insufficient.
- Extended the reachable variable-mip shadow investigation past CPU version
  invalidation: ShadowCopy::uses declares truth read + shadow write; record
  emits vkCmdCopyImage with GENERAL layouts and planned per-mip regions.
  Inline kept instances: record_kept ends prior render pass, records
  Barrier::everything, held_shadow_copies records copies outside render pass,
  and inserts another Barrier::everything after the copy run before resume.
  First-instance copies go through EncoderCore::shadow_copies/shadow_command;
  ordered encoders use command_named tracking, unordered use ShadowRun guards.
  Therefore a simple absent pre/post barrier at this inline copy site is not
  supported by source. Does not prove PanVK executes the barriers correctly.
- Concrete performance-relevant behavior: Barrier::everything is ALL_COMMANDS
  MEMORY_WRITE -> ALL_COMMANDS MEMORY_READ|MEMORY_WRITE; repeated feedback
  instances can require render-pass breaks, copies and global memory barriers.
  This is a reachable cost mechanism, NOT a measured explanation of low FPS,
  nor permission to remove required synchronization. Need actual instance/
  copy count and timings before an optimization claim.
- No tests, builds, device changes or speculative rendering patch. Mailbox212
  unchanged. Root cause unresolved. Next useful static boundary is PanVK's
  global-memory-barrier cache flushing and fragment/tiler ordering for the
  confirmed copy -> sample route, rather than rechecking CPU shadow invalidation.

## Static continuation: v14 interpolation, uploads and message waits (2026-11-01)

- Read Kraid encode_v9.rs OpLdVarBuf -> isa-v9-v14.xml v14 LD_VAR_BUF and
  LD_VAR_BUF_IMM definitions. Source type F32/F16 encodes2/3; update Store,
  Retrieve, Clobber encodes0/1/3, matching the Valhall ISA definitions. v14
  immediate byte offset is11 bits at8, not the old v9/v10 8-bit field at16;
  encoder permits <2048 on arch>=11. F16 destination staging count is packed
  as ceil(component_count/2), F32 as component_count. No demonstrated field
  mismatch from source reading; no binary/secondary build-artifact inspection.
- Metal vertex Half4 maps R16G16B16A16_SFLOAT. Original std_vert1 AIR returns
  unchanged half4 color plus float2 transformed UV, so source/destination width
  separation in LD_VAR_BUF matters; it is explicitly represented by mem_type
  versus dst_type. This does not prove the actual compiled shader is correct.
- PanVK physical-device source advertises shaderFloat16 on arch>=10; runtime
  enables it and private supportsNativeHardwareFP16 reflects that feature.
  half_chain.rs only holds half operations at float on a profile WITHOUT native
  shaderFloat16. Thus this emulation is not the default v14 route. Exact live
  profile still not dumped; do not attribute polygons to half-chain rounding.
- Followed setVertexBytes -> argument_table::set_bytes -> Uploads::upload:
  immediate copy, per-command-buffer chunks, monotonic aligned used offset,
  InlineBytes retains Buffer+offset+length, binding adds region offset once.
  Completed cleanup takes uploads only after completed handlers. No simple
  inline-byte arena wrap/early reuse seen. This does NOT cover caller-owned
  mutable MTLBuffers or prove DMA/cache visibility on hardware.
- Kraid message_slots.rs computes the first physical-register access deadline
  for each message result, at whole-register granularity (including packed-half
  overlap), and inserts waits before consumption/overwrite. Blocks drain their
  active message slots unless shader end. required_waits.rs additionally waits
  Resource for LD_TILE_RES, Barrier for ordinary color LD_TILE/ST_TILE, and
  ZS+Barrier for blend. Reference BIR va_insert_flow.c explicitly states outbound
  staging operands are latched/stalled by hardware, so tracking only message
  destinations is intentional, not a missing source-register lifetime fence.
  This is source consistency evidence, not proof that all generated waits or
  v14 hardware semantics are correct. No new confirmed root cause.
- Next focused static lead: actual Kraid register allocation/flow coalescing
  of the mixed half-color/float-UV shader and texture fusion; caller-owned
  vertex/index buffer mutation must remain distinct from copied inline bytes.
  No tests, build, device changes or shader replacement. Mailbox212 unchanged.

## Static continuation: actual compiler and HSR enable gate (2026-11-01)

- Followed the actual backend dispatcher, not just its name: build-panvk-native-ios.py
  enables panfrost-rust; pan_use_kraid defaults to ALL. pan_shader_compile calls
  bifrost_compile_shader_nir, but bifrost_nir.c:1499 dispatches that call to
  kraid_compile_variant/kraid_compile_nir. The function name does NOT prove BIR
  compilation. No PAN_USE_KRAID override found in searched X9 launch/build sources;
  process environment / compiled variant has not been captured independently.
- Concrete latent source gap: Kraid compile.rs::write_back_info and final
  pan_shader_update_info do not populate fs.hsr. pan_postprocess_nir zeroes info;
  va_gather_hsr_info is called only in bi_compile_variant. Kraid emits OpLdTile
  for both load_tile_pan and load_tile_res_pan without filling this HSR metadata.
  BIR handles both with BI_OPCODE_LD_TILE (resource form adds wait_resource), so
  the earlier suspicion of a distinct unrecognized LD_TILE_RES opcode is false.
  Local mesa-panfrost reference has the same Kraid omission.
- Crucial reachability gate: csf/panvk_vX_cmd_draw.c:1773 sets allow_hsr_prepass
  only for PANVK_DEBUG(HSR_PREPASS); instance.c parses PANVK_DEBUG, no default
  hsr_prepass. Framebuffer emission gates hsr_prepass_enable with this flag and
  pan_fb_can_pipeline_zs. No hsr_prepass enable found in searched X9 sources.
  Therefore do NOT call the Kraid metadata omission the current blur root cause
  or ship a speculative HSR-disable candidate; default prepass is already off.
  Exact running environment and non-prepass hardware interpretation remain unobserved.
- The DCD pack also reads cfg.z_write_or_stencil before assigning it, but this
  needs coverage-changing + depth/stencil behavior to matter; ordinary blur
  does not establish those conditions. No repair made for this unrelated lead.
- Independent fetch protection: input-attachment lowering records read bits
  before selecting load_tile/load_tile_res; luma reads target0 and writes0, so
  rt_read & rt_written already forbids hsr_can_cull even with missing metadata.
  hsr_can_be_culled has no equivalent read-mask guard, but is not evidence of
  active prepass. PanVK advertises rasterizationOrderColorAttachmentAccess on
  arch>=10; runtime input-fetch passes order all their color accesses, and FPK
  can-kill checks !roa_color. Broad shader output masks are not alone proof of
  incorrectly discarding masked MRT contents (Gallium also separates blend OFF).
- Returned to default Kraid interpolation: opt_var tracks hidden-register
  Store/Retrieve/Clobber via CFG dataflow; schedule.rs has forward RAW/WAW and
  reverse WAR dependencies for that hidden register. No simple scheduler
  reordering omission. lower_fs_inputs uses linked slot byte offsets, forwards
  memory type to OpLdVarBuf, applies component swizzle and noperspective FragW;
  encode_v9 selects source_format F16/F32 separately from destination type.
  Still need examine v14 instruction field encodings / actual shader variant
  and blur half arithmetic; no demonstrated interpolation fault yet.
- Read-only source work, no tests/build/device action or runtime behavior changes.
  Mailbox212 stays staged. Root cause and low-FPS cause remain unresolved.

## Static continuation: shadow invalidation and masked MRT (2026-11-01)

- Followed render/shadows.rs::plan -> EncoderCore::plan_shadows/before and
  shadows_used/after -> ShadowSync. target_uses includes COLOR_ATTACHMENT_WRITE
  for target.level..level+1. ShadowCopies::meets compares backing identity and
  overlapping mip/layer ranges; view_shadow.rs retains base identity/range.
  after loses matching current copies as Lost(Written); before schedules
  Step::Into with CopyCause::Changed. Whole overwrite optimization is gated.
  Thus no evidence for the simple hypothesis that a10-mip feedback shadow is
  copied only once and reused forever. GPU copy execution remains unverified.
- Followed VS/FS layout changes: compile_shaders hands the preceding HW VS
  layout to linked FS; unlinked FS descriptor generation resolves each FS slot
  against current VS by location. fs_desc_dirty explicitly includes VS dirty,
  so a VS change is not omitted from FS descriptor rebuild conditions.
  No confirmed mixed-half/float stale-layout bug from this review.
- Specific unresolved masked-MRT lead: attachment_clear emits outputs0+1 but
  live pipeline masks0 off and1 on. panvk color_attachment_written_mask uses
  shader outputs+attachment location map, whereas blend creation separately
  skips write-disabled/zero-mask attachments. DCD render_target_mask/write_mask
  and HSR use the former. Must establish hardware semantics before treating
  this difference as incorrect: separate blend-mask handling may be intended.
  Local reference mesa-panfrost has same structure; not a missing X9 patch.
  FPK already checks rasterization-order color access; HSR has separate rules.
  Do not blindly mask DCD outputs or disable HSR as a claimed fix.
- Read original fixed_frag AIR destination branch: framebuffer_fetch FC4
  selects passed attachment inputs; sample FC5 instead samples destination
  textures at pixel-center coordinates divided by their dimensions. These
  paths differ from ordinary blur texture3 and need specialized constants to
  distinguish. No new pipeline behavior/build/device action/tests this turn.
- Next static work: masked MRT DCD/HSR semantics and original/reference v14
  descriptors, actual shaders' output/read masks, and fixed_frag specialization
  contract. Root cause still unproven; active device mailbox212 unchanged.

## Static vertex/blur/blend route and reference comparison (2026-11-01)

- User requests static source tracing and reference comparisons, not another
  capture build. No device operation, build, shader substitution or test run.
- Compared AdrenoMetal-20261030-parity/NativeBridgeMetalFrontend.mm draw and
  render-pass forwarding with X9: same primitive/start/count/baseVertex/index
  byte-offset bridge calls, renderTargetWidth/Height forwarding. Reference's
  skipDraw/log hooks are diagnostics, not missing correctness fixes. Private
  full diff: .local/analysis-blur212/frontend-reference.diff.
- Reference bridge/quartzcore_blur_host.mm exercises 32x24 RGBA16Float,
  nonindexed6 vertices, stride40 Float4+Float2+Float4 in slot0, identity matrix,
  one attachment, narrow7 with explicit half uniform88 bytes and scale slot6.
  This does NOT cover live stride48 slot1/half4, indexed count54, BGRA8, mixed
  attachments, fixed_frag specialization or framebuffer fetch. Read source
  only; no new host pass claim. Local UniXNU-gpu is a vendored directory with
  no .git: git -C rev-parse walks into outer repo; NEVER use that as its SHA.
- hackios userland/src/eagl.c:1846-1983 + rc.jobs.vblur describe variable blur
  source/destination feedback and opacity mixing, plus mask edge clamp. That
  route replaces GLES shaders and synthesizes a separate level chain; it is
  not an original Metal shader repair and must not be transplanted wholesale.
- Concrete live correspondence: 211 graph draws51-54 cb76 write tex209 mip1..4
  (384x32,192x16,96x8,48x4), sample SAME tex209 with all10 levels bound. Draw55
  samples tex209 plus tex211 mask to tex207. This is not ordinary ping-pong.
  Runtime texture/attached_reads.rs::attached_in checks bound view mip range
  against target mip, so this shape triggers attachment-read handling;
  snapshot_color rejects it (target mip differs from base_level0).
  usage_shadow.rs::slot_view/new_usage_shadow selects a copied image retaining
  levels, render/shadows.rs::plan calls plan_shadows then shadows_used for
  each instance. Its one_image optimization explicitly requires1 mip, so the
  whole-image clear elision does NOT directly apply to this10-level texture.
  Still need trace shadow version/range invalidation through encoder.rs and
  shadow.rs to prove or exclude stale lower mip reuse. Same texture IDs alone
  do NOT prove an invalid same-subresource GPU feedback operation.
- Vertex route read: translator pipeline/link/varyings.rs matches user keys
  and type, checks uniform-input contract; PanVK emit_vs_attrib uses declared
  attribute offset/format/stride, VS outputs build_attr_buf_write selects
  hardware buffer1 for generic varyings on arch>=12; FS varying descriptors
  select buffer1 too. Compact layout aligns each varying to next-power-of-two
  component byte size; resize pass adjusts16/32-bit IO to linked layout.
  No proven mismatch yet; actual compiled variant/layout still unobserved.
- Mixing route: fixed_frag original AIR conditional fetch/sample flags are
  FC indices4/5, attachment count6, has_attachment0 index13. These function
  variants share shader names: graph name alone cannot identify destination
  fetch vs sampled-color branch. luma_boost reads target0 unconditionally.
  PanVK blend descriptor uses4 components and RT-format conversion; no simple
  BGRA/RGBA exchange or blend-factor forwarding error established.
- Static next focus: shadow mip invalidation for the live10-level chain;
  fixed_frag specialization values and attachment1 preservation; linked VS/FS
  mixed-width varying layout. These are scoped investigations, not root-cause
  claims. Ordinary narrow blur and triangle geometry need their own evidence;
  a variable-blur-only fault cannot explain all reported controls by itself.

## 212 runtime pixel capture decoded; interaction window missed (2026-11-01)

- Latest user log preserved `.local/analysis-blur212/runtime-latest.raw.log`.
  Active212 at3.435s. Decoder ran against actual log: 26 images, all26 complete;
  PNG/RGB, alpha, BGRA and manifest under pixels-latest. Private contact.png
  and active-regions.png inspected. No X9 PIXEL FAILED captured.
- Pixel windows selected cb22 at33.686s, cb34 at35.829s, cb46 at40.000s.
  New narrow15/23, variable blur, narrow11/7 and single-pass3 draws appear
  at151-160s, AFTER all three capture windows were exhausted. This later
  interaction has no pixel readback. Do not claim original polygon defect
  reproduced or disappeared based on these initial captures.
- cb22 source-after-pass tex23 is already magenta across the32x32 grid;
  initial downsample/narrow19 intermediates propagate magenta. It is present
  before these blur outputs/luma_boost, but source-after-pass is not a pre-draw
  snapshot and source provenance is untraced. ASTC error color/heap alias are
  possible leads ONLY, not confirmed causes.
- cb34/46 source grids show wallpaper-like repeated imagery and clock. Pooled
  tex45 64x64 is rendered only in26x26, tex37 114x73 in28x28: colors/images
  outside requested region are old contents, not by themselves corruption.
  Cropped active-region images remove those distracting pooled leftovers.
- cb34/46 final narrow27 tex71 alpha nonzero bbox is(0,0,8,8) in64x64;
  initial downsample tex69 bbox(2,2,46,46). Small coverage alone does not prove
  wrong viewport/UV: full draw geometry/scissors and intended footprint absent.
  Frontend viewport six-double forwarding inspected, no mismatch established.
- Root cause still unresolved. No source behavior changes/build/staging in
  this analysis turn; mailbox remains212. Next capture must cover actual late
  interaction (event/manual trigger or late window), plus viewport/scissor and
  geometry for those exact draws. Do not repeat startup-only pixel capture.
  Diagnostic blits perturb synchronization and cannot validate ordinary FPS.

## Mailbox212 successfully staged after user reconnected adb (2026-11-01)

- Normal updater (no adb retry shim) against x9/out/mailbox212-blur-pixels
  exited0. Root/mailbox preflight passed; `Mailbox committed: 51236864 bytes;
  each written range read back`. Commit marker published successfully.
- Exact log: x9/out/mailbox212-blur-pixels/mailbox-update-reconnected-190207.log.
  Supersedes all failed/disconnected attempts below. Device mailbox now212.
- User boots matching x9/out/mailbox212-blur-pixels/boot.img manually. No boot
  partition write/reboot performed. Open Control Center promptly, retain full
  USB log for at least20 seconds for asynchronous X9 PIXEL output.
- 212 is pixel observability, NOT a proven blur repair or FPS improvement.
  Actual212 runtime/visual result pending. Completed diagnostic source and
  generated212 allowlist committed with this handoff; no extra tests or
  secondary build artifact inspection.

## 211 runtime analyzed; 212 per-pass pixel capture built (2026-11-01)

- User supplied /Users/tosasitill/2.txt from an actual 211 run; private copy
  `.local/analysis-blur211/runtime.raw.log` (8,862 splitlines). 211 active at
  3.695 s. Decoded graph/JSON and original AIR extracts in that directory.
  No captured DeviceLost/panic or SpringBoard/backboardd CRASH. Unrelated
  Siri synthesis/locationd crashes exist; do not describe this as crash-free.
- Graph follows BGRA8 source IOSurface -> downsample -> narrow blur -> fixed
  composite -> original IOSurface; normalized samplers and completed status4.
  No demonstrated format mismatch or bad sampler. Retained slot bindings may
  be unused. 211's <=6 records per shader pair omit writes/variants and do not
  identify the first corrupted pixel. Root cause remains UNPROVEN.
- Original QuartzCore `luma_boost_lph` AIR reads render_target0 (framebuffer
  fetch), samples LUT at texture3, and adjusts current attachment RGB while
  preserving alpha. `attachment_clear_frag_lph` writes zero to two targets;
  captured pipeline enables only color1. Its count4 draw is TriangleStrip
  (Metal type4), not a malformed triangle list. Private graph parser now retains primitive type in JSON/text; raw DRAW
  lines remain authoritative. Do not infer a bug from n4.
- Reviewed input attachment mapping, PanVK tile-load/DCD/HSR read/write masks,
  rasterization-order flags, render_target input references and runtime split
  preservation. No concrete reachable violation established. Historical
  pan172 HFS was read-only; detached /tmp/x9-symbol-root (disk7 ejected).
- 211 active timing batches jobs>=20: 10 batches, medians pre3323/storage348.5/
  start538.5/wait14456.5/retire422/post1735.5 us. Different workload and trace
  logging prevent an FPS or regression comparison. Wallpaper unaddressed.
- 212 adds `X9RenderPixelCapture.inc`: three blur-containing command buffers
  per process, >=2 seconds and >=12 CB IDs apart. After existing encoder ends,
  copy source texture3 of the first blur pass (AFTER that pass, not pre-draw)
  plus stored color0 of subsequent passes into independent shared buffers.
  Original drawing/indexing/shaders/store actions are untouched. Only BGRA8,
  single-sample 2D, nonview/nonmemoryless textures, level/slice/depth0 output,
  Store are eligible. DontCare/color1 are NOT forced to store or read as valid.
  <=24 copies/CB, <=16MiB/image, <=64MiB/CB. Budget skips mean incomplete chain.
- Readback buffers are read only after actual Completed, retained on a separate
  serial dump queue; app completed handlers do not wait for row printing.
  Encoding failures fail the CB; failed completion never dereferences pixels.
  Raw BGRA bytes: <=128x128 intermediates logged whole, other images nearest
  32x32 grid (endpoints included); full-image FNV excludes padding. COPY ties
  each image to CB/encoder/draw range/texture/role/dimensions/pitch; DATA/ROW/END
  carry PID, preventing different processes' CB IDs from being conflated.
  Selected CBs log all draw pairs (global graph bound256); elsewhere pair cap6.
- Diagnostic copies introduce GPU work/barriers and logging overhead; a visual
  change on212 does NOT prove a repair. It may expose a synchronization lead.
  Large-image grids cannot prove all pixels correct or validate full-image FNV.
- Added `x9/tools/decode_render_pixels.py`: reconstruct complete grids as RGB
  PNG + alpha PNG + raw BGRA and manifest.json (including graph draw topology).
  Missing END/rows, conflicting or malformed chunks never produce an image.
  Replay when new log arrives:
  `python3 -I x9/tools/decode_render_pixels.py /Users/tosasitill/2.txt --output
  .local/analysis-blur212/pixels` (one shell line). Decoder not yet exercised
  against actual212 output; no synthetic tests run per user instructions.
- Final normal build `python3 -I x9/build/build.py all --revision 212 --name
  mailbox212-blur-pixels` exit0; signed plugins/package and matching boot.img
  9,420,800 bytes. Build.log preserves initial/follow-up compilations. Local
  only, no CI/tests/secondary artifact inspection. Generated allowlist now212.
- Staging: initial adb absent, then user connected it. First normal updater
  passed root/mailbox preflight and invalidated marker, but adb disconnected
  mid-payload; exit1. Log candidate mailbox-update.log. Connection recovered;
  full normal updater retried with preflight/readback, separate log
  mailbox-update-retry.log also failed after 33MiB; retry2 failed preflight.
  Private read-only adb retry shim attempt failed on short mailbox read. All
  failed attempts kept marker invalid. Superseded by successful staging above.
- Next hardware evidence: boot matching212 only after successful staging, open
  Control Center promptly, keep blur open and retain USB log for at least20 s
  so asynchronous pixel rows finish. Locate earliest bad stored output versus
  source; use graph to isolate pass/shader before another behavioral change.

## 211 render graph capture built and staged; root cause unresolved (2026-11-01)

- User requested continued investigation after 210 failed and introduced color
  errors. No new runtime log provided this turn. Continued from preserved
  .local/analysis-blur209 and blur210 logs and CPU input reconstructions.
- Reviewed render-pass frontend attachment/resolve/load/store forwarding,
  sampler normalization/LOD and C/Rust blend fields; no confirmed mismatch.
  Reviewed runtime shadow/feedback/snapshot ordering and local PanVK barriers.
  No new shader/sync/format workaround applied. 210 deindex remains removed.
- Inspected external reference patches from zenithblue-oss/panvk-kbase-android
  via GitHub API; private copies under .local/analysis-blur-upstream. The v14
  sparse TLS alias issue reports same shader mask 0x3300ff but X9 allocates
  full core-ID-range TLS with independent BO backing (cmd_buffer.c emit_tls),
  no sparse aliases. Not applicable and NOT ported. Bifrost scheduler patch139
  concerns same-tuple FMA descriptor encoding, not v14 Valhall tuple-free ISA;
  not ported. Treat third-party claims as leads, not device evidence.
- New `X9RenderGraphCapture.inc`: after first blur-named draw in a process,
  log <=96 draws and <=6 per shader pair, including subsequent non-blur/final
  composite pipelines. DRAW includes stable CB/encoder IDs and exact shader
  names; PASS records attachment texture IDs/formats/dimensions/mip/slice/
  load/store/resolve; BLEND records enabled/masks/factors/operations; TEX and
  TEXMEM record bound fragment textures, parent/view IDs, IOSurface IDs,
  dimensions/formats/pitch/offset/usage/options/swizzles; SAMPLER includes
  normalization/filter/address/descriptor LOD and setter overrides. COMMIT and
  COMPLETE correlate traced buffers with real submission order/status.
- Texture IDs are monotonic object IDs, not raw pointers. Records describe
  bound state, NOT proof a shader actually reads every bound slot or proof
  rendered pixel content. Graph window is bounded, may omit later writes.
  Capturing the graph does not add GPU commands/barriers/readback or alter
  original shader/format/blend/load/store. Logging and small metadata retention
  can perturb timing; do not measure performance from the initial trace burst.
  Detailed CPU input capture remains limited to blur shaders as in 209.
- Build `python3 -I x9/build/build.py all --revision 211 --name mailbox211-blur-graph`:
  final exit0, signed plugins/package and matching boot.img 9,420,800 bytes.
  Initial sampler-metadata compile failed on uninitialized const char key;
  fixed =0 and final rebuild passed. Local only; no CI/tests/artifact secondary
  checks. Exact build log candidate build.log (all attempts preserved).
- adb became available; updater using private manifest serial against
  x9/out/mailbox211-blur-graph exited0. Root/mailbox preflight passed,
  `Mailbox committed: 51216384 bytes; each written range read back`.
  Exact updater output: candidate mailbox-update.log. No boot partition write
  or reboot. Device mailbox now211; use matching candidate boot.img, not209.
- Next requires user hardware run: open Control Center immediately after UI
  available and retain full USB log including X9 GRAPH records. Reconstruct
  blur-output -> sample -> final IOSurface chain, detect actual format/alias/
  load-store/feedback/order contradictions before changing GPU behavior.
  No root-cause or visual repair claim is warranted yet; 211 is observability.

## 210 ineffective, color regression reported; rolled mailbox back to 209 (2026-11-01)

- Latest /Users/tosasitill/2.txt preserved at
  `.local/analysis-blur210/runtime.raw.log`, decoded inputs in same directory
  (decode.py/decoded.txt), aggregate observations.json. 210 installed/verified
  at 2.456 s, active at 3.296 s. DEINDEX logs through draws=640, all ok=1.
  No GPU DeviceLost/panic or SpringBoard/backboardd crash captured.
- User explicitly reports no improvement and additional wrong colors. Treat
  210 as ineffective and a suspected visual regression, not as a root fix.
  Source deindex hook/method removed and generated allowlist restored from
  original 209 package. No new speculative shader/format modifications.
- Experiment limitations: it changed vertex snapshot/allocation as well as
  indexed versus nonindexed execution; it covered canonical count6 downsample
  blur quads, not count54 downsample draws, variable_blur vertex shaders or
  every final compositing shader. Therefore no global claim that indexing,
  geometry or texture sampling has been ruled out. CPU correctness applies
  only to the captured records. Original control-center triangles may appear
  in final compositing rather than in the captured blur intermediate passes.
- Active batches jobs>=20 median us: pre3025/storage351/start539/wait8799/
  retire368/post1680. Different workload + diagnostic input logging prevents
  frame-rate comparison. No captured blit/framebuffer data proves which pass
  first corrupts the image. ROOT CAUSE REMAINS UNRESOLVED.
- adb was connected. Ran normal updater against original
  x9/out/mailbox209-blur-inputs with private root manifest serial; preflight
  passed, exit0, `Mailbox committed: 51212288 bytes; each written range read
  back`. Log: candidate `mailbox-rollback-from210.log`. Previous staging log
  preserved. No boot partition write, reboot or direct HFS file editing.
- Device mailbox now 209. User must use matching
  `x9/out/mailbox209-blur-inputs/boot.img` (already built), NOT 210 boot.img.
  XNU rollback not yet booted/observed; files210 may remain versioned on HFS
  and are not removed. These are ignored by 209's exact allowlist.
- Next work should follow actual render-pass outputs through final blur
  sampling/compositing (attachment identity, load/store, formats, blending,
  feedback dependencies), locating the first corrupted output. Do not make
  another shader replacement or attribute/cache experiment without stronger
  per-pass evidence. C/Rust NmbColorAttachment field ordering checked and
  agrees; no concrete blend ABI mismatch found. No new build/tests/CI.

## Mailbox 210 staged over adb (2026-11-01)

- User reported adb ready; adb get-state=device. Normal updater against
  `x9/out/mailbox210-blur-deindex` using private root manifest serial exited 0.
  Root/mailbox preflight passed, marker published after payload readback:
  `Mailbox committed: 51212288 bytes; each written range read back`.
  Exact output: candidate `mailbox-update.log`.
- Supersedes 210 pending-staging status below. No boot partition write,
  reboot or direct iOS root modification. User can now boot matching
  `x9/out/mailbox210-blur-deindex/boot.img` and reproduce blurred UI.
- Next evidence: X9 BLUR DEINDEX ok=1 and visual comparison of diagonal/
  triangle defects. XNU installation and experiment outcome not yet observed.

## Mailbox 209 inputs decoded; 210 indexed-draw isolation pending staging (2026-11-01)

- New user /Users/tosasitill/2.txt preserved at
  `.local/analysis-blur209/runtime.raw.log`; installed/verified 209 at 2.498 s,
  active at 3.333 s. Captured 22 original blur draws in backboardd, including
  downsample_4/8, narrow_blur_7/11/15/19/23/27, single_pass_blur_3 and variable
  blur, all lph. No DeviceLost, panic or SpringBoard/backboardd crash captured.
- Decoded raw input in `.local/analysis-blur209/decode.py` -> decoded.txt.
  Captured first six indices are 0,1,2,2,3,0. Vertex buffer1 stride48 has
  position Float4@0, UV Float2@16, auxiliary Float2@24, half4@32. Matrix
  buffer2/texture-transform buffer3 reconstruct ordinary rectangles and affine
  UVs. Example draw2 clips (+/-0.982456,+/-0.972603,0,1), UV range about
  -0.01..1.01 for blur border. Variable blur rectangles extending past clip
  bounds are expected scissored drawing, not spikes; their aux UVs are finite.
  Garbage in UNUSED auxiliary fields of ordinary downsample vertices is not
  evidence of wrong inputs. For count54 draws only first six indices captured.
- CPU inputs for captured canonical rectangles are valid. This does NOT yet
  prove GPU shader/compiler, index fetch, vertex reuse, interpolation or image
  synchronization as the culprit. Root cause still unresolved; do not label
  the following experiment a verified repair.
- 210: controlled deindexing ONLY when original vertex name is exactly
  downsample_blur_vert_lph, Triangle/count6/instance1/baseInstance0, all vertex
  attributes use buffer1, fixed stride48/perVertex/rate1, CPU Shared vertex and
  index buffers, and canonical relative quad indices. Copy six original
  48-byte records into inline vertex bytes, draw six unindexed triangles with
  unchanged pipeline/shaders/uniforms/textures, restore original buffer bind.
  Original AIR has no vertex/instance-ID input, making this transformation
  equivalent for the captured shader. All other draws remain indexed.
  Bounds checks precede copies; failure before encoding falls back. Encoding
  failure fails command buffer, never silently fabricates success. Logs
  X9 BLUR DEINDEX include bounded call counts and result.
- Build: `python3 -I x9/build/build.py all --revision 210 --name mailbox210-blur-deindex`,
  exit 0; signed plugin-package + matching boot.img (9,420,800 bytes) under
  `x9/out/mailbox210-blur-deindex/`, build.log there. Local only, no CI/tests
  or second artifact checks. Shader bytes and wallpaper unchanged.
- adb get-state: no devices. 210 mailbox NOT staged, no device writes/boot.
  Installed device remains 209. Do not boot 210 image alone. When Android adb
  is ready run normal updater with private manifest serial, then user boots
  210 and reproduces same blur scene. If diagonal corruption disappears with
  successful DEINDEX logs, focus on indexed/reused vertex GPU path; if not,
  investigate varying/fragment and render-target paths. Variable-blur vertex
  shader intentionally untouched, so not all blur draws use the experiment.

## Mailbox 209 staged over adb (2026-11-01)

- User said connection ready; adb get-state reports device. Ran normal
  `python3 -I x9/mailbox/update.py x9/out/mailbox209-blur-inputs --serial
  "$X9_SERIAL"` with serial from private root manifest.
- Root/mailbox preflight passed. Updater exit 0:
  `Mailbox committed: 51212288 bytes; each written range read back`.
  Exact output: `x9/out/mailbox209-blur-inputs/mailbox-update.log`.
  This supersedes prior pending-staging status. No flash/reboot/root edits.
- Next: user boots `x9/out/mailbox209-blur-inputs/boot.img` and opens Control
  Center/folder/App Library to capture original blur DRAW/ATTR/DATA input logs.
  XNU plugin 209 installation and root cause still await hardware evidence.

## Blur investigation: WC drift ruled out; input capture 209 pending staging (2026-11-01)

- User reports slight subjective improvement but corruption consistently occurs
  with blurred backgrounds (Control Center, folders, App Library); explicitly
  requests root cause. New /Users/tosasitill/2.txt preserved at
  `.local/analysis-blur-root/runtime.raw.log` (~60 s). Do NOT call root cause
  found or visual defect fixed.
- Live WC CHECK: 638 submissions, 12,841,266 pages checked, restored=0 throughout;
  average check grows to 1103 us. The attribute-drift hypothesis is ruled out
  for this run. Gated that scan behind explicit `x9_wc_check=1` (off by default)
  to remove its per-submit overhead; explicit WC mappings remain unchanged.
- Read original QuartzCore default.metallib from historical pan172 HFS mounted
  read-only. Extracted downsample_blur_vert_lpf/lph and narrow_blur_7_frag_lpf/lph
  AIR into `.local/analysis-blur-root/`; llvm-dis output for lph there.
  Original vertex shader is position*matrix (buffer2) plus UV scale/offset
  (buffer3); fragment samples texture3 using interpolated float2 UV, float2
  offsets/half weights in buffer1 and half EDR scale in buffer6. No authored
  spike geometry. This establishes expected input semantics, NOT actual live
  pipeline selection or a compiler error. Apple shader bytes untouched.
- Reviewed runtime argument-table inline-byte copies, draw forwarding, texture
  shadow/feedback ordering, PanVK vertex/varying descriptions and v14 ISA
  interpolation encoding. No concrete defective forwarding/instruction found.
  Local UniXNU-gpu blur reference and its reverted early-handler experiment
  were read; X9 already delivers completed handlers after actual completion.
  Do not transplant early completion or substitute shaders.
- Added bounded `X9BlurCapture.inc` to Metal frontend. Original pipelines whose
  vertex/fragment name contains `blur`: retain shader names/vertex layout,
  track vertex slots0..3 and fragment buffer1, capture at most two draws per
  shader pair and 24 per process. DRAW includes topology/count and first six
  absolute indices; ATTR records formats/offsets/stride/step/rate; DATA records
  raw hex rows of shared CPU-readable vertex/matrix/uniform data at draw time.
  Inline bytes copied (<=512 captured) when set, matching runtime copy timing;
  private/GPU-only buffers are not mapped for capture. No pixel readback or
  added GPU barriers; bounded logging can transiently perturb timing.
- Built `python3 -I x9/build/build.py all --revision 209 --name mailbox209-blur-inputs`:
  final exit 0, plugins signed/packaged, matching kernel/boot built. Candidate
  `x9/out/mailbox209-blur-inputs/`, build.log there; local only, no CI/tests or
  artifact reinspection. Header now allowlists 209. PanVK unchanged except
  revision identity, Metal contains capture. Historical photo/raw logs private.
- IMPORTANT: adb get-state reports no devices. 209 mailbox NOT staged; no
  device write/boot. Do not boot 209 candidate alone against installed 208.
  Once Android adb is available, run normal mailbox/update.py with private
  manifest serial and keep all preflight/readback. Then user boots candidate
  and reproduces Control Center blur to collect X9 BLUR DRAW/ATTR/DATA.
  Required before root-cause attribution: reconstruct position/UV/matrix for
  the exact original blur draw; distinguish invalid CPU geometry from varying,
  texture/attachment, and shader execution corruption. Existing aggregate
  submit log cannot make that distinction. Scope remains blur first; wallpaper
  not pursued in this milestone.

## Control Center photo: triangle corruption / WC invariant (2026-11-01)

- User provided control-center photo and new /Users/tosasitill/2.txt.
  Preserved `.local/analysis-blur-photo/control-center.png` and runtime.raw.log
  (~175+ seconds). Photo shows dark/light splits along panel diagonals and
  long thin triangles across panels, while many glyphs remain recognizable.
  This suggests draw/vertex/interpolation or stale per-draw data; it does NOT
  identify a specific shader/compiler defect on its own.
- No GPU DeviceLost or PosterBoard CRASH captured. Still visually missing
  wallpaper per user's prior feedback. Do not change wallpaper scope now.
  Active timing batches (jobs>=20) median storage 351 us, pre-cache 4749 us,
  post-cache 3105 us, wait 9633 us. Workloads differ from earlier captures;
  no measured FPS gain claim for last candidate.
- Read Metal draw parameter forwarding, setBytes copies, Vulkan cmd_draw/
  cmd_draw_indexed dispatch, PanVK varying descriptors and v14 interpolation
  lowering. No concrete forwarding bug established. Do not substitute a
  shader or blame optional _blendWithAlphaMaskB0 probing without proof.
- Ported the missing UniXNU NativeAdrenoReassertWriteCombined invariant from
  `.local/x9/work/references/UniXNU-gpu/Adreno740/NativeAdrenoCpu.cpp`:
  before submission, check physical page WIMG for held native WC BOs, restore
  WCOMB only where changed, verify it took effect, fail before GPU exposure
  on invalid descriptor/range or failed restoration. Uses X9 16 KiB pages,
  not reference's 4 KiB pages. Host imports never modified. Existing pmap
  transition is responsible for syncing formerly cacheable lines.
- Logs `X9 RENDER WC CHECK` accumulated checked/restored pages, last restored
  count, average check time and return code. This addresses a real missing
  guard for skipping maintenance; runtime attribute drift is still unproven.
  If restored remains zero, do NOT present this as cause/fix for the photo.
  Checking adds CPU work, included in pre-cache timings; measure overhead.
- Build `python3 -I x9/build/build.py --name mailbox208-wc-reassert` exit 0,
  `x9/out/mailbox208-wc-reassert/boot.img` 9,420,800 bytes. Local build only,
  no CI/additional tests or artifact reinspection. Existing 208 mailbox works;
  no device operations or reboot. NOT hardware verified.
- Next user run should reproduce Control Center and provide WC CHECK restored
  counters plus visual result. If no drift, investigate actual per-draw vertex/
  index/varying behavior with bounded capture rather than more cache guesses.
  Shared imported memory maintenance remains the major CPU cost and must
  retain correctness until a sound submission resource contract is added.

## Blur flicker / frame rate: explicit GPU cache publication (2026-11-01)

- User prioritizes polygon flicker in blurred UI and low frame rate; wallpaper
  still visually missing and should not distract this investigation. Latest
  /Users/tosasitill/2.txt preserved as
  `.local/analysis-render-cache/before.raw.log` (48.373 s).
- Wallpaper alias ready at 0.859 s; no PosterBoard CRASH captured this time.
  User observation remains missing wallpaper; do not call visible wallpaper
  fixed. No further wallpaper changes in this candidate.
- GPU still submits without DeviceLost in captured interval. backboardd grows
  to 866 resources / 591 MiB (209 imports / 98 MiB) at 43.310 s. Late submit
  cache maintenance ~4.5 ms pre + 2.25 ms post; wait ~6–10 ms. All mapped
  imports still scanned each submit. _blendWithAlphaMaskB0 lookup refusal
  persists; direct cause of flicker not proven.
- Found driver calls to clean_dcache64 for command bodies, queue pages,
  resident firmware/data and dirty GPU page tables. Local frozen dependency
  source caches_asm.s branches clean_dcache64 to clean_dcache; coherent
  caches.c implementation is DSB-only. No new binary disassembly/secondary
  artifact checks performed (user preference). Thus do not assert measured
  stale GPU reads or prove that frozen object exactly matches this source.
- Added X9GPUCache.h: explicit DC CVAC + DSB SY on held wired driver memory,
  <=4 KiB runs with preemption disabled through closing DSB, physical input
  translated via phystokv. Replaces those X9 CSF and render page-table calls;
  does not change global cache functions or imported-resource policy.
- GPU PA-range FULL sync now combines actual contiguous physical segments of
  each private 64 KiB storage allocation instead of 16 separate 4 KiB commands.
  Bounds/PA width/segment length validation retained; never covers holes or
  other allocations. Fence sync remains a single held 4 KiB page. Every
  completion/fault/timeout check and GPU flush/AS teardown remains.
- Build: `python3 -I x9/build/build.py --name mailbox208-gpu-cache-publish`,
  exit 0; candidate `x9/out/mailbox208-gpu-cache-publish/boot.img`, 9,420,800
  bytes, build.log in same directory. Local only, no CI/additional tests.
  Existing 208 plugins unchanged, no mailbox restage needed. No device write
  or reboot. This is a cache correctness/overhead candidate, NOT a verified
  fix for visual flicker or achieved frame rate.
- Next: user reproduces the same blurred scene; compare flicker and storage,
  start/wait/cache timings, ensuring no command/fence regressions. If flicker
  persists, investigate render-pass hazards/temporary texture lifetimes and
  WC attribute drift with exact runtime evidence. Larger remaining cache cost
  requires sound resource scoping, not blindly dropping import maintenance.

## Wallpaper fallback ENOENT confirmed and candidate fixed (2026-11-01)

- New user `/Users/tosasitill/2.txt` is the wallpaper-wc run (56.601 s),
  preserved separately at `.local/analysis-wallpaper-wc/runtime.raw.log`.
  17.793 s actual open of /Library/Wallpaper/Collections/Collections.plist
  returns errno 2. First heap scans of CollectionsPoste and PosterBoard
  explicitly report NSInvalidArgumentException: that exact missing file lacks
  a valid `order` value. Other wallpaper collection/resource plists open OK.
  This establishes the immediate abort cause, not why the idiom lookup missed.
- Added before launchd a missing-only symlink from that generic fallback to
  /Library/Wallpaper/Collections/Collections~iphone.plist, reusing the existing
  x9_cryptex_aliases opt-in and writable-native-root gate. Source must be a
  regular file; created/existing link must resolve to its held vnode. Existing
  regular fallback is preserved; conflicting entries are never overwritten.
  Failure is logged without blocking boot. No Apple file bytes replaced.
  Source existence/content on the current device still awaits candidate boot;
  original local pan172 resource was validated during the preceding analysis.
- Rechecked local UniXNU/hackios source for Collections.plist: no matching
  repair. Reused this project's existing verified ObjC resource alias helper.
- Build command: `python3 -I x9/build/build.py --name mailbox208-wallpaper-collections`.
  Initial packaging rejected a new boot argument (>607 bytes); removed it and
  used existing alias opt-in. Final compile/link/package exit 0. Candidate:
  `x9/out/mailbox208-wallpaper-collections/boot.img`, 9,420,800 bytes. Build log
  in same directory. Local build only, no CI run, no additional tests.
- Uses installed mailbox 208; no restaging. No device operations/boot performed.
  On user boot this creates only the absent alias inside the dedicated iOS
  root. Verify `X9 Wallpaper: Collections.plist -> original ... ready`, then
  successful collection open and whether PosterBoard passes this exception.
  DefaultWallpapers lookup/actual wallpaper rendering may expose later issues;
  do not claim visible wallpaper before hardware feedback.
- This run still has 8 PosterBoard crashes before the fix; active submissions
  (>=35 s) median pre-cache 2247 us + post-cache 1712 us, GPU wait 8809 us.
  CoreImage _blendWithAlphaMaskB0 EntryNotFound remains. No DeviceLost/panic
  captured. Different UI workload prevents a WC performance comparison;
  polygon flicker/low FPS remain open, no new shader/sync change in this fix.

## Mailbox 208: blur flicker, performance and wallpaper investigation (2026-11-01)

- User confirms touch/unlock is now OK; reports polygon-like flicker where
  blur is present, low frame rate, and missing wallpaper. New input
  `/Users/tosasitill/2.txt` preserved in `.local/analysis-render208/runtime.raw.log`
  (156.857 s). Do not confuse it with the previous 64 s lockscreen log.
- 28 PosterBoard SIGABRT events, no captured GPU DeviceLost. Per-submit
  medians: pre-cache 3272 us, wait 8842 us, post-cache 2486 us. Display
  windows with >=20 copies: 9.9–27.3 copies/s, median 17.5; NOT panel FPS.
  498 read-only import grants: the preceding prepare kr=2 refusals are
  recoverable permission negotiation, not proof of missing textures.
- Original 20H392 cache, matching symbol UUID checked by existing reader:
  slide 0x212f8000, PosterBoard -> PBFWallpaperKitBridge defaultWallpaperIdentifier
  -> WKDefaultWallpaperManager defaultWallpaperManager; CollectionsPoster
  throws at unslid 0x1b23dcc40 in _loadSystemWallpaperCollections. Exact
  throw: Collections.plist lacks a valid `order` array. File lookup uses
  MGCopyAnswer(GSDeviceName).lowercaseString -> Collections~NAME.plist,
  falling back to Collections.plist. Default wallpaper lookup separately
  uses DefaultWallpapers~NAME.plist. No Apple bytes modified.
- Checked MG hashed dispatch for GSDeviceName (9s45ldrCC1WF+7b6C4H2BA):
  callback 0x19c56ea44 derives class from hw.product, NOT product-name.
  Runtime X9 IDENTITY reports iPhone10,3/D22AP. Thus custom iPhone20 display
  name is NOT established as the cause. Local historical pan172 HFS has
  valid readable ~iphone files/order; it is NOT the live device root.
  Actual path/read failure or corrupt live file remains unknown.
- Local analysis scripts/results under `.local/analysis-render208/`;
  readonly pan172 HFS was mounted at /tmp/x9-symbol-root for analysis.
  Existing historical .ips reports corroborate the same exception site.
  UniXNU's iPadOS17 wallpaper PNG replacement and hackios media injection
  do not fix this iOS16 collection lookup exception; not transplanted.
- Candidate `x9/out/mailbox208-wallpaper-wc/boot.img` (9,420,800 bytes):
  explicitly sets kIOMapWriteCombineCache for native WC BO user views;
  host imports unchanged. This makes the no-maintenance mapping contract
  explicit; cache-attribute drift has NOT been observed, no flicker fix claim.
  References: local UniXNU-gpu Adreno740/NativeAdrenoCpu.cpp and XNU
  IOMemoryDescriptor mapping code. Did not force page attributes on imports.
- Added at most 48 Wallpaper plist open observations from PosterBoard/
  CollectionsPoste and one filtered heap-string scan per process name on
  first abort. Scan budget now bounds attempted reads too (previous loop
  could exceed the intended bound within a large region). No file mutation,
  lookup fallback, shader substitution, or synchronization removal.
- Build: `python3 -I x9/build/build.py --name mailbox208-wallpaper-wc`,
  exit 0 after replacing unavailable strstr calls with a local bounded-by-NUL
  helper. Local build only, built-in checks only; no CI/additional tests.
  Uses existing 208 plugins; no mailbox restaging required. No device online
  over adb, no device writes or reboot. Changes are a mapping correction and
  diagnostic candidate, NOT a completed wallpaper/performance repair.
- Next user hardware run: reproduce blur flicker and collect X9 WALLPAPER
  OPEN plus first STR scan. Determine exact failing plist/path/content before
  changing root. CoreImage _blendWithAlphaMaskB0 lookup in stitchable library
  has no raw entry (194 entries); may be optional fallback probing, not yet
  proven the cause of visual errors. Resource-scoped synchronization remains
  the larger performance direction; do not skip shared imported memory
  maintenance based merely on unchanged CPU-write counters.

## Mailbox 208 lockscreen failure: touch worker exit (2026-11-01)

- User reports lockscreen cannot reach desktop. Input `/Users/tosasitill/2.txt`
  preserved as `x9/out/mailbox208-ca5b8d5/lockscreen-runtime.raw.log`.
- Runtime proves 208 installed/verified at 2.680 s and active at 3.525 s.
  GPU DVFS firmware return 0, reports 858000/572000 kHz. Touch starts at
  19.817 s; at 24.842 s continued-read header is `ff ff` for 50-byte payload,
  then releases contact and prints `stream failed; stopped`. Source returns
  from touch_run and terminates the worker. This explains absent subsequent
  swipe input; underlying SPI/TCM failure cause is NOT established.
- Power-key HID events continue at 45/46/60 s and display/GPU activity
  continues. No SpringBoard/backboardd CRASH or GPU DeviceLost captured.
  PosterBoard and other daemon aborts persist (previously observed shared
  abort path); not established as the cause of this touch loss.
- Added at most three fresh-header retries after message read errors, with
  contact release, partial-frame discard and 20 ms scheduler waits. Only a
  validated, correctly sized touch report resets the error budget. Controller
  reset and invalid payload/status paths retain their existing stop behavior.
  Checked local UniXNU/hackios reference trees: no matching S3910 TCM
  continued-read implementation found. No speculative reset/MMIO changes.
- Built with `python3 -I x9/build/build.py --name mailbox208-touch-recovery`:
  exit 0, `x9/out/mailbox208-touch-recovery/boot.img` 9,420,800 bytes;
  built-in checks only, no extra tests, local build (no CI run). Matching
  plugin allowlist remains 208; mailbox does not need restaging.
- NOT booted/device-verified. User should boot this candidate and swipe;
  capture `validated touch stream resumed` or recovery-budget exhaustion
  and subsequent HID contacts to assess recovery. No device writes/reboot.

## Mailbox 208 built and staged (2026-11-01)

- User requested the latest mailbox build and adb installation. Built current
  source ca5b8d5 with `python3 -I x9/build/build.py all --revision 208
  --name mailbox208-ca5b8d5`; exit 0, plugins signed/packaged, matching kernel
  and boot.img produced. Local build, no CI run. First attempt stopped at
  stale migration symlinks; `python3 x9/build/prepare-local.py` repaired them.
- Candidate: `x9/out/mailbox208-ca5b8d5/`; boot.img 9,416,704 bytes.
  Generated kernel allowlist now names X9GeneralMetal208 and X9PanVK208.
  Build log: candidate `build.log` (including the first failed preflight).
- Ran `python3 -I x9/mailbox/update.py x9/out/mailbox208-ca5b8d5
  --serial "$X9_SERIAL"` using the private root manifest's device identity.
  Root and mailbox preflight passed; updater exited 0 and reported
  `Mailbox committed: 51191808 bytes; each written range read back`.
  Exact updater output: candidate `mailbox-update.log`.
- Mailbox staging is complete; XNU installation/discovery of 208 has NOT
  been observed. No boot partition write or reboot. Next: user boots the
  matching candidate and verifies plugin installation and GPU behavior.

## Build script consolidation (2026-11-01, after the user's dde45d7 migration)

- `build.py plugins --revision N` / `all --revision N`: prepare-local --check,
  PanVK (incremental ninja if configured, else full; `--panvk`), package
  X9PanVK<N>, build X9GeneralMetal<N>, `package-plugins.py` -> `x9/out/<name>/
  plugin-package/` + kernel allowlist; `all` continues with kernel + boot into
  the same directory (a complete candidate for `mailbox/update.py`). Long
  steps stream into build.log.
- `package-plugins.py` restores the pan171-174 packager from git history
  (4 KiB-aligned offsets from 4096, 0755 executables, identity = SHA-256 of
  sorted compact JSON file list, mailbox capacity check); reproduces the 207
  package's mailbox.json and allowlist exactly. `--from-mailbox` replaces the
  removed gen-plugin-allowlist.py.
- Maintained user-space scripts read Xcode/Homebrew from local.json
  (`developer_dir`, new `homebrew`; env DEVELOPER_DIR/X9_HOMEBREW override).
- Moved to legacy/work/x9-metal-render: one-shot prepare-panvk*/
  prepare-shared-cache passes, staged partial PanVK builds, build-render-
  deploy.py, prepare-pan208.py; build-pan206/207-panvk.py -> one
  build-panvk-incremental.py. prepare-local drops dangling links to moved
  scripts.
- mailbox/update.py: root preflight defaults to tools/check_plaintext_root.py
  with root_manifest from local.json (never skipped); pinfile path derived
  from the manifest (same as the old constant).
- Not run here (no Xcode/Rust on this host): only dry-run of build.py and the
  packager check against the 207 package.

## Old workspace cleanup (2026-11-01)

- User explicitly requested deletion of unwanted leftovers after migration. Removed 20 selected entries from the old HelioDarwin-x9-lk-preserve workspace: seven historical root images (including the duplicate of the retained latest image), Rust general-target cache, 11 superseded runtime trees, and legacy xnu_x9/out build output.
- Before deletion, rehashed the retained `.local/x9/work/x9-metal-install-pan172/edited-root.hfs` and matched the migration SHA256; checked retained pan205 source and built boot.img. `prepare-local.py --check` passed after cleanup.
- Actual filesystem free space increased from 12.17 GiB to 55.23 GiB (~43.06 GiB reclaimed; APFS clone sharing means summed file sizes differ). Exact private removal inventory: `.local/old-workspace-cleanup.json`.
- Device partition backups, current local dependencies, source repositories and unrelated work directories remain. No device actions. This supersedes earlier statements that all old workspace originals were left intact.

## Dependency migration and pruning (2026-11-01)

- User requested migration + commit, then explicitly requested retaining only newest useful versions. Local APFS snapshots moved inputs under gitignored `.local/`; original workspaces and partition backups remain intact.
- Pruned six superseded experimental root images, Rust general-target cache, 11 old runtime source trees, obsolete reference build output/history. Local snapshot size fell from ~180 GiB to ~35 GiB (du, shared APFS extents; not physical space reclaimed).
- Retained pan172 edited-root.hfs based on original mtime, successful fsck and verified install logs; copy SHA256 matched original. It is NOT the current phone state. Private cleanup inventory: `.local/migration-cleanup.json`.
- Kernel full build with migrated dxb passed: `x9/out/migration-check/boot.img` 9416704 bytes, Image 18735104 bytes. PanVK reconfigure/rebuild (836 steps) and revision 9999 package/sign passed. Logs in that candidate directory. No CI run or device operations; no hardware claim.
- Maintained userspace scripts use local dependencies and canonical x9/metal/General through symlinks. Restored runtime-source/source-library options, latest-runtime macros and compression link. Default retained runtime is pan205 (used by pan207). Complete Metal Rust rebuild not run after cache pruning.
- Historical helper sources archived under x9/legacy; syntax checked, serial literal redacted, not device-tested. Old differential restore plans require their exact removed baselines and must not be reused with the newer retained root.
- Mailbox ROOT fixed; requires explicit --root-check because original generated check-root.sh was absent. This missing private preflight remains a device-update prerequisite, not bypassed.
- Next: regenerate a verified root preflight before any mailbox deployment; rebuild Metal when needed. See x9/BUILD.md and x9/legacy/README.md. Migration commit: `dde45d7` (sources, build paths, helpers and documentation).

## External dependency audit (2026-11-01)

- Audited local build config and the three user-specified directories; see `x9/EXTERNAL-DEPENDENCIES.md` for paths and script comparisons.
- Confirmed 1069 external frozen XNU link inputs exist under HelioDarwin dxb; current root manifest remains in the old X9 candidate.
- Userspace scripts retain the old relative layout; two imported scripts lack options present in old workspace. Mailbox updater ROOT and legacy check-root.sh path also need migration.
- No builds, CI runs, device actions or file migration performed. Next: consolidate configurable dependencies and userspace/package paths; preserve private artifacts outside git.

## X9 build reproducibility (2026-11-01)

**Current state (read first):** the repo now builds the kernel and boot.img
itself: `python3 -I x9/build/build.py` on the build Mac, with
`x9/build/local.json` copied from `config.example.json`. See `x9/BUILD.md` and
`x9/kernel/README.md`. Commits: `06bcfaf` (pristine pan220 baseline + build
flow), next commit (the four perf/rendering fixes re-merged onto pan220).
Not yet built anywhere: this Linux host has no Xcode; `--dry-run` renders all
54 commands (51 compiles, link, strip, boot-xnu). First real run on the Mac
should be checked for compile errors before flashing.
- Six pan117 objects stay prebuilt (`x9/kernel/prebuilt`), so edits to
  `sweet_early.c`, `sleh.c`, `start.s`, `SweetCPU.cpp`, `prng_random.c`,
  `IOKitBSDInit.cpp` have no effect until a compile command is reconstructed.
- The frozen XNU object tree (`dxb/build/xnu.obj`) is still an external
  input. Its source is `dxb/xnu` = `xnu-8796.141.3` (`1b191cb`) + a working-tree
  diff (87 files) + 240 untracked files (captured in the second archive under
  `xnu/tree1-*`, extension-filtered, so e.g. `config/MASTER.arm64.sweet` is
  missing). Not imported yet.
- First Mac build (`/tmp/build.log`, 2026-11-01, commit 20f001d): 38/51
  compiled, 13 failed, all "file not found" for headers of the depfile-less
  pan111/pan150 objects (x9_ufs/*.h, x9_decmpfs/*.h, vendor lzfse/lzvn
  headers, x9_usb_log_protocol.h, x9_update_map.h, sweet_irq_entropy.h).
  Cause: debug info names them relative to the compile dir
  ("HelioDarwin-x9-lk-preserve/..."), which the collector ignored. Collector
  fixed (+ headers next to the source and in -I dirs for depfile-less
  objects); importer committed as `x9/build/scripts/import-build-inputs.py`.
  Waiting for a third collector run; then import into scratch and add only
  the new headers to `x9/kernel/src` (keep the four edited files).
  Third run (11:07 Mac time): 0 missing; 13 headers added to
  `x9/kernel/src` (11 named relatively by debug info + macro-only
  `lzfse_tunables.h`, `kern_exec_internal.h`). Importer notes about older
  `x9_usb_log_protocol.h` (pan111) and `SweetSurface.h` (pan111/141) are
  harmless: only x9_usb_log.c and SweetSurface/Framebuffer.cpp include them,
  and those used the kept pan150/pan205 copies.
- **First complete repo build (2026-11-01, Mac, commit 976b339):** 51/51
  overlay objects compiled (ccache), link + strip + shim packaging OK,
  `x9/out/x9-976b339/boot.img` (shim-boot.img 9416704 bytes, Image 18735104).
  NOT flashed / NOT hardware-verified. It is pan220 + the four fixes
  (d62cfc7); a pan220-only rebuild (revert d62cfc7) would separate rebuild
  regressions from fix regressions.
- First hardware run of x9-976b339 (`/tmp/2.txt`, USB log, user reports a
  panic): coherency line confirms the new kernel (`capable=0 gpu_side=0
  active=0`). 4 GPU submits per client OK at 7.7/8.6 s, no swaps yet
  (`copies=0`). Log stops mid-line at 15.278 s right after backboardd created
  IOSurface id 3 (1256x2760, 14 MB) and imported it (`X9 RENDER FOOTPRINT:
  phase=import`). No panic text in the USB log. The earlier pan2xx log
  (`/home/ubuntu/2.txt`) stopped at the same point (14.413 s,
  `thermalmonitord sel 78`), so the stop predates the four fixes (guess,
  until the panic string is read). Next: pstore console + disk sidecar of this
  run; optionally flash a pan220-only rebuild (revert d62cfc7) to compare.
- Panic of that run (screen photo OCR from the user): `Kernel data abort`,
  pc 0xfffffe0007bcaddc, esr 0x96000147 (EC 0x25, CM=1 WnR=1, DFSC 0x07 L3
  translation fault), far = x24 = start of a 64 KiB run, x0..x2 = +0x40..+0xc0,
  x16 = +0x10000, x28 = 0xd7c000 (= the 14 MB IOSurface import). Cause
  (inferred from registers + code): pan220's batched cache loop
  (`x9_render_memory_cache_spans`, 0271) runs DC CIVAC with preemption
  disabled; a host import's kernel alias (`createMappingInTask` of task
  memory) can lack PTEs, and the fault cannot be serviced with preemption
  off. pan219 had no preemption-off section. Fixed: `kIOMapPrefault` on the
  alias and a page-touch pass over imported spans before the loop. Rebuild +
  run pending; this is a pan220 bug, independent of d62cfc7.
- Run of x9-4e72465 (`/tmp/2.txt`, ~50 s USB log; user: freezes after a few
  animations, rendering errors). Import panic gone; desktop frames flow.
  Observed:
  - `X9 RENDER timing` per job: start ~0.55 ms, storage ~0.45, retire ~0.35
    (poll_pause works), but pre_cache 4-15 ms + post_cache 4-14 ms, wait
    0.4-13.6 ms, max_job up to 69 ms; worsens over time.
  - `X9 RENDER cache`: pre_bytes_avg grows 27 MB -> 194 MB, post ~188 MB per
    job; mapped spans 16 -> 677. Every job CIVACs the union of ALL
    CPU-exposed client mappings twice (submission ABI has no resource list).
  - backboardd native_bytes peaks 519 MB (client limit 512 MB), drops to
    ~60-80 MB later: churn, not a pure leak.
  - Display: 10-15 copies/s, gpu_defer 26-54 per 2 s (vsync tick finds the
    render owner lock held by long submits). Only single-layer swaps
    (no `X9 COMPOSE`), two black frames from explicit layer removal.
  - `SWEET SCALER: sel 1 in 0/368` twice at ~45 s, now refused.
  Guess: perceived freeze = cache-maintenance-bound submits (20-70 ms) times
  ~2.3 submits per frame plus display deferral; log ends at 50.2 s while
  still running, so a real hang after that is not captured.
  Fix direction needs a decision: per-submission resource list from the
  runtime (ABI + PanVK/bridge + plugin rebuild + new allowlist) vs. other
  ways to avoid full-union maintenance.
- Run after 608d5dc (`/tmp/2.txt`): no panic, kernel alive to 110 s, keys and
  sleep/wake work, but no desktop: backboardd's native GPU memory climbed
  ~60 -> 512 MiB (16 MiB runtime pool blocks per pan204 + ~13.9 MiB
  full-screen textures), the 512 MiB budget refused allocations at 63.8 s
  (also 56 `X9 PANVK PRIV` driver-private refusals), backboardd never
  submitted again (swap_id frozen at 392). GPU memory is not in the task
  footprint (footprint ~125 MB vs native 400+ MB), so iOS memory pressure
  never sees it.
- Fix direction taken (user: "method A", reference UniXNU): UniXNU
  Adreno740 `systemDescriptor()` allocates BOs as
  `IOBufferMemoryDescriptor::inTaskWithOptions(nullptr, InOut|MapperNone|
  Pageable|kIOMapWriteCombineCache)` + prepare(): no kernel mapping, the
  memory entry's WC mode covers pages and every client view, so no CPU
  maintenance. X9 now does the same for X9RenderAllocate (boot-arg
  `x9_bo_wc=0` reverts), skips WC BOs in pre/post maintenance (only imports
  remain), enforces a kernel budget of 1 GiB (`x9_gpu_client_mb`) while caps
  still report 512 MiB (X9RenderResources.c in the 207 plugin rejects a
  larger reported value), and logs `X9 RENDER LIVE` per client every 2 s and
  on refusal (alive/WC/imported/old30s/window alloc+release) to tell a leak
  from a high working set. UniXNU also re-asserts WC page attributes at
  submit (NativeAdrenoReassertWriteCombined) because a view can turn out
  cacheable; not ported yet - watch for stale-content rendering errors.
  This makes option 1 mostly unnecessary (imports ~30 MB remain).
- Run of f977d6a (`/tmp/2.txt`, "keeps scrolling text"): backboardd's 4th
  submit, the first 3-stream job, timed out after 10 s: streams 0/1 done,
  stream 2 stuck mid-buffer (extract 48/64, WAIT=0xc1000000 sync wait,
  BLOCK=3); engine marked uncertain (by design no reset), so every later
  client open is refused, the display tick never gets the owner lock
  (gpu_defer=121/2 s), SpringBoard derefs a nil Metal device (pc 0) and
  crash-loops. The same job succeeded on 608d5dc; only write-combined BOs
  changed. Guess: VM zero-fill of the new WC pages stayed in CPU caches, the
  GPU read stale DRAM in a sync object. Fix: scrub each WC BO once by DC
  CIVAC over its physical pages via phystokv (not flush_dcache64), and log
  the first pages' WIMG (expect 6 = VM_WIMG_WCOMB). Fallback if it recurs:
  boot-arg `x9_bo_wc=0`.
- Run of dcb008b (`/tmp/2.txt`): desktop works, WC confirmed (`wimg=6`).
  Per job pre/post cache 3.1/2.4 ms (was ~10/10), 350-450 MB/job skipped
  as WC, display ~18-19 copies/s; GPU wait 8-14 ms now dominates. GPU ran
  at the boot OPP 390/260 MHz (57 OPPs) - no DVFS policy runs here.
  User reports rendering errors, flicker, blur problems: backboardd's Metal
  device went DeviceLost at 65.5 s (6897 refused command buffers after it):
  PanVK's own mapping table hit X9_RENDER_MAX_MAPPINGS=1024 (`X9 PANVK PRIV:
  bind ... errno=22` comes from its userspace check), PanVK failed the whole
  device; resource count jumps with UI (175 -> 585 -> ~900), ~150-210 of them
  IOSurface imports. What showed afterwards was not GPU-rendered.
- Fixes (not built): kernel capacity X9_RENDER_KERNEL_MAPPINGS=4096 for
  arrays/checks while caps keep reporting 1024 (the 207 plugin's
  X9RenderResources.c refuses a larger reported value); PanVK's own table
  X9_PANVK_MAX_MAPPINGS=4096 (needs a plugin rebuild, e.g. revision 208);
  GPU DVFS: at resident session start query all OPP frequencies
  (CMD_GET_FREQ_BY_IDX=1, TARGET_GPU=1) and CMD_DUAL_COMMIT=9 the fastest at
  or below `x9_gpu_khz` (default 900000), from OnePlus MT6993 gpufreq_ipi.h /
  gpufreq_v2.c; `x9/build/scripts/gen-plugin-allowlist.py <mailbox.json>`
  regenerates kernel/generated/x9_render_external_manifest.h (verified
  identical for 207). Plugin builds read `<workspace>/MaliMetal/X9/General`,
  not this repo; the plugin-package/mailbox.json packager lives in the
  user's Mac workspace, not in git.
- PosterBoard (user asked to fix if possible): crash-loops with SIGABRT
  (ns 2 code 6) every ~10 s from 18 s on, 72 crash lines in `/tmp/2.txt`, no
  own log output. Unslid (minus shared-cache slide) its pc 0x1C2617198 /
  lr 0x1D1BA65F8 and backtrace (0x18DC5C4B8 0x1D1ADEF54 0x1D1ACFA4C
  0x1800D2AFC 0x1D1ADE424 ...) are identical for CollectionsPoster and, in
  the earlier run (slide 0x3ffc4000), KonaSynthesizer/SiriTTSSynthesizer/
  MacinTalkAUSP: one shared abort path, predating the GPU OOM; probably a
  missing service/kernel interface, not GPU. Needs symbolization against
  the 20H392 dyld shared cache on the Mac (not available here).
- User-supplied references for MT6993 kernel behaviour:
  github.com/oppo-source/android_kernel_oppo_mt6993,
  github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_mt6993.
- Option 1 (user chose 2026-11-01): narrow per-submit cache maintenance.
  Findings from metal2vulkan 219f013 + pan194-205 (cloned to the session
  scratchpad, all patches apply):
  - The runtime does NO flush/invalidate for host-visible memory; it relies
    on the kernel ownership contract (memory.rs "Submission cache
    ownership"). Its own CPU writes (staging, argument encoders, descriptor
    arenas, copied host buffers) go into the same host-visible blocks.
  - Memory is pooled: many resources per VkDeviceMemory block; large
    resources and host imports (IOSurfaces) get dedicated blocks.
  - Per-command uses are tracked (`Use<ResourceKey>`, residency.rs, heaps as
    whole ranges; dirty_pages.rs already derives GPU-written host ranges per
    command buffer at commit).
  - vkQueueSubmit is called from ~10 places (command_buffer/mod.rs,
    group.rs batched submits, rerun.rs, device/staging.rs, device/relay.rs
    relay thread, sparse/bind.rs, device/prime.rs, submit_stalls.rs), so a
    thread-local hint is unsafe; a scope must ride on the VkCommandBuffer
    (PanVK private entry point storing ranges on panvk_cmd_buffer, unioned at
    driver submit; any CB without a scope => full fallback).
  - PanVK-internal BOs (CS, descriptors, queries) must stay on full
    maintenance; only runtime-declared blocks may be narrowed.
  Pieces: kernel ABI (new selector/flag + capability bit, backward
  compatible with 207 plugins: userspace checks caps by mask), Mesa PanVK
  patch (cmd-buffer scope + submit), runtime patch (collect used host-visible
  placements per CB, incl. its own CPU-written internal blocks), plugin
  rebuild + new allowlist header. Nothing implemented yet.
- Separate blocker: after a few animations the phone hard-hangs and reboots
  ~10 s later; disk log stops at 68.23 s with no panic text (logger thread
  stopped too). Needs pstore console right after the automatic reboot.
  pstore console of the next freeze (`/tmp/1.txt`, x9-4e72465): panic at
  54.946 s `kmem_alloc(0xfffffe100ab71600, 16384, 0x100021): failed with 3
  @vm_kern.c:178`, caller 0xfffffe0007bf6070. 3 = KERN_NO_SPACE (VA, not
  physical). The map pointer lies just inside the zones range, i.e. a
  dynamically created submap, not the 256 GB data range. GPU use at the time:
  backboardd ~335 MB native, another client ~95 MB, import handles up to 808.
  Symbolized with `nm -n` of x9-4e72465 (`/tmp/sys.txt`; slide 0: the earlier
  pc lands in x9_render_memory_cache_spans+0x32c). Caller =
  `__kmem_failed_panic`; flags 0x100021 = KMA_NOFAIL|KMA_PAGEABLE|
  KMA_PERMANENT, which in xnu-8796.141.3 is exactly `io_map()`
  (osfmk/arm/io_map.c): permanent VA from the fixed 8 MiB io_submap, never
  freed. Root cause: render_submit called `ml_io_map(0x48090000,0x1000)`
  (user doorbell) on EVERY submit -> ~400 jobs exhaust io_submap -> panic.
  (Last log line before the panic: backboardd sequence=360.) Fixed by
  mapping once (also gpu_owned_request's per-request GPU register map).
  Every other ml_io_map call site runs once at init.
- Still required on the Mac for building: `HelioDarwin/xnu/build-dxb/dxb`
  (frozen objects, EXPORT_HDRS, fakeroot, dxb/xnu), Xcode 15, llvm/lld 22,
  and the device root-manifest.json (local.json currently points into the
  pan220 candidate directory).
- Fix merge onto pan220: `poll_pause()` = spin 64x16us, then pan219's 100us
  sleeps x20, then 1ms; replaces pan219 `render_cache_wait()` and also covers
  `idle_as` and the fence wait. Framebuffer/scaler and coherency changes were
  identical-base patches.


- pan220 (`/home/ubuntu/agsbx/pan220-batched-cache-ranges`, user-provided, not
  in git) links 1126 objects: ~1069 from the frozen XNU tree on the build Mac
  (`/Users/tosasitill/HelioDarwin/xnu/build-dxb/.../DEVELOPMENT_ARM64_SWEET`),
  ~39 from 11 older candidates (pan117 incl. `reconstructed-inputs`, 118, 119,
  126, 128, 129, 131, 139, 141, 143, 150), 18 in pan220 itself. Each candidate's
  prepare.py copies the previous one, string-patches files and recompiles only
  a few objects; most compile commands live in ancestor commands.json.
- Repo is behind pan220: pan219/220 already changed `SweetX9CSF.cpp` and
  `SweetX9RenderEngine.inc` (100us cache waits, cache plan/spans). The 4
  edits in `8c29858`/`0fba7b6` were made on older sources and must be re-merged
  on top of pan220 (`SweetFramebuffer.cpp`, `SweetX9CSFResident.inc` are
  identical to pan220, the other two conflict).
- pan220 `packaging-source/x9/tools/pull_disk_log.py` hard-codes the device
  serial as a default; strip it before importing tools into git.
  `root-manifest.json` holds the serial and extent map: keep out of git.
- `x9/build/scripts/collect-build-inputs.py` (run on the Mac) archives, for a
  candidate, every non-XNU linked object with its compile command (exact or
  by object name from ancestors), depfile and workspace deps, plus all
  candidates' recipes, XNU git state, workspace `xnu/patches`, tool versions.
  Dry-run here with `--path-map` only (other candidates absent). Waiting for
  the user to run it on the Mac and hand back the tarball.
  First Mac run (2026-11-01, `/tmp/x9-build-inputs-...-20261101.tar.gz`,
  extracted to `/home/ubuntu/agsbx/inputs-pan220`, not in git): 0 missing,
  6/57 overlay objects without a command (pan117 reconstructed-inputs + start.o
  + SweetCPU.cpo). Findings:
  - `HelioDarwin/xnu` is at 0185 (`513bc9a`) with no X9 edits; but compiles
    include `HelioDarwin/xnu/build-dxb/dxb/xnu/...` (has `bsd/sweet_hfs`), a
    second XNU tree whose state was not recorded yet.
  - prng_random.o, sleh.o, IOKitBSDInit.o come from
    `work/x9-usb-live/recover-build/` (found via DWARF strings, not collected).
  - By-name command matches can point at a later copy (sweet_x9_ufs.o: DWARF
    says pan207, by-name gave pan209). Use depfile/DWARF as source truth.
  - Flags are one template per subsystem (osfmk c, bsd c, iokit c++, pexpert
    c) plus per-object extras (`-DX9U_EXTENT_WRITES=1`, `-include
    sweet_hfs_compat.h`, warning switches). Headers: EXPORT_HDRS, dxb/fakeroot,
    MacOSX14.0.sdk Kernel.framework PrivateHeaders.
  Collector updated (DWARF sources, real XNU tree git state + untracked
  sources, kernel-compile-commands.json); second Mac run requested.

## X9 Metal perf / rendering review (2026-10-31, static source read only)

**Update (same day): fixes 1-2 applied in source, NOT built/booted.** No clang or
XNU tree on this Linux host, so not even a single-file compile was run.
- `SweetX9CSF.cpp`: new `poll_pause()` spins up to 64 x 16us (~1ms) before
  falling back to `sleep_ms()`. Used by `idle_as`, `flush_gpu`,
  `render_page_sync` and the `render_submit` fence wait; during the spin phase
  the fence page is acquired on every poll instead of every 1ms. Deadlines
  and all fence/extract/guard checks are unchanged.
- `SweetX9CSFResident.inc`: `x9_gpu_coherent=1` now logs `capable=/gpu_side=`
  and is refused (`gpu_side_coherency_programmed=false`) until PTE SH,
  MEMATTR and COHERENCY_ENABLE are actually programmed.
- Both edits mirrored in `x9/metal/General/kernel/`.
- Log `/home/ubuntu/2.txt` (user-provided, not committed, ~14s, no GUI frames
  yet): `x9_gpu_coherent=1` was passed but `group0_type=0 active=0`, so the
  coherent skip was NOT active in that run. Submit wall times from log
  timestamps: backboardd seq1 ~100ms, seq2 ~9ms, later ~3-6ms; mediaserverd
  ~4ms each. No `X9 RENDER timing` summary printed (needs >2s of jobs).
**Update 2 (same day): fixes 3-4 applied in `SweetFramebuffer.cpp`, NOT built/booted.**
- X9 compositor: layers stacked bottom-up by index. The bottom drawn layer is
  copied as before; upper layers are blended premultiplied source-over. With
  more than one drawable layer and no in-place panel layer, composition goes
  into a lazily allocated cached shadow (`fX9Compose`, ~14 MB) and is copied
  to the panel once, so blending never reads the WC panel. The shadow is
  seeded from the panel (one slow read) whenever it is stale: after any
  direct-path frame, fade or GPU publication. Single-layer frames keep the
  plain memcpy path (no extra cost).
- 'RGBA'/'ARGB' are converted to BGRA; other 4-byte formats are not drawn and
  logged (`X9 COMPOSE: ... not drawn`). Source rects that differ from the
  destination are nearest-neighbour scaled, only when the source rect is plain
  pixels inside the surface (`X9 COMPOSE: ... scaled`).
- Layer enable mask still ignored on X9: that was a deliberate earlier
  decision for the 20H392 record (see comment above the bzero), not changed.
- `sweetfb_blend=0` restores the old raw opaque copies entirely.
- Scaler user client on X9 now returns `kIOReturnUnsupported` and logs the
  first 16 selectors (`SWEET SCALER: sel ...`). `sweet_scaler_stub=1` restores
  always-success. Other boards unchanged.
- Risks to watch: an upper layer whose alpha channel is not really alpha
  (opaque content with alpha 0) would now disappear; if CoreAnimation treats
  the scaler failure as fatal instead of falling back, use
  `sweet_scaler_stub=1`. Per-pixel blend of a full-screen overlay is scalar C.
Next: build on the macOS host, boot, and compare `X9 RENDER timing`
(`storage_us`/`start_us`/`wait_us`/`retire_us`) and `X9 DISPLAY cadence`
`gpu_defer` against this log.

No device log or build was available in this checkout; everything below comes from
reading `x9/build/gpu-kernel-source` and `x9/metal/General`. Hypotheses still need
the `X9 RENDER timing` / `X9 DISPLAY cadence` lines from a real run to confirm.

Frame time (observed in code):
- `render_submit` (`SweetX9RenderEngine.inc`) runs fully synchronously under the
  global `metal_service_lock`: whole-range CIVAC of every CPU-exposed BO
  (bounding interval of all its GPU mappings) -> `flush_gpu` -> `install_as` ->
  CSG start -> doorbell -> poll fence -> CSG suspend -> `flush_gpu` ->
  `remove_as` -> post-CIVAC. Every Metal command buffer pays this.
- Every poll helper (`flush_gpu`, `idle_as`, completion wait, `stop_mcu`) uses
  `sleep_ms()` = 1 ms `assert_wait_deadline` + `thread_block`, with no short spin
  first. A job that finishes in microseconds still costs roughly 5-8 timer
  sleeps (2x flush, AS update/disable idles, fence poll), plus timer leeway.
- The display tick does `sweet_x9_render_display_try_enter()` (TryLock on the same
  lock); while any submit is in flight the frame is deferred to the next 16.7 ms
  tick (`gpu_defer` counter). Long submits therefore also drop display frames.
- `present()` copies every full-screen layer (1256x2760x4 = 13.9 MB) into the
  write-combined panel mapping on the CPU every frame.
Guess: most of the frame time is the per-submit sleep/flush/AS cycle times the
number of command buffers per CA frame, not shader time.

Rendering errors (observed in code):
- `x9_gpu_coherent=1` (pan209) only skips the CPU CIVAC. GPU PTEs still have
  SH=0 (non-shareable, `attributes()` in `SweetX9CSF.cpp`), MEMATTR index 0 is
  0x88 and COHERENCY_ENABLE (0x304) is only read, never written. If that boot-arg
  is set, CPU and GPU see stale data in both directions. pan184 boot-args do not
  set it; check the args of the run that showed the errors.
- `present()` memcpy's layers opaquely: no alpha blending, no pixel-format check
  or conversion, no scaling (source size taken from the destination rect), and on
  X9 the layer enable mask is ignored (any non-zero surface id is drawn).
- `AppleM2ScalerCSCDriverUserClient::externalMethod` returns success for every
  selector without doing anything, so IOSurfaceAccelerator transfers "succeed"
  and leave the destination stale/blank.

## X9 GPU source handoff (2026-10-31)

- Imported latest available X9 Metal/PanVK sources, XNU GPU patches 0254–0260 and 0263, plus runtime patches pan194–pan205 based on metal2vulkan `219f01365dc4c8eb346816c405b5b17403793764`. The retained kernel source includes pan209 coherency gating and pan212 persistent import mappings. Plugin inputs correspond to pan207; derive plugin identity from a verified allowlist.
- Source integration only: the repository build recipe remains pan184 and must be updated before claiming a reproducible current build. No build or device test ran in this checkout. Later device captures show repeated UI hangs without a captured GPU fault; no claim of a fix or measured performance improvement. No device data, Apple files, root manifests or binaries were included.

# iOS 16 on sweet (Redmi Note 10 Pro, SM7150) - working notes

## Agent handoff / active work (2026-10-15, latest)

### >>> UPDATE 2026-10-18 (read first, then the 10-17 handoff below) <<<

**STATE at end of 2026-10-18 (evening):**
- On the phone: userdata = **image v15** (`/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v15.img/.simg`; = v14 + `/private/var/preferences/SystemConfiguration/preferences.plist` with one network service "USB Ethernet": Interface DeviceName ue0 Type Ethernet, IPv4 Manual 192.168.7.2/24 router 192.168.7.1, DNS 1.1.1.1 8.8.8.8, set "Automatic" as CurrentSet). Fallback image v14 (v12/v13 deleted).
- Kernels: **0078** = all 8 cores (`cpus=8`, cpu6/7 = A76, homogeneous SMP; mkadt now emits 8 CPU nodes; ADT `shim/out/sweet-0078hi.adt`); **0079** = display off/on via IOMFB selector 12 kern_RequestPowerChange(state) (fade over 6 vsyncs, then black; verified: power button gives "SWEET FB: display off/on"); **0080** = `SWEET NET: <proc> route add/delete/change ...` and `... add/delete address ...` trace (built, `shim/out/shim-boot-0080s.img`, NOT booted yet).
- **Internet over USB: not working yet.** Host side is ready (temporary iptables: FORWARD accept for 192.168.7.0/24 + MASQUERADE, comment "ios-sweet"; lost on host reboot). With v15 the phone sent no DNS and no outbound traffic (only mDNS/ARP) -> configd probably did not apply the service (or did, without default route/DNS). Next: boot 0080s on v15, watch `SWEET NET` lines (does configd add 192.168.7.2 / default route on ue0?). If configd ignores ue0 (not an IOKit IONetworkInterface; configd 1241.140.6 source in `research/configd-1241`, SCNetworkInterface createInterface needs IONetworkInterface -> controller (IOMACAddress, SelfNamed) -> bus), add IOKit stand-ins and/or rename the ifnet to en*.
- Live log listener (`nc -ukl 5140`) may still be running, writing `logs/live-0078s.txt`.

**LIVE LOG over USB (0077, boot 0077s): no more recovery round-trips.** Boot an image with `sweetusb=2`, run `tools/sweet-usbnet.sh logs/live-NAME.txt` on the PC: it waits for the phone's CDC-ECM interface, sets 192.168.7.1/24 (sudo) and prints/saves the kernel console the phone streams every 100 ms as UDP to 192.168.7.1:5140 (starts with the last 64 KiB). `ping 192.168.7.2` checks the link. Current image: `shim/out/shim-boot-0077s.img` (kernel 0077, ADT sweet-0069hi, cpus=6, sweetfb_noconsole=1, sweetfb_hz=30, sweetusb=2). The recovery pull (`tools/sweet-log-pull.sh`) is still the way after a panic/hang before USB comes up.

**🎉 USB networking works (boot 0076s, 18:24 local): `ping 192.168.7.2` from the PC answers in ~1 ms, 0 % loss (200-packet burst, 1500-byte frames, 512/1024-byte frames).** 0075: CDC-ECM function (bulk 0x81/0x01, interrupt 0x82 NETWORK_CONNECTION; host MAC 02:00:5e:77:ee:01) + kernel ethernet ifnet `ue0` (legacy output, header strip like if_fake) with 192.168.7.2/24 set from the kernel (proto_plumb + SIOCAIFADDR on an in-kernel socket). 0076: received frames into one cluster mbuf (mbuf_allocpacket gave a first mbuf of length 0 -> panic "Failed mbuf validity check: len -14"). Host side (Linux cdc_ether, interface `enp0s20f0u3u1`): `nmcli dev set IF managed no; sudo ip addr add 192.168.7.1/24 dev IF; sudo ip link set IF up`. Boot image `shim/out/shim-boot-0076s.img` (`sweetusb=2`). iOS itself does not see ue0 yet (configd needs an IONetworkInterface in IOKit).

**USB device mode works (boot 0074s, 17:45 local): the host enumerates the phone at high speed as `1209:0001 ios-sweet "Sweet iOS (USB bring-up)"`.** 0072 read-only probe: ABL leaves the USB30_PRIM GDSC and all 7 USB clocks on, DWC3 v3.20a (GSNPSID 0x5533320a) in device mode (GCTL 0x00112000), halted (DCTL RUN_STOP 0, DSTS 0x00d3de04), event buffer still at ABL's 0xfdfc8000, QUSB2 PHY alive. 0073 (`sweetusb=2`): DCTL soft reset, own event buffer (polled, interrupt masked), ep0 DEPSTARTCFG/SETEPCONFIG/SETTRANSFRESOURCE, setup TRB, RUN_STOP; control-transfer state machine as Linux ep0.c; descriptors: vendor class, no data endpoints. It ran but no event came: **0074** forces the VBUS session in the Qualcomm wrapper (QSCRATCH 0xa6f8800: HS_PHY_CTRL |= UTMI_OTG_VBUS_VALID(20)|SW_SESSVLD_SEL(28), SS_PHY_CTRL |= LANE0_PWR_PRESENT(24), as Linux dwc3_qcom_vbus_override_enable) -> connect + enumeration. Image `shim/out/shim-boot-0074s.img`. Black Phone app: main thread waits in -[VMVoicemailManager fetchAccounts] (sync XPC; voicemail needs the modem).

**Navigation works (boot 0071s on v14, log `logs/boot-0071s.txt`):** swipe up from the bottom edge returns to the home screen; app switcher works. Two fixes: **0070** pid_hibernate (syscall 435 was nosys on the macOS config -> SIGSYS killed SpringBoard in -[SBToAppsWorkspaceTransaction _didComplete] when an app left the foreground; the only CONFIG_EMBEDDED-only entry in syscalls.master); **0071** touches starting within 48 px of an edge carry kIOHIDDigitizerEventFromEdgeTip (UIKit `_UIEventHIDTouchEdgeTypeForHIDEvent` reads the digitizer EventMask 0xb0007 bits 0x800/0x1000/0x2000/0x40000). Diagnosed, not fixed: Settings (Preferences) SIGTRAP in PodcastsUI func_1cdc47c58: force-unwrap of `URL.volumeTotalCapacity` (nil on our HFS+ root). Phone/Photos/Messages show black (MobilePhone launches, no crash) - next: stack samples via boot-arg `sweetsample=MobilePhone,MobileSlideShow,MobileSMS,SpringBoard` (image `shim/out/shim-boot-0071s.img`).

**🎉🎉 MILESTONE 2026-10-18 ~15:30 local: iOS 16.7 SPRINGBOARD HOME SCREEN ON THE PHONE** (boot `shim/out/shim-boot-0069s.img` = kernel 0069 + ADT `shim/out/sweet-0069hi.adt`, image **v14**; log `logs/boot-0069s-v14-HOMESCREEN.txt`). Home screen with a few apps, Settings opens, Control Center / lock screen / notification center work, touch + buttons work, 6 A55 cores (`cpus=6`), `sweetfb_noconsole=1 sweetfb_hz=30`. Known issue: cannot leave Settings (home gesture / AssistiveTouch home do nothing) - to investigate.
What it took today (in order): 0058 HID services (Apple IOHIDFamily-1915 user client), 0059 iPhone X identity (hw.product etc.), 0060 SPMI keys + GENI touch, 0061 1125x2436 presented display, 0062 RPMh L11C touch power + IOPMPowerSource battery, 0063 GENI FIFO packing (Goodix GT9896), 0064 PRIV_WORK_INTERVAL grant (SpringBoard "Failed to create work interval"), 0065 SMP via PSCI (6 A55), 0066 crash map regions, 0067 console mute without cons_ops write, 0068 IOSurface lookup of an already-mapped surface returns base -1 (backboardd snapshot crash), 0069 UFS log flush 10 s. Image v13: CommCenter removed from launchd.plist, BKNoWatchdogs. **Image v14** = v13 + Setup skipped (`/var/mobile/Library/Preferences/com.apple.purplebuddy.plist` SetupDone/SetupFinishedAllSteps true, SetupVersion 11, daemonizeNeedsToRun false; purplebuddy.notbackedup mini-buddy flags false; managedconfiguration.notbackedup LockdownActivationIndicatesCloudConfigurationAvailable false; springboard MCForceShowCloudConfigurationUI false) + mobileactivationd data ark seeded (`/private/var/containers/Data/System/D90C94C4-F94E-4FCF-8A28-A93BE3846D8E/Library/internal/data_ark.plist`, -ActivationState Activated etc.) + MediaToolbox D22/D221/Default SubServerConfiguration StartRecorderServer=false (mediaserverd crash loop: portrait-filter prewarm called NULL GL). **Activation:** ADT /chosen certificate-production-status 0, effective-production-status-ap 0, certificate-security-mode 1, effective-security-mode-sep 1 -> mobileactivationd's native Hactivation branch (Codex `codex/activation/report.md`; alternative 4-byte daemon patch documented there). Fallback image v13; v12 deleted.

**🎉 TOUCH WORKS (boot 0063u on image v13, 2026-10-18 ~13:15 local):** tapping (i) on Setup's Hello opens the device-info sheet; swipe-up is recognised (spinner, then back to Hello - under investigation). Chain: RPMh L11C vote (0062, `bsd/dev/sweet_rpmh.c`) -> GT9896 @0x5d on GENI SE7 (0063 fixed FIFO packing: register address went out byte-swapped) -> sweet_hid_touch -> SweetHIDTouch -> backboardd. Battery icon OK (0062 SweetBattery IOPMPowerSource, static 100 %). **Image v13** (= v12 + CommCenter jobs removed from launchd.plist + `BKNoWatchdogs`=true and `FBLaunchWatchdogExceptions{com.apple.purplebuddy:100}` in /var/mobile/Library/Preferences/com.apple.springboard.plist): Setup no longer killed by the scene-create watchdog (it blocked in CoreTelephony getSubscriptionInfo -> CommCenter, Codex `codex/setup/report.md`; that report also lists how to skip Setup: SetupDone/SetupFinishedAllSteps/SetupVersion=11 in com.apple.purplebuddy.plist + activation check). v9 deleted, fallback = v12. Input latency ~1 min with 1 CPU -> Codex task 11 = SMP via PSCI (`codex/smp/`).

**INPUT WORKS (boot 0060u on a freshly flashed v12, log `logs/boot-0060u.txt`):** power, volume up, volume down reach iOS (PMIC -> sweet_keys.c -> SweetHIDButtons -> backboardd). Power locks/unlocks (screen "off" shows the kernel console because the panel never really turns off). **Device identity fixed: iPhone X** (home indicator, Face-ID-style Setup). Earlier test (0059u, `sweethidtest=15` fake Home presses) advanced Setup from Hello to the language list.
- **0058** HID: `iokit/Kernel/arm/hid/`: IOHIDEventServiceUserClient + IOHIDEventServiceQueue + IOHIDEvent compiled UNCHANGED from Apple IOHIDFamily-1915.140.3 (= exact version of the iOS 16.7 IOHIDLib, clone in `research/iohid-1915`) over our minimal IOHIDEventService (SweetHID.cpp: SweetHIDButtons consumer page 0x0C power 0x30 / vol 0xE9/0xEA / menu 0x40; SweetHIDTouch digitizer collections). C hooks `sweet_hid_key(key, down)` (0 power, 1 vol+, 2 vol-, 3 home) and `sweet_hid_touch(finger, down, x, y)`. HID logs go to IOLog. Boot-arg `sweethidtest=N` = fake Home press every N s.
- **0059** identity: hw.machine/hw.model/hw.target/hw.product from the ADT (macOS-config kernel used to answer "iPad8,6"/"Redmi Note 10 Pro"). mkadt: root compatible `D22AP|iPhone10,3|xiaomi,sweet|...`, model iPhone10,3, target-type D22, `/product` copied from the real D22AP device tree (`research/ipsw-x/d22ap.adt`, extracted from the IPSW, NOT in git), `/buttons home-button-type 2`. Codex report `codex/mg/report.md`: MG ProductType = sysctl **hw.product** (unrecognized -> iPod), HWModelStr = hw.target, HomeButtonType = `/buttons/home-button-type`; disk cache `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist` (keyed by build + UUID only -> stale iPod answers survived; reflashed clean v12 to clear it).
- **0060** drivers by Codex (`codex/input/hw-report.md`): `bsd/dev/sweet_keys.c` (SPMI arbiter v5 observer reads, PON 0x810 bits KPDPWR/RESIN SID0, PM6150L GPIO2 SID4 0xc110; works), `bsd/dev/sweet_touch.c` (GENI SE7 I2C @0xa84000, starts 45 s after boot). Touch result: SE7 fw ok (proto 3) but **0x5d and 0x38 both time out -> touch IC unpowered** (L11C LDO needs an RPMh vote). Codex task 8 = RPMh client (`codex/rpmh/`), task 7 = battery (`codex/battery/`).
- **0061** (CI 39117078444): red status bar "rdar:45025538" = UIKit "No status bar visual provider for screen" (360x800 pt is no real iPhone). AppleCLCD now presents **1125x2436** (iPhone X) and shows the centered 1080x2400; touch mapped the same; ADT canvas 1125x2436. `sweetfb_native=1` reverts.
- Images: `shim/out/shim-boot-0060b.img` (0060 + /buttons ADT `sweet-0060hi.adt`). Next: 0061 image with ADT from current mkadt.

### >>> HANDOFF 2026-10-17 late (read this first) <<<

**STATE: iOS 16.7 Setup.app "Hello" screen is ON THE PHONE** (boot `shim/out/shim-boot-0057u.img` on image v12). Photo `/run/media/dipa/android/ios-sweet-work/MILESTONE-setup-hello-2026-10-17.jpg`. SpringBoard + backboardd + Setup run; nothing crashes in the GUI path. Remaining crash/relaunch loops: installcoordinationd (should be fixed by 0057, unverified), mediaserverd (VirtualAudio removed in v12, unverified), mobileactivationd (~100/boot, not analysed), bluetoothd + fairplayd.H2 relaunch constantly (no hardware).

**What is on the phone now:** userdata = image **v12** (`/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v12.img` + `.simg` + `.sha256`; v9 kept as fallback, v8/v10/v11 deleted or superseded). Kernel **0057** (`xnu/out-0057`, CI `39067532732`). ADT **`shim/out/sweet-0057hi.adt`** (QEMU twin `research/q0057-hi.adt`). Boot image **`shim/out/shim-boot-0057u.img`**. Boot args: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1`.

**Build a phone image** (kernel K, ADT A): `python3 tools/mkpayload.py $K $A shim/out/payload-X.bin --cmdline "$B" && (cd shim && ./build.sh ../shim/out/payload-X.bin && cp out/shim-boot.img out/shim-boot-X.img)` then check `python3 tools/unpack_bootv2.py shim/out/shim-boot-X.img shim/out/check-X && cmp shim/out/check-X/ramdisk shim/out/payload-X.bin`. QEMU smoke test: `python3 tools/mkpayload.py $K research/q0057-hi.adt $S/csq/payloadX.bin --ramdisk $S/csq/root-fork.img --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"` + `PATH=~/swift/usr/bin:$PATH python3 tools/qemu-sweet.py ... --secs 40 --kernel $K --fail-on-panic` (S = scratchpad; root-fork.img lives in the session scratchpad csq/ dir and may be gone in a new session).

**Kernel workflow:** edit `research/bt28/xnu/...` (keep the pre-edit copy in `research/preNNNN/`), syntax check `python3 tools/xnu-cc.py research/run28.log research/bt28 <file>`, make `xnu/patches/NNNN-*.patch` with `diff -u pre bt28 | sed` path rewrite, add new/changed files to `research/check-patch-order.py` list, run it (must say applied + MATCH), commit + push -> GitHub Actions `xnu` builds (~8 min) -> `gh run download <id> -R dipa-studio/ios-sweet -n xnu-kernel -D xnu/out-NNNN`. **Repo is PUBLIC since 2026-10-17** (private macOS runner minutes ran out: "recent account payments have failed"); public = free runners. Commit trailer: only `Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>`.

**Phone workflow (user does buttons, Claude runs commands):**
- Boot test kernel: phone in fastboot -> `fastboot boot shim/out/shim-boot-X.img`. NEVER `fastboot getvar`.
- **Read logs WITHOUT filming:** after a test the user holds Power+VolDown -> fastboot -> `fastboot boot recovery/recovery.img` (OrangeFox, temp) -> wait for `adb shell true` -> `tools/sweet-log-pull.sh /run/media/dipa/android/ios-sweet-work/logs/<name>.txt` (reads the 4 MiB console ring the kernel keeps in the last 1025 blocks of userdata; patch 0047). Then `adb reboot bootloader` brings the phone back to fastboot without the user.
- **Flash a root image:** `fastboot flash -S 512M userdata <img>.simg` (7 min, 25 chunks). After a big flash the Xiaomi ABL fastboot hangs: next `fastboot boot` stalls at "Sending" -> the USER must re-enter fastboot by hand (Power+VolDown). Do not retry commands against the hung ABL. A flash right after `adb reboot bootloader` once failed on chunk 1 (USB protocol error): just retry. User must NOT unplug during flash (once interrupted at 25/25 -> reflash).
- **Edit the phone root without reflashing (same-length only):** find the file's raw offset in the local image (it is one extent; search the image for its first 4 KiB), compare with `adb exec-out dd if=/dev/block/sda34 ...`, then `adb push` changed 4 KiB blocks and `dd of=/dev/block/sda34 seek=... conv=notrunc,fsync`, read back. Recovery kernel has no hfsplus.
- **Pull the whole phone root** (12 min): `adb exec-out "dd if=/dev/block/sda34 bs=1048576 count=16384" > img`; mount ro with udisksctl or `sudo mount -t hfsplus -o loop,ro`. To write into a pulled image: `sudo fsck.hfsplus -fy` first (volume is never cleanly unmounted -> Linux mounts ro even with -o force), then `sudo mount -t hfsplus -o loop,force`.
- Making images: copy latest img with `cp --sparse=always`, mount rw with force, change, `sync; umount`, `sha256sum`, `img2simg`. Keep only the latest image + one fallback on the SSD (/home is ~98% full; big work files live in `/run/media/dipa/android/ios-sweet-work/`, old kernels/images archived in `.../ios-sweet-work/archive/`).

**Diagnostics built into the kernel (all SWEET-only):** persistent console log (0047, boot-arg sweetlog=0 off; flush every ~1 s); `|name| text` copies of /dev/null writes for processes in `sweetnull` (default backboardd,SpringBoard; 0048); `IOK name:` IOKit lookup/open trace for `sweetiolog` (default backboardd,SpringBoard; 0049/0054, 400 lines max); `SWEET Sandbox: proc op N` (0050); `STR ...` heap text dump on SIGABRT/SIGTRAP for `sweetscan` (default backboardd,SpringBoard; 0051/0053) - this is how NSException/BSAssert reasons were found; `SMP name tN pc .. bt ..` user stack sampler for `sweetsample` (default SpringBoard,backboardd; first at 90 s then every 120 s, 40 samples, 16 frames; 0054/0055); `SWEET FB: N swaps` every 1000 swaps; `CRASH name ...` crash reports (0043/0044). Symbolicate cache addresses with `ipsw dyld a2s research/dsc/dyld_shared_cache_arm64 <addr - slide>`; main-binary addresses: addr - mh + 0x100000000.

**Patches added today (0046-0057, all CI-built):** 0046 IOSurface (IOCoreSurfaceRoot) + IOMobileFramebuffer (AppleCLCD on /arm-io/disp0) + arm-io nubs; 0047 persistent log; 0048 /dev/null tee; 0049 IOKit trace; 0050 permissive "Sandbox" MAC policy; 0051/0053 heap text on crash; 0052 RENAME_SWAP via exchangedata on HFS; 0054/0055 sampler, swap counter, AKS selectors 35 (extended device state = 17) and 101; 0056 panel copy from the vsync timer at 10 Hz (sweetfb_hz) instead of every swap; 0057 no config_ext_resolver (ACL group entries resolve without memberd). ADT (tools/mkadt.py): /arm-io/disp0, /chosen/display-scale 3, /chosen/unique-chip-id (synthetic 0x5357454554000001) + chip-id 0x8015 -> IOPlatformUUID A9683613-3704-53F2-95D0-98A8FB574E78. Image v9-v12: /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}; launchd.plist StandardOut/ErrorPath renamed to ...PatX (pointing jobs at /dev/console stops them launching: EACCES); v12: LastSystemVersion=20H392 in plain /var/mobile/Library/Preferences/com.apple.migration.plist (stops the 1-hour data migration that ran every boot), VirtualAudio.plugin moved to /Library/Audio/Plug-Ins/HAL.disabled/.

**Codex helper** (`codex exec --skip-git-repo-check -m gpt-6.1-sol -c model_reasoning_effort=high -s workspace-write -C /run/media/dipa/android/ios-sweet-work/codex -o OUT "$(cat task.md)" < /dev/null`, run detached with setsid nohup; config already defaults to gpt-6.1-sol high). Reports in `/run/media/dipa/android/ios-sweet-work/codex/`: mediaserverd-report.md, installcoordinationd-report.md (-> 0057), ufs-performance.md (ranked UFS speed plan), **ufs/0058-sweet-ufs-faster.patch** (READY, not yet applied: ufslogsecs default 10 s log flush batching, optional ufscache=1 directional cache ops, optional ufswait=1 spin-then-sleep; syntax-checked, host-stub verified; see ufs/README.md). Apply it as patch 0058 after review.

**Claude safety-classifier note:** several messages about QuartzCore renderer selection / writing an HFS reader tool were cut off by an automatic classifier (false positives, user agrees). Keep those topics short or let them be; they are not blockers now (rendering already works).

**NEXT STEPS (in order):**
1. **Input** (Setup stuck on Hello): touch + power + volume as IOHIDEventService/IOHIDDevice services backboardd matches (log shows backboardd matching IOHIDDevice with DeviceUsagePairs and IOService with HIDServiceSupport=true). Phone hardware: Goodix/Focaltech touch over I2C/SPI (check live DT `dtb/`), buttons via PMIC (pon) and GPIO keys. Start with a fake HID service (e.g. power button via a timer or boot-arg) to learn the ABI, then real drivers.
2. **Device identity**: MobileGestalt reports iPod + Home button. Add /product node / product-type (iPhone10,3, D22AP), device class iPhone, home-button-less (Face ID) properties in mkadt; find exact MG keys (MGIOMFBSupport.c, `IODeviceTree:/product` lookups seen in backboardd trace).
3. **Battery**: AppleSmartBattery/IOPMPowerSource style service (status bar shows empty red battery).
4. Apply Codex 0058 (UFS speed), consider cpus>1, check whether installcoordinationd/mediaserverd loops are gone, analyse mobileactivationd.
5. Hide kernel console over the GUI when not debugging: boot-arg sweetfb_noconsole=1 (but then panics are not on screen).

- **🎉 MILESTONE: iOS GUI ON THE PHONE - Setup.app "Hello" screen (2026-10-17 ~21:30 UTC, boot 0057u on image v12).** Photo: `/run/media/dipa/android/ios-sweet-work/MILESTONE-setup-hello-2026-10-17.jpg` (animated "dobar d(an)" hello lettering, status bar "iPod" + battery icon, bottom "Pritisnite Home za otvaranje" = "Press Home to open" (locale hr), (i) button). Rendered by iOS 16.7 SpringBoard/Setup through CoreAnimation -> backboardd -> kernel IOSurface (IOCoreSurfaceRoot) -> kernel AppleCLCD swaps -> ABL panel memory. What made it: data migration finally skipped (v12: `LastSystemVersion` 20H392 in the plain `/var/mobile/Library/Preferences/com.apple.migration.plist`; ADT `/chosen/unique-chip-id` + `chip-id` for IOPlatformUUID), 0057 (no ext resolver -> ACL containers work), plus everything before (0046 display/IOSurface, 0050 Sandbox policy, display-scale 3, containers dirs, RENAME_SWAP, AKS 35/101, 10 Hz present).
  **Missing (next work, in rough order):**
  - **Input: no touch, no power button, no volume buttons** (no HID drivers; backboardd looks for IOHIDDevice / HIDServiceSupport services). Setup is stuck on "Hello".
  - **Device identity: it thinks it is an iPod with a Home button** (status bar "iPod", "Press Home to open"): MobileGestalt has no product/model identity (no /product node, product-type, etc.) - give it iPhone10,3 / home-button-less (Face ID class) identity.
  - **No battery detection** (no battery/charger/PMIC service; status bar battery shows empty/red).
  - Locale came up Croatian (hr) - harmless.
  - Performance: cpus=1, polled UFS (Codex UFS patch in progress: `codex/ufs/`), backboardd presents at 60 Hz.
- **README refreshed (2026-10-17):** read the full notes and updated the public overview through patch 0045, v7 hardware observations and prepared v8 diagnostics. Documents real Apple userspace, the missing display/IOSurface GUI work, limited security services, highmem, persistent userdata root, current tooling and build examples. Documentation only; no kernel build, tests, flashing or phone boot performed.

- **v11 still migrated (boot 0056u, 20:32 UTC, `logs/boot-0056u-v11.txt`).** On iOS kCFPreferencesCurrentHost lands in the plain plist: all DMEnvironment setters (setLastBuildVersionPref, setUserDataDispositionPref, ...) use CurrentHost and the phone's `/var/mobile/Library/Preferences/com.apple.migration.plist` holds DMUserDataDisposition; datamigrator's XPC service runs with `_RoleAccount: mobile`. **Image v12** = v11 + `LastSystemVersion` = 20H392 added to that plain `com.apple.migration.plist` (keeps DMUserDataDisposition, 501:501 0600) + `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` moved to `/Library/Audio/Plug-Ins/HAL.disabled/` (Codex: stops the mediaserverd SIGSEGV loop; untested). **installcoordinationd loop explained (Codex `codex/installcoordinationd-report.md`):** os_crash "Failed to create data directory" EACCES for `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.installcoordinationd/Library/InstallCoordination`: `Library`/`Library/Caches` in the container are ACL-only (uid/gid 0xfffffffe, mode 0000, ACE for group GUID ...012B = gid 299 systemusers); with `config_ext_resolver` the kernel asks an identity resolver (memberd/opendirectoryd) that iOS does not run -> EWOULDBLOCK -> ACE skipped -> EACCES. **Patch 0057**: drop config_ext_resolver from FILESYS_BASE (compatibility UID/GID<->GUID mapping is used instead), likely fixes other ACL-protected containers too. Codex task 3: UFS driver performance design (`codex/ufs-performance.md`).
- **mediaserverd SIGSEGV loop explained (Codex report `/run/media/dipa/android/ios-sweet-work/codex/mediaserverd-report.md`):** the fault (FAR 0x390) is in the HAL plug-in `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio` (ApplyBeamFormerPreference <- VirtualAudio_PlugIn::SetPropertyData <- HALS_AHPPlugIn::ObjectSetPropertyData <- AudioObjectSetPropertyData <- MediaExperience vaemVADSerializationListener): the default virtual audio device ('vdef', VirtualAudioDevice_Default) does not exist (no audio hardware/driver, no /product/audio). Proposed workaround for GUI bring-up: move VirtualAudio.plugin out of /Library/Audio/Plug-Ins/HAL/ in the next root image (untested). v11 reflashed OK after an interrupted flash (USB unplugged at chunk 25/25). Codex task 2: installcoordinationd crash loop.
- **ROOT CAUSE of the endless migration: no IOPlatformUUID (2026-10-17).** datamigrator's `-[DMMigrator isMigrationNeededWithBuildVersion:lastBuildVersion:clientPID:]` gets lastBuildVersion from `-[DMEnvironment lastBuildVersionPref]` = `CFPreferencesCopyValue("LastSystemVersion", "com.apple.migration", kCFPreferencesCurrentUser, kCFPreferencesCurrentHost)` -> a ByHost file. The phone had `/var/mobile/Library/Preferences/com.apple.migration.plist` (only DMUserDataDisposition) and NO `Preferences/ByHost` directory at all. On arm XNU publishes IOPlatformUUID (gethostuuid) only from `/chosen/unique-chip-id` (8 bytes) + `/chosen/chip-id` (4 bytes) (IOPlatformExpertDevice::generatePlatformUUID, called from IOPlatformExpert::start); the SWEET ADT had neither, so every CurrentHost preference write failed. Fix: mkadt adds `unique-chip-id` = 0x5357454554000001 (synthetic) and `chip-id` = 0x8015 -> UUID `A9683613-3704-53F2-95D0-98A8FB574E78` (same SHA1 namespace computation as the kernel). ADT `shim/out/sweet-0057hi.adt`, boot image `shim/out/shim-boot-0056u.img` (0056 kernel + 0057hi ADT, QEMU OK). **Image v11** = v10 + `Preferences/ByHost/com.apple.migration.<UUID upper and lower>.plist` {LastSystemVersion: 20H392} (501:501, dir 0755 files 0600). Flashing v11 2026-10-17 ~20:10 UTC. Codex (gpt-6.1-sol high) analysing the mediaserverd SIGSEGV loop in `/run/media/dipa/android/ios-sweet-work/codex/` (report `mediaserverd-report.md`).
- **v10 did NOT stop the migration (boot 0056n on v10, 19:18 UTC, `logs/boot-0056n-v10.txt`):** datamigrator + migrationpluginwrapper ran again, backboardd drew the progress UI (13000+ swaps in ~5 min). So `com.apple.DataMigration.plist`/LastSystemVersion is not the deciding input. What is known about the decision (datamigrator XPC service `DataMigration.framework/XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator`, strings): `Migration sentinel present. Migration necessary.` (sentinel `/private/var/datamigrator/migrate`, created and removed by datamigrator itself; absent in v10), `System build version changed from %@ to %@. Migration necessary.` / `...unchanged... not necessary`, `Disposition dictionary with build version %@ is out of date relative to current build version %@`, `SyntheticDidUpgrade`, DMUserDataDispositionManager; previous build source not found yet (not in the binary's plain strings; likely in DataMigration.framework's DMUserDataDispositionManager / preferences domain `com.apple.migration`). backboardd calls `DMPerformMigrationIfNeeded` and holds SpringBoard in `checkInAndWaitForDataMigration` until it returns. NEXT: find where the previous build version / disposition dictionary is stored (disassemble DataMigration.framework DMUserDataDispositionManager + datamigrator's isMigrationNeeded path), write it into the image; or get datamigrator's os_log out (OS_ACTIVITY_DT_MODE needs a launchd env change).
- **Data migration never records completion -> skip it (image v10, 2026-10-17).** Boot 0056n (18:16 UTC, `logs/boot-0056n.txt`, 30 min): 10 Hz panel copies, backboardd still presents at ~60 Hz (107k swaps), SpringBoard still in `BKSSystemShellService _checkIn` at sample 14. Every migration run takes 1+ hour on cpus=1 and repeats each boot. DataMigration decides from `/var/mobile/Library/Preferences/com.apple.DataMigration.plist` key `LastSystemVersion` (vs ProductBuildVersion 20H392); the pulled phone root (21:01 local) had NO such file. **v10** = that pulled phone state + fsck.hfsplus -fy (volume was marked not cleanly unmounted -> Linux mounts it read-only even with -o force; fsck found only hard-link counts + minor volume header, repaired) + `com.apple.DataMigration.plist` {LastSystemVersion: 20H392} (501:501 0600). `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v10.img` (+ .sha256, .simg); v8 files deleted, v9 kept as fallback.
- **Repo made PUBLIC (2026-10-17, user request) so GitHub-hosted macOS runners are free:** the 0056 CI run was refused ("recent account payments have failed or your spending limit needs to be increased"). Checked before: no device dumps, serials, IMEI or Apple binaries in the history (.gitignore keeps backup/, dtb/, recovery/, research/ out); commit author emails are public now. 0056 rerun `39056660104` passed -> `xnu/out-0056`, QEMU OK, image `shim/out/shim-boot-0056n.img` booted 18:16 UTC.
- **Boot 0055n (17:16 UTC, ~65 min, `logs/boot-0055n.txt`): migration bar reached 99% in ~30 min (was ~60), then crawled.** Root cause of the slowness: backboardd presents the progress UI at ~58 Hz (**167,000 swaps in ~48 min**) and AppleCLCD copied the full 10 MB frame into the panel on every swap -> ~600 MB/s memcpy on the single CPU (cpus=1), starving everything (polled UFS I/O too). Sampler (16 frames): **SpringBoard waits in `-[BKSSystemShellService _checkIn]`** (synchronous BSXPC call to backboardd, from FBSystemShellInitialize <- SBSystemAppMain) - backboardd holds the system shell check-in until the data migration ends, so the migration is the gate to SpringBoard UI. **Patch 0056**: swap only records the latest swap record; the vsync timer copies it to the panel every 60/sweetfb_hz ticks (default 10 Hz); `SWEET FB: N swaps` every 1000.
- **Boot 0054n (16:53 UTC, ~12 min, `logs/boot-0054n.txt`, patch 0054 sampler):** the data migration runs AGAIN on every boot (it completed once in the 0053s run) - completion is not being recorded. backboardd keeps swapping the progress UI (3700+ swaps in ~10 min, `SWEET FB: N swaps`). Sample during migration: SpringBoard main thread blocked in a synchronous XPC call (`mach_msg2_trap <- dispatch_mach_send_and_wait_for_reply <- xpc_connection_send_message_with_reply_sync <- -[BSXPCServiceConnectionMessage _sendSynchronously:] <- BSXPCServiceConnectionProxy ...`). Unsupported AKS selectors seen: 101 `aks_assert_consume`, 93 `aks_get_internal_info`, 34 `_aks_stash_load` (datamigrator), **35 `aks_get_extended_device_state`** (SpringBoard, chronod, remindd, searchd, symptomsd...). **Patch 0055**: AKS 35 answered like 17 (same DER device state), 101 succeeds; sampler backtraces 16 frames, 40 samples (~80 min).
- **MILESTONE: FIRST iOS PIXELS ON THE PANEL (boot 0053s long run, 15:33 UTC, ~60 min; photos IMG_20261017_183029/183222/183419; log `/run/media/dipa/android/ios-sweet-work/logs/boot-0053s-long.txt`, 407 KB, 1428 flushes).** The iOS data-migration boot screen (Apple logo + progress bar) was shown and the bar advanced to full in ~60 minutes. It is drawn by **backboardd through our kernel display path**: `SWEET SURF: backboardd create id 2/3 1080x2400 fmt 0x42475241 (BGRA) bpe 4 bpr 4352 size 10452992 keys 11`, IOSurface sels 9, 27, 5, 34 and traps 2/3 (lock/unlock), IOMFB SwapBegin (4) / SwapEnd (5, 1132-byte struct) / GetLayerDefaultSurface (3) / 68 / 55, then `SWEET FB: swap N layer 0 surface 2|3 1080x2400 ... a 0,0 1080x2400 b 0,0 1080x2400` alternating (double buffering; only the first 16 swaps are logged). Triggered by `com.apple.datamigrator` (AKS sels 101/93/34 return unsupported) and `com.apple.migrationpluginwrapper`. Probably ProgressUI-style CoreGraphics drawing into IOSurfaces (no GPU used). After the migration: SpringBoard and backboardd stay up (exec once each, no crash), nothing new drawn; crash loops only for mediaserverd, installcoordinationd, mobileactivationd (~108 each in the hour); bluetoothd and fairplayd.H2 relaunch constantly (no hardware). Kernel console text overlays the image (sweetfb_noconsole=1 would hide it).
- **MILESTONE: SpringBoard and backboardd both stay up (boot 0053s on v9, 15:29 UTC, `logs/boot-0053s.txt`).** With /chosen/display-scale = 3: `exec SpringBoard` once, `exec backboardd` once, no CRASH for either; MobileGestaltHelper reads IOMFB size/dot pitch; backboardd opens AppleCLCD (sels 57,15,22,29,33,79,74 as before). No IOSurface creates and no swaps: backboardd keeps polling `IOProviderClass IOAcceleratorES` (-> 0 found) and AGXAccelerator/IOAcceleratorESLegacy - no GPU, so no CoreAnimation renderer yet. Log goes quiet after ~47 s (only 47 flushes): the system idles. NEXT WALL: CoreAnimation rendering without a GPU (CPU path) so the window server produces frames.
- **SpringBoard's invalid mode = scale 0 (boot 0053n, 15:19 UTC, `logs/boot-0053n.txt`, heap scan):** `FBSDisplayMode cannot be configured as requested : <FBSDisplayMode: ...; infxinf@0x (1080x2400/0) 60Hz sRGB SDR>` - pixel size and 60 Hz are fine, scale is 0. libMobileGestalt (MGIOMFBSupport.c, sub_19c560a54) reads `display-scale` (u32, default 0) and `display-rotation` from **IODeviceTree:/chosen**, pixel size via IOMobileFramebufferGetDisplaySize (MobileGestaltHelper sel 8/28 in the log). mkadt now adds `/chosen/display-scale` = 3 (1080x2400 @3x = 360x800 pt). ADT `shim/out/sweet-0054hi.adt` (QEMU `research/q0054-hi.adt`), image `shim/out/shim-boot-0053s.img` = 0053 kernel + 0054hi ADT. Possible catch: MobileGestalt may have cached scale 0 in `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/...`.
- **MILESTONE: backboardd survives and opens the display (boot 0052n on v9, 15:07 UTC, `logs/boot-0052n.txt`).** No `CRASH backboardd`. It walks `IODeviceTree:/arm-io` children, matches IOMobileFramebuffer, `open AppleCLCD type 0 -> 0x0`, calls IOMFB selectors 57, 15, 22, 29, 33, 79 (GetBlock, out 1580/40), 74 (SupportedFrameInfo), 12, 6, 27, 17, 84, 50; opens IOCoreSurfaceRoot (sel 13); looks for AGXAccelerator / IOAcceleratorES / IOAcceleratorESLegacy (none, repeatedly), `IODeviceTree:/product` (none, repeatedly), `/buttons`, aop-smart-cover, IOPMrootDomain. No swaps yet. **SpringBoard now gets past display discovery** and dies (sig 5) in `-[FBSDisplayMode _initWithWidth:height:preferredScale:scaleOverride:refreshRate:gamut:hdr:rotation:virtual:validityCheck:]+580` <- `_initWithCADisplayMode:scale:rotation:` <- `FBSDisplayConfiguration _initWithImmutableDisplay:` <- `FBSDisplaySource initWithDisplay:` <- `FBSDisplayMonitor` (slide 0x14504000): "FBSDisplayMode cannot be configured as requested". Checks: width/height >= 2, preferredScale > 0, scaleOverride >= 0, refreshRate non-zero, valid gamut/HDR/rotation. Suspects: refresh rate 0 (IOMFB timing blocks answered with zeros) and/or scale 0 (MobileGestalt main-screen-scale; no /product node in the ADT). **Patch 0053**: heap text scan also on SIGTRAP and for SpringBoard (default sweetscan list backboardd,SpringBoard) to read the exact mode description.
- **Image v9 (2026-10-17) = v8 + `/private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}` (root:wheel 0755; on iOS these live on the data volume, absent from the rootfs) + launchd.plist with StandardOutPath/StandardErrorPath renamed to ...PatX (the in-place phone fix; daemons' stdout reaches the log via the 0048 /dev/null tee).** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v9.img` SHA256 aa9f9139...6e67, `.simg` 12,145,303,864 bytes; written through `sudo mount -t hfsplus -o loop,force`. **Patch 0052** (`xnu/patches/0052-sweet-hfs-rename-swap.patch`, CI `39042761826` -> `xnu/out-0052`, QEMU OK, image `shim/out/shim-boot-0052n.img`): vn_rename falls back to VNOP_EXCHANGE when VNOP_RENAMEX(RENAME_SWAP) returns ENOTSUP for two regular files on one volume (HFS exchange swaps data and cnode descriptors = swap semantics). Flashing v9 started 2026-10-17 ~15:00 UTC.
- **backboardd EXCEPTION FOUND (boot 0051n, 14:41 UTC, `logs/boot-0051n.txt`, patch 0051 heap scan):** `*** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason: '*** -[NSURL initFileURLWithPath:]: nil string parameter'`. First-throw stack (slide 0x337f0000): `+[NSURL fileURLWithPath:]` <- `-[BSSystemContainerForCurrentProcessPathProvider libraryPath]` <- `cachesPath` <- backboardd main+0x1d858 (StartWindowServer, shader cache path). BaseBoard: libraryPath = `BSSystemSharedResourcesDirectoryForIdentifier` = `BSSystemSharedDirectoryForIdentifier(id)` + "/Library"; that calls `container_system_path_for_identifier` (libsystem_containermanager -> `_container_legacy_lookup` -> containermanagerd XPC), which returned NULL (BaseBoard logs "Error getting shared system container ..."). With 0050 the sandbox denials are gone. containermanagerd.log.0 in the pulled (pre-0050) image: "Did not receive any persona attributes", "Expected to find at least one persona", notify_register_dispatch failed 9; `/private/var/containers` does NOT exist in the root image (on iOS it lives on the data volume). usermanagerd does create kernel personas 99/199/1000, but fails to save `/private/var/keybags/usersession.kb` and `persona.kb`: "Failed to atomically swap writing file: Operation not supported (45)" (HFS has no RENAME_SWAP). NEXT: create `/private/var/containers` (and check what containermanagerd needs for system group containers), fix or emulate RENAME_SWAP on HFS, then re-check backboardd.
- **Disk housekeeping (2026-10-17):** /home was at 4 GiB free. Moved (not deleted) `xnu/out-0010..0044` and old `shim/out/*.img`/`payload-*.bin` (everything before 0046) to `/run/media/dipa/android/ios-sweet-work/archive/{xnu,shim-out}` (6.3 GiB); /home now 8.7 GiB free. Large work files (phone-root.img 16 GiB, logs, tracev3 dumps) live in `/run/media/dipa/android/ios-sweet-work/`. **Patch 0051** (`xnu/patches/0051-sweet-crash-heap-strings.patch`, CI `39041440935`): on SIGABRT of a process in boot-arg `sweetscan` (default backboardd) the crash log prints heap text runs as `STR ...` (to find the NSException reason). Boot 0051n 14:41 UTC.
- **Boot 0049n (14:11 UTC, `logs/boot-0049n.txt`, patch 0049 trace): backboardd's IOKit lookups before it dies** (each run): `IODeviceTree:/arm-io/pearl-sep` -> none, matching ApplePearlSEPDriver -> none, `IOService:/IOResources/AppleKeyStore` open OK, matching IOHIDDevice (DeviceUsagePairs) / `HIDServiceSupport` notifications, `usb-drd,t8027` notification; **never any IOMobileFramebuffer / display lookup** - the exception happens before display detection. launchd (tracev3) shows backboardd's mach lookups denied with `error = 159: Sandbox restriction`: com.apple.mobilegestalt.xpc, com.apple.iohideventsystem, com.apple.system.notification_center, com.apple.logd, containermanagerd, ACCHWComponentAuthService, oscard. libsystem_sandbox uses `__mac_syscall("Sandbox", op, arg)` (ops: 1 init, 2 check, 3 note, 4 container path, 5/6/7/8 extensions, 0x15 check_bulk, ...); with no policy named "Sandbox" every call gets ENOPOLICY. **Patch 0050** (`xnu/patches/0050-sweet-sandbox-policy.patch`): built-in permissive MAC policy "Sandbox" in sweet_amfi.c (mpo_policy_syscall returns 0, container/extension ops ENOENT; logs first use of each op as `SWEET Sandbox: proc op N`).
- **Phone root pulled (16 GiB, 12 min, `adb exec-out dd if=/dev/block/sda34`) to `/run/media/dipa/android/ios-sweet-work/phone-root.img` (loop-mounted ro at /run/media/dipa/iOSRoot).** No .ips crash reports. logd tracev3 (Persist, LZ4 `bv41` blocks, decompressed strings in `ios-sweet-work/tracev3/all.txt`): /private/var/db/uuidtext is EMPTY so only launchd's own messages are readable. Findings: v8 launch failure = `Service could not initialize: posix_spawn(/usr/libexec/backboardd), error 0xd - Permission denied` (xpcproxy exit 78) - /dev/console not openable for the mobile user; backboardd[98] spawned `com.apple.MTLCompilerService` before SIGABRT; **launchd denies many mach lookups with `error = 159: Sandbox restriction`** (mediaserverd -> com.apple.backboard.display.services, remoted, mobiletimerd -> sleepd, identityservicesd -> apsd) and **tccd exits OS_REASON_SANDBOX "profile not found"** - no Sandbox kext/profiles on SWEET; likely cause of many daemon crash loops (next wall after the display). CF's uncaught-exception text never reaches stderr for daemons (no CF handler; libc++abi prints only the type). **Patch 0049** (`xnu/patches/0049-sweet-iokit-trace.patch`): `IOK name: ...` console lines for registry path lookups, child iterators, matching (with result count/class), matching notifications and IOServiceOpen of the processes in boot-arg `sweetiolog` (default backboardd), max 400 lines.
- **CONFIRMED: launchd StandardOut/ErrorPath=/dev/console stops backboardd from launching.** After the in-place rename (boot 0047n-b, log `logs/boot-0047n-b.txt`) backboardd launches again and dies as on v7: sig 6, `___pthread_kill <- abort <- demangling_terminate_handler <- _objc_terminate <- std::terminate <- _dispatch_client_callout <- _dispatch_once_callout <- main+0x9334` (window-server dispatch_once). It never opened AppleCLCD or IOCoreSurfaceRoot (no `SWEET FB/SURF: backboardd` lines) -> throws before/at display detection. **Patch 0048** (`xnu/patches/0048-sweet-devnull-tee.patch`, CI `39037749668` -> `xnu/out-0048`, image `shim/out/shim-boot-0048n.img`): writes to /dev/null by processes named in boot-arg `sweetnull` (default `backboardd,SpringBoard`) are printed as `|name| text`. Boot 0048n (13:48 UTC, log `logs/boot-0048n.txt`): only `|backboardd| libc++abi: terminating due to uncaught exception of type NSException` (CF's reason line goes to os_log, not stderr, despite CFLOG_FORCE_STDERR/OS_ACTIVITY_DT_MODE). Next: pull the whole 16 GiB root from recovery (`adb exec-out dd if=/dev/block/sda34`) to `/run/media/dipa/android/ios-sweet-work/phone-root.img` and read ReportCrash .ips files / logd tracev3 for the exception reason.
- **Persistent log WORKS (boot 0047n on v8, 13:31 UTC; pulled from OrangeFox: 70 KB, 111 flushes, `/run/media/dipa/android/ios-sweet-work/logs/boot-0047n.txt`).** In 3 min **backboardd was never exec'd** (v7 ran it), ~70 bare `exec xpcproxy`, SpringBoard loops on FBSDisplayMonitor (no display), mediaserverd sig 11 loop, locationd, lockdownd sig 5. Suspect the v8 plist (StandardOut/ErrorPath=/dev/console on backboardd). **In-place plist edit from recovery (no reflash):** launchd.plist is one extent at image/partition byte offset 0x1e025f000 (195 blocks; verified bytes identical on phone vs local v8 mount); renamed StandardOutPath->StandardOutPatX and StandardErrorPath->StandardErrorPatX (same length, both jobs) in blocks 68 and 105 via `adb push` + `dd of=/dev/block/sda34 seek=... conv=notrunc,fsync`, read back VERIFIED. Phone now differs from local v8 image in those 2 blocks (files in `/run/media/dipa/android/ios-sweet-work/`). Test boot 0047n 13:38:42 UTC. Recovery kernel has no hfsplus (raw block edits only).
- **Patch 0047: persistent console log (CI `39036478663` -> `xnu/out-0047`, QEMU no panic; phone image `shim/out/shim-boot-0047n.img` = 0047 + sweet-0046hi.adt + 0045n cmdline).** `_cnputs` (all console output: kernel printf + daemons' /dev/console) also appends to a 4 MiB ring in sweet_ufs.c; after the UFS partition is ready and writable (ufsrw=1) a thread writes new ring blocks + header to the LAST 1025 blocks (4 KiB) of userdata every ~1 s and SYNCHRONIZE CACHE (prints `SWEET LOG: console log to the last 4100 KiB ...`). Read: phone to fastboot -> `fastboot boot recovery/recovery.img` (OrangeFox) -> `tools/sweet-log-pull.sh OUT.txt` (adb dd of that area + `tools/sweet-log.py`). Not yet verified on the phone. Panics after the last flush are not in it (screen still shows them). 2x font attempt reverted: XNU's v_scale only scales the progress icon.
- **Boot 0046n on v8 (13:05 UTC, VID_20261017_150522_HSR_120):** phone prints `SWEET SURF: IOCoreSurfaceRoot registered` and `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0x9c000000, default surface 1` (nubs + match work on hardware). Visible userspace: early boot tasks, launchd_cache_loader, `exec SpringBoard`; no `exec backboardd` / `SWEET FB: backboardd` line seen in the readable frames (video blurry). Frame 00:47: lower half of the screen shows OLDER console text (16:01:12) below newer lines -> unconfirmed hint that a swap copied an old snapshot of the panel (CA reads the default/displayed surface = panel memory). SpringBoard still dies in `-[FBSDisplayMonitor _initWithBookendObserver:transformer:]+1160` (sig 5, slide 0x30960000). **Shim: 2x console font** (`shim/loader.c` Video.v_depth scale field = 1) so phone videos are readable; image `shim/out/shim-boot-0046b.img` = 0046n + 2x font.
- **Patch 0046: built-in IOSurface + IOMobileFramebuffer (2026-10-17, offline, NOT phone-tested).** `xnu/patches/0046-sweet-iosurface-iomfb.patch`: new `iokit/Kernel/arm/SweetSurface.{h,cpp}` (IOCoreSurfaceRoot + IOSurfaceRootUserClient: sel 13/0/4/34/35, traps 0-5, wired buffers mapped into the caller, per-client refs) and `SweetFramebuffer.cpp` (AppleCLCD : IOMobileFramebuffer on /arm-io/disp0: sizes, swap = CPU copy of enabled layers into the ABL panel memory, 60 Hz vsync via sendAsyncResult64WithOptions(CanDrop), panel memory published as the default IOSurface, kernel text console paused on the first swap only with `sweetfb_noconsole=1` (default keeps the console: pausing switches it to serial and would hide panics)); SweetPlatformExpert creates nubs for /arm-io children (not gic) attached to the platform expert (IOPlatformDevice::compareName casts its provider to IOPlatformExpert, so nubs under the arm-io nub never matched - first CI build showed that in QEMU). mkadt: /arm-io/disp0 {device_type display-subsystem, compatible sweet,display, canvas-width 1080, canvas-height 2400}. CI `39021572169` -> `xnu/out-0046`. **QEMU** (`research/q0046-hi.adt`, RAM disk init, msgbuf): `SWEET SURF: IOCoreSurfaceRoot registered`, `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0x9c000000, default surface 1`, init alive, no panic. Phone image **`shim/out/shim-boot-0046n.img`** (SHA256 9c4d2fe9..., CI `39024108588` with the review fixes: swap bounds check, zeroed 64-bit async ref, port release, 1.5 GiB wired budget, ref leak/race fixes; QEMU re-checked) = 0046 kernel + `shim/out/sweet-0046hi.adt` + the 0045n cmdline; boot it on image v8 (backboardd/SpringBoard log to console) and film. Expected new console lines: `SWEET SURF: <proc> create id ...`, `SWEET FB: backboardd sel N ...`, `SWEET FB: swap ...`. Open question for the next wall: QuartzCore's IOMFB server builds its renderer from Metal first (`CAMetalContextCreate`), GLES only when enabled; how the CPU renderer gets selected on a device without a GPU is not yet established.
- **IOSurface values ABI (offline RE, `/run/media/dipa/android/ios-sweet-work/iosurface-values.md`) - TODO in SweetSurface (0046 only accepts and drops values, so IOSurfaceCopyValue always returns NULL):** sel 9 SetValue in = {u32 id, u32 pad, IOCFSerialize([value, key]) or ([dict] = merge)}, sel 0xb RemoveValue in = {u32 id, u32 pad, key\0} (empty key = remove all); both out 4 bytes = new generation, client accepts only (gen+2)|1. Kernel after each change: gen = (gen+2)|1 (odd, never 0) -> state+0x20, serialized size (>=1) -> state+0x28. sel 0xa in {u32 id, u32 0, u8 0}, out {u32 ?, u32 gen, serialized non-bulk dict}; too small -> 0xe00002db + needed size (client retries x16). sel 0x1b in 0x88: bulk struct 0x70 @0, u64 valid size @0x70, u64 changed mask @0x78, u32 id @0x80 -> copy masked fields into state+0x30 (bit0 32B @0, bit1 16B @0x20, bit2 8B @0x30, bits3-10 1B @0x38..0x3f, bits11-14 24B @0x40/4B @0x58/8B @0x5c/1B @0x64).
- **IOMFB display matching confirmed (offline RE, `/run/media/dipa/android/ios-sweet-work/iomfb-match.md`):** the framework never matches on "AppleCLCD"; any registered IOMobileFramebuffer subclass works. `iomfb_match_callback` reads only `dfr`/`AppleTV`/`external` (CFBoolean, service itself, no parents); internal main display = all absent/false (type 0, name "primary"). Display count = /arm-io children whose device_type starts with "display-subsystem" (count 0 -> CreateDisplayList returns NULL; more nodes than services -> 30 s wait). IOMobileFramebufferOpen fails only on IOServiceOpen failure; canvas-width/height read as 4-byte CFData from the IOService-plane parent (optional). CopyProperty = IORegistryEntrySearchCFProperty on the service only. Patch 0046 (disp0 node, `external`=false, AppleCLCD : IOMobileFramebuffer) fits all of this.
- **IOMobileFramebuffer swap + notification ABI (2026-10-17, offline).** Swap record = fb+0x18, **0x46c bytes**, zeroed by SwapBegin and sent whole by `kern_SwapEnd` = IOConnectCallStructMethod(sel 5, rec, 0x46c, no output). **SwapBegin** = sel 4, no input, 1 scalar out = swap id (stored at rec+0x50). `kern_SwapSetLayer(fb, layer<4, IOSurfaceRef, CGRect a, CGRect b, flags)`: rec+0x54+4*layer = **IOSurface id**; rec+0x104 bit layer = layer enabled (rec+0x108 bit cleared); rect a (int x,y,w,h, scaled) at rec+0x64+16*layer, rect b at rec+0xc4+16*layer; rec+0x114+4*layer = transform from flags&0xf; flag bits 5/31/6/7/8/9 set bytes rec+0x2ca/0x2cc/0x2e3/0x2e5/0x2e6/0x467+layer. A NULL surface only sets the enable bit with zero id. SwapWait = sel 6 scalars [swap id, timeout, 0].
  - **Notifications**: `IOMobileFramebufferScheduleWithDispatchQueue`/`GetRunLoopSource` create an IONotificationPort and `IOConnectSetNotificationPort(conn, type, port, 0)` (kernel `registerNotificationPort`). `kern_EnableNotifications(fb, type, cb, ctx)` = **sel 72 scalars [NotifyCallbackFuncs[type], fb, type, 0]** -> the kernel keeps an async reference {port, func, refcon=fb} per type and fires it with `sendAsyncResult64`. Types: 0 HotPlug, 1 Power, 3 FrameInfo (args[0] = mask of 31-bit keys, args[1] u32, then values), 4 CRC, **5 Vsync**: 6 args, each a 32-bit half in a u64 slot: (ts lo, ts hi), (t2 lo, hi) passed through the time-ingest helper, (t3 lo, hi); the framework then calls the client's vsync callback(fb, ts, t2', t3, ctx). Vsync callbacks are dropped while the notification port has queued messages.

- **IOSurface.framework user-client ABI reversed (2026-10-17, offline, step 2 of the GUI plan).** `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image IOSurface` (selector scan script: scratchpad `selscan.py`).
  - Connect (`_iosConnectInitalize`, pthread_once): `IOServiceGetMatchingService(IOServiceNameMatching("IOCoreSurfaceRoot"))`, retried with sleep(1) up to 241 times (a missing service stalls every client ~4 min, no crash); `IOServiceOpen(type 0)`; then **sel 13** (no inputs, struct out 0x18 bytes = `_ioSurfaceLimits`) and requires u16 at out+0x16 == 1 (ABI version); out+0 = offset/plane-base alignment mask, out+4 = bytes-per-row alignment mask (used as (x+m)&~m), +0..+0x14 also copied into IOSurface remote-method init, otherwise logs and closes the connection.
  - **Create = sel 0**: scalar in [parent id or 0], struct in = `IOCFSerialize(props, kIOCFSerializeToBinary)` (OSUnserializeBinary format; keys IOSurfaceWidth/Height/BytesPerRow/BytesPerElement/PixelFormat/AllocSize/PlaneInfo/CacheMode/IsGlobal/...), struct out **0xa68 bytes** ("lock result"). **Lookup = sel 4**: scalar in [id], same struct out. LookupFromMachPort = sel 0x22 (scalar in [port name]); CreateMachPort = sel 0x23 (scalar in [id, options], scalar out [port name]).
  - Lock result (copied to client+0x68, size 0x68 + planes*0x80, min 0xe8): 0x00 u64 base address **mapped in the caller's task**; 0x08 u64 task VA of a per-surface shared state page (+0 u64 registry id, +8/+0xa u16 bound accel ids, +0xc u32 seed, +0x14 u8 tiled, +0x20 u32 value generation, +0x24 YCbCr matrix, +0x28 u32 non-bulk values size, +0x2d u8 displayable, +0x2e u8 sysmem-only, +0x30 0x70 bytes bulk attachments); 0x10 u64 task VA of timestamp page (4 u64 indexed timestamps, +0x20 detach code); 0x18 u32 surface id; 0x1c u32 alloc size; 0x20 width; 0x24 height; 0x28 bytes per row; 0x2c buffer offset; 0x30 pixel format; 0x34 u32 (dcache flush length); 0x38 plane count; 0x40 u16 bytes/element; 0x42 u8 element width; 0x43 u8 element height; 0x44 cache mode; 0x4c u8 subsampling; 0x4d u8 prohibit-use count; 0x50 parent id; 0x54 slice count; 0x58 u64 trace id; 0x60 u64 protection options; planes[n] at 0x68+n*0x80 (+0 width, +4 height, +8 bytes/row, +0xc offset, +0x14 size, +0x18 u16 bpe, +0x1a/+0x1b u8 element w/h, +0x1c u8 component count, +0x1d address format, +0x54 u8 compression type (non-zero -> no CPU base address)).
  - **Traps** (IOConnectTrapN -> `IOUserClient::getTargetAndTrapForIndex`): 0 IncrementUseCount(id, category), 1 DecrementUseCount(id, category), 2 **Lock**(id, options, &seed_out), 3 **Unlock**(id, options, &seed_out), 4 Release(id), 5 Release after a failed create/lookup setup (id).
  - Other selectors: 5 SetYCbCrMatrix, 6 WrapClientImage, 7 WrapClientMemory, 9 SetValue (serialized), 0xa UpdateNonBulkValueCache (struct in 9), 0xb RemoveValue, 0xc BindAccel, 0x11 SetSurfaceNotify (async), 0x12 RemoveSurfaceNotify, 0x13 Log, 0x14 SetPurgeable, 0x15 SetOwnership, 0x16 SetTiled, 0x18 SetTimestamp, 0x19 GetTileFormat, 0x1a CopyDataValueBytes, 0x1b SetBulkAttachments (struct 0x88), 0x1e/0x2e-0x31 (via outlined helpers), 0x1f SetCompressedTileDataRegionMemoryUsed, 0x20 GetGraphicsCommPageAddress, 0x21 SetIndexedTimestamp, 0x24-0x26/0x2d IOSurfaceAccelerator, 0x27-0x29 shared events, 0x2a SignalEvent, 0x2b Set/InitDetachModeCode, 0x2c SetOwnershipIdentity, 0x32-0x34 data properties.
  - => SWEET kernel service: class/name **IOCoreSurfaceRoot** + IOSurfaceRootUserClient; minimum = sel 13, 0, 4, 9/0xa/0xb (values), traps 0-5, 0x22/0x23 later for cross-process surfaces (CA sends surfaces to backboardd by mach port). Memory: IOBufferMemoryDescriptor per surface, mapped into each task with createMappingInTask; ids from a global table.

- **Image v8: daemon logs on the console.** `tools/patch-launchd-plist.py IN OUT LABEL...` rewrites launchd.plist (binary, plistlib): renames every `_PanicOnCrash` to `_PanicOnCrasX` and gives the listed jobs `StandardOutPath`/`StandardErrorPath` `/dev/console` (devfs console is 0622, writable by mobile) plus `OS_ACTIVITY_DT_MODE=YES` (os_log mirrored to stderr) and `CFLOG_FORCE_STDERR=1`. v8 = v7 + that plist for com.apple.backboardd and com.apple.SpringBoard, written through the mount (796310 bytes, root:80 0644), SHA256 in `ios16-root-20H392-v8.img.sha256`, `.simg` ready; boot with `shim/out/shim-boot-0045n.img`. Goal: see backboardd's uncaught exception text and the window server's display probing. ODDITY (unresolved): the v5 raw-image rename at offset 0x1e025f000 is present in the v5-v7 image bytes, but the plist read through a Linux mount of v7 still had the original `_PanicOnCrash` keys; filefrag shows the file as one extent. Never patch file contents through raw offsets again; write through the mount.
- **IOMobileFramebuffer.framework reversed (step 1 of the GUI plan).** `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image IOMobileFramebuffer`.
  - Display discovery (`IOMobileFramebufferCreateDisplayList` -> `getDisplayListNumber`): iterates children of `IODeviceTree:/arm-io`, counts nodes whose `device_type` is "display-subsystem" or "ext-display-subsystem" (or `compatible` "paravirtualizedgraphics,gpu"); then `iomfb_populate_display_infos` adds an IOServiceAddMatchingNotification(IOServiceFirstMatch, **IOServiceMatching("IOMobileFramebuffer")**) and runs the run loop up to 30 s until that many services matched. `iomfb_match_callback` reads service properties `dfr`, `AppleTV`, `external` (CFBoolean) and names displays (external ones "%s-%d").
  - `IOMobileFramebufferOpen(service, task, type, &fb)`: `__ioMobileFramebufferAlloc` (CFPreferences com.apple.iokit.IOMobileGraphicsFamily: canvas_width/height, override_display_width/height, override_panel_width/height, benchmark, enable_ktrace), `IOServiceOpen(service, task, type)` (GetMainDisplay passes type 0), `getEDTProperty` reads `canvas-width`/`canvas-height` from the service's IODeviceTree parent, then fills a table of `kern_*` function pointers.
  - User client selectors (`kern_*` -> IOConnectCall*): 3 _kern_GetLayerDefaultSurface[ScalarMethod_stub]; 4 _kern_SwapBegin[ScalarMethod_stub]; 5 _kern_SwapEnd[StructMethod_stub]; 6 _kern_SwapWaitWithTimeout[ScalarMethod_stub] _kern_SwapWait[ScalarMethod_stub]; 7 _kern_GetID[ScalarMethod_stub]; 8 _kern_GetDisplaySize[ScalarMethod_stub]; 10 _kern_SetTVOutMode[ScalarMethod_stub]; 11 _kern_SetWSSInfo[ScalarMethod_stub]; 12 _kern_RequestPowerChange[ScalarMethod_stub]; 13 _kern_EnableStatistics[ScalarMethod_stub]; 14 _kern_SetFlags[ScalarMethod_stub]; 15 _kern_SetDebugFlags[ScalarMethod_stub]; 16 _kern_SetTVOutSignalType[ScalarMethod_stub]; 17 _kern_SetGammaTable[StructMethod_stub]; 18 _IOMobileFramebufferIsMainDisplay[ScalarMethod_stub]; 19 _kern_SetWhiteOnBlackMode[ScalarMethod_stub]; 20 _kern_SwapSignal[ScalarMethod_stub]; 22 _kern_SetDisplayDevice[ScalarMethod_stub]; 23 _kern_SetDigitalOutMode[ScalarMethod_stub]; 24 _kern_GetHDCPDownstreamState[StructMethod_stub]; 25 _kern_GetDigitalOutState[ScalarMethod_stub]; 26 _kern_printDisplayRegs[ScalarMethod_stub]; 27 _kern_GetGammaTable[StructMethod_stub]; 28 _kern_GetDotPitch[ScalarMethod_stub]; 29 _kern_GetDisplayArea[StructMethod_stub]; 30 _kern_EnableDisableDithering[ScalarMethod_stub]; 31 _kern_SetUnderrunColor[ScalarMethod_stub]; 33 _kern_EnableDisableVideoPowerSavings[ScalarMethod_stub]; 34 _kern_SetVideoDACGain[ScalarMethod_stub]; 35 _kern_SetLine21Data[ScalarMethod_stub]; 41 _IOMobileFramebufferEnableMirroring[ScalarMethod_stub]; 42 _IOMobileFrameBufferGetMirroringCapability[StructMethod_stub]; 43 _IOMobileFramebufferSetRenderingAngle[StructMethod_stub]; 44 _IOMobileFramebufferSetMirrorContentRegion[StructMethod_stub]; 47 _kern_HDCPSendRequest[ScalarMethod_stub]; 48 _kern_HDCPGetReply[ScalarMethod_stub]; 49 _kern_SurfaceIsReplaceable[ScalarMethod_stub]; 50 _kern_SetBrightnessCorrection[ScalarMethod_stub]; 51 _kern_SetColorRemapMode[ScalarMethod_stub]; 52 _kern_SwapCancel[ScalarMethod_stub]; 53 _IOMobileFramebufferSetCanvasSize[ScalarMethod_stub]; 54 _IOMobileFramebufferGetMatrix[Method_stub]; 55 _IOMobileFramebufferSetMatrix[Method_stub]; 56 _IOMobileFramebufferKernelTests[StructMethod_stub]; 57 _kern_GetColorRemapMode[ScalarMethod_stub]; 66 _kern_SPLCSetBrightness[ScalarMethod_stub]; 67 _kern_SPLCGetBrightness[ScalarMethod_stub]; 68 _kern_SetParameter[Method_stub]; 69 _kern_SwapSetColorMatrix[StructMethod_stub]; 72 _kern_DisableNotifications[ScalarMethod_stub] _kern_EnableNotifications[ScalarMethod_stub]; 73 _kern_ChangeFrameInfo[ScalarMethod_stub]; 74 _kern_SupportedFrameInfo[Method_stub]; 75 _kern_FactoryPortal[StructMethod_stub]; 77 _kern_GetCurrentAbsoluteTime[ScalarMethod_stub]; 78 _kern_SetBlock[Method_stub]; 79 _kern_GetBlock[Method_stub]; 80 _kern_GetLinkQuality[ScalarMethod_stub]; 81 _kern_SwapCancelAll[ScalarMethod_stub]; 82 _kern_GetProtectionOptions[ScalarMethod_stub]; 83 _kern_CopyLayerDisplayedSurface[ScalarMethod_stub]; 84 _kern_SetContrast[StructMethod_stub]; 85 _kern_GetBufBlock[Method_stub]; 86 _IOMFBGainEncoderFinishEncoding[Method_stub]; UNKNOWN {}
  - => Kernel needs: an `/arm-io` child node with device_type "display-subsystem" (+ canvas-width/height) and an IOService of class **IOMobileFramebuffer** (subclass name match) attached under it, opened with type 0, implementing at least GetDisplaySize(8), IsMainDisplay(18), GetID(7), GetLayerDefaultSurface(3), SwapBegin(4)/SwapEnd(5 struct)/SwapWait(6), Enable/DisableNotifications(72), frame info(73/74), CopyLayerDisplayedSurface(83), plus IOSurface for the swapped buffers. NEXT: CoreAnimation's AppleDisplay/IOMFBDisplay usage of these (constructor, first swap, vsync), then IOSurface.framework's IOSurfaceRoot selectors.
- **GUI PLAN (decided 2026-10-16): CPU rendering + kernel IOMobileFramebuffer.** User: CPU rendering is fine for now (goal = prove the GUI runs), may later research an Apple-style GPU driver for the Adreno 618 -> keep display output and renderer separable. QuartzCore findings (symbols from the cache's .symbols subcache: `ipsw dyld symaddr research/dsc/dyld_shared_cache_arm64 --image QuartzCore`; strings: `ipsw dyld macho ... QuartzCore --strings`):
  - Renderers: `sw_new_context` (CA::OGL::SWContext, full CPU renderer incl. blur, images, IOSurface-backed surfaces, make_buffer_current(WindowServer::Surface)), `gles_new_context`, `metal_new_context`, `new_null_context`. Env knobs include CA_ENABLE_OGL, CA_DISPLAY_TYPE, CA_FORCE_LOCAL_SERVER, CA_FORCE_TIMER, CA_CANVAS_WIDTH/HEIGHT, CA_MAIN_DISPLAY_SCALE/ORIENTATION, CA_DISABLE_RENDER, CA_ALWAYS_RENDER (selection logic not traced yet).
  - `-[CAWindowServer _detectDisplays]` (0x1884e3150) iterates static tables `open_funcs`/`count_funcs` = AppleCLCDDisplay, AppleRGBOutDisplay, AppleWirelessDisplay only; each `count()` = `display_count_of_type(IOMFBDisplayType)` over **`IOMobileFramebufferCreateDisplayList()`**, `open(i)` = `create_display_of_type` -> `CA::WindowServer::AppleDisplay(CFString name, ..., IOMFBDisplayType)` (an IOMFBDisplay driven by IOMFBServer: vsync_callback, frame_info_callback, swaps). `CA::WindowServer::VirtualDisplay` exists but is never auto-detected (explicit API only). => the main display MUST come from an IOMobileFramebuffer kernel service.
  - NEXT: reverse IOMobileFramebuffer.framework: IOMobileFramebufferCreateDisplayList / IOMobileFramebufferOpen (service matching, display type property), the user-client selectors used by IOMFBServer (display size/modes, swap begin/set-layer/end with IOSurface ids, vsync notifications), then the IOSurface kernel side (IOSurfaceRoot user client: create/lookup/lock, memory mapping) that both IOMFB swaps and CA surfaces need. Kernel: SWEET IOMobileFramebuffer service that copies the swapped IOSurface to the 1080x2400 framebuffer at 0x9c000000.
- **logd fixed; backboardd = DISPLAY WALL (boot 0045n on v7, 21:08:57 UTC, frames VID_20261016_230850 00:55-01:28, slide 0x4c3ec000, `research/crash-0045n-v7.txt`).** No `CRASH logd` any more; new daemons run (runningboardd, WirelessRadioManagerd, networkserviceproxy, dasd, cloudpaird, magicswitchd, atc, askpermissiond, ManagedSettingsAgent, containermanagerd uses AKS 0/17). New: locationd sig 11 at CFRunLoopAddSource+132 (NULL source) from its main+0x2af138 in a dispatch_once. **backboardd** sig 6: abort <- std::terminate <- _objc_terminate <- dispatch_once <- main+0x9334 = the "start %{public}@..." wrapper (0x100009264) for **"window server"** (block 0x10000a770 -> dispatch_once body 0x10001d7f0 = StartWindowServer): MTLSetShaderCachePath, `[CAWindowServer serverWithOptions:@{kCAWindowServerDisableUpdatesOnMainDisplay: @YES}]`, `setRendererFlags:`, **`mainDisplay`** -> `displayId` -> `[CADisplay displayWithDisplayId:]`, setWindowServerDisplay:, bounds, pointScale, currentMode, preferredScale, nativeOrientation, touchScales... With no display, mainDisplay is nil and it throws. (HID system and GS start fine before it.) SpringBoard dies on the same missing display (FBSDisplayMonitor). QuartzCore display backends (from the cache's .symbols subcache, `ipsw dyld str`): `CA::WindowServer::AppleCLCDDisplay` (IOMobileFramebuffer hardware) and **`CA::WindowServer::VirtualDisplay`** (open(), new_server, clone_update). `CA_DISPLAY_TYPE` and `CA_FORCE_LOCAL_SERVER` strings exist in QuartzCore but have no direct xref (likely read via a table of CA_* env names). NEXT: find how CA::WindowServer::Server enumerates displays (IOMobileFramebuffer services vs VirtualDisplay / CA_DISPLAY_TYPE) and choose: (a) kernel IOMobileFramebuffer service + IOSurface, or (b) a VirtualDisplay rendered by CA's CPU path (CA::OGL::SW) and copied to the framebuffer.
- **watchdogd + CommCenter + identityservicesd fixed (boot 0045n on v6, 20:41:30 UTC, frames VID_20261016_224133 00:39-01:13, slide 0x20fec000).** `SWEET WDT: watchdogd sel 0 in 0/0 out 1/0 -> ok`, no watchdogd/CommCenter/identityservicesd crashes (CommCenter/identityservicesd now call AKS 0/17/7). usermanagerd boot task runs (UMD_INIT creates primary session uid 501 "mobile", personas 99 System and 199 Default registered with the kernel); `Failed to atomically swap writing file /private/var//keybags/usersession.kb: Operation not supported (45)` (HFS lacks the atomic swap rename; non-fatal so far). Remaining: SpringBoard (FBSDisplayMonitor, no display), backboardd (uncaught exception), mediaserverd, lockdownd (crash inside obfuscated code, main+0x4c1b4?), logd. **logd**: main+0x18160 = `BUG IN LIBTRACE: Failed to migrate directory` from 0x10001801c(dir 0): 0x100016538 requires the dir to exist with uid/gid/mode from the table at 0x10002df14 (0: /private/var/db/diagnostics 272:272 0751, 1: /private/var/db/uuidtext 272:272 0755, 2: /private/var/db/diagnostics/timesync 272:272 0755) plus a `.migrated` file, else XPC "operation 6" to a helper and crash. **Image v7** = v6 + those dirs/markers (also in mkiosroot.sh), SHA256 in `ios16-root-20H392-v7.img.sha256`, `.simg` ready. Boot with `shim/out/shim-boot-0045n.img`.
- **Exit-reason text works (boot 0044n, frames VID_20261016_221726 00:32-00:53, slide 0x22d5c000).** CommCenter/identityservicesd: `couldn't dlopen libobjc-trampolines.dylib: dlopen(/usr/lib/libobjc-trampolines.dylib, 0x0106): tried: '/usr/lib/libobjc-trampolines.dylib' (no such file, not in dyld cache)`: the file only exists in the OS cryptex and dyld does not search cryptexes at ignition_level=1. 19 cryptex files have no root counterpart (OS: DriverKit dyld cache + .symbols, VisualTestKit, libffi-trampolines, libobjc-trampolines, libstdc++ 6.0.9 + 2 symlinks; App: SafariSupport.bundle, AuthenticationServicesAgent, com.apple.Safari.History, passwordbreachd, safarifetcherd, webbookmarksd, webinspectord, webpushd) -> hard-linked (symlinks copied: Linux hfsplus cannot hard-link symlinks); `tools/mkiosroot.sh` does it generically. watchdogd still crashed: the `-wdt_no_pmu` / `/arm-io/wdt no-pmu` check (0x10000a01c) only runs when os_variant_has_internal_diagnostics; otherwise 0x100009b18 looks up IOServiceMatching("IOService") + IOPropertyMatch {IOPMUPrimary: 1} (NSConstantDictionary) and crashes if absent, then IORegistryEntrySetCFProperty(boot stage 0xff) (failure only logged). Patch **0045**: IOWatchdog sets IOPMUPrimary = 1 and accepts setProperties; CI `38961814194` -> `xnu/out-0045`. **Image v6** = v5 + the 19 cryptex links, SHA256 7c610c6a...297a, `ios16-root-20H392-v6.simg` (full flash needed). Boot image `shim/out/shim-boot-0045n.img` (0045 + sweet-0044hi.adt). Still open: SpringBoard (no display), backboardd (uncaught exception, reason none), mediaserverd (Fig NULL dict), logd (main+0x18160), lockdownd (main+0x4c1b4).
- **System survives daemon crashes (boot 0043p, video frames VID_20261016_215006 00:25/00:42/00:54).** With the v5 plist patch no panic: launchd keeps launching (runningboardd, powerd, tccd, healthd, peopled, cryptexd, routined, aksd, mediaremoted, familynotificationd, nanoprefsyncd, navd, cloudpaird, passd, amsaccountsd, accessoryupdaterd, wcd ...). Shared cache slide that boot 0x406b4000 (`research/crash-0043p.txt`). watchdogd still crashed at main+0xb82c from the PMU step: `-wdt_no_pmu` is only honored when os_variant_has_internal_diagnostics; customer builds read `no-pmu` from IODeviceTree:/arm-io/wdt (IORegistryEntrySearchCFProperty) -> mkadt adds /arm-io/wdt {no-pmu}. **cryptexd** sig 6: abort_with_payload <- os_crash_fmt <- `__darwin_arm_secure_boot_chip.cold.7` ("panic: failed to get crypto algorithm") <- `_img4_chip_select_personalized_ap`: reads /chosen use-ddi-secure-boot, allow-ecid-mismatch, crypto-hash-method ("sha1" | "sha2-384"), /chosen/asmb lp-smb0/1 -> mkadt adds crypto-hash-method "sha2-384". **identityservicesd** sig 6 = same `_objc_fatal` in imp_implementationWithBlock as CommCenter (block trampolines; message needed). lockdownd sig 5 at main+0x4c1b4. Patch **0044**: crash report adds `CRASH name reason ns N code X: text` (exit reason description = abort_with_payload text). CI `38959096811` -> `xnu/out-0044`, QEMU OK. Image `shim/out/shim-boot-0044n.img` = 0044 + `shim/out/sweet-0044hi.adt` (wdt no-pmu, crypto-hash-method) + cmdline (wdt=-1, no -wdt_no_pmu).
- **First symbolicated daemon crashes (boot 0043u on journaled v4, 19:38:53 UTC; video frame chat image 36; transcribed in `research/crash-0043u.txt`; shared cache slide 0x40d60000 for every process).** (Gemini's video transcription invented values, use frames only.) `tools/sweet-crash.py research/dsc/dyld_shared_cache_arm64 x research/crash-0043u.txt`:
  - **watchdogd** pc main+0xb82c (`_os_crash` trap helper) from 0x100009bb0 = set PMU boot stage via IORegistry (no PMU) -> fix: boot-args `wdt=-1 -wdt_no_pmu` (Apple's own test mode writes "%s wdt=-1 -wdt_no_pmu"; watchdogd logs "skipping setting PMU boot stage because -wdt_no_pmu found in boot-args or no-pmu found in EDT"). Kernel effect of wdt=-1: sched hygiene debug off, zone_exhausted_timeout -1 (harmless).
  - **SpringBoard** sig 5 in `-[FBSDisplayMonitor _initWithBookendObserver:transformer:]+1160` <- `+[FBDisplayManager sharedInstance]` <- `FBSystemShellInitialize` <- `SBSystemAppMain`: no main display. GUI WALL: display service (IOMobileFramebuffer / CoreAnimation display).
  - **backboardd** sig 6: abort <- std::terminate <- _objc_terminate (uncaught exception; message not shown).
  - **CommCenter** sig 6: `_objc_fatal` in `imp_implementationWithBlock` (from `+[_PFRoutines initialize]`, CoreData): objc block trampolines cannot be allocated -> likely kernel gap (trampoline page remap); will hit many processes.
  - **mediaserverd** sig 11: CFDictionaryGetValue(NULL) in FigNotificationListenerStartNotifications (routing session manager).
  - **logd** sig 5 at its main+0x18160 (bt main+0x9dfc); lockdownd sig 5 (not yet symbolicated).
  - Image v5 = v4 + launchd.plist (`/System/Library/xpc/launchd.plist`, contiguous at image offset 0x1e025f000, 1456980 bytes) with the 7 `_PanicOnCrash` keys renamed `_PanicOnCrasX` in place (raw image patch, no metadata change) -> `diff-v4-v5.simg` = 7 blocks; v5 SHA256 78f96974...185a. Boot image `shim/out/shim-boot-0043p.img` = 0043 + `wdt=-1 -wdt_no_pmu`.
- **0043 console crash reports (QEMU verified); image `shim/out/shim-boot-0043u.img`; NEXT: flash v4 + boot 0043u.** Boot 0042u (video photo IMG_20261016_211646): short exec lines work; watchdogd crashes with NO `SWEET WDT:` line (fails before its first IOWatchdog call: lookup or IOServiceOpen, or elsewhere); lockdownd, SpringBoard, mediaserverd, CommCenter, identityservicesd, backboardd, watchdogd all crash repeatedly ("Too many corpses being created" = corpse limit, no crash reports). AKS callers: lockdownd (0, 7), imagent/sleepd (0, 17, 7), backboardd (0, 17), UserEventAgent (0, 7, 102, 103 -> unsupported). Patch **0043**: `sweet_crash_log` (osfmk/kern/exception.c, called from proc_prepareexit for crash exits on the current proc) prints `CRASH name[pid] sig S code (exc<<32|code) sub X slide SR mh MH`, `CRASH name pc lr sp fp far`, `CRASH name bt` (10 fp frames); AKS sel 103 notify_state_is_set -> 0. CI `38954520419` -> `xnu/out-0043`. QEMU test: init forks, child `brk #1`: `CRASH launchd[2] sig 5 code 0x600000001 sub 0x1000004f0 slide 0x0`, pc correct, parent keeps running (pid 1 crashes panic before proc_prepareexit, so test with a child). `tools/sweet-crash.py CACHE ROOT [report]` symbolicates typed-in CRASH lines (`ipsw dyld a2s` with addr - slide; main binary offsets = addr - mh + 0x100000000).
- **v3 reflashed 18:50-18:55 UTC (`research/flash-userdata-v3b.log`, 24/24 OKAY); 0042u booted 19:01:17 UTC on the clean disk (`research/boot0042u-2.log`), SLOW MO VID pending (watchdogd failure reason).** ABL hangs on the first `fastboot boot` after every big flash; user re-enters fastboot. User gave standing approval to reflash/boot without asking (memory standing-flash-approval). Commits now end with `Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>` (user request; never a Claude trailer).
  - watchdogd flow: connect_to_kext (0x100005408): IOServiceGetMatchingService("IOWatchdog") fails -> "failed to discover" + _os_crash; IOServiceOpen(type 1) fails -> "IOServiceOpen failed with error : 0x%x" + _os_crash. Then sel 0 (out 1 scalar): 0 -> "userspace monitoring disabled", 0x100009bb0, sleep 2, exit(0); nonzero -> "userspace monitoring enabled", normal operation.
  - **Image v4 (JOURNALED):** `tools/mkiosroot.sh` now does `mkfs.hfsplus -J`, mounts with `-o loop,force`, adds the /System/Library/Caches/com.apple.dyld hard links (44) and /private/var/keybags/systembag.kb (0700/0600 root). Built from the IPSW dmgs (OS 087-86698-044, SystemOS 087-86593-047, AppOS 087-86715-047) -> `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v4.img` SHA256 fba4aeae...ec0f9 + `.simg`. Header: attr 0x80002100 (unmounted, journaled), journal info block 129, journal 8 MiB at 532480 flagged need-init. Linux `fsck.hfsplus -n` reports "Volume header needs minor repair" on any fresh journaled volume even after -y (quirk). QEMU: 64 MB journaled test root (`rd=md0 rootrw=1`, 0042 kernel): `hfs: Initializing the journal (joffset 0x3000 sz 0x80000)` -> mounted -> init runs, no panic. NEXT: flash v4 (full), boot 0042u.
- **SPRINGBOARD STARTS (boot 0041u 18:39:53 UTC, photo IMG_20261016_204142).** backboardd, bluetoothd, fairplayd.H2, SpringBoard (pid 111, crashed: "Too many corpses"), xpcproxy run; `SWEET AKS: backboardd sel 0 -> 0`, `sel 17 in 1/0 out 0/4096 -> 0`. Panic: watchdogd[113] exited, namespace 2 subcode 5 (SIGTRAP = its _os_crash): watchdogd crashes on purpose when IOServiceGetMatchingService("IOWatchdog") fails ("connect_to_kext() unexpectedly failed") or IOServiceOpen(type 1) fails ("IOServiceOpen failed with error"); IOWatchdog DOES register (QEMU msgbuf: `SWEET WDT: IOWatchdog registered`), cause not on screen yet -> need video. Note `wdt_test=no_user_checkin` makes watchdogd log "detected testing boot-args to not run, exiting" (path not reached here?). **HFS CORRUPTION:** `hfs_swap_BTNode: offset #0 invalid (0xBC1D) (blockSize 0x2000 numRecords 81)`, `node=19677 fileID=4` (catalog), `hfs_mark_inconsistent` repeated. Likely cause: non-journaled HFS + unclean panics while writing (0038e wrote a 2.7 GB core then panicked; every boot since ends in panic). Plan: reflash v3 (clean), longer term HFS journaling. Patch **0042**: one short `exec NAME` console line instead of the 3-line "Allowing spawn of iOS binary" notice; CI `38949451276` -> `xnu/out-0042`; image `shim/out/shim-boot-0042u.img` (same cmdline as 0041u).
- **LAUNCHD DAEMONS RUN (boot 0039u, 18:29:13 UTC, photo IMG_20261016_203052).** With `launchd_unsecure_cache=1` all boot tasks pass and launchd starts LaunchDaemons (pids > 100: identityservicesd 114 and lockdownd 106 crash, "Too many corpses being created"). Fatal: `watchdogd: ERROR: failed to discover watchdog KEXT service` -> exit (namespace 2 = signal, subcode 5) -> `_PanicOnCrash` panic. watchdogd: IOServiceMatching("IOWatchdog"), IOServiceOpen type 1, IOConnectCallScalarMethod sel 0/1 (check in)/3/4 (disable/enable userspace monitoring)/5, IOConnectCallStructMethod sel 2 (1 KiB in) and 9 (small out); VM path ("detected virtual machine environment and no watchdog KEXT found, exiting...") only when it detects a VM. Console also flooded with `hfs: set hardlink bit on vol=iOSRoot cnid=321318` (Linux-made hard links lack kHFSHasLinkChainBit; repaired only in memory on each lookup). Patches **0040** (`iokit/Kernel/arm/SweetWatchdog.cpp`: IOWatchdog + IOWatchdogUserClient, all selectors succeed with zeroed outputs, each logged once as `SWEET WDT: ...`) and **0041** (sweet_hfs hfs_catalog.c: the three link-repair printfs limited to 8 per boot); CI `38948245372` PASSED -> `xnu/out-0041` (QEMU regression OK). Image `shim/out/shim-boot-0041u.img` (0041 + sweet-0038hi.adt + ufsrw rootrw + launchd_unsecure_cache=1) booted 18:39:53 UTC (`research/boot0041u.log`), photo pending.
- **v3 FLASHED + keybag passes; next wall AMFI (launchd_cache_loader).** User said "flash, nothing to lose": `fastboot -S 512M flash userdata ios16-root-20H392-v3.simg` 17:51:52-17:57:01 UTC, 24/24 chunks OKAY (`research/flash-userdata-v3.log`). userdata = v3 (SHA256 fa84274c...22df) until the next rw boot. ABL fastboot then hung ("Sending" forever, `reboot bootloader` timed out) until the user re-entered fastboot by hand (common Xiaomi ABL crash). Boot 0038e 18:06:15 UTC (`research/boot0038e-3.log`), photos IMG_20261016_200812/200838: keybag OK, then boot tasks tzinit (`Bad zoneinfo link`, resets to system partition, OK), finish-restore, finish-demo-restore, sysstatuscheck, prng_seedctl (`failed to load kernel prng seed`, `failed to load virtual random (-147)`, AllowCrash), auearlyboot, then **launchd_cache_loader: `abort: Unable to find AMFI`** -> 2.7 GB core written to /cores in 91 s -> launchd panic `launchd_cache_loader[22] exited -- exit reason namespace 7 subcode 0x1`. launchd_cache_loader: SecStaticCodeCheckValidity of /System/Library/xpc/launchd.plist with detached .sig, then IOServiceMatching("AppleMobileFileIntegrity") + IOServiceOpen type 0 + IOConnectCallMethod sel 6 (1 scalar = 1, struct in = cdhash) = trust the cdhash, then sends the cache to launchd. Boot-arg `launchd_unsecure_cache=1` skips signature + AMFI ("Using unsecure cache"); the value is parsed with strtoul base 10 (helper 0x100002da8), so a path = 0 = off: boot 0039w (18:17:13 UTC, `=/System/Library/xpc/launchd.plist`, photo IMG_20261016_202008) still aborted `Unable to find AMFI` (core dump now skipped: `Failed to generate core file ... error 14`, so 0039 works). Image `shim/out/shim-boot-0039u.img` uses `launchd_unsecure_cache=1`. Patch **0039**: do_coredump = 0 on SWEET (kern.coredump sysctl still works); CI `38945267341` -> `xnu/out-0039` (_do_coredump now in __common). Image `shim/out/shim-boot-0039w.img` = 0039 + sweet-0038hi.adt + ufsrw rootrw + `launchd_unsecure_cache=/System/Library/xpc/launchd.plist`. TODO later: real AppleMobileFileIntegrity IOService (sel 6 trust cdhash etc.).
- **Image v3 ready, awaiting user "flash".** Boot 0038w 17:38:10 UTC, photo (chat image 30): `SWEET AKS: keybagd sel 0 -> 0x0`, `sel 14 out 1/0 -> 0x0` (x2), `MKB_INIT: mount is with NO CPROTECT ... does not support Data Protection`, `System Keybag loaded`; then keybagd still opens /private/var/keybags/systembag.kb (0x100012d3c, for BackupKeyBagKeys) -> ENOENT, `no-effaceable-storage` not set -> `Can't load the keybag. tears in rain... Time to die` -> `FATAL KEYBAG ERROR: kb_load` -> reboot -> Halt/Restart Timed Out panic. Fix: ADT `/defaults/no-effaceable-storage` (mkadt.py; `shim/out/sweet-0038hi.adt`, `research/q0038-hi.adt`, QEMU OK) + file `/private/var/keybags/systembag.kb` (0600 root, dir 0700; XML plist {KeyBagKeys: data}). **v3 image** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392-v3.img` = v2 + that file, fsck OK, SHA256 fa84274c...22df; `ios16-root-20H392-v3.simg` (img2simg: zero blocks become FILL chunks, so a full flash overwrites every block the rw boots changed). Flash: `fastboot -S 512M flash userdata ios16-root-20H392-v3.simg`, then boot `shim/out/shim-boot-0038e.img` (0038 + sweet-0038hi.adt + ufsrw=1 rootrw=1).
- **0038 AppleKeyStore (CI 38938744130 PASSED -> `xnu/out-0038`; QEMU msgbuf: `SWEET AKS: AppleKeyStore registered (no passcode, unlocked)`; phone image `shim/out/shim-boot-0038w.img` booted 17:38:10 UTC, `research/boot0038w.log`, photo pending).** QEMU full kernel log trick (fb.txt is only the last screen): `llvm-nm KERNEL | grep _msgbufp` -> `--hmp "x /1gx ADDR"` gives struct msgbuf address (static, 0xfffffe0007b56678 on 0038) -> `--hmp "x /3gx STRUCT"` gives bufc (3rd quad) -> `--hmp "memsave BUFC 0x20000 \"FILE\""` (quote the file name), `tr -d "\0" < FILE`. Same inputs give the same addresses. Boot 0037w 17:08:35 UTC, photo IMG_20261016_191052: bindfs fixed mount; mount-phase-1/2 pass; `cc_fips_test` boot task: all corecrypto FIPS POST tests PASSED in userspace; then keybag boot task `keybagd --init`: `MKB_INIT: FATAL KEYBAG ERROR: failed to load system bag`, `Could not get options entry from the device tree` (x2, IODeviceTree:/options = NVRAM, non-fatal), `REBOOTING INTO RECOVERY MODE` -> reboot -> panic `Halt/Restart Timed Out @IOPlatformExpert.cpp:884` (no restart path on SWEET).
  - keybagd (static libaks): load_devicebag (0x100012c30) calls aks_get_system (sel 14, 0-1 scalar in, 1 out) and loads /private/var/keybags/systembag.kb only if the result is 0xe00002f0 (NotFound); any other error -> fatal; SUCCESS -> skip the file (userspace-reboot path). systembag.kb is a plist {KeyBagKeys}; decrypted with effaceable storage unless IODeviceTree:/defaults has `no-effaceable-storage`. Image has no /private/var/keybags. has_data_protection: statfs of /private/var; no cprotect on our HFS -> device-bag volume mapping skipped.
  - libaks connection: IORegistryEntryFromPath / IOServiceMatching("AppleKeyStore"), IOServiceOpen type 0, then selector 0 (no args) must succeed.
  - **AppleKeyStore selector map (iOS 16.7 AppleKeyStore.framework, via `ipsw dyld disass`):** 0 open, 2 create_bag, 3 save_bag, 4 unload_bag, 5 set_system_with_passcode, 6 load_bag, 7 get_lock_state (1 in, 1 out; bit 2 = unlocked since boot), 9 unlock_device, 10 wrap_key, 11 unwrap_key, 12 unlock_bag/test_stashed_kek, 13 lock_bag, 14 get_system, 15 change_secret_se, 16 internal_state, 17 get_device_state (1 in, struct out <=4K: DER SET of SEQUENCE{UTF8 key, INTEGER}: ss state, sls lock_state (MKBGetDeviceLockState returns it), sb backoff, sbo, sfa failed attempts, sfao, sgs generation, sas assertions, sgpe grace, srcd countdown, sr reserved), 18 recover_with_escrow_bag, 19 obliterate_class_d, 20 drain_backup_keys, 21/22 set/clear_backup_bag, 23 get_bag_uuid, 24 rewrap_key_for_backup, 26/27 assert_hold/drop, 30/31 set/get_configuration, 33-37 stash, 38 auth_token_create, 40 generation, 41 fdr_hmac_data, 42 verify_password, 43 _aks_operation, 44 remote_session, 49/50 signing keys, 51/53/54 stash enable/persist/escrow, 55 unload_session_bags, 57/58 remote peer, 59-62 smartcard, 63 migrate_s_key, 65 set_keybag_for_volume_with_cookie_persona, 66/67 sync bags, 68 copy_volume_cookie_persona, 69-73 se_*, 74-79/84/85/99 fv_* (FileVault/VEK), 80/81 absinthe, 82 make_public_backup_bag, 83 drop_auxiliary_auth, 86 kext get/set options, 87 invalidate_bag, 88-91/94/95/106/107 se_*, 92 lower_iteration_count, 93 get_internal_info, 96-98/117 keybag_persona create/list/delete, 100/101 assert promote/consume, 102/103 notify_state set/is_set, 104 fairplay, 105 change_secret_epilogue, 108 delete_xart_leak, 110-113 backup volume, 114 sealed_hashes_set, 115 prewarm_sps, 118-128 AKSIdentity*/AKSGetLastUser.
  - Patch **0038**: `iokit/Kernel/arm/SweetKeyStore.cpp` (class AppleKeyStore on IOResources, IOUserClientClass AppleKeyStoreUserClient; personality in KernelConfigTables.cpp): 0/4/5/9/12/13/26/27/55/102 success, 14 -> handle 0, 7 -> 0x4, 17 -> DER state (ss 4, sls 0), others kIOReturnUnsupported; logs `SWEET AKS: <proc> sel N arg0 in a/b out c/d -> ret` for every call.
  - Tools: shared cache copied to `research/dsc/` (git-ignored; ipsw needs a writable dir for its .a2s symbol cache); `ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image AppleKeyStore|MobileKeyBag`; scratchpad `annot2.py BIN DIS FIXUPS` annotates llvm-objdump output with strings and import names (`ipsw macho info --fixups`).
- **0037 bindfs (phone boot 0037w pending photo).** Boot 0036mw 16:57:47 UTC, photo IMG_20261016_190102: WRITABLE ROOT WORKS: `/dev/ufsroot0 on / (hfs, local, noatime)` after mount_hfs update (GetMasterBlock EBUSY harmless), coredump of mount written to /cores (rw). Then `mount: failed to mount /private/preboot/5CBE3601.../usr/standalone/firmware -> ...: Operation not supported by device (19)` -> exit 74 -> launchd panic `mount[2] exited`. Cause: the macOS-derived config had nullfs but no bindfs (ENODEV = unknown fs type). Caller passes env 0 -> bind not required, ENOENT (missing source dir) tolerated. Patch **0037** adds `bindfs` to FILESYS_BASE in MASTER.arm64.sweet; CI `38936550483` -> `xnu/out-0037` (bindfs_vfsops present; QEMU probe regression OK). Image `shim/out/shim-boot-0037w.img` (0037 + sweet-0036hi.adt + ufsrw=1 rootrw=1) booted 17:08:35 UTC (`research/boot0037w.log`). NOTE: userdata has now been modified by rw boots; it no longer equals the v2 image, so sparsediff with v2 as OLD is not safe any more (full reflash or a new baseline needed).
- **0036 phone: launchd boot tasks run; mount -P 1 needs /chosen/boot-manifest-hash.** Boot 0036hr 16:15:32 UTC, photo IMG_20261016_185236: platform check passes; fsck boot task runs (`fsck: not booting main or diagnostic OS. Skipping fsck on OS container`); mount-phase-1: `failed to get filesystem info` x2 (no IODeviceTree:/filesystems/fstab, non-fatal), `mount: data volume missing, but not required in env: 0`, mount_hfs update of /: `GetMasterBlock: Error 16 opening /dev/ufsroot0` (EBUSY, mounted block dev, non-fatal) then mount() EROFS (read-only boot) -> `mount: / failed with 1`; then `failed to get boot-manifest-hash` -> errx exit 66 -> launchd panic `mount[3] exited`. /sbin/mount: fn 0x100005f90 reads IODeviceTree:/chosen boot-manifest-hash (hex), bindfs-mounts /private/preboot/<HEX>/usr/standalone/firmware (ENOENT tolerated unless required). `tools/mkadt.py` now adds `boot-manifest-hash` = SHA-384("sweet boot manifest") (5CBE3601...5205). ADTs `shim/out/sweet-0036hi.adt`, `research/q0036-hi.adt` (QEMU probe OK, csops 0x26000001). Image `shim/out/shim-boot-0036mw.img` (0036 + new ADT + ufsrw=1 rootrw=1), booted 16:57:47 UTC (`research/boot0036mw.log`): FIRST WRITABLE BOOT of userdata (restore source: v2 image on SSD).
- **0036 CS_SIGNED (phone boot 0036hr pending photo).** Phone boot 0035hr 2026-10-16 15:57:02 UTC (`research/boot0035hr.log`, photo IMG_20261016_180249): highmem works on hardware; shared cache maps (trust-cache warnings only), Apple launchd runs (`Darwin Bootstrapper Version 7.0.0`, `entering ondemand mode`), starts boot task fsck, then userspace panic `boot task is not a platform binary (fsck, /sbin/fsck)`. launchd+0x3fea0: `csops(pid, CS_OPS_STATUS)` must have bit 0x04000000. Cause: nothing set CS_SIGNED (AMFI does it in vnode_check_signature), so csproc_get_blob() returns NULL for every process and csops never reports CS_PLATFORM_BINARY (also breaks per-process cdhash/entitlement queries). QEMU probe init (`csops` status printer, scratchpad csq/) on 0035: `0x02000001`; patch **0036** (`*cs_flags |= CS_SIGNED` in the SWEET AMFI hook), CI `38929635791` -> `xnu/out-0036`: `0x26000001`. Images `shim/out/shim-boot-0036hr.img` / `-0036hw.img` (same cmdlines as 0035). 0036hr booted 16:15:32 UTC (`research/boot0036hr.log`). Next launchd boot tasks (embedded __config plist): fsck -qL and mount -P 1/2 (RequireSuccess); no /etc/fstab on the image.
- **0035 ENTITLEMENTS + PLATFORM BINARIES (QEMU verified).** `xnu/patches/0035-sweet-amfi-entitlements-platform-binaries.patch`: new `iokit/Kernel/arm/SweetEntitlements.cpp` (OSObject subclass: OSDictionary from OSUnserializeXML of the signature's XML entitlements + original XML for CS_OPS_ENTITLEMENTS_BLOB); `bsd/kern/sweet_amfi.c` answers queryEntitlementBoolean/String(+WithProc, string also matches array members), copyEntitlementAsOSObject(+WithProc, retained), asdict (retained), get_xml (kalloc_data copy); built-in MAC policy "SWEET AMFI" (registered from bsd_init before vfs/exec) with mpo_vnode_check_signature: every signed binary on the MNT_ROOTFS mount gets CS_PLATFORM_BINARY (stand-in for AMFI's static trust cache: iOS system binaries are ad-hoc, cd_flags 0x2, platform byte 0), and attaches entitlements ONLY for the writable copy (cs_blob != csb_ro_addr; revalidation passes the RO blob). LESSON: first version attached in adjustContextWithoutMonitor -> QEMU `unable to adjust OSEntitlements context without monitor: 53` because ubc_cs_convert_to_multilevel_hash (4K-page signatures on 16K kernel) sets csb_entitlements_blob = NULL; ALL iOS 16 binaries (launchd, backboardd, SpringBoard) use 4K code pages, so this would have broken launchd on the phone. Boot-arg `sweet_amfi_debug=1` logs `SWEET AMFI: <ident>: N entitlements (test: ...)`. `tools/mkinit.py OBJ OUT [ENTS.plist]` adds an XML entitlements blob (special slot -5; default output byte-identical). QEMU `research/q0035-ent` (RAM disk `research/root-hfs-ent.img`, init with 3 test entitlements, highmem): `org.sweet.init: 3 entitlements (test: bool 1 string 1 array 1)` then init PID 1. MAC policies loaded: 0034 = 1, 0035 = 2 (QMP `mac_policy_list`). CI `38909807230` -> `xnu/out-0035`. Phone images: `shim/out/shim-boot-0035hr.img` (highmem, ro), `-0035hw.img` (highmem, +ufsrw=1 rootrw=1), `-0035r.img` (low mem fallback).
- **PHONE TEST ORDER (updated):** 0035hr -> if instant reset, 0035r -> then 0035hw.

- **HIGHMEM: XNU in the upper DDR bank, ~3.95 GiB (was 1.2 GiB).** Live FDT memory = <0x80000000 2G> + <0x1_00000000 0xfcc00000>; all fixed reserved-memory carve-outs are in the low bank (highest: ramdump_fb 0xaf000000, rmtfs ~0xfde01000), dynamic pools have alloc-ranges <= 0xffffffff, UFS node has no `iommus` (Linux DMAs anywhere). `tools/mkadt.py --highmem` sets /chosen dram-base 0x100000000 dram-size 0xfcc00000; `tools/mkpayload.py` now takes phys_base/mem_size from the ADT (default payloads byte-identical, verified); `tools/qemu-sweet.py` sizes -m from the payload (8G for highmem). Patch **0034** (`0034-sweet-ufs-dma-above-4g.patch`): UFS refuses DMA memory above 4G without UFSHCI CAP.64AS (bit 24), logs `DMA at 0x...`. CI `38905889883` success -> `xnu/out-0034`. **QEMU verified** (`research/q0034-hi`, ADT `research/q0034-hi.adt` --qemu --highmem, HFS RAM disk): phys_base 0x100000000, init PID 1 heartbeat, no panic; `max_mem` = `mem_size` = 0xfcc00000 read via QMP (`research/q0034-hi-mem`). Phone images: `shim/out/shim-boot-0034hr.img` (read-only) / `-0034hw.img` (+ufsrw=1 rootrw=1), ADT `shim/out/sweet-0034hi.adt` (asmb SIP + sweet-ufs + highmem). PHONE RISK: unknown runtime TZ/XPU protection in the upper bank would show as an instant reset; fall back to 0033r/w. Watchdog ruled out for the overnight reset: msm-watchdog bark is 20 s (qcom,bark-time 0x4e20) yet panic screens stay up for many minutes.

- **Offline research 2026-10-16 (user away, phone unavailable). Images rebuilt** `shim/out/shim-boot-0033r.img` / `-0033w.img` now also carry `wdt_test=no_user_checkin`. NEXT ON PHONE: boot 0033r (expect past in6_nigroup), then 0033w (first writes: rw root fixes HFS link-chain bits itself and gives /var for launchd sockets /var/run/syslog, /var/run/lockdown.sock).
- **launchd.plist** (`/System/Library/xpc/launchd.plist`, compiled; 532 daemons, 66 RunAtLoad/KeepAlive, list in scratchpad `early-daemons.txt`). `_PanicOnCrash` daemons: containermanagerd/usermanagerd (InternalOnly), lockdownd/remoted/trustd/remotepairingdeviced (AutomationOnly), **watchdogd unconditional** (KeepAlive SuccessfulExit=false). watchdogd needs IOKit `IOWatchdog` kext service; if absent it exits only when `kern.hv_vmm_present` (ADT `/defaults/vmm-present`, hvg_sysctl.c) or with boot-arg `wdt_test=no_user_checkin` ("detected testing boot-args to not run, exiting"; reads kern.bootargs). Using the boot-arg (vmm-present would change many daemons; keep as an option).
- **GUI path research (QuartzCore in the shared cache, `ipsw dyld str CACHE STR`, ~10 s/search):** CA::WindowServer has IOMFBServer/IOMFBDisplay (Apple DCP/IOMobileFramebuffer), AppleCLCDDisplay, AppleRGBOutDisplay, **VirtualServer/VirtualDisplay** and **MemorySurface**, EAGLServer. Renderers: CA::OGL::MetalContext, GLContext and a **CPU software rasterizer CA::OGL::SW** (samplers/blend modes; "image is too large for software renderer"). Env knobs: `CA_ENABLE_OGL`, `CA_DISPLAY_TYPE`, `CA_FORCE_LOCAL_SERVER` (249 CA_* vars). Hypothesis (unverified): backboardd's render server could run a VirtualServer/MemorySurface with the SW renderer and we copy the surface to the ABL framebuffer -> needs disassembly of CA::WindowServer server creation. Expected other GUI walls: IOSurface (closed-source kext; needs a reimplementation), IOMobileFramebuffer user client, IOHID touch events, AppleKeyStore/keybag (SEP) for lock state, MobileGestalt answers (AppleCLCD etc.).

- **0032r RESULT (boot 05:07:50 UTC, `research/boot0032r.log`, photo IMG_20261016_071200): SHARED CACHE MAPPED, LAUNCHD RUNS.** With SIP relaxed (ADT asmb) the `not in trust cache` lines are only warnings for every subcache (.31 ... .42.dyldlinkedit); no SIP rejection; libSystem loads; launchd configures networking (`skip attaching fsw to lo0 using legacy TX model`) -> kernel data abort far 0x28 in `SHA1Init` (symbolized as sha1_init) from `in6_nigroup` (IPv6 on lo0): g_crypto_funcs NULL. Kernel addresses are unslid: `~/swift/usr/bin/llvm-symbolizer --obj=KERNEL ADDR` works directly.
- **0032** BearSSL SHA-1 (`0032-sweet-bearssl-sha1.patch`), **0033** BearSSL SHA-384/512 (`0033-sweet-bearssl-sha384-sha512.patch`, BearSSL ctx stored in SHA512_CTX, static-asserted to fit; ubc_subr.c uses SHA-384 for code directories). `0020-stage-bearssl.sh` now also stages hash/sha1.c, hash/sha2big.c, codec/{dec,enc}{32,64}be.c. CI `38854909053` success (0032-only run `38854802073` superseded). Kernel `xnu/out-0033` (SHA1Init -> _br_sha1_init verified by disassembly; nm symbols have a leading underscore). Images (ADT sweet-0032): `shim/out/shim-boot-0033r.img` (read-only), `shim-boot-0033w.img` (+ufsrw=1 rootrw=1). Still NULL g_crypto_funcs users: des, chacha20poly1305, rsa, aesxts, rand, flow_divert.
- `research/check-patch-order.py` now skips patches touching CI-staged sweet_hfs sources (0031) instead of stopping.

- **0031r RESULT (boot 05:03:52 UTC 2026-10-16, `research/boot0031r.log`, photo IMG_20261016_070508): SHARED CACHE FOUND.** dyld now opens `dyld_shared_cache_arm64` (v2 image hard links), kernel refuses to map it: `shared_region: ... map(...'dyld_shared_cache_arm64'): not in trust cache` -> `vnode is not SIP-protected` (vm_unix.c: macOS arm64 path needs trust cache, else root-owned + SF_RESTRICTED unless CSR_ALLOW_UNRESTRICTED_FS) -> `dyld cache '(null)' not loaded: syscall to map cache into shared region failed` -> libSystem not loaded -> coredump to /cores fails error 30 (EROFS) -> panic initproc failed -> "MACH Reboot" (spins; screen kept). Lots of `hfs: set hardlink bit on vol=iOSRoot cnid=... linkid=...` spam (Linux-made hard links lack the link-chain bit; read-only mount cannot persist the fix). FIX: ADT `/chosen/asmb` with `lp-sip0`=0x6f (u64; incl. CSR_ALLOW_UNRESTRICTED_FS) and `lp-sip1`=1 (unauthenticated root), `tools/mkadt.py` -> `shim/out/sweet-0032.adt`. Images with kernel 0031: `shim/out/shim-boot-0032r.img` (read-only) and `shim-boot-0032w.img` (+ufsrw=1 rootrw=1).

- **2026-10-16 morning:** userdata now = **v2** image (`diff-v1-v2.simg` flashed 21:06:50 UTC 2026-10-15, OKAY, `research/flash-userdata-diff-v2.log`); future diffs use `ios16-root-20H392-v2.img` as OLD. Booted `shim-boot-0029k.img` (ignition_level=1) right after (`research/boot0029k-v2.log`); **no photo** (user asleep). Next morning the phone was in PixelOS recovery = some hardware reset (ABL then booted Android, which cannot use userdata). OBSERVATION: our kernel cannot reset the SoC itself (`PE_halt_restart` is never set; reboot/halt print "MACH Reboot"/"CPU halted" and spin), so the reset came from outside XNU: Qualcomm watchdog or a TZ/XPU violation. Unknown which; re-boot 0029k and watch/video until the reset. Do NOT tap factory reset/format data in recovery (would wipe the iOS image).
- **0030** (`xnu/patches/0030-sweet-ufs-writes.patch`, CI `38852655542`): UFS WRITE(10) + read-modify-write for partial 4 KiB blocks + SYNCHRONIZE CACHE (DKIOCSYNCHRONIZE), only with boot-arg `ufsrw=1`; `ufs_write_blocks` re-checks partition bounds before every write (only place data goes out).
- **0031** (`xnu/patches/0031-sweet-read-write-root.patch`, CI `38852837176`): boot-arg `rootrw=1` clears MNT_RDONLY before the root mount (no data volume on sweet: /private/var is on the root; image has no /etc/fstab); Apple HFS accepts a dirty non-journaled ROOT read-write on SWEET (logs "mounting dirty non-journaled root read-write"). Later: journaled HFS for crash safety. Kernel `xnu/out-0031` (CI `38852837176` success, 5m31s; 0030 run cancelled by the newer push). Boot images (phone ADT `sweet-0028.adt`, ramdisk byte-matched): `shim/out/shim-boot-0031r.img` = 0029k args (read-only, ignition_level=1) and `shim/out/shim-boot-0031w.img` = same + `ufsrw=1 rootrw=1`. PLAN: boot 0031r first and film until the reset (diagnose overnight reset), then 0031w (first ever writes to userdata; v2 image on the SSD is the restore source). `research/check-patch-order.py` cannot check 0031 (staged sweet_hfs files are not in its index); CI stage scripts run first, so CI is the check.

- **ignition_level=1 result** (boot 21:01:39 UTC, `research/boot0029k.log`, photo IMG_20261015_230308): preboot/cryptex stages skipped (need >= 2/3), `dylib_cache: finding shared cache on system volume: ignition level = 1, search root fd = 4, subpath = System/Library/Caches/com.apple.dyld/` -> not found -> `ignition boot failed: 19` -> no shared cache -> libSystem not loaded. FIX IN PROGRESS: image v2 = copy of the flashed image + `/System/Library/Caches/com.apple.dyld/` with HARD LINKS to the 44 cryptex cache files (no extra space; avoids symlink-following doubts), then `tools/sparsediff.py ios16-root-20H392.img ios16-root-20H392-v2.img diff.simg` and flash only the diff. Boot with `ignition_level=1` (shim-boot-0029k.img).
- **ignition_level=0 result** (boot 20:58:07 UTC, `research/boot0029j.log`, photo IMG_20261015_230037): all stages skipped as "ignition level insufficient" (hello/preboot/cryptex1 sniff >= 2, select os cryptex path/graft >= 3, **dyld_cache >= 1**), `ignition boot failed: 89`, "ignition partially disabled", then same `Library not loaded: /usr/lib/libSystem.B.dylib (no such file, no dyld cache)`. => dyld gets the shared cache directory from ignition's dyld_cache stage. Next: `ignition_level=1` (`shim/out/shim-boot-0029k.img`) to skip preboot/cryptex stages but run dyld_cache; its log "finding shared cache on %s ... subpath = %s" will show where it searches. If it searches rootfs /System/Library/Caches/com.apple.dyld, add a symlink there (image rebuild; consider a diff-only sparse flash tool to avoid rewriting 12 GB).
- **APPLE dyld/launchd RUNNING (libignition) with `unentitled_ios_sim_launch=1`** (boot 20:53:24 UTC, `research/boot0029i.log`, photo IMG_20261015_225617): kernel prints "Allowing spawn of iOS binary ... boot-arg set to true"; `shared_region: ... check_np ... vm_shared_region_start_address() failed` (no region yet, normal); dyld's built-in **libignition** (Darwin Ignition Sequence 1.0.0, libignition-51~1974; prints rd=ufsroot0, hardware target sweet, ignition level 0x5, platform 0x2, boot spec local, 7 stages): `hello from launchd.1` -> `preboot: mounting preboot` -> `failed to lookup preboot: 25` (ENOTTY: looks for a separate Preboot volume by role) -> ignition fails -> dyld `Library not loaded: /usr/lib/libSystem.B.dylib ... (no such file, no dyld cache)` -> launchd exit -> panic initproc failed. dyld boot-args found via strings: `ignition_level`, `ignition_force_dylib_root` (forced rootfs/livefs/cryptex fallback), `ignition_halt_after`, `ignition_live_app_graft`; dyld cache search paths include /System/Cryptexes/OS, /System/Volumes/Preboot/Cryptexes/OS/, /private/preboot/Cryptexes/OS/ (present in our image). Next try `ignition_level=0` (`shim/out/shim-boot-0029j.img`).
- **APPLE launchd EXECUTED FROM UFS (2026-10-15).** Flashed iOS image: `img2simg` -> `ios16-root-20H392.simg` (12.1 GB) then `fastboot -S 512M flash userdata ...simg` (24 chunks, 307 s, `research/flash-userdata-ios16.log`; a raw-image flash was killed by earlyoom at 2.6 GB RSS - always flash the sparse image). ABL fastboot hung on getvar after the flash; power+vol-down fixed it. Boot 0029u 20:49:04 UTC (`research/boot0029u-ios16.log`), photo IMG_20261015_225134: `hfs: mounted iOSRoot` -> load_init_program /sbin/launchd -> panic `unexpected SIGKILL of launchd (CS_KILLED) with reason -- namespace 9 code 0xe` = OS_REASON_EXEC / EXEC_EXIT_REASON_WRONG_PLATFORM: our kernel is XNU_TARGET_OS_OSX, iOS-platform binaries are "alien" and refused unless spawn attr platform set or DEVELOPMENT TUNABLE `unentitled_ios_sim_launch`. Next try: same kernel, cmdline + `unentitled_ios_sim_launch=1` (`shim/out/shim-boot-0029i.img`). LONGER TERM: macOS-config kernel marks iOS maps alien (vm_map_mark_alien) and differs from iOS in many policies; an iPhoneOS-config kernel build may eventually be needed.
- **iOS 16 ROOT IMAGE BUILT:** `/run/media/dipa/android/ios-sweet-ipsw/ios16-root-20H392.img` (16 GiB case-sensitive HFS+, label iOSRoot, 12 GB used, fsck OK after `fsck.hfsplus -y` rewrote 32 non-ASCII localized WorkflowKit Shortcut names Linux decomposes differently). Built by `tools/mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]` (needs sudo; apfs-fuse as root; OS dmg is UDIF -> link must end .dmg). Contents: iPhone X 20H392 OS root + SystemOS cryptex at /private/preboot/Cryptexes/OS (so /System/Cryptexes/OS -> cache, 44 files, dyld_v1 arm64) + AppOS at .../App. /sbin/launchd and /usr/lib/dyld root:wheel 0755. NEXT: (confirm) `fastboot flash userdata ios16-root-20H392.img` (auto-sparse), boot 0029u (rd=ufsroot0) -> Apple launchd attempt.
- **MILESTONE ON HARDWARE: BOOT FROM INTERNAL FLASH.** 0029 CI `38797924912` PASSED, `xnu/out-0029` SHA256 `a68bc847...2a37`. Image `shim/out/shim-boot-0029u.img` (payload-0029u = 0029 kernel + `shim/out/sweet-0028.adt` + `rd=ufsroot0`, NO RAM disk). Boots 20:01:34 and 20:12:37 UTC (`research/boot0029u*.log`), photo IMG_20261015_221403: `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB, read-only` -> `BSD root: ufsroot0, major 1, minor 0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `SWEET: root volume is not authenticated (error 25); continuing unsealed` -> `load_init_program ... /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> heartbeat (ran for minutes unattended). userdata currently holds the 8 MB `research/root-hfs.img`. NEXT: build a multi-GB HFS+ image with the iOS 16 (iPhone X 20H392) userland (OS rootfs + SystemOS cryptex contents incl. dyld shared cache at /System/Library/Caches/com.apple.dyld; preserve modes/owners/symlinks; decompress LZBITMAP via apfs-fuse), flash it to userdata (sparse, needs user OK), then try Apple's dyld/launchd. Driver is read-only (root mounts RDONLY), fine for first launchd attempts.
- **userdata FLASHED with the 8 MB HFS test volume** (user said "flash it", 2026-10-15 19:53:17 UTC, `research/flash-userdata-hfs-test.log`: `fastboot flash userdata research/root-hfs.img` OKAY; pre-checks: one sweet device, partition-size:userdata 0x1AC4BFB000, partition-type f2fs, image fsck OK, SHA256 3626ea69...). Android userdata is now destroyed (expected/approved). Boot 0028u at 19:53:20 (`research/boot0028u-hfs.log`), photo IMG_20261015_215541: `BSD root: ufsroot0` -> **`hfs: mounted SweetRoot on device b(1, 0)` from internal UFS** -> panic `rootvp not authenticated after mounting @bsd_init.c:931` (arm64 macOS-config requires FSIOC_KERNEL_ROOTAUTH sealed root for non-ramdisk roots). Patch **0029** (`dd1c284`): SWEET logs `SWEET: root volume is not authenticated (error N); continuing unsealed` instead of panicking (Apple configs keep the panic; pivot_root still requires auth). CI **`38797924912`** running. Next: download out-0029, phone image with sweet-0028.adt + rd=ufsroot0 (no RAM disk), boot -> expect init from UFS.
- **UFS ROOT DRIVER VERIFIED ON HARDWARE (read-only).** 0028 CI `38796139570` PASSED, `xnu/out-0028` SHA256 `8b981640...9da6`; QEMU HFS RAM-disk regression `research/q0028-hfs` still runs init. Phone boot 2026-10-15 19:50:24 UTC (`research/boot0028u.log`, image `shim/out/shim-boot-0028u.img`, phone ADT `shim/out/sweet-0028.adt` with /sweet-ufs, NO RAM disk, `rd=ufsroot0`), photos IMG_20261015_215151/215220: `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB, read-only` -> `BSD root: ufsroot0, major 1, minor 0` -> `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : f188` (Android-encrypted data, as expected) -> `cannot mount root, errno = 19`, kernel retries mountroot (no panic). Nothing written to flash. NEXT (destructive, needs user confirmation right before): `fastboot flash userdata research/root-hfs.img` (8 MB HFS with test init) then boot rd=ufsroot0 -> expect init from internal flash.
- **PATCH 0028 (`33aceac`): XNU read-only UFS root device**, CI **`38796139570`** running (next artifacts xnu/out-0028, research/bt29, run29.log). `bsd/dev/sweet_ufs.c`: reuses ABL's UFSHCI state (like the shim probe), takes over UTRL at a kmem_alloc_contig region (DMA layout: UTRL 0x0, UCD 0x1000, 1 MiB bounce 0x4000), verifies UTRLBA readback before any doorbell, IE=0 polled, one SCSI cmd at a time on LUN0 (TUR x3, READ(10) via bounce, PRDT 256 KiB chunks), flush_dcache64 by PA around each command (host not dma-coherent per DT), finds GPT partition by name (`ufsroot=` boot-arg, default `userdata`), exposes `/dev/ufsroot0`/`rufsroot0` (512-byte logical sectors, physical 4 KiB, DKIOC* ioctls), refuses writes (EROFS). IOKitBSDInit: `rd=ufsroot0` -> sweet_ufs_rootdev() (lazy init) else panic. ADT: `tools/mkadt.py` adds `/sweet-ufs` reg <0x1d84000 0x3000> for the PHONE only (not --qemu). Local compile OK, imports resolve. Sandbox research/bt28 (pre0028 originals). FIRST PHONE TEST PLAN (non-destructive): boot 0028 with phone ADT + `rd=ufsroot0` and NO RAM disk: expect `SWEET UFS: "userdata" on LUN 0: first block 0x2b8c00, 109643 MiB` + `BSD root: ufsroot0`, then root mount failure (userdata is still Android-encrypted) -> panic. That validates the driver before flashing an HFS image into userdata (needs explicit user confirmation at that time).
- **SUPERSEDES the 64 GiB iosroot GPT plan: user said "u can delete android" (2026-10-15).** Use the existing `userdata` partition (LUN0 sda34, 4K LBA 0x2B8C00, 0x1AC4B MiB ~107 GiB) directly as the iOS root: NO GPT changes. Keep bootloaders, recovery (OrangeFox), modem/persist/fsg/modemst untouched. Write the HFS image later with `fastboot flash userdata` (sparse, auto-split under the 768 MiB limit). Android will no longer boot normally (expected; reinstallable). Order: XNU UFS driver first, read-only against the current GPT; flash only once it can locate userdata. Nothing erased yet.
- **DECIDED (user, 2026-10-15): 64 GiB `iosroot` on LUN0 (sda), taken from userdata (wipes Android user data; Android/super kept).** Planned GPT (4K LBAs): userdata 0x2B8C00..0xD7D6FF (~43 GiB, keep type/unique GUIDs and attributes); iosroot 0xD7D700..0x1D7D6FF (0x1000000 blocks = 64 GiB, 1 MiB aligned); backup GPT area unchanged. Procedure: user backs up phone data -> boot OrangeFox recovery (adb) -> back up GPT (sgdisk --backup + raw dd of LBA 0-5 and last 6 blocks) to research/ -> sgdisk edit -> Format Data -> verify Android boots to setup. NOT EXECUTED YET.
- **INTERNAL UFS WORKS FROM THE SHIM (read-only), user authorized internal-UFS approach incl. flashing (2026-10-15, "i allow everything").** Shim probe `shim/ufs.c` (commit `12ed3ed` + filter): ufshc@1d84000 is UFSHCI 2.1 (ver 0x210, cap 0x1587031F, 32 slots), ABL leaves HCE=1, HCS=0x10F (device present, lists ready, UIC ready); probe takes over UTRL at its own buffer (verifies UTRLBA readback before any doorbell), TEST UNIT READY/READ CAPACITY/READ(10) all succeed. LUN0 = 0x1D7D800 x 4 KiB (~126.6 GB), valid GPT (64 entries at LBA 2). Photos IMG_20261015_212916/213325/213429. Big LUN0 partitions (start in 4K LBAs / MiB): super 0xCA00/0x2200 (8.5 GiB), logdump 0x22CA00/64, minidump 0x230A00/128, rawdump 0x238A00/128, recovery 0x240C00/128, cache 0x248C00/384 ext4, exaid 0x260C00/384, cust 0x278C00/1024 ext4, **userdata 0x2B8C00/0x1AC4B MiB (~107 GiB), fs unreadable = encrypted (metadata encryption)**. cache..cust contiguous = only 1.75 GiB (too small for iOS ~10+ GiB). Plan needing user consent: shrink userdata in GPT (wipes Android data) and add an `iosroot` partition at the end of LUN0. LUNs 1-5 not listed yet (sdb..sdf backed up in backup/lun).
- **fastboot limit measured (2026-10-15, read-only getvar):** `max-download-size: 805306368` (768 MiB); `max-fetch-size` unsupported; `is-userspace: no` (ABL fastboot). A `fastboot boot` image (shim+kernel+RAM disk) must be < 768 MiB, so the 2.5 GiB iOS shared cache cannot ride in the boot RAM disk even if XNU gets all RAM. Real userland needs storage access (UFS driver, or USB host) or a much smaller cache. Unlocking full RAM is still needed later (GUI).
- **iOS 16 USERLAND SOURCE (Claude, 2026-10-15).** Chose **iPhone X (iPhone10,3/10,6, D22/D221AP)** iOS **16.7.16 (20H392)** over iPhone 8: same A11 (no PAC -> plain arm64, runs on Kryo 470/ARMv8.2), last iOS for A11; X userland is home-button-less and tall-screen (1125x2436 vs sweet 1080x2400), better fit for later GUI. Note the X has its OWN SystemOS cryptex (different digest from iPhone 8's). IPSW URL in `research/ipsw-x/URL.txt` (+ BuildManifest). Images on the user's external SSD (NOT in git, do not format that disk: it holds the user's photos): `/run/media/dipa/android/ios-sweet-ipsw/iPhone10,3_16.7.16_20H392/` — `087-86593-047.dmg` = Cryptex1,SystemOS (2,743,074,816 B), `087-86698-044.dmg` = OS rootfs (4,730,814,044 B), both zip CRC-32 verified, plus Firmware/*.root_hash/.trustcache/.mtree. AppOS cryptex is 087-86715-047.dmg (10 MB, same as iPhone 8).
  - Images are raw APFS; files use LZBITMAP (decmpfs type 14). 7z cannot decode. Tools: `ipsw` 3.1.738 (AUR ipsw-bin, user-approved) needs apfs-fuse on Linux; **apfs-fuse** built in user space (system cmake broken: cmake 4.4.4 wants libjsoncpp.so.27, jsoncpp 1.9.6 installed = partial upgrade, NOT fixed) with pip cmake + `-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint"`, installed to `~/.local/bin/apfs-fuse` (+apfs-dump-quick). Mount via a comma-free symlink (apfs-fuse splits options on ',' in `iPhone10,3`; exFAT has no symlinks -> put link elsewhere): `apfs-fuse LINK/087-86698-044.dmg MNT` -> `MNT/root/...`; unmount `fusermount3 -u MNT`.
  - Verified from headers: `dyld_shared_cache_arm64` magic `dyld_v1   arm64`; `/sbin/launchd`, `/usr/lib/dyld` = cputype 0x100000c subtype 0 (plain arm64). libSystem exists only inside the cache.
  - **BLOCKER for real userland via RAM disk:** shared cache = 3.10 GiB in 44 files (~2.5 GiB without the 592 MB `.symbols`), but the kernel manages only 1.20 GiB (0xb0000000..0xfd000000). Options: (A) give XNU the phone's full ~6 GB (banks 0x80000000+2G and 0x100000000+0xfcc00000, carve-out holes need pmap support) AND check fastboot max-download-size for a multi-GB boot image; (B) storage driver (Qualcomm UFS) to read a root from flash (no flashing allowed: would need a file/partition strategy); (C) run against individual dylibs extracted from the cache (uncertain).
- **MILESTONE ON HARDWARE: HFS+ ROOT FILESYSTEM -> PID 1.** Boot 2026-10-15 18:22:12 UTC (`research/boot0027b.log`, image `shim/out/shim-boot-0027b.img`), photo `IMG_20261015_202334.jpg`: `BSD root: md0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> repeated `SWEET init: still alive`, no panic. Kernel 0027 (built-in Apple OSS HFS + BearSSL MD5), init with SP alignment fix. NEXT: real iOS 16 userland on the HFS image (dyld, libSystem/dyld shared cache, launchd). Known gaps: entitlements (none granted), SHA-1/SHA-384/512 providers, no storage driver (RAM disk only), root mounted read-only.
- **PHONE HFS ROOT (0027) + SIGBUS ROOT-CAUSED.** Phone boot 2026-10-15 17:49:06 UTC (`research/boot0027.log`, photo `IMG_20261015_200031.jpg`): `hfs: mounted SweetRoot on device b(1, 0)` (HFS works on hardware), init printed its first line, then `pid 1 exited -- exit reason namespace 2 subcode 0xa` = SIGNAL/SIGBUS -> `initproc failed to start` panic. Diagnostic boot 18:08:13 UTC (`research/boot0027d.log`, image `shim/out/shim-boot-0027d.img`, diag init `research/diag-init` from scratch diag-init.S with [a][b][c] markers, `cs_debug=1`; photo `IMG_20261015_201547.jpg`): output `PID [a][b]` then SIGBUS -> fault at first `strb w21,[sp]`; no code-signing messages. Cause: kernel enters a static binary with sp at argc, not necessarily 16-aligned (depends on arg/apple string lengths, differs mockfs vs HFS); EL0 SP alignment check -> EXC_ARM_SP_ALIGN -> SIGBUS (sleh handle_sp_align). QEMU does not enforce SP alignment. NOT a kernel bug: real binaries start in dyld which aligns. Fix `1a8fc15`: sweet-init aligns sp at entry. Rebuilt `research/root-hfs.img`, QEMU `research/q0027-hfs2` OK. Phone image `shim/out/shim-boot-0027b.img` SHA256 `098ba46c...c5c` (payload-0027b, ramdisk match check-0027b) ready, awaiting FASTBOOT. Lesson: Assembly `;` is a comment in Apple arm64 asm (use .macro, not ;-joined #define).
- **HFS ROOT WORKS (QEMU).** 0027 CI `38779441895` PASSED, `xnu/out-0027` SHA256 `a8643886...5f6a`. QEMU `research/q0027-hfs` (RAM disk = `research/root-hfs.img`, rd=md0): `BSD root: md0` -> `hfs: mounted SweetRoot on device b(1, 0)` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` + heartbeat, no panic. Phone image `shim/out/shim-boot-0027.img` (payload-0027 = 0027 kernel + sweet-0021.adt + root-hfs.img + rd=md0, ramdisk byte-match check-0027) ready; awaiting FASTBOOT for phone test.
- **HFS ROOT WORK (Claude), patches 0026-0027.**
  - 0026 (`f0e990c` + link fix `64c96f4`): Apple OSS **hfs-627.100.6** (commit `31d5ec238dfab485e78a9a30b8b92d13e8914f03`, paired with xnu-8796.141.3 in macos-135 release.json) staged by `xnu/patches/0026-stage-hfs.sh` into `bsd/sweet_hfs` (core + hfs_encodings; AppleKeyStore include/key prototypes guarded by CONFIG_PROTECT; SDK TargetConditionals copied). New config option `sweet_hfs` (MASTER + MASTER.arm64.sweet); 40 `optional sweet_hfs` files; Makefile.template foreach adds `_CFLAGS_ADD = -include sweet_hfs_compat.h -I...` (MUST come after kernel -D flags; compat undefs BSD/XNU_KERNEL_PRIVATE and the CONFIG_* HFS tests so kext-config.h decides: macOS profile, no content protection). `sweet_hfs_iokit.c` replaces hfs_iokit.cpp/hfs_encodings_kext.cpp: `sweet_hfs_register()` called in bsd_init after vfsinit (encodings init, vfs_fsadd VFS_TBLCANMOUNTROOT, zones, sysctls); is_ejectable 0, iterate_media none, serial KERN_NOT_SUPPORTED, OSKextGetCurrentLoadTag -> kOSKextInvalidLoadTag (renamed via compat macro). hfs_encodings' own `hfs_allocated` renamed per object (separate kexts on Apple; first CI `38777325964` failed on that duplicate at link). mockfs now validates Mach-O magic (its TODO) so non-Mach-O roots fall through to HFS (new fs entries come AFTER mockfs in vfsconf). CI `38778133720` PASSED, `xnu/out-0026` SHA256 `0b49170c...701a`, prints `SWEET HFS: hfs-627.100.6 registered`.
  - HFS+ image: `research/root-hfs.img` (8 MB, mkfs.hfsplus from AUR hfsprogs installed with user approval; loop-mounted with sudo; /sbin/launchd = research/sweet-init, /dev; fsck.hfsplus OK). QEMU `research/q0026-hfs`: `BSD root: md0` then data abort in MD5Init from hfs_getvoluuid (mount computes volume UUID).
  - 0027 (`14bf2b7`): 0020 stager also copies BearSSL hash/md5.c + codec/dec32le.c/enc32le.c; `corecrypto_md5.c` SWEET branch converts MD5_CTX <-> br_md5_context around br_md5_* calls; wrappers sweet_md5.c/sweet_dec32le.c/sweet_enc32le.c. Host test 40/40 match md5sum. CI **`38779441895`** running.
  - Local tooling: `tools/xnu-cc.py` has `XNU_CC_POST` (flags after logged ones). Sandbox `research/bt28/xnu` = 0025 CI tree + 0026/0027 edits; originals `research/pre0026`, `research/pre0027`. HFS local objects `research/hfs-objs`. Checker entries updated to bt28.
  - SHA-1 / SHA-384/512 still have no provider.
- **MILESTONE ON HARDWARE: USERSPACE PID 1 RUNS ON THE PHONE.** Boot 2026-10-15 17:16:00 UTC (`research/boot0025.log`: send OKAY0.410s, boot OKAY0.135s), image `shim/out/shim-boot-0025.img` SHA256 `9dbfa0b1...89db0`. Photo `IMG_20261015_191733.jpg`: `BSD root: md0` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> repeated `SWEET init: still alive` (5 s heartbeat), no panic. Kernel 0025 + mockfs RAM disk + built-in no-entitlements AMFI. No flashing performed.
- **NEXT (toward real iOS userland):** real root filesystem with real binaries. Options: (a) build Apple OSS `hfs` (macOS 13.5 tag `hfs-627.100.6`) into SWEET like libpthread, then an HFS RAM disk image; (b) iOS 16 dyld + libSystem/dyld shared cache from the IPSW (needs APFS or repacking onto HFS). Expected hurdles: entitlements (sweet_amfi grants none), SHA-1/MD5/SHA-384/512 providers (BearSSL), missing kexts/daemons. Current init is a static test program, not launchd.
- **MILESTONE: USERSPACE RUNS (QEMU).** 0025 CI `38774788048` PASSED, `xnu/out-0025` SHA256 `5496069b...4877`. QEMU `research/q0025-rd` (150 s, no panic): `BSD root: md0` -> `load_init_program: attempting to load /sbin/launchd` -> `*** SWEET userspace: init running as PID 1 ***` -> `SWEET init: still alive` every 5 s. Exec, ad-hoc SHA-256 code signature, syscalls, devfs /dev/console, select timeouts all work. Phone image prepared: `shim/out/shim-boot-0025.img` 16,515,072 bytes SHA256 `9dbfa0b15140c0bd733a16c4f727f6a7df44a8741f98d8e266d5f2fa4f689db0` (payload-0025 = 0025 kernel + sweet-0021.adt + research/sweet-init + rd=md0; ramdisk byte-match check-0025). Waiting for user to put phone in FASTBOOT.
- **0024 CI `38773277811` PASSED**, `xnu/out-0024` SHA256 `a0ba964b...4348`. QEMU `research/q0024-rd` and **PHONE (boot 2026-10-15 17:05:55 UTC, `research/boot0024.log`, image `shim/out/shim-boot-0024.img` SHA256 `cf3dac86...6964`, 16,510,976 bytes, payload `shim/out/payload-0024.bin` = 0024 kernel + `shim/out/sweet-0021.adt` + RAM disk `research/sweet-init` + rd=md0; ramdisk byte-match `check-0024`)** agree: `Added memory device md0`, `BSD root: md0`, `load_init_program: attempting to load /sbin/launchd`, then data abort in accelerate_entitlement_queries (PC 0x...77d2458, FAR 0x1a0) = NULL AMFI interface. Photo `IMG_20261015_190715.jpg`. RAM disk root is physically verified.
- **0025 (`2518c79`)**: new `bsd/kern/sweet_amfi.c` registers a built-in AMFI interface at EARLY_BOOT/MIDDLE: no entitlements (adjustContextWithoutMonitor succeeds without creating a context, queries KERN_DENIED, copies NULL/KERN_NOT_FOUND, get_xml/transmuted false); WithMonitor KERN_NOT_SUPPORTED; OSEntitlements_query and query_context_to_object panic explicitly (CoreEntitlements closed; no kernel caller); TrustCache version 0 (0015 keys on trust_cache_rt==NULL, img4 absent -> still unavailable); CoreEntitlements table zero (PMAP_CS only). Compiles locally; checker updated (bt27 entries). CI **`38774788048`** running. Next artifacts `xnu/out-0025`, `research/bt29`, `research/run29.log`. Real iOS binaries will need real entitlements later (no OSS CoreEntitlements exists).
- **0021-0024 RAM-disk-root iteration (Claude), QEMU only so far, no new phone boot since 0020.** Each kernel tested with `tools/mkpayload.py KERNEL research/q0021.adt OUT --ramdisk research/sweet-init --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0"` then `tools/qemu-sweet.py OUT DIR --secs 150 --kernel KERNEL --fail-on-panic` (PATH needs ~/swift/usr/bin for llvm-nm). Fresh ADTs: `research/q0021.adt` (QEMU), `shim/out/sweet-0021.adt` (phone).
  - 0021 CI `38769532000` PASSED, `xnu/out-0021` SHA256 `2b7500a1...962e`, `research/bt25`/`run25.log`. QEMU `research/q0021-rd`: md0 root, then assert `iomediaClass` in IOService::publishHiddenMedia.
  - 0022 (`20ece9c`, publishHiddenMedia without IOMedia class) CI `38770666158` PASSED, `xnu/out-0022` SHA256 `a4d0e787...30c5`, `research/bt26`/`run26.log`. QEMU `research/q0022-rd`: data abort in SHA256_Init (NULL g_crypto_funcs) from vm_kernel_addrhash_internal.
  - 0023 (`c8089d1`, libkern SHA256_Init/Update/Final use built-in ccsha256_ltc_di + ccdigest_init/update on SWEET; also serves code-signing SHA-256; SHA-1/MD5/SHA-384/512 still have NO provider - BearSSL from 0020 could supply them later) CI `38771876423` PASSED, `xnu/out-0023` SHA256 `d763fed7...1003`, `research/bt27`/`run27.log`. QEMU `research/q0023-rd`: **mockfs mounted md0, `load_init_program: attempting to load /sbin/launchd`**, then panic `mockfs_fsnode_vnode failed to create fictitious pages ... rvalue = 5` (mockfs.c:351).
  - 0024 (`3293c80`): mockfs used memdev mi_base (4K page number of a VIRTUAL kernel address for IOKit RAMDisk, truncated to 32 bits) << PAGE_SHIFT as a physical address. Now zero-copy pager mapping only when mi_phys (with 4K units); virtual md uses pagein/blockmap/strategy -> memdev bcopy. memdev's own mdPhys path assumes 4K ppnums (unsafe on 16K arm64), so do NOT switch the RAMDisk to physical. CI **`38773277811`** running. Next artifacts `xnu/out-0024`, `research/bt28`, `research/run28.log`.
  - Sandboxes: bt25/bt26/bt27 contain the next patch's edited files; pristine copies in `research/pre0022`..`pre0024`.
- **PATCH 0021 (Claude): RAM disk root via mockfs -> first userspace program.** Commits `9c37771` (initial; CI `38768500280` FAILED: mockfs_fsnode.c kfree_type(mockfs_fsnode_t) typed-alloc static assert; local xnu-cc stubs `__builtin_xnu_types_compatible` so it can't catch this) and `193772f` (fix), CI **`38769532000`** running: https://github.com/dipa-studio/ios-sweet/actions/runs/38769532000. Next artifacts: `xnu/out-0021`, `research/bt25`, `research/run25.log`.
  - Kernel: `config/MASTER.arm64.sweet` FILESYS_BASE += `mockfs` (XNU's built-in FS: root device is itself a Mach-O, served as /sbin/launchd, plus /dev for devfs). Upstream mockfs bit-rot fixed: kalloc_type/kfree_type used the POINTER typedef mockfs_fsnode_t (nodes were 8 bytes) -> `struct mockfs_fsnode`. `IOService::setRootMedia` asserted IOBlockStorageDriver class exists (IOStorageFamily absent): SWEET-only, a missing class means root=NULL (same as IOSetImageBoot); Apple path keeps assert. All 21 patches apply (`research/check-patch-order.py`); 5 TUs compile locally with -DMOCKFS (`research/mockfs-objs`), imports resolve vs 0020 kernel. Sandbox `research/bt24/xnu` now holds 0021 versions of those 3 files; pristine 0020 copies in `research/pre0021`.
  - Tools: `tools/mkpayload.py --ramdisk IMAGE` appends IMAGE as an extra payload segment right after the kernel, moves kernel_end past it (shim unchanged: ADT+boot_args follow it, image stays below topOfKernelData), and adds ADT `/chosen/memory-map` `RAMDisk`=<phys,size>. Boot with `rd=md0`. Without --ramdisk output is byte-identical to existing 0020 QEMU and phone payloads (verified). `tools/sweet-init.S` + `tools/mkinit.py`: PIC static init (open /dev/console, print PID, heartbeat every 5 s via select, never exits) wrapped into a static arm64 Mach-O (__PAGEZERO, __TEXT@0x100000000, LC_UNIXTHREAD, ad-hoc SHA-256 CodeDirectory v0x20400, 4K pages). Build: `~/swift/usr/bin/clang -target arm64-apple-ios16.0 -c tools/sweet-init.S -o X.o && tools/mkinit.py X.o research/sweet-init` (ld64.lld cannot link static iOS). Signature not verified by a real codesign; the kernel will tell.
  - QEMU with 0020 kernel + RAM disk (`research/q0020-rd.bin`, rd=md0, 150 s `research/q0020-rd150`): `Added memory device md0`, **`BSD root: md0, major 1, minor 0`** (plumbing works), then the setRootMedia assert (now fixed in 0021). NOTE: QEMU needs >60 s to reach IOFindBSDRoot (60 s run showed nothing after stf0; likely RTC/inittodr wait), so use --secs 150.
  - NEXT: download 0021, QEMU 150 s with ramdisk; expect mockfs mount then init exec -> `*** SWEET userspace: init running as PID 1 ***`. Then phone boot with same payload (phone ADT shim/out/sweet-0020.adt pattern + --ramdisk + rd=md0).
- **0020 PHONE RESULT (user transcription of screen, 2026-10-15): KERNEL INIT COMPLETE, WAITING FOR ROOT.** After Skywalk self-tests PASSED, kminit, ifnet_attach lo0/gif0/stf0, screen prints `BSD`, then `Waiting on <dict><key>IOProviderClass</key><string>IOMedia</string><key>Content</key><string>Apple_HFS</string></dict>` and `Failed to write boot-picker-bringup-reason to NVRAM.` No panic. This is IOFindBSDRoot waiting for a root device (none exists); NVRAM warning is expected (no NVRAM driver). QEMU `cpu_idle` snapshot is presumably the same wait (not GDB-confirmed; milestone GDB run was cancelled by user since phone answered it). Draft `research/check-milestones20.gdb` (hbreaks inittodr/vfs_mountroot/IOFindBSDRoot/bsd_utaskbootstrap) left uncommitted/ignored.
- **NEXT MILESTONE: root filesystem -> launchd.** Plan: (1) RAM disk: shim places an image in RAM, ADT /chosen/memory-map `RAMDisk` entry, boot-arg `rd=md0` (XNU md driver). (2) HFS is a separate kext on Apple; build Apple OSS `hfs` (tag paired with macOS 13.5 in distribution-macOS macos-135 release.json) into SWEET like libpthread 0019. APFS is closed. (3) Minimal HFS image with iOS 16 launchd (later: dyld/shared cache; AMFI/code-signing absent will be a later hurdle). User's end goal is full iOS GUI.
- **0020 (AES) VERIFIED + PHONE BOOTED (Claude took over after Codex usage limit).** Actions `38765422261` (`ab9e23f`) PASSED; kernel `xnu/out-0020/kernel.development.sweet` checksum OK; buildtree `research/bt24`, log `research/run24.log`. Actual-kernel AES checks PASS (`research/check-aes20.gdb` / `.log`, 12 extra CBC cases match OpenSSL). 45s QEMU `research/q0020`: no panic, zero unknown glyphs, past old TCP/AES panic -> Skywalk self-tests PASSED -> bsd_autoconf kminit -> ifnet_attach lo0/gif0/stf0; final snapshot PC in `cpu_idle` (idle or waiting; NOT yet diagnosed, could be root-device wait). Image `shim/out/shim-boot-0020.img` 16,461,824 bytes SHA256 `775200cb02d95fc750981516b79d91dcdc96184bbe67a38f9d57c59b4a391828`, ramdisk byte-matches `payload-0020.bin` (`check-0020`). **Phone temporarily booted 2026-10-15 16:10:00 UTC**: one FASTBOOT product sweet, send OKAY0.407s, boot OKAY0.134s, total0.549s, exit0 (`research/boot0020.log`). Awaiting user photo; do not reboot the phone before it. NEXT: read photo; diagnose what QEMU waits on after stf0 (GDB backtrace of threads / longer run). Codex's STOP request below is complete.
- User requests STOP after completing the current0020 AES iteration (Actions/QEMU/phone), then leave a precise committed handoff for another AI. User says FASTBOOT ready again after0019 photo. Do not start a new blocker fix after this iteration.
- Goal: iOS16-generation XNU on Redmi Note10Pro sweet/SM7150. Always commit, use gh/Actions for kernel builds, keep this handoff current. Temporary fastboot boot only; preserve Android/backups. No userland/root filesystem boot yet.
- **Patch0020 update:** initial `e71098d` / Actions `38765012466` superseded by scalar-only compile flags. Added -mgeneral-regs-only to all six provider/BearSSL TUs and corecrypto_aes.c; object disassembly now confirms no SIMD. Updated implementation `ab9e23f`, Actions `38765422261` queued/running: https://github.com/dipa-studio/ios-sweet/actions/runs/38765422261. Do not launch another run. portable BearSSL AES CBC and legacy one-block ECB wired directly into SWEET corecrypto_aes.c. Genuine bitslice implementation, no CPU AES/SIMD requirement, heap or fake global crypto table. Source fetched by `0020-stage-bearssl.sh` from official BearSSL git at immutable `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`; MIT license retained, downloaded source stays out of git. Kernel limits include adapter only. All128/192/256-bit keys, CBC encrypt/decrypt, copied contexts, in-place/out-of-place; zero block count and size overflow handled, invalid keys clear context. Seven TUs compile to arm64 objects (`research/aes-objs`), imports resolve, all20 patches apply and sources match (`research/patch0020-check.log`). Sandbox `research/bt23/xnu`, originals `research/pre0020`. NEXT: Actions then actual-kernel NIST/random CBC/ECB checks and >=45s QEMU before another phone boot. Next artifacts out-0020 / bt24 / run24.log. 0020 updated CI pending. Local sources/checks match the scalar-only patch.
- **Latest built kernel0019**, implementation `256d3f0`, patches0001-0019. Actions `38763072907` PASSED (4m5s): https://github.com/dipa-studio/ios-sweet/actions/runs/38763072907. Kernel `xnu/out-0019/kernel.development.sweet`, SHA256 `a27df7568f03fb541cf0e8024c0a594fa178a85054fcf9eba29158cbd4c5ecc0`; checksum verified. Log/source `research/run23.log` / `research/bt23` (no nested .git). Seven pthread source/build/header files and staged adaptations byte-match local compile inputs.
- **0019 hardware photo received** `IMG_20261015_175306.jpg`: pthread registration visible, passes mbinit and DLIL checksum/scheduling, same AES fault as QEMU: PC`0xfffffe00079174e0` (`aes_encrypt_key128`), LR`0xfffffe000767c3a4` (`tcp_init`), FAR0x78, x8=0. Disassembly proves NULL g_crypto_funcs->ccaes_cbc_encrypt. Photo processed; no more0019 observation pending. General crypto table remains absent, other hash/mode operations may still fault after0020. DO NOT bypass TCP or install fake crypto. XNU already contains SHA256/HMAC implementations; AES source there is a dispatch wrapper. Full future provider interface libkern/libkern/crypto/register_crypto.h. Apple/corecrypto default README describes evaluation-only licensing, so no external corecrypto was fetched or staged; BearSSL selected for this portable AES integration.
- **0019 pthread FIXED in QEMU and observed beyond its init on phone**: 45s QEMU `research/q0019`, zero unknown glyphs, then AES panic. Actual-kernel API checks PASS `research/check-pthread19.gdb` / `.log`, guest `research/q0019-api`: function/callback table identity, actual provider pthread_init returns, locks/global hash/threadcall/typed zones initialized, thread_selfid via syscall shim + buffer bounds, process hash init/delete via shims restores NULL. Monitor quit prints expected remote-disconnect after PASS. All fixture guests stopped.
- **0019 provider**: genuine Apple libpthread-514.100.2 commit `a6c70ca0e544347a0377babf748f831577ec24f9`, exact paired release with xnu8796.141.3 in distribution-macOS/macos-135/release.json (`research/macos-135-release.json`). 0019 shell stage runs before patches (workflow copies *.sh); three SWEET wrapper TUs use kext BSD header view, vm_fault_external, renamed provider-side pthread_kern/current_uthread; register real function table at EARLY_BOOT SECOND, real pthread_init later. Stager applies bounded address casts, zero user addresses, proper panic format. SWEET opaque ksyn storage enlarged48->56 for trailing kwe_uth, checked with provider static assertion. Local pre-CI TargetConditionals fixture was syntax-only; artifact bt23 carries actual SDK header and CI is authoritative.
- **Phone0019 temporarily booted** 2026-10-15 15:51:36 UTC: exactly one FASTBOOT/product sweet verified; send OKAY0.406s, boot OKAY0.134s, total0.546s, exit0 (`research/boot0019.log`). Image `shim/out/shim-boot-0019.img`, 16,457,728 bytes, SHA256 `19429a57e486b856880df9e27327ed488e0139345311fdb0350974e72103a9a1`. Phone ADT/payload `shim/out/sweet-0019.adt` / `payload-0019.bin` timer19, unpacked ramdisk byte-match (`check-0019`). EntryPA0xb146b488, kernelendPA0xb2000000, boot_argsPA0xb2004000/top0xb2008000. Last user FASTBOOT+USB reply preceded this boot; do not assume phone is still in FASTBOOT.
- **CPU0018 physically verified** by `IMG_20261015_173951 (2).jpg`: boot CPU0 registered, real SGI1 verified, initialization complete -> BSD/MAC/mbinit -> old pthread-table panic. SweetCPU uses legacy IOCPU, ml_processor_register/processor_start/initCPU, real SGI1 clears SIGPdisabled before max CPUs completion; no forced flags/skipped waits. CPU APIs and two-CPU rejection passed (`research/check-cpu18.gdb`, `q0018-api`, `q0018-smp`). Single core; SMP/suspend unsupported. CURRENT SWEET has NO USE_APPLEARMSMP.
- QEMU next run: generate fresh `tools/mkadt.py research/q0020.adt --qemu` (timer27), mkpayload with `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`, then `tools/qemu-sweet.py PAYLOAD OUT --secs 45 --kernel KERNEL --fail-on-panic`. 12s can miss later panics because IORTC waits30s. Runner snapshot only: panic exit2, blank/incomplete exit3; absence of panic does not mean complete boot. Qualcomm XPU/ABL/panel and phone SMC PSCI require hardware checks.
- Local checker `tools/xnu-cc.py research/run23.log research/bt23 FILTER --per-file`, XNU_CC_OBJDIR for objects. For new BearSSL TUs add XNU_CC_EXTRA with -I bt23/xnu/libkern/crypto/sweet_bearssl/{inc,src}. Patch checker `research/check-patch-order.py` uses temporary upstream index; source fixtures/artifacts ignored under research. GDB mask registers to unsigned64, use function-entry manual-call return traps, discard manipulated guests. No QEMU guest intentionally left running.

## Session log and earlier handoffs (superseded by the active summary above)

### Prior 0019 transition notes (superseded)

- Goal: iOS 16 generation XNU on sweet. Always commit completed work, use gh/Actions for kernel builds, keep this handoff current. Temporary fastboot boot only; preserve Android/backups.
- **0019 build PASSED** Actions `38763072907` (4m5s), implementation `256d3f0`. Kernel `xnu/out-0019/kernel.development.sweet`, SHA256 `a27df7568f03fb541cf0e8024c0a594fa178a85054fcf9eba29158cbd4c5ecc0`; checksum verified. Log/source `research/run23.log` / `research/bt23`; all seven patched files and staged provider adaptations match local compile inputs. Prefer bt23 as the next baseline.
- **0019 pthread FIXED in QEMU**: 45s run `research/q0019` registers provider, passes mbinit, DLIL SUM16 checks and DLIL thread scheduling. Actual-kernel GDB checks `research/check-pthread19.gdb` / `.log` PASS: function/callback table identity, real _pthread_init returns, locks/global hash/cleanup threadcall/typed zones non-NULL, thread_selfid via actual syscall shim with output bounds, process hash init/delete through actual shims restores NULL. Guest stopped after tests; no fixture left running. GDB monitor quit prints expected remote disconnect after PASS.
- **Current next blocker:** QEMU0019 panic data abort FAR0x78 at `aes_encrypt_key128` PC`0xfffffe00079174e0`, called by `tcp_init` LR`0xfffffe000767c3a4`. Disassembly confirms `g_crypto_funcs == NULL`, loading ccaes_cbc_encrypt at+0x78. General crypto registration is still absent; DO NOT bypass TCP or supply fake crypto. Next integrate a genuine compatible crypto implementation/provider (AES first needed here, other digest/mode APIs also used). XNU contains SHA256/HMAC but no AES implementation; its corecrypto_aes.c is a dispatch wrapper. Full crypto table is in libkern/libkern/crypto/register_crypto.h.
- **0019 phone image prepared and verified**, user reports FASTBOOT+USB again; exactly one device/product sweet verified. `shim/out/shim-boot-0019.img`, 16,457,728 bytes, SHA256 `19429a57e486b856880df9e27327ed488e0139345311fdb0350974e72103a9a1`; unpacked ramdisk matches phone payload (`shim/out/check-0019`, `payload-0019.bin`, `sweet-0019.adt`, timer19). EntryPA0xb146b488; kernelendPA0xb2000000; boot_argsPA0xb2004000/top0xb2008000. Usual debug cmdline cpus=1/-noprogress. **Temporarily booted 2026-10-15 15:51:36 UTC**: send OKAY0.406s, boot OKAY0.134s, total0.546s, exit0 (`research/boot0019.log`). User asked for photo after45s. Await the photo; DO NOT reboot again before observation. Bootloader acceptance only; no0019 physical progress observed yet.
- **Hardware photo confirms 0018 progress** (`IMG_20261015_173951 (2).jpg`): real phone prints `SWEET CPU: boot CPU 0 registered, SGI1 verified, initialization complete`, enters BSD/MAC, reaches `mbinit: done`, then the same missing-pthread-function-table panic as QEMU at pthread_shims.c:318. CPU completion/interrupt delivery is now physically observed. The photo was received; no further phone boot performed yet.
- **Patch0019 committed/pushed `256d3f0`; Actions `38763072907` queued/running:** https://github.com/dipa-studio/ios-sweet/actions/runs/38763072907. Do not launch a duplicate. genuine Apple OSS libpthread-514.100.2 provider (immutable commit `a6c70ca0e544347a0377babf748f831577ec24f9`), paired with xnu-8796.141.3 by Apple's `distribution-macOS/macos-135/release.json` (`research/macos-135-release.json`). `0019-stage-libpthread.sh` fetches/stages it before patches; workflow now copies shell scripts too. SWEET-only wrappers compile all three real kernel sources, rename provider-side symbols, use kext header view and vm_fault_external, register the real function table at EARLY_BOOT SECOND. Existing pthread_init then runs the actual provider initialization. No pthread wait/syscall is bypassed. Source staging includes bounded explicit address casts/zero user addresses and panic-format fixes. SWEET kernel opaque ksyn storage enlarged from48 to56 bytes to contain this provider's trailing kwe_uth, with a provider size assertion. Local compile of three provider TUs plus pthread_shims/kern_fork passed; imports resolve and provider exports do not collide with existing kernel symbols. All19 patches apply and changed sources match (`research/patch0019-check.log`). Source sandbox `research/bt22/xnu`, originals `research/pre0019`, objects `research/pthread-objs`. Local TargetConditionals fixture only has target macros for syntax; CI stages the real macOS SDK header and is authoritative. Next Actions artifacts: out-0019 / bt23 / run23.log; run QEMU >=45 seconds. 0019 CI is pending; no 0019 QEMU run yet.
- **Latest kernel implementation `e7d9550`, patches0001-0018; CI `38760004398` PASSED (3m56s)**: https://github.com/dipa-studio/ios-sweet/actions/runs/38760004398. No build is pending. Kernel `xnu/out-0018/kernel.development.sweet`, SHA256 `b3c8d59acc88f93a2c836eb2527a05ed4fbaac7c580a2ea8c3b795137f4aadf1`; checksum verified. Source/log `research/bt22` / `research/run22.log` (baseline for patch0019; no nested .git). New driver/config sources match CI exactly.
- **CPU wait FIXED in QEMU**: SweetCPU uses legacy IOCPU, validates single boot CPU, ml_processor_register -> processor_start -> initCPU real SGI1 -> scheduler/IOKit completion -> ml_set_max_cpus. It does NOT force a flag or skip a wait. CPU APIs passed: Running CPU0, processor_array[0] association, SGI signalling enabled, name, unsupported secondary-start error (`research/check-cpu18.gdb` / `.log`, `research/q0018-api`). Single-core only; SMP and suspend unsupported. Two-CPU topology fails explicitly (`research/q0018-smp`).
- **Next blocker reproduced in 45s QEMU** (`research/q0018-long`, zero unknown glyphs): BSD initialization reaches MAC framework, network mcache and `mbinit: done`, then **`pthread kernel extension not loaded (function table is NULL). @pthread_shims.c:318`**. Do not bypass pthread_init: find/integrate the matching open-source libpthread kernel implementation and register its real function table. A 12s run `research/q0018` looked panic-free before the 30s IORTC wait elapsed; use >=45s or GDB checkpoints for the next iteration. No userland/root filesystem boot yet.
- **Phone 0018 TEMPORARILY BOOTED** 2026-10-15 **15:31:15 UTC**: exactly one FASTBOOT device verified product sweet; send OKAY0.400s, boot OKAY0.134s, total0.539s, exit0. Local log `research/boot0018.log`. This confirms bootloader acceptance only. User was asked for a photo; await it, DO NOT reboot the phone again before their observation. No flashing performed.
- Current hardware image `shim/out/shim-boot-0018.img`, **16,437,248 bytes**, SHA256 **`b7a23595d143a76c72c9fe2cfcc5d77a8f16987b5af1a2703bbffdfa6922908c`**. Phone ADT/payload `shim/out/sweet-0018.adt` / `shim/out/payload-0018.bin` (timer19), unpacked ramdisk exactly matches payload (`shim/out/check-0018`). EntryPA0xb146b488; kernelendPA0xb1ffc000; boot_argsPA0xb2000000/top0xb2004000. Command line `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`.
- QEMU payload/ADT `research/q0018.bin` / `research/q0018.adt` (timer27). Run `python3 tools/qemu-sweet.py research/q0018.bin research/NEW-OUT --secs 45 --kernel xnu/out-0018/kernel.development.sweet --fail-on-panic`; fresh ADT seed for a new payload. Output fb.png/fb.txt/regs.txt/qemu.log/result.json. Visible panic exit2; blank/incomplete decode exit3; no_panic_observed is snapshot-only, not successful boot. -noprogress prevents the graphical indicator corrupting console text. Qualcomm XPU/bootloader/panel and SMC PSCI still need physical validation.
- Patches0014-0017 fixed PRNG registration/direct SHA256, explicit unavailable trust-cache service with rejected loads/no matches, built-in SweetPlatformExpert, and timer PPI from ADT (phone19/default19, QEMU27). All their checks/artifacts are preserved in the history below. CURRENT SWEET has NO USE_APPLEARMSMP; do not assume gCPUIC/gAIC/gPMGR AppleARMSMP globals exist.
- Local single-file checker: tools/xnu-cc.py research/run22.log research/bt22 FILTER --per-file, output via XNU_CC_OBJDIR. Patch checker `research/check-patch-order.py` applies all18 via temporary index, matches sandbox sources. Sandbox for0018 `research/bt21/xnu`, originals `research/pre0018`, objects `research/cpu-objs`. First compiler issues were missing machine/atomic.h and nonexistent getNameSymbol; fixed and both TUs compiled successfully before CI. Fixtures/artifacts under research are local-only/ignored.
- GDB: mask int(register)&((1<<64)-1) for unsigned kernel pointers; use function-entry manual-call return traps, discard manipulated guests. No QEMU fixture is intentionally left running.

### Earlier active handoff snapshot (superseded)


- **0018 milestone:** Actions `38760004398` PASSED (3m56s), checksums passed, new CPU/build files byte-match CI tree. Kernel `xnu/out-0018/kernel.development.sweet`, SHA256 `b3c8d59acc88f93a2c836eb2527a05ed4fbaac7c580a2ea8c3b795137f4aadf1`; log/tree `research/run22.log` / `research/bt22`. QEMU `research/q0018` 12s: boot CPU registered, SGI1 verified, CPU initialization complete, **BSD initialization entered** (copyright, MAC framework, buffer headers), no panic/zero unknown glyphs. Direct CPU API checks PASS (`research/check-cpu18.gdb`, `.log`, `research/q0018-api`): CPU0 Running, processor matches processor_array[0], real SGI enabled, CPU name correct, secondary-start returns KERN_NOT_SUPPORTED. Two-CPU ADT correctly panics before registration (`research/q0018-smp`). Longer 45s run `research/q0018-long` still running; next BSD wait/fault not yet identified.
- **Prepared 0018 phone image:** `shim/out/shim-boot-0018.img`, 16,437,248 bytes, SHA256 `b7a23595d143a76c72c9fe2cfcc5d77a8f16987b5af1a2703bbffdfa6922908c`. Phone ADT/payload `shim/out/sweet-0018.adt` / `shim/out/payload-0018.bin`, usual debug args with cpus=1 and -noprogress. Unpacked ramdisk byte-compared (`shim/out/check-0018`). Entry PA remains 0xb146b488; kernel end PA 0xb1ffc000, boot_args 0xb2000000/top0xb2004000. User reports FASTBOOT+USB ready; read-only check found exactly one product sweet. NEXT: temporary fastboot boot of this verified image, record send/boot results, then await hardware photo without rebooting again.
- Patch 0018 committed/pushed **`e7d9550`**; Actions **`38760004398`** queued/running: https://github.com/dipa-studio/ios-sweet/actions/runs/38760004398. Do not launch a duplicate run.
- Active continuation: patch **0018** implemented in `iokit/Kernel/arm/SweetCPU.cpp` + arm64 source list + exact qcom,kryo470 personality. Keeps legacy IOCPU, accepts ONLY the single boot CPU, fills ml_processor_info from parsed topology with CPU object as cpu_id, calls ml_processor_register and processor_start. initCPU sends SGI1 via ICC_SGI1R_EL1 and waits (1-second deadline) for real cpu_signal_handler to clear SIGPdisabled. Only after successful startup calls ml_cpu_init_completed/cpusRunning and publishes completion/max CPUs. Secondary startup/suspend are explicitly unsupported. Local C++ compile passed for both TUs (`research/cpu-objs`), all imports resolve, object contains dsb ishst / ICC_SGI1R / isb; all18 patches apply in order and match sources. Sandbox `research/bt21/xnu`, originals `research/pre0018`; next CI artifacts should use `xnu/out-0018`, `research/bt22`, `research/run22.log`. User has the phone available; first run QEMU, then prepare/temporarily boot useful hardware progress.
- Goal: iOS 16 generation XNU on Redmi Note 10 Pro / sweet. User wants persistent progress, commits, kernel builds via `gh` Actions, and this handoff kept current across agent switches. Temporary `fastboot boot` only; preserve Android and backups.
- Latest kernel implementation: **`aa3f991`**, patches **0001-0017**. All four new Actions builds PASSED: 0014 `38748543058` (4m0s), 0015 `38749549779` (4m3s), 0016 `38750733996` (4m3s), 0017 **`38751507437` (4m7s)**. Latest: https://github.com/dipa-studio/ios-sweet/actions/runs/38751507437. No build is pending.
- **Current blocker: CPU initialization has no SWEET CPU driver.** QEMU registers SweetPlatformExpert, then kernel_bootstrap_thread waits in **kperf_init_early -> ml_wait_max_cpus**. Confirmed via actual-kernel GDB call stack (`research/locate-cpu-wait2.log` / `.symbols`) and normal-run `max_cpus_initialized == 2` (MAX_CPUS_WAIT). Do not force the flag or remove the wait. Next implement real CPU registration and completion using XNU's CPU/processor interfaces. CURRENT SWEET uses legacy IOCPU (`USE_APPLEARMSMP` is absent); enabling AppleARMSMP is a separate architectural change requiring GIC/PMGR services. No userland/root filesystem boot yet.
- Fixed this session: PC=0 unregistered PRNG callback (0014, built-in HMAC-SHA256 provider + direct entropy/write_random hashing); absent Image4/AMFI trust-cache service (0015, unavailable runtime, loads rejected and queries no-match); missing platform driver (0016, SweetPlatformExpert exact personality); incorrect QEMU timer IRQ (0017, validated ADT PPI property, phone default19/QEMU27).
- **Latest kernel** `xnu/out-0017/kernel.development.sweet`, SHA256 `055a80190ab998b4df9203f1102072dcc78faad1779a537a514e522b682b582f`. Buildtree/log **`research/bt21` / `research/run21.log`**; all nine changed C/C++/header/config files byte-match local checked sources. Checksums passed. Entry PA `0xb146b488`, kernel end PA `0xb1ff4000`. Preferred baseline for patch 0018: `research/bt21/xnu` (no nested .git).
- **QEMU verified**: `research/q0017-noprogress` ran **30 seconds**, no visible panic, **zero unknown glyphs**, IRQ exceptions returned normally (4 IRQs, tickless idle). It still waits for CPU completion, so this is NOT a completed boot. Earlier 15s run `research/q0017` captured 122 IRQs, timer value27, GICR_ISENABLER0 `0x08000002`, and CPU WAIT; graphical progress overlay corrupted 13 glyphs. Add **`-noprogress`** to keep the console readable. New runner classifies incomplete/blank decoding explicitly (exit3 with --fail-on-panic), visible panic exits2. Snapshot-only; use regs/GDB to diagnose stalls.
- Current QEMU payload/ADT: **`research/q0017-noprogress.bin` / `research/q0017.adt`**. Run: `python3 tools/qemu-sweet.py research/q0017-noprogress.bin research/NEW-OUT --secs 30 --kernel xnu/out-0017/kernel.development.sweet --fail-on-panic`. Generate fresh seed with mkadt for a new payload. QEMU virt models generic ARM execution, not Qualcomm XPU/bootloader/panel or the phone's SMC PSCI conduit.
- **Prepared phone image**: **`shim/out/shim-boot-0017.img`**, 16,416,768 bytes, SHA256 `51ff7fe30b4a6668c3452ffe1da922c5ac30cb8de60625c2a652587ecbd0ec92`. Phone ADT/payload `shim/out/sweet-0017.adt` / `shim/out/payload-0017.bin` (timer19). Unpacked ramdisk exactly matches payload (`shim/out/check-0017`). Command line **`debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`**. NOT sent to phone this session: known CPU-driver wait should be addressed in QEMU first. Last actual phone boot remains 0013; user's latest photo `IMG_20261015_160033.jpg` confirmed the old PRNG PC=0 panic.
- Direct QEMU/GDB API checks: PRNG 600-byte chunking, buffer bounds, changing output, zero-length reads/writes, user reseed, contended refresh, generator1 init/output PASS again on latest0017 (`research/check-prng17.gdb` / `.log`, guest `research/q0017-api`). Trust-cache unavailable-service/argument/output cases PASS on0015 (`research/check-trustcache.gdb` / `.log`). Platform registration + target/product normal/truncated/zero/null/bounds PASS on0016 (`research/check-platform.gdb`, `research/check-platform3.log`). Different platform ID correctly retains IOPanicPlatform fallback (`research/q0016-negative`). Timer PPI32 and size2 correctly panic (`research/q0017-badppi`, `research/q0017-badsize`); omitted property selects19 (`research/q0017-default`). These are API/bring-up checks, not crypto certification or SMP scheduling validation.
- Local checker **`research/check-patch-order.py`** verifies all17 patches and changed source equality using a temporary upstream git index. Fixtures/logs/artifacts under research are local-only and ignored. For GDB unsigned kernel pointers, mask `int(register_value) & ((1<<64)-1)`; x30/x29 may be rendered signed. Stop at a function entry for manual-call return traps, and discard each manipulated guest after API tests. All fixture QEMU guests have been stopped.


- User wants persistent progress toward iOS 16, commits for completed changes, builds through `gh` / GitHub Actions.
- Keep this file current at every milestone so another agent can resume immediately, including when usage limits interrupt a session.
- Patch 0017 committed/pushed `aa3f991`, Actions `38751507437` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38751507437. Next artifacts: xnu/out-0017, research/bt21, research/run21.log. Do not duplicate.
- Patch 0016 kernel downloaded/checksum verified: SHA256 `d06336c54baa06e0df6561a8bd7df98a9b9f7502c9381c62d1a44c045cd1ce46`, `xnu/out-0016/kernel.development.sweet`; buildtree/log `research/bt20` / `research/run20.log`. QEMU `research/q0016` (10s) prints platform expert registered, zero unknown glyphs, no panic observed BUT stalls (timer mapping still wrong until 0017). Direct actual-kernel GDB tests passed for registered instance and target/product names, including maxLength=1/0 and NULL, sentinels intact (`research/check-platform.gdb`, `research/check-platform3.log`, `research/q0016-api3`). First two fixture attempts used premature/wrong return breakpoints; discarded, corrected fixture uses start-return observation then a function-entry return trap. No phone boot yet.
- Patch 0016 CI `38750733996` PASSED (4m3s); artifacts not yet downloaded/booted. Patch 0017 prepared to fix QEMU timer fidelity BEFORE interpreting future no-panic/stall runs: actual `qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=research/qemu-virt.dtb ...` timer property has virtual PPI 11 => INTID 27, while phone uses INTID 19. Kernel formerly hardcoded 19. New optional /arm-io/gic virtual-timer-intid property selects the PPI (validate exact u32 and 16..31); missing property retains phone default 19. One RO-late global drives GIC enable and sleh_irq dispatch. tools/mkadt.py emits phone19/QEMU27. Both C TUs compile (`research/timer-objs`), ADT serialize/parse checks confirm values/addresses, all 17 patches apply and match. Sandbox `research/bt19/xnu`, originals `research/pre0017`; initial local sleh compile failed due read-only exported header not updated, fixed export and BOTH recompiled successfully. CI exports normally.
- Architecture correction for next agent: CURRENT SWEET board_config does NOT define USE_APPLEARMSMP; it compiles the legacy IOCPU path, not cpu_boot_thread/IOPMGR AppleARMSMP. Existing roadmap mentions AppleARMSMP as a future approach. Do not look for gCPUIC/gAIC/gPMGR globals in the current kernel: they are absent. Determine the actual CPU-init wait after platform registration before choosing the next driver architecture.
- Patch 0016 committed/pushed as `0dcdf5c`; Actions `38750733996` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38750733996. Next artifacts: `xnu/out-0016`, `research/bt20`, `research/run20.log`.
- Patch 0016 implemented: new `iokit/Kernel/arm/SweetPlatformExpert.cpp`, arm64 source list, built-in personality in KernelConfigTables.cpp guarded by ARM64_BOARD_CONFIG_SWEET. Matches only xiaomi,sweet at score 10000, keeps IOPanicPlatform fallback for other hardware. Uses IODTPlatformExpert base initialization/power domain and createNubs CPU logical-ID annotation; overrides top-level publishing because base requires nonexistent Apple NVRAM /options. Publishes real CPU/top-level nodes, provides bounded target/product names, and registers the platform service. GIC/PMGR and arm-io child-bus drivers remain separate work. Both C++ TUs compile (`research/platform-objs`), all imports resolve against 0015/own objects, all 16 patches apply and sources match. Sandbox `research/bt19/xnu`; originals `research/pre0016`. NEXT: full Actions/QEMU; inspect CPU boot waits after the platform registers, do not equate absence of panic with successful boot.
- Patch 0015 CI `38749549779` PASSED (4m3s), checksum verified. Kernel `xnu/out-0015/kernel.development.sweet`, SHA256 `ca1b424a596c7058550ad1659080a333e7643270f019656401d80bf2b876645f`; buildtree/log `research/bt19` / `research/run19.log`. QEMU `research/q0015` (10s, zero unknown glyphs) passes trust-cache handling and machine lockdown, then panics `Unable to find driver for this platform: "xiaomi,sweet" @IOPlatformExpert.cpp:2319` (IOPanicPlatform fallback). Next concrete work: add a built-in SWEET IODTPlatformExpert personality/class; keep the fallback for mismatched platforms. No phone boot yet.
- Direct trust-cache API checks PASSED in the actual 0015 kernel under QEMU/GDB (`research/check-trustcache.gdb`, `research/check-trustcache.log`, `research/q0015-api`): runtime pointers NULL, hash/UUID queries return NOT_FOUND, typed/untyped/legacy loads return NOT_SUPPORTED, capabilities outputs zero and bounds preserved, invalid-argument cases still reject. Second PRNG check also verifies contended refresh returns immediately (`research/check-prng2.log`, `research/q0014-api2`).
- Patch 0015 committed/pushed `8f851d4`, Actions run `38749549779` running: https://github.com/dipa-studio/ios-sweet/actions/runs/38749549779. Do not duplicate.
- Direct PRNG API checks passed in the REAL 0014 kernel under QEMU/GDB, stopped before trust-cache init (`research/check-prng.gdb`, log `research/check-prng.log`, guest `research/q0014-api`). Invoked read_random for 600 bytes (chunking and buffer sentinel intact), repeated output differed, zero-length read/write succeeded, write_random(64-byte input) succeeded, secondary generator 1 initialized and generated distinct output. QEMU log has no exceptions during these checks; guest was discarded after the tests. This tests API behavior, not a cryptographic certification or SMP scheduling.
- Patch 0015 implemented in `bsd/kern/kern_trustcache.c`: SWEET with missing Image4/AMFI marks both runtime pointers NULL and logs service unavailability. Existing static loader handles NULL. Public load APIs return KERN_NOT_SUPPORTED; hash/UUID queries return KERN_NOT_FOUND; capabilities outputs are zero with KERN_NOT_SUPPORTED. Invalid-input checks remain, and Apple paths/available-provider initialization retain existing behavior. No trust is granted. Local arm64 compile passed (`research/trustcache-objs`), all 15 patches apply and sources match. Sandbox `research/bt18/xnu`, baseline `research/pre0015`. Next: Actions/QEMU and direct unavailable-service API checks using GDB.
- Patch 0014 CI `38748543058` PASSED (4m0s), artifact checksum passed. Kernel `xnu/out-0014/kernel.development.sweet`, SHA256 `741de20c851755d333759d7e668346f3edb4f325efac4cc7c142d80d1f5ff75c`; buildtree/log `research/bt18` / `research/run18.log`. QEMU run `research/q0014` (10s, zero unknown glyphs) prints `SWEET PRNG: HMAC-SHA256 registered`, passes commpage initialization, loads vnguard policy, THEN panics `image4 interface not available @kern_trustcache.c:253`. PC=0 fault FIXED IN QEMU. No new phone boot performed.
- Next work: SWEET currently has neither AppleImage4 nor AMFI. `trust_cache_runtime_init` unconditionally requires both; unlike BCM2837, SWEET enters this startup path. Investigate an explicitly unavailable trust-cache runtime with rejected loads/no query matches while providers are absent; preserve all Apple-platform checks. Merely skipping the startup call would leave later AMFI/IMG4 dereferences unsafe. Existing load_static_trust_cache already handles trust_cache_rt == NULL.
- QEMU runner now supports `--kernel MATCHING_KERNEL --fail-on-panic`: runtime font extraction from the payload produces exact `fb.txt`, plus `result.json`; a visible panic returns 2. Baseline 0013 re-run `research/q0013-text` detects the same NULL callback panic with zero unknown glyphs. Synthetic glyph roundtrip (including literal `?`) and invalid-VA rejection passed. Screen rows remain circular, not a full log; `no_panic_observed` does not prove successful boot. README has the invocation.
- Patch 0014 committed/pushed as `be61f99`; Actions run `38748543058` is running: https://github.com/dipa-studio/ios-sweet/actions/runs/38748543058. Do not start a duplicate build.
- 2026-10-15 active: hardware photo `IMG_20261015_160033.jpg` confirms the same PC=0 instruction-fetch abort as QEMU (LR `0xfffffe0007443fe4` = read_random). Independently reproduced with kernel 0013 in `research/q0013` (8-second run); prior allocator fix works. Latest tooling commit reviewed: `8d66d2d`.
- Patch 0014 implemented: registers a built-in HMAC-SHA256 DRBG provider at EARLY_BOOT/FIRST, before commpage creation. A shared mutex serializes output/reseed across CPUs (correctness first; no per-CPU scaling yet), initgen validates CPU identities, refresh uses try-lock and the existing entropy filter/health tests. SWEET entropy accumulation and write_random now use the built-in SHA256 directly; no incomplete g_crypto_funcs table is registered. The existing bootloader-seed requirement remains enforced.
- Local validation: both PRNG translation units compile to arm64 objects in `research/prng-objs`; no SHA256 wrapper imports remain. All 14 patches apply in CI order and resulting PRNG sources match local inputs. Sandbox `research/bt17/xnu`, originals `research/pre0014`, checker `research/check-patch-order.py`. NEXT: Actions kernel build, download to `xnu/out-0014`, run the QEMU payload before requesting another phone boot. Baseline QEMU ADT/payload are `research/q0013.adt` / `research/q0013.bin`.
- 2026-10-15 08:15 QEMU EMULATION WORKS (`tools/qemu-sweet.py`, see "QEMU emulation" below). Patch 0013 VERIFIED in QEMU: boot passes kmem_crypto_init and ulock_initialize. NEXT BLOCKER (reproduced in QEMU, expected on hardware too): `pmap_create_commpages -> random() -> RandomULong -> read_random -> prng_funcs.refresh == NULL` (instruction fetch abort at pc 0, lr read_random+0x34). Nobody calls `register_and_init_prng` (closed corecrypto kext does it on Apple). Fix = patch 0014: SWEET-registered cckprng_funcs provider (osfmk/prng/prng_random.c); note random_cpu_init also panics while !prng_ready. The 07:26 hardware boot of 0013 will most likely show this same panic.
- Previous hardware boot: patch 0013 (`a3a46e2`) CI `38686345689` PASSED (3m44s), image `shim/out/shim-boot-0013.img` was temporarily booted on 2026-10-15 at 05:26 UTC: send OKAY (0.407s), boot OKAY (0.134s), total 0.546s. Superseded by the hardware/QEMU PC=0 observation and patch 0014 work above.
- Latest kernel implementation commit: `8f851d4`. Latest successful kernel CI: `38749549779` (patch 0015), https://github.com/dipa-studio/ios-sweet/actions/runs/38749549779.
- CURRENT image: `shim/out/shim-boot-0013.img`, 16,412,672 bytes, SHA256 `110d1c46aee4b60253db24b6535e9781c4cf0fd25ca2a70e9016cf81378cea25`. Kernel `xnu/out-0013/kernel.development.sweet`, SHA256 `f0b1d375995316c550ce7014ff234d8271e9b9ac2857795ceff41b76ba2cd740`, plain arm64. CI log confirms 0013 applied; checksums passed. Entry PA `0xb146b488`, kernel end PA `0xb1ff4000`.
- Current log/buildtree: `research/run17.log`, `research/bt17` (extracted, no nested .git; preferred baseline for patch 0014). Payload/ADT: `shim/out/payload-0013.bin`, `shim/out/sweet-0013.adt` (includes /defaults). Unpacked ramdisk byte-compared with payload in `shim/out/check-0013`.
- Patch-0012 original image: `shim/out/shim-boot-0012.img`, 16,412,672 bytes, SHA256 `cf4b8ec4a65a51888d71c17d09cd0831d6b761b90e2531fdb8e575c32f4fd24a`. Kernel `xnu/out-0012/kernel.development.sweet`, SHA256 `69b78f49f6459cc63a0af44e7cf73c1779ef72130ef72b12ae86c88183ffc742`; checksums passed and sweet_early_relocate symbol present. Entry PA remains `0xb146b488`, kernel end PA `0xb1ff4000`.
- Patch-0012 log/tree: `research/run16.log`, `research/bt16` (extracted, no nested .git; preferred baseline for next patch). CI log confirms 0012 applied. Payload `shim/out/payload-0012.bin`, ADT `shim/out/sweet-0012.adt`; packed ramdisk byte-compared with payload in `shim/out/check-0012`.
- Active payload command line: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam`.
- Latest observed photo: `/home/dipa/Downloads/IMG_20261015_071346.jpg` (local only). Stages 9 (kernel framebuffer VA), 10 (TTBR0 off/TLB flush), 11 (physmap table pointers) and 12 (calling pmap_bootstrap) all visible. Persistent diagnostics are VERIFIED ON HARDWARE.
- pmap.c:1987 is the assert after SecureDTLookupEntry(NULL, "/defaults", &entry). The pmap-max-asids property itself is optional; an empty /defaults uses XNU's MAX_ASIDS. Do not bypass the assertion or guess an ASID limit.
- Previous payload-only fix image: `shim/out/shim-boot-0012-defaults.img`, SHA256 `3ff61a268853c6ea7cbf284833f1346a1d791980ca366b6e1a215a86e040512b` (16,412,672 bytes). Same kernel/CI as 0012; ADT 2516 bytes (`shim/out/sweet-0012-defaults.adt`), root /defaults present with no ASID override. Payload `shim/out/payload-0012-defaults.bin`; unpacked ramdisk exactly matches payload (`shim/out/check-0012-defaults`). This boot reached the allocator crypto NULL-pointer fault below.
- Latest hardware photo: `/home/dipa/Downloads/IMG_20261015_071608.jpg` (local only). Normal serial framebuffer console works. VM bootstrap/zone initialization, logging, scheduler, IPC, KDP and ulock_initialize print before the kmem_crypto_init data abort. gPanicBase warnings are expected because PLATFORM_PANIC_LOG_DISABLED is configured, and are not the faulting call.
- Faulting instruction: `ldr x0, [x8, #0x150]` in crypto_random_kmem_ctx_size; x8 = NULL g_crypto_funcs. Source: libkern/crypto/corecrypto_rand.c and osfmk/vm/vm_kern.c:2448. rand.h explicitly permits NON-cryptographic kmem RNGs, but requires no locking in generation and context size <=256. Do not substitute read_random here: the global PRNG provider is also unregistered and kmem generation must remain lock-free. early_random may seed contexts during single-CPU bootstrap; it uses the built-in HMAC-SHA256 DRBG with the host-generated ADT seed.
- Patch 0013 keeps the replacement entirely private to vm_kern.c: SplitMix64 state is 8 bytes per CPU/sizeclass, initialized using early_random. Uniform selection uses rejection sampling to avoid modulo bias. Other platforms retain the original corecrypto calls; SWEET's generic crypto APIs/registration remain untouched. Reference: https://prng.di.unimi.it/splitmix64.c (Sebastiano Vigna, public domain).
- Validation: vm_kern.c compiles to arm64 (`research/kmem-rng-objs`, using `research/run16.log` + `research/bt16`). Undefined symbols include early_random and NO crypto_random_* references. All 13 patches apply in CI order and match the checked source. Original saved as `research/vm_kern.pre0013.c`.
- RNG host checks: extracted actual new helper in `research/kmem-rng-check.c`; 16,384 outputs match the author's downloaded reference (`research/splitmix64-reference.c`), 147,456 bounded outputs satisfy their limits (including 1 and UINT64_MAX), and a forced rejection advances correctly. Full kernel crypto services and the registered kernel PRNG provider remain future work; this patch only supplies the allocator RNG contract.
- Patch-0011 artifacts: kernel `xnu/out-0011/kernel.development.sweet`, log `research/run15.log`, extracted buildtree `research/bt15` (no nested .git). CI log confirms patch 0011 applied; artifact checksums passed. Kernel is plain arm64, entry VA `0xfffffe000746b488` -> PA `0xb146b488`, kernel end PA still `0xb1ff4000`.
- Patch-0011 image: 16,412,672 bytes, SHA256 `29ad5cbfc752473de4af2c471a24a8b34f92da4bff5afec65c614b90cd3ad53a`. Kernel SHA256 `8ac3a956894b09f7bd0d890de8c5de38c65513287feb1e1bcd12db8ca772ee5c`. Payload/ADT in `shim/out/payload-0011.bin`, `shim/out/sweet-0011.adt`. Header unpacked/checked in `shim/out/check-0011`.
- First patch-0011 attempt timed out waiting for FASTBOOT; superseded by the successful send/boot recorded above. Do not repeat the boot before the user reports the current screen.
- Patch-0011 photo: `/home/dipa/Downloads/IMG_20261015_070218.jpg` (local only). Visible stages 1-8 and old red shim text; entry PA B146B488, boot args PA B1FF8000 / top B1FFC000. No evidence yet locating execution after stage 8.
- Patch-0012 sandbox: `research/bt15/xnu` (NO nested .git). Proposed permanent debug VA: VM_MIN_KERNEL_ADDRESS + ARM_TT_L2_SIZE, in the reserved gap below gVirtBase and above low globals; map the same PA framebuffer as Normal-NC in the final TTBR1 before arm_vm_physmap_init changes phystokv(). Keep the early panic sink until normal kprintf startup.
- Patch 0012 validation: all 4 changed C translation units compiled to arm64 objects in `research/post-ttbr0-objs`; internal header export added to osfmk/arm64/Makefile and recreated locally in EXPORT_HDRS for pe_serial.c. All 12 patches apply in CI order and match all 6 edited files. Baselines are saved under `research/pre0012/`.
- Actual mapping-helper host model (`research/early-map-check.c`, barrier stubbed only for host compilation) confirms: VA `0xfffffe0002000000`, L1 index 2016, L2 index 1; descriptor `0x006000009c000605` = framebuffer PA, Normal-NC, outer-shareable, EL1 RW, PXN+UXN. All other L2 entries preserved. The gap is excluded by ARM_LARGE_MEMORY's pmap_virtual_region(0).
- Next boot command line MUST add `-enable_kprintf_spam`: DEVELOPMENT disables kprintf_output by default even when DB_KPRT is set (`pexpert/arm/pe_kprintf.c`). Planned command line: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam`.

### Patch 0012 screen legend (after original stages 1-8)
- 9: kernel framebuffer VA live; 10: TTBR0 off / TLB flushed; 11: physmap pointers rebound.
- 12: calling pmap_bootstrap; 13: pmap_bootstrap returned; 14: early I/O page tables ready; 15: arm_vm_init returned.
- 16: calling serial_init; 17: fbcon config read / mapping; 18: fbcon mapping returned; 19: serial_init returned; 20: kprintf startup complete.
- Normal serial initialization clears the screen, so earlier stages may disappear before 19. If serial exists, early console disables after 20; otherwise early panic output stays enabled.
- Local source sandbox: `research/bt13/xnu` (git baseline `623b060`, patches 0001-0008 already in baseline; uncommitted export edits correspond to patch 0009). Preserve those edits when producing patch 0010.
- Previous compile log/tree: `research/run13.log`, `research/bt13`; previous kernel: `xnu/out/kernel.development.sweet`. New successful log/tree: `research/run14.log`, `research/bt14`; new kernel: `xnu/out-0010/kernel.development.sweet`.
- Current device access: patch-0012 image was temporarily started from FASTBOOT; no adb connection is expected. Never flash partitions for bring-up; use `fastboot boot` with the shim image.
- Patch `0010` lives in `xnu/patches/0010-sweet-early-framebuffer-debug.patch`. Its exact compiled sources are under `research/bt13/xnu`; generated objects are in `research/early-objs`.
- Local checks passed: all 5 changed/new translation units compiled to arm64 Mach-O objects; new console only imports `_doprnt` and `iso_font` (both exist in the previous kernel). All 10 patches applied to a temporary upstream git index in CI order, and all 7 resulting changed source files matched the local compile inputs.
- Reproduce changed-file compilation: `XNU_CC_OBJDIR="$PWD/research/early-objs" python3 tools/xnu-cc.py research/run13.log research/bt13 arm_init.c arm_vm_init.c kern/debug.c arm64/start.s osfmk/arm64/sweet_early.c --per-file` (create the output directory first). The helper now accepts explicit new source paths absent from the old artifact's Makefiles and rejects empty selections.
- Device command result: `adb reboot bootloader`, then `timeout 30s fastboot boot shim/out/shim-boot-0010.img`: send OKAY (16028 KB), boot OKAY, total 0.541s. This confirms bootloader acceptance only, not XNU progress.
- The successful boot above was on 2026-10-14. On 2026-10-15, `timeout 45s fastboot boot ...` timed out waiting for a device; no image was sent. Boot image SHA256 rechecked and unchanged. No new build is needed to repeat the trial.
- Earlier retry: `timeout 90s fastboot boot shim/out/shim-boot-0010.img` timed out (exit 124). This is superseded by the successful 2026-10-15 repeat boot recorded above; do not repeat it until the user reports the screen.
- User observation confirms MMU-on C entry, platform expert, CPU topology/bootstrap, startup bootstrap and cpu_init all complete. Failure is within arm_vm_physmap_init (non-XNU_MONITOR branch), before stage 8 / TTBR0 teardown. Old red shim text remains behind the early console; this is expected because only written rows are cleared.
- New edit/compile sandbox is `research/bt14/xnu` (extracted artifact, NO nested .git; `git diff` there would act on the parent project). Use a saved baseline file / generated unified diff, or explicitly initialize a local baseline before editing. `research/bt13/xnu` does have its own baseline git repo.
- Exact user-reported panic: `arm_vm_init.c:1606 Assertion failed: (temp_ptov_table[ptov_index - 1].va + temp_ptov_table[ptov_index - 1].len) <= physmap_end`.
- Root cause: MacOSX build sets `ARM_LARGE_MEMORY=1`; `XNU_MONITOR` is off. Block alignment in arm_vm_physmap_slide can add gaps, but the non-PPL large-memory physmap_end omitted the padding already present in the small-memory branch. Patch 0011 reserves PTOV_TABLE_SIZE*ARM_TT_TWIG_SIZE (8*32MB) in both the aperture bound and its L1 allocation; monitor/PPL builds retain zero padding. The assertion is preserved.
- Validation: `arm_vm_init.c` compiled to an arm64 object using `research/run14.log` + `research/bt14` (object in `research/physmap-objs`). All 11 patches apply in CI order and produce exactly that source. Host harness `research/physmap-check.c` extracts the real XNU slide function with page-table-write stubs: all 65,536 page-aligned slides within 1GB checked against the current kernel memory layout; old bound fails 65,504 cases, padded bound fails zero; worst overrun 0x1ffc000 bytes. Baseline source saved as `research/arm_vm_init.pre0011.c`.
- CI: https://github.com/dipa-studio/ios-sweet/actions/runs/38638702083 (success; artifacts and full log downloaded).
- New image: `shim/out/shim-boot-0010.img` (16,412,672 bytes, SHA256 `ed2c270e0081bad5ff517be415f9bfe8805463aea7866732203fa97ac737e64e`). Previous image preserved as `shim/out/shim-boot-pre0010.img`.
- New kernel SHA256: `3ee05270cc291c49589a101678f904b1b69cb2f2575e05398bce8a4e052f409a`. Artifact checksums passed; arm64 (not arm64e), early-console symbols present. Payload entry VA `0xfffffe000746a488` -> PA `0xb146a488`; kernel end PA `0xb1ff4000`. Command line remains `debug=0xc serial=3 cpus=1 keepsyms=1`.

### Patch 0010 screen legend
- Bottom-left white 64x32 tile: assembly entered `start_first_cpu`, before page tables/MMU.
- Text `SWEET EARLY 1`: entered `arm_init` with MMU on; 2: platform expert done; 3: CPU topology done; 4: CPU bootstrap done; 5: startup bootstrap done; 6: CPU init done; 7: entering `arm_vm_init`; 8: leaving bootstrap TTBR0.
- `SWEET EARLY PANIC:` prints a copy of panic arguments before normal panic handling needs per-CPU/thread state.
- In patches 0010/0011 the console disabled before TTBR0 invalidation; patch 0012 replaces that with a persistent kernel VA alias and diagnostics through normal serial/kprintf startup (see its legend above).
- Screen contents require a physical observation from the user: adb cannot capture a framebuffer after Android has been replaced by a bare-metal kernel.

## QEMU emulation (2026-10-15)
- `tools/mkadt.py OUT.adt --qemu` = same ADT with QEMU virt GICD 0x08000000 / GICR 0x080a0000 / CNTFRQ 62.5 MHz.
- `tools/qemu-sweet.py PAYLOAD OUTDIR [--secs N] [--hmp "x /8gx ADDR"] [--gdb]`: host-side copy of shim/loader.c
  (segments + ADT + boot_args at their PAs, 6-instruction stub sets x0 and jumps), `-M virt,gic-version=3 -cpu cortex-a76`
  (same ID_AA64MMFR0 as Kryo 470), 4G RAM so 0x9c000000 fb and 0xb0000000 kernel are plain RAM.
  Outputs `fb.png` (the screen), `regs.txt`, `qemu.log` (exceptions with ESR/FAR/ELR). `--gdb` halts with a stub on :1234
  (system `gdb` + `set architecture aarch64`, `hbreak *VA` works with the MMU on).
- Not emulated: Qualcomm XPUs, ABL, real panel, PSCI conduit (QEMU virt uses hvc). Hardware still decides those.
- Example: `python3 tools/mkadt.py /tmp/q.adt --qemu && python3 tools/mkpayload.py xnu/out-0013/kernel.development.sweet /tmp/q.adt /tmp/pq.bin --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam' && python3 tools/qemu-sweet.py /tmp/pq.bin /tmp/q`

## Layout
- `backup/`  partition dumps + full sdb..sdf LUNs, `SHA256SUMS` (all verified on-device == local)
- `dtb/`     live FDT (from OrangeFox), PixelOS boot.img dtb, dtbo, dts versions
- `shim/`    bare-metal boot shim (`./build.sh` -> `out/shim-boot.img`)
- `research/` mainline sm7150 dts, XNU pe_fiq.c etc, PocketDarwin patches
- `tools/unpack_bootv2.py`

## Device access
- No root in PixelOS 17 (user build, adb root disabled). Root = `fastboot boot recovery/recovery.img` (OrangeFox R12.0_2, temp boot, not flashed).
- `fastboot boot` works for stock boot.img, OrangeFox, and our shim.
- Shim resets itself via PSCI SYSTEM_RESET; IMEM restart_reason 0x77665500 is seen
  (bootreason=bootloader) but ABL still boots Android, so it does not stop in fastboot.

## Hardware facts (live DT + shim measurements)
- Framebuffer (ABL cont splash): 0x9c000000, 1080x2400, stride 4320, a8r8g8b8, panel is DSC *video* mode (no kick needed).
- RAM: 0x80000000+2G, 0x100000000+0xfcc00000. Live reserved-memory = PixelOS base dtb + sweet dtbo fixups.
- GICv3: GICD 0x17a00000, GICR 0x17a60000 (stride 0x20000, 8 PEs). GICD_TYPER 0x007B041D: SecurityExtn=1 (DS=0).
  ABL leaves GICD_CTLR = 0x10 (ARE_NS on, groups off). Writing 0x13 reads back 0x12: EnableGrp1A (bit1) is the one that matters.
- GIC Group 1 VERIFIED (shim 2c): vtimer INTID 19 = 30 IRQs/300ms, ptimer INTID 18 = 30, SGI 1 to self = 10/10.
  ICC_SRE=1 already, ICC_CTLR 0x8440, priority/enable writes stick. GICR_IGROUPR0 and GICD_IGROUPRn read 0 from NS
  even though Group 1 delivery works (RAZ in NS view) - do not use them as a health check.
  INTID 28 shows pending constantly (unknown PPI 12).
- Timer 19.2 MHz. Qualcomm PPI numbering: secure-phys/phys/virt/hyp = PPI 1/2/3/0 -> vtimer INTID 19, ptimer 18 (NOT 27/30).
- PSCI 1.1 via SMC, SMCCC 1.1. CPU_ON ok; SYSTEM_SUSPEND and SYSTEM_RESET2 unsupported.
- SMP: CPU_ON works for all 7 secondaries (MPIDR 0x81000n00).
- Entry EL1, MMU off, x0 = DTB (magic ok). ID_AA64MMFR0 = ...101122: 16K granule YES, 4K YES.
- No serial console: debug UART SE is in SPI mode on sweet. Screen is the only debug channel.

## XNU implications
- XNU GICv3 path (pexpert/arm/pe_fiq.c, VMAPPLE) puts the timer (PPI 27) in Group 0 = FIQ. With SecurityExtn=1,
  Group 0 is secure -> impossible from NS EL1. Need: Group 1 IRQ, ICC_IGRPEN1, INTID 19, and the timer handled in the IRQ path.
- XNU VMAPPLE PSCI uses `hvc`; sweet needs `smc`.
- PocketDarwin NonAppleGenericCpu.patch only swaps -mcpu=apple-h7 for -march=armv8-a.

## Milestone 3 (2026-10-14): 16K MMU verified
- ABL loads the Image at 0x80080000 (base 0x80000000 + 0x80000) regardless of text_offset=0.
- 16K granule, T0SZ=28 (L2 start, 32MB blocks), MAIR 0x44ff00, TCR 0x1409CB51C, PARange=2 (40-bit PA).
- Only own image pages WB (Qualcomm XPUs: never map carve-outs cacheable); fb Normal-NC; rest Device.
- AT S1E1R checks all as expected; unmapped -> translation fault L2.
- Fill fb: 110 ms (MMU off) -> 3.5 ms (NC). Memory loop 70 ms -> 0.7 ms. vtimer + SGI IRQs still fine with MMU on.

## XNU loader design notes (xnu-8796)
- Entry: `_start` -> `start_first_cpu`, x0 = boot_args (PA), MMU off, EL1. boot_args: Revision/Version 2,
  virtBase/physBase/memSize/topOfKernelData, Boot_Video (fb 0x9c000000, rowBytes 4320, 1080x2400, depth 32),
  deviceTreeP/Length (ADT), CommandLine[608], bootFlags, memSizeActual.
- start.s maps [kernel mach header .. physBase+memSize) with cacheable BLOCKS. On Qualcomm every XPU carve-out
  must therefore lie below the kernel: use physBase = 0xb0000000 (above ramdump_fb 0xaf000000+16M,
  cdsp_sec, qseecom), end before rmtfs 0xfde01000 -> memSize ~0x4d000000 for bring-up.
- Base ADT: iPhone 8 d20ap DeviceTree from iOS 16.7.16 IPSW (tools/remotezip.py + tools/adt.py; LZFSE payload).
  Template values are empty: the loader must fill chosen/*, memory-map, cpu*/reg etc. like iBoot.
- XNU build: GitHub Actions (dipa-studio/ios-sweet, xnu.yml) via blacktop/darwin-xnu-build, MACOS_VERSION=13.5.
  New-clang fixes in xnu/patches; Xcode 16 iig is incompatible (IORPC.kernelContent) -> macos-14 runner, oldest Xcode.

## Kernel architecture plan (SWEET machine config)
- First stock build (VMAPPLE) is arm64e (PAC). Kryo 470 = ARMv8.2, no PAC -> SWEET must build plain arm64
  (HAS_APPLE_PAC is only defined for __arm64e__, so arm64 drops PAC automatically).
- Template: VMAPPLE (16K pages, GICv3 code in pe_fiq.c) minus APPLEVIRTUALPLATFORM hypercalls/paravirt PAC,
  plus BCM2837 (generic ARM: no APPLE_ARM64_ARCH_FAMILY Apple sysregs). Do not link the KDK lib<cfg>.os.*.a.
- Timer: XNU expects it on FIQ (sleh_fiq -> rtclock_intr). SWEET: GIC Group 1, so sleh_irq must ack ICC_IAR1,
  route INTID 19 to the same timer path, SGIs to cpu_signal_handler, EOI via ICC_EOIR1.
- SMP/interrupts: open-source AppleARMSMP (USE_APPLEARMSMP) needs an IOInterruptController (AIC role) and an
  IOPMGR (CPU power -> PSCI CPU_ON via SMC). Plan: built-in IOKit classes (SweetGIC, SweetPMGR, platform
  expert) compiled into the kernel and matched via iokit/KernelConfigTables.cpp -> no closed kexts needed.
- Debug: xnu patch 0003 sweet-fbcon = kprintf drawn on the framebuffer (ADT node "sweet-fbcon").
- ADT /arm-io ranges must not map to parent 0 (pe_arm_get_soc_base_phys()==0 disables serial): use parent 0x1000000.

## Status 2026-10-14 late: first XNU boot attempt
- SWEET kernel builds (CI run 38635343732, patches 0001-0009). Verified offline: arm64, no PAC, no >v8.2 code,
  70300 classic relocs all in range (slide 0 = no loader work), entry = __start.
- Boot: shim loader worked (initrd 0x846F0000-0x855FE3E0, BOOTARG 0xB1FF8000, JUMP 0xB146A488) but screen stayed
  red -> xnu died/hung before serial_init (fbcon). kprintf is a no-op before STARTUP_SUB_KPRINTF.
- Historical next-step plan (patch 0010, now implemented and built; see active handoff above): early fb debug channel
  1. start.s start_first_cpu (after mov x20,x0 / mov x21,#0): PA marker band (MMU off, direct store to 0x9c000000).
  2. start.s after both create_bootstrap_mapping calls: add ONE block entry to the existing V=P L2 table
     (x25 + 2*PGBYTES; do NOT call create_bootstrap_mapping again: fb and RAM share L1 entry 0, it would replace
     the L2 table). Entry = TYPE_BLOCK|VALID|SH(OUTER)|ATTRINDX(CACHE_ATTRINDX_WRITECOMB=1)|AF|PNX|NX|0x9c000000,
     index = (va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT.
  3. new osfmk/arm64/sweet_early.c (+files.arm64 entry): sweet_early_mark(n) colored bands at screen bottom and
     sweet_early_putc using iso_font via VA=PA while the bootstrap TTBR0 is live; disable before
     arm_vm_init's set_mmu_ttb(invalid_ttep) (arm_vm_init.c:2148).
  4. markers in arm_init after PE_init_platform, ml_parse_cpu_topology, cpu_bootstrap, kernel_startup_bootstrap,
     cpu_init, before arm_vm_init.
  5. panic hook at top of panic_trap_to_debugger: va_copy + _doprnt(fmt, &copy, sweet_early_putc, 16).
  Verify with tools/xnu-cc.py (compile+symbols) and patch-order simulation before the single CI run.
