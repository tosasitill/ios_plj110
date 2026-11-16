# X9：去左右黑边，保留 UIKit / 键盘逻辑尺寸

## 结果与范围

已实现并离线打包一个**默认关闭**的 `x9_fill=1` 候选。源提交 `e8c5d2ec`，整合提交 `4cc05e25`（吸收 main `ff9e900f` 的CPU/GPU/显示休眠更新），分支
`feat/display-fill`，继承状态栏/灵动岛候选 `5af3d156`。不扩大 UIKit 屏幕尺寸；
在独立逻辑画布合成，再横向铺满物理面板，HID 使用同一变换的逆映射。

**没有手机启动/触摸/键盘/灵动岛实测，没有设备操作，没有 CI run ID。**
编译和 host 测试通过不代表原生 SystemAperture、应用 safe-area 或所有应用状态栏已通过。
2026-11-16 用户批准将候选合入 main 并删除 `feat/display-fill`；此次为源码收尾，
不代表已获实机验收或批准设备写入。主 checkout 的 SMS 分支/未提交 NOTES 未修改。
合并时保留最新 main `7ca335c5` 的 Apple 服务交接；相对候选 `51fbd533` 无生产代码变化，
既有 v2 镜像仍对应 `4cc05e25` 的已构建源码，不冒称重新构建了最新 main。

## 1. 黑边来自哪里（运行时优先）

`~/330.txt` 的实际输出：

```text
SWEET FB: AppleCLCD 1256x2760 stride 5120 ... presented as 1242x2760
X9 HID: panel 1256x2760 -> display 1242x2760 offset -8,0
SWEET FB: swap ... surface ... 1242x2760 ... a 0,0 1242x2760 b 0,0 1242x2760
```

既有路径：CA 的 1242px 宽画面复制到 panel 的 x=8..1249，剩下左8/right6像素清黑。
笔记“左右7px”是理想居中的名义值；实际 16B 对齐将 offset 舍入为 -8。
这些是物理像素（平均约2.333pt@3x），**不是已证实各7pt**，仍需物理屏幕观察。

既有107.txt/源码记录过直接申报1256px时：keyboard frame 为414pt，planes却为418.67pt，
切换123被判为resize并重新加载。此次不再次走已失败的直接扩大ADT宽度方案。

## 2. 当前候选完整链

```text
ADT /arm-io/disp0 canvas-* = 1242x2760
  → IOMobileFramebuffer GetDisplaySize = 1242x2760
  → UIKit / CA / 全宽键盘 = 414x920pt @3x
  → IOSurface + swap source/destination rectangles（逻辑像素）
  → 原有 pending/ready GPU fence + IOSurface holds
  → presentFill：合成进独立 cached 1242x2760 canvas，stride5120
  → pixel-centred nearest-neighbour x:1242→1256，y:2760→2760
  → 未被扫描的LK物理slot / WC stores + dsb sy
  → 原有 x9QueuePresent / fresh TE

S3910 validated report（max12559x27599）
  → 原有 raw→physical pixel mapping
  → SweetHIDTouch：normalized16.16 = (physical_pixel+0.5)/physical_extent
  → backboardd 使用逻辑屏幕尺寸与既有方向变换
  → UIKit hit-test（同一逻辑画布）
```

共同几何头：`x9/kernel/src/iokit/Kernel/arm/X9DisplayGeometry.h`。输出查表：

```text
source_x = floor((physical_x + 0.5) * 1242 / 1256)
HID_x16 = floor((physical_x + 0.5) * 65536 / 1256)
HID_y16 = floor((physical_y + 0.5) * 65536 / 2760)
```

HID不能再减8px，也不能再乘一次1242/1256；backboardd已经乘逻辑尺寸。
所有逻辑列都输出，14列各重复一次，画面横向放宽约 **1.127%**，垂直保持不变。
这不是应用获得“418.67pt真实宽度”，而是统一的虚拟屏幕与物理输出变换。
内部截图仍是逻辑尺寸，不能凭内部截图证明玻璃屏幕左右已铺满。

### 保留和隔离

