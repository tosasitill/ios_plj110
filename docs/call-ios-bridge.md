# 通话 iOS 桥：已核实子集与启用边界

## 目前交付什么

这是**信令桥基础，不是已可使用的通话服务**。现有 WMS/IMSA 短信实现保持独立。

| 部分 | 实现 / 验证 |
| --- | --- |
| iOS 请求 codec | VOICE20 普通拨号有界解码，21 挂断、22 接听严格 tag1/u8；动作要求显式已验证的 route / 单通话映射 |
| Android 请求形状 | 新增本机卡2实测 `0901` 的六个 TLV；接听 `0907 {}`；挂断 `0905 {MODE100/u32=0, ID101/u32}` |
| 拨出状态生产者 | 完整 `4901` 的真实 ID / ECPI reducer；已实现、单对象可编译；未有完整真机 wire 重放或 iOS 运行验收 |
| 空快照与异步确认 | 完整 owned `090a` 数组解析、号码与原始presentation有界owner；`0902 + 新 MO epoch` 汇合生成 VOICE20/tag10/u8；DIAL 仍无生产 profile/admission 调用者 |
| 单通话挂断 / SIM 绑定 | VOICE44/tag01/u8 独立绑定；显式 VOICE21 → owned current query → 同 MO epoch/真实 ID/完整单通话 snapshot → guarded0905 → 同token0906 → 原iOS txn 的 result；已接请求分发，未设备验收 |
| 初始化 / 事件注册 | 明确绑定后的 VOICE03：13=0 与14=0/1 可独立并发 → 全局 CMD8901 → 逐 SIM IND4901/4902 → owned0909/090a；逐token确认，13=1仍拒绝；仅编译、未运行 |
| NAS 本地订阅注册 | NAS03保存原txn/localgen/QMUXgen，真实host queue receipt后才原子提交tag13/18/19订阅patch；FIFO、重复结果缓存及旧background IND gate；不是网络注册/IMS-ready |
| handover 通知 | 严格 MD4902/EVENT3 → Android SRVCC → Apple54/tag1/u8；订阅cursor/generation/SIM隔离、注册与live事件均整批queue receipt后释放，queue/OOM保序等待；未设备验收，未迁移handover后的ID/RAT/profile |
| 来电 CMD / 状态 | `8901` 的字段、长度和唯一 tag 验证；独立 pending ID → 完整 ECPI0/132/6/133 的 MT reducer / iOS observer；未发布 CallList |
| END 原因 metadata | 成功完整解析、被当前 MO/MT 身份接受的 `133/mode20/10a=16` → 内部 Apple reason145；无cause/其它cause未知，不从无ID0910补归属；只编译，未发布 END |
| 来电许可 RSP | `8902` 的正确 approve/call-id 字段；显式调用才构造，保留原 txid；按已核实 RIL slot 转换生成 RSP mask |
| 物理 transport | 独立 call inbox / CMD owner，复用原有唯一 MD tick writer；SMS ACK 保持优先 |
| 超时 / 重复 / 失败 | 无自动重发；重复 outstanding CMD 去重；内容变化及未知退休规则隔离 calls；不清空 mutation owner |
| 默认启动 | **不发布 QMI VOICE09、不主动注册 CALL CMD/IND、不发通话请求、不自动 approve，无强制开启 bootarg**；注册只由绑定后的有效 VOICE03 触发 |

`X9Calls.inc` 当前接明确的 VOICE44 路由绑定、VOICE03 已核实13=0/14=0或1注册与handover订阅，以及 VOICE21 单普通 MO 挂断；其它 VOICE 请求仍拒绝。
绑定成功只代表本地路由保存，不代表 MD 注册、bootstrap 或音频已可用；VOICE09 仍不发布。
`x9cw_action()` 仅生成报文，生产 VOICE21 调用它后还必须过 MD 锁内 / writer 的 target guard；VOICE22 接听尚未准入，CallList 尚未发布。
`x9cw_dial()` 同样仅编码：必须显式提供已验证的卡2 profile、无现存通话；当前没有 profile provider。
原 transport 已允许这一严格 DIAL 子集；上一版宿主测试闭合发送/CNF。新增异步 join / 空快照 / 发送前 guard 目前只有编译结果，没有执行验证，也没有 iOS 自动拨号调用者。

## 原件证据（2026-11-15）

### 用户态传输、加载及 Android profile：生产接入边界

现有用户态→内核传输已经存在，不能把“没有自定义call API/生产caller”写成“没有传输”。
`libATCommandStudioDynamic.dylib` 原export trie明确导出
`qmi::Client::createWithQueue19ecd6b20/19ece432c`、`Client::send19ecd2294`及
`ClientRouter::addClient19ecea700/get19ecd5340`。前者使用原
`com.apple.commcenter.atcs.xpc` broker，真实State强owner及队列，而非额外raw QMUX reader。
`Client::send→State::send19ecd2c7c`序列化真实QMIServiceMsg，以原`send_qmi_message`/
`send_timeout_ms`字段进入现有异步路径；共享QMux校验service/cid后保存owned SerializedMessage。
Router的实际属性名为`sim`，same-key分支**替换**强pair并释放旧owner，缺key的get抛
`out_of_range`，不能隐式取默认SIM。VOICE SendProxy借Client对象地址，须保证借用期间地址稳定；
内部另有State/capture所有权，不把该要求扩大成所有callback都继续借Client对象。

原ABP type0 selectors2/write、3/read是已有内核入口：复制输入→`bb_host_write`（bb锁内）→
`cbq_from_host`→`bb_to_modem/bb_queue_tx`→现有bridge poll→`x9bb_modem_send`→
`x9q_send`→`x9c_request`。channel open对第二owner返回`kIOReturnExclusiveAccess`，
新provider应复用原broker/逻辑client，不能与原读者竞争。319已有qmux1/2打开、异步selector2及
WMS4c两个client的运行证据，但镜像SHA未对齐，**不是新typed通话flow已执行**；本轮未逐条闭合
ATCS至libIOABP的所有virtual边，也没有新增用户态callable API。09仍未公告，20无生产准入。
12处当前root原cache小切片及窄export读取在`x9/out/call-native-ingress/`；独立peer确认21条
所选export及5处owner/key原字节，不代表整个库只有21个export。

当前CommCenter launchd cache/plist没有call注入环境。原dyld真实链是AMFI op5a→Security flags→
`DYLD_INSERT_LIBRARIES` owner→getLoader→initializer list→imageBase+offset→constructor BLR。
现`sweet_amfi`已按真实5a ABI返回ff，无依据再放宽`CS_RESTRICT`；16处当前root原字节相符。
原CommandDriversFactory仅按vendor选固定M/K库并初始化真实command-driver owner，
不是通用插件发现、也不是单独IMS CallFactory插入口。**完整功能provider具备后**才接正常
builder、签名后manifest、PID1原子发布与精确CommCenter exec注入；不先添加空loader。
原loading及独立ATCS review在`x9/out/call-runtime-loading/`，实际已在主checkout，无需重搬。

Android原`RFX_MSG_REQUEST_IMS_DIAL/cf33`→`requestDial381410`明确
emergency=false/video=false/IMS=true，六TLV为`8100/string、108/u8、101/102/103/104/u32`。
number producer可证明101=2、ordinary voice branch证明102=1、normal IMS证明103=0；
**108和104不能由“普通Apple Call”默认归零**。108来自原owned Dial+8，Rtm非ECC且CLIR0时
可用slot key124策略改写；Rmm key125精确对比字符串`true`，不是运营商标识。Rtm构造的临时
copied request并非实际send对象，requestToMcl仍接原引用，其owned CLIR已被策略改写。
104每次依赖MCL72 preferredRat/31 voice RAT，ordinary cf33不执行已发现的ECC reset caller。
完整配置/两privacy byte/RTT/relay语义尚未映射，不能静默丢弃、造ready或借实包充当生产profile。
当前encoder在该窄链无已证错误；字段来源、原日志及精确owned copy在
`x9/out/call-md-profile-owner/`。本节没有生产行为修改、编译、测试、设备操作或最终镜像。

### NAS03本地初始化订阅：真实host queue receipt

本项与VOICE03/IMSA的网络注册不同。Apple `registerEvents_sync1f1ab620c` 以
tag19/u8请求SigInfoEvents，ConfigSigInfo2/message6c是独立threshold配置。生产NAS03
仍保留既有tag13/18/19宽度1、bool0/1校验；unknown tag按原accepted-but-unserved
语义处理，不合成对应indication。以前直接改flags后用void-send，队列满/OOM时可能
没有注册响应却已开始后台查询；新有界owner将这两者以实际queue receipt连接。

每个原请求保存slot、local generation、真实QMUX generation、SIM、txn、validated bytes
和首结果；最多32个pending，同client按单调order FIFO。只有实际BB队列接受原response
才将mask/values应用到CURRENT flags、重启采样cycle；未触及字段不被入队时旧snapshot覆盖。
last-completed cache独立于pending池，exact重复只回复原结果，不再apply/next；changed或
malformed同txn由原owner保留首结果，不给第二BAD/ARG或发MD。合法新txn成功admission
才清cache；ARG/BUSY和order达到UINT64_MAX不抹cache。OOM/full只保留host回复重试，
没有物理命令、额外网络ready赋值或猜测timeout。

DEL/localgen或真实QMUXgen变化退休旧host owner；pending时不同SIM绑定BUSY，同SIM不阻，
成功换SIM清cache。关闭订阅的receipt以后，已在途background NAS24/4d/4f自己的IND
以当前localgen/SIM/对应flag再gate，物理查询仍drain；foreground和其它合法subscriber的
共享signal样本fanout不受这条self gate影响。原pool-full BUSY/ARG仍是普通非receipt
错误边界，不将此次修复泛称所有QMI回复可靠。独立peer发现的相邻6c旧隐式signal-enable
已删除；6c保留原ACK，但不写signal/next，不改变NAS03订阅、也不重启采样周期。
实际Apple `setThresholds_sync1f1ab589c` 在 `58d0` 构造6c，`58dc/58e8` 调
prepareThresholds/populateThresholds，`58f4` 另设tag a0 LinkQualityIndicator；
NAS03 `6200/620c/6210` 构造03并写独立tag19，`6238` 是IndicationRegister SendProxy。
因此6c是threshold/delta/LQI配置而不是订阅。本桥仍ACK但未应用这些配置，日志明确该限制；
不会把删除隐式enable说成完整阈值实现。具体生产提交/固定构建结果见NOTES。

`coachz_baseband_poll`在原唯一线程中运行send/del后再进入`x9q_poll→x9r_poll`；
BB通知只完成host read/Mach通知，host后续write和release只排队到下一owner poll，
不会在receipt与flags commit之间直接重入同owner。这是实际callsite串行证据，非
设备时序测试。严格host及真实recipe对象只编译，人工向量未执行；不代表iOS已经
学到真实网络注册，更不代表typed IMS、VOICE09、CallList或声音已可用。

### VOICE03 与 Android 注册 owner

Apple `sendVSIndRegisterReq_sync` 的实际生产分支 `1f1ad092c..0a80` 构造
**message03 / tag13 / u8=0** (`VoiceIndRegCallEventsNotify`，writer `1f192a0c4`)；
另一分支 `1f1ad0bec..0d44` 构造 **message03 / tag14 / u8=1**
(`HandoverEventEnable`，writer `1f192a0e0`)。不能把后者叫成“开启 CallStatus46”或 voice-ready。
两个 response consumer `1f1ad0424..04dc` / `1f1ad0694..0748` 仅检查 ResponseBase error，
成功不消费任何 payload，所以只回原 transaction 的 Result，不造 ready/ID TLV。
`SendProxy1f1ad03ec` 的 host guard 为 25000 ms。

Android `RmmCallControlUrcHandler37c42c..37c470` 注册20项 URC 和 CMD8901；
rodata `133a44` 的前两项为 `4901/4902`，其余未消费的 URC 不照抄全部注册。
`RfxBaseHandler1e2810..2828` 先将 handler slot 转为低位 SIM mask 再调用
`mipc_msg_register_ind_api`；`1e2bf0..2c00` 的 CMD 注册不带 SIM 参数。
匹配 `libmipc14228..14248` 写 **0307 / 原 SIM mask / tag100=u16 indication ID**；
`14510..1452c` 写 **030b / maskff / tag100=u16 command ID**。
MD `91335654..913358ea` / `913358ea..91335a34` 写 `0308/030c`：
普通单 ID 成功只有 result0/u32，IND 单 ID 失败还可带 tag100/u8=0；
不能把 timeout、局部结果或额外 failure TLV 当成功。

生产流程（没有开机探针或设备操作）：

1. 独立 VOICE44 显式选卡；44 本身不发 MD 注册。
2. 有效 VOICE03 请求入每client两个有界waiter；13=0与14=0/1独立transaction可并发，
   重叠options返回BUSY。13=1仍unsupported，拒绝未知、重复、错误长度；不把14的订阅当CallList开关。
3. 全局一次 `030b{100/u16=8901}`，原 token 完整成功才进入逐卡链。
4. 各卡独立 `0307{100/u16=4901}` → 同 token0308 → `0307{100/u16=4902}` → 同 token0308。
5. 一次 owned `0909{}` / 完整成功090a 初始快照。非空不是 idle；SIM2 的空基线还必须通过
   MD revision/count guard。`registered`、`baseline_known` 是不同状态，均不等于 IMS/profile/audio-ready。
6. 所有步骤完成后回各原 VOICE03 Result，只应用该请求present的options。14=1的待确认真实
   handover事件在Result之后按序发布54；不coalesce START/COMPLETE。**不从注册 CNF 造 CallList46、ID 或音频能力。**

多个 client 共享物理逐卡注册但不共享 host generation；换卡遇未答 waiter 返回 BUSY，删 client 清请求，
原 MD token 继续消费，不回答复用槽。无 waiter 时不自动启动下一步骤。host 总计25s和每物理步骤25s均有界；
物理 timeout/transport loss 为本 boot terminal hold，迟到成功只 drain，不 advance/复活，不重发。
SMS ACK 优先与唯一物理 writer 不变；8901 仅保留原 incoming owner，**不自动 approve/answer**。
已有泛用 MIPC 白名单未整体封闭，只对新增 CALL 注册形状施加严格检查。

这闭合了已核实注册子集及handover通知的生产调用与响应，不是完整 Apple bootstrap：CallList46发布、
handover后的ID/RAT/profile迁移、运行DIAL profile/admission和voice音频仍缺，VOICE09仍关闭。人工源码向量只编译，未执行。

#### 注册Result / retained54的原子host queue准入

源码review确认旧`x9c_reg_answer`使用`x9q_packet::send`，底层
`coachz_baseband_qrtr`只返回“port recognized”，不返回`bb_push`的实际queue/OOM结果。
因此队列满时仍会设置registered/handover并擦除尚未入队的START/COMPLETE；
Result与各事件分开入队也可能只有前半批成功。此前getter与host入队之间另有CALL poison窗口。
这些是可达源码缺口，不是新增设备复现。

生产`4e30917`（原`4b2619b`）仅修注册publication：原Result03和启用期间保留的最多8条54在锁外预分配，
`x9_md_mipc_call_publish(floor,callback,context)`持同一MD锁检查online/CALL poison/RX floor，
受信callback再检查原client/generation、control-ready及原25s deadline，
以MD→bb mutex顺序覆盖**整批真实host queue准入**。bb锁内验证非release_pending的真实VOICE
client、service/node/route及整批容量，只header/copy/入队，不malloc、不MD重入；两mutex仍可等待。
失败不转任何buffer所有权，锁外擦除释放；queue/OOM只保留本地waiter/events重试到原deadline，
不重发MD请求。只有整批成功才grant并清buffer，Result先于START/COMPLETE。
成功后MD再次offline由原poll撤host订阅，不把这笔历史成功当永久voice-ready。

