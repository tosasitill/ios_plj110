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

### 真实 per-personality 路由与强 owner getter（2026-11-16）

这是新的原件 ABI 前置，不是已接线 provider。当前根盘 Cryptex cache 的九个
选定 text 范围及九个选定 export 已读取，text与对应 extracted dylib相符：
`x9/out/call-personality-context/{original-context-proof.json,original-context-proof-v2.json}`。
不重复 CallList第七byte的旧排除，也不凭这些 helper公告09或指定默认SIM。

- 真导出 `PersonalityIDToSimSlot1f0323a38` 接 `x0` 强 Registry pair 的借用地址、
  `x1` 实际 PersonalityID；要求 initialized Registry，经其 map取强 PersonalityShop，
  shop virtual+0按该ID返回强 PersonalityInfo。缺shop/record返回0。
  record `+31/u8!=1` 时读真实 `+34/u32`；`+31==1` 时还取Registry配置强owner，
  配置 `+111/u8`为false则**返回slot1**，为true才读record+34。
  因此 helper返回1不单独证明物理卡1，不能无条件把它用于native CALL route。
- `sd::isSimPersonality1f03390cc` 对同一种shop查询，只接受非空record且
  `+31==0`；缺对象返回false。两个独立查询不构成同一record/revision的原子快照。
- `PersonalitySpecificImpl::personalityInfo1f032651c` 使用 `x8` sret，
  复制真实 `this+8/+10` shared pair，并递增非空control的强引用。
  **这是可复用的强owner producer，不是返回裸指针或借用pair。**
  对同一qualified具体对象，`simSlot1f031f1fc`读取该owned record+34，
  不执行前述Registry单slot策略fallback。仍须真实对象adjustment、原调用队列与
  rebind/revocation fence，不能只从临时Call+28的clientId派生身份。
- 完整构造 **C1 `1f0392bbc`** 的shared pair形参在x1；base构造
  **C2 `1f0392b84`** 的x1是construction-table/VTT输入，shared pair在x2。
  两者都强own pair；不能因demangled名称相同就按C1的两参数ABI调用C2。
  `personalityId1f0392cbc`使用x8 sret、把record+18交原helper；本轮未重新实名该helper，
  更不将personalityId等同call UUID/clientId。
- `subscriber::simSlotAsSubType1f1ab81f4` 原值转换为
  `uint32_t(slot-1)<3 ? slot : 0`；`simSlotAsInstance1f0322cc8` 则
  slot1→0、slot2→1、其余→-1。先前原VOICE44 subscription2→wire1是第三层转换。
  native桥仍只支持已限定的sim0/1，不接SIM3或无效/缺失fallback。

下一接线应取**实际per-personality强owner**并证明其具体caller、queue及身份变更fence，
而不是固定personality枚举、Call指针/clientId或盲用helper返回值。原broker的State/QMUX
generation仍是独立层；上述源码资格不提供native epoch、MD receipt或media readiness。
此项未改生产源码、未编译/测试/操作设备，selected CLIR jar及最终短信/通话验收仍待输入。

### 原 broker SendProxy、消息与回调的真实所有权（2026-11-16）

本轮修正旧 `call-native-ingress/native-ingress-contract.json` 的
`callback fields@+18/+20` 描述：VOICE wrapper `1f1a2482c` 持有的
**0x28-byte inner qmi SendProxy 的 +18 不是 callback**。
原 `Client::send19ecd2294` 只取真实State并进入 `State::send19ecd2c7c`：

| inner offset | 原 ABI / 生存期 |
| --- | --- |
| +0 | 借用稳定地址的 `qmi::Client*`；wrapper同步dispatch期间不能失效 |
| +8 | owned `QMIServiceMsg*`；不是裸7-byte QMI SDU |
| +10 | u32 timeout；VOICE wrapper明确初始化25000ms |
| +18 | 可空的借用 `uint16_t*` transaction输出地址；`19ecd2ed4/2ed8`同步写入u16 |
| +20 | response block；`19ecd2db8/2dc0`读取并调用真实 `_Block_copy` |

`19ecd2f84..f94` 构造40B async send capture：State、owned XPC dict、retained
message、copied block，以及**u16 transaction值**。外围SharedSynchronizable wrapper
保State强owner；消费者 `19ecd3998` 再调用 `send_sync19eccfd8c`，不继续借用原
Client地址或transaction输出地址。同步txn输出只是本地分配值，不是MD接受或终态。
不能将+18当block，也不能因原临时wrapper销毁就退休
本native epoch。传输仍复用既有 `com.apple.commcenter.atcs.xpc` / QMUX / ABP，
没有新增raw reader或callable backend。

原export trie另证 `QMIServiceMsg::create(const void*,unsigned short)19ecff70c`
与 `createRequest(const void*,unsigned short)19ecff82c`：两者将借用输入交给
`QMIServiceMsg19ecce120`，读取**u16 message-id + u16 TLV字节数的4B header**，
再为每个tag/u8、length/u16、value独立分配/深复制。输入不是带flags/transaction的7B
QMI SDU，更不是QMUX frame。原constructor断言总长度并在bool=true时另解析Result；
`createRequest`的bool=false不解析response Result。构造与回调有真实intrusive message
owner，不能用raw结构memcpy替代它，也不能把callback的 `const QMIServiceMsg&`
裸引用保留到异步工作以后。

`postResponseToClient_sync19ecd219c` 在State+78 callback queue上安排40B capture：
retained response、copied block、State+100/+108的weak handle pair和
**State+81 configured-handle flag快照**。消费者 `19ecd3b58` 中，flag非零时必须
weak-control `lock()`成功且weak identity非空才invoke；flag为零仍可invoke，不能
泛称“没有weak handle就无回调”。`setClientHandle19ece28e8` 只更新State中的pair/flag，
不会改已入队capture；`stop19ece29c8` 是State+10配置队列上的async请求，其实际消费者
`19ece5514` 清的是State+80 started bit，不是旧capture的flag。+10/+78是不同配置槽，
不足以证明运行时queue身份必不同，也未证明这里存在顺序barrier。**stop/release或weak-lock
本身都不是callback queue fence**；晚回调仍需由真实context/generation/UUID/epoch owner拒绝。

`handleResponse_sync19ecd15c0` 按该State pending vector里的transaction匹配并移除owner，
原raw响应再交constructor、local error走原错误消息，最终发布copied block。这个匹配不
赋予UUID/native epoch归属，也不替代派生backend的expected message-id/route核对或真实
typed terminal delegate。这里仅闭合可复用的原传输ABI，不造新的成功回执/ACTIVE。

证据 `x9/out/call-context-backend/`：`broker-owned-contract.json`、六组原件汇编、
`qualify-broker-original.py` / `broker-current-cache-qualification.json`，22处本轮原件
范围均匹配当前root Cryptex cache；`read-owner-islands.py` 将四个原cache支岛实际解至
`_Block_copy/_Block_release/__shared_weak_count::lock/__release_weak`，不凭nearest-symbol
注释定名。这不是产物复检、provider执行、编译或通话验收；VOICE09仍关闭，生产DIAL
caller/profile、完整typed backend及双向专用媒体仍缺。

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

#### 通话 AFE descriptor / DSP trigger owner：原 HAL 决定门与参考实现

本轮只读原件及已有拨出日志。**固定输入首先缺同版 `audio_dsp` 固件**：已抓 properties 的
OTA 分区列表含此名，但现有 `.local/x9` 分区备份、modem 原件与已抓 vendor 文件中没有该
payload，也没有同版 audio DSP 模块。不能拿 `scp_b`、SmartPA A-core 固件或开源默认路由替代。
真实 task-info[scene0] 的8B、独立 `ADSP_USIP_PHONECALL` owner 与 DSP consumer 仍缺。

原 HAL `AudioDspPhoneCall::setPhoneCallDspShareMem25f380` 将 bool&1 依次写入实际字符串
`adsp_calldl_sharemem_scenario`（VA100c7c）、`adsp_callul_sharemem_scenario`（VA125bc8）；
第一项成功才写第二项，两项成功才返回0。下面仅是由这些**同名 controls** 锚定的 MT6993
reference 实现（HEAD `4d505a4292dab3176a45fec66dc85debc362e24c`），**不是同版驱动或固件执行证明**：

- `mt6993-afe-pcm.c:1428..1550/2134..2141` 按实际 task attrs 将 CALLDL 的 MEMDL、CALLUL 的
  MEMUL/MEMREF（只有 memif>=0）设为 `use_adsp_share_mem`，没有固定 DL/UL/REF 编号。
  substream 未关便从1解除为0会标记 `err_close_order` 并触发 AEE；setter 返回0也不能证明
  实际 attrs 选到 memif，更不是 DSP ready。不能只释放通用 audio ticket 便退休此 owner。
