# 蓝牙合入与 ColorOS 原厂振动波形核对

## 结论

- 蓝牙生产树已正常三方合入 `main`（`d0609eda`），辅助分支的等价原始历史另行保留（`790c3b78`、`792f2bf7`）。已 push 后删除本地六条蓝牙分支及远端 `feat/bluetooth-native`。
- 清理后，本地 **11 条分支（含 main）**、远端 **28 条（含 main）**，同名去重 **34 条**。未被任何实际远端分支包含的独立提交 **45 个**：`feat/camera-ios-preview` 33 个、`feat/camera-source` 12 个。其它本地分支的未 push 提交均为 0；未合 main 不等于未 push。
- 官方同平台、同 device_id 的源码确有**非单一 173 Hz 正弦的 RTP 字节波形**。按源码默认采样率分析，其中部分按键波形的频谱峰落在约 281–284 Hz。不能因此宣称现版 PLJ110 ColorOS 实跑覆盖 80–300 Hz 每个频点，也不能把频谱峰当成马达实际运动频率。
- **把 iOS 80–300 Hz 全部映射到 170–173 Hz 不保证更好。** 连续振动可能更强、更高效，但短 tap 的起振、停止、锐度和刹车可能变差。用户尚未实测，本轮没有改转换器，保留透传基线。

## 1. 合并与保留

输入 `main=3fad865c`、本地主蓝牙 `e3647484`；此前远端蓝牙是较旧的 `f3332b89`。

正常生产 merge 无冲突。69 个蓝牙改动文件中，除合并交接的 `NOTES.md` 外，最终 blob 均与本地主蓝牙一致；main-only 文件全部保留。20 个 Python 文件 AST 解析通过。最终 staged diff-check 在 `0006-x9-bt-uart-irq.patch` 的序列化上下文行上有 6 个 space-before-tab 警告，未为消除警告而改变原 patch。

四条辅助分支 `feat/bluetooth-integration`、`feat/bluetooth-ios-power`、`feat/bluetooth-own`、`fix/bluetooth-launch-cache` 的非主蓝牙原始提交分别为 7/6/5/1 个，`git cherry e3647484 <branch>` 全为 `-`。它们不是新的生产 delta；在正常生产 merge 后，通过 history-only `-s ours` merge 保留原始 hash，前后 tree 相同。旧 `feat/bluetooth-status-20261115` 的 `c14d363b` 同样已证明 patch-equivalent，单独保留历史且 tree 不变。

已 push `main` 至 `792f2bf7` 后，确认上述分支均为远端 main 祖先，再清理对应分支/worktree。本轮报告与最后交接随后正常提交、ff 合入并 push，不改变生产代码。

原 ignored 产物共 117 项无覆盖保存：不冲突项迁至主 checkout 的同名 `x9/out/<name>`；冲突项及额外 ignored 输入保留在 `x9/out/bluetooth-branch-archive-20261116/`。候选仍在 `x9/out/bt-native-rootboot13-20261116/boot/boot.img`，**未上机验证**。没有改长期 `.local` 输入或备份，没有编译 kernel/boot、启动 CI 或操作设备。

## 2. 官方数据是否给这颗马达使用

参考仓库 `.local/references/oppo-modules-dt-mt6993`，HEAD `4d505a4292dab3176a45fec66dc85debc362e24c`，对应 OPPO 公开 MT6993 厂商模块/设备树。它是官方参考，**不是当前手机正在执行的驱动二进制证明**。

振动源码根：

```text
.local/references/oppo-modules-dt-mt6993/vendor/oplus/kernel/vibrator/oplus_haptic/
```

已保存设备 DT `.local/x9/modem/live.dts:9272` 的 `aw_haptic@5A` 含 `qcom,device_id=<0x329>`，即 815，并有 AW8693XS 属性。官方路径为：

```text
haptic_common_driver.c:2537 get_rtp_key_data
  → device_id == DEVICE_ID_0815
  → :1620 custom_0815_rtp_key_file
  → 按效果 ID 与 haptic_get_f0 返回值选择真实字节数组
  → haptic_hv.c:2373 rtp_key_work_routine
  → memcpy 到 aw_rtp->data，上传 OSC trim，RTP mode / GO
  → :1149 rtp_play → set_rtp_data
  → aw8693xs.c:447 → RTPDATA I2C 写入
```

该路径受 `OPLUS_FEATURE_CHG_BASIC` 等构建/运行条件约束；本轮没有资格当前设备的实际编译配置或效果调用。

`haptic_hv_rtp_key_data.h` 含 140 个真实数组，其中 `aw_haptic_0815_*` 有 35 个，后缀为 **162/166/170/174/178 Hz**。选择阈值见 `haptic_common.h:58–62`：1640/1680/1720/1760/1800。这是依据执行器 f0 选择的**调校版本**，不能只看文件名就断言所有字节是该频率的单一正弦。若沿用此前交接记录的 Android f0=1729，则会选 174 Hz 版本；本轮没有重采这个 f0 值。