peer-review再发现callback检查后可能等待bb锁跨deadline，`051bc35`将原64-bit deadline传入
实际bb helper，**获得bb锁后、queue mutation前**再用`clock_get_uptime→ns/1000000`检查；
不用32-bit诊断时钟，也不是在callback锁前/后补一次getter。
新增共7项人工源码向量只编译未执行；初版host/header及真实recipe两对象均exit0，
锁内deadline修复严格host编译exit0，最新统一kernel结果见NOTES。
此项当时不改已经启用后的普通54发送路径；该路径的queue-full保留已在下节单独补入，
不发CallList/VOICE09，不改变SMS/CALL mutation、真实ID/profile、注册CNF或MD物理writer。
统一kernel结果见NOTES；仍无设备注册/通话/最终镜像验收。

#### 已注册后的live54保序publication

旧live路径同样只调用`coachz_baseband_qrtr`，消费CALL inbox后没有保留队列失败的事件。
现在每个已绑定client独立保留最多8条真实54状态及最后RX sequence，按原SIM/generation/floor
接收，**不合并重复START/COMPLETE**。poll锁外预分配全部QMUX buffer，再复用native
`x9_md_mipc_call_publish`持MD锁复核online/CALL poison/floor，受信callback复核当前control、
subscription/generation/SIM，以MD→bb锁顺序调用与注册共用的整批publication helper。
真实VOICE service/node/route、non-release_pending及完整queue容量都在bb锁内检查。
成功才清本地events；queue-full或任一分配失败不转半批ownership，擦除释放临时buffer，
保留原序等待下一poll，只重试本地入队，**没有新MD请求或网络重发**。

live事件不是host请求，因此不虚构25s deadline；注册Result03仍在获得bb锁后复核原64-bit
deadline。收到第9条/非单调sequence使该subscription的publication sticky-blocked，停止继续
发部分状态，不影响其它client/SMS；已接受的enable waiter也不能擦掉此owner。只有成功发布的
显式14=0、delete或不同SIM/generation的rebind可退休本地batch；新的14=1取新cursor，
不重放丢失区间。transport loss/poison撤订阅，allocator之后的poison由锁内fence拒绝。

新增8项人工源码向量仅严格C++17 `-c`；shared header C11 strict syntax和真实recipe的
两对象编译均exit0，**没有执行向量/套件**。这仅修host queue receipt/有序保留，不能替代
iOS实际消费、handover后身份迁移或声音验收；VOICE09仍关闭，未生成最终boot.img。
编译原日志在`x9/out/handover-live-compile/`，固定kernel结果见NOTES。

### SRVCC / Apple handover54（2026-11-15，源码接入，未设备验收）

完整证据链来自本机匹配原件，不借标准QMI枚举猜消息：

- MD `mipc_call_event_ind913258e6..91325a22` EVENT3分支`91325956..62`：
  `4902 {100/u32=3, 115/u32=state}`，state源为u8，完整32B。
- RIL `handleCallEventIndicationMessage37cfd4..37d020` 读115/u32，仅0..3交
  `notifySrvccState380840`，数值不变。Android16 radio HAL `SrvccState`定义
  STARTED0/COMPLETED1/FAILED2/CANCELED3，是对应接口契约，**不是本机真实SRVCC捕获**。
- RIL `obtainMipcUrc19f898..19f8a4` 调用与CMD相同的`mipcSlotIdChangeToSlotId`；
  codec只接本机低route1/2，保留高位但不从高位选SIM，不复用IMS/NAS绑定。
- Apple20H392 handler lambda19 `1f1ad3864/38a0`明确message54，`38ec..38f0`指向
  consumer`1f1ac317c`；`1f1ac325c`读取tag1，`HandoverState parseV192ad18`读取u8。
  `asString193a040`为START1/FAIL2/COMPLETE3/CANCEL4，10是LIKELY但本原件SRVCC0..3
  没有对应来源，不合成LIKELY。映射 `{0,1,2,3}→{1,3,2,4}`。

`x9_call_handover.h`只接受完整4902、唯一100/115、两个u32、32B及合法state；
非EVENT3不生成handover通知。iOS完整11B indication为
`04 00 00 54 00 04 00 01 01 00 <state>`，不写Result、默认ID/RAT/profile。

接受enable时从MD锁内取RX cursor；ordered drain另返回RX sequence，不混MD transaction/
CMD reply token。只发布同client generation、同独立VOICE SIM、订阅成功且sequence在floor之后的事件；
初始化期间最多保存8个真实状态，先Result再有序flush。已成功enable的重复注册不重放；13-only不能清空14，
14=0成功后停止本地发布。timeout/delete/换卡/overflow不flush，未注册/旧generation/订阅前queued事件不发布。
通话inbox隔离或transport失联立即终止host等待并清订阅；物理token仍继续drain，不把generic MIPC在线当CALL可用。

MD收到有效SIM2 SRVCC时在锁内、writer选择之前invalidate旧空基线并block MO/MT，
防止queued旧DIAL/END跨交接发送；SIM1有效事件不借来block SIM2。畸形事件隔离calls，不影响SMS。
RIL `RtmCallController::handleSrvcc26a2a0..`完成时清IMS列表，证明不能继续用交接前ID/profile。
**当前没有实现交接后的身份重建/CS动作策略，不能宣称handover后仍可控制通话。**

私有派生材料：`x9/out/call-status-analysis/{md-call-event,ril-event-dispatch,ril-handover,
ril-srvcc-notify,ril-srvcc-controller,ril-urc-route,ios-handover,ios-ho-dispatch}.asm`和
`android16-radio-types.hal`；真实10086材料没有SRVCC完成，不能算handover验收。
新增8项人工向量只编译，未运行；旧overflow向量改用合法MD RING事件6，以免畸形4901在首帧就隔离而掩盖FIFO容量。

#### handover 标记与真实 Call ID 迁移：独立 typed 消费者（静态闭合）

本轮从当前根盘 CommCenter 和正确的 `libCommCenterMCommandDrivers.dylib` 恢复两条独立链，
不再将它们当作七byte CallList 的 offset6。后者仍未闭合，不因此生成 CallList。

- `handleVoiceAllCallStatusIndication1f1ac3644` 的 `3f80` 明确取 **tag1f**，
  `3fac→192ab2c/192aaa0` 解析 `IsHandoverCallList`：count/u8，每项两个u8。
  `4014..4030` 以 `ldrh >= 0x100` 筛选，即 **item[1]非零**，不是bit0；以item[0]为键
  保存bool，并在`4500..453c/493c..4954`按真实CallList ID查回该标记。
- 相邻 **tag20** 是 `ParentCallInfo`，不是上述标记表。`4124→192abfc` 解析，
  `41e0/4234/4290..4294` 证明item[1]作旧ID键、item[0]作新ID值、步长3B；
  第三个字节语义未恢复，不猜。`42a8..42bc` 调用delegate virtual+148，
  `1f1b2098c` 转至内层virtual+1e8；root表 `10158ac10→1006965c4`。
  后者 `100ff3dc8` 按真实 **Call+140==SimSlot** 选择强Call owners（bool0绕过额外+a0过滤），
  `6966d0`仅以 **ldrb Call+c4** 查旧ID，命中后`6967a4→101049ad4`，
  `101049ae8`实际 **str w1,[Call+c4]** 写新u32 ID，再走原通知链；缺键保留原ID。
  这是原件的身份迁移消费者，不是当前MTK桥已经产生了有资格的ParentCallInfo。
- SRVCC CallInfo由`1f1ac48c8`真copy构造并strong-own driver后异步发布；
  `1f1ad98a0`调用driver+78的delegate virtual+a0，真实wrapper addresspoint
  `20dd487a8` 的+a0=`1f1b20704`，转内层virtual+d8；
  root表`10158ab00→1006949ec`是`handleHandoverCallInfo`。
  `MCommandDriversFactory::createCallCommandDriver1f1b1eba8`保存输入强delegate pair至wrapper+8/+10，
  `EurekaCallCommandDriver` ctor`1f1abd468..478`安装wrapper强pair至driver+78/+80。
  这是QMI自己的wrapper，**不是此前追到ID更新槽的IMS三层adapter**。
- root `1006949ec`按CallInfo+d8选driver并比较controller+b8；有既有Calls的对应分支
  调`10068d734`先查真实UUID owner，driver virtual+128或无UUID owner触发整数ID fallback。
  `10100db28`遍历controller+78的强Call列表，准确入选谓词为
  **Call+c4==CallInfo+5c && w2!=0 && Call+2f0!=1**；本调用w2=1。
  2f0语义未解，不命名为RAT/结束/IMS标志；w2=0跳过全部，不是取消附加过滤。
  随后的`10100a698(controller,&sharedCall,0)`处理原typed Call，**不是拿CallInfo更新ID**。
  无owner/no-calls/wrong-driver有清理或拒绝分支，不凭handover通知创建假Call。
  VoiceHandoverEvent走QMI wrapper+98=`1f1b206ec`→root+d0=`1006945b8`，与CallInfo分开。

root当前Cryptex cache的14处有界原代码/表读取均与提取原件逐字一致；仅资格原件，
不是新运行流量或构建产物检查。独立peer复读上述分支、四处8B表和实际ID setter。
材料在`x9/out/call-handover-typed-owner/{handover-contract,current-cache-qualification}.json`；
peer在`x9/out/call-md-voice-owner/handover-peer-contract.json`。
仍缺MD交接后的同epoch身份producer、第三字节资格和真实CS/profile/action/media迁移，
不能直接把MD4902翻成该ID表；VOICE09继续关闭，没有handover功能验收。

### CallList 七byte原件核对（进行中，不生成默认字段）

`CallListItem parseV1929b2c`逐byte读七字段，consumer`1f1ac4330..`确认offset0真实ID、
1 CallEventStatus、2 Call_Type、3 CallDirection、4 CallTechnology；5已证明为multiparty，
6在当前大consumer未直接消费。匹配`libCommCenterBase`的
`callDirectionToString1f03908b4`为MO1/MT2；`callTechnologyToString1f0392238`表
NoService0/CDMA1/GSM2/UMTS3/LTE4/TDSCDMA5/Unknown6/UnknownTech2 7。
`EurekaGSMCallFormatter::isTechnology1f199fad8`只接2/3/5，不接LTE4；不能为“LTE通话”随便挑formatter。
offset5的consumer `1f1ac4df0..4dfc`写tree node+0x7d，而`1f1ac4d28`证明CallInfo为node+0x28，
所以实际是CallInfo+0x55（不是CallInfo+0x7d）。Base `CallInfo::toString1f039177c`字符串
`Is in Multi-Pary call: `，`1f039178c`读取+0x55；`getLegacyFlags1f0390bec`也读+0x55并映射bit30。
Call_Type细分与offset6语义仍有未闭合项，**不填默认0**；RemoteNumberListItem presentation的已证映射见下。
`1929bfc`是CallConfigItem writer，不能拿其string长度解释CallList第七byte。
CallList tag1的`192a74c→1929b2c`为count/u8后7B逐字节条目；END tag14 `192aa10`为
count/u8后每项`{id/u8,reason/u16LE}`，wire步长3（内存padding不是wire）。
RemoteNumberListItem byte1经`1f1ac5c60..5c70`映射1→internal1、2→internal2、其它→0；
Base `remoteNumberAvailabilityToString1f03906c0`直接0基表`20dad4d50`为Available/Restricted/Unavailable，
因此wire0/1/2对应这三项，未知拒绝不猜。MD号码presentation真实来源为8114/detail_info[0]，
RIL `3854ac..bc→3855e8→388380`，不是105/107的整word。

`x9cw_number`现有128B含NUL的有界owner：progress8104与current8111分别保存原号码、
length（不含NUL）及8106/8114的原始presentation byte。先验证全帧再提交out，不持有会被重用的RX指针；
生产owned-current路径随原快照复制，stale/error快照不替换。只标记实际成功解析的metadata为valid，
不能把zero-init对象的0当Available。`x9cw_number_presentation()`明确native0→Available0、
1→Restricted1、2/3/4→Unavailable2；未知返回失败且不改输出，**不沿用RIL的未知→Available fallback**。
未来publisher仍须只在有效owner/正确epoch下使用，Restricted/Unavailable不得发布原value[]；
尚未实现号码格式/TOA转换、最终显示号码合并或CallList46生产编码/发布；progress原身份owner见下。
没有新增查询/服务公告/默认字段，不能仅empty encoder冒充完成。新增两个人工owner向量仅编译，未执行。

offset6追加原件排除：`192a770→192b150/192b194`只是接收vector resize/append；
`192a794→1929b2c`必将wire+6覆盖到item+6（`1929be0/be4`），默认构造不能作为来源。
相邻`1929bfc`为CallConfigItem，`1929c3c..54`把C++ string长度写wire+6后复制wire+7，
是变长结构而非7B CallList serializer。当前匹配host原件仅有CallList parse/resize/copy，
未发现其writer；QMIParserDynamic已有metadata也无vs/ALS定义。ALS只是未证假设，不按标准字段名填0。

#### 已接受progress的epoch raw identity owner

完整4901 parser现在同时有界保存408B detail和最多512B含NUL的PAU，仍先校验全帧再改out。
原MO/MT reducer此前仅保存ID/state/cause，排空raw inbox后号码来源会消失；现在仅在原
identity/route/state/epoch/revision准入成功的commit处复制number/presentation/TOA/detail/PAU。
MT新CMD清旧owner，foreign/late/rejected/revision失败不改；legacy valid0不授parsed provenance，
current仍是独立owned-query快照，**不倒填MO/MT**。这是最后accepted整帧的raw身份，不是显示号码。
来源commit`c7008018`，原集成`3143ca5b`，保留main后为`bf96e0dc`；3项新增人工向量、C11 header、host C++17 `-c`、
两真实recipe对象均编译exit0，未执行。精确切片/编译日志原在identity树
`x9/out/call-identity-owner-analysis/`，收尾迁回主checkout同名out。

RIL `26931c/269c44→26e000→270120`显示合并还有真实slot状态输入：178控制既有号码/ECPI
更新，179影响MO是否解析PAU，180影响sip/tel选择；不是固定PAU>ECPI。PAU parser的真实
delimiter字符串`[;@,]+`是strtok_r的**字符集合而非regex**，anonymous.invalid还有例外，
compareLoosely匹配时保留旧号码。真实writer已闭合为`loadConfiguration21f020`通过
`libcarrierconfig.so`的`getKeyCount/getValuesByMccMnc`按slot写setValue，有效缓存优先、
default仅fallback；保存的同机1761 maps证明库实际映射，但仓内缺库实体及三键live值。
不能给true/false默认值，
不能把raw number/PAU直接公开；Restricted/Unavailable/未知presentation仍禁止泄露原号码。
IMS最终producer的TOA为合并号码是否含`+`的0/1、presentation为空1/非空0；current/CS
才消费native TOA与8114/detail[0]。两套producer不能混用。此项不发CallList/VOICE09，
不增加query或移植未核实的PAU归一化。

### END cause16 → Apple reason145（内部metadata，未发布）

Apple END tag14 consumer `1f1ac50a4`读u16，`1f1ac50bc`交
`convertCallEndReasonToString1f19fd3c8`，`1f1ac5170`交
`convertCallEndBBCodeToCSIError1f19fd830`。**原始数值16在Apple空间是Unknown / CSI1**，
不能把MD10a直接copy为Apple reason。Apple145/0x91明确名为
`QMI_FAILURE_CAUSE_NORMAL_CALL_CLEARING`；CSI跳表entry34的目标`1f19fd8e4`返回0。
Apple0是OFFLINE，不能作为无cause默认发布。

matching MD原因空间及完整源到10a链已闭合：

1. IMS disconnect producer `910d30e6` 的`910d3104..3110`与progress producer
   `910d3366` 的133分支`910d3398..33a4`均调用`imc_map_last_cause_to_ext_report911006ec`。
   CEER request `910f6678`的`910f6696..66a4`调用**同一mapper**；formatter
   `9143ec76..92`使用table`929950d8`，其中`[16]→9244a76c="CM_NORMAL_CALL_CLR"`。
2. `910d7f58..7f80`把映射输出放ECPI info+57/presence1、+58/u16cause；
   VDM update`921ecd2a`保存ctx+f/presence、+212/cause，fill`921ecb6c`原值拷回。
   无cause路径presence0；内部备用值31**不能冒作真实cause发布**。
