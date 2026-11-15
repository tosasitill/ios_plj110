# 通话 iOS 桥：已核实子集与启用边界

## 目前交付什么

这是**信令桥基础，不是已可使用的通话服务**。现有 WMS/IMSA 短信实现保持独立。

| 部分 | 实现 / 验证 |
| --- | --- |
| iOS 请求 codec | VOICE20 普通拨号有界解码，21 挂断、22 接听严格 tag1/u8；动作要求显式已验证的 route / 单通话映射 |
| Android 请求形状 | 新增本机卡2实测 `0901` 的六个 TLV；接听 `0907 {}`；挂断 `0905 {MODE100/u32=0, ID101/u32}` |
| 拨出状态生产者 | 完整 `4901` 的真实 ID / ECPI reducer；已实现、单对象可编译；未有完整真机 wire 重放或 iOS 运行验收 |
| 空快照与异步确认 | 完整 owned `090a` 数组解析；`0902 + 新 MO epoch` 汇合生成 VOICE20/tag10/u8；只接 staging API，没有生产 admission 调用者 |
| 来电 CMD | `8901` 的字段、长度和唯一 tag 验证；号码只留在有界内存，不打印 |
| 来电许可 RSP | `8902` 的正确 approve/call-id 字段；显式调用才构造，保留原 txid；按已核实 RIL slot 转换生成 RSP mask |
| 物理 transport | 独立 call inbox / CMD owner，复用原有唯一 MD tick writer；SMS ACK 保持优先 |
| 超时 / 重复 / 失败 | 无自动重发；重复 outstanding CMD 去重；内容变化及未知退休规则隔离 calls；不清空 mutation owner |
| 默认启动 | **不发布 QMI VOICE09、不注册 CALL CMD/IND、不发通话请求、不自动 approve，无强制开启 bootarg** |

`X9Calls.inc` 当前只明确拒绝意外直达 VOICE 请求。它不会成功应答初始化并伪装服务已可用。
`x9cw_action()` 仍只生成报文；拨出 ID/state producer 已补，但动作 admission / CNF / CallList 发布未接，所以生产路径不会调用它提交动作。
`x9cw_dial()` 同样仅编码：必须显式提供已验证的卡2 profile、无现存通话；当前没有 profile provider。
原 transport 已允许这一严格 DIAL 子集；上一版宿主测试闭合发送/CNF。新增异步 join / 空快照 / 发送前 guard 目前只有编译结果，没有执行验证，也没有 iOS 自动拨号调用者。

## 原件证据（2026-11-15）

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
10b ID、10c direction、10d mode、10e state、10f multiparty、110 toa、8111 number/NUL≤128、
112 priority、113 flags、8114 binary408、115 aux；非string/binary项均u32。
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
因此不发CallList IND、不注册CALL、不启动GET_STATUS、不发布VOICE09；新增`x9c_dial_submit`没有生产调用者。
原SMS/IMSA流不作本轮修改，也不把短信注册当语音可用。

## 代码与生命周期

- `x9/kernel/src/bsd/dev/x9_call_wire.h`：共用、freestanding C/C++ codec。
- `x9/kernel/src/bsd/dev/x9_call_state.h`：完整progress/current parser、MO reducer与dial join。
- `x9/kernel/src/bsd/dev/x9_mipc_calls.inc`：独立 call inbox、CMD response owner。
- `x9/kernel/src/bsd/dev/x9_mipc_bridge.inc`：严格动作白名单 / RX 分发 / 单 writer 接线。
- `x9/kernel/src/iokit/Kernel/arm/X9Calls.inc`：bounded observer + 未启用的异步staging；直接VOICE仍unsupported。

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