- `mtk-afe-fe-dai.c:418..440` 的共享内存分支调用 `mtk_adsp_allocate_mem`；后者
  `mtk-dsp-mem-control.c:1077..1137/1434..1468` 从 `AUDIO_DSP_AFE_SHARE_MEM_ID` gen_pool
  分配真实 `snd_dma_buffer`，pool 源于 `ADSP_AUDIO_COMMON_MEM_ID` reserve memory，
  **不是 USIP_ID24 或 ADD0/ADD1 注册的 AP↔DSP 两个65536B region**。
- AFE `:475..546` 设置真实 dma_addr/bytes/channel/rate/format 后调用 `afe_pcm_ipi_to_dsp`。
  `mtk-dsp-mem-control.c:1369..1400` 根据动态 memif 与 default/runtime/ref_runtime 选择 task；
  `mtk-dsp-common.c:495..578` 再检查 task、runtime/default，将物理 ring bridge、PCM attrs、
  memif/IRQ 写进该 task 的 `audio_afepcm_buf`，复制至同 owner 的 `msg_atod_share_buf`。
  HWPARAM201 / PREPARE203 发送 PAYLOAD+NEED_ACK，payload 是 **physical descriptor address**，
  不是 PCM samples；方向 DL1/UL2/REF4 也须走真实 REF_RUNTIME 判定。scene 由 CALLDL→PHONE_CALL_SUB、
  CALLUL→PHONE_CALL 映射，不猜实际 core。发送还受 `is_audio_task_dsp_ready`/default 门约束。
  外层 AFE 忽略发送返回值，内层忽略 attr 构造返回值，不能以 hwparams 成功代替消费或 ACK。
- **实际 trigger 决定门**：`mtk-afe-fe-dai.c:919..935` 对 `use_adsp_share_mem` 返回 false；
  `mt6993-afe-pcm.c:216/303` 因而跳过主机 memif/IRQ 启停，其 owner 在 DSP。
  现有 host-only AFE 播放/录音、SmartPA coreA 或两条 REG_DMA 环不能冒充此通话硬件 provider。

还必须区分真实本次拨出路线：MO 原日志 `:624..640` 实际执行 `modemPcmOpen` 和两次
`Hostless_Speech` open（2ch/48000/1024×2/format0）。原 `modemPcmOpen28220c/282214`
检查 ADSP option 与 frame-base scheduling；frame-base 真分支会跳2826f0，绕过 Hostless open。
所以不能凭 `AudioDspPhoneCall::adspPcmOpen` 符号存在便称本次走了上面的 descriptor 链。
reference Hostless 图连接 PCM_0/1 与 ADDA/I2SOUT，仅是候选图，未证本次所选端点。

原 `SpeechExtMemUSIP::openShareMemory1bc970` 的另一 ioctl **c00844f0** 读 size，再映射
AP parameter region。真实 maps 的 `/dev/usip` rw-s 长度30000，不是 PHONECALL8000。
`formatShareMemory1bcc70` / `writeSphParamToShareMemory1bd050→1bd390` 将参数放在
APmap+4000 的2bfe0区间，保存/发布 `vendor.audiohal.speech.shm_usip` 与
`vendor.audiohal.speech.shm_widx`；当前 MO `configSpeechInfo` 的 usip param size/index均0，
既不证明参数写入，也不证明通话 PCM。`AudioDspCallFinal::processDmaMsg238e80` 仅消费101
dump与严格64B的103 dump metadata，不是 AD50/MD3D00 或物理 PCM consumer。

完整窄切片、原日志行号、静态复提脚本与分层 JSON 在 gitignored
`x9/out/call-voice-dsp-owner/owned-call-descriptor-boundary.json` 及同目录；未改生产源码、
未编译、未运行测试/人工向量、未操作手机。此证据补齐独立 buffer/trigger 决定门，
**仍不证明同版 DSP receipt、USIP_PHONECALL 消费、iOS Baseband Voice I/O 或双向声音成功**。

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
初始实现只接受 `{sim=1,address_type=2,call_type=1,domain=0,ecc_domain=0,clir=0}`；
本文件末节记录后续已明确raw CLIR1/2的codec扩展，仍没有生产策略选择器。
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

## 2026-11-16 通话 action 参数、深所有权与原 AP 漏路隔离

2026-11-16；源码基线 `9db480c4`。本文是当前 20H392 原件的静态 ABI 契约，
**不是已安装的 provider，也不证明拨号、音频或注册成功**。未操作设备、出探针、
构建产物或执行测试。本轮没有新增空 loader / unused API。

### 1. 来源资格与纠正

- 当前挂载 `CommCenter` UUID：`20b0ce28-18a1-3eea-aa2d-ef53e33c039a`。
- 当前 `IPTelephony` UUID：`8b3ce7d6-41f4-3eed-bbcc-7bf1dff85575`。
- 原 Cryptex dyld cache 的 exports trie 与八处新切片均实际读取；八处切片与
  当前 extracted dylib 相符。这只是原件来源资格，不是运行成功。
- 真 `Call` vtable address point 是 `0x1d90b7dc8`，不是导出 vtable 符号起点。
  **AP `+0x10` 是 `ring() const`；拨号在 `+0x80`。** 旧表未回写覆盖。
- 以下地址都是 unslid 静态 VA。执行必须绑定实际加载的同 UUID/ABI/代码，
  并使用 dyld fixup 后的运行时地址；磁盘 encoded pointer 不能直接调用。
- C++ mangled method 名不编码返回类型；原 caller 没有消费同步 status。
  不能据此添加 `errno`/成功返回，也不能把 action 返回当 modem acceptance、
  registered 或 connected。

### 2. 完整 action 参数表

表中的 `string`/`vector` 是原 userland `std::__1` libc++ ABI，不是 C 指针。
所有槽相对**真 address point**，`x0` 均为本次实际 `Call*`。

| 槽 | 真方法 / 实现 | 其余物理输入 | 原异步 owned 参数 / AP 路径 |
| --- | --- | --- | --- |
| `+10` | `ring` `1b7c42d90` | 无 | UUID；block `1b7c42e5c` → `ringCall` |
| `+18` | `answer` `1b7c436c0` | `x1 string const&` | UUID、参数 string 深拷贝；block `1b7c43800` → manager virtual `+20` |
| `+20` | `hangup` `1b7c434b4` | `x1 CallEndReason const&` | 仅读首 u32 后转换 reason string，UUID/string 深拷贝；block `1b7c43628` → manager virtual `+28` |
| `+28` | `setTextMediaSessionMode` `1b7c432fc` | `w1 u32 enum` | 写 `Call+40`，另 owned UUID/u32 → block `1b7c433d4` → AP setter |
| `+30` | `setRelayCall` `1b7c43898` | `w1 bool` | 写 `Call+44`，另 owned UUID/bool → block `1b7c43970` → `setRelayMode` |
| `+38` | `setCallIdBlocking` `1b7c42d84` | `w1 bool,w2 bool` | 只写 `Call+09=w1,Call+08=w2`，没有 AP work |
| `+40` | `playDtmf` `1b7c40bec` | `w1 unsigned char` | UUID/u8；block `1b7c40cc0` → byte overload |
| `+48` | `playDtmf` `1b7c40d50` | `x1 vector<unsigned char> const&` | UUID 与 vector **内容**深拷贝；block `1b7c40e90` → vector overload |
| `+50` | `stopDtmf` `1b7c40ff8` | 无 | UUID；block `1b7c410c4` → `stopDtmf` |
| `+58` | `reportBrandedCallingImageFetchingFailure` `1b7c41150` | 无 | UUID；block `1b7c4121c` → 同名 manager 方法 |
| `+60` | `mute` `1b7c43ac4` | `w1 bool` | 原件只有 `ret`，不能声称已改变媒体 |
| `+68` | `refresh` `1b7c42ee8` | `w1 bool` | UUID/bool；block `1b7c42fbc` → `refreshCall` |
| `+70` | `hold` `1b7c4304c` | 无 | UUID；block `1b7c43118` → `deactivateCall` |
| `+78` | `resume` `1b7c431a4` | 无 | UUID；block `1b7c43270` → `activateCall` |
| `+80` | `dial` `1b7c41468` | `x1 calledAddress string const&`; `x2 CallConfig const&`; `x3 additional string const&`; `w4 bool` | calledAddress + owning options；block `1b7c41878` → `initializeNonEmergencyCall` |
| `+88` | `pull` `1b7c429c8` | `x1 string const&`; `x2 CallConfig const&` | owning options；block `1b7c42cec` → `initializePullCall` |

