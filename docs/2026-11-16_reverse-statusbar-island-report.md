# 状态栏与原生灵动岛：20H392 识别、启用及 20 px 偏移候选

## 结论与边界

当前系统**已经启用岛式状态栏 provider，尚不能据此认定原生灵动岛已开**。
`330.txt` 同时证明内核写入 `DynamicSplitMedium` 成功、SpringBoard 实际选中了该类。
原生灵动岛的另一条路径是 **MobileGestalt artwork → SystemAperture**。
本次在原件中确认它识别 `2556 / 2796`，而本项目默认 ADT 仍提供 `2436`。

增加默认关闭的 `x9_ui=1` 候选：artwork 改为 `2556`；一次性重建 MG 缓存；
安装一个自有、哈希寻址的 dylib，只对 SpringBoard 的整个内层状态栏加 **20 物理像素**。
不改 D22 / iPhone10,3 兼容身份，不改 1242×2760 画布、字体、各图标尺寸或触摸坐标。

**尚未上机验证**。编译、签名、离线算术测试不等于屏幕效果或原生岛工作。
首轮只注入 SpringBoard；应用进程内独立绘制的状态栏不在首轮覆盖范围内。
原生岛自身的窗口不随状态栏一起移动。

## Evidence → Finding → Path

| 证据 | 判断 | 路径 / 重放 |
| --- | --- | --- |
| `330.txt`：`X9 STATUSBAR: ... DynamicSplitMedium ... errno=0`；SpringBoard `WARNING: Using overridden ...` | provider preference 实际生效，不只是源码开关 | `kern_exec.c::sweet_x9_seed_uikit_statusbar` → `com.apple.UIKit/UIStatusBarVisualProviderClassName` → UIKitCore `__UIStatusBarGetVisualProviderClassForScreen`，`0x1891aaa80` |
| `product_identity.py` 提供 subtype 2436；MG 原件含 `artwork-device-subtype`，接受 2556/2796 的分支 | artwork 和 ProductType 分离，可以只换 UI 画像 | ADT `/product/artwork-device-subtype` → MG `oPeik/9e8lQWMszEjbPzng.ArtworkDeviceSubType` |
| SBF `0x18e6093fc` 的 once body `0x18e619144`，两个原 CFString 分别为上面的 MG key 与 `ArtworkDeviceSubType` | SpringBoard 有缓存的有效 artwork subtype，不会随进程内临时修改立即更新 | `SBFEffectiveArtworkSubtype` |
| SBUI 原件 `0x1a345a39c`，SpringBoard bundle 分支比较 `0x9fc / 0xaec` | 2556 / 2796 是原生岛识别值；2436 不通过这层 | `SBUIIsSystemApertureEnabled`，非 SpringBoard 进程使用 availability 路径 |
| SBS `0x191555160`，once body `0x1915551a0`；`SpringBoard/Jindo` 条件后读 MG 相同字典并比较 2556/2796 | native availability 还依赖 feature flag，不能只改状态栏类 | `SBSIsSystemApertureAvailable` |
| root 克隆原 `System/Library/FeatureFlags/Domain/SpringBoard.plist`：Jindo 为 `DevelopmentPhase=FeatureComplete` | 原发行资产已含 Jindo；不等于证明设备运行时 feature flag 返回值 | 未修改该 plist；如岛未出，下一次直接采样该函数结果 |
| UIKitCore `-[UIStatusBar_Modern layoutSubviews]` `0x189244f3c`：wrapper.bounds → inner `_UIStatusBar.setFrame:` | 精确的整个状态栏平移插入点；每轮原函数重置，避免累加 | 原 IMP → inner frame.origin.y 加 `20 / nativeScale` |

这里的地址均为 **20H392 提取镜像的 unslid VM 地址**。
提取文件的部分共享 selector/stub 指针不完整，因此不把 `llvm-objdump` 的越界最近符号
当作真实被调函数。上表的 CFString 内容、具名函数、编码与比较指令分开记录。

## GitHub 参考

