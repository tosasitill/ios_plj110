# iOS 波形透传与 X9 马达兼容性核对

## 结论

**AW8693XS 驱动芯片支持 RTP 流式波形输入，现有设备日志也证明这条通路已运行；但不能由此推出 OPPO 马达能原样还原 D221/Taptic Engine 的触感。**

`66dd5f8c` 的改动可作为保留 iOS 波形结构的基线，不是已经完成的本机机械响应校准。没有证据说明必须回到旧版包络→固定 173 Hz 重合成；同样没有证据支持“透传后全部场景自然适配”。

本轮仅核对、离线分析和文档交接，没有修改蓝牙/振动生产代码、运行设备或编译内核。

## 1. 实际检查的提交

- 本地 `feat/bluetooth-native`：`e36474841265318b9acd3d4b6e6059611289a014`，2026-11-16 14:56，NOTES 交接。
- 对应功能提交：`66dd5f8c4a5af87c563084487fa111e28b4b81a2`，2026-11-16 14:55。
- 本轮 `git ls-remote` / `git fetch origin refs/heads/feat/bluetooth-native` 得到远端 `f3332b8907ad16b88793e3f43ffc0b767034107e`，日期为 11-15；不是本地这次波形改动。没有推送或改写这条活动分支。
- 核对工作树基线 main：`9e25175b`。不要把 main 的振动实现当作本地功能提交的实现。

## 2. Evidence → Finding → Path

### E1：已有设备运行日志证明芯片/RTP 路径，不证明新版透传效果

`~/330.txt:698–699`：`chip=9381 RTP=12000 fifo=2048 f0=173000 mHz gain=80 native I2C5 ready`、`mode=2 flags=1 errno=0`。

`:10278–10279` 有 `mediaserverd` writer/START；`:10844` 有 `submitted=3336`、`state_or=08`、非零 peak，同时 `underruns=6`。这些证明旧版实际写入/运行，不是新版 rootboot13 上机验收。

日志的 `f0=173000` 来自代码默认值/boot arg，不是 XNU 在此测出的机械共振。蓝牙分支 NOTES 记录过 Android `f0=1729`，即约 172.9 Hz；本轮未找到/重采该次原始 Android 输出，不将交接记录升级为本轮实测，更不重读曾导致重启的 `f0_save`。

### E2：原厂确实提供直接流式输入，但输入仍需匹配执行器

原厂参考根目录：`.local/references/oppo-modules-dt-mt6993/vendor/oplus/kernel/vibrator/oplus_haptic/`。

调用路径：

```text
haptic_common_driver.c IOCTL_STREAM_MODE
  → haptic_hv.c aw_stream_mode: 选择 boost、上传 AW_OSC_CALI_LRA
  → haptic_rtp_work: 等 RTP_GO，直接传 opbuf->data/opbuf->length
  → aw8693xs.c aw8693xs_set_rtp_data
  → i2c_w_bytes(AW8693XS_REG_RTPDATA, data, len)
```

- `aw8693xs.c:360–385` 支持 48/24/12/8 kHz WAVDAT。
- `haptic_hv.c:2793–2799` 的 `aw_set_gain` 限制 `HAPTIC_GAIN_LIMIT=128`。
- `aw8693xs.c:684–691` 的 RTP 模式开启硬件 auto-brake、boost；删除软件 kick/brake **不等于关闭所有硬件刹车**。
- `aw8693xs.c:745–775` 显式区分 F0 和 OSC trim；RTP 流式路径上传 OSC trim。它不会分析收到的 PCM 并自动重调到机械共振频率。
- 设备 DT 的 `qcom,device_id=0x329`（0815）；原厂 `aw8693xs_haptic_value_init` 为非 0816 配置 `f0_pre=1700`。这是约 170 Hz 的设计预设，不是“80–300 Hz 等强机械输出”的证明。

因此，原厂流式通路支持任意输入字节的事实，只说明数字/驱动接口成立。LRA 是有共振、惯性、阻尼的机械系统；离共振输入仍可产生响应，但幅度、相位、起振与余振不保证等同另一型号。

### E3：最新转换器保留载频，没有做本机响应适配

按 `66dd5f8c` 提取真实 `x9_haptic_convert.h` / `x9_haptic.h`，编译小型 Mac C wrapper 后离线执行：

| 输入正弦 | 数字输出测得频率 | 相对理想线性缩放的 RMS |
| --- | --- | --- |
| 80 Hz | 80.00 Hz | 0.9987 |
| 173 Hz | 173.02 Hz | 0.9972 |
| 230 Hz | 229.97 Hz | 0.9942 |
| 300 Hz | 300.00 Hz | 0.9886 |