普通 model 的 `+80` 调用处 `1007cb970` 中，`x3` 是空 string，`w4` 来自原
`CallInfo+57` 的 `Is Listed As Emergency`；不能把它猜成 Android 域值。
仍必须保留完整五参数 ABI，不用普通 caller 当前空值删参数。

DTMF vector 原 ctor `1b7ad04ec` 分别深拷贝到 local、block `+38`，以及执行
block 后的 manager 按值参数。只复制 vector 的 24B 指针头会产生悬空/双释放。

`hangup` 原 `1d90b8990` 表是两级 pointer：1 → `DoNotDisturb`，
2 → `CallWaitingDisabled`，3 → `SRVCC`，4 → `DeferToSecondary`，
5 → `Blocked`；其余 → `UserTriggered`。这是 **Apple 原 reason string**，
尚不是 Android/MD cause 的可互换值。

### 3. `CallConfig` 完整 copy / destructor 已闭合

| 项目 | 原 ABI |
| --- | --- |
| 存储 extent / 对齐 | `0xf8` / 8B；真实 caller 的 `sp+3c8` 与相邻 `sp+4c0`，最大 optional engaged 字节 `+f0` |
| 完整 copy | CommCenter 私有 `100dd7560(dst,src)` |
| 完整 destructor | CommCenter 私有 `1007cd508(this)`，完整函数至 `1007cd5a0` |
| optional string copy helper | `10001f234`；storage string 24B，`+18` engaged |

完整字段：`+00 u8`、`+08 owned string`、`+20 u8`、`+28 owned string`、
`+40..6f` 48B 原 direct-copy 区、`+70/+88/+a0/+b8` 四个 owned strings、
`+d0 u32`、`+d8 optional<owned string>`（engaged 在 `+f0`）。

**不能 raw memcpy 全结构，不能丢普通 dial 暂未读取的 `+28/+d0`。**
正常 cleanup 按 optional、`+b8/+a0/+88/+70/+28/+08` 逆序释放 owning strings；
不对 48B header 单独执行析构。

决定证据：`100dcfbbc` 将 `sp+4e0` 深拷贝到 `sp+3c8`；
`100dcfbdc..100dcfc50` 在正常 action 返回后逐字段 cleanup；
异常路径 `100dcff0c..100dcff10` 对**同一 destination**调用 `1007cd508`。
这比根据附近函数名猜析构更直接。

排除候选：`100dd77d8` 是大 CallInfo/package 析构；`100dd7718` 是大 nested
package；`100dd76b8` 仅 subregion；`100dd73f8` 是更大 string package。
scratch 旧文件名不构成类型证据，以上均不能作为 whole-config destructor。

owner 必须在完整 copy 成功后发布。原 copy 的异常路径已经清理构造成功的
子域，不得对失败的 partial construction 再运行 whole destructor。

### 4. `IMSCallOptions` 不是 `0x13a0` POD

- 完整 owning copy：IPTelephony 私有 local `1b7c41904(dst,src)`。
- 完整 destructor：私有 local `1b7b432d8(this)`。
- `+08` 是 `ImsUuid` vptr、`+10` 是 owning UUID string；
  `+28` 是 `0x160` BambiCallOptions（copy `1b7b42aa4`、dtor `1b7b42cc4`）；
  `+188` 是 `0x1200` BambiEmergencyCallOptions（copy `1b7b41970`），
  含 owning strings、SipRegistrationConfig、SipStackConfig、SipUri；
  `+1388` 是 owning clientId string。
- 普通 dial 从 local 深拷贝到 block `+38`；block `1b7c418bc` 再深拷贝
  给按值 manager 参数。pull 同样深拷贝到 block `+20`，执行时再 copy。
  不能把 block payload 当 `memcpy(0x13a0)`。
- **LocationInfo 的 48B 仅是 header**：Bambi `+90..bf` direct copy，
  后面 `+c0/+d8/+f0/+108` 是四个深拷贝 string。完整 LocationInfo extent
  是 `0x90`，dtor `1b7ab56e4` 读取其 ownbase `+30/+48/+60/+78`。
  异常路径调完整 LocationInfo dtor，正常 Bambi dtor逐 string inline。
  原行为没有把这四个 owner 浅拷贝。

原 cache exports trie 的 `IMSCallOptions`、`BambiCallOptions`、
`BambiEmergencyCallOptions`、`ims::CallConfig` ctor/dtor 限定前缀没有导出；
extracted dylib `nm` 的 options ctor/dtor 是 local `t`，两证据一致。
**不能伪造 external declaration 并以为可链接**。如生产 userland 要使用这些
私有 helper，必须有实际 loaded-image 资格与 function-pointer 接入路径。

已可复用的导出包括完整 `Call::dial/pull`、Call 构造/析构、text/relay/CLIR、
answer/hangup/DTMF/stop/branded、ring/refresh/hold/resume；可导入不代表应该
调用原 AP action。`mute` 在本次限定 exports 中没有可导入项且原槽 no-op。

### 5. 最窄生产接入边界与剩余依赖

可以复用的输入边界是 **typed `Call` action 本身**，无需先构造 AP options。
每次先深 own 完整 typed 参数，以及临时 Call 的 `+10 UUID`、`+28 clientId`、
`+08/+09 CLIR`、`+40 text`、`+44 relay`。不要捕获裸 `Call*`、借用 string/
config/vector pointer，也不要将 userland C++ ownership 当 kernel request 头。
原临时 action handle 的析构不是 MD terminal event；已有生命周期证据不重扫。

text/relay 要保留原字段 mutation，同时隔离其 MainQueue AP 发送；CLIR 是
唯一原 field-only setter。所有其它 AP actions（含 pull、DTMF、hold/resume）
都要隔离，不能 MD admission 后又 fallback 到 original，产生双发。
**不能调用原 dial/pull “仅生成 options”——它们会排入 AP queue。**

目前仍缺下列真实生产边，故本轮没有可诚实落地的完整 action/provider：

1. 当前 typed factory/action 到实际 userland adapter、私有 ABI lifecycle 的加载/安装路径。
2. logical VOICE09 client/context、broker admission，以及 UUID/clientId 对 MD epoch/subscription 的真实关联。
3. Android 证据限定的每 action 策略、cause、CS/IMS/handover producer；Apple reason string 不能替代 MD enum。
4. 请求 owned 生命周期经现有 broker，到权威 MD terminal receipt，再到匹配的 typed delegate/当前 Call owner。
5. 通话专用物理音频、最终 UI/对端端到端验收。本契约不证明这两者。

### 6. 本地取证与重放

本轮 `x9/out/call-action-adapter/`（gitignored，原件 bytes 不入 git）：

- `typed-action-owned-contract.json`：全槽参数/owned/AP 隔离及缺边；
- `callconfig-real-dtor.asm`、`config-copy-callers.asm`、`callconfig-dtor-xrefs.asm`；
- `options-deep-copy-dtors.asm`、`locationinfo-dtor.asm`、`end-reason-table.json`；
- `read-original-options.py`、`options-original-qualified.json`、`options-original-exports.txt`、
  `options-local-symbols.txt`、`original-*.bin`：原 cache exports 与八处相符切片；
- `write-owned-contract.py`、`root-query.py`、`macho-read.py`。

原件仍存在时，在本功能树执行 `python3 x9/out/call-action-adapter/read-original-options.py`
可重放限定 export/bytes 读取；`python3 x9/out/call-action-adapter/root-query.py
f:0x1007cd508 f:0x100dcfbbc` 可重放 config 析构与同 destination cleanup。
这都是离线文件读取，不触手机。

复用既有 `x9/out/call-typed-actions/`、`call-typed-dial-context/` 与
`call-provider-install/` 的已归档证据，不覆盖旧结论。本轮正式编译、CI、启动结果：
均无；没有通话短信最终修复镜像。NOTES、合入和取证归档由 root 统一记录。

## 拨号 profile 的 CARD_TYPE 与 AP72→MCL72 owned 生命周期（2026-11-16）

本节补上匹配设备原 `libmtkmipc-ril.so` 的输入/ownership 证据，不是新生产桥、
完整初始化或通话修复。原件 SHA256 为
`19fb2bc0c2b5532d8b4f8460779159277ebfe54930eff0be2198d9e871f5bdba`；
34 组固定 ELF 切片、原始字节 hash、编号反汇编与 contract JSON 位于 gitignored
`x9/out/call-profile-input/`。不拿其他 Android 版本的常量代替匹配设备 producer。

**MCL1 是卡类型，不是 NR capability。** 默认表 initializer `1b4384/1b4394/
1b43cc..1b43e8` 确认 key1 名称 literal `0xf136c="RFX_STATUS_KEY_CARD_TYPE"`、
variant tag2、初始 int **-1**。`RmmSimCommUrcHandler::onHandleUrc36c458..4c8`
读取原 owned MIPC data 的 MID，并将 **4512** 分派给 `handleCardType36e1c0`。
该 handler 从 original message+40 的 data 读 uint8 tags100/101/102：

