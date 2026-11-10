# iPhone X / 20H392 的控制中心与 5G SA 逻辑

## 结论与范围

**不能由“根盘来自 iPhone X IPSW”推出“X9 永远不能用 5G SA”。**
本地 iOS 16.7.16 / 20H392 的 **arm64** 用户态保留了 5G/SA 的 API、
QMI 注册解析和 SA 设置命令。之前 NOTES 中“CommCenter 没有 5G 概念”的判断撤回。

但这也**不等于原封不动的 D22 配置已经支持 SA**：NR 处理有 radio personality
能力门，当前桥接缺 NR System Info，而且数据呼叫/收发尚未验证。
真正的 iPhone X 原装基带不能因为这些代码而变成 5G 基带；本项目讨论的是
MT6993 基带 + 自编译兼容层 + iPhone X 用户态，不是原装 iPhone X。

范围：只读本地已提取 Mach-O、根盘快照 dyld cache、现有 USB 日志和桥接源码。
初次分析没有接触设备、改 Apple 二进制、编译、CI、上机或修改蜂窝策略。
后续获用户同意,在独立 `x9-nrsa` 分支准备了桥接/用户态实验副本并本地构建,见文末;仍未操作设备。
源码观察基线为 `137ba97`；该提交在本轮分析期间由其他工作更新，不属于本轮修改。

## Evidence → Finding → Path

证据目录：`.local/x9/work/cc-5gsa-20H392/`，不入 git。
以下地址均为 **20H392 未 slide 的 VM address**，不是文件偏移。
源文件哈希未新计算（content_hash=n/a）；使用 LC_UUID、根盘 SystemVersion
以及四处目标代码与快照原 cache 的逐字节一致性确认来源，详见 `provenance.txt`。
这些是本地快照/静态证据，不冒充本次设备内存取证。

| E-id | source_ref / 关键证据 | 对应发现 |
|---|---|---|
| E1 | `ConnectivityModule.methods`、`cellular-buttons.asm`、`cellular-update.asm` | F1：控制中心蜂窝按钮是数据总开关，不选择 RAT。 |
| E2 | `CoreTelephony.symbols`、`ct-get-supports-sa.asm` | F2：SA API 有实际 XPC 调用路径，不是只有常量。 |
| E3 | `nr-sysinfo-response.asm`、`nr-indication.asm`、`qmi-enable-sa.asm` | F3：QMI 驱动有 NR System Info 解析与 SA 命令构造代码。 |
| E4 | `nr-registration-gates.asm`、`radio-personality-ctor.asm` | F4：NR 分支受 personality 和输入有效性门控。 |
| E5 | `~/167.txt:6301,12713–12714`、`~/168.txt:4050,9056–9057`；`X9QMIAT.inc` 的 `x9q_nas` | F5：MTK 曾注册 SA；桥 NR 信息不完整；当前镜像主动限定 LTE。 |

F1–F4 为高置信度静态发现，severity=n/a_re，status=confirmed-static。
F5 的注册与 LTE-only 为日志确认，NR 信息缺失为源码/既有消息形状确认；
“补齐后可以完成 SA 上网”仍是 **未验证假设**。

### P1：控制中心按钮（callflow）

`CCUIConnectivityCellularDataViewController`：

1. `buttonTapped:`，`0x209d598d8` → `_toggleState`。
2. `_toggleState`，`0x209d599e0`：非演示模式创建 CT connection，
   `0x209d59a4c` 调 `_CTServerConnectionGetCellularDataIsEnabled`，
   `0x209d59ae0` 将取反后的布尔值交给 `_CTServerConnectionSetCellularDataIsEnabled`。
3. `_updateState`，`0x209d55a18`：读取 `_CTServerConnectionGetCellularDataSettings`，
   结合限制/演示状态刷新按钮。

这条按钮路径并不设置 LTE/NSA/SA。按钮可见或变绿不能证明 5G 注册，更不能证明数据通。

### P2：SA API 与 QMI 驱动（callflow）

