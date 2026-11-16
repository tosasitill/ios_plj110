# SMS acknowledge 入口逆向 checkpoint

> 2026-11-16。只确认原注册名、handler 的 ID 转换及条件 virtual 调用。
> **尚未证明同 ID map 退休、Messages COMMIT、物理 MT ACK 或完整通话短信。**

## 范围与证据来源

- 输入为既有 CommCenter 原件元数据与保留库存；UUID
  `20b0ce28-18a1-3eea-aa2d-ef53e33c039a`。本轮分支起点 `5f406973`。
- 延续 [retrieve route 报告](2026-11-16_reverse-sms-retrieve-route-report.md)，
  不重新扫描其 16998412B 原 text 库存或遍历 80 页 chains。
- 本轮资格 3 个完整原 `LC_FUNCTION_STARTS` 函数，共 1940B；函数均从保留
  NEW 原字节 promote，无新原 text 读取、无新 import 资格。
- 私有证据位于 `x9/out/sms-retire-ack-20261116/`，包括 `scope.md`、
  `timeline.md`、`checkpoint-contract.json` 与每条命令的 `*.exit.json`。
  原字节、转储及日志不进 git。
- 未执行测试/人工向量、设备、数据库、mount、构建或已成功 kernel 的二次检查。
  此处没有 CI run ID 或 boot 结果。

## Evidence → Finding

以下均为**静态原件资格**，不是实机调用记录。content_hash 为 n/a：依赖已有
原件 UUID/元数据资格及保留读记录，本 checkpoint 未新 hash 原件。

| Evidence | source_ref（相对私有证据目录） | 不可变观察 |
| --- | --- | --- |
| E-001 | `ack-key-data.json`、`root-registered-retire-handler.json/.asm`、`ack-handler-promote-r1.exit.json` | 10123c31c 是 `kSmsGetRecievedMessageIds`；1004ba4f4..1004ba8f4，1024B；promote actual exit0 |
| E-002 | `next-key-data.json`、`root-next-registered-handler.json/.asm`、`next-handler-promote-r1.exit.json` | 10123c336 是 `kSmsGetIncomingMessageCount`；1004ba8f4..1004baa84，400B；promote actual exit0 |
| E-003 | `registered-names-data.json`、`registered-names-data-r1.exit.json`；上一轮 `root-retrieve-name-reference.asm` | 10123c366 是 `kSmsAcknowledgeMessage`；原 registrar 供应 handler1004bad6c、w3=3；名字 reader actual exit0 |
| E-004 | `root-acknowledge-message-handler.json/.asm`、`real-ack-handler-promote-r1.exit.json` | 1004bad6c..1004baf70，516B；ID default -1、第二转换 default 0、bit0 条件与 +e0 BLR；promote actual exit0 |

### F-001：初始退休 lead 不成立

- severity: n/a_re；category: other；status: false_positive；confidence: high。
- evidence_ids: E-001、E-002；location: 1004ba4f4、1004ba8f4。
- get-IDs handler 经旧 getter1000a95fc 调 service virtual+100，填两组 u32
  vectors 并写 reply arrays；不从 request 取单 ID。count handler 调 virtual+d8。
- 这两条只是候选 ID 状态面；临时 vector 释放不能证明 Model map 删除或 ACK。
  初始暂定名 `root-registered-retire-handler` 不再承载退休语义，原文件不改名。

### F-002：真实 acknowledge 路由及条件调用已资格

- severity: n/a_re；category: other；status: validated（仅静态）；confidence: high。
- evidence_ids: E-003、E-004；location: 1004b9764..1004b977c、1004bad6c。
- 原 registrar 以实际名字、handler 和 w3=3 调既有注册 helper100a318c8。
  w3 的业务策略含义未资格。
- handler 保 x3 为 input owner、x4 为 reply owner，通过旧
  `Registry::getServiceMap`/1000a95fc 得 service pair。
- service 非空时，先按已资格 `kSmsMessageID` 取值，以 unsigned 转换、default
  **-1** 保到 x20；这与 retrieve 的 default **0** 不同。
- 第二字段地址为 **10123c5e5**，转换 import 为 **1010817b4**，default 0，
  结果保到 x21。名字和转换类型尚未资格，不称 bool、success 或 relay。
- handler 检查 `((w20 == -1) OR w21)` 的 **bit0**。bit0=0 时在1004bae74
  调 service virtual **+e0**，x1 为原 ID；bit0=1 时令 w0=0，跳过该调用。
- 返回路径调用未新资格的101085084，写入尚未资格的 reply key10123c603。
  service 为空走另一分支，不把它混为上述 w0=0 reply 路径。

## P-001：已闭合的窄调用路径

path_type: callflow。

1. 原 registrar 的10123c366名字与1004bad6c handler pair（E-003 → F-002）。
2. handler 取 supplied Registry 的 service pair；service 必须非空（E-004）。
3. 原 ID 转换/default -1，第二转换/default 0，按组合结果的 bit0 分支（E-004）。
4. 允许分支保原 ID 调 service virtual+e0（E-004 → F-002）；**在此停止**。

残余边界：+e0 的 concrete target、同 Model+60 map mutation、下游 modem ACK
尚未资格；server+128 Registry 安装及当前具体 Model 选择仍是条件，不从同 token
推出 live 同 owner。

## 不重采的离线复核

在仓库根运行以下命令，只读取已有记录，不触碰原件或设备。私有证据不随 git
分发，因此另一机器必须先具备这些保留产物；不要用本节触发新的采样。

```bash
cat x9/out/sms-retire-ack-20261116/real-ack-handler-promote-r1.exit.json
cat x9/out/sms-retire-ack-20261116/registered-names-data-r1.exit.json
grep -n -E '1004badc0:|1004badf0:|1004bae04:|1004bae0c:|1004bae10:|1004bae6c:|1004bae70:|1004bae74:' \
  x9/out/sms-retire-ack-20261116/root-acknowledge-message-handler.asm
cat x9/out/sms-retire-ack-20261116/checkpoint-contract.json
```

原始 reader/promote 的真实 argv、cwd、数值 exit 分别保存在相应 `*.exit.json`；
`timeline.md` 记录实际顺序，不能把命令文件列表当时间线。

## 下一动作与功能缺口

1. 先矿保留 data，再 bounded 资格10123c5e5、10123c603，以及精确 imports
   1010817b4、101085084；不得先给第二字段补业务名字。
2. 资格同 Model AP101528d10 的+e0 slot **101528df0**。slot target 确认后先矿
   旧函数，无旧 body 才从保留原库存 promote，禁止再扫原 text。
3. 只沿该 target 分清同 u32 ID 的 lookup、Operation callback、erase、size
   mutation、pair release及 Controller/物理 ACK 下游。必要的+100/+d8状态面也
   不替代 mutation 证据。
4. publisher 原 init entry、Registry 安装/current selection、GUID/record、
   COMMIT/重开、MT物理 ACK/MO，以及 CALL owner/UUID/nativeepoch/双向媒体仍缺。

完整通话短信目标保持 active；本报告只是入口 checkpoint，无生产行为 delta。