| 已证输入 | 实际 CARD_TYPE 贡献 | 原件位置 |
| --- | --- | --- |
| tag100=0 / 1 | SIM `0x1` / USIM `0x2` | `36e390/36e3b8`，原 literal `USIM` / `SIM` |
| tag101=0 / 1 | RUIM `0x8` / CSIM `0x4` | `36e40c/36e434`，原 literal `RUIM` / `CSIM` |
| tag101=3 | 两个 CDMA bits `0xc` | `36e628` |
| tag102=1 | OR `0x10`，不猜其语义名称 | `36e45c..468` |

组合在 `36e458` OR 后，经 `36e4e8..510` 的 per-slot MCL manager 将 **真实计算值**
作为 owned int variant 写入 key1。中间 key239/key1 的附带 event 分支仍汇合到该写入。
原 getter 缺 tag 各 default2，且两类型均2的特例最终 `36e6bc` 写0；这是原件 fallback，
不能作为桥的有效卡类型来源。前述 `isVoiceGsmRat` 的 `key1&0xc` 因此实际检查卡类型 bits；
NAS/data RAT、IMSA registered、初始 -1 均不能补出已知 CARD_TYPE 或 network-ready。

**AP72 的 reset 来自真正 current-calls response，而非空对象初始化。** 普通未被 GWSD
截获的 `requestGetCurrentCalls381354..398` 在 original request 的 slot 上构造0909，
`callToMipcMsgAsync` 保留 original request reference。`handleCurrentCallsCnf3851b4..1c8`
先要求原 message+40 data 非空且 MIPC result0；失败发送 error38 的 VoidData response。
成功时 `385208..218` 读 native count tag100；原 getter default0 **不等于 strict decoder
已确认 count0**。真实 count0 分支 `3852d8/3852e0` 置 output byte size0 并直接跳 `3856c8`；
`3856d4..750` 构造 `RfxCallListData(array,0)`，以 original request ID+0c、original MCL
message reference 构造 error0 owned response，再 `responseToTelCore`。

`RfxMclMessage::obtainResponse19f4c4..530` 保留原 request 的 +18/+1c/+24 字段，
将 supplied data clone 到 response+10。Dispatch `1d6d4c..db0` 用 response+24 精确匹配
pending AP request+20 并移除该节点；`1d7404..43c` 以 matched AP fields 和原 response
error+48/data+10 构造 AP response，`1a51d0..1ec` clone data 到 AP response+48。
clone manager `1cf310..458` 按 request ID 查 response-kind1 的 copy-by-object 函数；
缺注册项返回 null，不生成 empty success。`CallListData::copyDataByObj1c31e8..1f4`
保留原 +8 pointer/+10 size 并建立新 owner；这是静态 ownership 链，不冒称新增 runtime 验证。

AP response loop `1d8404..8430` 送入 MainThread。Rtm 真实 direct current-calls reset route
是 **AP request ID c938**：`26ba14..1c→26bd2c→26bd34`，不是 cf21/cf34；GWSD/fake-mode
可提前截获。`handleGetCurrentCallsResponse26c1d8..218` 从实际 response+48 的 data 读取
size+10，**unsigned size<=7** 才将 slot AP72 写为 owned int0。该 route/handler 在此之前
**没有 error gate**。生产桥应取更严格的 owned、successful、native-count 明确0子集，
不能照抄 error-void empty、缺 count default0 或 size1..7 来授予 baseline/profile。

**AP72→MCL72 是软件 status 同步，不是 MD ACK。** 显式 policy setter
`setEccPreferredRat2794d8/279570..598` 将原 input w1 原值写入 AP72；普通 MO 不因此获得
默认0。AP `setValue1ca0b0` 的 extra flags 为0；即使值相等且 notify=false，实际调用仍经
`1ca230..238→1ca89c` 到 forwarding。它同步的是已发生的 setValue 事件，不是从“相等”
推导出缺省 reset。`1ca8a8..8d0→updateValueMdComm1cabd0` clone 原 variant，生成 type6
StatusSync 并 `requestToMcl`；AP message 在 `1a5d18..4c` 拥有 +24 slot/+54 key/+58 variant
和 +88/+89/+8a flags。`1df01c..54` 再 clone，生成 MCL message；`19fcc0..fd04` 明确其
type6/+1c slot/+68 key/+70 owned variant/+a0..a2 flags，正常路径进入 MCL queue。

MCL Messenger `1e7a0c..58` 用同 slot manager，clone message+70，按 key+68 调用
`setValueByRfx(...,w5=1)` 禁止回响 AP。`1dd730→1dce80` 的 storage base 是 manager+10；
缺 entry 在 `1dcf80..90` 分配并赋原 owned variant，existing changed entry 在
`1dd5d4..dc` 赋原值，同值保留原 owner。只有真实 reset 生命周期最终得到 **MCL72=int0**
后，才能应用上节已证的“104所有分支返回0”；不把 queue transfer 当网络/IMS/media-ready。

当前 `X9Calls.inc` 的 VOICE03 注册后 baseline query 已有自己的 token/result/native-empty
严格门，可提供桥自身的 owned successful-empty 事件，但不能声称现有 baseline_known
本身已包含 Android AP72/MCL72 manager lifecycle。Android matching UI/framework 的
**拨号意图→raw CLIR1/2 producer 仍缺**：retained AIDL Parcel 只证 int 字段，原 Rtm/Rmm
consumer 与 Apple 两套枚举已闭不代表该缺边已闭。本轮不开放默认 profile、不加 unused
接口，不新增设备抓取、探针、测试或镜像，不将静态进度描述为生产修复。

本机精确重读：仓库根执行
`python3 x9/out/call-profile-input/read-original-profile-input.py`；该脚本先核对上述原件 SHA，
只读固定 VA，写同目录的 `profile-input-original-proof.json`、`*.original-code.bin` 与
`*.numbered.asm`。结论/边界在 `call-profile-input-contract.json`；实际 source head 与
branch 记录在 proof JSON 中。原始与衍生设备字节均不提交 git。

## 2026-11-16 实跑 Hostless owner：动态 PCM 选择，不补固定硬件路由

本轮只读原 HAL 新范围与已保存 MO 日志，不重跑旧 probe。原 controller ctor
`27f940` 的 GOT `44d900` 由 `R_AARCH64_GLOB_DAT` 明确指向
`android::keypcmHostlessSpeech45d4e0`；相同 String8 传给 **this+3f8/+420**
两个独立 AudioPcmControl。`modemPcmOpen282630..282684` 对它们分别传
flags=`10000000`/`0`、owned mMdConfig、prepare=false；不是先前 frame-base
`adspPcmOpen` 的 descriptor 路线。

真实 MO `mo-call-audio.txt:625..633` 只有两次 `Hostless_Speech` open；
`:2145..2166/2256..2275` 的 stop/close 同样有两个 Speech owner，而 HW_SRC_1/3
的所有 controls 明确 **mPcm==NULL**。结合 ctor/open 分支，这次 basic Hostless
owner 已收束；不凭缺 start 日志断言未启动，也不把 open 当 DSP receipt。
原 `AudioPcmControl2632c4..263310` 是用实际名称经
`GetCardIndexByString315000/GetPcmIndexByString314f40` 查动态 descriptor，
读取 card+18/pcm+1c，未找到为-1；不是硬编码 card0/PCM编号。
`263488..263518` 调 `pcm_open` 并要求 non-null/`pcm_is_ready`，false prepare
参数不调用 `pcm_prepare`，本地成功不产生 iOS stream 或 firmware ACK。

还缺实际 ALSA descriptor/card/PCM 与通话时 DAPM/mixer 选择。同次 kernel 日志
只证明模块名 `snd_soc_mt6993_afe.ko`、`mt6993_mt6681.ko`，不证明已有对应 payload
或具体文件路径。reference 的 PCM_0/1→ADDA/I2S候选图不能直接变成生产寄存器值；
同版 audio_dsp、scene0 task-info8B、独立 USIP_PHONECALL 与 iOS BasebandVoice I/O
仍未闭合。不借媒体录放或 SmartPA owner 填通话声音。本轮没有独立可确认的生产
mutation，因此未改码、未编译/pack/上机，不把该静态收束称作最终音频修复。

原件窄范围、relocations、原日志行号与边界保留在 gitignored
`x9/out/voice-hostless-route/hostless-production-boundary.json`；只读复提命令
`python3 x9/out/voice-hostless-route/read-hostless-owner.py`。下一仅从真实缺失输入
或可达生产连接继续，不重复已闭范围、旧镜像、人工向量或未变更内核构建。

