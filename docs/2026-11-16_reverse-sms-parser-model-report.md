# CommCenter parser response 到 SMS Model/status 的静态证据

**结论：同 Controller 的 parser response → concrete Model → 原 callback 的 status/pair
已闭合；未reset分支带同强Model，reset分支返回raw1/null pair；app通知ID尚未闭合。** 原 `details+0/u32 → Model+8/u32`
是 **SIM slot**，不是通知 ID。这是静态进展，不是短信保存、COMMIT、物理 ACK 或设备验收。

基线为 `1960aedd`，分析分支 `feat/sms-parser-model-owner`。原件来自现有 root clone
的 `System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter`；不修改 Apple
用户态。私有原件、解码、读记录及 reader 保存在
`x9/out/sms-parser-model-owner-20261116/`，不入 git。

## 1. Evidence → Finding → Path

| Evidence | Finding | Path / 下一边 |
| --- | --- | --- |
| 上轮 `100813f14..100814254` callback；本轮 `1007f622c..1007f72bc` | 原 response、details、ACK function 进入同 Controller；格式分支构造 GSMModel/C2KModel | 共同结果 helper `1007f72bc` |
| 两个 ctor、base ctor、两层 owning wrapper；原 RTTI/format6 slots | concrete Model 由强 pair 持有，ctor own-copy 同 Controller+90 Registry | 未reset分支保同Model；reset分支raw1/null pair |
| `10001b360`、`1007e582c`；真 named import `10107dba8` | 原 details+0 写 Model+8，后者实际传 `subscriber::isValidSimSlot(..., SimSlot)` | **撤销候选 messageID 命名**，不能接到通知 uint ID |
| `1007f7694..1007f7724`；ACK function copy `10080d87c..10080d8e0`；pair reset `1007eced4..1007ecf30` | 非空 callback 接收最终raw status和当前pair；raw1分支已清空pair | callback operator 执行及物理 ACK receipt另有边界 |
| Controller AP+e0 原 slot `10159e838 → 1007f8994` | 同 Controller 带同 Model 进入多门控处理，再可到 `1007f2004` | Controller+9c0 外部 consumer 的实际 supplier/通知 producer未闭 |

简化数据流（箭头仅表示静态可达及参数同源，不表示每条路径都调用）：

```text
同 Controller 的 parser callback
  → 1007f622c(response pair, details pair, ACK function)
  → GSMModel / C2KModel + owning shared pair
  → Model virtual+160
  → 1007f72bc(details, raw status, 同 Model pair, ACK function copy)
      ├─ 条件满足：Controller virtual+e0 → 1007f8994 → 1007f2004
      │             → Controller+9c0 的 virtual consumer（未闭通知 ID）
      └─ callback 非空：virtual+30(&最终 raw status, 当前 pair)
                        未reset：同Model；reset：raw1/null
```

## 2. response、类型与强 owner

consumer `1007f624c..1007f6270` 保存 x0=Controller、x1=response pair、x2=details pair、
x3=原 callback。callback+18 为空时提前退出。其 response 使用 XPC dictionary
value/boolean 门和 data 反序列化；本轮只资格必要 imports，**未读对应 key 字符串**，
不替 key 指定新的语义。

- details+8 分支 **1**：原 `SmsPduDecoderSerialized`，分配 `0x288` 对象，在
  `1007f68e0` 调 `100847ca8`。输入为原 details+0/u32、Controller+90/+98 的 retained
  Registry pair、decoded PDU pair及 `(details+4 == 2)`。
- 分支 **2**：原 `C2KSmsPduDecoderSerialized`，分配 `0x210` 对象，在 `1007f6bcc`
  调 `1007e57cc`，保留相应原 SIM/Registry/PDU/boolean 输入。
- GSM ctor `100847d74..100847d7c` 安装 AP **1015a6268**；C2K ctor
  `1007e58f4..1007e58fc` 安装 AP **10159e3e0**。原 format6 actual chain 与 RTTI name
  分别为 **N3sms8GSMModelE / N3sms8C2KModelE**；offset-to-top 均为0。