## 3. 真实字节的离线频谱

`haptic_hv.c:2610` 初始化为 `AW_PWM_24K`；检查的按键路径没有另设采样率。本轮采用 **24 kHz 条件**，uint8 字节按 signed 8-bit 解码、去均值、不额外加窗，FFT zero-pad 至 65536 点，在 20–1000 Hz 范围寻找最大功率点。

| 0815 的 174Hz 版本 | 样本数 | 持续时间（24k 条件） | 频谱峰（约） |
| --- | ---: | ---: | ---: |
| RTP 110（INPUT_LOW） | 317 | 13.21 ms | 281 Hz |
| RTP 111 | 336 | 14.00 ms | 284 Hz |
| RTP 112（INPUT_HIGH） | 340 | 14.17 ms | 283 Hz |
| RTP 302/303（SG DOWN/UP HIGH） | 各 248 | 10.33 ms | 344 Hz |
| RTP 304/305（SG DOWN/UP LOW） | 各 235 | 9.79 ms | 342 Hz |

这些是短时、宽带的复合脉冲。zero-padding 只细化取样栅格，不提高短波形的真实频率分辨率。**频谱峰不是一个“281 Hz 载频命令”，更不是机械输出实测。** 实际时钟、OSC 校准、boost/gain、自动刹车及马达响应都会影响最终输出。

可以确认官方参考输入包含 80–300 Hz 区间的频谱内容，且不全是固定 173 Hz 正弦；不能确认现版 ColorOS 在这颗马达上覆盖整个 80–300 Hz 范围。当前系统的 RichTap 动态资源、真实 PCM 写入与寄存器采样率尚未抓取。

## 4. 为什么不立即做统一频率压缩

马达约 173 Hz 的机械共振不是输入格式限制。RTP 支持波形输入，而机械响应对频率、时长和幅相敏感。

- **连续效果**：将稳态载频靠近共振可能提高振幅/效率，但 170–173 Hz 的窄范围基本会丢掉原来的频率层次；更强不等于更像 iOS。
- **短 tap**：并非稳定正弦，不能简单按一个频率标签重调。直接改采样率会同时改变时长；保时长的重合成又可能改变推力、制动和锐度。官方本身也在使用非单一共振正弦的短脉冲。
- **现有基线**：此前真实 converter 离线验证保留 80/173/230/300 Hz；Apple D221 的输入也已经包含其自身频率相关增益，不是未调校通用波形。

下一步先用同一镜像/增益对少量明确事件实测透传：短轻 tap、短强 tap、长低频效果，分别记录力度、起振、停止和余振。只有基线结果说明问题后，再对“连续/瞬态”分别适配，并一次只改一个变量。当前不恢复旧版固定载频包络重合成，不把所有效果强行压到 173 Hz。

## 5. 库存口径与复核

未 push 定义为“从任一本地分支可达、但从 `git ls-remote --heads origin` 返回的任何真实远端分支均不可达”的 commit hash，最终去重。没有同名远端不代表其提交尚未 push，例如几个 HEIF 分支与远端 `feat/hevc-hw` 共享已发布历史。

库存详单、45 个提交的 hash/subject、原始 FFT 数据、保存/删除清单及 merge tree 验证均归档到：

```text
x9/out/bluetooth-merge-20261116/
  inventory.json / unpublished-commits.txt
  coloros-analysis.json / analyze_coloros.py
  cleanup-bluetooth.json / bluetooth-preservation-inventory.json
  merge-tree-check.json / pre-*-tree.txt / post-*-tree.txt
```

重复 FFT 时使用新输出目录，不覆盖原始结果：

```sh
mkdir -p x9/out/coloros-haptic-replay-<唯一名称>
/opt/anaconda3/bin/python3 x9/out/bluetooth-merge-20261116/analyze_coloros.py \
  .local/references/oppo-modules-dt-mt6993/vendor/oplus/kernel/vibrator/oplus_haptic \
  x9/out/coloros-haptic-replay-<唯一名称>
git branch --list '*bluetooth*'
git ls-remote --heads origin 'refs/heads/*bluetooth*'
git merge-base --is-ancestor e3647484 origin/main
```

另一个活动 SMS 工作区 `feat/sms-retrieve-retirement` 的未提交 `NOTES.md` 独立保留、不计入 commit 数。本轮未切换或写入该工作区；前后私有 diff 内容不同（同期 SMS 小节有措辞更新），不能宣称前后字节完全不变，更没有为了得到相同 diff 而恢复/覆盖它。