3. END route`921e9758→921e9604`经encoder`9143dc36`设置msgtype133；
   common`9143da74`由presence决定是否写末尾cause字段。L5 ECPI parser
   `91392d86..92db6`保留presence/u16cause，send`91392ae0..92aea`把session+4/+6
   拷至IND+231/+232，serializer`9132587e..25894`仅presence==1才LHU零扩展写10a/u32。

真实 retained radio日志`x9/out/android-incoming-20261115-192820/retained-radio-logcat.txt`
2977..2979行，19:27:31.399的完整0910同时含code16和`CM_NORMAL_CALL_CLR`；
matching RIL`385848..5894`保留code和字符串。0910没有通话ID，**只佐证原因语义，不倒填4901的ID、
不把前一通话的cause补给后一通话**。已有真实4901日志只保留486B，声明512/520/528/536B；
仍无完整设备END frame，不补零或截断解析。

共享progress仅在完整frame及所有TLV校验通过后置`valid=1`。手工/zero-init对象保持既有reducer准入语义，
但不能授予END原因provenance。MO/MT reducer仅在原身份/路由/方向/状态/revision检查全部成功后保存
`end_reason{known,apple}`：仅完整`133/mode20/has_cause/cause16`known145；其它或缺cause未知。
拒绝事件、late133、revision overflow不会更新原因；首次unknown END也不能被late16补成known。
NEW130与新的MT CMD begin清前一epoch的metadata。没有改注册、MT approval/answer、运行profile、
CallList/END编码或VOICE09；本节不是END发布完成。

私有窄反汇编现复制到`x9/out/call-end-reason-analysis/`（含`ios-end-reason-*`、
`md-ims-*-cause-source.asm`、`md-vdm-*`、`md-end-*`、`md-ceer-*`、`ril-last-cause-*`）；
人工provenance/MO/MT三项向量、C11 header及真实recipe两对象仅编译，未运行套件或设备验收。

### CALL_RAT 不等于真实 voice access RAT

MD `913a29ae/b2→l5ucc_get_rat_from_call_mode913a0756`将mode20..39分类为2，
0..19和40..50分类为1，其余0，写session+39f；current112来自record+99。
这是CLCC/CLCCS默认分类，不是实时LTE/NR/WFC证明。
同一次真实10086日志`android-calls-20261115-183941/live-logcat.txt`的81634/81635行
（18:42:37.767）与92175..92178行（18:42:41.304/305）明确slot1 voice RAT20/NR_SA、
`NW_VoNR_Supported_2=1`。所以不能把current112=2无条件编码为AppleTechnology LTE4。
AppleTechnology原件枚举没有NR，不以Unknown占位绕过真实来源。
RIL真实voice注册查询是`requestVoiceRegistrationState30f8f0→042f→8100/10B`，
networkType来自value+1/u16LE，再经convertCSNetworkType进入ServiceState.voiceRat；
独立于PS/data RAT和IMSA注册快照。此处只记录原件路径，未新增query或准入默认值。

### Apple 普通号码类型不证明 IMS command context（原件架构纠错）

匹配Base `CallInfo::toString1f03911dc..11f4`输出`Call Sub Type:`并读CallInfo+44/u8，
`asString(PhoneCallSubType)1f0390818`明确Telephony1/Wifi2/VoLTE4/VoNR8/SOSCall16；
`isCallSubTypeCellularIMS1f039230c`只接4/8。这是独立于CallInfo+40/PhoneNumberType的维度。
所以Apple普通Call_Type0可以是普通号码，但本身不能选择Android`IMS_DIAL/cf33`。

完整`EurekaCallCommandDriver::sendDialReq1f1ac1f8c`仅按号码、普通/紧急类型、CLIR、TTY、
slot(+d8)构造VOICE20的tag1/10/11（紧急另14）、60；未直接读取+44或透传明确IMS context。
不能把该wire中的tag10=0或IMSA registered反推出subtype4/8、选中IMS driver或真实IMS client链。

根盘matching CommCenter的`CallStatusDelegate::dial101017a94..101018318`保留该subtype，
`101018060/8078..88`取selected driver并经vtable+18 dispatch；
`CallCommandDriverType`实际Baseband0/IMS1。真实chained pointers解码表明
typeinfo1015fa830的字符串为`20IMSCallCommandDriver`（1011e0d8b），
secondary address point1015fa390的slot+18指向100dcdbc8的this-=8 thunk，再进100dcd7b0。
其subscriber/SIM call-control分支在100dcdcdc读取+44并交call-control，
后续100dcd114/async100dcf6e8不是Eureka sendDialReq。

CommCenter包装100177b88导入的`IMSStartCall`，ordinal58实际provider为
`/System/Library/PrivateFrameworks/IPTelephony.framework/Support/libIPTelephony.dylib`。
`IMSStartCall1b7d69208`构造XPC的`request=kCallStart`、`kCallDialedNumber`、`kCallOptions`，
经1b7d6933c→`XpcConnectionClient::sendXpcMessage1b7ccff18`发送；实际client/connection链为空则返回0。
**selected IMS driver、subtype4/8、live IMS transport及caller options才是已证的上游维度**；
当前普通VOICE20内核入口没有这些来源；以下闭合原件XPC接收端至AP socket，
但Apple IMS provider到MTK modem IMS的桥及语音能力options仍未实现。
此处是架构边界纠错，不注入host代码、不新建QMI默认字段、不开放VOICE09。

`IMSClientCreate1b7d67e4c..e68`与server instance block`1b7b816b4..81730`使用同一
`com.apple.ipTelephony` endpoint。根盘实际`com.apple.CommCenter.plist`的MachServices注册该名，
ProgramArguments指向CommCenter；不是仅凭字符串猜服务。
`XpcDaemonClient::setHandlers1b7b62314..354`实际注册`kCallStart`到block`1b7b635ec`，
读取`kCallDialedNumber`及`kCallOptions`。普通分支`1b7b63bd8..bf4`的virtual+80
进入`ims::Call::dial1b7c41468`，async block`1b7c41878/418cc`再调用
`IMSCallManager::initializeNonEmergencyCall1b7b32708`；不是先前同名startCall wrapper。

实际普通呼叫链为`initializeNonEmergencyCall1b7b33110 → IMSCallInfo::startCall/continueCall
1b7b28438/1b7b28de4 → BambiClient::initializeCallToRemoteUri/createNewCall1b7da99e0
→ BambiCall::initializeMobileOriginated1b7daa180`。`SipStack+af8` handler virtual+60
进入`internalStart1b7d7948c → initializeMediaForMOSession1b7bc3ed0`；真实异步media创建回调
`BambiClient::handleMediaSessionCreatedStatus1b7dad73c → setMediaSessionCreated1b7bc629c`
才调用`SipSession::sendInvite`。

INVITE继续经`sendRequest1b7b94ecc/1b7b91a48 → SipDialog::sendRequest/sendRequestInFork
1b7d0b438 → internalSendRequest1b7d0dea4 → SipTransactionLayer::sendRequest1b7ce3ed0
→ SipTransportLayer`。原件rebases已定名IPSec address point`1d90a4d20`：
slot+38/sendMessage、+78/sendMessageViaBestTransport、+80/SipIPSecTransportGroup::sendData。
`1b7d77aac`实际virtual+80到`1b7ac52a4`；`1b7ac5700`的socket virtual+28
经UDP secondary`1d90b7388`的slot+28 thunk`1b7c3c2d8(this-=48)`到
`sendOverSocket1b7c3c2c0 → ImsUdpSocket::send1b7c3be68 → 1b7c3bf18`。
最后stub`1b7e0f0b8`由该dylib的indirect-symbols表证明为`_sendto`，不是nearest-symbol注释。

因此该原件的`IMSStartCall`主信令是一套**AP SIP流程**，不是普通Eureka VOICE20的上游。
上述静态链不证明本机iOS已选择IPSec、成功IMS注册/拨号或建立media；IMSA registered、NAS/UAC
辅助请求不能替代真实client、SIP registration及call-state readiness。
私有可复读材料在主checkout`x9/out/dial-profile-remaining-analysis/`，包括
`ios-ims-callstart-handler.asm`、`ios-ims-sip-invite.asm`、`ios-ims-transport-vtables.txt`、
`ios-ims-udp-socket-send.asm`与`ios-iptelephony-indirect.txt`；未修改原件或操作设备。

#### IMS provider与client-ready的实际构造边界

实际CommCenter派生`CCIMSClientFactory` typeinfo`101587d10`，address point`101587cf0`：
+10→createCallClient`10065c82c`，+18→createSMSClient`10065c9c8`。
其`10065c6f8`经Registry/SystemDeterminationManagerInterface virtual+18→
`SystemDeterminationManager::getIMSClientRef1f2e4c210`按**PersonalityID**查controller→virtual+60
获取ims::Client。不能以基类`1f03a8804/80c`返回空对象认定实际provider缺失。
更早`sd::IMSSubscriberFactory` address point`1015494a8`的+38→root`100315998`，
controller`createIMSClient_sync1f2e6803c/1f2e68118`实际调用该槽，随后stub`1010815f8`
→`ims::CreateNewClient1b7c43a00`→`IMSClientManager::createClient1b7c335b0`，对象写controller+d8。

`1f2e68214`注册StackDelegate后、client非空时`1f2e6829c`直接调用
`handleClientReady1f2e68358`；client virtual+30实名setCapability`1b7c3ffec`。
bootstrap`1f2e67398..3a0`注入controller+90的IMSSubscriberEventInterface，ready事件经+90
→`sd::IMSSubscriberEventHandler1007ab880`→PersonalityID/bool map→`1007b8634/1007b1b38`
→`RestModule::setProperty1010830bc`发布**`/cc/props/ims_client_state`**。
这是AP IMS client对象可用，**不是网络IMS注册成功**；handleInitialized`1f2e6b1d0`
只写controller+f8=2，不伪造registered。

`IMSCallCommandDriver::bootstrap100dd55f4/100dd5734..5784`observe同属性，真实callback
`handleIMSClientStateChanged_sync100dcc390`；ready true的`100dcc51c`分支在`100dcc570..588`
调用driver+98 factory的+10，也就是同一CCIMSClientFactory::createCallClient，随后才走上述AP SIP。
已定位CC factory的call/SMS创建槽和更早的subscriber factory+38，但前者仅是legacy接口，
**不是覆盖正常typed dial及全部state callback的完整替换点**，见下节；
**尚缺返回并持续驱动MTK IMS provider的实际接口实现**，包括Personality、capability、delegate、
初始化和网络状态，不能补ready常量/普通VOICE20 TLV代替。本窄段未找到可直接启用的modem-managed
provider，不据此宣称全系统不存在；没有production hook。
私有复读材料主checkout`x9/out/ims-provider-boundary/`的
`ims-controller-createclient.asm`、`ims-controller-ready.asm`、`ims-root-vtables.txt`、
`ims-call-driver-ready-handler.asm`及`root-query.py`。

#### 普通拨出实际typed CallHandle / UUID契约（仅原件静态）

CC factory返回`IMSCallClientInterface`，其legacy +28/dial`100177b88`把
`IMSStartCall`结果转为bool，**不返回native call ID**。正常IMSCallCommandDriver还有另一条：
`100dce268→100dcf174`取driver+328的CallFactoryInterface、virtual+40，实际
CallFactory address point`101532938`+40→`10019c234`。CallInfo里的CFUUID经
`100fb3980`调用CFUUIDCreateString并assign到C++ string，构造
`ims::Call(uuid,真实ims::ClientHandle*)`。普通client由`100dd3498`按CallInfo+0xd8 SimSlot
调用PersonalityIdFromSlotIdEx，再经SystemDeterminationManager取得该Personality的IMS client。
这不是把MD整数ID/token编码成UUID，也不能借NAS/WMS的默认SIM。

model virtual+118→`1007cb578`接收真实shared_ptr<ims::CallHandle>；normal分支dynamic_cast
到ims::Call后在`1007cb970`调用virtual+80/dial，number、CallConfig及CallInfo+57的bool
另有实际上游，不由普通VOICE20/tag10=0推导。更正早报：`1007cbdcc`的+68是
EmergencyCall分支，不是normal Call::dial/refresh错版。root与OS20H392 CommCenter原件相同，
root cryptex/sysos MAIN cache UUID都为`8f970452-c9e3-3e88-b114-feb1864bc9e0`；
同一cache ctor/vtable原字节支持上述消歧，未用不同版本拼ABI。

delegate也不只经legacy Event5：`setDelegatesOnIMSClientHandle100dcb8f0`另取原ims::Client，
virtual+a0/addCallManagerDelegate和+a8/addDefaultCallDelegate绑定IMSCallModel。
ACTIVE callback`1007c4d3c`按uuid string→CFUUIDCreateFromString，经model command delegate
+e0找到原CallInfo，再同步状态/通知。生产provider必须同时保有typed client/call、UUID↔
native epoch/真实ID映射以及delegate生命周期；**单独替换CC factory返回对象不足以修好拨号、
state/END或handover**。不新增hook/ready常量。
私有精确切片在IMS树`x9/out/ims-call-provider-contract/`；这是静态契约，不是设备已选此路径。

CallInfo原`toString`证明`+40=PhoneNumberType`、`+44=PhoneCallSubType`、
`+57=Is Listed As Emergency`，最后bool不是video；`100dd7560`是CallConfig copy ctor，
不是配置生成入口。桥需保留上游真实opaque配置，未知`+d8` optional string不猜用途。
typed delegate按clientId/name保持强shared_ptr，再保存queue/weak_ptr；这里的name是**登记key，
不是call UUID**。`100dcbf14`以C++ string `operator+`生成`ims.call.`+输入clientName，
manager登记为`ims.manager.`+name；emergency另用`ims.call.e911`/`ims.manager.e911`。
`IMSWrapperDelegate::handleActiveEvent1b7c7b640`与disconnect `1b7c7aacc`均迭代该client
record的全部call delegate records，**不是在wrapper map里按event UUID查找**。各record保存queue
及weak_ptr，异步block另深拷贝event UUID，dequeue先weak-lock，再在登记queue（空时MainQueue）
调用真实delegate+20(uuid)。删除按同client/name退休，不能异步缓存裸指针或用跨subscriber全局UUID表。

UUID过滤发生在CommCenter：ACTIVE `1007c4d3c`先调`1007c4490`，将UUID string转为CFUUID，
调用model+80 command delegate的+e0查原CallInfo；lookup失败走`1007c4f28→1007c51a4`，
不执行`1007c503c`的+100同步及`1007c5054`的+40 ACTIVE通知。model+80/+88是强owned
`IMSCallModelCommandDriverDelegate<IMSCallModelCommandDriverDelegateBase>`，不是typed `ims::Call`；
其RTTI `101533100`/名称`1010c3f9b`及`10019ef10/ef68`的shared allocation有原件证据。
真实CallHandle是model::dial的x1 shared pair，由driver同一factory结果传入。

#### typed dial实际输入与异步所有权（2026-11-15，原件静态）

`ims::Call::dial1b7c41468`实参为`(this, calledAddress string const&, CallConfig const&,
additional string const&, bool)`。driver `100dceb44`向model传递原CallInfo、同factory CallHandle、
deep-copied CallConfig及calledAddress；normal model在`1007cb970`保留前两项，additional传空string，
bool传原CallInfo+57。`Call`对象只保存owned UUID/clientId、两CLIR byte、text mode和relay；
**不保存calledAddress/CallConfig，也不包含callback owner**。

`IMSCallOptions`大小`0x13a0`，ordinary typed dial的明确拷贝如下，偏移均为十六进制；
未知字段不据数值猜成video、voice或默认profile：

| 输入 | IMSCallOptions | 拷贝形式 |
| --- | --- | --- |
| CallConfig+0/+20 | +1/+b0 | 各1 byte |
| CallConfig+8 | +148 | owned string |
| CallConfig+40..6f | +b8..e7 | 48 bytes |
| CallConfig+70/+88/+a0/+b8 | +e8/+100/+118/+130 | 各owned string |
| CallConfig+d8 | +168 | optional owned string |
| additional string | +30 | owned string |
| Call+9/+8 | +28/+29 | 两个真实CLIR byte |
| Call+40==2 / Call+44 | +79/+7a | text-mode条件 / relay byte |
| bool实参 | +160 | 原CallInfo+57，不是video |
| Call+10 UUID / Call+28 clientId | +10/+1388 | ImsUuid representation / owned string |