- `CoreTelephonyClient(Registration) getSupports5GStandalone:error:`，
  `0x1881b8170`：取得 `synchronousProxyWithErrorHandler:`，再在 `0x1881b824c`
  调 `getSupports5GStandalone:completion:`，不是直接返回常量 false。
  同库还有 `setSupports5GStandalone:enabled:`、NR SA/NSA RAT 常量。
- `libCommCenterMCommandDrivers.dylib` 的 GetSysInfo response 回调：
  `0x1f1b08910` 查 TLV **0x4B**，`0x1f1b08934` 调
  `tryTlv<nas::tlv::NR5GSysInfo>`（`0x1f1b098ac`）。
  SysInfo indication 回调在 `0x1f1b0f924` 传 **0x4D**,`0x1f1b0f928` 调同一解析器。
  **共用解析器不代表标签相同**:NR service status在响应/通知中也分别为 **0x4A / 0x4C**。
- `QMINetworkRegistrationDriver::enable5GStandalone`，`0x1f1afc288`：
  建 NAS **0x33 SetSystemSelectionPreference**，写入 `NRDisableMode`
  （由 enabled 取反）和 `EnableNrUpdateReason`，经 `ClientRouter::SendProxy` 发送。
- CommCenter 本体还有 `Show5GStandaloneSwitch`、`Enable5GStandaloneByDefault`、
  `5GStandaloneEnabled` 及运营商/用户设置相关日志字符串。这一项仅作策略定位线索，
  没有据此宣称相应策略已在设备上启用。

### P3：当前未打通的门（callflow）

`QMINetworkRegistrationDriver::processSystemInfo_sync`，`0x1f191c440`：

- `0x1f191ce04` 调 `Registry::getRadioPersonality()`，随后读取对象 **+0x121**；
  `0x1f191ce2c` 为零则跳过 NR 分支。
- `0x1f191ce30–0x1f191ce3c` 还检查 ServingSystemInfo 的两个 presence 字节；
  NR 路径随后调用 `translateToNasRegistrationState`，并包含
  `NR system is present, processing it` 日志。
- `GsmRadioPersonality` 构造器会按硬件配置在部分分支设置 +0x121；例如
  `0x1f036d6c4` 写 1。因此“二进制含 NR 代码”与“D22 当前能进 NR 分支”是两回事。
  **没有读取设备上该对象。后续已离线求值D22构造分支,返回时该位为0;仍不能把它当作设备实测。**
- 初次分析时的 `x9q_nas` 对 NR 只给 **0x4A 的三字节 Service Status**；只在 LTE 注册时
  生成 **0x19 LTE System Info**，没有生成 **0x4B NR System Info**。
  所以 167 中“SA 时不推进、LTE 后推进”的差异不能用于证明 NR 解析代码不存在。
- ic/id 沿用 `AT+ERAT=3`，主动 LTE-only；这个限制是实验策略，不是 IPSW 能力结论。

## 实测边界与下一步

167 在 24.503 秒：`C5GREG 1 / CGATT 1 / COPS-AcT 11`，说明 MTK 曾注册 NR SA。
54.509 秒换到 `CEREG 1 / C5GREG 0 / AcT 7` 才发完整 LTE System Info。
168 在 17.294 秒明确记录 `LTE-only AT+ERAT=3 -> OK`。
这些都不证明 iOS 的 SA PDU session 或网络收发成功。

建议保留 LTE 基线，另做最小 SA 分支，而不是先换整套 iPhone 14 Pro IPSW：

1. 定位/观察 +0x121 的 D22 初始化及消费者，确认能否只向兼容层暴露真实 X9 NR 能力；
   不改整机身份来顺带开启一堆未实现功能。
2. 从真实 MTK 状态补 NR System Info、信号、RAT/SA 策略的双向映射；
   对未获取的 TAC/cell 等标 invalid，不把 NR 伪报成 LTE。
