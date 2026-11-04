# X9 Wi-Fi 实施计划（MT6653 / CONNAC 3.0，PCIe0，iOS 16.7.16）

- 日期：2026-11-04。前置评估：[`wifi-porting-assessment.md`](wifi-porting-assessment.md)（2026-11-03，下称"评估"）。
- 参考：<https://unixnu.com/zh/porting/network>，及其链接的源码
  `UniXNU-sheng@516a780 package/WifiNet.cpp`、`package/WifiProbe.cpp`、
  `Ath12kWifi@f6bf561 include/nw_abi.h`（BSD-2-Clause-Patent，可带署名复用）。
- 本仓库源码：`/home/ubuntu/android_kernel_modules_and_devicetree_oppo_mt6993`（下称"MTK 源码"）。
- 标签：**SOURCE** = 本次读到的文件/行；**推断** = 未验证。本文只是计划，没有改代码、没有碰设备。

## 1. 结论：拆成两条互不阻塞的线

UniXNU 的做法是把 Wi-Fi 切成两半，中间只隔一个很小的"站点接口"（`nw_abi.h`）：

- **上半（iOS 契约）**：`WifiNet.cpp` 发布 `IO80211Controller` / `IO80211Interface` /
  `IONetworkStack` 注册表替身，用 ifnet KPI 建真实的 `en0`，在 `en0` 上应答 wifid 的
  `SIOCGA80211`/`SIOCSA80211`（Apple80211 请求：power、scan、associate、state、ssid、bssid、
  rssi、channel、country……），发 `KEV` 802.11 事件；**不接触射频**，每个未处理的请求都记录
  type/方向/长度后拒绝。
- **下半（射频）**：只需实现 `scan / bss_get / connect(ssid,bssid,pass) / disconnect / tx /
  status` 和 `scan_done / connect_done / link_lost / rx` 四个事件。

评估把"iOS 用户态契约（P5）"列为 XL 研究项，这份参考源码基本把它变成了移植工作。我们照此拆分：

| 线 | 内容 | 依赖射频？ | 难度 |
| --- | --- | --- | --- |
| **A** | iOS 契约层（照 `WifiNet.cpp`），先挂一个**假射频后端** | 否 | 中 |
| **B** | MT6653 射频：conninfra 上电 → PCIe0 RC → gen4m `os/darwin` + 固件 → 站点接口 + WPA2 | 是 | 高 |
| **C** | A + B 合流：设置里扫描、加入、DHCP、收发 | — | 中 |

A 线可以马上开始、每轮都能在现有构建上验证，并且最早回答"wifid 在我们的系统上会不会动"；
B 线是长线，风险集中在上电和内存。

## 2. 本次新确认的硬件事实（SOURCE，来自 MTK 源码设备树）

评估的头号阻塞是"没有 MT6993 设备树"。同步的源码里有：
`kernel/kernel_device_modules-6.12/arch/arm64/boot/dts/mediatek/mt6993.dts`（SoC）和
`vendor/oplus/kernel/devicetree/oplus6993_24203.dts`（X9 板，包含
`oplus/wifi/mt6993_wifi_cust_without_CS.dtsi`）。

| 事实 | 值 | 出处 |
| --- | --- | --- |
| Wi-Fi 所在 RC | **pcie0** `pcie@16910000`（MAC，4 KiB×4），与评估里 `mt6653.c:3564` 的 `ioremap(0x16910000)` 一致 | `mt6993.dts:5415` |
| RC 中断 | `GIC_SPI 369`（pcie1 是 371） | `:5422` |
| 出站窗口 | `ranges = <0x82000000 0 0x50000000 0 0x50000000 0 0x8000000>` = 128 MiB MEM @0x50000000（满足评估里 BAR ≥2 MiB） | `:5424` |
| PHY | `phy@16900000`（sif）+ `0x16920000`（ckm），1 lane，校准来自 efuse `pcie0@154` 和 `pcie@234` | `:5501`, `:1852` |
| 时钟/复位 | `pextp0cfg_ao_clk` 的 MAC0 TL/REF/AXI_250/AHB_APB/PL_P、`cksys_top TL_SEL`、`ap_pd PEXTP_MAC0`；复位 `pextp0ao_rst 0/1`（mac/phy） | `:5433-5442` |
| IOMMU | **pcie0/pcie1 都没有 `iommus`/`iommu-map`/`msi-parent`**：推断 RC 不在 SMMU 后面，MSI 用 MAC 自带的 MSI set（评估：`0xc00+i*0x10`） | `:5415-5457` |
| connv3 | `mediatek,mt6993-connv3`，radio wifi/bt/md，`co-clock = <0>`，32 kHz，MT6376 PMIC 用 GPIO 65/251，fault 中断 `pio 65` 下降沿，SKU 用 `mt6661_6_aio3` ADC，13 个 pinctrl 状态（pmic-en、combo-gpio、por-reset…） | `:2107-2144` |
| Wi-Fi EMI | `emi-addr = <0>`（**开机时分配**），`emi-size 0x1900000`，16 MiB 对齐，`emi-max-addr 0xc0000000`；connsys EMI 布局 12 区（6653 WiFi MCU 12 MiB、WFDMA1 25 MiB、WF MISC 1 MiB…），**MPU 区经 ATF 设置**（`emi-region-to-atf = <1 7>`） | `:19192`, `:2058-2105` |
| 其他保留 | `wifi-tx-cma` 84 MiB、`wifi-misc` 1 MiB | `:19226-19234` |
| 板级 | X9 = 24203，前端 QM42655/QM45655（2G+2A），`flavor-bin = "dx5_2g2a"` | 板 dtsi |

