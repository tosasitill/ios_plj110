# ios-x9

**在 OPPO Find X9(PLJ110 / MT6993)上直接运行 Apple iOS 16.7.16。**

自编译的 Apple 开源 XNU(xnu-8796.141.3,iOS 16 同代)由 MediaTek LK 直接引导,其上跑**未修改的 iPhone X(D22)iOS 16.7.16 用户态**:
launchd、系统 daemon、backboardd、SpringBoard,到桌面、设置、控制中心。GPU 经 Metal→Vulkan(metal2vulkan + Mesa PanVK)渲染桌面。

> 实验性 bring-up,不是可用的手机系统。大量功能缺失或为桩,Apple 的安全模型基本关闭(宽松签名/沙箱、无真实数据保护)。
> 不要放任何个人数据。仓库仅供研究;所有刷机/启动由设备拥有者手动完成,工具不写设备。
> (仓库名沿用 `ios-sweet`:最初的目标是 Redmi Note 10 Pro,旧 README 在 [`docs/sweet/`](docs/sweet/README.md)。)

当前状态与交接:[`NOTES.md`](NOTES.md)。

## 现状(2026-11-08)

| 区域 | 状态 |
| --- | --- |
| 启动 | 临时 `fastboot boot` 镜像;LK → shim → XNU,不写 boot 分区 |
| CPU/内存 | 8 核,16 KiB 页 |
| 存储 | 自研原生 UFS 驱动(HS-G1,双 lane);根盘是 `userdata` 内 pinned 专用 extent 里的 16 GiB HFSX,可写;Android 数据保留 |
| iOS 用户态 | 原版 launchd/daemons,keybagd 等初始化通过 |
| 显示 | AppleCLCD 呈现 1242x2760(414x920 pt),GPU 渲染 + CPU 拷贝进 LK 扫描缓冲 |
| GPU | CSF Mali 持久会话,Metal/PanVK 插件渲染桌面,约 17-22 fps(显存带宽受限,DVFSRC 调试中)|
| 输入 | 触摸、电源/音量键、电池 |
| 网络 | Wi-Fi(MT6653):扫描、Open 网关联/DHCP/DNS;WPA2 已实现待验证 |
| 缺失 | 背光、蓝牙、蜂窝、音频、相机、传感器、SEP/Face ID、硬件 vsync |

## 启动链

```text
MediaTek LK
  -> boot.img v4 (legacy LZ4 arm64 Image)
  -> x9 shim -> SWEETXNU payload (XNU + ADT + boot-args)
  -> 原生 UFS -> HFSX 根盘 -> launchd -> backboardd + SpringBoard
```

## 构建(在构建 Mac 上)

```bash
cp x9/build/config.example.json x9/build/local.json   # 一次;填路径
python3 x9/build/prepare-local.py                     # 一次;链接 .local 外部输入
python3 -I x9/build/build.py all --revision <插件号> --name <名字> --boot-arg ...   # 插件 + 内核 + boot.img
```
新插件包需先 stage 到设备 mailbox(`X9_SERIAL=<serial> python3 x9/mailbox/update.py x9/out/<名字>`),再由设备拥有者 `fastboot boot x9/out/<名字>/boot.img`。
完整参数、外部输入与 mailbox 规则见 [`x9/BUILD.md`](x9/BUILD.md)、[`x9/EXTERNAL-DEPENDENCIES.md`](x9/EXTERNAL-DEPENDENCIES.md)、[`x9/mailbox/README.md`](x9/mailbox/README.md)。

## 目录

| 路径 | 内容 |
| --- | --- |
| `x9/` | **当前工作**:shim、内核 overlay、Metal/PanVK、mailbox、构建与工具(入口 [`x9/README.md`](x9/README.md))|
| `NOTES.md` | 活的交接笔记:当前镜像、启动参数、命令、未解决问题 |
| `AGENTS.md`、`x9/AGENTS.md` | 协作规则(设备安全、日志、构建后检查)|
| `docs/graphics`, `docs/network`, `docs/audio`, `docs/brightness` | 分系统的评估与设计文档(GPU 架构/审计、Wi-Fi 计划、音频/背光移植难度)|
| `docs/sweet/` | 旧 sweet(SM7150)移植的 README 与 QEMU 测试说明(存档)|
| `shim/`、`tools/`、`xnu/`、`site/` | sweet 时代的 shim、工具、XNU 补丁与项目站点(存档;`x9/` 有自己的副本)|

## 许可与来源

见 [`LICENSE`](LICENSE)。Apple 的二进制与 IPSW 内容不在仓库中;设备转储、序列号与日志也不入库。
参考项目:UniXNU、hackios-xnu-kernel、dali(兼容修复来源),OPPO MT6993 内核源码(硬件寄存器参考)。