## 2026-11-16 生产装载入口收束：能复用的安装链与首缺 caller

基线 `a2b9036c`；本次只核当前仓库生产源和挂载根盘的 launchd 配置，不重复
typed/action/broker 原件切片。**缺的是连接代码和 owned 输入 producer，不是一个
待下载的二进制文件。** 没有新增空 loader、unused API 或探针。

可复用的具体 own-code 安装链已经在生产源码中：

1. `x9/build/build.py:304..312` 调用 torch/camera builder；
   `x9/torch/build.py` 用 Xcode iPhoneOS SDK 编译、ad-hoc 签名 arm64 iOS16 dylib，
   生成 hash-addressed root 路径和 `generated/x9_torch_bridge.inc` 的 embed 数据。
2. `kern_exec.c:8817..8820` 在 PID1 启动 launchd 前调用 torch/camera install；
   `x9_torch_install.inc` 经既有 `x9_bbfw_write/equal` 发布/核对自有库，
   拒绝未知现存文件或 symlink，要求明确 opt-in 和可写原生 UFS 根盘。
3. `kern_exec.c:5838..5842` 的 exec 环境插入点调用现有 helpers；
   torch/camera **只匹配 `/usr/sbin/mediaserverd`**，AppSync 仅匹配其三个
   verifier/UI executable，已有 DYLD key 不追加。没有匹配 CommCenter 的分支。
4. `torch/media_bridge.m::initTorch` 是实际 dylib constructor：先要求真实
   torch backend，再用 UUID/image/原 IMP/caller gate 安装 ObjC hook。
   这证明有完整 own-code 加载模式，**不是 C++ CallFactory hook 已实现**。

参考 `hackios-xnu-kernel/userland/src/inject.c::replace_virtual` 已有 expected
pointer 比对、`vm_protect(...VM_PROT_COPY)` 的 process-private vtable 修改方式；
它的 20F66 地址/目标不能直接迁成 20H392 Call provider。无需为此移植通用 tweak
loader或替换 Apple launchd/cache。

挂载根盘 `com.apple.CommCenter.plist` 和 `/System/Library/xpc/launchd.plist`
对应项都启动原 `/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter`
（`_wireless`），已有 `com.apple.commcenter.atcs.xpc` / `com.apple.ipTelephony`
MachServices，但没有 DYLD injection 环境。这是**持久配置观察**，不是实时进程
加载证明。仓库没有 Call runtime builder/embed/env 集成或 typed factory/action
替换实现；不能把 mediaserverd/AppSync 模式已存在写成“CommCenter adapter已加载”。

**首缺可达 caller 已落到当前代码：** `X9Calls.inc:372` 的
`static x9c_dial_submit(slot,req,len,profile)` 只有定义，没有生产调用。
现有 `X9QMIAT.inc:780 → X9Calls.inc:451 x9c_request` 仍只接44/03/21，
20落 `UNSUPPORTED`。`x9_call_wire.h` 的 measured SIM2 profile validator 不是
真实用户意图/卡路由 producer；普通 QMI20 也不携带原 typed UUID/clientId 与
per-subscription broker context/epoch 的绑定。因此只安装 dylib 并不能补出
`typed action → owned user context → 现有 broker → admitted kernel dial`。

下一实装需要一个带原 typed Call UUID/clientId、完整 owned 参数、真实
per-subscription broker context/profile/generation 的 caller，以及相应接收
admission 和匹配 terminal event 返回路径；不能用默认 profile、派生 UUID、
单开09或先给20加 switch case代替。此次没有找到能独立修复的已可达生产窄问题，
故未改 loader/内核，也未编译、运行测试、操作设备或生成 boot.img。

源码位置/引用及挂载 job 摘录保留在 gitignored
`x9/out/call-production-connect/{production-connect-source.json,production-entry-refs.txt,commcenter-launch-entry.json}`；
NOTES 与归档由 root 统一记录。

## Android 拨号意图 producer 的首个缺失原件（2026-11-16）

本轮仅核对已抓 input 的 manifest/文件名，不重复 native CLIR、prefix、CARD_TYPE 或 AP72。
`android-calls-20261115-183941/capture-manifest.json` 有124条抓取记录，但 **APK/JAR/DEX
code artifact 为0**；实际 files 树225项、incoming树33项、modem vendor/source/parts/mdota
分别8/2/8/784项，文件名 inventory 亦无 Java code 候选。没有 matching DEX，不启动 ASC。

同一抓取 `build.txt` 记录 Android17 的
`OPPO/PLJ110/OP5E17L1:17/CP2A.260605.016/B.46c413f-20895a6-20bdbdd:user/release-keys`。
已保存 `mapped-paths.json` 明确出现下列**实际设备路径，但未保留文件字节**：

- 首缺 producer 输入：`/system/framework/telephony-common.jar`、
  `/apex/com.android.telephonycore/javalib/framework-telephony.jar`。
- 同 build 链接/厂商扩展输入：`/system/framework/framework.jar`、
  `/system/framework/oplus-framework.jar`、`/system_ext/framework/mediatek-framework.jar`。
- 已观察 IMS APK：`/system_ext/priv-app/ImsService/ImsService.apk`；
  抓取的 `3302-cmdline.txt` 实为 `com.mediatek.ims`，不是 Phone/Dialer 进程。

Telecom metadata 中虽有 `com.android.phone`/`com.android.contacts`，但没有其确切 APK
位置或 code artifact；不编造 TeleService/Dialer 固定路径。上述 JAR 若无 DEX，仍需同 build
真实 code companion，不能拿 AOSP、另一版本或猜测的 sidecar 代替。本轮无设备操作、
通话要求、探针、测试、编译或生产代码改动；**matching intention→raw CLIR 缺边保持阻断**，
不开放默认0或新增 unused profile 准入。父代理统一安排后续输入，不要求用户此刻操作。

证据与 source SHA/head 在 gitignored `x9/out/dial-intent-input/dial-intent-input-proof.json`，
文件名快照在 `bounded-artifact-inventory.json`；重读现存 metadata 使用
`python3 x9/out/dial-intent-input/inspect-retained-intent-input.py`，不重新扫描或接触设备。

## 2026-11-16 SMS 当前生产链复核：版本与最终验收边界

复核 source **`a2b9036cc5b8e19d3b99cb4115a559abdd5f7090`** 的现有生产入口：
WMS 初始化→真实 per-SIM host-ready；WMS20→605 token→唯一 writer→匹配606/result/MR；
8601/8603→原 token/mask/txid→MT host queue receipt→WMS37→原物理 ACK→terminal host receipt。
这些路径已有生产调用者，不是 staging API；本轮未发现新的、有独立原件依据的 codec/dispatch
矛盾，未重跑已修 queue/malformed/lifecycle 类，不为“补全”制造成功 ACK、ready 或重发行为。

物理 writer 的精确定义仍须保留：`x9_modem_s3.inc:34..75` 的 result0 在 AP CCCI ring 完整
写入、barriers 与 producer-index commit 之后返回，pending doorbell BUSY不再重复通知。
它经 `x9_mipc_bridge.inc:531..534` 推进 ACK result；**不是新增的 modem 侧确认、数据库提交或
对端送达证明**，本轮也没有捕获新的物理 result0。

旧319的 SIM2 token703/txn27→modem CNF是历史运行观察；其 boot.img/source 未对齐，
不能套用当前 HEAD 或最新 NAS45 kernel。旧220的来信 token1、Android 的601/602/605/606/
8601/8602形状也不能拼成当前 iOS 同一样本成功。主 `x9/out` 限定目录/文件名盘点仍未找到
可认定为当前同 token 的 Messages 成功 record/COMMIT→重开显示、相关物理 ACK与WMS37完成，
或 MO 对端收到证据；空 grep/带 `stored` 的无关进程日志不证明短信成功或失败。

**首缺权威输入**是当前 image/source 对齐的一条完整短信运行链，其后才是同 token 的 MT
保存成功、重开持久化和 ACK，以及 MO 对端确认；既有 Apple 静态 SQL success 分支不替代它。
本轮不要求新抓手机，不读取个人短信数据库；无生产 delta便不重编旧产物，不出探针或最终镜像。
当前源码精确行号、旧证据资格与限定盘点在 gitignored
`x9/out/sms-production-audit/current-sms-final-evidence-boundary.json` 及同目录；仅新增此短小节，
无源码修改、编译、测试/人工向量或设备操作，完整短信验收仍未完成。

## 2026-11-16 matching Java 拨号 producer：显式 CLIR 与 per-sub 来源（仅静态）

