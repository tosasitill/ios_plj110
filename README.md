# ios-x9

**在 OPPO Find X9（PLJ110 / MT6993）上直接运行 Apple iOS 16.7.16。**

仓库名沿用 `ios-sweet`，当前工作对象是 X9：自编译 XNU xnu-8796.141.3 加 SWEET/X9 补丁，由 MediaTek LK 直接引导，运行**未修改的 iPhone X / D22 iOS 16.7.16（20H392）用户态**。launchd、系统 daemon、backboardd 与 SpringBoard 可启动到桌面；GPU 走 Apple Metal → metal2vulkan → PanVK → XNU Mali 后端。

> 实验性 bring-up，不是可用的手机系统。多项功能仍是桩或仅完成部分链路，Apple 安全模型被显著放宽。不要存放个人数据。
> 设备写入、mailbox staging、重启和 root 修改均须设备拥有者明确授权；工具不会自行决定执行这些操作。

当前交接、候选与验收边界见 [`NOTES.md`](NOTES.md)。旧 Redmi Note 10 Pro / SM7150 文档保留在 [`docs/sweet/`](docs/sweet/)。

## 当前状态（2026-11-26）

表中“已观察”只代表对应历史设备日志，不代表当前 `main` 候选已经再次上机。

| 区域 | 当前事实与边界 |
| --- | --- |
| 启动 | LK → Android boot.img v4 → X9 shim → SWEETXNU。设备不支持临时 `fastboot boot`；经明确授权只写 `boot_b`，不改 GPT、bootloader、recovery、modem、persist 或备份。 |
| CPU / 内存 | 8 核，16 KiB 页，12 GiB RAM。 |
| 根盘 | `userdata` 内 pinned/immutable extent 上的 **32 GiB HFSX**（16 GiB base + 16 GiB extension）；原生 `ufsroot0` 可读写并已观察 HFS 挂载与持续 I/O。 |
| iOS 用户态 | 原版 launchd/daemons、keybagd/usermanagerd 初始化、桌面、设置与控制中心已有历史运行证据。 |
| 显示 / GPU | 物理面板 1256×2760、stride 5120、fb `0xfc910000`；ADT canvas 1242×2760（414×920 pt @3×）。Metal/PanVK 插件 allowlist 当前为 **revision 280**。桌面可渲染，但当前帧率、硬件 vsync 与 90/120 Hz 用户态节拍未验收。 |
| 输入 / 基础硬件 | S3910 触摸、电源/音量键、电池、RTC 等已有实现；不作为全场景回归结论。 |
| Wi-Fi | MT6653 固件、扫描、Open 网关联/DHCP/DNS 有历史证据；WPA2 路径不能据此标为完整验收。 |
| 蜂窝 | Mav17 Apple 兼容路径已进入 SIM/注册/RAT 阶段；VOICE09、受约束的 VOICE20、CallList46 已在源码中发布，但 stock typed `ims::Call`/FSM/delegate 与双向音频未接通。 |
| 短信 | MO 有 modem 接受证据，不是投递回执；MT/ACK、存储枚举与 Messages 持久化未完成。`0x210000` 不是空收件箱证据。 |
| 蓝牙 | MT6653 H4 桥、FE01 动态固件重载、`0xE0..0xFF` 厂商事件过滤已有设备证据，消除了已定位的 reason-607 崩溃。默认 `boot-device.json` 只有 `x9_bt=1`；完整初始化候选须显式加 `x9_bt_init=1 x9_bt_fc=1`。搜索、配对、BLE 与音频未验收。 |
| 光线 / 距离 | `x9_als=3`、`x9_prox=3` 为默认且写入完整设备配置；共享 SCP 与 HID 桥已实现，历史日志有 LIST/ENABLE/lux 与 `near=1`。`near=0`、物理 oneshot 极性、自动亮度与遮挡熄屏未验收。 |
| 相机 | 传感器、接收配置、SMMU/IRQ/DMA/RAW 组件持续集成；历史 RAW 为黑电平单帧，后级 SMMU retire 曾触发复位。没有预览、照片或视频验收。 |
| 音频 / 扬声器 | ADSP/IPI/AFE/PA 生命周期与安全收尾有大量源码和局部编译；loader 默认关闭，未注册完整物理后端，扬声器 `Ready=0`。 |
| USB / AppSync | USB2 CDC-ECM 与私有 bulk 候选不等于原生 Apple USB mux。AppSync v6 构建/注入链存在；Mac 配对、IPA 传输和实际安装均未验收。 |
| 存储用户态 | UFS/HFS 根盘可用；缺 `IOBlockStorageDevice`/`IOMedia` 与 APFS Data-volume 链，Settings/Files 不能视为已修复。 |
| 灭屏 / 唤醒 | GPU retention 与 modem 调试轮询会增加活动；没有整机 suspend 或降温验收。点击亮屏、抬腕唤醒仅有 Android/源码线索，没有 iOS 工作链。 |

