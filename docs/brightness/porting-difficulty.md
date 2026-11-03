# MT6993 显示亮度（backlight）移植难度评估

- 设备：OPPO Find X9（`PLJ110` / `OP5E17L1`，MediaTek MT6993），X9 分支
- 面板：`panel_ac287_p_7_a0020_dsi_cmd`，1256x2760，MIPI DSI command-mode AMOLED
- 目标 OS：未修改的 iOS 16.7.16（`20H392`）用户land + 本仓库 XNU `8796.141.3`
- 源码基线：`/Users/tosasitill/HelioDarwin/oppo-source`（GKI `android_kernel_oppo_mt6993` + `modules/vendor/oplus/kernel/...`）
- 参考方法论：<https://unixnu.com/zh/porting/display>（《显示与亮度》）第 3、4 节
- 日期：2026-11-03
- 性质：**只读评估**。本文未改动任何生产代码，未构建产物，未接触设备。

---

## 1. 摘要与难度结论

**难度：中低。是本仓库已评估过的子系统里工作量最低的一项——低于音频（中高），远低于 GPU/Metal 与 Wi-Fi（高）。**

决定性理由有三条：

1. **传输层已经在实机上打通了。** 参考页给的亮度范例是 `AppleARMBacklight.cpp` + `GeniI2c.h`，即"高通 GENI I2C 驱动一颗分立 WLED IC（KTZ8866）"。我们这个面板两个都不是：亮度是 DDIC 内部寄存器，靠 **MIPI DCS 0x51 长写**送达。而这条通路我们**已经实测点见过玻璃**（见 §7 DEVICE VERIFIED 条目），并且顺手证明了**不需要 MTK CMDQ/ADP 硬件命令队列**，普通寄存器长写就能落地。
2. **要写的编码已经在源码里、且字节序已经是对的。** 现有 [`setPanelLevel()`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1454) 组的是 `{0x51, level>>8, level}`（MSB first），与设备树里 `51 00 00` 的模板逐字节吻合；需要改的只是 nits→DBV 的量程常量。
3. **iOS 侧服务外壳已完整存在**，`AppleARMBacklight` 已发布并带 `backlight-control` / `brightness-nits` / `max-nits` / `IOMFBBrightnessLevel`——正是参考页第 4 节列的接口清单。缺的只是板型门。

| 阶段 | 内容 | 难度 | 是否碰 DSI 链路 |
| --- | --- | --- | --- |
| A | shim/早启动期单笔 0x51 写，肉眼三档验证 | 低 | 是（已实测过） |
| B | 内核 `AppleCLCD` 的 X9 DSI 传输替换 + 解门发布服务 | 中 | 是（未测语境） |
| C | 滑块端到端 + 数字调光性能回归 | 中 | 否 |
| D | 渐变调光 `FadePeriod` / 显示电源耦合 / LHBM / 屏下指纹压暗 | 中高 | 是 |

一句话分工结论：**参考页可复用的是它的服务形状与验收口径，不可复用的是它的传输与寄存器编码全部。**

---

## 2. 范围与非范围

**在范围内**：0x51 DBV 通路的传输层、编码层、iOS 侧亮度服务接入与验收。

**不在范围内**（同一 DT 文件里确实存在，但都属于"锦上添花"，且都有独立的实机风险）：

- LHBM / DC 双亮度模式（`oplus_display_dc.c`、`oplus,dsi-lhbm-*-command` 空表）
- AOD 亮度（`51 0D BB 03 DD` 变体，多出的两字节语义未查）
- 屏下指纹压暗（`oplus,ofp_need_keep_apart_backlight`）
- 温补、CABC/PQ/global dimming
- 面板供电与复位（LK 已做完，见 §5）

---

## 3. 证据与标签词表

- **SOURCE CONFIRMED** — 本次会话里直接读到该文件该行内容
- **DEVICE VERIFIED（记录在案）** — 物理设备上观察到的行为，本文转引自本仓库代码注释；本次未重跑
- **NOT TESTED** — 逻辑成立但未在硬件上验证
- **INFERENCE** — 推断，附依据