`100dd7560`深拷贝CallConfig时还保留+28 string和+d0 u32；即便本普通dial未读取它们，
桥也不能截断配置或对含string的结构裸`memcpy`。`setCallIdBlocking1b7c42d84`将w2/w1写Call+8/+9，
driver `100dca8bc..940`从真实上游设置选择`(1,1)`、`(0,0)`或`(1,0)`，不能固定CLIR0。

MainQueue block `1b7c41878` capture+20是owned calledAddress，capture+38为独立options copy；
调用后再次stack-copy options，经真实IPTelephonyManager+90直入
`initializeNonEmergencyCall1b7b32708`，**不capture Call/shared delegate，亦不重新走Client虚拟dial**。
该函数以options+1388查`BambiClient::sipStack(clientId)1b7db11e0`；null stack只日志后返回，
stack+98!=2才发disconnect，不能由void dial返回或无callback证明已成功准入。normal分支还需
`makeOutgoingUri1b7cc6ec0`成功、无existing unconfirmed calls，才分配0x280 IMSCallInfo并startCall。
这里的IMSCallInfo+18/+20/+80分别是manager/BambiClient/BambiCall，不是CommCenter CallInfo或typed Call。

当前root原cache export trie确认Call C1/dial、ClientHandle addDefaultCallDelegate/addCallManagerDelegate、
IMSClientManager updateCallDelegate/delegateForStack为真实导出；导出存在不代表MTK transport可直接实现。
`sendCallDisconnectionNotification1b7b33400`经clientId→delegateForStack，向wrapper virtual+10传
owned UUID string、reason和DisconnectInfo；需要上述真实client record、登记owner及原UUID lookup链。
本轮尚无证据支持完整生产provider替换：最早缺口是按原Personality初始化且具真实有效SipStack/注册状态的
owned client，随后才是完整配置语义→Android请求及UUID↔epoch/native-ID适配，不能以空factory或直接ACTIVE绕过。
只读切片、登记key解码、完整dial/owner JSON及原cache export读取脚本在
`x9/out/call-typed-dial-context/`（收尾归档主checkout；原件及转储不入git）。

typed hangup沿`CallHandle+20→1b7c434b4→MainQueue→endCall(uuid,reason)1b7b39be0`
找真实IMSCallInfo，再经Info+80的BambiCall调用`endWithReason1b7bc4500`；最终模型mutation
与MD disconnect cause映射仍未闭合，不能直接删模型或用固定reason替代。
`BambiClient::createNewCall`真实输入是SipStack/SipUri/delegate/options，不是简单UUID工厂。
ACTIVE决定门`1b7b30944`同时要求event+10等于Info+80实际BambiCall，以及其+140
MediaSession→+280 interface→virtual+98非零，才发布真实UUID事件。只替工厂或直接调用
ACTIVE会绕过真实owner/media门，不是已实现通话。最早provider缺口仍是完整client/personality
初始化及网络注册语义/lifecycle，随后才是UUID↔MD ID、状态/END和双向媒体。

#### caller privacy 与 TLV104 策略来源（2026-11-16，原件静态）

**两privacy byte已追到真实SIP消费者，但还不是生产dial profile准入。**
`Call+9/+8→IMSCallOptions+28/+29`是`BambiCallOptions`子对象的`+0/+1`。
`initializeNonEmergencyCall1b7b327ac..7b8`将options+28交给copy ctor
`1b7b42aa4`，其`2ac4/2ac8`以u16保留两byte；
`SipSession::initializeWithOptions1b7b90d24/28`再原值保存到Session+2a0/+2a1。
以下偏移限定于这条owned **SipSession** 链，不把其它对象的同偏移RTT字段当privacy。

`SipSession::fillCommonRequestHeaders1b7ba85a0`的决定门如下：

| Call+9 / Call+8 | 原件实际效果 |
| --- | --- |
| 1 / 1 | Session+2a1真：Privacy header使用当前stack的owned配置string；空string回退`id`，并调用`SipUri::makeAnonymous1b7ba8758`设置anonymous From |
| 1 / 0 | Session+2a1假、+2a0真：Privacy header明确设置`none`，不走上述anonymous分支 |
| 0 / 0 | 不做per-call override；当前stack的AlwaysAddPrivacyHeader仍可独立要求`Privacy: none`，不能解释为显式允许显示号码 |

`kPrivacyId1e26ef2d0`/`kPrivacyNone1e26ef2e8`由真实initializer
`1b7d882a8..88308`从literal`1b7e589e2="id"`/`1b7e785d3="none"`构造；
不能把磁盘上的零初始化C++ string误当运行时空值。stack owner也已闭合：
Session+c8→SipDialog（`90e24`）、Dialog+a8→SipDialogManager（`d07f14`）、
Manager+60→SipUserAgent（`b49968`）、Agent+8→SipStack（`b4f590`）。
SipStack ctor`cff9ac/b4`把SipStackConfig建于+a0；initialize`d014e0/e8`
真实copy入该配置，其首部是SipUserAgentConfig。后者assignment`ae6e9c..ebc`
保留+248..273及owned+278 string。因此consumer的Stack+2fd/+318严格对应
UAConfig+25d/+278；`initializeFromPrefs b4de70..dea4`写AlwaysAddPrivacyHeader，
`b4e44c..e480`写PrivacyHeaderValue。**它们不是Android key124的已证替代来源。**

真实上游`CallCommandDriver::computeClirValue_sync1f40dfba8..dfc9c`读取CallInfo+4c的
CLIRRequestState：3直接返回CT值2；4查询driver+60的per-SIM `rest::ClirInfo` map，
key来自CallInfo+d8，缺记录返回2；其它state仅2返回1，其余返回0。
state4有记录时：record+4==3→2；record+0==3或+8==0→record+4；
+8==2→2；其余→record+0。真实字符串表确认这是**两套不同枚举**：
CLIRRequestState的1/2/3/4为Suppression/Invocation/Default/Invalid；
CTCallingLineIdRestrictionValue的0/1/2/3为Disabled/Enabled/NetworkControlled/Unknown。
CommCenter`100dca8a4..940`将CT1→setter(1,1)、CT0→(1,0)、CT2/3→(0,0)，
与上表SIP效果一致；不能把Default3直接当Android raw CLIR0或2。

Android已证的可用窄子集是：**non-ECC、普通十进制号码、原owned CLIR明确为1或2**，
Rtm`265500`不会读取key124来改写它；Rmm`387fb0`虽读取key125并精确比较`"true"`，
但只有CLIR1且号码以`*82`开头，或ECC且key171为`"1"`才改写0，所以该子集1/2均原值下送108。
`getClirPrefix387450`的实际literal为1→`#31#`、2→`*31#`、其它→空string。
本小节不以常识替代尚未闭合的Android输入意图producer；记录这两个原值的策略稳定性及
Apple已证显式blocking/non-blocking效果，**不据此开放默认profile或增补生产白名单**。

104不是“当前NAS/IMSA显示有网”的派生默认值。`getEccPreferredDomain387a30`先读MCL72，
再两次独立读`isVoiceGsmRat`：首次真且pref==3→1；否则第二次真→0；否则再读owned MCL31
的voiceRadioTech，以`C={4,5,6,7,8,12,13}`分类，pref==3→!C，C真→0，其余pref==4→2、否则0。
不把多次status读取冒作一个原子snapshot。**pref72确为0时，所有分支均返回104=0**，
但仅凭本地空vector、普通拨号或10086实包不能授予真实pref72来源：
`setEccPreferredRat2794c0`写AP key72，经StatusManager同步到MCL；普通cf33不走pre-local-ECC。
`handleGetCurrentCallsResponse26c1d8..c218`确在原owned response data.size unsigned<=7时
reset AP72=0；需保留这条真实response/同步生命周期，不能用零初始化对象模拟reset。

`isVoiceGsmRat387810`还读取MCL1：`(key1&0xc)!=0 && voiceRegState!=5 && voiceRadioTech==20`
时强制false，否则调用`isGsmRat387c50`；后者`387cf0..d10`的CCMP决定实际bool，接受
RIL RAT集合`{1,2,3,9,10,11,14,15,16,17,19,20}`，前段mask只选择日志路径。
key31不是任意radio整数：`requestVoiceRegistrationState30f8f0`的042f response
要求8100/10B，value+1/u16LE进入slot CS cache+10；`updateServiceStateValue30e370`
经`convertCSNetworkType2fd710`构造RfxNwServiceState的voice RAT并set MCL31。
其中0x8000→RIL20；CS不in-service且isAlwaysReportRatAndCellIdentityInfo假时voice RAT为0。
当前kernel `X9QMIAT.inc`的CREG/CEREG/C5GREG及COPS AcT生成serving/data NAS快照；
`X9Messages.inc`的0a05/100(type)/101(state)只生成IMS-registration快照。
**NAS radio12、COPS AcT11、IMSA registered均不是上述owned MCL31.voiceRadioTech20，
也不提供MCL1/MCL72生命周期。** 本轮不新增query、探针、provider假ready或encoder字段。

私有证据为`x9/out/call-privacy-policy/`的`privacy-policy-original-proof.json`、
`read-original-privacy-policy.py`、`privacy-*.numbered.asm`及`ril-voice-gsm-rat.asm`/
`ril-is-gsm-rat.asm`；30个固定code切片直接读取当前root原cache、root CommCenter及匹配RIL，
cache text与提取镜像对应切片全部一致。只是原件静态qualification；未运行设备、测试或编译，
未改生产准入；完整caller-default/runtime profile仍未闭合。

#### typed事件 → FSM → 权威CallState（静态闭合，未hook）

三层model command wrapper真实地址点为 `101532fc0→1015fb090→101532ab0`，
最后进入 `CallCommandDriverDelegate10158aa28`。factory `10019b654` 建wrapper并传入
`100dc8b1c`，ctor将真实owner move到driver+88/+90；UUID lookup经
`+e0→+110→+1a0→100693d28` weak-lock CallController+78 vector、CFEqual后owned-copy
CallInfo。不是裸指针或把native整数ID当UUID。

ACTIVE实际 `+40→10068f0ec→1010488f0→Call+338→10104eccc→10104f4b8`，
END经真实 `+50→10068f610→1010494a8→Call+338→10105837c→101058b38`。
`4Call` ctor `10104730c` 分配0x68 FSM、保存强shared_ptr status delegate至FSM+8/+10，
Call+338拥有FSM。事件名来自原日志 `10104f530 CallActive` / `101058bb0 CallEnded`；
transition table由原ctor `10105e0c4` / `10105e568` 填BSS，磁盘零指针不等于无handler。

这里有两套不同状态，不能互抄：FSM+18的表 `10162f248` 为
Idle0/Ringing1/WaitingToDial2/InitializingMedia3/Dialing4/DialingCallEnded5/
Alerting6/Active7/Waiting8/UserAnswered9/Held10/WaitingToDisconnect11/Disconnected12。
CallActive从合法Ringing/Dialing/Alerting/UserAnswered/Held分支写FSM state7；CallEnded保留
Disconnected12和DialingCallEnded5的不同分支，不直接删Call。此前 `100696b2c` 写
Call+a4的是SRVCC状态，不是ACTIVE CallState。

最终status delegate `10158af20+28/1006984b8→10158b148+28/100699dd0→101018528`
先weak-lock nested delegate+20/+28 owner，按真实UUID `101005398` 取Call shared_ptr，
再于 `101018718` 写 **Call+a0**。ACTIVE传入0，不是FSM的7。匹配
`libCommCenterBase callStateToString1f03908e0` 原表 `20dad4e78` 是
Active0/Held1/WaitingToDial2/InitializingMedia3/Dialing4/Ringing5/Disconnected6/
Alerting7/Waiting8/Idle9。END走status+50 `100698710→10069a00c→101018e18`，
`101018f54` 直接写Call+a0=6，后于 `101019424` 再调用原state维护入口(...,6)。
正常state mutation保留 `101018ad8` 等维护，再经 `100ff81c4→owner+170 notify virtual+10`
传owned CallInfo；Call+2d4非零可在权威写入后提前分支，不能假设每次通知必到。
notification sender具体type/后续UI publish仍未闭合。

本项闭合的是已有typed事件到真实模型mutation，不提供MTK producer、UUID↔native
epoch映射或运行dial admission，也不绕过Bambi owner/MediaSession门直接调用ACTIVE。
它不解决CallList46 producer/offset6，不能将FSM/CallState/ECPI或wire CallEventStatus
编号混用。原件/枚举/窄asm与复读脚本在 `x9/out/calllist-handler-closure-analysis/typed-model/`；
无生产hook、设备操作、测试或新镜像。复查可在该typed-model目录运行
`python3 root-query.py f:0x100693d28 f:0x101018528 f:0x101018e18`，仅读原件；
旧文件 underlying-active-end-lookup.asm / underlying-real-uuid-lookup.asm 曾误将
100694eac/100696fcc称UUID consumer，不再采用。旧10068f434不是最终END consumer，
旧END CTState8已撤销；准确值为Call+a0=6，而非FSM12/5或SRVCC+a4。

#### 2026-11-16 typed拨出状态发布：TRYING不发布，RINGING实际是CallAlerting

原 `IMSCallModel` 的 CallEventDelegate address point `10159c718` 中，
`+30→1007c55f0` 是 TRYING，`+38→1007c57d0` 是 RINGING。
TRYING 按真实 UUID 经 `1007c4490` 查找后仅记日志；原 literal `1013ff6c3`
明确为 `IMS Incoming Indication: TRYING ... Ignoring.`，没有模型 state sync 或通知。
不能把 MD ECPI130 送 TRYING 后便称拨号状态已发布。

RINGING 的真实执行链是 `1007c57d0→UUID lookup→command+30/10019f148`
`→100dda270→10019ce40→10068e548→10068d734→10104879c`，
最后取真实 `Call+338` 的 FSM，进入 `10104e2c8→10104e890→10104eab4`。
该事件的原日志 literal `10127ac81` 是 **CallAlerting**，不是凭 callback 名称
解释为呼入 Ringing。`10104e348` 的临时 payload byte5 不当作最终 CallState。

dispatcher 读取 `FSM+18`，以 `(state+1)*8` 索引 BSS 表 `101673178`；
原初始化函数 `10105e078` 为表 `101673180` 的13个 state entries 填入 handler。
只有 Dialing4 的 entry（`1016731a0`）是 `10104ec28`，其他 entries 是
返回0的 `10104ec20`。合法 handler 先复核前态4，再经 `FSM+8` 的强 status
delegate virtual+28 发送 **CallState Alerting7**，随后提交 FSM Alerting6。
因此前态、FSM状态、CT CallState、QMI wire event 编号是不同契约，不能互抄。

status delegate `1006984b8→100699dd0→101018528` 保留原 owned 参数、
weak owner 与真实 UUID lookup `101005398`，在 `101018718` 写入 `Call+a0=7`；
其后的维护与 `100ff81c4` 通知仍须保留，`Call+2d4` 可在写入后提前分支。
InProgress 的 command+38 另经 `10068e834→10068e6ec`，不是上述 RINGING 链。

这是原件静态闭合，不是已经安装 MD producer 或用户态 provider。仍须真实
`(clientId, UUID, epoch, native ID)` 映射、可达 admission、完整生命周期和媒体准入；
不直接广播 ACTIVE，不构造未知的七byte CallList46，不把 void callback 当最终UI验收。
新原件切片/派生契约在 gitignored `x9/out/calllist-production/`，
`typed-publication-contract.json` 保存精确地址/字节；表来源复用
`x9/out/calllist-handler-closure-analysis/typed-model/stateengine-tables-init-range.asm`。
无测试、设备操作或新镜像。

#### typed初始化 / 注册 / 注销的原件契约

