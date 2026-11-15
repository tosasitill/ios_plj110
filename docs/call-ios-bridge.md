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
| handover 通知 | 严格 MD4902/EVENT3 → Android SRVCC → Apple54/tag1/u8；订阅cursor/generation/SIM隔离、先Result后有序发布；未设备验收，未迁移handover后的ID/RAT/profile |
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

生产`4b2619b`仅修注册publication：原Result03和启用期间保留的最多8条54在锁外预分配，
`x9_md_mipc_call_publish(floor,callback,context)`持同一MD锁检查online/CALL poison/RX floor，
受信callback再检查原client/generation、control-ready及原25s deadline，
以MD→bb mutex顺序覆盖**整批真实host queue准入**。bb锁内验证非release_pending的真实VOICE
client、service/node/route及整批容量，只header/copy/入队，不malloc、不MD重入；两mutex仍可等待。
失败不转任何buffer所有权，锁外擦除释放；queue/OOM只保留本地waiter/events重试到原deadline，
不重发MD请求。只有整批成功才grant并清buffer，Result先于START/COMPLETE。
成功后MD再次offline由原poll撤host订阅，不把这笔历史成功当永久voice-ready。

新增6项人工源码向量只编译未执行；host/header及真实recipe两对象均exit0。
此项不改已经启用后的普通54发送路径，**尚未闭合该路径的queue-full持久保留/重试**，
不发CallList/VOICE09，不改变SMS/CALL mutation、真实ID/profile、注册CNF或MD物理writer。
统一kernel结果见NOTES；仍无设备注册/通话/最终镜像验收。

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
尚未实现号码格式/TOA转换、progress到通话身份的号码合并生命周期或CallList46生产编码/发布。
没有新增查询/服务公告/默认字段，不能仅empty encoder冒充完成。新增两个人工owner向量仅编译，未执行。

offset6追加原件排除：`192a770→192b150/192b194`只是接收vector resize/append；
`192a794→1929b2c`必将wire+6覆盖到item+6（`1929be0/be4`），默认构造不能作为来源。
相邻`1929bfc`为CallConfigItem，`1929c3c..54`把C++ string长度写wire+6后复制wire+7，
是变长结构而非7B CallList serializer。当前匹配host原件仅有CallList parse/resize/copy，
未发现其writer；QMIParserDynamic已有metadata也无vs/ALS定义。ALS只是未证假设，不按标准字段名填0。

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
