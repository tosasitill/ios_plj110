# SMS 同 ID map 退休与 ACK owner 边界

> 2026-11-16。**已资格的是 CommCenter 内存 Operation map 退休，不是物理 ACK 或
> Messages COMMIT。** 完整通话短信目标仍 active，无生产行为修改或设备验收。

## 范围与输入

延续 [ACK handler checkpoint](2026-11-16_reverse-sms-ack-handler-report.md) 与
[retrieve route](2026-11-16_reverse-sms-retrieve-route-report.md)。本轮单代理
`feat/sms-ack-owner`，clean parent `ff9e900f`；原 CommCenter UUID 为
`20b0ce28-18a1-3eea-aa2d-ef53e33c039a`。

私有证据：`x9/out/sms-ack-owner-20261116/`，包含 `scope.md`、`timeline.md`、
`ack-owner-contract.json`、命令 `*.exit.json`、原读记录及完整 FS 资格。
所有函数从上一轮保留的 NEW 原 text 库存 promote；不重扫原件 text，也不重新
遍历原件的 80 页 chains。新 bounded 字符串与必要 metadata/import 读均 ≤1024B。

无测试/人工向量、kernel/boot/CI、设备/ADB/mailbox/mount/数据库/分区操作。
原字节与日志不进 git。source 只用于解释静态边界，不代表这些路径已在设备执行。

## Evidence → Finding

以下 source_ref 相对私有证据目录；content_hash 为 n/a：复用既有 UUID、metadata
及原读记录资格，本轮不新 hash 原件或成功 kernel。

| Evidence | source_ref | 观察 |
| --- | --- | --- |
| E-001 | `ack-follow-data.json`、`ack-import-cache.json` | 第二 key 为 `kSmsMessageIsMobileOriginated`，reply key 为 `kSmsMessageWasAcknowledged`；bool 转换与 bool_create 有旧精确 proof |
| E-002 | `ack-follow-data.json`、`root-model-ack-e0.*`、`root-model-ack-*-invoke.*` | 原 Model AP+e0 slot101528df0→1000c05b0；queue invoke 保原 ID，并执行1000c7878 |
| E-003 | `root-model-ack-core-invoke.asm`、`root-ack-tree-detach.*`、`root-ack-operation-pair-release.*`、`ack-node-free-import.json` | 同 ID match 分支更新 begin、size减1、detach、release node pair、delete，返回1；miss返回0 |
| E-004 | `ack-control-slot.json`、`ack-operation-dtor-slot.json`、`root-ack-control-last-owner.*`、`root-sms-operation-deleting-dtor.*`、`root-ack-registry-pair-release.*` | 原 admitted SmsOperation 的 control last-owner→Operation deleting dtor；释放字符串与 Registry pair |
| E-005 | `ack-count-slot.json`、`root-model-incoming-count.*`、`root-incoming-count-*.asm` | 原 incoming-count +d8 slot101528de8→1000c0264；core1000c6b40读同Model+68 |
| E-006 | 当前仓库 `X9Messages.inc`、`x9_mipc_messages.inc`、`coachz_baseband.cpp`（详见下节） | 当前 kernel WMS37 按原 native command token/QMI owner 工作；与 app Operation 通知ID不是同一命名空间 |

### F-001：原 ID 条件调用，非 MO 不自动代表 modem ACK

- severity: n/a_re；category: other；status: validated（静态）；confidence: high。
- evidence_ids: E-001、E-002；location: 1004bad6c、1000c05b0。
- 旧 ACK handler 对 `kSmsMessageID` 用 unsigned/default -1；第二字段 bool/default
  false。ID=-1 或 MO=true 时跳过 service+e0，reply bool 为 false。
- 允许分支保原 u32 ID。+e0 wrapper 捕同 Model、指向该 ID 的 stack storage，并
  使用旧精确 imports `dispatch_sync` 或 `dispatch_async_and_wait`。两 invoke 把
  core 返回值写原 result byte，wrapper再转 bool。
- 这限定原 handler 的此条路径；没有由字段名推出 modem receipt。

### F-002：同 Model+60 Operation map 的真实退休

- severity: n/a_re；category: other；status: validated（静态）；confidence: high。
- evidence_ids: E-002、E-003、E-005；location: 1000c7878..1000c7964。
- core 以原 u32 ID 对同 Model+60 树作 unsigned lower-bound，再验证等值。
  root为空或无匹配返回0，不进入删除分支。
- match 分支计算 successor；若 node 是 Model+58 的 begin，则更新 begin；
  **1000c7930..1000c7938 将 Model+68 size减1**。
- 1000c7940 调完整 tree detach/rebalance100006610；1000c7948 对 node+28
  owning pair 调1000c6834；1000c7950 调精确 Mach-O import `__ZdlPv`，最后返回1。
- 旧 admission1000bfc18 以同 ID 写 node+20、同 Operation pair写+28/+30，
  入同 Model+60 树并加同 Model+68；旧 retrieve1000c6bb8在该树只 lookup/retain。
  本轮不能把这几个不同操作混为一次 lookup。
- 原 incoming-count handler1004ba8f4的+d8实际消费者1000c6b40读取的，正是
  同 Model+68。故 count 是已资格的同状态观察面，不是依字段名猜测的缓存值。

**条件保留**：实际 server+128 Registry 安装、runtime当前选择此具体 Model、request
调用到此路径仍未闭合。无并发重新插入/ID复用且同 owner 时，退休后的 lookup 将无该
entry；未运行请求、重放或设备测试来验证此推论。