`createIMSClient_sync1f2e6803c→factory+38/100315998→CreateNewClient1b7c43a00`
创建真实Client并保存controller+d8；StackDelegate绑定controller+40 alias、真实shared_ptr及
controller+18 queue。正常`initializeImsClient_sync1f2e6e6d0`从model+268取得真实704B
StackConfig、model+38取得personality string，调用Client+f0/`initialize1b7c3d768`；
其四个MainQueue block进入两个真实`initializeFromSIM` overload，保留真实CFDictionary/
clientId/personality。controller+f8=1及initialized回调置2只是初始化阶段；后续真实
NetworkServices来自model→Client+130/`updateNetworkServices1b7c3f368`，不能绕过配置。

网络枚举单独来自`handleRegistrationStateChanged1b7abc4a4→wrapper1b7c83344→queue/
weaklock→controller1f2e6b420→model+60`：0 Deregistered、1 Registering、2 Registered、
3 Deregistering。真实64B RegistrationActiveInfo由`1b7abc5a4`填service mask、注册URI、
stackId、expiry差值；URI尚未最终实名为IMPU，expiry截u32未见clamp，不用号码/固定秒代替。
consumer `1f2e6b660`按真实DataMode/LastAttempted策略归一服务mask；optimized cache stub
`1f2fb1828→1f038d49c`为`(ipMask&1)|((ipMask>>1)&2)`，不是registered bool→全能力。
随后`saveImsRegistrationState1f2e6da78→updateCurrentImsState1f2e78df4`在model锁内保存
DataMode/registered/u64 mask，再经真实service+78/`1007ab37c`写root per-personality节点
`+38 bool/+40 mask/+48 flag/+50 DataMode`，observer+30通知真实map。静态model mutation
已闭合，但observer后续CT/UI发布与实际MD-backed输入未闭合，不称设备已注册。

真实20B RegistrationInfo含i32 retrySeconds、SIPcode、termination callerBool、willRetry、
reserved0与RegFailureReason；error producer仅匹配ImsResult domain取SIPcode，否则0，
termination默认200来自原件常量。retrySeconds对-1/0保留，其他max(absolute-time(NULL),0)。
失败consumer `1f2e6bbdc`先提交false/mask0再处理真实retry/PDN/policy，不能只删map。
原enum6 DisabledCountry、7 Authentication Failed即使SD日志显示Unknown也不能抹除。

正常deinitialize `1f2e6f40c→Client+100/1b7c3e578→MainQueue→IPManager1b7cbe71c`
进入真实Bambi/SipStack注销；只有uninitialized回调`1f2e6b264`才将阶段置0。
factory shared_ptr deleter`100315b00`仅ret，release不等于delete；cleanup里的Client+c0
是removeStackDelegate，不是removeDefaultCallDelegate或全销毁（更正此前槽标签）。
显式Client析构→removeClient→record/maps销毁有据，但普通controller的cleanup并不
等于同步注销完成、队列排空或Client析构。`callOnMainQueue`
实际导入dispatch_async，原removeClient block捕获clientId裸指针；不模仿此未闭合时序，
不声称原件UAF。最窄替换边界是per-personality真实typed Client+StackDelegate owner/queue，
仍需真实MD服务mask、URI、expiry/retry、PDN/DataMode、failure/注销确认及call/media生命周期。
精确原件与复读脚本保存在`x9/out/ims-call-provider-contract/`，本项没有production hook。

#### 实际typed Client接口与拨号的非virtual边界

同版原件的`ims::Client` address point是 **`1d90b86d8`**，不是legacy
`IMSCallClientInterface`的`101531058/20dada348`。实际Client有44个槽（`+0..158`）：
`+30 setCapability`、`+90 addMessageDelegate`、`+98 addStackDelegate`、`+a0 addCallManagerDelegate`、
`+a8 addDefaultCallDelegate`、`+b0 addXcapDelegate`、`+c0 removeStackDelegate`、`+d0 removeDefaultCallDelegate`、
`+f0 initialize(personality,StackConfig)`、`+100 deinitialize`、`+108 sendSMS`、
`+130 updateNetworkServices`。**这张表没有dial槽**；不能把legacy `+28`解释成typed拨号。
构造函数`1b7c3eb68`先设置ClientHandle address point`1d90b8860`，再写上述Client
address point，ClientHandle+10是原clientId string，不是独立可替换transport指针。

关键直接依赖在正常Call工厂之外：`ims::Call` ctor **`1b7c412a8`**在
`412e8..41314`直接复制ClientHandle+10的string到Call+28；它没有在该构造链中保存
ClientHandle指针或shared_ptr。因此为factory+38返回自造vtable对象，既会遇到这个
非virtual布局读取，也不会使已有Call的dial改走新Client vtable。不能凭这个局部事实
宣称全系统没有client owner；manager/record与SD controller另有真实owner。

`Call::dial1b7c41468`复制真实UUID/clientId与CallConfig到IMSCallOptions，
`41790`派送MainQueue block **`1b7c41878`**。block于`418a8..418ac`从
IPTelephonyManager+90取真实IMSCallManager，在`418cc` **直接BL**
`initializeNonEmergencyCall1b7b32708`，不是Client virtual调用。它继续使用既有
AP SIP/Bambi owner链，不会因Client初始化回调换成MD-backed就自动转成MTK DIAL。

据此修正上一节的“最窄替换边界”：per-personality Client/StackDelegate只是
**初始化与注册子边界**。完整生产桥至少还要同时闭合正常CallFactory+40所创建的
真实typed Call/UUID、dial/action实际入口、client/epoch/native-ID owner以及真实state/media
消费者；不以只替Client或CC legacy factory、复制常量registered、直接ACTIVE回调代替。
当前`X9Calls.inc::x9c_dial_submit`仍无生产admission caller，service09亦未公告。
现有AppSync exec注入不覆盖CommCenter，日常构建没有call用户态provider，未添加空注入stub。
这是实际调用链与当前构建源码的静态结论，不是设备已经选中或执行该路径。

原件切片与44槽注释保存在`x9/out/call-provider-integration/`：
`typed-call-dial-direct-manager.asm`、`typed-client-slot-map.txt`；ctor及dial上游原件在
`x9/out/ims-call-provider-contract/`。复读只反汇编固定原件：
`/opt/homebrew/opt/llvm@22/bin/llvm-objdump -d --demangle --start-address=0x1b7c41878 --stop-address=0x1b7c41904 .local/apple/dsc-20H392/System/Library/PrivateFrameworks/IPTelephony.framework/Support/libIPTelephony.dylib`。
本项没有生产改码、编译、设备操作、测试或镜像。

#### 普通cleanup、异步注销与late callback的真实边界

本轮直接读取真实Cryptex cache：Client ctor76B、vtable352B、SD cleanup槽调用44B，
与固定提取原件一致；上一轮`+b0 removeStack / +c0 removeDefault`为手工减基址错误，
**不是版本/提取ABI差异**。正确槽为`+98 addStack / +c0 removeStack / +d0 removeDefault`。
实际`cleanup_sync1f2e4233c`的`42420..42430`调用Client+c0，因此**普通cleanup已接
removeStackDelegate**，此前“cleanup仅removeDefault、未连接removeStack”的结论撤回。
真实cache完整Call block140B同时确认`418cc`的直接BL，不依赖提取文件的单独槽猜测。

但这不等于Client退休：普通deinit写controller+f8=3，uninitialized置0并保留
controller+d8/e0；controller dtor释放shared control block时，root factory custom deleter
100315b00仍仅ret。CreateNewClient实际返回独立64B Client，manager的48B record是另一对象。
已审普通deinit/cleanup/release没有证明Client真析构已发生，不能据此free或重用旧identity。

`removeStackDelegate1b7aaaa40→updateStackDelegate1b7aab668`只发MainQueue
dispatch_async，不是完成fence。block1b7c34e30的nullptr分支在34ff0调用
IMSWrapperDelegate::removeStackDelegate1b7aaba1c，擦wrapper+40的weak/queue登记，
然后直接跳35160，**绕过SharedHolder+0的stack强owner map**。
nonnull分支是emplace：3500c的现存非空entry保留首strong owner，35014..35068才插空位；
不能机械复制它来实现任意same-key owner替换。

uninitialized producer1b7c84274已经复制出去的weak pointer与reason不会被擦登记撤回。
dequeue consumer1b7c844c0仅weak.lock成功后以临时strong调用delegate+40，失败drop；
该consumer未检查source-client-ID、init epoch或当前phase。真实cache支岛
1bc65ff8c→194712078为libc++ weak.lock，1bc65ff80→194712048为release_weak，
不是uniform-delta猜测。因此“remove已返回→late callback已取消”不成立，不将局部时序
形状称为原件实测UAF。

真Client dtor1b7c3ec00才同步removeClient1b7c33900，销毁record的wrapper weak maps、
SharedHolder strong maps、strings/queue并擦manager record，随后异步339e0→
BambiClient::invalidateStack1b7da7e0c。末步仍不是completion fence，且block捕获borrowed
client-ID pointer；新provider须own-copy身份并建立真实终态/队列退休条件，而不是仿照
异步裸指针或把uninitialized当全对象销毁。

精确原cache切片、修订后的自动槽表与复读脚本在`x9/out/ims-client-lifecycle/`：
`actual-cache-client-vtable.txt`、`actual-cache-sd-cleanup-slot.txt`、
`actual-cache-typed-call-dial-block140.txt`、`lifecycle-decoded.json`、`static.py`。
可从主目录用`python3 x9/out/ims-client-lifecycle/static.py ip dis 0x1b7c34e30 reread-update-stack.asm`
只读原件。固定UniXNU/hackios-xnu-kernel/QcomBaseband及其tracked patches的窄typed-provider
名称查询未找到直接可复用实现；QcomBaseband已核入口是QRTR/QMI wire/board transport，
不把限定零命中升级成全系统不存在。无生产改码、构建、测试或设备动作。

#### 真正 CallFactory / RTTI / action owner 与可导入构造入口

正常factory的vptr是`101532938`（RTTI=`11CallFactory`），createCall为+40→`10019c234`。
物理ABI：x0 factory，x1指向UUID owning首字段（driver实际传CallInfo地址），
x2临时`shared_ptr<ims::ClientHandle>`，w3真实`isEmergencyCall(CallInfo const&)`，
x8为`shared_ptr<ims::CallHandle>` sret；stripped正式x1声明仍不猜。
正常分配0x60=control0x18+真实Call0x48，control release真调用Call virtual destructor，
不是Client的ret-deleter。UUID经CFRetain和CFUUIDCreateString，不能生成默认UUID。

`100b93568→10019b3d0`创建40B factory；`100fe1b54/1dd8`将强pair安装至controller+48/+50；
Registry按`CallControllerInterface` RTTI插入controller，不是独立factory item。
factory锁自身weak后`10019b4c4→10019b654→100dc8b1c`，driver ctor`100dc8dd0/dd8`
保存同一强factory pair至+328/+330。factory自身weak为+8/+10，Registry weak为+18/+20。
正常action还通过`1007cb918 __dynamic_cast(ims::CallHandle→ims::Call)`，
成功才virtual+80；不拿任意假vtable/裸对象代替typed拥有与RTTI。

当前根盘原cache **export trie**确认Call C1真实导出`1b7c41390`（单B到C2`1b7c412a8`），
Call RTTI`1d90b81d0`和vtable symbol`1d90b7db8`也导出；Call D0/D1/D2仅本地、没有相应export，
导出的是CallHandle D2`1b7c41344`。提取dylib的export command为0/0不代表原cache无导出。
不能声明不可导入的Call destructor，或仅构造原Call就声称拨号已转MD：原dial仍直入IMSCallManager。

已恢复18槽Call、13槽CallHandle、25槽event wrapper与6槽manager delegate；manager delegate+10
外部目标仍未解。Call字段+10 UUID/+28 clientId、+8/+9 CLIR、+40 TextMediaSessionMode/+44 relay有据；
answer/end调用manager+20/+28，end reason先经原enum→string，不直猜MD cause；mute+60的ret不证明媒体静音。
本HEAD无call用户态provider builder/CommCenter loader，exec allowlist不含CommCenter；
`x9c_dial_submit`无生产caller，`x9c_request`没有VOICE20 admission，现有内核C ABI未证明用户态callable面。
因此本轮不加仅loading/空handler，尚须owned factory替换、profile/UUID/epoch/native-ID、内核入口及真实媒体生命周期。
原表/汇编/导出复读脚本与准确decision在`x9/out/call-typed-actions/`。

#### provider-install：真实 factory 返回面、临时 action handle 与 AP 漏路（静态闭合，未安装）

最窄的真实消费者是`100dcf174`：`100dcf198`取driver+328的强owned factory，
`100dcf1a8`读其vptr+40、`100dcf1c0` plain `BLR`；正常factory addresspoint
`101532938`的该槽地址是`101532978`，原实现`10019c234`。
因此可实现方案应保留真实factory/controller/driver与原+40构造，只在真实返回面接管action；
不是替换完整MCommandDriversFactory，也不是查找不存在的独立Registry factory item。
本轮尚未实现对该loaded槽或真实factory实例的可写保护、安装时序、原指针保存/原子切换和撤销，
不能把一个地址或loader receipt写成已安装。运行时必须校验上述原件image UUID/ABI和dyld已fixup指针，
不能使用磁盘chained-fixup编码值；当前arm64 plain `BLR`不授权猜测arm64e签名规则。

**Call是临时action handle，不是MD生命周期owner。**正常dial的factory pair在sp+600/+608；
config helper的copy sp+5d8/+5e0在`100dce798..7cc`释放，model正常路径仅保有local cast pair，
调用Call+80后释放；driver `100dcec04..c70`又释放剩余sp+5d0与sp+608。
另一个真实动作`100dd484c→100dd4888`重新调用同一helper创建Call，调用+30后在
`100dd48e8..491c`释放。这已否定“仅首次dial对象安装，然后靠其析构退役MD”的方案。
每次factory返回均须关联既有logical client/UUID/native epoch，context寿命独立于这些临时对象。

真Call addresspoint为`1d90b7dc8`，不是导出vtable symbol`1d90b7db8`。
D1/D2 `1b7c43ac8→1b7c41344`仅销owned UUID/clientId；D0 `1b7c43acc`随后delete，
**两者均不发hangup、END或deinit**。shared control `10019ed40`将this调整至embedded Call+18，
调用其vptr+0 **nondeleting** destructor，control D0再释放整个分配，不能对embedded Call执行+8删除。
若以后使用共享action adapter，应保原RTTI[-2/-1]、原D1/D0及typed布局，表至少与引用它的所有对象同寿；
native退休只在真实终态/准入失败/逻辑client注销与callback fence闭合后执行，不以Call析构代替。

**只改dial/hangup仍会漏到AP。**CLIR+38 `1b7c42d84`仅写w2→+8、w1→+9，可保原setter；
text+28 `1b7c432fc`除写+40还派block `1b7c433d4→1b7b3ad4c setTextMediaSessionMode`；
relay+30 `1b7c43898`除写+44还派block `1b7c43970→1b7b3acac setRelayMode`。
后两者在config阶段已可调用，MD-owned对象必须保字段mutation但隔离AP发送，不能原函数passthrough。
其余会发AP的ring/answer/end/DTMF/refresh/hold/resume/pull/branded-image动作同样必须有明确准入，
未实现不能回落AP；原mute仅ret不证明媒体静音。当前缺native user provider context/admission，
仅内核内部dial入口、VOICE20仍拒绝，故本轮不写unused installer或空factory，亦无生产编译/上机结论。

精确汇编、原件UUID和facts/design分层见`x9/out/call-provider-install/provider-install-contract.json`；
`factory40-actual-callsite.asm`、`call-helper-other-action-and-config.asm`、`driver-normal-pair-release.asm`、
`call-original-d0-d1.asm`及`text-config-ap-leak.asm/relay-config-ap-leak.asm`可原件复读。
原typed callback最终消费者由`x9/out/calllist-production/`独立追踪；返回真Call不等于state发布，
不要用TRYING忽略通知、未知状态数值映射或fake ACTIVE冒充通话流程。

### 通话专用voice音频契约（仅原件静态，不冒充媒体speaker）