本文链接的每条行号都在本次会话里用 `Read`/`grep -n` 复核过。**`mtk_drm_setbacklight()` 往下的完整驱动调用链未读**（应用户要求停止扫盘），相关条目已降级为 INFERENCE 并集中列入 §13。

---

## 4. 参考页对照：UniXNU(display) 的可复用边界

参考页第 3、4 节原文点名 `AppleARMBacklight.cpp`（KTZ8866 控制器 OS 服务示例）与 `GeniI2c.h`（GENI 传输状态机），并要求"更新板级引擎映射、时钟投票和中断路由"、"参考 GENI 路径采用固件加载的引擎代码与时序"。**GENI 是 Qualcomm Serial Engine，KTZ8866 是 I2C 分立 WLED 驱动 IC，两者在 MT6993 上都不存在。**

参考页自己给了分叉口，原文：**「使用 DSI 命令或 PWM 背光的设备，需要在面向 OS 的亮度服务下实现相应传输与亮度编码」**。X9 正是这一支，所以第 3、4 节的传输与编码要全部自写。

| 参考页条目 | 我们的对应物 | 可复用性 |
| --- | --- | --- |
| `GeniI2c.h` GENI 状态机 / `geni_i2c_prepare`/`_start_write`/`_event`/`_timeout` | MTK DSI 命令寄存器长写 | **零**。仅"prepare→start→完成或取消后才能发下一笔"这条串行纪律可照搬 |
| KTZ8866 寄存器编码（`BL_BRT_LSB/MSB`、`BL_EN`、`vddpos/vddneg`） | DCS 0x51 两字节 DBV | **零** |
| 引擎映射 / 时钟投票 / 中断路由 | 无对应：亮度写不需要独立时钟域或 IRQ；X9 的 keys/touch/gauge 全部是轮询 | **零**，且这是**减法**（省掉全部前置条件） |
| `IODisplayParameters` 亮度范围与 `backlight-control` | [`SweetFramebuffer.cpp:592-614`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L592) 已有 | **高**，形状照用 |
| `brightness` + `FadePeriod` 属性 | 现有 CPU 渐变（topaz 分支） | 中 |
| `sheng-backlight=1` probe 开关 | 现有 [`sweetbl`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L601) boot-arg | **高**，语义相同 |
| `native_backlight_stats` 统计（发布/请求/回读/中断/失败） | 需自建；X9 已有成熟的 `IOLog` + 磁盘 ring 惯例 | 中，口径照抄 |
| 验收：最低/中间/最高对应回读与可见亮度 | 直接采用 | **高** |

**SOURCE CONFIRMED（negative）**：`oppo-source/android_kernel_oppo_mt6993/` 那份是 GKI common kernel，全树对 `mt6993` 零命中、无 `drivers/misc/mediatek/`、无板级 DTS。真正的板级描述在 `modules/vendor/oplus/kernel/devicetree/`，也就是本文 §5 的出处。

---

## 5. 设备侧真实机制：设备树已经给出答案

**SOURCE CONFIRMED**，全部读自 `oppo-source/modules/vendor/oplus/kernel/devicetree/oplus-display/`：