两个直接后果：

1. **SMMU 问题基本有答案（推断）**：没有 iommu 绑定，DMA 走物理地址，评估担心的"要先写 SMMU 驱动"大概率不成立。仍需在 B2 用一次 DMA 回读确认。
2. **EMI 地址要从 LK 拿**：`emi-addr = <0>` 说明地址由引导阶段分配，并通过 ATF 配 MPU。我们的 shim 已经解析 LK 的实时 FDT（RAM 映射来自 `chosen/x9-ram-map`），可以零风险地把 `reserved-memory` 里 connsys/wifi 相关的节点打印出来——**不需要回 Android**。

## 3. A 线：iOS 契约层（先用假射频）

**A1 站点接口**：新建 `x9/kernel/src/.../X9WifiSta.h`，结构照 `nw_abi.h` 的站点部分
（`nw_bss_info`、`nw_wifi_status`、`NW_STA_*`、`NW_RES_*`、四个事件），这是 A/B 之间唯一的契约。

**A2 假射频后端**（`x9wifi=1` 之类的 token 或默认关）：`scan` 返回 1~2 条固定的合成 BSS
（带合法 IE：SSID、Supported Rates、DS、RSN），`connect` 走状态机到 `NW_STA_UP` 但不收发；
目的只是驱动上半层。

**A3 iOS 层**：照 `WifiNet.cpp` 写 `X9WifiNet.cpp`（保留原作者署名），包括：
- `IONetworkStack` / `IO80211Controller`（`IOMACAddress`、`IOBuiltin`）/ `IO80211Interface`
  （`IOInterfaceName=en0`、`BSD Name`、`IOInterfaceNamePrefix=en`、`IOInterfaceType=IFT_ETHER`、
  `IOPrimaryInterface`、`IO80211InterfaceRole=Infrastructure`）；
- 用 `ifnet_allocate_extended` 建 `en0`（MTU 1500，`IFEF_ENQUEUE_MULTI`）；
- `SIOCGA80211`/`SIOCSA80211` 应答，以及 KEV 802.11 事件（class 6/subclass 1）和
  `KEV_DL_LINK_ON/OFF`；
- **与现有 `SweetX9NetworkStack.cpp`（只做命名的 `IONetworkStack`）合并**，避免两个
  `IONetworkStack`；
- 每个请求打日志 `X9 WIFI REQ: type= set/get len= -> err`，前 300 条全打。

**A4 验证点**（每项单独记录）：
1. configd 给出 `en0`，`ifconfig` 等价信息出现在日志统计里；
2. wifid 开始在 `en0` 上发 Apple80211 请求（日志里出现 type 列表）——**这一步回答"wifid 在
   我们的系统能不能跑"**；如果没有请求，转去查 wifid 是否被 launchd 拉起/沙盒/AMFI；
3. 设置里 Wi-Fi 开关可开，列表出现合成 SSID。

**A5 版本差异**：UniXNU 的请求编号和结构布局针对他们的 iOS 版本（需核对其目标版本）。iOS
16.7.16 若有不同，以 A4 日志里 wifid 实际发的 type/len 为准，必要时对 iOS 16 的
`/usr/libexec/wifid`、`WiFiKit`/`MobileWiFi` 做定点逆向（dyld cache 与 OS 根镜像在 Mac 上已
解开：`.local/x9/work/dsc-20H392/`，`/tmp/os20H392`）。

## 4. B 线：MT6653 射频

