# OPPO Find X9（PLJ110 / MT6993）实现说明

X9 在 **OPPO Find X9（PLJ110 / OP5E17L1，MT6993，12 GiB，面板 1256×2760）** 上由 MediaTek LK 直接启动自编译 XNU 8796.141.3，运行未修改的 iOS 16.7.16（20H392，iPhone X / D22）用户态。

实时交接见 [`../NOTES.md`](../NOTES.md)，完整构建与 mailbox 规则见 [`BUILD.md`](BUILD.md)。旧 bring-up 流水账见 `git show 2406f8d:x9/README.md`。

```text
MediaTek LK
  -> boot.img v4（legacy LZ4 arm64 Image）
  -> x9 shim（MMU/FDT/UFS、LK 控制台、payload 装载）
  -> SWEETXNU（XNU + ADT + boot-args，16 KiB 页）
  -> 原生 UFS -> userdata 内 32 GiB HFSX 根盘
  -> launchd -> daemons -> backboardd / SpringBoard
```

## 操作边界

- 设备不支持临时 `fastboot boot`。只有用户明确授权后才写 `boot_b` 并重启；不得改 GPT 或其它分区。
- 插件 staging、iOS root 写入、设备重启是独立操作，构建工具不会自动执行。
- 构建成功、host 回归通过、设备启动和功能验收必须分别记录。
- 当前 `main` 没有因本轮文档整理产生新的设备结果。

## 状态（2026-11-26）

| 区域 | 已实现 / 已观察 | 未完成或未验证 |
| --- | --- | --- |
| 启动 / UFS | LK 接受 v4 镜像；32 GiB HFSX `ufsroot0` 可读写，历史日志有挂载和持续 I/O | 当前 `main` 联合候选未在本轮上机 |
| iOS 用户态 | 原版 launchd/daemons、keybagd/usermanagerd、桌面/设置/控制中心 | 不是完整可用系统；多项安全与数据保护功能关闭 |
| 显示 / GPU | AppleCLCD、Metal/PanVK、CSF 持久会话；当前插件 revision **280** | 当前帧率未测；无硬件 vsync；90/120 Hz 的 IOFB mode、DSI/PHY 与 app 节拍未闭环 |
| 输入 | S3910 触摸、PMIC 按键、电池、RTC | 点击亮屏与抬腕唤醒未实现 |
| 光线 / 距离 | SCP LIST/ENABLE/DATA、HID；默认 `x9_als=3 x9_prox=3`；历史 lux 与 `near=1` | `near=0`、oneshot 物理极性、自动亮度、遮挡熄屏 |
| Wi-Fi | MT6653 扫描、Open 网关联/DHCP/DNS | WPA2 与完整网络场景不据此标为验收 |
| 蜂窝 / 短信 | Mav17 路径、SIM/注册/RAT；VOICE09、受约束 VOICE20、CallList46 源码发布；MO 有 modem 接受 | typed `ims::Call`/FSM/delegate、双向 PCM、IMS wire；MT/ACK、SMS 存储与持久化 |
| 蓝牙 | H4 桥、FE01 动态重载、`0xE0..0xFF` 过滤，历史设备不再出现已定位的 607 崩溃 | Settings 原生 setup 结果、真实搜索/配对/BLE、HFP/A2DP/SCO 音频 |
| 相机 | sensor/receiver/clock/PHY/SMMU/IRQ/DMA 组件和 RAW 路径 | 多帧非黑画面、稳定 retire、预览/照片/视频；当前无出帧验收 |
| 音频 | ADSP/IPI、AFE、PA 安全生命周期与局部编译 | 完整 board backend；loader 默认关，扬声器 `Ready=0`，无实体出声 |
| USB / AppSync | ECM + bulk、usbmuxd/lockdownd 工具、AppSync v6 构建链 | 原生 Apple USB mux、Mac 配对、IPA 实际安装 |
| 存储用户态 | UFS/HFS 根盘 | `IOMedia`/`IOBlockStorageDevice`、APFS Data volume、Settings/Files 完整链 |
| 调度 / 睡眠 | 分档 GPU governor、retention idle、modem 调试路径 | 整机 suspend、灭屏降温因果；现存日志不能复证 dark 57°C |

