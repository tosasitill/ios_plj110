# X9 手电筒初步对接

**目前是编译通过的初步实现，尚无实机点灯或控制中心验收结果。默认关闭。**
仅支持 torch，不实现相机拍摄闪光、频闪、高电流或相机 ISP。

## 启动模式

在已有完整启动配置上增加一个 `--boot-arg`，不要丢失 Wi-Fi/GPU/蜂窝参数：

| 参数 | 行为 |
|---|---|
| `x9_torch=0` / 不传 | 不映射闪光灯寄存器、不建 `/dev/x9-torch`、不注入 mediaserverd |
| `x9_torch=1` | 只读 MT6379 身份、ITORCH、使能、故障状态；只读设备节点，不发布 flash 能力 |
| `x9_torch=2` | 有新鲜且安全的电池样本时自动点一次 **25 mA / 2 s**；无样本则 15 s 后跳过，不开放用户写入 |
| `x9_torch=3` | 发布 flash 能力和 `/dev/x9-torch`，PID1 安装自有桥并只向 mediaserverd 添加注入环境；**启动时不点灯** |

`x9_torch_ch=0..2` 选择单路，默认 0；实际接灯通道仍需确认。
`x9_torch_ma=25..100` 设置单路最大电流，默认 100 mA，向下取 5 mA 档。
`x9_torch_s=1..30` 设置连续点亮上限，默认 15 s。模式 2 固定 25 mA / 2 s。
模式 2 会主动点灯，应先完成模式 1 读回，再明确选择它。

## 数据路径

控制中心 / `AVFlashlight` → 原生 flashlight Mach service → mediaserverd 的
`FigFlashlight` → 仅供 flashlight 的 vendor / stream → `TorchLevel` →
`/dev/x9-torch` → PMIF-P SWINF2 / SID14 → MT6379 ITORCH 与 FLED_EN。

- 本机捕获的 DT 描述 `mediatek,mt6379-flash`，三路最大 torch 电流均为 400 mA。
  初版主动限到 **单路 100 mA**，不搬 hackios 的 Qualcomm PM8350C 寄存器。
- MT6379 `DEV_INFO & 0xf0 == 0x70`；ITORCH=`0x204 + channel*3`，
  电流=`25000 + code*5000` µA；FLED_EN=`0x20e`，torch bit4，通道 bit(2-channel)。
  写白名单只有选中路的 ITORCH 和 FLED_EN，启动继承了使能/故障则拒绝写模式。
- 内核复用 gauge/touch 的 `X9PMIFAcquire/Release`，同址写后等待 9 µs 再读回。
  缺失、超过 15 s 的电池温度样本，或温度不在 `[0,45)` °C，拒绝点灯。
  点灯期间每 100 ms 检查硬件故障、使能状态、温度与持续时间。
- 连续超时后必须明确请求 0 才能再次开启；改亮度不延长超时。
  故障和通信错误锁存到重启；未确认熄灭保留 `OFF_FAILED`，不伪报已关。
  writer 关闭也尝试熄灭；仅 root 或 mobile 身份的 mediaserverd 可打开 writer，且只允许一个 writer。
- `media_bridge.m` 使用本机 20H392 的 CMCapture UUID、原方法地址、7 个准确返回地址，
  保留非空原 vendor；其他捕获调用仍返回原结果。stream 写入错误原样返回 FigFlashlight。
  同时采用 hackios 的定点 portrait prewarm 跳过，不添加其音频/视频/相机替换。

## 构建和手动复核

常规 `x9/build/build.py kernel|all` 自动构建 iOS 16 arm64 桥和 `torchctl`。
桥约 71 KiB，内嵌内核，生成头被 gitignore；**Metal 插件 275 不变，无需 mailbox**。
只构建工具也可以：

```bash
python3 -I x9/torch/build.py \
  --developer-dir /Users/tosasitill/Downloads/Xcode.app/Contents/Developer \
  --out .local/x9/torch-tools
```

模式 3 在可写 iOS 根盘创建自有 `/private/var/x9/torch/libx9-torch-<内容哈希>.dylib`。
不覆盖不同内容或符号链接，不改 Apple 文件、launchd plist 或分区布局；每次注入前再验证内容和权限。
`torchctl` 不自动安装、运行；部署后可手动运行：

```bash
torchctl status
torchctl av 0.25 wait 1 av 0 status
# 只有没有 mediaserverd writer 时，root 可测原始驱动：
torchctl raw 250 wait 1 raw 0 status
```

日志先后应出现 `X9 TORCH: ... ready`、`bridge install ... errno=0`、
`mediaserverd bridge environment appended`、`X9 TORCH MEDIA: ... hook installed`、
`FigFlashlight-only vendor created`，最后是请求与 **confirmed current / enable**。
应同时观察实体灯；`AVFlashlight` 返回成功或按钮出现均不能单独作为验收。

## 未解决边界

- 未验证 PMIF 写入权限、实际 LED 通道、点亮、亮度、熄灭、控制中心 RPC 和 mediaserverd 稳定性。
- MobileGestalt 的已有磁盘缓存可能仍说没有 flash；**本功能不自动删除缓存**。
  如需使用已有 `x9_mgwipe=1`，先单独确认该删除操作。
- watchdog 在内核正常调度时有效。**内核 panic / 停调度时的可靠熄灭尚未实现**，
  不能将软件超时称为硬件安全定时器。硬件限流/热保护仍由 PMIC 保留。
- watchdog 自主熄灭尚未主动发送 FigFlashlight level 通知，UI 可暂时保留旧档位；以 `/dev` 状态和实体灯为准。

## 参考

- UniXNU `ae68700` 和 `f0721e9` 本地参考未发现可直接移植的 torch 后端；复用项目已移植的 AMFI dyld 策略与原生根盘集成方式。
- hackios-xnu-kernel `c84b463`：`hakios.torch.c`、`inject_media.c`、`torch_test.c`、loader 的 `/product/camera/flash` 元数据。用户态派生部分保留 `LICENSE.hakios`。
- OPPO modules `4d505a42`：`drivers/leds/flash/leds-mt6379.c`、`drivers/mfd/mt6379{,-spmi}.c`。
- 本机 20H392 CMCapture 的已抽取符号和原 cache 指令；私有定位材料在 `.local/x9/work/torch-20261110/`，不入 git。
