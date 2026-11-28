# 短信入站：CommCenter 消费后不完成的定因与最小判别方案

**结论：桥帧与驱动侧路径已逐项验清、等价于原厂输入；断裂收窄到 CommCenter
核心的 consumer/response 一层——最可能 producer 给出 raw2/null Model，
consumer 按原厂语义直接日志/返回、永不调用 ACK callback，于是无 ACK、
无投递、无任何错误。** 这是静态定因，不是设备验收；下一步按本文第 4 节
判别，不在判明前改桥帧形或伪造 ACK。

基线：origin/main `7fb3eca6` 时点调查（文档合入时以合入点为准）。
原件：DSC 提取的 20H392 `libCommCenterMCommandDrivers.dylib`
（UUID `7C3F4685-46D6-3178-B3EA-B43E59F11923`，全本地符号）；
核心侧引用树内 `docs/2026-11-16_reverse-sms-parser-model-report.md`
（CommCenter 主二进制静态证据）与 `docs/call-ios-bridge.md`。
设备证据：`~/round3-act-20261128.txt`、`~/round3-iomedia-20261128.txt`
（两轮同构：queued 与 host read 同瞬、30s ACK 缺席、depth 归 0）。

## 1. 消费后路径图与每环完成条件

```text
桥 bb_x9_sms_event 入队 → CommCenter QMUX 读走（= 设备日志 "host read"）
  → qmi::ClientRouter 按 (WMS, msg 0x0001) 分发
  → QMISMSCommandDriver registerIndications_sync::$_1 handler
      ├─ 门 A：(driver+0x70 成员, slot) 导入查询 bit0=1
      │        slot 由 SubscriptionType 派生：(type-1)<3 ? type : 0
      │        bit0=0 → 仅记日志、直接返回（全静默）            ← 缺口 G2 面
      ├─ QMIServiceMsg 形态分派（[+0x28]==1 && [+0x2c]==3 走主路，
      │   否则走 [+0xc] msg-id 的兼容路；两路汇合，非丢弃门）
      ├─ TLV 抓取：0x10 MemoryStorageIdentification（可选）/
      │   0x16 MTSMSOnIMS / 0x1a WarningArea /
      │   0x11 TransferRouteMTMessage / 0x13 ETWSMessage
      ├─ format==7（GW Broadcast）→ 广播支；其余 → 主路
      ├─ 门 B：[driver+0x88]（基类成员）非空；为空 → 清理返回   ← 缺口 G2
      ├─ handleMTMessage(slot, mtMsg, onIms)（已内联进 handler）：
      │   构造 sms::MTSmsDetails（导入 ctor：slot, 1,
      │   format 映射类 6→1/7→3/0→2/其余 0, &PDU 向量, 0, ack_ind==0）
      │   构造完成 callback（std::function<$_54>，0x48B __func）：
      │     捕获 +0x40 = MTSMSOnIMS 标志 → sendAck 的 boolC
      │     捕获 +0x41 = (ack_indicator==0) → callback 内的 ACK 门
      └─ 提交：[driver+0x80] 对象 vtable+0x20(details, callback)
  → CommCenter 核心 Controller：parser response（XPC dict 门 +
    decoded PDU pair）→ consumer 1007f622c(response, details, callback)
      ├─ callback 的 callable 槽 (+0x18) 为空 → 提前退出
      ├─ details+8 选解码器：1 → GSM SmsPduDecoderSerialized；
      │   2 → C2K；构造 GSMModel/C2KModel（强 pair 持有）
      ├─ 解码失败/response 缺门 → **raw status 2 + null Model**
      └─ helper 1007f72bc(details, raw status, Model pair, callback)
          ├─ **Model 为空（raw2 入口）→ 直接日志/返回，不调 callback** ← 缺口 G1
          ├─ (1<<status)&0x1b2（status 1/4/5/7/8）才进 Controller+e0
          │   处理组（存储/通知方向）；其余分支可跳过该组
          ├─ 终态特殊分支可清 pair 并把 status 改为 1（raw1/null 回调）
          └─ callback 非空 → virtual+30(最终 status, 当前 pair)
  → QMISMSCommandDriver::handleMTMessage::$_54 callback(status, Model)
      ├─ status 9 (CMASSuccess) → 静默返回，永不 ACK（设计如此）
      ├─ status 1 (MemoryFull) → sendAck(null, 0,0, txn, fmt, C, 1)
      ├─ 捕获 +0x41 == 0 → 静默返回（ack_indicator!=0 的帧走此支）
      ├─ status 4 (SmsSuccess) → sendAck(Model, 1,0, txn, fmt, C, 0)
      └─ 其余 status → sendAck(null, 0,1, txn, fmt, C, 0)（失败 ACK）
  → QMISMSCommandDriver::sendAck(...)：**无阻塞门**，被调即组帧发送
    QMI WMS msg 0x37（必备 TLV1 = {txn, format!=0 标志, 失败字节}；
    可选 TLV 0x12 = boolC；失败时 TLV 0x11 = cause 2/3 + code 102/103）
  → 桥 bb_x9_sms_ack_result 收下 → 回 MTK 层 ACK，所有权释放
```

