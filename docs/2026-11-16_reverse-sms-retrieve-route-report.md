# SMS retrieve：真实注册、同 ID 取件与 reply 边界

## 结论

本轮是 **progress，不是完整短信或通话验收**。补上了静态的
`kRequest → kSmsRetrieveMessage → handler → MessageCenterModelInterface →
同 u32 ID 的 Operation map 查找 → reply 字段写入`。

关键纠偏：

- `MessageCenterModel+60` 的取件函数 **只查找并 retain/copy pair**，不在该函数中
  erase map 项；临时 strong pair 的释放不是退休、Messages COMMIT 或 modem ACK。
- handler 的 `x20` 先保存请求 ID，之后被 Operation virtual `+40` 的返回值覆盖。
  reply `kSmsMessageType` 使用的是后者，不能把它当作原 message ID。
- 事件 `0x1d`、客户端 `CTMessage.messageId`、Operation ID、reply type 是不同字段。
  原 ID 的客户端边继续复用 `sms-save-authority/identity-boundary.{json,txt}`。

没有改生产代码、编译、CI、运行测试/人工向量、打包或操作设备。
既有 CALL/SMS kernel 输入仍是 `41deaefb` 的
`x9/out/x9-call-end-native-owner-20261116/kernel.development.sweet`，本轮没有复检它。

| Evidence | Finding | Path |
|---|---|---|
| 原注册器、node insert、server lookup/indirect call | 真实 name→同 server handler，受 policy 分支限制 | §2；case `root-retrieve-register-*.asm` / `root-server-*.asm` |
| 同 Registry token insert/getter、Model+d0 真 slot、queued map body | 同 u32 ID 查 owning Operation；该 body 不 erase | §3；case `root-registry-service-pair-insert.asm` / `root-model-retrieve-*.asm` |
| handler 1004ba090 与四个原 reply key | type 覆盖 ID；字段写入不是 COMMIT/ACK | §4；case `root-retrieve-registered-handler.asm` / `handler-id-data.json` |

## 1. 原件和资格

- 原件：20H392 `CoreTelephony.framework/Support/CommCenter`；UUID
  `20b0ce28-18a1-3eea-aa2d-ef53e33c039a`。本机原件路径见 case `scope.md`。
- 实际 feature 基线：`3e4d50e737714bed3d77cea8e59657606c081d3c`，不是首核对的
  `f3425e5b`；两次核对间有同期 main 推进。
- case：`x9/out/sms-retrieve-route-20261116/`，所有 Apple bytes、raw reads、反汇编
  留在 gitignored out，不提交。
- 原 format6 数据链库存复用 11 旧页、遍历 69 新页，合 80 页；后续 selected slots
  复用此 chain membership，不重复遍历。
- 三个精确地址目标的原 text 库存排除 66756 个旧 instruction occupancy，捕获
  **16998412 B** 新原件字节。每次原件读取不超过 1024 B；库存核对原 header、
  commands、`LC_FUNCTION_STARTS`，实际 exit0。
- 后续 **25 个完整原 FS 函数、18340 B** 全由已保存 NEW chunks 资格/解码，
  **不是再次读取这 25 个函数**。promote 要求完整覆盖、无 cached gap、完整解码。
- 新精确 named imports **19 个**：stub/slot、indirect index、undefined nlist、
  bounded symbol name 均独立核对；这是原件导入资格，不是 live GOT 证据。

## 2. 真注册与消费者

`10123c308` 与 `101247a60` 的原 CString 均为 `kSmsRetrieveMessage`。
后者唯一真数据 slot 是 **10158b318 → 101247a60**。318 前两 words 为零，
不是 CFString；邻接 RTTI `DataProvisioningDataPlanManager` 不是 SMS handler 角色证明。

注册器 **1004b96cc..1004b9814** 的 `1004b96f4..1004b970c`：