同机HAL `ScpSpeechOn2474a0`先registerAdspFeature14，`247544`以scene0、AP↔DSP各65536B region
和真实`processDmaMsg246660`/this注册callback；GOT44d3b8由原库relocation确认。
`AudioMessengerIPI1dceb0→audio_ipi_dma_cbk_register42f790`保存callback/this，
`42fa5c..6c`发ioctl40046915，20B payload含magic/~magic、scene/reg、a2d/d2a尺寸。
Android参考REG_DMA→alloc_region(scene0)按实际task/dsp/controller发双向region的
offset/size/read_idx/write_idx，再以ADD1 NEED_ACK UPDATE_REGION发布；公共descriptor ABI可共用，
但不能复用SmartPA的coreA/scene/单环尺寸与owner。
`247688..6bc`实际scene0/layer2/type0/ack1/AD20，param1是实际时间/on打包值，param2是modem index，
`42efc0→42f548..55c`落ioctl40046900。没有把本机抓到的单次值当运行默认值。

**更正**：OpenDSP this+158是SpeechDriverNormal；`sendIpcCmd24c720→1b2ac0`最终只有返回0，
不是MD↔DSP3D00 transport。scene0 callback只分DA00/DA10/DA11/DA12；3D00是另一MD→DSP方向，
其完整producer/consumer、MD voice DMA、voice AFE与Aurisys参数生命周期仍缺。
当前native IPI_DMA只拥有coreA/task14或32，板级transport实际只注册core0；固定参考中双核scene0
路由HiFi3_B，但实际physical routing来自task-info，不把Aurisys core_type1猜为物理core1。

实际HAL `audio_messenger_ipi_init42e2d0`在`42e5f8..42e60c`以
**ioctl40046914**取kernel task-info到BSS4675d4；每项8B，`audio_get_dsp_id4302a0`
读取首u32，`get_audio_controller_task430310`读该项+7。Android固定参考按
`core_total>1`选择scene0→HIFI3_B(enum3)，`ipi_dsp_id_to_adsp_cid`减2→physical core1，
再走`adsp_send_message`。已抓DT明确core-num2、core1 mailbox2/3与独立IPI_DMA_B
0x100000；这仍不是本次runtime task-info[0]的8B回读，更不是DSP consumer证据。
HAL静态`get_audio_controller_task_by_id430370`常量002f2b2a2e2e明确id2→42/id3→43，
也不能拿它替代动态scene→dsp-id选择。

REG_DMA参考布局现在已闭合：header128B，scene0的AP_TO_DSP / DSP_TO_AP两个entry
分别在+128/+512，每entry384B；offset+0、size+4、read_idx+128、write_idx+256。
ADD0 DIRECT发`{u64 base_phy,u64 pool_size}`，只有发送成功才置inited；ADD1 NEED_ACK
以param1=scene0发布region。**这两条65536B region没有被证明是MD voice PCM的UL/DL环**，
不能用AP↔DSP控制/数据region的已知布局补造MD3D00的payload、PCM地址或节拍。
窄MD speech/SPC原件区间90636ce8..90659400与907ff500..90808600未见3D00/D300
immediate，仅限定这两个范围；`spcIO_sendMsgViaCCCI90652c34`直接走CCCI4/ccci_write9065281a；
另一SPC stream producer907ff728在907ff8e0调用AFxx mapper907ff500、907ff972调用
ccci_write。两条legacy链均不是已证MD→DSP3D00，不把不同caller混成同一wrapper调用链。

当前`X9Calls.inc`没有native audio/DSP/REG_DMA生产调用，原core0/IPIA/scene14或32
owner也未拥有scene0通话。生产接入仍需独立call epoch/SIM/modem-index生命周期，
以实际task-info确认core/pool选择（若core1则B）的双region及同generation强pin、真实AD20与完整DSP reply，
然后闭合MD3D00 producer/payload、同版DSP consumer/ACK、MD PCM地址/权限/codec节拍。
不能借用SmartPA owner或新增stub宣称通话音频修好。本轮只读材料归档
`x9/out/call-voice-md-handshake/`（原asm、源码切片、task-route-and-dma-decoded.json）；
没有生产改码、编译、测试、设备操作或镜像。

iOS原件VirtualAudio factory21ebc8比较CFString62e790的UID`Baseband Voice`，匹配后21ee7c
创建592B/vtable621c30的独立设备，包含`Baseband Downlink`/`Baseband Uplink`。
virtual+30→326560消费`ttye`/4B→异步CTSetTTYMode、`ctsr`/8B double→真实callback
CTAudioSampleRateSwitchDidComplete；CT callback328944的type0..2经table524698映射
`vcdt`/`vcdr`/`thsr`并通知。root/os20H392的VirtualAudio LC_UUID同
`3196333F-5A48-373B-B427-0146DC1177FB`，原件build metadata为20H392/16.7.16。
不能仅广告设备UID或复用Speaker/Hawking媒体设备就称voice接通。
全部窄切片/私有原件在主checkout`x9/out/call-audio-analysis/`；没有生产改动/编译/设备验收。

新增stream入口静态证据：Baseband Voice factory调用base ctor `20c078`，`20c110`把真实
输入AudioObjectID保存于device+b8；slot88/`327b18`分别调用`20a4c4(dir0/dir1)`。
该函数用实际ID查询`stm#`的`inpt/outp` scope，经AudioObjectGetPropertyDataSize/
GetPropertyData取得真实stream ID，再由virtual+188/`20a29c→231740`构造owned wrapper。
不是写UID就凭空生成双向stream。voice+240另是`dispatch_once→216d50`创建的CT singleton
shared owner（`217124`调用CTServerConnectionCreateOnTargetQueue），**不是MD DMA/media指针**。
模块内CreateIOProcID helper `322d3c`检查错误及非NULL IOProc，但它从voice入口的完整caller/
真实硬件provider仍未闭合，不把模块级API引用当voice streaming成功。窄切片/复读
`x9/out/voice-provider-analysis/{voice-provider-boundary.json,macho_functions.py}`；原件静态，
没有audio生产改动、构建或设备动作。

#### USIP AD50 前置与实际 MD SpeechOn：不同层、不同 owner

HAL真打开`/dev/usip` O_RDWR，`c00844f1/c00844f3`读phone-call config/hwif；参考驱动实际copy u32，
不能当8B task-info/ready。已抓DT config3/hwif0、真实拨出opendsp3；`configSpeechInfo1a6840`
清128B并从动态config/hwif填+3/+55，不把它们替换为默认常量。

固定`mtk-usip.c`的open发送 **AP AD50 / IPI_MSG_A2D_GET_EMI_ADDRESS，不是MD3D00**：
header16B（magic8888/scene/u8 src1,target2/type1/ack0/idAD50/payload_size16/param2=0），
payload两个u64 `{USIP_AP_PA+0x30000,0x30000}`。main scene0及enh bit1时sub scene19共用payload，
BYPASS_ACK，send_result0不等于DSP回执。范围包含PHONECALL8000和SMARTPA28000，
不能借SmartPA core-A/ticket替代独立call owner。当前实际LK ID24 descriptor为
AP=f0d00000/MD=42d00000/size60000/flags2；这是日志事实，不硬编码为构建参数。
当前`x9_modem.inc`明确跳过ID24映射/清零，ap_off=ffffffff不是NC offset。
未来只能先按锁内crs/regs配对读真实descriptor并取得独立访问/lifetime资格，不能盲map/clear/start/ready。

另外`SpeechOnByApplication1ad1d0/1ad300`真发 **MD2f20/sub25/128B sph_info**；
MD body校验并保存配置→Spc_SpeechOn→SP_SpeechOn→af20已由原件及Android拨出af20 result0闭合。
此控制响应仍不证明MD PCM环、iOS双向声音或AD50消费者。
同版AD50 DSP consumer、真实8B task-info/core、独立USIP生命周期、MD3D00/D300和UL/DL PCM地址/节拍仍缺；
不用另一SPC空sendIpcCmd返回0、参考路由或REG_DMA两个65536B来填空。
准确分层、原汇编及参考推导（未发送、非抓包）在`x9/out/call-md-voice-owner/usip-ad50-and-md-speech-on-contract.json`。

#### IMSA 注册分流窄修与短信初始化配置边界

RIL `onHandleUrc290e18..290ecc`先按4a01/tag100/u8分流：type0 CIREGU/tag101、type1 WFC、
type2 ImsRegInfo、type3 RegResp。生产`X9Messages.inc`现在必须**type0**才解析101为注册bool0/1，
不混CIREG query的0..4枚举；此修复只隔离实际parser分流，不设置网络ready。
两条真实4a01/type2（retained-radio line5157/5321）证明共用消息：104是account_id、
8105是URI、106是expire_time；**255是expiry，不是service mask**。expiry非零→report_type1，
为零→0；type3另owner report_type2/account104/error107/reason108，core保存expiry=-1及code/reason。
type2实际没有101，旧parser已忽略；不声称日志证明曾污染注册state。
不以account猜stackId/personality，不用脱敏URI、未知expiry单位/clock或缺失Apple service mask补ActiveInfo。
原dispatcher、core owner、帧解码及真实Baseband recipe单对象exit0/34warnings在
`x9/out/call-init-production/registration-contract.json`；固定完整kernel结果见NOTES。

短信原初始化emitter→callback准确对应：WMS3a DCDisconnectTimer、39 RetryInterval、38 RetryPeriod。
这些配置回调成功/失败日志不产生ServiceReady，未证明unsupported配置终止初始化；
不补无MD timer writer依据的fake success ACK。已有Android4601→601/602→SMSC形状与实现吻合，
当前Messages同token commit/重开或MO对端收到无新增运行样本，不当作验收。
精确原回调与边界在`x9/out/call-init-production/sms-followup-boundary.json`。

### SMS集成：未发布MT的原子host queue准入

短信仍用独立WMS/SMS注册，不能作为VOICE readiness。生产修复`f1aa29f`在原
`message_event`已交state2后、重复changed CMD poison/control loss与host入队之间增加原子fence：
`x9_md_mipc_message_publish(token,sim,callback,context)`在同一MD锁内检查真实state2 owner、
原mask低SIM位、online/SMS poison及既有CS envelope，持锁覆盖真正host队列入队。
受信callback仅`bb_x9_sms_event`，frame锁外预分配，失败锁外擦除释放，原PDU/token保留。
锁序为MD mutex→bb_lock；callback无malloc或MD重入，两个mutex仍可能等待，不称完全nonblocking。
原整CMD mask/txid、已delivered/replying/selected/partial和物理ACK drain、普通bb_push/CALL/data不改。
新增13项人工源码向量、总33项**只编译未执行**；统一kernel-only结果见NOTES。
host queue receipt不等于Messages显示/持久化、物理ACK成功或MO对端收到，仍无最终通话短信镜像。

短信TAG scoped生产`25e40ee`（原`2db0d50`）再修indexed0607/103=0已提交但未selected时
超时/删client仍物理mark-read的缺口：native allocator和唯一writer携原slot/generation及64-bit
monotonic deadline检查；精确token/slot/gen abandon只state1→原state6/take=-3，不造CNF。
selected/partial/late-CNF及已mark-read的step2验证/hold不变；其他generic提交者不变。
Apple readStored callback`1975920..38`实际用index/readtag/mode，`197596c`是25s host guard；
未拿旧319中unsupported的存储启动日志当修复验收。9项新TAG人工源码向量、总42只编译未执行，
SMS/CALL host、header与真实recipe两对象均exit0；统一kernel构建见NOTES。

### Apple短信消费者 → Messages保存分支（原件静态，非当前消息验收）

本轮核对Apple WMS01的TransferRoute parser **`1f192cfb8`**：
ack_indicator/u8、transaction/u32 LE、format/u8、length/u16 LE及SMSC/TPDU bytes与
当前桥匹配。WMS31 List parser **`1f192d720`**同样是count/u32与每项index/u32、tag/u8。
没有找到新具体codec mismatch，不为“补全iOS”另造PDU parser或绕过原Messages存储。
真实libCommCenterBase `asString1f0386df8` / 表20dad23d0明确
MsgPduProcessStatus：1 MemoryFull、2 ParsingFailed、4 SmsSuccess、9 CMASSuccess；
这个processing状态既不是QMI Result error，也不是Messages数据库提交结果。

原SMSServiceSession的完整接收入口已静态闭合：
`smsMessageReceived1f3cc4ebc→_processSMSorMMSMessageReceivedWithContext1f3cc4818`
→CTMessageCenter `incomingMessageWithId`（49dc）→转dictionary（4be4）→
`writeMessagePayloadToDisk:fileName`（4c44）→`_receivedSMSDictionary`（4c58）→
`_processReceivedSMSMessage1f3cd41e0`→`didReceiveMessage:forChat:style:fromIDSID`（1f3cdc934）
→IMDServiceSession `didReceiveMessages1c6518118→_storeMessage1c6518e88`
→`_IMDStoreMessage1c650556c→IMDMessageStore.storeMessage1c64799e4`。
payload文件写盘在前，不等于message record已提交，也不证明当前token走过此链。

IMDPersistence `_IMDCreateIMDMessageRecordRefFromIMMessageItem1a8b33ad8`调用
`_IMDMessageRecordCreate1a8ae2834`，按实际`_IMDIsRunningInDatabaseServerProcess`
决定数据库server内建记录或sync-XPC路径；不是收到WMS就必然直接写SQL。
数据库server create分支在 **`1a8ae3380`** 调AddMessageRecord1a8b5467c，
locked callback1a8b54720于54740调用CoreSDB AddRecord1e75fbc78，
再于 **54748** 调SaveStore1e75f7668。SaveStore于1e75f7698传transactionType=1到
SaveWithPreAndPostCallbacksAndTransactionType1e75faa88，正常事务分支
1e75facb0调用Commit1e75ff1f8。只有`sqlite3_get_autocommit=0`才在1e75ff228调用
PerformSQL1e75f8890执行 **`COMMIT;`**（原CFString20d0a0360/data1e7606177/7B），
再经1e75f88b8→Step1e75f5674→sqlite3_step1a987c348。这里已经连接真实调用，
不是仅搜到COMMIT字符串；已经autocommit、无pending工作及特殊transactionType仍有不同分支。

save于1e75facd4..face4把SQL result0/101（SQLITE_DONE）转bool，AddMessageRecord
保存并返回它；**create的1a8ae3384**据此决定失败释放record并返回null，或取真实RecordID。
因此实际保存成功须过这一决定分支，不能用callback出现替代：XPC completion
**1a8b7b3e8即使Create null也调用**；IMD helper `didStoreBlock1c6505f0c`的w1来自
storeMessage之前的shouldStoreDecision，不是SQL commit result。post callback（若提供）
也在commit之前运行；这里SaveStore本身没有提供pre/post callbacks。

原件静态保存控制流/失败分支已闭合，**仍无当前短信token到成功record/SQL分支、
数据库重开/最终Messages显示、或MO对端收到的运行证据**；WMS37 receipt不改变此边界。
精确原asm、selector解码与字段/事务JSON在`x9/out/sms-ios-consumer/`，包括
`apple-storage-boundaries.json`、`apple-imdaemon-store-helper.annotated.asm`、
`apple-add-message-record.asm`、`apple-coresdb-save-commit.asm`、
`apple-coresdb-sqlite-commit.asm`、`apple-xpcserver-record-create-tail.asm`。
新支岛target与COMMIT常量已从当前root Cryptex cache只读核对：12个支岛各12B、
CFString32B、SQL7B均与先前采样逐字一致；`apple-root-cache-provenance.{json,txt}`
保存实际subcache/offset/VA/raw bytes。旧sysos mount只作比较来源，不再作为权威原cache。
从主目录可用`/opt/homebrew/opt/llvm@22/bin/llvm-objdump -d --start-address=0x1e75ff1f8 --stop-address=0x1e75ff240 .local/apple/dsc-20H392/System/Library/PrivateFrameworks/CoreSDB.framework/CoreSDB`
只读固定原件。不运行人工向量/测试，不读真实短信数据库文件、不改生产、不编译或操作设备。

### WMS37物理ACK completion：实际host receipt后退休