此前缺失的同版 Java 原件现已只读提取，build 为 Android17
`CP2A.260605.016/B.46c413f-20895a6-20bdbdd`；raw/hash/ADB command 的权威记录在
`x9/out/android-call-inputs-20261116-092442/files-manifest.json`，不重算原件 hash。
本节只收束已有 ASC 反编译与 refs，**没有原 DEX code_item 偏移/opcode 复核或运行证明**；
下列 `*.java:L..L` 都是保留反编译文件的行号，不冒充原字节地址。

- `GsmMmiCode.java:1598..1611`、`ImsPhoneMmiCode.java:1498..1511` 的
  `getCLIRMode`：SC31 的 action `*`/Activate → raw2，`#`/Deactivate → raw1；
  action 判定分别在 `1434..1453`、`1285..1304`。
  `CommandsInterface.java:29..31` 命名 DEFAULT0/INVOCATION1/SUPPRESSION2，非 Apple privacy enum。
- temporary 不是单独设置 CLIR：regex 的 group12 经 `makeEmptyNull` 成实际非空 dialingNumber，
  还需 SC31 和上述 action；对应 `GsmMmiCode.java:436..468,1714..1723`、
  `ImsPhoneMmiCode.java:272..313,1681..1690`，空串处理分别在 `74..81`、`29..36`。
- `ImsPhone.java:318..399` 先把 tracker `getClirMode()` 放进 builder，再由 temporary MMI
  覆盖并仅将 extracted number 送 tracker；普通无 prefix **不等于固定 raw0**。
  `ImsPhoneCallTracker.java:3171..3182` 读取 `clir_sub_key + mPhone.getSubId()`；
  `Phone.java:3805..3822` 的保存者也用该 per-sub key，并检查 commit 失败。
- 缺 preference 时传入动态 `mInterfaceImpl.getClirDefaultSuppression()`；factory 选择在
  `ImsPhoneCallTracker.java:1140`。现存 `IOplusImsPhoneCallTracker.java:81..84` 仅 interface
  返回0，未取得实际被选厂商实现；另有 null preference/default-phone guard 返回0。
  **二者均不是当前运行默认值**，不据此制造默认 profile/0 值准入。
- 非 ECC tracker 保留 `DialArgs.clirMode`（`3219..3235`），`dialInternal:182` 写 `oir`；
  Java `ImsCallSessionProxy.java:4594..4656` 普通非 ECC/非 `*82` 正常 voice 分支保持该 int，
  经 `ImsRILAdapter.java:3773..3787,2528..2531,2207..2236` → **MTK**
  `RadioVoiceProxy.java:66..78` → `RILUtils.java:4270..4306` 的 HAL `Dial.clir` 原值赋值。
  此为 Java 静态路径，不替代旧 native 证据或本次真实 MD receipt。
- Java owner：`ImsService.java:1026..1064,3244..3260` 按 phone index 建 adapter、取同 index
  handler/adapter 创建 session；session 留 `mPhoneId`/adapter（`2000..2016`），adapter 留
  constructor phoneId 并据此选 HIDL/AIDL service（`1569..1572,1762..1772,1848..1858`）。
  这不是当前 subscription/SIM/native epoch 或 handover 后绑定已经闭合。

证据在 `x9/out/dial-java-producer/matching-java-producer-final.json`；已有命令产物为
同目录 `get-*.stdout/stderr`、`tel-getclirmode-refs.txt`、`ims-dial-refs.txt` 等。
`replay-commands.txt` 明标重建 recipe 而非历史 argv/exit；本轮未重跑 ASC、扩大扫描或触设备。
fresh `x9/out/call-owned-runtime/` 的 CLIR/OIR 零命中亦不证明 raw0。仍缺实际 default 实现/值、
完整可达 iOS owned producer/backend 与最终事件/媒体验收；无生产源码、测试、编译、CI/镜像，
VOICE09 保持关闭，不新增 unused API、默认 profile 或开放拨号准入。

### selected CLIR factory 的原 DEX 分支（2026-11-16）

续查只沿上述 `OplusTelephonyFactory` 新 lead，不重扫已排除的 wrapper 类。
matching `telephony-common.jar:classes.dex` 的四个原 `code_item` 已独立读取，
以下 offset 是**原未压缩 DEX**内位置，不是 ASC 重建 DEX 或 Java 行号：

- `getInstance`：`0x12b348`，instructions 从 `0x12b358` 开始。
  `myUserId()==0` 时用 `newInstance` 加载
  **`/system_ext/framework/oplus-telephony-common.jar`**，类名
  **`com.oplus.internal.telephony.OplusTelephonyFactoryImpl`**；正常返回后保存 factory，
  `sIsValidOplusFeature=true`。非 owner Android user 则构造 base factory、置 false。
  **Android user ID 不是 SIM slot 或 iOS PersonalityID。**
- ASC 此次 Java 输出遗漏了真实的 `mLock` monitor 和 `Exception` fallback。
  原 DEX 的 `cu0x10..0x43` try 范围有 `Exception → cu0x24` handler：
  反射失败会打印错误、构造 base factory并置 false；catch-all `cu0x22` 经
  monitor-exit 再抛出。不能将“代码会尝试加载 vendor”写成实际选择成功。
- `newInstance`：`0x12b4a4`，用实际 parent classloader 创建 `PathClassLoader`，
  `Class.forName(...,true,loader)` 后取无参构造并 `newInstance`。
- `ImsPhoneCallTracker::getClirMode`：`0x20ed00`，确认
  `clir_sub_key + mPhone.getSubId()`，并把实际 `mInterfaceImpl` 的
  `getClirDefaultSuppression()`返回值交给 `SharedPreferences.getInt`。
  interface 默认方法 `0x329aec` 的原两条指令确为返回0；仍不证明 vendor override
  或当前 preference/实际拨号值。

新的首缺输入是上面**selected factory jar**，它不在现有 `files-manifest.json`
采集清单，也未在本树原件/备份的 exact-basename 查找中找到；已留的
`oplus-framework.jar` 是另一份 wrapper，不能代用。下一需要同版该 jar 的
`getFeature`选择分支及 tracker implementation，再核其 default 方法和运行分支。
保留的 main/radio log没有本次 factory/default 搜索的决定性输出，不能推断成功或失败。

原件解码、位置 JSON、ASC输出和实际命令在 `x9/out/call-clir-selected/`。
初次原 DEX parser 因 Android17 hidden-API metadata enum4 不受支持而 exit1；
仅在 decoder 中保留未知注解的 numeric value 后 exit0，原 archive/DEX bytes未改变，
每条输出指令均核对其原 offset/raw bytes。不将 metadata workaround 当 runtime 策略。
本项无生产修改、构建、设备操作或功能验收，仍不开放 VOICE09/20 或赋予默认profile。

## 2026-11-16 同版 DSP 原件：task-info producer 与 USIP AD50 的窄契约

新 capture `android-call-inputs-20261116-092442` 已保留 active `audio_dsp_b`
**33554432B** 和7个真实 audio 模块；旧“未抓同版 payload”阻断已解除。
本节只收束现存 qualification/JSON/selected asm，不重新解包、反汇编、hash或扫描。
以下 `.ko` 地址均为 **ELF section-relative offset**，不是运行 VA。

容器头候选给出 `adsp_0_sram [200,a480)`、`adsp_0_dram [b560,6398e0)`、
`adsp_1_sram [63a9c0,643c40)`、`adsp_1_dram [644d20,15a4020)`（hex），
各有 `cert1/cert2`；这是头/长度一致性，不是证书验证、loader地址或entrypoint证明。
现存 strings 有真实 `task_phone_call_*`，但字符串不能证明已装载、scene0归属或receipt。

真实 `audio_ipi.ko .init.text+38..90` 循环48次，生产 `g_audio_task_info`
的 **48×8B**：`+0 u32 dsp_id / +4 support / +5 use_adsp / +6 use_scp /
+7 controller`。ioctl **40046914** 的 `.text+470..47c` 向用户copy **384B**；
入口还可能初始化DSP，不把它当纯只读查询，也不在本轮实际调用。
scene0的 `.rodata+3d38` 首byte **14(hex)** 使dispatch到 `.text+b758`：
`get_adsp_core_total()>1 → dsp_id3`；fallback再次查询，`1→2 / 0→4`。
只有3分支才经 `.text+ae48..88` 的 **dsp_id−2** 得physical core1（仍有total检查），
`.text+b350..36c` 给controller **43**。这不是实跑scene0的8B值；
`get_adsp_core_total` 在该模块仍为import，两个core的resume日志不能代替其回读。