| 事实 | 出处 |
| --- | --- |
| 亮度包模板 `39 00 00 00 00 00 03 51 00 00` | `panel-ac287-p-7-a0020-dsi-cmd-common.dtsi:5-7` |
| 亮度写在 HS 模式 | 同文件 `:203` `oplus,dsi-set-backlight-command-state = "dsi_hs_mode"` |
| 关屏序列：`...0A 00 01 28` / `...96 00 01 10`（0x28 disp off、0x10 enter sleep，带延时） | 同文件 `:1-4` |
| AOD 亮度 `51 0D BB 03 DD`，且先做页选 `FF 5A A5 00` | 同文件 `:8-20` |
| 量程 1..4094、`led-bits=13`、`trans-bits=13`、`normal-max=3515`、默认 35% | `mt6993-zhujiang-24203-display-overlay.dtsi:13-24` |
| 面板供电：`vddi` 1.8 V LDO、`vddr` GPIO 105、`1p2` 1.2 V LDO、`vci` 3.0 V LDO | 同文件 `:80-108` |
| PWM 混合调光阈值 0（即本面板不用 PWM） | `panel-ac287-p-7-a0020-dsi-cmd.dtsi:1449` |
| ESD 健康值：读 `0x0A` 应为 `0x9C` | 同文件 `:1482-1493` |
| 消费该属性的驱动 | `modules/vendor/oplus/kernel/display_driver/oplus_drm/oplus_dsi_panel_cmd.c:70`（属性表）、`:321`（`DSI_CMD_SET_BACKLIGHT`） |

**包格式解码**（对照 `:1-4` 的关屏序列反推，`INFERENCE`）：7 字节前缀为 `[数据类型][0][0][标志][延时 hi][延时 lo][净荷长度]`，其后是净荷。故 `39 00 00 00 00 00 03 | 51 00 00` = DCS long write、无延时、3 字节净荷 = `0x51` + 16 位参数。`0x0DBB`、`0x0E7A` 都落在这个位置，与"后两字节是占位、由驱动填入 level"一致。

**板级归属确认**：`mt6993-zhujiang-24203-display-overlay.dtsi` 引用 `panel-ac287-p-7-a0020-dsi-cmd.dtsi`，而 `24203` 是唯一引入 ac287 的板；面板名与 `x9/README.md` 记录的 X9 面板名逐字相同。

---

## 6. level → DBV 编码

三个锚点（**SOURCE CONFIRMED**）：

| 锚点 | 线上值 | 十进制 | 对应量程 |
| --- | --- | --- | --- |
| AOD | `51 0D BB` | 3515 | 恰等于 `oplus-brightness-normal-max = <3515>` |
| 我们点亮玻璃用的值 | `51 0E 7A` | 3706 | 高于 normal-max，即 HBM 档 |
| 模板占位 | `51 00 00` | 0 | `min-brightness = <1>`、`max = <4094>` = `0x0FFE` |

**INFERENCE**：DDIC 侧近似直通（level 原样大端两字节写入），gamma/感知曲线在 Linux 用户态 HAL 完成。依据是 3515 这个数在 DT 里同时以"属性"和"线上字节"两种形式出现。这意味着**我们不需要移植曲线就能得到单调、可用的亮度**；最坏情况是低档偏亮/偏暗，可以后续补表。

需要落地的改动：现有 [`setProperties()`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1484) 把 nits 映射到 **1..0x7ff（2047）**，那是 topaz 面板的 11 位域；X9 要换成 **1..0xffe（4094）**，HBM 上限 3706。[`setPanelLevel()`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1457) 的 MSB-first 组包已经正确，无需改动。

---

## 7. 本仓库已有资产