## 启动链

```text
MediaTek LK
  -> boot.img v4（legacy LZ4 arm64 Image）
  -> X9 bare-metal shim（MMU/FDT/UFS、LK 控制台、payload）
  -> SWEETXNU（XNU + 生成的 ADT + boot-args）
  -> 原生 UFS -> userdata 内 HFSX 根盘 -> launchd
  -> backboardd / SpringBoard -> Metal/PanVK 显示
```

## 构建与插件

私有输入布局见 [`x9/EXTERNAL-DEPENDENCIES.md`](x9/EXTERNAL-DEPENDENCIES.md) 和 [`x9/BUILD.md`](x9/BUILD.md)。

```bash
cp x9/build/config.example.json x9/build/local.json   # 一次；填写本机工具路径
python3 -I x9/build/prepare-local.py --check

PATH=/opt/homebrew/opt/llvm@22/bin:$PATH \
python3 -I x9/build/build.py all \
  --boot-config x9/build/boot-device.json \
  --name <unique-name>
```

- `boot.json` 只有基础参数；完整设备候选使用 `boot-device.json`。
- 每次使用新的 `--name`，产物在 gitignored `x9/out/<name>/`。
- 插件不在 boot.img 中。revision 或任一插件哈希变化会重写内核 allowlist，必须同步 staging 并安装匹配包；纯内核改动且 allowlist 不变时无需 mailbox。
- staging、`boot_b` 写入、重启、root 安装和分区操作是独立授权步骤，不在构建命令中自动执行。
- 编译成功、host 测试通过、候选可启动、设备功能验收是四种不同标签。

## 目录

| 路径 | 内容 |
| --- | --- |
| `x9/` | 当前 shim、kernel overlay、Metal/PanVK、mailbox、构建与工具；入口 [`x9/README.md`](x9/README.md)。 |
| `x9/xnu/` | XNU 补丁与对象树构建脚本。 |
| `NOTES.md` | 精简后的实时交接：目标、固定输入、构建、当前候选、子系统边界和下一步。 |
| `AGENTS.md`、`x9/AGENTS.md` | 协作、设备安全、构建与证据规则。 |
| `docs/` | 分系统设计、逆向与历史验收报告。 |
| `docs/sweet/` | 旧 sweet / SM7150 档案。 |
| `xnu/`、`shim/`、`tools/`、`site/` | sweet 时代存档；X9 构建使用 `x9/` 内对应实现。 |
| `.local/`、`x9/out/` | 私有输入与构建产物，均不入 git。 |

## 许可与来源

见 [`LICENSE`](LICENSE)。Apple 二进制、IPSW 内容、设备转储、序列号和设备日志不在仓库中。参考来源包括 UniXNU、hackios-xnu-kernel、dali、OPPO MT6993 源码及本地 Apple 20H392 静态分析；采用其设计时不把来源项目的运行结果当成本机结果。