| 阶段 | 内容 | 里程碑（日志可见） | 主要风险 |
| --- | --- | --- | --- |
| **B0 事实收集（只读）** | ① shim/内核打印 LK 实时 FDT 中 `reserved-memory` 全部节点（重点 connsys/wifi/emi 名称与地址）、`connv3`/`pcie0`/`wifi` 节点的实际 status；② 从 Android `vendor` 拉固件清单与哈希（`mt6653_wifi*`、`WIFI_RAM_CODE_6653*`、`*PATCH*`、`*PHY_RAM_CODE*`、TxPwr、NVRAM/cal、`conninfra.cfg`、`connfem.cfg`、`WMT.cfg`），放 `.local/`；③ 确认 `.ko` 变体（plain/`_triband`/`_mcl50`） | 一张 EMI/保留内存表 + 固件清单 | 无（只读） |
| **B1 conninfra 上电** | 照 `connv3_core.c` / `connv3_hw.c` / `platform/mt6993/*`：pinctrl（pmic-en、combo-gpio、por-reset 30 ms）、MT6376 PMIC 写序列、MT6687 时钟 buffer、32 kHz；做成 XNU 内建驱动，token 门控、默认关 | connsys ID/版本寄存器可读，32k 与时钟确认，**先于任何 PCIe 访问** | **最高**：多射频仲裁状态机、按名字查找全要硬编码，错了是不上电而不是报错 |
| **B2 PCIe0 RC** | 照 `pcie-mediatek-gen3.c`：PHY 校准（efuse）、时钟/复位、PERST 100 ms、链路轮询、trans-table 映射 0x50000000、配置空间读、MSI set → SPI 369；做最小 RC，不做通用 PCI 子系统 | `X9 PCIE: link up 14c3:6653 bar0=...`；一次 EP 读写 DMA 回读通过 | 大（XL），但有完整 Linux 源码可照 |
| **B3 gen4m `os/darwin` + 固件** | 照评估：实现 `gl_os.h`/KAL（`os/uefi`、`os/windows` 为模板）+ PCIe HIF 后端；WFDMA 环/EMI 必须落在 **LK 分配、ATF 已配 MPU 的区域**；固件经 mailbox 管线交付（同 X9GeneralMetal 的打包/白名单） | `fw_dl` 成功、MCU running、读到固件版本；MAC 来自 NVRAM（评估：`CFG_TC1_FEATURE` 关） | EMI MPU/ATF 不放行会让 MCU 访问失败；固件/cal 变体 |
| **B4 站点后端** | 把 gen4m 的扫描/加入接到 X9WifiSta 接口；**WPA2-PSK 四次握手要内核内实现**（gen4m 在 Linux 上依赖用户态 wpa_supplicant）：PBKDF2-SHA1 → PMK → PTK、EAPOL-Key 1-4、CCMP 装钥；先只支持 WPA2-Personal/CCMP | 扫描出 BSS → 加入 → DHCP → ping | 握手与装钥的固件命令格式 |

UniXNU 的经验可直接借用（SOURCE，其页面）：不扫 6 GHz（被动信道占大半扫描时间）；对端条目
用 AP 自己的 HT/VHT 能力；5 s 内仍收到帧就忽略固件的 beacon 丢失；事件回调在工作线程里必须
快速返回，RX 交给异步路径前要拷贝；MAC 为零时用芯片 ID 生成（我们对应 NVRAM 缺失的情况）。

## 5. C 线：合流与验收（UniXNU 口径）

把 A3 的后端从假射频换成 B4。验收分别记录：扫描、认证、关联、地址配置（DHCP）、接收、发送；
网络记录写明 AP 配置、固件、重连行为、持续传输结果。参考实现的终点是"加入 WPA2-Personal、
DHCP 拿到地址、能打开网页"。已知限制可接受：设置可能无法保存密码（UniXNU 同样如此）。

诊断计数（照 `nw_wifi_stats` / `nw_wifi_where`）：`X9 WIFI STATS: stage scans frames beacons
bss_seen last_err`，加 MSI 计数和固件日志转储。

## 6. 风险排序（更新评估）

1. conninfra 上电（B1）——不变，仍是最大正确性风险。
2. **EMI/MPU**（新）：Wi-Fi EMI 由引导阶段分配并经 ATF 配 MPU，必须复用 LK 给的区域；自己分配的内存 MCU 可能访问不到。
3. WPA2 内核握手（B4）。
4. iOS 16 与 UniXNU 目标版本的 Apple80211 布局差异（A5）——由 A4 日志尽早暴露。
5. ~~SMMU~~：设备树无 iommu 绑定，降级为待 B2 确认。
6. ~~boot-arg 预算~~：已于 2bf1891 解决（215/607 字节）。

## 7. 建议的下一步（按顺序）

1. **A1–A3 + 假射频**：一轮构建即可上机，验证 wifid/设置是否跟上来（不碰任何射频硬件）。
2. **B0①**：在同一轮里顺带打印 LK FDT 的 `reserved-memory` 和 connv3/pcie0/wifi 节点（只读）。
3. **B0②**：你在 Android 下拉一次固件清单（我给出 adb 命令清单），放 `.local/`。
4. 之后再进入 B1；B1 第一轮只读寄存器、不写 PMIC，写之前单独确认。

设备侧每一步仍由你手动刷入；B1 起任何上电/写 PMIC 的步骤都会先单独确认。