原物理token ACK结果已take后，legacy void-send仍可在OOM/queue-full时丢WMS37回复并清owner。
来源`ec56e429`，集成`8c6a00a9`把原local generation与真实BB/QMUX generation分开保存；
BB helper只接受14B WMS37 terminal结果，持bb锁校验原route/service5/node/used/
non-release_pending/generation及容量，实际入队才answered。失败擦除临时buffer，保留原
req/slot/txn/result，只重试local queue，不重发MD ACK、不复投PDU。`ack_done`确保物理
reply_take仅一次；物理owner退休后纯host reply不再触MD，generation丢失仍drain旧token，
不回新client。尚未决定的结果在取得MD锁后再判原64-bit deadline；首次terminal保持，
先timeout后late-success不覆写，及时物理success也不被后续queue延期改成timeout。

仅same-slot/local-generation/txn的现存WMS37 job消费重送：原bytes相同等待原结果，
changed/malformed reuse保原owner/result，不再产生第二BAD/ARG或MD ACK。scoped判定在
generic validator前，但仍要求合法node/route/type0/msg37/至少7B header；无owner与其他
service/request不绕通用校验。成功receipt只打印token/SIM/txn/result一次，不含号码/PDU，
明确`not durable storage proof`。58项人工源码向量仅编译未执行；现存313–321日志及Apple
MsgPduProcessStatus→sendAck callback均没有同token Messages持久化/重开或MO对端收到证明。

### modem 与 Android serializer

输入 `.local/x9/modem/parts/modem_a.img` 的 md1rom 是 **nanoMIPS**，不是 ARM 或普通 MIPS。
本版本 ROM VA `0x90000000` 对应文件偏移 `0x200`；md1_dbginfo 可恢复函数边界。
OPPO `mipc_msg_tlv_const.h` 仅用于解释，以下字段已对照实际 MD producer/consumer：

| 原件函数 | VA | 关键结论 |
| --- | --- | --- |
| `mipc_call_approve_incoming_cmd` | `0x9132568a..0x913257b6` | 8901: 100 id/u32，8101 number/NUL≤128 B，102 toa/u32，103 seq/u32，104 mode/u32，105 evoltesi_flow/u8 |
| `mipc_call_approve_incoming_rsp` | `0x9131f2bc..0x9131f340` | **8902: 100 approve/u8，101 cause/u32（可选），102 id/u32，103 seq/u32** |
| `mipc_call_answer_get_req` | `0x9131be92..0x9131bf98` | 0907 支持省略 mode/id；不能因此猜 Android 多通话策略 |
| `mipc_call_answer_get_cnf` | `0x9131bf98..0x9131bfbc` | 0908 的结果 tag0/u32 |
| `mipc_call_hangup_req` | `0x9131d234..0x9131d374` | 0905 mode100/u32、id101/u32；codec 仅保留旧 Android 记录中的 mode0 + 显式 id |

**更正旧交接**：8902 的 0100 是批准与否，不是 call id；0102 才是 call id。
本地测试故意让 approve=1、id=`0x12345678`、seq=`0x90abcdef` 不相等，防止样本值同为 1 掩盖错误。

MD CMD producer 使用高半字节 mask（例如 0x10/0x20）。Android `libmipc.so` 的 CMD dispatch
`0x13630..0x136a4` 原样传递 header；`mipc_msg_rsp` `0x145d0` 经 `0x13d60` serialize/write。
**不能套用 WDS/SMS 的低位 AP SIM mask，也没有据此猜 iOS subscription 映射**。
此处此前仅凭 libmipc 得出的“整字节单 bit / RSP 沿用原 mask”结论已被下方真实来电与完整 RIL 路径推翻。CMD owner 保存原 mask 作为身份，RSP 独立按 RIL 的 slot 转换，不从高位选择 SIM。

### 2026-11-15 真实呼入：CMD 路由纠正

只读采集已保存于 `x9/out/android-incoming-20261115-192820/`；`retained-all-logcat.txt`
保留呼入开头，`retained-radio-logcat.txt` 开头已部分滚出，不能据后者否定前者。
完整 `8901` 为 mask22/tx2a9a/72B，Android 实际 `8902` 为 mask02/同 tx/40B，
approve100/u8=1、id102/u32=1、seq103/u32=100。
号码 value 被日志脱敏（连 NUL 位置也遮盖），这不是未脱敏 wire 重放。

已追同版 RIL 原件完整路由，而非仅据一条样本猜 `mask & 0xf`：

1. `RfxMclMessage::obtainMipcCmd` `19f978..19f984` 读取 CMD mask，调用
   `RfxMipcUtils::mipcSlotIdChangeToSlotId` `1ada10..1ada34`：取低四位，表 `129c50`
   对低值2..8为 `{1,0,2,0,0,0,3}`。低1走 slot0；高位不参与 slot。
2. `handleIncomingCallIndication` `37c688..37c694` 独立读取原 txid；`37c77c..37c7b0`
   将 handler 的 slot 与 id/number/toa/mode/seq/tx 交 TelCore，未把原 mask 作为 RSP 参数。
3. `requestSetCallIndication` `382458..38247c` 用原 txid + handler slot 新建 `8902`；
   `RfxMipcData::obtainMipcData(id,slot,tx)` `1c0814..1c0824` 经
   `slotIdChangeToMipcSlotId` `1ad9e0..1ada00` 转换，slot1..3表 `129c44={2,4,8}`，slot0为1。
4. `382480..3824ac` 填 approve/id/seq，`382540..382548` 提交 RSP。

codec 现保留 `mask`、单独输出 `sim` / `response_mask`；只准入本机低1/2，拒绝无低位、
多低位、SIM3/4。不仿照 Android fallback 把高位-only 或非法低位误选卡1。
显式 approval 的 owner 保留原 CMD 身份，而 serializer 使用转换后 mask；自动 approval 仍未启用。
新增人工路由向量源代码，仅编译、不运行套件；旧版原 mask golden 已修正。

真实顺序：ECPI0/mode20/direction1 → ANSWER0907{} → ECPI132→6→6 + 0908/result0 →
HANGUP0905{mode0,id1} → ECPI133 + 0906 → 090f{} / 0910/cause16。
4901 DUMP 仍截断（486B，声明528/536B），不能补零；信令日志不是双向声音验收。

### iOS 实跑版本原件

输入是 `.local/apple/dsc-20H392/System/Library/Frameworks/CoreTelephony.framework/Support/libCommCenterMCommandDrivers.dylib`：

- `EurekaCallFormatter::createAnswerIncomingCallReq` `0x1f1a05da8` 构造 message22/tag1。
- `EurekaGSMCallFormatter::createEndThisCallReq` `0x1f19a0048` 的**单通话**分支构造 message21/tag1。
- `tlv::writeV<vs::tlv::CallIdentifier>` `0x1f19483e0` 使用 `ldrb/strb`，wire 是 **u8**。
- CallListItem parser `0x1f1929b2c` 读七个连续字节；字段语义和 Android 状态到 iOS 的映射还未闭合，代码没有编造它们。

这些只证明 wire contract。Android 的 incoming policy、RIL 状态与音频路由不能由 MD 默认值代替。
接听与来电许可不同：approve 不是 ANSWER；挂断 CNF 也不是完整通话结束状态证明。

### 2026-11-15 卡2拨出：运行包与 RIL 原库一致

恢复的 RIL 原件：4148424 B，SHA256
`19fb2bc0c2b5532d8b4f8460779159277ebfe54930eff0be2198d9e871f5bdba`。
实际 PID1761 maps 加载 `/vendor/lib64/libmtkmipc-ril.so`。用户手动拨10086并在本机挂断：

| 时间 | 消息 / 实测字段 |
| --- | --- |
| 18:42:37.684 | 0901，mask2，tx044c，72 B；8100/string 长6、108/u8=0、101/u32=2、102/u32=1、103/u32=0、104/u32=0 |
| 18:42:37.687 | 0902，tx044c，24 B，**只有 tag0/u32 result=0，没有 call id** |
| 18:42:37.688–41.769 | 5条4901；完整的前部 scalar 中100/u32=1，Android随后已记录 IMS call 状态变化；不据残帧生成 iOS 状态 |
| 18:42:51.865 | 0905，mask2，mode100/u32=0，id101/u32=1 |
| 18:42:51.878 | 最后1条4901；0906 result0 |
| 18:42:51.894–896 | 090f空请求、0910确认；后验两 SIM 的 Android callstate 均0 |

**日志并非完整未脱敏 wire**：AT DUMP 的8100 value六个字节（包含终止符位置）都被 logger
替换成 `*`；NUL规则来自 RIL `addMipcTlvStr` / serializer，不是从星号还原号码。
六条4901 header声明512/520 B，而各 DUMP只486 B，尾部被截断。原始日志按原样保留。
离线比对明确把整个8100 value换成**人工五位数字+NUL**、归零txid，其他捕获的 tag/长度/scalar/padding
与生产 encoder 全字节一致；这不是未脱敏包重放，更不是 iOS 实跑请求。

RIL `RmmCallControlCommonRequestHandler::requestDial` `0x381410..0x3817a0`：
- `0x3814f4..0x3814fc` 限制处理后的地址 strlen <41。
- `0x381614` 8100/string；`0x381620..628` 101/u32=2；102取语音/视频/紧急分支结果。
- `0x3815f8..608` 103来自 `getDomain(bool,bool)`；104来自 `getEccPreferredDomain()`。
- `0x3816e4..6ec` 108是 CLIR/u8，不能遗漏，不能沿用MD省略字段默认值。

`getDomain` `0x387690` 的初值为 IMS-request bool ? 0 : 5；第二个 bool 为非紧急时直接返回该初值。
`onHandleRequest` `0x380ebc..0x380ed8` 传入的第三个 `requestDial` bool/w4决定 IMS 请求分支；
这不是“所有 domain0 都依赖网络状态”。`getEccPreferredDomain` `0x387a30` 仍查询运行状态；测得0
**不代表所有卡/网络/拨号类型默认0，也不证明 iOS 已有 IMS 请求上下文**。本阶段 `x9cw_dial_profile` 由调用者显式提供，
只接受 `{sim=1,address_type=2,call_type=1,domain=0,ecc_domain=0,clir=0}`；还没有生产策略选择器。
不把 QMI CallType 数值直接当 MIPC type，也不把 CNF result当通话状态或ID。

iOS20H392 `sendDialReq` `0x1f1ac1f8c` 构造 VOICE20/tag1；CallingNumber writer
`0x1f192a104` 仅复制 vector 字节，**wire没有NUL**。普通分支
`0x1f1ac2188..0x1f1ac2198` 写 tag10/u8=0。本 codec 支持1..40位十进制数字，
tag10可省略或0；CLIR、其它类型、未知/重复TLV、暂停/USSD等均明确拒绝，不静默丢弃参数。

RIL `mipcCallStateToRilType` `0x386180..0x38619c` 只将0..5原值写入 `RIL_CallState`，
其余失败；**该函数不能用来解释4901/tag109的ECPI event**。
`handleCallProgressIndicationMessage` `0x37ca80..0x37cef0` 分别取100、101、102、103、105、109、10a、
8104、810b，另经 `handleECPI` 转换；CALL_STATUS_IND 与 GET_STATUS array / iOS CallList 并非相同类型。
这些内部类型不能混用。完整状态/ID producer 已按下节补充，不要求用户再测来电。

音频日志已证明该场景实际走 `SpeechDriverOpenDSP::SpeechOn/ScpSpeechOn`，48 kHz、Aurisys
scenario7 / UL3 / DL1 / ref2；拨出后新加载 `lib_aurisys_opve.so`、`libaurisysdemo.so`、
`libspeechparser_vendor.so` 已按新 maps补抓。**这些不是可直接照抄的iOS音频配置，也不是双向声音验收。**

### 2026-11-15 拨出状态 / ID 与异步汇合（新增，编译完成，执行未验）

原始六条4901 DUMP尾部仍截断，不补零、不从日志造完整包；但同次 **RIL解析日志**保存了事件。
卡2观察顺序为 `130 → 2 → 132 → 6 → 6 → 133`：初始130的mode0/direction0，后续接通段mode20。
实测IND header的mask为低位2；仅适用于此IND，**不能推广到8901 CMD的高位mask20**。

实际 `RtmCallController::handleImsCallInfoUpdate` `0x268db0..0x269dd0`：
130经`assignImsCallId` `0x26ecc0..0x26ee58`绑定MD真实ID（`setCallId`），内部state1；
2设state2，132/6设state3，133设state8并remove。
`RfxImsCallInfo::getRilCallState` `0x199ee0` 使用 `0x125e20` 的七项表 `{2,2,3,0,1,4,5}`；
内部state1/2/3分别得到RIL DIALING2/ALERTING3/ACTIVE0，state8不在此映射表内。
所以4901/tag109不是RIL状态，也不能从0902/result0创建ID。

Apple `vs::callList::asString(CallEventStatus)` `0x1f1939f8c` 的跳表/字符串证明
ORIGINATION1、ALERTING5、CONVERSATION3、END9。
新增 reducer采用 `130→1、2→5、132/6→3、133→9`；ID保持原u32，只有1..255才能作Apple u8，超范围不截断。
仅接受已观察的MO子集；未知/外来ID/多通话隔离，END保留身份，迟到6不复活。
**完整4901 parser尚未执行重放，这个映射是实测事件+静态consumer依据，不是iOS实跑结果。**

MD `0x913257b6..0x913258e4` 的完整4901：100/101/102/103/105/107/109各u32，
8104/NUL≤128、810b/NUL≤512、可选10a/u32（源u16）；**8106虽带高bit却是固定408B binary**。
整体长度、全部字段、重复/未知tag校验后才更新状态，日志号码不作为解析输入。

RIL `requestGetCurrentCalls` `0x381354..0x381398` 发送 `0909 {}`，无selector/default TLV。
MD `0x9131d574..0x9131d86c` 的090a **无条件写 result0/u32 + count100/u32**（失败不当空列表），再写11数组：
10b ID、10c direction、10d mode、10e state、10f NUMBER_TYPE、110 toa、8111 number/NUL≤128、
112 CALL_RAT、113 TYPE完整u32、8114 binary408、115 aux；非string/binary项均u32。
**更正旧交接**：10f不是multiparty、112不是priority。RIL `385494..4a4`读113保存w23，
`3855a0/5bc`以bit0写RIL_Call.isMpty；MD `l5ucc_get_call_status_cb9138fbb4..fdf2`中`9138fd0a..4c`对IMS/CS共用TYPE构造：
低位来自session190c（call_session156c+3a0），upper flags只在bit4/8/12/16/20合并。
CLCCS parser `913a29dc..e0`和CLCC parser `913a2a80..84`均填同一session+3a0，
未过滤mode20..39。因此只在既有mode20子集上以TYPE bit0保守拒绝多方，不扩展为conference控制。
libmipc `add_array` `0x12310` / `add_idx` `0x12420` 和MD `0x9141b68a/0x9141b77c` 证明数组是
**16B len0 descriptor + 独立元素TLV**，不是packed count*u32：reserved8B=0、base/u16、count/u16，
元素tag=base+i并继承parent高bit；count0不产生descriptor。
MD快照包含IMS模式；RIL后续过滤20..39是CS消费策略。新增parser限7条，拒绝重复ID、孤儿/重叠tag、截断；**没有真实090a完整样本，未执行验证**。

空闲不凭END或零初始化猜测：仅完整owned卡2 GET_STATUS证明`count_known`；
查询TX时记录revision，事件/调用mutation穿越查询则旧CNF不更新快照，失败清空已知count。
异步staging保留原iOS txn/msg、client generation/SIM、真实MD token及epoch。
只有同token0902成功且出现下一MO epoch的真实ID才构造18B响应；Apple
`handleNormalVoiceDialResp` `0x1f1ac2a34` 读成功响应 **tag10/u8 CallIdentifier**，不是请求tag1。
若先CNF或先状态均等待汇合；本地60s guard不是猜MD定时器。超时、删client、换generation/SIM都不能让迟到结果复活host请求；私有号码buffer清零。