3. 核对运营商/APN/SA policy 与当前仍拒绝的 NAS 0x33，之后才在独立候选中解除 LTE-only。
4. 最终验证：CommCenter 接纳 NR → WDS Start → MIPC 激活 → DPMAIF 收发 →
   iOS 实际联网；显示 `5G` 不作为最终成功条件。IMS/VoNR 是另外的验证项。

## 离线复核

在仓库根运行；Python 需 capstone；原始提取目录和 `/tmp/x9root` 快照需仍存在。
私有脚本仅用于本地解码，没有包含 Apple 二进制或设备身份。

```bash
E=.local/x9/work/cc-5gsa-20H392
python3 -I "$E/macho_evidence.py" dis 0x209d599e0 0x140
python3 -I "$E/macho_evidence.py" dis 0x1881b8170 0x19c
python3 -I "$E/macho_evidence.py" dis 0x1f1b0890c 0x78
python3 -I "$E/macho_evidence.py" dis 0x1f1afc288 0x280
python3 -I "$E/macho_evidence.py" dis 0x1f191cdf8 0x178
rg -n 'NAS registration|NAS SysInfo|LTE-only' ~/167.txt ~/168.txt
rg -n 'p.tlv\(0x19|p.tlv\(0x4a|p.tlv\(0x4b' \
  x9/kernel/src/iokit/Kernel/arm/X9QMIAT.inc
```

## 获准实施后的离线候选（`x9-nrsa` / `dde26eb`）

完整构建/安装边界以 `NOTES.md` 的 **HANDOFF 105-SA** 为准。

- **E6 → F6 → P4**:新增私有 `nrsa-20261110/evaluate_d22.py` 和
  `d22-evaluation.txt` 对原始20H392指令做局部执行,得出
  `D22 → Telephony product 0x40 → hardware config 7 → ctor +0x121=0`。
  输入模型和少量外部调用有明确建模;这不是整台iOS仿真或设备内存测量。
- **E7 → F7 → P5**:`X9QMIAT.inc` 已修正 NR 查询/通知标签,并从
  `C5GREG + COPS(AcT=11) + CGATT` 生成29字节NR信息;保持未知位置字段invalid。
  Apple解析器 `0x1f194bd78–0x1f194bfdc` 最终消费0x1d字节,与libqmi一致。
- **E8 → F8 → P6**:新增 `prepare_nr_registration_cache.py` 只产生离线cache副本,
  不安装、不覆盖源文件。UUID/原指令匹配后将构造器两条指令共8字节替换,
  保留+0x120并增加+0x121。作用case为硬件配置5–10,D22使用7。
  这只开放已知NR注册门,**不承诺SA设置、信号和数据策略全部开放**。
  原签名没有重新生成,设备接受这个副本尚未验证。

本地准备命令（必须在独立worktree运行;输出文件必须不存在）:

```bash
cd .local/x9/work/ios_plj110-nrsa
python3 -I x9/tools/prepare_nr_registration_cache.py \
  --source /tmp/x9root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.31 \
  --output .local/x9/work/nrsa-20261110/patched-cache/dyld_shared_cache_arm64.31 \
  > .local/x9/work/nrsa-20261110/cache-patch.json
```

已有副本不要覆盖;再次准备需选择新的输出名。精确改动为cache组件偏移
`0x36ed1b0`: `2900805269820439` → `2920805269420279`。
此处不是根盘/UFS偏移;部署前还须确认原始字节、实际文件映射和可回退安装方式。

`x9-rev252nr1-md/boot.img` 本地构建成功,插件275不变。
实验参数 `x9_md_lte=0` 仅取消主动LTE-only,不强制SA;
**boot.img不包含上述cache补丁,尚未具备已验证的SA启动/联网链**。
原LTE镜像和设备根盘均保留。未上机、未执行额外测试套件、无CI run。

NAS0x33的进一步线索已收敛为 `NRDisableMode=0x2E/u32 LE`、
`EnableNrUpdateReason=0xA4/u8`;MTK策略映射与NR信号尚未实现。
下一个决定性证据应是补丁加载后的CommCenter NR处理日志,随后才是数据面实收发。