- 默认 surface 在 fill 模式强制为逻辑尺寸RAM buffer，不能让用户态在物理扫描帧上原地画。
- 独立逻辑canvas保留damage之外的像素，**从不从已拉伸的panel读回**，无累积拉伸。
- 沿用changed-layer mask语义：不是enable mask；显式移除layer0才清保留帧。
- 完整source crop/byte offset/descriptor length/bpr/element/format检查；负destination正常clip，
  不将整个负坐标层错误移到原点。BGRA/RGBA/ARGB与premultiplied alpha复用原backend。
- 两套scanout slot仍是物理1256x2760；fill复用现有shown/pending排除与GPU fence，
  不写DSI/MDP/scaler新寄存器，不改GPU插件。fill不进入legacy direct/staged presenter。
- console/fade是物理输出，不能污染独立逻辑canvas。canvas分配失败拒绝显示启动，
  不静默退回centered display配fill HID。
- 非fill保留原路径、ADT、默认surface大小与触摸偏移；修正HID负数左移UB为等价乘65536。
- 键盘、普通应用、状态栏、岛图层都在逻辑canvas合成后统一输出，不能单独拉伸键盘或窗口。

## 3. 灵动岛与状态栏

这轮没有更改上一轮原生岛路径：`x9_ui=1` → artwork subtype2556 → MG cache →
SBFEffectiveArtworkSubtype / SpringBoard Jindo / SystemAperture。详见
`docs/2026-11-16_reverse-statusbar-island-report.md`。

只改artwork，不改D22身份、ProductType或屏幕canvas。状态栏hook仍只注入SpringBoard、
仅接受20H392 UIKitCore UUID，原layout先reset再整体加20/nativeScale pt。
fill只改x，所以原20px竖直位移不变，岛层与其他层共享横向变换。

**仍未证明**：Jindo运行时返回值、真正岛展开/避让/命中、wrapper裁剪、各app独立状态栏。
20px位移不是已全局覆盖每个应用的状态栏，也不单独移动原生岛窗口。
fill不自行调整safe-area/状态栏申报高度，不伪造这些已验证。

`x9_ui=1` 首次启动会装自有hash-addressed dylib并一次重建MG缓存，上一轮未获上机确认，
本轮也未执行。联合镜像**须用户确认这些持久动作后再测试**。
仅去黑边镜像未添加该UI开关/新MG epoch；若曾启用岛，普通关UI不能保证清旧subtype缓存。

## 4. 离线验证（实际结果）

- `test_fill.py`：3项exit0，实际C++ helpers穷举1256/2760输入、14重复列、像素中心inverse、
  full frame、padding、8轮无反馈、crop/负clip/offset/长度/alpha/无部分越界写、
  **模拟**键盘底层+岛形overlay与20px位置。这不是苹果键盘或原生岛运行测试。
- 原`test_display.py`：4项exit0（artwork-only ADT变化、默认不变、开关投影、真实C状态栏算术）。
- LLVM22 + 显式Xcode14 SDK的ASan/UBSan测试exit0。
- 两真实overlay单文件cross compile r1均exit0。后续日志改动亦被整包重新编译覆盖。
- `prepare-local.py --check` exit0。
- `build.py all` 初版与整合新main后的v2均实际 **103/103 overlay（3 prebuilt）**，exit0，完整kernel/link/shim/boot成功。最终只推荐v2，不回退main的新电源策略。
- 初版与v2同kernel的baseline、combined、rollback打包均exit0；初版作为中间证据归档，不交付为推荐。
- `verify_fill.py` 从实际boot v4解压LZ4到Image，逐字节比Image/payload/ADT，读取真实
  SWEETXNU cmdline/subtype/canvas，初版四份和v2四份均exit0，各SHA与metadata一致。

保留失败：初次host错猜Xcode工具路径；Apple15 ASan在Darwin27初始化崩溃；LLVM自动用
SDK27与旧linker不兼容；初次FB单文件因`ushort ?: 4` narrowing失败，改`4u`后成功。
v2吸收main时NOTES冲突亦记录，保留完整incoming后仅插入fill/UI交接；FB自动合并，实际整包重编通过。
日志在主checkout `x9/out/display-fill-20261116/`；删除功能树前的完整 ignored 归档在
`x9/out/display-fill-retirement-20261116/archive/`，逐文件 SHA-256 校验，不隐藏失败或算成成功。

### 复跑

