# 通话 iOS 桥：已核实子集与启用边界

## 目前交付什么

这是**信令桥基础，不是已可使用的通话服务**。现有 WMS/IMSA 短信实现保持独立。

| 部分 | 实现 / 验证 |
| --- | --- |
| iOS 请求 codec | VOICE20 普通拨号有界解码，21 挂断、22 接听严格 tag1/u8；动作要求显式已验证的 route / 单通话映射 |
| Android 请求形状 | 新增本机卡2实测 `0901` 的六个 TLV；接听 `0907 {}`；挂断 `0905 {MODE100/u32=0, ID101/u32}` |
| 来电 CMD | `8901` 的字段、长度和唯一 tag 验证；号码只留在有界内存，不打印 |
| 来电许可 RSP | `8902` 的正确 approve/call-id 字段；显式调用才构造，保留原 txid / 原 mask |
| 物理 transport | 独立 call inbox / CMD owner，复用原有唯一 MD tick writer；SMS ACK 保持优先 |
| 超时 / 重复 / 失败 | 无自动重发；重复 outstanding CMD 去重；内容变化及未知退休规则隔离 calls；不清空 mutation owner |
| 默认启动 | **不发布 QMI VOICE09、不注册 CALL CMD/IND、不发通话请求、不自动 approve，无强制开启 bootarg** |

`X9Calls.inc` 当前只明确拒绝意外直达 VOICE 请求。它不会成功应答初始化并伪装服务已可用。
`x9cw_action()` 只生成报文；没有呼叫状态来源与映射生产者，因此生产路径不会调用它提交动作。
`x9cw_dial()` 同样仅编码：必须显式提供已验证的卡2 profile、无现存通话；当前没有 profile provider。
原 transport 已允许这一严格 DIAL 子集并由宿主测试闭合发送/CNF，但没有 iOS 自动提交通话的调用者。

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
新通道保存单 bit 原 mask，不把它归一化成 SIM index；多 bit 命令不自动选择一个目标。

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

`getDomain` `0x387690`、`getEccPreferredDomain` `0x387a30` 依赖实时状态；测得0
**不代表所有卡/网络/拨号类型默认0**。本阶段 `x9cw_dial_profile` 由调用者显式提供，
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
完整状态/ID producer仍是后续拨出桥的必要工作，不要求用户再测来电。

音频日志已证明该场景实际走 `SpeechDriverOpenDSP::SpeechOn/ScpSpeechOn`，48 kHz、Aurisys
scenario7 / UL3 / DL1 / ref2；拨出后新加载 `lib_aurisys_opve.so`、`libaurisysdemo.so`、
`libspeechparser_vendor.so` 已按新 maps补抓。**这些不是可直接照抄的iOS音频配置，也不是双向声音验收。**

## 代码与生命周期

- `x9/kernel/src/bsd/dev/x9_call_wire.h`：共用、freestanding C/C++ codec。
- `x9/kernel/src/bsd/dev/x9_mipc_calls.inc`：独立 call inbox、CMD response owner。
- `x9/kernel/src/bsd/dev/x9_mipc_bridge.inc`：严格动作白名单 / RX 分发 / 单 writer 接线。
- `x9/kernel/src/iokit/Kernel/arm/X9Calls.inc`：iOS 未启用边界，直接 VOICE 返回 unsupported。

CMD owner 状态：收到 → 已交付 → 显式许可排队 → 物理发送 → 结果可取 → 身份 tombstone。
成功/失败都保留 tombstone 到 boot，不为同一个原始 txid 再发响应；payload 在物理 TX 完成后清零。
队列或身份空间用尽只隔离 calls。当前最多保留八个 CMD；**这是待接入阶段的明确限制，不是最终通话容量设计**。
需要 Android 证明 call-end、seq/txid 重用及多卡路由之后才能安全退休。MD 物理失败仍遵循现有全局 offline/quarantine，不能假装继续在线。

原有 request owner 不释放未到 CNF 的已发 mutation；没有新增重试或 cancellation。
新的 response API 返回的 success 仅指 **RSP 物理 transport**，不是 iOS 来电显示 / 接听 / 音频成功。

## 离线重放与验证

```bash
# 使用现代 sanitizer runtime；本机 Xcode15 的 ASan 在当前 macOS 启动时会崩溃。
export CXX='/opt/homebrew/opt/llvm@22/bin/clang++ -isysroot /Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX14.0.sdk'
python3 -m unittest x9/tools/test_x9_call_bridge.py -v

# 只读恢复本版本 MD 函数边界；符号存在本身不是状态/策略证据。
python3 x9/tools/md_call_evidence.py
```

24 项宿主测试编译**实际生产 transport / codec**，使用人工测试向量，不需要 `.local`、设备或网络。
每个用例用新进程，从干净 owner 状态开始。覆盖独立 inbox、fragmented RX、审批字段 golden bytes、原 txid/mask、
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

## 下一步的必要输入

1. RIL与卡2拨出材料已补齐；先离线闭合完整 CALL_STATUS / GET_STATUS framing、ID producer和iOS单通话状态映射，再产生真实profile。
2. 核实普通VOICE初始化/订阅与每请求响应；0902不能替代带真实CallIdentifier的QMI20成功。
3. 从刚抓回的实际音频实现核实 MD/SCP voice 上下行路由，再分别验收信令和双向声音。
4. 用户本轮不方便来电测试，来电/多通话暂不扩展，不再以补来电为当前拨出工作的前提。

在这些输入闭合前不发布 VOICE，不以猜测默认值或探针 boot.img 代替依据。
