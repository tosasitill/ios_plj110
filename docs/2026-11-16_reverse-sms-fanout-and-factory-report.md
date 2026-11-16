# SMS 通知 fanout 与同 Model factory 分流

## 结论与范围

本轮是静态取证进展，**不是完整通话短信验收**，没有生产代码改动。

- 通知的同一 envelope 已追到原 `xpc_connection_send_message` 条件调用；没有证明 app 收到或同步取件成功。
- `kEvent` 在构造时是布尔值，在真实 queued invoke 中被覆盖为事件整数；事件 code 与嵌套的 Operation message ID 不可混用。
- 携带同 MessageCenterModel pair 的 factory `+18`、`+20` 分支分别落到 **TerminalRegistrationController**、**PhoneNumberRegistrationController**，不能当作 retrieve server。
- 当前 server `kSmsRetrieveMessage` route、同 ID 的取件/Operation 退休、Messages 成功 record/COMMIT、物理 ACK/MO 仍未闭合。

基线 main/origin：`9e25175ba3fa1407d6ea520e3a59d35ff5cc0efd`；功能分支：`feat/sms-retrieve-retirement`。原件为既有 20H392 CommCenter，UUID `20b0ce28-18a1-3eea-aa2d-ef53e33c039a`。它证明原件静态路径，**不证明当前设备加载或执行该路径**。

私有原字节、采样记录、命令和派生产物全部留在 gitignored `x9/out/sms-retrieve-retirement-20261116/`，下文路径以此目录为基准。未新采设备、挂载、读数据库或改共享输入。

## Evidence → Finding → Path

| Evidence | Finding | 下一实际边 |
| --- | --- | --- |
| `root-notification-envelope.asm`、`envelope-data.json`、`envelope-imports.json` | 同 dictionary 被包装为 envelope；四个 key、bool create、null-type 引用已资格 | queued invoke 对 envelope 的真实修改 |
| `root-notification-invoke.asm`、`root-notification-broadcast-impl.asm`、`predicate-data.json`、`root-notification-predicate.asm`、`send-imports.json` | 同 envelope 经 predicate 与订阅过滤进入原 XPC send API | 实际 subscriber 收件，而非以 send 当 receipt |
| `root-same-model-factory-plus18.asm`、`root-same-model-server-create.asm`、`server-object-valid-subset.json` | 同 Model pair 进入 TerminalRegistrationController，不是已资格 retrieve server | 返回请求 route 注册证据 |
| `root-same-model-factory-plus20*.asm`、`factory-plus20-type.json` | 下一分支条件创建 PhoneNumberRegistrationController，同样不闭 retrieve | 不继续猜 factory slot；从实际请求 key/handler 建立调用边 |

`root-same-model-server-create` 等文件名是取证时的 lead 标签；**真实 RTTI 优先于标签**。

## 同 envelope 到条件发送调用

### 构造及字段

`100b73080..100b732e4`（612B）保持入口 code、原 dictionary 与 second owner，并构造 envelope：

| 原 cstring | key | 值/门控 |
| --- | --- | --- |
| `101261446` | `kEvent` | 原 `xpc_bool_create(true)` |
| `10126144d` | `kNotificationEvent` | `sxtw` 后的 code，原 `xpc_int64_create` |
| `1012563c0` | `kNotificationDict` | input 非原 `__xpc_type_null` 时放入同 retained input |
| `101256396` | `kNotificationAdditional` | second owner 非同 null type 时放入 |

四个字符串按原 `__cstring` section 和严格 NUL 上界读取，没有全 strings 扫描。这里的 **code 是事件 code**；上一轮 Operation ID 位于原 `kCTMessageIdKey` payload 内，仍保持独立。

### service pair、队列与字段覆盖

- `100b7352c..100b7366c`（320B）经 `100a32664..100a326c0`（92B）取得全局结构 `10166ffc0+40/+48` 的 retained pair；读取前后调用原 `pthread_mutex_lock/unlock`。ptr 为 null 时不派发。
- 它捕获同 envelope、code、service pair，以及真实 inner invoke `100b742e8`，把 service `+48` 和 capture 传给 `100a3a604..100a3a70c`（264B）。
- helper 经 `100a3a09c..100a3a0dc`（64B）锁住 supplied weak pair；null control/失败锁走 `10001f9c0`，不从未资格目标名推异常语义。成功后把 inner block 指针与强 pair 捕获到 outer block，用原 `dispatch_async` 派发到 supplied 结构 `+10` 的 queue。
- outer invoke `100a3a70c..100a3a718`（12B）加载 outer `+20` 的 inner block，再从 inner `+10` tail-branch 到真实 invoke。
- `100b742e8..100b7439c`（180B）把同 envelope 的 **`kEvent` 改成 signed code int64**，再带同 service/code/envelope/flag1 进入 `100b7439c`。

Block 生命周期边界：inner copy/dispose（96B/52B）显式 retain/release XPC envelope 与 service pair。outer copy/dispose（36B/8B）显式保留/退休成功 weak-lock pair，但已读 outer copy **没有显式 inner-block retain**。generic descriptor/layout 对此的语义尚未资格；不能据此报 UAF，也不能把 capture 指针自动当已证 owning block。

### predicate、订阅过滤与 send

`100b7439c..100b74464`（200B）建立 temporary callable，再调用 `100b74464..100b746e8`（644B）。其真 AP `1015d2fe8+30` slot `1015d3018` 指向 `100b75ac0..100b75ac8`（8B），实际返回 `w0=1`。

对本轮 SMS code `0x1d`：