真实 `snd-soc-mtk-common.ko .text+4bdc→4f88` 取
`get_smem_phy_start_addr(0,24,&size)`，`4c1c` 的AD50 producer依据
`EMI_TABLE` 生成两个u64 **{AP PA+30000(hex), 30000(hex)}**，不是MD PA。
`4d10..30` 发送scene0；enhancement bit1条件下 `4d4c..88` 可另发scene19。
数值 `data_type=1 / ack_type=0 / payload=16B / transport=32B` 与既有reference
契约一致；这是实际AP producer，不是DSP消费确认或通话双向media owner。

已抓 `adsp-v3.ko` 仅 **40184B**，init调用的是import `adsp_system_bootup`；
它不能补出主loader的实际加载/地址契约或上述core-total定义。剩余首缺是实际
loader/core-total、scene0实跑task-info8B和可归属DSP receipt，之后仍需通话时
DAPM/mixer选择、独立USIP_PHONECALL及iOS BasebandVoice双向I/O；不写猜测路由。
精确原件资格、section/行号索引和缺口在gitignored
`x9/out/voice-dsp-payload/{final-dsp-contract.json,final-evidence-index.md}`；
只重读已有证据即可复核，不接触设备或旧备份。本轮仅docs，无production delta、
测试、编译、CI、boot pack或上机；VOICE09不因静态收束开放。

## 2026-11-16 同版 Android17 SMS owner：raw 接收 ACK 不等于 Messages commit

本次已抓 matching `telephony-common.jar`（Android17 `CP2A.260605.016/B.46c413f-20895a6-20bdbdd`），
仅收束既有 ASC Java、selected DEX 和原 offset JSON，不重跑提取、hash、测试或设备命令。
以下 `code_item` 均是原 `classes.dex` 文件偏移；`+offset` 仅指 ASC rebuilt-ref 渲染的相对指令位置。
**长构造器标签纠正：** extractor 按 method name 索引，两个 `<init>` 覆盖了 selected 标题；
原 JSON 的 SmsDispatchersController 长 ctor 是 **`0x175c0c/512B`**，短 ctor 才是 `0x175e1c/26B`。
保留原件不改写，不将该错标题当原长方法地址，也不把 rebuilt 相对位置直接算成已核对的原绝对地址。

长 ctor 的 selected `+0100` 创建 GsmInboundSmsHandler；Gsm ctor `0x1f2c14/+0026`
将 `Phone.mCi.setOnNewGsmSms(handler,1,null)` 绑定事件1，factory `0x1f2a0c/+0014` 调 `start()`。
这是静态 owner/start 链，不证明当前进程实际 override、StartupState 推进或 baseband ready。
DeliveringState `0x1138e8/+01ac` 在事件1内调用 `handleNewSms`；事件2另在 `+015c` 调
`processMessagePart`。`handleNewSms 0x1170fc/+003e` 调 dispatchMessage；其 receive-disabled、
polling、satellite 等 gate 本次只有 retained ASC Java，不声称这些 gate 的原指令已逐项闭合。

**条件化的普通 CS GSM 接收分支：** radio-specific `0x1f2a38/+0162` → normal
`0x116084/+0198` → raw helper。`addTrackerToRawTable 0x115dc8/+0098` 调 provider insert，
随后 parse row id、设 tracker 删除条件并返回1；duplicate 返回5。`sRawUri` 在 retained Java
构造器指向 `Telephony.Sms.CONTENT_URI/raw`，不是最终 SMS app inbox 或 iOS Messages 数据库。
helper `0x115f50/+001a` 在 result1 时**先排事件2，再返回1**；duplicate5 则直接转1、不排新事件2。
`handleNewSms/+008c` 对 -1 跳过 ACK，result1 置 success=true 后 `+0098` 同步调用
notify/ACK；这条分支不等待事件2广播完成、最终短信 app 保存或重开显示。

Gsm ACK `0x1f2ccc` → `resultToCause 0x1f2bb4`：`-1/1→0`、`3→0xd3`、其它 `→0xff`；
其中 -1 的 cause 映射**不代表 handleNewSms 已发 ACK**。RIL `0x14941c` 先 canMakeRequest，
再申请 decimal request37，经 serial/lambda → RadioMessagingProxy `0x158cd4`，非 empty 时
调用真实 `IRadioMessaging` AIDL 或 `V1_4/IRadio` HIDL。Android request37 与 QMI WMS37
是不同命名空间；调用存在不等于 modem ACK、completion，也未证本次实际 AIDL/HIDL 分支。

selected 未保存完整 try/exception-handler 表，ASC Java 有类型/控制流失真；raw helper Java
还漏掉 selected 中 URI-error 分支。因此不补猜完整异常条件、provider transaction COMMIT 或 IMS 契约。
fresh 日志现位于主 `x9/out/call-owned-runtime/`；既有窄筛初始化/SMS ACK为0 hits，窗口09:15后，
不能据此判未初始化/失败；旧 Java/IMS owner 活动也不能拼成本次短信接收成功。
既有 kernel memory-full D3 映射与此静态分支吻合，无新独立 codec/dispatch 矛盾，不造 ready/ACK。
**Android raw 接收 ACK 不能替代 iOS 验收：** 仍需当前 image/source 对齐的同 token Messages
COMMIT、重开持久化、相关物理 ACK/WMS完成，以及 MO 对端收到；本次没有 production delta 或镜像。
原偏移、标签边界与逐条只读 `cat/sed` 入口在 gitignored
`x9/out/init-sms-java/matching-sms-final-contract.json`；root 统一归档/交接，无编译、CI 或上机结果。

## 2026-11-16 actual personality 更新、typed Client 输入与退休边界

以下为当前root/cache原件静态链，**不是已安装的MTK typed backend或通话/SMS验收**。
前节普通typed client的路由继续成立；本节补齐它依赖的真实owner、队列和re-add边界。

### 更新的是强owned model，不是临时Call或单slot helper

CommCenter原 `PersonalitiesTracker` 绑定点 `1007a1fe4..1007a2034` 将
handler+990的tracker与callback **`1007a5150`** 绑定；输入target queue来自handler+18。
event前24B为真实PersonalityID，action在+18/u8。原jump table `10114d135` 为
`0e1cf42af4f40000`：0→added、1→enabled、6/7→removed，2/4/5返回；
不替action3猜业务语义。handler+48是manager secondary interface，其+128/+130/+138
经this−0x30 thunks到 `handlePersonalityAdded_sync1f2e52048`、
`handlePersonalityEnabled_sync1f2e52f58`、`handlePersonalityRemoved_sync1f2e53bc8`。

added已有controller分支 `1f2e528d8..8fc`、enabled正常分支 `1f2e53180..1a4`
都从强controller virtual+38调用 **`getModel_sync1f2e421c8`** 得model强pair，
再从model virtual+30调用 **`updatePersonalityInfo1f2e77c04`**，传入真实
`shared_ptr<PersonalityInfo const> const&`。后者 `1f2e77d00..d08` 将model+28/+30
替换为incoming强pair，并通知model+58 observer；不是只更新integer slot。
`getPersonalityInfo1f2e77d7c` 在x8返回该强pair并increment control。
model getter本身没有revision/MD epoch，也不能由本函数未见mutex否定调用者串行化。

### tracker退休fence与manager getter同步必须区分

`PersonalitiesTracker::bind1f038aa40` 先unbind旧hookup，再创建**独立serial child queue**，
target为输入queue。`updateEvent1f038c1c4` 保存weak hookup/group、复制event、retain强record，
经async **`1f038c4e0`** 调tracker+50第一阶段，随后group notify **`1f038c75c`**
调tracker+70→上述CommCenter handler。两个执行阶段都weak-lock并检查旧hookup+20。
`unbind1f038a944` 在 `1f038a95c` 先清旧hookup+20，再清owner和三个callback。
所以不能把“已排旧event在rebind后调用新callback”当作既定行为；反过来，这也不是
native binding/epoch、已发MD mutation或typed delegate终态的退休fence。

原件decoder已修正：四个dispatch stub是 **ADRP/LDR GOT/BR**，不是ADRP/ADD直达。
当前GOT mapping的slide-v2实值 `page_size=4096 / delta_mask=0x00ffff0000000000 /
value_add=0`；各GOT均通过page-start与实际rebase chain成员资格，再核对当前export trie：

| 原stub | 原GOT | unslid target / 真export |
| --- | --- | --- |
| `1f03c19b8` | `20f7e8d98` | `18dba5310 _dispatch_async_f` |
| `1f03c1a0c` | `20f7e8e90` | `18dba555c _dispatch_group_notify_f` |
| `1f03c1a24` | `20f7e8f48` | `18db970d0 _dispatch_queue_attr_make_with_qos_class` |
| `1f03c1a3c` | `20f7e8f58` | `18dba5814 _dispatch_queue_create_with_target$V2` |