将 converter 初始化的 `f0` 从 173 改为 190 Hz，或 `sharpness` 改为 0，四组输出逐样本完全相同。HAL 使用 `X9_HAPTIC_RETUNE`，但该模式名已是历史名称：新实现只选择 ±127 上限，不再重调载频。原生 sharpness 信息须由 iOS 已渲染的波形携带。

实际转换含一阶低通、四样本平均、48→12 kHz 降采样和 S16→S8 量化，不是逐字节无损透传。没有带入本机机械响应模型。

### E4：iOS 资产本身含设备调校，不能当成通用马达指令

本轮新只读挂载固定仓库输入 `.local/apple/ios16-20H392/OS.dmg`，检查 `Library/Audio/Tunings/D221/Haptics`；旧 `/private/tmp/os20H392` 由仓库外镜像提供，未用作本轮权威采样。新挂载已 detach。

`hapticengineconfig.plist` 的 `Default Sine Lookup`：172.4427 Hz zone 的 Gain 为 `-5.3521`，230 Hz zone 为 `-10.8338`。不同频率已有不同增益，证明不能把 Apple 输出视作只需一个通用比例的未调校波形。该配置不提供 OPPO 马达的频响。

三份原始 WAV 经真实新 converter：

| 资产 | 文件时长 | 转换后非零跨度 | 峰值 / 1/4 intensity 峰值 |
| --- | --- | --- | --- |
| MicroTap230Hz | 7.917 ms | 6.750 ms | 127 / 32 |
| MiniTap150Hz | 12.604 ms | 11.167 ms | 127 / 32 |
| FullTap80Hz | 36.021 ms | 33.333 ms | 127 / 32 |

这支持“新版不再额外重合成几十毫秒的嗡声、幅度比例正常”的数字层判断，不证明物理位移/加速度或全部 iOS 场景。MicroTap230 的原始最小值为 -814、最大值 32767，准确说是**明显单向为主**，不是严格无负值。

## 3. 仍未闭合的边界与建议

1. **机械适配**：先以透传版为基线，对少量明确事件/频率确认本机力度、起振和余振，再决定是否需要有限的频率相关增益、瞬态补偿或本机波形。不要盲目恢复逐样本增益下限、×6 削顶或固定载频重合成。
2. **校准**：最新 `hconfigure` 保留工厂 oscillator source，没有执行原厂流式路径的 `upload_lra(AW_OSC_CALI_LRA)`。已经能运行不代表采样时钟已按 Android OSC 校准；不要直接复制 F0 trim 当 OSC trim，也不要把 OSC trim 当成机械均衡。
3. **延迟**：HAL 提前 20 ms 放包；内核的 240-frame/20-ms 阈值是软件队列启动条件，随后还有 GO/RTP_GO 等待、FIFO/I2C、机械起振。转换器本身的低延迟不能代表整条链路只延迟 1 ms。
4. **200 ms 静音续流**：依赖 HAL 继续供给 PCM。worker 的 100 ms 无新 write 仍会 park，最后一个 client `StopIO` 也会直接 STOP；所以“每次按键都不再重新 GO”是待场景验收的目标，不是无条件保证。

## 4. 重放与产物

静态复核（仓库根）：

```sh
git show 66dd5f8c -- x9/kernel/src/bsd/dev/x9_haptic_convert.h x9/haptic/actuator_plugin.c
git show 66dd5f8c:x9/kernel/src/bsd/dev/x9_haptic_aw8693xs.inc
rg -n 'X9 HAPTIC' ~/330.txt
```

分析归档：`x9/out/haptic-pass-review-20261116/`，含 scope/timeline/workitems、脱敏 field-journal、真实转换器源、wrapper、`analyze.py`、`analysis.json`、数值退出码、原厂切片和 attach/detach 记录。Apple WAV/配置原件没有复制进 git。

重跑离线转换时，用固定 `.local/.../OS.dmg` 在**新的 scratch mountpoint** 只读挂载，将其 `Library/Audio/Tunings/D221/Haptics` 路径作为参数：

```sh
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
  x9/out/haptic-pass-review-20261116/analyze.py <本次只读挂载的Haptics目录>
```

脚本使用项目已有 Xcode 15/MacOSX14 路径。首轮 host link 因默认 CLT MacOSX27 `.tbd` 与旧 linker 不兼容而 exit1，原始失败保留；指定项目 Xcode/SDK 后 exit0、stderr 空。本轮没有 CI run、新 kernel/boot build、设备播放或分区操作。