1. 遍历 service `+e0` 起始、`+e8` sentinel 的内存树；temporary callable 的 predicate 放行。
2. 用 node `+50/+58` 的整数 vector 查同 code；未命中时查 `-1` 通配。特殊 `0x99..0x9e`、`0x86` 分支不适用于 `0x1d`，不扩展其业务语义。
3. 命中后，`100b74570..100b74578` 把 node `+20` 的 connection 与**同 envelope**交给 stub `1010850f0`；原 indirect/nlist/string table 与 12B stub 共同资格为 **`_xpc_connection_send_message`**。

该条件调用无 app receipt/COMMIT 收据；没有 subscriber 时不发送。没有读取设备当前订阅表，也没有证明 Messages 收到这条通知。

## 同 Model pair 的两个分流

复用上一轮已闭的 installed MessageCenterModelFactory AP `10154f170` 和原 `1000bde4c` 构造器，不重采旧 installer/constructor。

### factory +18：TerminalRegistrationController

原 `1000be900..1000be948` retain 同 Model pair 到 `sp+78`，带 queue 调 factory `+18`。真 slot `10154f188→10036a0c4`，新 wrapper 232B 保持 pair，调用新 `100585858..10058656c`（3348B）。

后者同 owning object `+a0/+a8` 保留 supplied Model pair。原 AP `1015790a0`（offset-to-top 0）与 secondary AP `1015790d0`（-8）的同 typeinfo `101579218`，NUL 终止原名均为 **`30TerminalRegistrationController`**。因此不能按函数文件名把它命名为短信 retrieve server。

### factory +20：PhoneNumberRegistrationController

原 `1000beaa4..1000bead4` 传同 Model pair 调 factory `+20`。真 slot `10154f190→10036a1ac`，新 172B wrapper 保持 pair，调用新 `1009cded4..1009ce018`（324B）。

getter 返回对象 `+174/u8==0` 时返回 empty pair；非零才构造新 object。不从此字节位置命名功能开关，也不预测设备当前值。

新 ctor `1009ce018..1009ce4b8`（1184B）将同 supplied Model pair retain 到 owning object `+58/+60`；最终 AP `1015beed8`（offset-to-top 0）、typeinfo `1015bef90` 的原 RTTI 为 **`33PhoneNumberRegistrationController`**。此分支同样不是已资格 retrieve route。

## 执行记录、失败与复核

- **19 个新原 FS 完整函数，共 7848B**；各 function reader 实际 exit0/stderr 空，code/data 单次读取不超过 1024B，匹配既有原 header/load-command/FS 证据。大输出保存在文件。
- **7 个 exact original undefined named imports**：bool create、null type、NSConcreteStackBlock、dispatch_async、pthread mutex lock/unlock、xpc connection send。不声称 live GOT。
- cache inventory 兼容 Capstone `#0x...` 与 LLVM `0x...`。`send-cache-r1` 实际 exit1：误猜 `100a38cec` 非原 FS，库存 guard 在原 text 读取前拒绝。没有实际代码边支持该地址，已弃用。
- `server-object-data-r1` 实际 exit1：前两个 Controller RTTI 完成后，后续 control AP `101579310` 的 RTTI name pointer `0x10080001121a1d` 未过 untagged format6 guard。原脚本、原 raw 与 stderr 均保留，不重读或掩 tag。
- `terminal-subset-recovery-r1` 离线重建保存的 byte/chain 子集，实际 exit0；合同明确原 reader **仍是 exit1**，只接受已完成的两个 Controller RTTI。不资格后续 control name。
- 继承 data reader 的输出 label 曾被 metadata loop 变量覆盖；本轮在第一次执行前改用独立变量。没有运行缺陷版本或覆盖旧输出。

核查入口：`scope.md`、`timeline.md`、`workitems.md`、`field-journal.md`、`fanout-factory-contract-r1.json`、`actual-reader-commands-r1.json`。命令清单按文件名排序，**不是时间线**；真实顺序和终态以 `timeline.md` 与各 `*.exit.json` 为准。

复核同 input→effect：先看 envelope 的 key/data proof，再看 inner invoke 的字段覆盖，最后看 predicate slot/body、subscription 分支和 exact send import。factory 分流则从旧 constructor 的 supplied pair、原 slot→新 wrapper→ctor own-copy→RTTI 顺序核查。`recover-terminal-subset.py` 展示仅用保存 raw 恢复的具体过程。

上述 reader 拒绝覆盖现有输出和已缓存函数。需要独立新采时应另建 `x9/out/` case、调整 reader/runner 的固定输出路径并保存新 argv/exit；**本轮没有重放已闭采样或执行测试向量**。

## 下一步与未完成项

1. 从实际 request 注册/dispatch 找 `kSmsRetrieveMessage` server handler。既有字符串偏移 `10123c308/101247a60` 仅为缓存 lead；本轮未运行新 whole-text/key reference inventory。不要继续按 factory slot 顺序猜角色。
2. handler 的同 notification Operation ID→同 MessageCenterModel `+60` 内存 map 的取件、reply 与退休；保持 SIM slot、PDU u16 ID、Operation u32 ID、GUID/RowID/native epoch 分离。
3. 真实 app 收件及 GUID→成功 record/COMMIT/重开、MT 物理 ACK/MO 对端；CALL 逐动作 record/Client/UUID/epoch 与独立双向媒体仍需完成。

本轮无 kernel/host 编译、测试/人工向量、CI run、boot pack、设备/ADB/mailbox/mount/数据库/分区操作，亦无构建后二次产物检查。最新有效 kernel 仍是完整输入 `41deaefb` 的 `x9/out/x9-call-end-native-owner-20261116/kernel.development.sweet`；本轮不重编、不改名，也不把它称为完整验收镜像。