要点：除 status 9 与 callback 未被调用两种情形外，**任何** callback
触发都会在桥上留下一条 0x37 请求（含失败 ACK）。设备上 0x37 恒缺席，
所以断裂必在 callback 被调用之前。

## 2. 已证伪（离线或设备证据，不再回头查）

| 假设 | 证伪依据 |
| --- | --- |
| format 值错 | MessageFormat 表（asString @ VA 0x20dd1f380）：6 = GW Point to Point，桥发 6 正确；7 才是广播支 |
| ack_indicator 语义反了导致 callback 门关 | 门字节 +0x41 = (ack_indicator==0)，由 handler 在构造 __func 时逐指令确认；桥发 0 → 门开。sendAck 本体无任何阻塞门 |
| TLV 形制不合 | 桥帧（0x11 值 = ack u8 / txn u32 / format u8 / len u16 / SMSC+TPDU；附加 0x12=1、0x16=0）与原厂 parseField 逐字节一致（先行结论复核通过）；0x12 不被 handler 读取，无害 |
| PDU 形态错 | in.pdu 为标准 PDU 形态（pdu[0]=SMSC 长度，pdu[pdu[0]+1] 为首 TPDU 字节，MTI=0 DELIVER）；桥分类 x9ms_pdu_class 与路由均已在设备上走通（帧确被入队并读走） |
| handler 没注册 / 订阅没武装 | 设备日志实锤：`X9 SMS: SIM1/SIM2 event subscription MT 1 MWI 1`（24.361s）——CommCenter 对双卡都发了 WMS Set Event Report 且 MT=1；registerEvents 用的与 handler 入口同款 (driver+0x70, slot) 门在双卡均通过 |
| details 构造与原厂不等价 | MTSmsDetails ctor 的全部入参（slot、常数 1、format 映射、PDU 向量、0、ack_ind==0）均由原厂 handler 代码从解析结果派生，我们的帧解析结果与真机帧同值 → details 必同值；"decoder class (+8) 被映射坏"随之证伪 |
| status 9 静默 | 9 = CMASSuccess，由 CMAS/ETWS 类路径产生；普通 SMS-DELIVER 在 consumer 内的 status 演化只有 4→5（decoder class 2 / SIM-download / details+4==2）或 4→7（语音信箱指示），到不了 9 |
| callback 是空 std::function | callback 由 handler 现场构造 __func 后随提交传入，槽 +0x18 即 __func 指针，非空；提交面为进程内 dylib（libCommCenterMCommandDrivers 由 CommCenter 进程加载），不存在跨进程丢函数的序列化边界 |

## 3. 剩余缺口排序

**G1（最强）核心 consumer 收到 raw2/null Model，callback 永不被调用。**
producer 侧（parser response 的 XPC dict 门 / decoded PDU 生成）在本
port 环境下对我们的 MT 提交给出失败或空响应，consumer 按原厂语义
"raw2/null 入口直接日志/返回"。与观测的吻合是唯一的完全吻合：无 ACK
（连失败 ACK 都没有）、无投递、无报错、双卡同构、出站与送达报告
（不经此 consumer）全正常。具体子因（response 哪把门没过、decoded
PDU 为何为空）在主二进制 producer 侧，离线材料（parser-model 报告
明确"未读对应 key 字符串"）不足以再分，需运行时证据。

**G2（弱，离线不可排除）handler 门 B：[driver+0x88] 基类成员为空。**
该成员由 CSISMSCommandDriver 基类（主二进制/libCommCenterBase）维护，
本 dylib 内无赋值点；若为空，handler 在解析后、构造 callback 前返回，
签名同样是全静默。削弱证据：驱动其余功能（订阅、MO、存储查询、
8603 送达报告处理）全正常，基类成员单独为空的可能性低；且门 A 的
同款查询已在 registerEvents 路径上对双卡通过（但门 A/B 是不同成员，
不能互相证伪）。

**G3（与 G1 同层合并判别）提交后 response 永不回。**
parser-model 报告的开放端：Controller+9c0 的外部 consumer/supplier
"未闭"。若 MT 提交进了 Controller 但 response 生产者缺位，consumer
根本不会被调用，签名与 G1 不可分。判别上与 G1 合并：先证明 consumer
有没有被调用，再分 G1/G3。