## 当前配置与产物

- 完整设备参数：[`build/boot-device.json`](build/boot-device.json)。基础参数 [`build/boot.json`](build/boot.json) 不应用于完整设备候选。
- 当前蓝牙默认只有 `x9_bt=1`；完整初始化候选显式追加 `x9_bt_init=1 x9_bt_fc=1`。
- 插件 allowlist：[`kernel/generated/x9_render_external_manifest.h`](kernel/generated/x9_render_external_manifest.h)，当前 **280**。
- overlay 完整构建当前为 **104/104 对象（3 prebuilt）**；准确数量以当次 build.log 为准，不沿用历史 57/103。
- 最近 host 构建候选与未上机边界见 `../NOTES.md`“当前候选”。

## 目录

| 路径 | 内容 |
| --- | --- |
| `shim/` | 裸机入口、FDT、UFS、显示/USB 日志、boot 打包 |
| `kernel/` | XNU overlay、`recipe.json`、`link-order.txt`、生成头与 prebuilt；见 [`kernel/README.md`](kernel/README.md) |
| `xnu/` | XNU 源码补丁与对象树构建 |
| `metal/` | Metal/PanVK 前端与桥；见 [`metal/README.md`](metal/README.md) |
| `mailbox/` | 独立插件包 staging；见 [`mailbox/README.md`](mailbox/README.md) |
| `bluetooth/` | MT6653 H4、初始化、诊断与 root payload |
| `baseband/` | Mav17/MIPC/QMI 蜂窝桥；QCOM 子目录仅上游参考 |
| `camera/`、`audio/`、`torch/` | 相机、扬声器/ADSP、闪光灯组件与安全操作边界 |
| `appsync/`、`usb/` | AppSync runtime、Mac 安装工具与 USB 后端 |
| `build/`、`tools/` | 总构建入口、ADT/payload/boot.img、主机测试与日志工具 |
| `xnu-patches/` | SWEET XNU 历史补丁，不参与当前构建 |
| `out/` | gitignored 构建/分析产物 |

## 加载契约（稳定部分）

- Linux arm64 入口：MMU 关闭，EL1 或 EL2，`x0` 为物理 FDT。EL2 归一化到 EL1 后再进入 XNU。
- Image 的 `+4` 分支与 PE 兼容入口指向 shim `+0x1000`。
- boot header v4：1584 B 头、4096 B 页；legacy LZ4 单 magic、8 MiB 块与 EOF；无 AVB footer/vbmeta。
- XNU 使用 16 KiB 页；shim 提供 revision/version 2 boot-args 和指向内核虚拟地址的 `deviceTreeP`。

| 板级项 | 值 |
| --- | --- |
| 根 compatible | `mediatek,MT6993` |
| GICD / GICR | `0x06800000`（`0x40000`）/ `0x06880000`（`0x200000`）；虚拟定时器 INTID 27 |
| 看门狗 | TOPRGU `0x1c010008`，key `0x1971` |
| LK framebuffer | `0xfc910000`，1256×2760，stride 5120 |
| 显示 carveout | `0xfc910000..0xfff6ffff` |
| UFS HC | `0x16810000` |
| 面板 | `panel_ac287_p_7_a0020_dsi_cmd`（DSC v1.2 + SPR，命令模式） |
| pstore | `0xaec00000..0xaef10000` |

不要复用 MT6789 的显示、GPU、PMIC 或时钟序列。

## 日志与测试

- 原始设备日志由用户保存到 Mac `~/NNN.txt`；pstore 与磁盘 sidecar 是辅助来源。无日志不等于未启动。
- USB EP0 只读采集见 [`tools/usb-live/README.md`](tools/usb-live/README.md)。
- 主机回归入口：

```bash
python3 -I -m unittest discover -s x9/tools -p 'test_*.py' -v
```

这些测试检查 ADT、FDT、镜像边界、入口与打包等主机夹具，不模拟手机。按 `AGENTS.md`，日常构建后不自动追加测试套件或产物二次检查。