| 位置 | 内容 | 状态 |
| --- | --- | --- |
| [`x9/shim/display.c:497-508`](file:///Users/tosasitill/ios_plj110/x9/shim/display.c#L497) | `display_backlight_on()`：LP 模式（config LONG，不带 HSTX）向 `0x3e730000` 发 `0x39` + `51 0E 7A`，前后 `0x52` 回读、`0x0A` ESD | **DEVICE VERIFIED（记录在案）**：同文件 `:490-496` 注释记载 pan36 的 LP 版落地、玻璃点亮；pan35 的 HS 变体失败（DI=0x02 Ack&Error、error bits 0x0003 SoT error，时钟 lane 未运行）；更早 pan4 的长写曾把 DSI 链路写死。**当前无调用点，未接线** |
| [`x9/shim/display.c:479`](file:///Users/tosasitill/ios_plj110/x9/shim/display.c#L479) | DSI 基址对 `0x3e730000` / `0x3e740000` | SOURCE CONFIRMED |
| [`SweetFramebuffer.cpp:592-614`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L592) | `AppleARMBacklight` 发布，带 `backlight-control`/`brightness-nits`/`max-nits`/`IOMFBBrightnessLevel` | 代码就绪，**双重门挡住**：`fDsi != NULL` 且 `sweetbl=1` |
| [`SweetFramebuffer.cpp:583`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L583) | `fDsi` 只在 `sweet_board_qcom()` 分支映射（`0x05e94000`） | X9 走不到 → 无亮度服务 |
| [`SweetFramebuffer.cpp:575-579`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L575) | MT6789 分支已映射 MTK DSI + MUTEX 的先例 | SOURCE CONFIRMED，形状可套 |
| [`SweetFramebuffer.cpp:1401-1441`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1401) | `dsiSend()`：CMD_DMA 模型，偏移 `0x004/0x03c/0x048/0x04c/0x084/0x090/0x110/0x15c` | **高通寄存器布局，X9 不可用**，需替换 |
| [`SweetFramebuffer.cpp:1454-1517`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1454) | `setPanelLevel` / `setProperties` / `blWork`（去重 + thread_call） | 结构可用，仅量程与传输需换 |
| [`x9_lk_console.h:64`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/pexpert/arm/x9_lk_console.h#L64) | pexpert 已映射 MT6993 DSI `0x3e730000`，`:65` 检查偏移 `0x34`，`:157` 读状态 `0x0c`（bit2 = TE_RDY） | SOURCE CONFIRMED，内核侧已有该块 |
| [`x9_lk_console.h:152`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/pexpert/arm/x9_lk_console.h#L152) | 注释明确「No interrupt mask, TE routing, **DCS**, PHY or panel mode is reprogrammed」 | 现状刻意回避 DCS，是 R1 的根因 |
| [`X9Gauge.inc`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/X9Gauge.inc) | MT6993 I2C0 FIFO master + PMIF，实测读到外部 gauge `type=0x..` | 能力存在但**亮度用不上**（无 I2C 背光 IC） |
| `x9/xnu-patches/0104`/`0105`/`0106`/`0110`/`0096` | topaz 的 DCS 亮度、`AppleARMBacklight`、DCS on/off、opt-in 门、平滑渐变 | 形状参考 |

---

## 8. 传输层工作分解

- **W1 — 拿到 X9 上的 DSI 长写。** 把 shim 里已验证的 `dsi_cmdq_send_k(base, 0x39, payload, len, mode)` 的 **LP 语义**（config LONG、无 HSTX）复刻到内核侧，落在 pexpert 已映射的 `0x3e730000` 上。参考页"当前传输完成或取消后再发起下一次传输"的串行纪律必须保留；`fBlLock` 已有。
- **W2 — 量程与 clamp。** `setProperties` 的 `0x7ff` → `0xffe`，最小 1，HBM 3706 上限策略单独定档。
- **W3 — 解门。** 在 X9 分支为亮度建立独立判据（不能复用 `fDsi`，它是高通分支产物），保留 `sweetbl` 作为 opt-in。
- **W4 — 回读与统计。** 每笔写入前后读 `0x52`、周期性读 `0x0A` 校验 `0x9C`；实现参考页口径的 `native_backlight_stats` 等价物（发布/请求/写入/回读/超时计数）。

**注意**：参考页 `dsi-set-backlight-command-state = "dsi_hs_mode"` 与我们的实机结论冲突——Linux 运行时 DSI 时钟 lane 在 HS，所以它能用 HS；我们从 LK 继承的链路上 HS 长写产生 SoT error（pan35，DEVICE VERIFIED 记录在案）。**采用 LP 通路，不要照抄 DT 的 HS 声明。**

---

## 9. iOS 侧接入

现有实现已覆盖参考页要求的服务面，无需新增用户land 组件：

- 匹配键：`backlight-control`（[:605](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L605)），CoreBrightness 靠它找到服务
- 亮度单位：`brightness-nits` / `max-nits`（[:606-607](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L606)）
- 帧缓冲侧属性：`IOMFBBrightnessLevel`（16.16 nits），由 CoreBrightness 通过 `setProperties` 下发（[:1472-1503](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1472)）
- 渐变的 `FadePeriod` 尚未接；建议 M3 之后再开

`setProperties` 里那句 `if (n == NULL || fDsi == NULL) return kIOReturnUnsupported;`（[:1478](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L1478)）是 X9 上必须一并改掉的判据。

---

## 10. 里程碑与验收口径

| 里程碑 | 内容 | 验收口径 |
| --- | --- | --- |
| M0 | 单笔 LP 长写（shim 内，接线 `display_backlight_on`） | 肉眼可见变暗/变亮 + `0x52` 回读变化 + `0x0A` 仍为 `0x9C` + 桌面帧不中断 |
| M1 | 内核侧传输替换（W1/W2） | 三档（1 / 2000 / 3706）各有对应回读值；超时计数为 0 |
| M2 | 服务发布 + 滑块端到端（W3） | `backlight-control` 被匹配；系统滑块三档可见；**渲染帧率不显著下降**（见 R3）；无 SpringBoard 崩溃循环 |
| M3 | 渐变与电源耦合（`FadePeriod`、0x28/0x29） | 熄屏前渐变可见、唤醒恢复；参考页"渐变调光完成、超时计数稳定" |

参考页把亮度排在显示之后（第 3 节明确"再进入触控和按键"的前置是显示与亮度各自记录）。当前 rev249-lo 已有桌面画面（`NOTES.md` 顶部条目，DEVICE VERIFIED），**顺序上我们正好落在该做亮度的位置**。

---

## 11. 风险登记

按实机代价排序。

- **R1 — 内核帧循环中途插一笔 DCS，是未测过的语境（高）。** X9 每帧走 mirror → ack TE_RDY → 等 fresh TE → mutex enable → `DSI_START 0→1` 的两阶段触发（[`x9_lk_gui_step()`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/pexpert/arm/x9_lk_console.h#L153)），且 [`:152`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/pexpert/arm/x9_lk_console.h#L152) 刻意声明不动 DCS/PHY/panel mode。pan36 的成功是在 shim 里、链路刚从 LK 继承时取得的。**同类冲突在 x676c 上已用 patch 0174 修过一次**（dsi push 寄存器与 CPU 路径争用）。缓解：M0 先只做单笔、在 frame 间隙（`x9_lk_submit_lock` 已提供串行点）发起，并在写后立即验证 `0x0A` 健康值。
- **R2 — 失败模式不是"没反应"而是"写死链路"（高，不可约）。** pan4 的长写曾把 DSI 链路写死（记录在案）。每一轮验证都要付出一次刷机 + 一次黑屏的成本。缓解：保留 rev249-lo 已知可出画面的 boot.img 作为回退，任何亮度候选都用 `sweetbl` 门控，默认关。
- **R3 — CoreBrightness 的数字调光会拖垮渲染（中）。** 代码注释直接记着教训：发布亮度服务后 CoreBrightness 对每帧叠加调色滤镜，CPU 渲染路径掉到 <1 fps（[`SweetFramebuffer.cpp:596-599`](file:///Users/tosasitill/ios_plj110/x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp#L596)）。X9 现在虽然上了 Metal，仍需单独回归。缓解：M2 用 `x9lo`/`x9ob` 那套短 token 风格加一个"只接受服务但不启用滤镜"的对照档。
- **R4 — 编码可能不是纯直通（中低）。** §6 的直通结论是 INFERENCE。若 Linux 侧确有曲线，我们的低档会比 stock 偏亮。缓解：先做到"单调且覆盖全量程"即算达标，曲线留作后补表；补表前需要读到 `mtk_drm_setbacklight()` 与 `led-bits/trans-bits` 的实际用法（§13）。
- **R5 — DT 声明 HS、实机要 LP（低）。** 已在 §8 说明，按 LP 走即可，但要防止后来者"照抄 DT"。

---

## 12. 难度定级对照

| 子系统 | 本文口径难度 | 主要未知量 |
| --- | --- | --- |
| 亮度（本文） | **中低** | 仅剩曲线语义与 R1 的链路时序 |
| 显示/scanout | 已完成（rev249-lo 实测桌面） | — |
| 音频 | 中高（`docs/audio/porting-difficulty.md:14`） | DSP 起核、mailbox 共享环 ABI |
| Wi-Fi | 高，本项目最难（`docs/network/wifi-porting-assessment.md:16-18`） | PCIe 根端口契约 + conninfra + Broadcom 形状接口 |
| GPU/Metal | 高，已跨越 | 已过 |

亮度之所以是最低的一项：**前置条件全部由 LK 承担完毕**（面板供电四路、复位、init 表、DSI/DSSC 模式配置、DSC 参数），交棒后 XNU 独占 SoC，Android 内核不再运行，因此参考页里那条 I2C WLED 路线上要求的 regulator、enable GPIO、升压初始化、GENI 引擎时钟投票与中断路由**一项都不需要**；Linux 侧那一整套 `apollo_brightness` / DC / 温补 / 指纹压暗 / PQ 竞争写入也**不需要移植**——没有第二个 writer 会抢着写 0x51。

---

## 13. 未决问题、下一步与文件索引

**未决（本次按要求停止扫盘，未读驱动码）**：

1. `mtk_drm_setbacklight()` / `mtk_drm_setbacklight_without_lock()` 的**定义不在这两个 oplus 目录内**（调用点在 `oplus_display_sysfs_attrs.c:224`、`oplus_display_device_ioctl.c:116`、`oplus_display_apollo_brightness.c:497/533`，均 SOURCE CONFIRMED），本体应在 MTK vendor DRM 中，尚未定位。要确认的是：level 是否真直通、`led-bits=13`/`trans-bits=13` 的语义、以及 `SET_ELVSS_PN` 标志在本面板是否参与。
2. `:533` 的 `..._without_lock_video(..., cmdq_handle)` 说明 Linux 的视频路径经 CMDQ 提交，但**与本文结论不冲突**：我们的实机证据只证明"寄存器级 LP 长写能落地"，因此不必实现 CMDQ。
3. AOD 变体 `51 0D BB 03 DD` 多出的 `03 DD` 两字节语义。

**建议下一步**（都不需要设备写）：定点读 `modules/vendor/oplus/kernel/display_driver/oplus_drm/oplus_display_apollo_brightness.c` 与 `oplus_dsi_panel_cmd.c`，把 §6 的 INFERENCE 升成 SOURCE CONFIRMED；然后按 M0 接线 `display_backlight_on()` 出一个候选。设备侧每一步仍由用户手动刷入。

**文件索引**：

- 参考页：<https://unixnu.com/zh/porting/display>（第 3 节"确认亮度控制接口"、第 4 节"适配并启用亮度"）
- 设备树：`oppo-source/modules/vendor/oplus/kernel/devicetree/oplus-display/panel-ac287-p-7-a0020-dsi-cmd{,-common}.dtsi`、`mt6993-zhujiang-24203-display-overlay.dtsi`
- oplus 驱动：`oppo-source/modules/vendor/oplus/kernel/display_driver/oplus_drm/`
- 本仓库：`x9/shim/display.c`、`x9/kernel/src/iokit/Kernel/arm/SweetFramebuffer.cpp`、`x9/kernel/src/pexpert/arm/x9_lk_console.h`、`x9/xnu-patches/0104..0110`
- 历史实机记录：`/Users/tosasitill/HelioDarwin/wiki/MT6993-Display.md`（pan35/pan36 段落，本次未复核页码）