- base ctor `10001b354..10001b3d0` 的 AP **101520078** 原 RTTI 为
  **N3sms5ModelE**。GSM ctor直接调用该 base ctor；C2K ctor内联初始化相同前缀。
- 两个 owning helper **10080b600 / 10080b52c** 保存输入 object 指针，分配0x20 control
  block并在其+18保存同object；对应 wrapper **1007ece64 / 1007ecdf4** 将生成的
  ptr/control换到 consumer 的 sp+a0。不是仅临时裸指针或把另一个对象称为 Model。

两种 Model 在 consumer 内调用 virtual+160：原 slots 分别指向
**1008485e8..100848b94 /1452B** 和 **1007e4e58..1007e57cc /2420B**。
新 body 未观察到直接写 Model+34 的指令；其中仍有 virtual calls，其全部目标/副作用
未资格，故不据“postparse”文件名声称无任何 mutation、保存成功或新 ID 分配。
本轮另核 +140 的8B目标：GSM仅读 Model+22c/u8，C2K仅读 Model+1b4/u8；不替这个
predicate命名业务状态。

## 3. 必须区分的三个整数

1. **SIM slot**：GSM base `10001b360`、C2K `1007e582c` 均把 ctor w1写
   **Model+8/u32**。这个 w1来自原 details+0。下游 `1007f2060..1007f2074` 取该值传真
   `subscriber::isValidSimSlot`；其 exact undefined named import为 **10107dba8**。
   此证据否定本轮最初“候选 messageID”的临时标签。
2. **Model 的16-bit message ID**：前轮真实 accessor仍仅写/读 **Model+34/u16**。
   GSM本轮 base ctor将其初始化为0；C2K `1007e5818 / 1007e586c` 将真 named
   `C2KSmsPduDecoder::getMsgID()` 的返回值写该字段。没有据此证明 app通知用它。
3. **通知 uint messageID / CTMessage.messageId**：前轮 app侧通知到同步取件的同源链
   保留，但本轮**没有将其连接到上述SIM或Model+34**。它也不是 MD transaction、
   Messages GUID、SQL RowID或native epoch。

## 4. status与callback；不冒充通知或保存

普通格式初始 raw status为 **4**。decoder `getClass()==2`、`GetSimDownloadFlag`
及原 details+4==2 的门可使其为 **5**；`isVoiceMailIndication` 非零可使其为 **7**。
C2K初始 raw **4**。这些 imports均由新原 stub/indirect entry/nlist/string exact资格。

`1007f6d18` 调共同 helper **1007f72bc**，输入同 Controller、原 details retained
pair、raw status、新 Model retained pair及原 callback copy。helper只在非空 Model
入口继续；consumer的raw2/null Model分支进入它后，**直接日志/返回，没有调用callback**。
不能把这个分支写成error ACK或保存收据。

helper的 `(1 << status) & 0x1b2` 门仅对 raw **1/4/5/7/8** 进入该处理组；它还会按
后续路由/配置门将status改为 **1** 或 **8**。不把所有enum命名为已知成功/失败。
普通未阻止分支 `1007f7628..1007f7658` retain同 Model pair传同Controller virtual+e0；
其他分支可以跳过这次处理。

终态特殊分支 `1007f758c..1007f75c4` 按Controller+e8及Model virtual+e0门，调用
**1007eced4..1007ecf30 /92B**。新body `1007ecee4` 先清pair的ptr/control，再release
旧control；随后helper将w22置 **1**。因此该分支的callback输入是 **raw1/null pair**，
不能笼统称全部终态都带原Model；也不能与入口raw2/null直接返回混同。

`1007f7694..1007f7724` 在callback+18非空时复制当前pair（非空则retain），保存最终 w22，调用
callback object virtual+30，参数分别指向raw status与pair。新copy helper
**10080d87c..10080d8e0 /100B** 对heap function调用virtual+10 clone，对inline
function调用virtual+18复制到目的buffer；为空则清目的+18。不再把该copy直接当裸ABI猜测，
但也不由generic copy替全部callback operator副作用作保证。