1. [DynamicCowTS](https://github.com/matteozappia/DynamicCowTS)，
   commit `372fed4738b576a190a78325203d8e48fe16fa03`：
   `DynamicCowTS/View/ContentView.swift::replacePlistData` 修改
   `CacheExtra["oPeik/9e8lQWMszEjbPzng"]["ArtworkDeviceSubType"]`，Pro=2556 / Pro Max=2796。
2. [DynamicCow](https://github.com/matteozappia/DynamicCow)，
   commit `136d05a023f906e41c1b55a8199d5366ad092411`：同样的字段与数值，使用 MDC 写入。

本项目有可控 XNU/ADT，不需要这些项目的 MDC/TrollStore 写入手段，
也不照搬其 red-bar 分辨率修改。DynamicCowTS README 的支持范围不包含 16.7.16；
这里只引用原字段和数值，并用本机 **20H392** 的比较分支独立验证。

## 实现及开关

- `x9/tools/product_identity.py` / `mkadt.py`：`island=False` 保持原件；True 只提供 2556。
- `x9/tools/boot-xnu.py`：从同一个 `x9_ui=1` 解析结果决定 ADT。
- `kern_exec.c`：`x9_ui=1` 默认触发一次 UI artwork epoch 的 MG 缓存重建；
  明确 `x9_mgwipe=0` 禁止缓存写入，适合静态诊断但旧 cache 可能仍阻止岛。
- `x9/kernel/src/bsd/kern/x9_ui_install.inc`：仅 opt-in + launchd pid1 + writable UFS root；
  自有 dylib 先验证内嵌 SHA，再使用既有不覆盖不同文件/拒绝 symlink 的写入器。
  SpringBoard exec 前再次验证 size/SHA，已有任何 `DYLD_INSERT_LIBRARIES` 时保留原链并跳过 UI。
  此项在 AppSync **之后**，不会抢占 AppSync。
- `x9/display/statusbar.m`：只接受 UIKitCore UUID
  `1242978a-2c2c-3781-8d6c-9777edce2804`、原方法类型 `v16@0:8` 和 getter `@16@0:8`。
  使用 ObjC 方法表替换，不改 executable text、不申请 W+X trampoline。
- `x9_ui=0` / 不带参数：不安装、不注入；原画布、默认 artwork、原状态栏 provider 保持。

20 physical px 在 @3x 上为 **6.666666667 pt**，不是 20 pt / 60 px。
只调整竖屏主屏；横屏和外屏不动。应用 safe-area、状态栏申报高度没有被改写；
剪裁、状态栏触摸、避让区域和动态岛动画需在首轮确认，不能用离线测试替代。

## 构建与检查

全部从仓库根执行，使用 NOTES.md 的 repo-local 输入布局：

```sh
python3 -I x9/build/prepare-local.py --check
python3 -I x9/display/build.py \
  --developer-dir "$(python3 -c 'import json;print(json.load(open("x9/build/local.json"))["developer_dir"])')" \
  --out x9/out/display-standalone

# Host SDK 需与链接器匹配；本机 CLT 的 MacOSX27 SDK 与旧 linker 不兼容。
DEV="$(python3 -c 'import json;print(json.load(open("x9/build/local.json"))["developer_dir"])')"
CC="$DEV/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang" \
SDKROOT="$DEV/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" \
  python3 -I x9/display/test_display.py
```

已通过：4 项离线测试（默认 ADT 不变、唯一 subtype delta、开关投影、实际 C 的
20/nativeScale 算术与 10000 次 reset-then-shift）；iOS arm64 dylib 编译/签名及
`codesign --verify --strict`；真实 kern_exec overlay 单文件编译。

完整候选构建采用 **330.txt 的 593 B 原启动参数 + `x9_ui=1` = 601 B**，不砍其它硬件参数。
对应 scratch `baseline-330.json` 仅含启动配置，不含设备序列号或转储。

源码 commit **00478b31**，完整 `build.py all` 实际 exit 0，**103/103 overlay（3 prebuilt）**。
同一 kernel 的 baseline、enabled 和 rollback 三份打包均通过。
从实际 `payload.bin` header 读取 cmdline 和内嵌 ADT，分别为
baseline **2436 / 593 B**、enabled **2556 / 601 B**、rollback **2436 / 600 B**；
baseline → enabled 的 cmdline 精确只增加 `x9_ui=1`。
三者 kernel SHA 相同；Image 内精确嵌入 payload，Image/payload/boot SHA 与 metadata 一致。
完整镜像 **31887360 B**，boot.img **14929920 B**。

已无覆盖复制到主 checkout：

| 用途 | 本地 boot.img | SHA-256 |
| --- | --- | --- |
| 启用候选 | `x9/out/x9-ui-island20-20261116/boot.img` | `7c9ac834fcceb221cc03bb18e840fc9eda22aad1fa64a667007f27048644ce7c` |
| 同 kernel 未启用基线 | `x9/out/x9-ui-baseline20-20261116/boot.img` | `e40867c2c7bddf79900c427a4c45212d3a0d5eda141ffc46b202ecb5b279f3de` |
| 单次恢复缓存 | `x9/out/x9-ui-rollback20-20261116/boot.img` | `45706c63fa5400294399bd6945f0d7f917789df869a335ad7fe9487cab9435eb` |

无 GitHub Actions run（本轮使用本机 Xcode/LLVM 编译），无设备启动结果。
2026-11-16 用户批准通过继承此实现的 `feat/display-fill` 合入 main 并清理该分支。
这是默认关闭候选的源码整合，不是原生岛已开启或已实机验收；原 `feat/statusbar-island`
分支/工作树不在此次清理范围。新电源策略已纳入 v2 联合镜像，见 display-fill 报告；
以上三份初版 UI 镜像保留作历史证据，不冒称最新 main 整包。

## 上机验证与恢复（需用户确认，尚未执行）

第一轮会产生两项持久写入：**重建 MG 缓存**及**创建自有哈希 dylib/epoch**。
没有 flash、GPT 修改、Apple 二进制替换或 mailbox 插件变更。
用户确认后才临时 `fastboot boot`；先不要把此候选称为已生效。

1. 内核应出现 `X9 UI: own statusbar bridge install ... errno=0`、
   `X9 MGWIPE: ...` 和 `X9 UI: verified SpringBoard statusbar environment appended`。
   若出现 `existing injection chain kept ... skipped`，该轮不会平移，不能把岛式 bar 当作成功。
2. iOS logd subsystem `com.x9.display` 应出现 UUID/方法契约通过、
   `root layout hook installed` 和 `entire inner status bar shifted 20 native px`。
3. 对照同一竖屏截图：时间、蜂窝/Wi-Fi、电量等整体下移 20 px；
   字体尺寸、X 坐标和桌面位置不变。多次进出锁屏/控制中心，检查没有每轮再加 20 px。
4. 启动计时器或音乐；必须看到**原生 SystemAperture 的内容、紧凑/展开状态以及交互**，
   静态黑药丸或两侧图标 provider 本身不算开启成功。
5. 验证无底部裁切、无状态栏点击错位、无应用 safe-area 内容重叠。
   再决定是否扩大到应用进程，不在首轮无证据全局注入。

恢复分两件事：`x9_ui=0` 禁止 hook/恢复 ADT，**缓存仍可能保留 2556**。
完整恢复使用同基线的 **`x9_ui=0 x9_mgwipe=1` 单次**临时启动，重建回 2436，
同时只删除自有 UI epoch，使下一次启用不被旧 marker 阻断；随后再使用不带 mgwipe 的普通镜像。
哈希 dylib可留在自有目录但不会加载，不必为恢复删除任何 Apple 文件。

恢复参数额外容纳 mgwipe：本轮 main/UI kernel 没有嵌入 Bluetooth root payload，
`x9_bt_root=1` 在其中无安装作用，恢复配置省略这一 no-op 参数以符合 607 B 上限。
**不会恢复/删除已经安装的 Bluetooth root 文件**；这不是 rootboot13 Bluetooth 整包，
不能据本轮 UI 构建判断 Bluetooth、XNU IRQ 或其它硬件回归通过。
初次 rollback 打包因证据脚本误用 `adt.parse` 的 tuple 返回值而未生成配置，实际 exit 1；
修正后 exit 0，失败日志与成功日志分开保留。

## 本地证据位置

原 UI 工作树 `.worktrees/statusbar-island/x9/out/statusbar-island-20261116/` 保留，
以下原 UI 证据不在此次删除范围（display-fill 归档保存的是 fill 证据及生成的 UI bridge）：

- `uikit-provider-selection.txt`、`uikit-modern-layout.txt`：实际入口/整条 root layout。
- `sbf-subtype*.txt`、`aperture-enable.txt`、`sbs-aperture-init.txt`：原件启用分支。
- `modern-method-contract.json`：按已资格的原 IMP 地址提取类型编码；不冒认缺失的 selector 指针。
- `display/`、`display-build.log`：签名 own-code，Apple cache / 下载参考只在 ignored scratch。
- `tests.log` 与 `tests-initial-sdk-failure.log`：成功与失败分开保留。
- `kernel-single.log`、`local-check.log`、`full-build.*`：确切构建结果。
- `packaged-verification.json`：三份镜像的实际 payload cmdline/subtype 和 SHA 检查。

这是一份可上机验证的候选实现，不是原生灵动岛已运行的证明。
