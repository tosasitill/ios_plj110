# OPPO Find X9 (PLJ110 / MT6993) — 实现说明

iOS 16.7.16(20H392,iPhone X/D22 身份)经自编译 XNU 8796.141.3 在 **OPPO Find X9(PLJ110 / OP5E17L1,MT6993,12 GiB,面板 1256x2760)**
上直接从 MediaTek LK 启动。实时状态、当前镜像、启动参数、未解决问题:[`../NOTES.md`](../NOTES.md)。构建:[`BUILD.md`](BUILD.md)。
(2026-11-08 重写;旧 bring-up 流水账与 pan184 时代说明见 `git show 2406f8d:x9/README.md`。)

```text
MediaTek LK
  -> Android boot.img v4(legacy LZ4 arm64 Image)
  -> x9 裸机 shim(MMU/FDT/UFS 初始化、LK 控制台与 Wi-Fi DMA pool 保留、payload 装载)
  -> 内嵌 SWEETXNU payload(内核 + 生成的 ADT + boot-args)
  -> SWEET XNU(plain arm64,16 KiB 页)
  -> 原生 X9 UFS 驱动 -> userdata 内 extent 根盘(16 GiB HFSX,rw)
  -> 原版 launchd -> daemons -> backboardd + SpringBoard
```

本仓库的工具**从不写设备**;用户手动 `fastboot boot` 候选镜像。

## 状态(2026-11-08,均为设备观察)

| 区域 | 状态 |
| --- | --- |
| 启动/根盘 | LK 接受 v4 镜像;原生 UFS 接管,HS-G1/RateB/2 lane(~130-155 MiB/s);HFSX 根盘 rw 挂载;`cpus=8` |
| iOS 用户态 | 未修改的 launchd 与系统 daemon;keybagd/usermanagerd 初始化;SpringBoard 桌面、设置、控制中心 |
| 显示 | 内核发布 AppleCLCD,呈现 1242x2760(414x920 pt @3x);帧由 GPU 渲染后 CPU 拷进 LK 扫描缓冲(`0xfc910000`,stride 5120)|
| GPU | CSF 持久会话,Metal(metal2vulkan)+PanVK 插件经 mailbox 交付渲染桌面;约 17-22 fps,带宽受限 |
| 输入 | S3910 触摸、电源/音量键(PMIC→HID)、电池服务 |
| 其它 | RTC 时间、相册(TCC 扩展)、机型 iPhone、键盘 123 均已修好 |
| Wi-Fi | MT6653 over PCIe:固件起来、扫描、Open 网关联/DHCP/DNS;WPA2 4-way 已实现未上机 |
| 未做/缺 | 背光控制、硬件 vsync、modem/蓝牙/音频/相机/传感器、SEP/真 keychain 保护、captive portal 弹窗 |

## 目录

| 路径 | 内容 |
| --- | --- |
| `shim/` | 裸机 shim:`start.S`(EL2→EL1 归一化)、`loader.c`、`ufs/`、FDT、显示/USB 日志、`build.py` |
| `kernel/` | XNU overlay(57 个对象 + `recipe.json` + `link-order.txt`)和生成头(CSF 固件、插件 allowlist);见 `kernel/README.md` |
| `metal/` | Metal/PanVK 前端与桥(`General/`),历史 GPU bring-up 说明;见 `metal/README.md` |
| `mailbox/` | 插件 mailbox 更新器;见 `mailbox/README.md` |
| `build/` | `build.py`、`boot.json`、第三方补丁、打包脚本 |
| `tools/` | `boot-xnu.py`(ADT/payload/boot.img)、`mkadt.py`、根盘检查、`usb-live/usb_log.py`、Wi-Fi 抓取脚本等 |
| `re/` | MT6993 显示 mutex / 帧路径的逆向笔记(OPPO 内核源码提取)|
| `xnu-patches/` | SWEET XNU 补丁历史(参考;不参与构建)|
| `out/` | 构建产物(gitignored)|

## 加载契约(稳定部分)

- Linux arm64 入口:MMU 关,EL1 或 EL2,x0 = 物理 FDT。EL2 归一化为 EL1:先写 `HCR_EL2`(清 `E2H/TGE`)再装 `VBAR_EL2`;
  `ERET` 前设置 FGT/CPTR/VPIDR/VMPIDR 以及(GIC 存在时)`ICC_SRE_EL2/ICH_HCR_EL2`;缓存维护在 EL1 执行。陷入走 shim 自己的向量表。
- Image 的 +4 分支与 PE 兼容入口都指向 shim 的 **+0x1000**。
- boot header **v4**(1584 字节头、4096 页,装载地址由 LK 决定);legacy LZ4 单 magic + 8 MiB 块 + EOF;无 AVB footer/vbmeta(未解锁 LK 是否强制未验证)。
- XNU 用 **16 KiB** 页;shim 提供 revision/version 2 boot_args 与内核虚拟地址的 `deviceTreeP`。
- 板级实测值:

| 项 | 值 |
| --- | --- |
| 板识别 | 根 `compatible` = `mediatek,MT6993`(LK 交的 DT 实际如此;`oppo,x9` 不存在)|
| GICD / GICR | `0x06800000`(0x40000)/ `0x06880000`(0x200000);虚拟定时器 INTID 27 |
| 看门狗 | TOPRGU 重启寄存器 `0x1c010008`,key `0x1971`(timer IRQ 里喂)|
| LK 帧缓冲 | `0xfc910000`,1256x2760,stride 5120;显示 carveout `0xfc910000..0xfff6ffff` |
| UFS HC | `0x16810000` |
| 面板 | `panel_ac287_p_7_a0020_dsi_cmd`(DSC v1.2 + SPR,命令模式);Android 扫描用 M4U IOVA,不是 LK 物理地址 |
| pstore 保留区 | `0xaec00000..0xaef10000` |

  不要复用 MT6789 的显示/GPU/PMIC/时钟序列。

## 日志

持久控制台(LK 保留的 pstore / 专用磁盘 sidecar)与 USB 实时日志(`tools/usb-live/usb_log.py`);见 `NOTES.md §2`。
一个挂死后强制重启的候选可能完全没有记录:"无日志"不等于"没启动"。

## 测试

```bash
python3 -I -m unittest discover -s x9/tools -p 'test_*.py' -v
```
覆盖 ADT 身份/几何、SWEETXNU 边界、FDT 解析、ARM64/PE 入口、LZ4、boot header 篡改与旧内核拒绝等。这些是主机侧夹具,不模拟手机。
(按 `AGENTS.md`,日常构建后不额外跑测试套件。)