```text
x0 = supplied server object
x1 = 10123c308 / kSmsRetrieveMessage
x2 = 1004b9f5c / retrieve handler
w3 = 3                         # 不猜其策略语义
BL 100a318c8
```

`100a318c8` 检查同对象 `+b0/+c8` 树后，向 `100a3bcbc` 传 handler/flags pair。
新 node `+20` 是 name wrapper，`+30/+38` 保存 handler/zero-extended flags；
节点挂到同对象 `+c8` 树。重复 name 分支不是无条件替换。

发布函数 **1004be9f0..1004bea28** 将注册器 function pointer 交给 `100a31678`，
后者加入全局 vector `101674010`。实际消费者 **100a3a0dc..100a3a26c** 在
`100a3a170..18c` 遍历同 vector，以同 supplied server 作 x0 调每个指针。
这证明静态注册对象同源；尚未核对该 publisher 的原 init-table entry 或 live 初始化执行。

实际 XPC 安装函数 **100a32770..100a32ee4** 在 `100a32c34..78` 构造
invoke=`100a33884` 的 block，并调用精确导入 `xpc_connection_set_event_handler`。
该 block 保存同 server、weak owner/control 和 connection。

**100a33884..100a34f3c** 的关键请求流：

1. 从原 dictionary 的 `10123c772 = kRequest` 取值，经精确
   `xpc_string_get_string_ptr` 得请求名；不把缺失/错误类型当正常输入。
2. dictionary 分支在 `100a34688..6ac` 用同 server `+c8` 和请求名/strlen 调
   **100a3be0c**。该函数用 **100a3ba84 → memcmp + length** 做精确 name 查找，
   不只是 page hit 或前缀匹配。
3. 真命中读取 node `+30` handler 和 `+38` flags，capture handler 到 block `+38`。
4. 原 policy 返回值等于 1 且 handler 非空的分支进入 **100a35494..100a35db0**；
   在 `100a35a68..aa0` 调 saved handler：x0=server+128，x3=input owner，
   x4=reply owning pair。拒绝/空 handler 走另支，不宣称所有请求都执行 handler。

`100a348fc` 的精确 import 是 **xpc_dictionary_handoff_reply**，不是
`xpc_dictionary_apply`；不得用近似名字推回调或送达语义。

## 3. Registry 与同 Model map

handler **1004b9f5c..1004ba4f4** 由 `kSmsMessageID` 取 input object；
`100015884` 的真实导入是 `xpc_dictionary_get_value`，null 走 XPC null wrapper。
随后 **1010817d8** 的原名为
`xpc::dyn_cast_or_default(xpc::object const&, unsigned int)`，传入默认值 **0**。
本轮资格的是调用签名/参数，不穷尽库内所有转换、越界或类型规则。
`cmn w20,#1` 拒绝 u32 `-1`；不能说所有缺失 ID 都被此分支拒绝。

`Registry::getServiceMap` 后的 **1000a95fc..1000a9750**：
mutex 下以真 token **1010da781** 查 ServiceMap+40 的 hash map，复制/retain
node `+18/+20` service pair。原 `__const` token name 是
`27MessageCenterModelInterface`，不是 CString section。

复用上一轮已资格 **1000bde4c..1000bf424**：

- 创建 AP **101528d10** 的 `MessageCenterModel`，原 RTTI base 是 public/offset0
  `MessageCenterModelInterface`；同 strong pair 已给 Controller+9c0/+9c8。
- `1000bec28..94` 以 **同 token1010da781**、同 returned Model pair，向原 Registry
  的非 const `getServiceMap` 返回对象+40 调新 **10006a170..10006a3dc**。
- insert 在不存在 key 时把 token、object/control 存 node `+10/+18/+20`，留强引用并
  增 size；重复 key 返回原 node/false，不替换。**不能因此断言任意 runtime Registry
  当前一定选中该新 Model**。server+128 的实际 Registry 安装来源也尚未穷尽。