weak-lock veneer **`1f2f7b31c`** 原12B `b0bcd0f010e2019100021fd6` 直接解析到
真export **`194712078 __ZNSt3__119__shared_weak_count4lockEv`**。旧
`qualified/original-producer-proof.json` 四个`optimized_queue_stubs.target=20f7e8000`
**无效且保留不覆盖**；其它21个text/6个data/25个export不因此失效。新补证独立在
`qualified-queue/original-queue-proof.json`，此次只读target/metadata，没有重读旧21个text范围。

### 普通typed factory真正消费的Client强pair

已知 `100dcf174` 为factory准备临时Client pair时调用 **`100dd1168`**：
从CallInfo+0xd8读slot，driver+88 virtual+190的结果分普通/另一Client路径，
**不以该未知predicate或factory的emergency形参推测全部紧急策略**。
普通分支tail-call **`100dd3498`**，调用原 `PersonalityIdFromSlotIdEx` 得真实ID，
再经Registry service-map helper **`1001acd00`** 取强
`SystemDeterminationManagerInterface`。root GOT `10151b320` 的原import为此RTTI，
不是独立CallFactory item；磁盘bind编码 `8010000000000e62` 不是可调用地址。
manager primary addresspoint **`20e057338`** 的+18（slot `20e057350`）实际目标为
**`getIMSClientRef1f2e4c210`**。

这个getter首先调用 **`isPresent1f038b750`**（原veneer `1f2fb16cc`），不是
`sd::isSimPersonality`。isPresent只检查非空ID并按ID查强record是否存在，**不检查
record+31的SIM类型、+34的物理slot或当前初始化/注册状态**。
present分支从manager+18取queue，根据保存的dispatch包装状态选
`_dispatch_async_and_wait1f2e82fe8→18dba52ec` 或
`_dispatch_sync1f2e83084→18dba5a00`；原wrapper均将真实结果搬入x8 sret。
实际map访问block **`1f2e61a04`** 查manager+68的PersonalityID→强controller map，
存在则virtual+60→**`getImsClientRef1f2e68b38`**，从controller+d8/e0返回强Client pair，
缺entry则空pair。non-present分支另有特殊PersonalityID比较/诊断路径，不能简化成SIM0 fallback。

这闭合了普通typed action→真实强manager/controller/Client输入，不把clientId当personalityId。
**仍不能把三个独立读取（slot→ID、isPresent、manager排队后的Client）视作原子
record/revision快照，也不能把强pair成功视作REGISTERED或media-ready。**
物理provenance仍须取同一owned record，并由provider自有binding/epoch跨异步准入复核。

### removed→re-add不会自然生成新的Client身份

manager removed正常分支只查强controller并virtual+178；完整本函数没有erase该map entry。
controller removed `1f2e69980` 的+a0/+a8真实槽分别为
`deinitializeImsClient_sync1f2e6f40c` / `updateImsClientNetworkServices_sync1f2e6f000`，
还清理model、状态和DCN owner；不是Client destructor。普通deinit/cleanup/late callback
与root factory ret-deleter的原边界复用前节已认证证据，不再次声称remove即终态。

随后**若同ID entry仍在**，manager added替换model强info，virtual+170→
`handlePersonalityAdded_sync1f2e696c4`，再virtual+90→
`createIMSClient_sync1f2e6803c`。后者入口 `1f2e68054..58` 只要controller+d8 Client非空
就直接返回，不生成新UUID/Client。因此同一Client/clientId可以跨record替换与re-add保留；
不能仅按clientId或Call地址建立native epoch，不以强owner寿命代替当前record绑定有效性。
这是一条条件化原件控制流证明，**不是实机已发生移除/re-add或late event误投的观察**。

证据都在 `x9/out/call-context-producer/`。`qualified-client-targets/original-client-proof.json`
含9个前次成功比较的保留raw范围、此次唯一新增的isPresent text、7个data范围及3个新ABI目标；
第一次client pass因误给predicate猜名而exit1，未覆盖raw，resume只使用保留的9个text，
读新predicate/目标并核对原vtable rebase后 **exit0**。两个空objdump输出和此前short-VA查询
均保留为invalid，不用于结论。维护工具 **`x9/tools/call_context_evidence.py`** 消除scratch
import依赖，支持独立producer/queue/client模式，只允许NEW `x9/out`输出；本轮只静态syntax
parse，未为了维护版本重跑已闭范围。需要重放时从主目录使用已有root mount，例如：

```sh
python3 x9/tools/call_context_evidence.py --mode queue --out x9/out/<唯一名>
python3 x9/tools/call_context_evidence.py --mode client --out x9/out/<另一个唯一名>
```

本轮未修改生产call/SMS行为、运行测试/构建/CI或操作设备。下一真正接线仍是owned
record→logical binding/MD epoch→profile/admission→匹配typed终态；不能新增空loader或
默认profile绕过。**VOICE09仍关闭，VOICE20仍无生产caller**；CallList/handover、独立
双向DSP和当前Messages同token COMMIT/重开/物理ACK/MO对端验收仍未完成。

## 2026-11-16 显式 Android CLIR 的现有 codec 保真修复

上一轮matching Java producer已补齐一个过去不具备的输入子集：temporary SC31的
明确raw CLIR1/2，由非空extracted dialingNumber与同一DialArgs owner携带；它不是
查询vendor/default结果，也不是把Apple CLIR/CT枚举直接作为Android raw值。
复用 `dial-java-producer/matching-java-producer-final.json` 与
`call-privacy-policy/privacy-policy-original-proof.json` 的已有原件/selected证据，
本轮不再反汇编这些范围或查询旧wrapper。
在**non-ECC、普通十进制号码、raw CLIR已明确为1/2**的子集，原Rtm `265500`
不经key124覆盖，Rmm `387fb0` 的`*82`与ECC分支也不适用，原值进入108/u8。
selected vendor factory/default仍缺失，但不能把这个独立缺口扩大成“显式1/2也只能丢弃”。

当前代码的陈旧限制是 `x9cw_dial_profile_valid` 要求clir==0，且encoder无条件写108=0。
现修复**既有函数**，没有新增unused profile API或生产caller：

- profile validator接受已由调用者明确拥有的Android raw0/1/2；0仅保留已有capture子集，
  **不增加missing/vendor/network-default选择规则**。
- `x9cw_dial` 在改写output前先copy scalar profile，验证同一个local snapshot后逐字段序列化，108取真实
  `route.clir`，不硬编码0。原号码own-copy/同buffer规则、route/type/domain/ecc-domain约束保持。
- 物理writer上游既有 `x9cw_native_dial` 同步接纳mandatory108/u8的0/1/2，仍要求
  相同tag/order/width/decimal/NUL/padding和其余四个measured字段；大于2失败，不截断成byte0。
- QMI号码parser仍拒绝额外CLIR/未知字段；**并未从QMI的缺字段或者Call+08/+09猜raw值**。
  默认/Apple typed privacy转换、完整owned route与binding仍须真实producer。

这是现有encoder与native validator的功能扩展，**不是已接通iOS显式隐藏/显示号码**。
其0分支保持原包；1/2分支在现有40byte scalar tail中只改变108的value，号码不插入prefix。
无新通话类型、SIM0或domain选择，没有改registration、lease、token、epoch、terminal或SMS路径。
`X9Calls.inc::x9c_dial_submit` 仍无生产caller，VOICE09未公告，不能由codec成功授予REGISTERED。

测试源只补上显式1/2与无效3/0x100的期望，**本轮未执行任何测试脚本/套件或人工向量**。
固定输入 `prepare-local.py --check` exit0；仅按正常recipe编译实际消费者
`X9Baseband.cpp.o` 与 `sweet_x9_gpu_dependencies.o`。首次源码与review后先copy再验证的最终r2源码
分别编译，均为 **exit0，两项True，34/251 warnings**；第二次对应真实源码变化，不是产物复检。
输入为 `aad2b3ab`＋各次codec diff，首次记录
`x9/out/call-profile-producer/{source-head.txt,source-input.diff,compile-command.txt,
compile-recipe.py,single-compile.log,compile-exit.txt,compile/recipe-results.json}`。
最终r2记录为同目录 `source-input-r2.diff/compile-recipe-r2.py/single-compile-r2.log/
compile-exit-r2.txt/compile-r2/recipe-results.json`，原session29457已实际取得exit0。
scalar local snapshot防止验证后再次读取caller字段，不代表跨并发record/revision的原子性。
没有link、boot pack、CI或设备操作，也没有构建后二次hash/解包/反汇编检查。
完整typed/backend/profile/epoch/state/media和当前Messages最终验收仍未完成；后续应连接真实
owned producer，而不是为此小修复开放默认profile或重复收集已闭合的CLIR原件。