## 4. 最小判别方案（按成本排序，逐级收窄）

### 步骤 1（零新代码）：unified log 归档轮
- 时机：任一带日志 boot 的轮次，给 SIM2 发一条 MT 短信，等 ≥60s。
- 取证：刷回 Android 后按已验证路径（拉 CrashReporter 文件同法）
  从 iOS Data 卷拉 unified log 归档（`/var/db/diagnostics` 下
  Persist/ 与当轮 timesync 文件），在 Mac 上用 `log show` 只查
  CommCenter 进程窗口（短信到达时刻 ±2min）。
- 判读：
  - 有 handler 的 MT 日志行（ack_indicator/transaction/format 值）
    → handler 已运行且解析成功，G2 的门 B 之前全通；继续看下条。
  - 有 raw2 分支的日志行（consumer 对 null Model 的日志）→ **G1 坐实**，
    下一步转 producer 侧：逐 key 对 response 门（需再做主二进制
    producer 的静态资格，读 key 字符串）。
  - handler 日志行有、consumer 侧毫无动静 → G3（response 永不回），
    转查 Controller 的 parser 派发/supplier 注册。
  - handler 日志行无 → 回到分发/门 A/门 B（G2），进步骤 3。
- 注意：OSKext 先例表明部分子系统日志可能被采样/关闭；若该窗口
  CommCenter 日志整体为空（连无关行都没有），本步无判别力，直接
  进步骤 2，不重复尝试。

### 步骤 2（小代码）：桥侧合成 PDU A/B
- 实现：新门控 `x9_sms_synth`（默认关，纯诊断，不与"修好默认开"
  口径冲突——它不是功能开关）。SIM2 host-ready 后 5s，用与
  `x9m_deliver` **完全相同**的构帧/入队路径（抽共用 helper，禁止
  另写一份构帧）注入一条教科书合法的 SMS-DELIVER（固定字节：
  常用 SMSC + GSM7 默认字母表纯文本、无 UDH），token 走原生分配器，
  sim=SIM2，ind 语义与真实 4602/8601 路径一致。
- 判读（30s 窗口，看既有 X9M_ACK/所有权行）：
  - 合成帧拿到 host ACK（0x37 到达）→ 真实帧的 **PDU 内容**是缺口
    （G1 的内容子因）：下一步逐字节 diff 真实捕获 PDU 与合成 PDU
    （首疑 SMSC 的 TON/NPI 与长度口径），桥侧可修，届时转 (a) 路线。
  - 合成帧同样无 ACK → 与内容无关，G1/G3 坐实在核心 response 层，
    桥侧无可改，进步骤 3 或转 producer 静态资格。

### 步骤 3（前两步仍分不开 G1/G2 时）：CommCenter 诊断 dylib
- 沿 X9BTAudioDiag 的 GOT 重绑定先例做只读诊断 dylib，注入
  CommCenter，挂 libCommCenterMCommandDrivers 的**导入发送路径**
  （sendAck 最终经导入的 QMI client 发送调用出帧），按
  (service=WMS, msg=0x37) 过滤计数并经既有诊断通道回报。
- 判读：计数 >0 而桥未收到 0x37 → 问题在桥侧 ACK 入队（回炉查
  bb_x9_sms_ack_result 的准入）；计数 =0 且步骤 1 显示 handler
  已运行 → 断点在 handler 门 B 与 consumer 之间，结合步骤 1 的
  consumer 日志行有无分 G2/G1。

## 5. 明确不做

- 不改桥帧形：已逐项验清，改动只会污染判别。
- 不由桥伪造 MTK 层 ACK：现行所有权模型（等 host 0x37 才回 MD）
  是刻意设计；伪造会掩盖投递失败并让 MD 侧状态与 iOS 不一致。
- 不在 CommCenter 二进制上做行为补丁：缺口未判明前任何补丁都是
  盲改；判明后若 producer 缺的是本 port 应提供的 supplier/注册，
  优先在桥/内核侧补齐环境，而不是改原厂二进制。

## 6. 上游核查

按"先查上游"规矩核查（2026-11-28）：hackios 生态（GitHub
hackios-xnu-kernel / HackiOS）自述状态表里 Cellular 尚在注册/数据
会话阶段、无短信完成路径结论；本项目此前引用的 ios-asteroids 先例
（0116）只涉及 assetsd TCC 沙箱扩展，与 SMS 无关；ARIstoteles 等
公开文献针对 Intel ARI 协议族，非 QMI WMS。**无现成结论或补丁
可移植**，本定因全部来自 20H392 原件反汇编与树内报告。