MD锁内检查空快照/revision/epoch及未退休CALL mutation，writer选择时再检查；过期排队dial返回本地`take=-3`，不发物理请求、不伪造CNF。
guarded lease跨CNF/超时保留到boot，阻止其它CALL mutation，当前**只容许一次staging尝试，无后续挂断/release admission**；不是最终通话容量。
writer和RX均在现有MD worker，唯一物理writer不变、SMS ACK优先不变。
这些是代码边界，**未证明MD ring在途状态与所有时序race已经闭合**。

七字节CallList的其他字段、普通VOICE bootstrap/BindSubscription44/IND注册与运行profile/audio尚未完整核实。
这是拨出state阶段的历史边界；当前明确VOICE03已触发注册/初始GET_STATUS（见上文），
但仍不发CallList、不发布VOICE09，`x9c_dial_submit`仍没有生产admission调用者。
原SMS/IMSA流不作本轮修改，也不把短信注册当语音可用。

### 2026-11-15 独立 MT state（编译完成，执行未验）

新增 `x9cw_inbound_state`，不复用或推进 MO epoch：完整 `8901` 提供真实 native ID / sequence /
原 txid、已核实 SIM 路由，只记 pending，不把“收到 CMD”当作已许可或接听。
同 native ID 的完整 `4901` / ECPI0、direction1、mode20 才进入 Apple INCOMING2；
132/6 → CONVERSATION3；133 → END9，迟到0/132/6/133不复活。
只实现实测卡2、普通IMS mode20、evoltesi0；不推广 waiting、多通话、其它卡或模式。
Apple u8 ID 必须1..255；原ID保持u32，不推断单通话count，不用MT ID完成DIAL。

RIL `2686ec..268784` 从CMD建立IMS对象，`269884..269898` 为ECPI0设置incoming5/waiting6
（取决于已存在的active记录），本次真实日志为incoming5。132/6为active3，133为end8/remove；
对应Apple事件名来自原件`1939f8c`，不是直接复制RIL内部state编号。

MD新snapshot API和iOS observer接入，仅观察，不自动approve/answer/query/注册或发布。
单个MT中未知ID、缺ECPI0、异常回退、第二MT、同sequence重用隔离该state；未证明退休规则前不清CMD tombstone。
本次新增3项人工MT回归源码并登记route项，只编译、不运行套件；37项包含原未发布服务守卫。
当前仍没有生产action admission、每请求CNF与CallList发布，VOICE09保持关闭。

## 代码与生命周期

### 2026-11-15 单普通 MO 挂断生产路径（编译阶段，未上机）

- 原 Apple `registerForQMIIndications::$_11::operator()` `1f1ad1b30..1b4c` 构造
  VOICE44/tag01，内部 subscription2 用 `cmp/cset/strb` 转为 wire1；另一个为0。
  `tlv::writeV<vs::tlv::SubscriptionType>` `192ad04..14` 只写 u8，`size=4` 包含3B TLV头，
  不能按 DMS/DSD 的 u32 来解释。VOICE 单独保存 bound/sim/generation，无默认 SIM，不借用 NAS/WMS 的 binding。
- 挂断请求仅已核实 VOICE21/tag01/u8 非零真实ID。要求该 client 已绑定 SIM2、完整 ECPI 已有
  同 native ID 的 MO epoch/mode20，随后才发 Android 原件 `0909 {}` 只读查询。
  owned `090a` 完整数组的 result0/count1、同真实ID、direction0/mode20、TYPE bit0无多方标志和 current
  ACTIVE0/ALERTING3 必须匹配 ECPI → Apple CONVERSATION3/ALERTING5。
  单通话判断改用113/TYPE bit0，不再错误地将10f/NUMBER_TYPE当isMpty；后者NONE0/SIP_URI1/NUMBER2，
  数字NUMBER2不意味着存在多方通话。IMS TYPE低位与CS共用session+3a0及CLCCS/CLCC parser填充，补充链见current节；
  不把CS RIL过滤策略照搬为MD缺少IMS列表，不宣称已验证所有多通话模式。
  初始 ECPI130/mode0、MT、未知 ID/空 count、过期快照或其它模式不准入。
- target={epoch,revision,native_id,sim} 在 MD admission 与唯一 writer 选择时各复核一次。
  允许原 DIAL lease **在其 CNF 已被消费、没有在途 CALL mutation 后** 挂断自己的真实单通话；
  不开放任意 action、空 ANSWER、多通话或新的 DIAL profile。
- MD `mipc_call_hangup_cnf` `9131d374..398` 只写 tag0/result u32。只有同 token/906/完整24B
  才回答原 VOICE21 transaction。Apple 普通 GSM `createEndThisCallReq::$_2::operator()`
  `19a1bf4..1cd8` 只检查 ResponseBase error，成功直接返回，不消费 CallIdentifier；因此不编造 response ID。
  MD 错误/未知 confirmation 返回 host error，**result0 不造 END、不发空 CallList**。
- 本地25s host guard 来自 Apple `createEndThisCallReq19a00d4` 的25000，不假装 MD/network timer。
  删除/换 generation/超时会在锁内撤掉尚未 writer-selected 的 guarded905；已经 selected/partial TX
  的原 token 必须继续消费迟到 CNF，不向新 client 回答，不自动重发。
  同client generation/transaction/真实ID的重复VOICE21在当前epoch owner内去重；request清零后仍保留
  标量身份，不先给同一transaction回复BUSY又在迟到CNF时回复success，不提交第二个query/mutation。
- 实际 END133 + action CNF 已消费后只做一次只读清理查询；完整 owned empty090a 必须仍属
  同epoch/revision，且所有 CALL mutation slot 已消费，才退休 end/DIAL lease。END 单独、挂断CNF单独、
  超时、stale/error query 均不能退休。cleanup admission 暂无空slot时只等待，不重发 mutation；
  已发送 cleanup query 失效后 bounded hold，不做循环探针。
- 这是已经接入 `x9c_request` 的具体挂断分发路径，不是新增 unused staging；但 **VOICE未发布、
  完整bootstrap/运行DIAL策略/CallList/语音音频仍缺，注册子集见上文，不能叫可用通话或最终镜像**。
  新增人工回归源代码只编译，不执行。原件/派生asm只在 `x9/out/call-end-analysis/`。

- `x9/kernel/src/bsd/dev/x9_call_wire.h`：共用、freestanding C/C++ codec。
- `x9/kernel/src/bsd/dev/x9_call_state.h`：完整progress/current parser、独立MO/MT reducer与dial join。
- `x9/kernel/src/bsd/dev/x9_mipc_calls.inc`：独立 call inbox、CMD response owner。
- `x9/kernel/src/bsd/dev/x9_mipc_bridge.inc`：严格动作白名单 / RX 分发 / 单 writer 接线。
- `x9/kernel/src/iokit/Kernel/arm/X9Calls.inc`：独立MO/MT bounded observer + 未启用的异步staging；直接VOICE仍unsupported。

CMD owner 状态：收到 → 已交付 → 显式许可排队 → 物理发送 → 结果可取 → 身份 tombstone。
成功/失败都保留 tombstone 到 boot，不为同一个原始 txid 再发响应；payload 在物理 TX 完成后清零。
队列或身份空间用尽只隔离 calls。当前最多保留八个 CMD；**这是待接入阶段的明确限制，不是最终通话容量设计**。
需要 Android 证明 call-end、seq/txid 重用及多卡路由之后才能安全退休。MD 物理失败仍遵循现有全局 offline/quarantine，不能假装继续在线。

原有 request owner 不释放未到 CNF 的已发 mutation；没有新增重试或 cancellation。
新的 response API 返回的 success 仅指 **RSP 物理 transport**，不是 iOS 来电显示 / 接听 / 音频成功。

## 离线重放与验证

按`x9/AGENTS.md`本轮只编译，**不额外运行测试脚本/套件**。下面24项结果属于上一版9483481，不能套用到新增代码。

24 项宿主测试编译**实际生产 transport / codec**，使用人工测试向量，不需要 `.local`、设备或网络。
每个用例用新进程，从干净 owner 状态开始。覆盖独立 inbox、fragmented RX、审批字段 golden bytes、原 txid/mask（旧版本假设，本次已修正）、
SMS ACK 优先、未决 mutation / late CNF、重复/变更 CMD、容量隔离、物理失败、FIFO、多通话拒绝、默认零通话 TX 与 VOICE 未发布守卫。
共享 header 同时用 C11 / C++17 编译。ASan + UBSan 的通过不等于设备通话验收。
新增6项覆盖普通拨号 golden / 40位上界 / 同缓冲转换 / 显式profile、非法/重复TLV、
严格native白名单、原writer发送→fragmented/late CNF、不重发/不释放未确认mutation、
物理失败及无ID确认。未发布VOICE守卫保持通过。

测试输出：`x9/out/call-ios-bridge/host-tests/`；本轮原件反汇编、MD symbol JSON、编译日志也在该 `x9/out/call-ios-bridge/`。
没有将原件、调试数据或反汇编入 git。

单对象验证使用 `Builder.compile_one()` 的真实 recipe，编 `X9Baseband.cpp.o` 和 `sweet_x9_gpu_dependencies.o`。
第一次后者缺 gitignored `x9_mdota.inc`；从主 checkout 复制**现有**生成头后重编，两对象成功。
没有运行 `Builder.compile()` / kernel link / boot pack / CI；没有修改 modem/NV/设备。

本轮新增9项人工回归源码（合计33项含VOICE服务表守卫）及host QMI/client/clock stub；仅编译，没有执行。
覆盖完整progress、ECPI序列、真实ID范围、数组/错误形状、查询revision fence、join、client删除/超时与pre-TX拒绝。
`Builder.compile_one()` 第二次开始真正源码编译：C对象通过，C++对象因namespace类型不一致失败；
将共享state header移到`X9Baseband.cpp`全局scope后两对象通过。最终收尾编译结果见NOTES。
host `clang++ -c` 和header C11 `-fsyntax-only -Wall -Wextra -Werror`通过，不代表人工用例执行通过。
日志/原始分析在gitignored `x9/out/call-outbound-compile/`、`x9/out/call-outbound-analysis/`，不入git。

## 下一步的必要输入

1. 状态/ID/GET_STATUS子集已实现，但完整真实wire及parser执行未验；继续核实CallList其他六字节与生产profile，不用截断DUMP填补。
2. 核实普通VOICE初始化/订阅与每请求响应；0902不能替代带真实CallIdentifier的QMI20成功。
3. 从刚抓回的实际音频实现核实 MD/SCP voice 上下行路由，再分别验收信令和双向声音。
4. 已保存一次真实来电，不再要求补来电；来电 admission / MT 状态 / 双向音频仍需接入，多通话不依据单通话样本扩展。

在这些输入闭合前不发布 VOICE，不以猜测默认值或探针 boot.img 代替依据。

## 2026-11-16 初始化前置 NAS45：真实 owner 与本地 receipt 修复

本轮沿正常 typed Client 初始化追踪，不改 loader/factory，不借 IMSA registered bool 或
VOICE03 完成初始化。以下代码与三个决定性 vtable 指针的原始字节，分别与当前挂载根盘
Cryptex 中的 dyld cache 做了窄范围比较（12 个代码 slice + 7 个 owner/code/table slice
均相符）；这证明原件来源一致，**不是设备已经执行、注册或拨号成功的日志**。

- `Client::initialize1b7c3d768` 的 MainQueue 调用
  `IPTelephonyManager::initializeFromSIM1b7cc1a90`。`1b7cc1e44` 先将真实
  clientId/personality/StackConfig 保存到 `DeferredInit`；`1b7cc1e48` 从
  StackConfig+2b4 取实例，`1b7cc1e68` 进入 `initializeNasClient1b7cbee3c`。
  后者使用真实 NasDeviceClient+108、IMSClientManager queue 和 IPManager+68 的
  IMSNasClientDelegate alias，不凭空创建 SipStack 或向空 delegate 回调。
- QMI backend `QMINasClient::initialize1b7d6021c` 将 owned client name 和 subscription
  存入自己的 +c8 map。**event2** 的 capture address point 为 `1d90c99b8`，
  +30 指向 `1b7d64b60`；**event1** 为 `1d90c9b30` / `1b7d65554`，后者是 stopped
  路径，不能因文件名或注册事件混为 ready producer。
- event2 构造 **NAS45 BindSubscription**：`1b7d64c30` 的 message 为45；
  `1b7d64d58..60` 将内部 SubscriptionType2 转为 tag01/**u8 wire1**，另一个为0，
  不是直接发送内部枚举，也不是 DMS/DSD 的 u32。`1b7d64d9c` 设置原 SendProxy
  guard 25000ms。
- Bind consumer `1b7d65084` 只有 Response error0 才继续；失败只记录错误，不完成初始化。
  成功 `1b7d65258` 调 `startListening`，`1b7d65284..2ac` 仅对 +c8 map 中 subscription
  匹配的 owned name 调真实 delegate+10，再擦已处理的条目。IPManager ctor 安装的 NAS
  alias address point 为 `1d90c08b0`，+10 指向 thunk `1b7cc0ef0`，调整 -68 后进入
  `nasSystemInitialized1b7cbeffc`。
- `nasSystemInitialized` 先查同 clientId 的 +138 DeferredInit；没有 pending 仅日志，
  不造栈。普通分支 `1b7cbf1d4..1e0` 将 node+68 的 **owned StackConfig** 送进
  `_initializeFromSIM1b7cbf240`，再擦 deferred 项。该函数设置真实 delegates 并进入
  `BambiClient::initializeStack1b7da81e0`；Bambi 必须先找到真实 SipStack，才调用
  `SipStack::initialize1b7d00710` 和 SDP template 初始化。注册仍使用真实 pending
  SipRegistrationConfig → `BambiClient::startRegistration1b7da884c` → AP
  `SipStack::startRegistration1b7d025e0`，不是 MD 代发或常量 Registered。

可达生产修复仅限原 `x9q_send` NAS45 分支：旧代码先改 `c.sim_slot/next` 并清 registration
cache，再使用不能证明 host queue receipt 的普通 `p.send`。现在 NAS03/45 共用现有 bounded
32-entry FIFO 和 per-client completed cache，45 保留原请求、txn、旧 SIM、目标 SIM、本地和真实
QMUX generation；只有固定 Result45 **实际入 host queue** 后才换路由并重启后续 sample。
OOM/full 只保留并重试本地回复，不向 MD 重发任何命令，也不取消/重定向已有 query token。
新换路由 bind 遇到已有未终态 NAS03/NetworkName owner 为 BUSY；换路由 receipt 等待期间
不准入新 NAS44/03/45 旧路由 owner，避免其被随后的 rebind 提前置为失活。

03/45 在通用 TLV 校验前仅接管同实际 client generation/txn 的既有 owner：先匹配 txn 再比较
完整原请求，改变 message/body/length 的同 txn 重用仍保留首个 owner/result，不额外回复或绑定。
completed 的精确重复只 replay 首结果，不 rebind、不重置 flags。45 的本地25s guard在 admission
开始计时，并在 single-owner → bb mutex、实际 frame transfer 前复核；过期后仅保留同 txn 的
NOT_READY 错误等待 receipt，迟到 success 不再改路由。已 receipt 的 success replay 不另计 guard。
NAS03 未新增超时；DEL/真实 QMUX allocation 更换仍清理旧 owner，DMS54/DSD27 原路径未改。

这是 **NAS bind 初始化前置的本地路由/回复修复**，不是 NAS 全部 bootstrap、MD IMS 注册、完整
用户态 provider、Call admission 或 media-ready。普通 `Call::dial` 仍是前述 AP manager 的直接调用，
`x9c_dial_submit` 仍无生产 caller，VOICE09 仍不公告；不能把这次45成功当成“最终修好通话”。
没有新增 SMS ready/ACK、设备操作、测试套件、人工向量或探针镜像。

本轮实际生产 recipe `Builder.compile_one(X9Baseband.cpp.o)` exit0（34 条已有警告）；没有 link、
boot pack 或上机验收。原件窄切片、cache 对比 JSON、`compile-bootstrap.py`、recipe result 和编译日志
在 worktree 的 gitignored `x9/out/call-client-bootstrap/`，不提交 Apple/设备原件。