### F-003：map 删除与最后 Operation 析构必须分开

- severity: n/a_re；category: other；status: validated（静态）；confidence: high。
- evidence_ids: E-003、E-004；location: 1000c6834、1000c6784、10042d120。
- node pair release 只在 control+8 **原计数为0** 的 last-strong 分支调
  control virtual+10，再调旧 weak-release。其它 strong owner（包括 retrieve
  retaining owner）可延迟析构；map 已删除不意味着 Operation 当场析构。
- 对已资格通知创建路径中的 SmsOperation，旧 dispatcher1000c6258安装 control
  AP101529378、control+18保存该 Operation。其+10真slot101529388→1000c6784；
  后者取同 control+18对象并 tail 调 Operation virtual+8。
- 原 SmsOperation AP101561878+8真slot101561880→10042d120。该 deleting dtor
  释放 owned strings及 Operation+58 Registry pair，经10000f394 last-owner处理；
  未观察其直接提交 modem ACK、修改短信 SQL 或 COMMIT。
- 此结论限定已资格的 SmsOperation 类型，不替所有 map entry 的动态类型作保证；
  Registry 最后 owner 的全部析构副作用也未穷尽，不能用“只是析构”证明下游全无副作用。

## P-001：同 ID 的已闭合路径

path_type: callflow。

1. 旧 admission 同 ID→同 Model+60/node+28 owning pair（旧通知证据；E-003/F-002）。
2. 原 acknowledge handler 取同 ID/MO，条件调用 service+e0（E-001/F-001）。
3. actual Model+e0→queue invoke→同 ID match（E-002/F-002）。
4. 更新 begin、size减1、detach/release/delete→reply bool（E-003/F-002）。
5. 若原 pair 为 last strong且类型为已资格 SmsOperation，则进入其删除析构
   （E-004/F-003）；否则只有该 map owner 被释放。
6. 原 incoming-count读同 size（E-005/F-002），不是物理 ACK receipt。

路径终点是内存 map 退休；COMMIT/物理 ACK 是未资格的另一条路径。

## 当前生产桥接核对：没有据此新增 adapter

source_ref 与静态读范围：

- `x9/kernel/src/iokit/Kernel/arm/X9Messages.inc`：`x9m_ack_repeated`、
  `x9m_ack_response`、`x9m_deliver`、WMS37分支、ACK poll。
- `x9/kernel/src/bsd/dev/x9_mipc_messages.inc`：message publish/reply。
- `x9/kernel/src/bsd/dev/x9_mipc_bridge.inc`：唯一 writer `x9mi_tick`。
- `x9/baseband/qcom/baseband/coachz_baseband.cpp`：actual QMUX ACK identity/result。

当前 source 的 WMS37取请求中的原 native command token，并核 delivered owner的
SIM、QMI slot/local generation及actual QMUX generation；再以原 MIPC command
txid提交8602/8604、由唯一 writer取物理 TX结果并向原 host queue送 response。
它没有用 CommCenter Operation通知u32 ID定位 native command。相关 source也明确
不把 physical TX/host admission当durable保存。

因此，本轮证据没有支持把 `kSmsAcknowledgeMessage` 挂到这个 kernel ACK API；不新增
无真实 caller 的转换 adapter、不虚构两个 ID 的关联、不修改生产行为。这是当前源码
观察，不是新运行日志，也不声称现有 bridge 的全部 owner/race或物理功能已验收。

## 不重采的复核步骤

在仓库根执行，仅看既有产物，不访问原件、kernel或设备；另一机器需预先具有私有
证据目录。各 reader/promote 的真实 argv/cwd/exit见各 `*.exit.json`，实际顺序见
`timeline.md`。

```bash
cat x9/out/sms-ack-owner-20261116/ack-owner-contract.json
grep -n -E '1000c78a8:|1000c78cc:|1000c7930:|1000c7938:|1000c7940:|1000c7948:|1000c7950:' \
  x9/out/sms-ack-owner-20261116/root-model-ack-core-invoke.asm
cat x9/out/sms-ack-owner-20261116/root-incoming-count-core.asm
cat x9/out/sms-ack-owner-20261116/root-ack-control-last-owner.asm
cat x9/out/sms-ack-owner-20261116/ack-node-free-import-r1.exit.json
```

## 本轮范围与下一窄边

新完整原FS **13函数2128B**，新精确 import **1处**（operator delete）；bool/queue
imports复用旧 proof。原text新读取 **0B**，原chain页/selected slot重读取 **0**；
promote、selected-data及新import reader均actual exit0。曾误引用不存在的旧constructor/
report文件名，相关 stderr/纠正顺序保留于 timeline；不计新原件采样或资格结论。

下一不重复这13函数或前轮handler：从旧 parser结果 helper1007f72bc 在1007f7720
调用的 **ACK function virtual+30**追原 callback动态类型/安装来源及其原token→
driver WMS37；原 parser callback100813f14保存/复制的 supplied ACK function也作为
已读 consumer边。app map退休不能替这条 physical ACK owner链。

同时仍缺 server+128 Registry安装/current concrete选择、publisher原init entry、
GUID/record、Messages COMMIT/重开、MO对端，以及 CALL逐动作record/Client/UUID/
nativeepoch退休、typed DIAL、CallList/handover与双向媒体。完整目标未完成。