```sh
# 在仓库根执行；原 display-fill 工作树将在收尾后删除。
cd /Users/tosasitill/ios_plj110
DEVELOPER_DIR=$(python3 -c 'import json; print(json.load(open("x9/build/local.json"))["developer_dir"])')
export DEVELOPER_DIR
SDK=$(xcrun --sdk macosx --show-sdk-path)
mkdir -p x9/out/display-fill-replay/tmp
export TMPDIR="$PWD/x9/out/display-fill-replay/tmp"
CC="$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang" \
  SDKROOT="$SDK" python3 x9/display/test_display.py
CXX=/opt/homebrew/opt/llvm@22/bin/clang++ CXXFLAGS="-isysroot $SDK" \
  X9_TEST_SANITIZE=1 python3 x9/display/test_fill.py
python3 x9/build/prepare-local.py --check
python3 -I x9/build/build.py all \
  --boot-config x9/out/display-fill-20261116/baseline-330.json \
  --boot-arg x9_fill=1 --name x9-fill-replay
python3 x9/display/verify_fill.py x9/out/x9-fill-replay \
  --kernel x9/out/x9-fill-replay/kernel.development.sweet --fill 1 --island 0
```

## 5. 镜像与上机验收

交付至主checkout的`x9/out/`，每个boot **14938112 B**，Image **31907840 B**：

| 目录 | fill/UI | cmdline bytes | boot SHA-256 |
| --- | --- | --- | --- |
| `x9-fill-v2-20261116` | 1/0 | 603 | `561fe368f4646f79c0cc65fc08d20d13624687486e93f9f9982b9a4abe4b60db` |
| `x9-fill-baseline-v2-20261116` | 0/0 | 593 | `dbc3ad9e66272b1c159fa4386cbb5cdb46e13af293f88754d6e792bb11c70f3b` |
| `x9-fill-island20-v2-20261116` | 1/1 | 598 | `fd48b4612b6a6754d7de355e4422f66c02fb3d6652f89b179d1b42a140d6c504` |
| `x9-fill-rollback-v2-20261116` | 0/0 + mgwipe | 600 | `a60b08b59c3e1775f7f32c33cbd3a3c50ee9500c2b9ed51362fe598c276b6be8` |

fill相对330基线只增加`x9_fill=1`。combined为容纳607B限制移除该kernel中的no-op
`x9_bt_root=1`；此kernel本就没有嵌入BT root payload，**不是rootboot13 Bluetooth整包**，
不删除已安装BT文件，不能声称其它硬件回归已通过。
插件仍为GPU280，未生成plugin-package；若设备仍为330日志确认的280，**无需mailbox**。

先在无新UI缓存操作的条件下手动临时boot，保持其它变量不变：

```sh
fastboot boot x9/out/x9-fill-v2-20261116/boot.img
```

验收不能只看中间日志：

1. 日志必须同时出现`X9 DISPLAY FILL:`、`X9 HID FILL:`、`X9 FILL frame ... refused=0`，
   GetDisplaySize仍1242x2760；用全白背景确认**物理面板**左右边缘没有software黑条。
2. 竖屏应用的UIScreen/窗口逻辑尺寸继续414x920pt、@3x；检查键盘通知frame与实际full-width
   keyboard布局一致。反复ABC/123/emoji/中文切换、候选栏/长按/多点、开关键盘，不重载跳回ABC。
3. 键盘和全屏测试的左/中/右/四角目标命中；检查左右原黑边处新画面、底部home gesture、
   顶部通知/控制中心边缘滑动，不能只测屏幕中心。
4. 横竖屏切换、app启动退出、overlay/弹窗/dim、息屏唤醒，检查没有错位或残影。
5. 观察原cadence/latency/gpu-wait/TE计数；CPU横向resample新增成本要实机测，
   不能以host耗时保证120fps。若性能不合格关闭fill，后续单独验证硬件/GPU scaler。
6. 用户确认MG/dylib动作后才boot`x9-fill-island20-v2-20261116/boot.img`，验证真岛活动/展开收起、
   safe-area避让、点击和状态栏20px位移，按上一轮报告采日志；外观provider警告不等于岛可用。

只测fill时恢复：`fastboot boot x9/out/x9-fill-baseline-v2-20261116/boot.img`。
若启过UI，恢复2436需用户确认后**单次**boot rollback（`x9_ui=0 x9_mgwipe=1`），
随后boot普通baseline；不重复mgwipe，不把UI cache持久状态误当fill影响。