原Controller+e0 slot指向 **1007f8994..1007fb04c /9912B**，其输入是同Model pair。
例如 +140 predicate为0时，`1007f8e30..1007f8e58` retain原pair并调用
**1007f2004..1007f3e2c /7720B**。后者保留SIM、Registry与多项格式/配置门，若到
`1007f39c4..1007f3a24`，接收者为同Controller+9c0的object、调用virtual+40，带SIM及
若干原内容参数。旧creator `1007ea6b4 / 1007eac28..1007eac48` 证明该object是creator
**x2提供并retain到+9c0/+9c8的pair**，并非自动证明已安装的通知sender。

scratch文件名 `root-result-publisher`、`root-controller-publish`、
`root-notification-dispatch` 是采样标签，**不是已资格的函数名或app通知语义**。
本轮没有证明这里发送通知uint ID、durable保存、COMMIT、重开显示、WMS37物理ACK或MO对端。

## 5. 原件范围、执行结果与复核

全部 **18个新完整函数合30776B**，每段code read ≤1024B，原header/load commands/
LC_FUNCTION_STARTS与前轮捕获相符；旧七个SMS函数只读已有解码/证据，不重采原code。

| reader / 实际终态 | text范围 |
| --- | --- |
| `read-new-consumer.py`，consumer-r1 exit0 | 4240B |
| `read-new-result.py`，result-r1 exit0 | 1628B |
| `read-model-constructors.py`，models-r1 exit0 | 2420B /5函数 |
| `read-publication-functions.py`，publication-r1 exit0 | 10236B /4函数 |
| `read-notification-helper.py`，notification-r1 exit0 | 7720B |
| `read-routing-gate.py`，routing-r1 exit0 | 552B |
| `read-postparse-functions.py`，postparse-r1 exit0 | 3888B /4函数 |
| `read-result-pair-mutation.py`，pair-mutation-r1 exit0 | 92B /1函数 |
| `read-qualified-data.py`，data-r2 exit0 | 3个RTTI、selected format6 slots、12个exact imports |

所有上述成功stderr为空。data-r1因错误寻找不存在的reads索引文件退出1，只读header/
commands，未采新slots；保留 `read-qualified-data.r1.py` 和data-r1 stderr/exit。
r2使用前轮proof的exact `segment_starts_offset`，逐read追加 `data-r2.reads.jsonl`。
新pagechain必要节点仅作原pointer qualification，不将disk rebase称live GOT。

最终窄合同是 **parser-model-contract-r2.json**，复核时再读 `qualified-data.json` 与所引 `.asm`。
保留初版 `parser-model-contract.json`：其未reset限定的“callback带同Model”措辞由r2纠正，
不重写原生成物，也未重采成功code。
若以后确需重放采样，必须使用**fresh worktree/fresh out**，不得覆盖已成功目录：

1. 将本轮reader复制到fresh树的 `x9/out/sms-parser-model-owner-20261116/`，不要复制其
   输出；复制前轮 `x9/out/sms-controller-dispatch/` 元数据及
   `x9/out/call-context-capture-review/qualified-selection-r1/` 的fixup proof/starts原件。
2. 原件需已存在上述root clone路径；reader不mount、不修改它。text按表顺序执行，
   data资格reader在postparse及pair-mutation之前执行；`actual-reader-commands-r2.json` 是
   命令清单，实际采样先后顺序见 `timeline.md`。执行
   `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3` 加对应reader路径，
   stdout/stderr/实际数值exit分别写fresh目录。不要执行保留的data-r1原版。
3. 检查成功日志及新read provenance，再由exact调用参数、RTTI/slot和import核本报告；
   不是只靠文件名、候选符号或数值相等。

本轮未重放成功采样、未运行测试/人工向量、compile/build、CI、boot pack/设备动作、
mount/短信数据库读取或构建后二次二进制检查。没有新的生产SMS改动或验收镜像。

## 6. 下一项

沿旧creator的 **x2 supplier → Controller+9c0强owner → 实际virtual+40 consumer**，
找其真实通知uint ID分配/发布与同消息关联；不要继续拿SIM或Model+34替通知ID。
该supplier的actual caller尚未资格，本报告不给未读取的地址编造类型或ready状态。
另一侧returned BlastDoor GUID→成功record、Messages COMMIT/重开与物理ACK仍独立缺失。
完整通话短信目标继续active，不以本静态链标complete或blocked。