真 AP `+d0` slot **101528de0 → 1000c0334..1000c0404**：
保原 `w1/u32 ID`，构造 invoke=`1000c6bb8` 的 block，按 Model+18/+20 分支调用
`dispatch_async_and_wait` 或 `dispatch_sync`，返回 owning pair。

**1000c6bb8..1000c6c28** 从 capture 取同 owner 和 ID，在 owner+60 的树按
unsigned u32 查找，真命中读取 node `+28/+30` Operation object/control 并 retain。
未命中返回零 pair。`1000c6c28/1000c6d08 → 1000c6ca4` 把临时结果移到 caller out-pair，
释放的是被替换的临时引用；上述新函数没有 erase tree、改 size 或发物理 ACK。
不由此排除其它虚调用的未追踪副作用。

## 4. Reply 不等于保存或 ACK

非 null Operation 先调 virtual `+40`，**在1004ba090覆盖 x20 为 type**。
接着 virtual `+c8` 接 buffer 和 flag=1；只有返回 bit0 成功才继续。
用精确 `CFDataCreateWithBytesNoCopy` 构 data，经 `ctu::cf_to_xpc` 和已资格
`1000188f4 → xpc_dictionary_set_value` 写以下原 key：

| key | 来源 |
|---|---|
| `kSmsMessageBody` | virtual+c8 buffer 的 data/XPC 转换 |
| `kSmsMobileCountryCode` | SubscriberUtility 返回内容的 XPC string |
| `kSmsMessageType` | 已覆盖的 w20/type，不是 request ID |
| `kSmsReplyEnabled` | Operation virtual+f8 的返回值 |

Operation virtual+c8/f8 本轮未展开，不把这些 field 写入升格为完整 parser、
成功业务 record、SMS.db COMMIT、重开读取或 modem ACK。handoff reply API 也不是
客户端实际收到/完成保存的证明。

## 5. 失败、纠偏与重放

- `service-token-data-r1` 把 __const token 当 __cstring，metadata 后实际 exit1，
  未读该 name。按真实 section 修正 reader 后 `service-token-const-data-r1` exit0。
  后发现同 token 已在上一轮 RTTI 缓存出现；此次小 name 读取是重复，不计新发现。
- `route-retained-ref-r1` 混合所有 out 的缓存且按排版猜 endianness，遇
  100531f60 conflict 实际 exit1，无资格结论。r2 改为已知 CommCenter 文件逐文件、
  mnemonic 验证字节序，不跨原件合并；实际 exit0。歧义 word 不作证据。
- 原件/关键 readers 和每个 promote/import runner 的真实 argv/stdout/stderr/数值 exit
  见 case `*.exit.json`，执行次序见 `timeline.md`，不得覆盖成功或失败 label 重跑。

无需重复原件采样即可复核：

```bash
CASE=x9/out/sms-retrieve-route-20261116
cat "$CASE/retrieve-route-contract.json"
cat "$CASE/root-server-exact-handler-find.asm" "$CASE/root-model-retrieve-queue-inner.asm"
sed -n '66,153p' "$CASE/root-retrieve-registered-handler.asm"
```

新 case 若要重放原读取，使用保存的 scope、metadata、spec、runner 的 argv；
使用新唯一输出目录/label，先确认原件 UUID 与原 metadata，不覆写现有证据。

## 6. 下一条窄边

由已读注册器继续 `10123c31c → handler1004ba4f4` 的实际操作名、同 ID/同 Model
取出/erase/ACK；先缓存矿，复用本次完整 raw inventory 和 80 页 chains。
同时补 publisher init entry、server+128 Registry 来源，不按邻接 RTTI 或 factory 名称猜。
完整目标仍缺：成功 record/GUID、COMMIT/重开、MT 物理 ACK、MO 闭环，以及 CALL
record/Client/UUID/native epoch 与双向媒体验收。
