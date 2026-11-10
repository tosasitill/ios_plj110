# X9 基带桥：AT 控制面、MIPC IPv4 WDS 与 DPMAIF

**当前实测仍是 rev252hl-md / 157；最新候选 rev252hs-md 已编译，尚未上机。**
11 个完整 Apple 固件文件已可读，Sahara finished、QMUX 开启；这不等于 iOS 蜂窝可用。
控制查询走真实 AT；hs 的 IPv4 WDS 走实际 RIL 使用的 MIPC1，含 DPMAIF 主动初始化/收发代码。
**PDP 激活、DPMAIF DMA 可达与实际包收发仍未实机验证；语音/IMS 未实现。**
当前候选与交接以仓库根 `NOTES.md` §7 为准。

`qcom/` 引入 UniXNU/QcomBaseband（BSD-2-Clause，HoshinoYumeka / chiba233；上游提交见
`qcom/UPSTREAM.json`）。只使用 portable `baseband/`：AppleBaseband IOKit、QMUX/QRTR、
Sahara 兼容桥、PDP helper 与 VINYL Tape。Qualcomm IPA/GSI 不适用于 MT6993 的 DPMAIF。
未导入参考项目针对 iOS17 的 CommCenter/BBTicket 机器码修改；本项目保留原始 20H392 用户态。

## 用户现在怎么启动

在仓库根，由用户手动执行：

```sh
fastboot boot x9/out/x9-rev252hs-md/boot.img
```

- **不用再放固件或挂载目录**：独立 34 MiB pinned+immutable 固件文件已 stage；不重新 create/resume。
- 插件 **275** 未改，不需 mailbox staging，也不传 `--revision` 重建插件。
- 不 flash、不改 GPT、不删除 Android，不解除 root/插件/日志/固件专用文件的保护。
- 回退镜像：`x9/out/x9-rev252hl-md/boot.img`。hn/hr 为历史未上机候选；hm 不作为上机候选。
- 新日志另存为新的 `~/N.txt`；不要把 156/157 的缓存拼接成 hs 的运行证据。

直接重编（本机私有配置、固件/根盘 manifest 已准备时）：

```sh
python3 -I x9/build/build.py all --name x9-rev252hs-md \
  --boot-config .local/x9/modem/boot-rev252hi-md.json \
  --boot-arg x9_dpmaif=2 --boot-arg x9_mipc=1
```

同名目录会覆盖：后续代码变更使用新的候选名。保留内置 20 MiB boot guard、原启动配置及 options。
构建完成不额外解包/hash/反汇编镜像、不运行额外测试套件。hs 构建输出 boot.img **12918784 B**，
日志 `x9/out/x9-rev252hs-md/build.log`；大小/编译成功不是设备启动结论。

## 链路和所有权

```text
Apple 原始用户态：ABM / CommCenter
       │ AppleBaseband、QMUX、QMI SDU
       ▼
qcom/baseband/coachz_baseband.cpp（单 QMI owner）
       │ QRTR：node 1，port 0x2000 + service
       ▼
X9Baseband.cpp → X9QMIAT.inc（32 个有界异步 job）
       │ submit / take / cancel，不直接写 CCIF
       ▼
x9_modem_bridge.h → x9_at_bridge.inc（16 槽邮箱）
       │ 唯一 MD owner；ttyC0 RX ch10 / TX ch12，queue5
       ▼
x9_modem_s3.inc → CCCI/CCIF → 物理 MT6993 MD → SIM/网络
```

`x9_bb=1` 在 launchd 之前准备 PDP helper，物理 HS2 后才启动完整桥。
桥启用时 `x9_qmi_at` 默认 1，并统一使用串行 level3 初始化；ATE0、CMEE1、
SIM/注册 URC 设置、CFUN1 完成后才接 QMI job。设 `x9_qmi_at=0` 可回到诊断/未支持响应，
不是另一条完整蜂窝实现。

AT 没有事务 ID：完整终止行才完成，不用 `+CME ERROR` 子串提前判定。
响应最多 2048 B；溢出后仍排空至 final，再返回 overflow；缺 final 或 TX 失败则隔离 ttyC0 到下一次 boot，
不拿迟到 OK 匹配新请求。已发请求被取消也必须排空；exception/offline 清理 active。
TX 可能等待 ring，必须在邮箱 mutex 外执行。物理监听在 `x9_bb=1` 时持续运行，不再四分钟后退出。

QMI SDU 检查 flags、精确长度、TLV 边界及重复 tag，保存原 transaction/message ID。
client allocation 有 64 位 generation；queued/held 请求保存 generation，过期请求丢弃。
DEL_CLIENT callback 取消全部 job/订阅后才释放该 port，防迟到回复落到新 client。
AT 正文、SIM 标识、APN、APDU 和 ch10 包正文不写 USB/pstore；仅记录协议头和结果。

## 已编码的消息与限制（全部待 hn 实测）

| 服务 / 消息 | 实际读取链 | 不能声称的能力 |
| --- | --- | --- |
| UIM CardStatus `0x2f` | CPIN READY → CRSM GET RESPONSE EF_DIR → FCP → READ RECORD → BER AID | 不从 READY 编造 AID；未知 PIN/UPIN/personalization 保持未知 |
| UIM SlotStatus `0x47` | CPIN → READ BINARY EF_ICCID | 只支持主 SIM/card1；不冒充第二槽/eUICC/ATR |
| UIM ReadTransparent `0x20` / ReadRecord `0x21` | 校验 session/path/file/范围 → FCP → 实际读取 → 原 SW1/SW2 与数据 | 不支持写/PIN/PUK、任意 AID/APDU、未知路径、加密/indication-token、多记录扩展 |
| UIM RegisterEvents `0x2e` | 仅 card/slot mask bit0/4，后台真实快照 | refresh/应用/PIN/eSIM事件未实现；不能 ACK 它们 |
| NAS ServingSystem `0x24` | CREG/CEREG/C5GREG、CGATT、COPS | 注册/attach 不是 WDS 会话或 iOS 路由 |
| NAS SystemInfo `0x4d` | 仅有实际查询结果的 LTE/NR service-status TLV | 不伪造小区/TA/RSRP/SNR/IMS voice-support |
| NAS SignalStrength `0x20` | 注册/制式查询 + CSQ → RSSI | 仅默认或零 mask；未知 99 / 额外指标请求失败 |
| NAS RegisterIndications `0x03` | 仅 serving/system 的后台快照 | SignalInfo、网络选择、band/reject/time 等未支持 |
| DMS `0x21/22/23/25/2d/3c/43` | CGMI/CGMM/CGMR/CGSN/CFUN/CRSM ICCID/CIMI | 无伪造 IMEI/IMSI；CFUN0/4 映射 LOW_POWER，不是 RESET |
| WDS `0x27/c7/29/2b` | 每槽 RAM-only 非持久 profile 创建、替换、删除、读取 | 不等于 MTK CID/连接；卡 epoch 隔离，索引同 boot 不复用 |
| WDS `0xa2/4d/20/21/22/2d` | mux/family 意图；MIPC OPEN/订阅 → CID 范围 → ACT → GET + EIF/IFUP → DPMAIF bind | 显式/非持久 profile/网络默认 APN、IPv4/no-auth；失败不生成 handle，不借用外部 context |
| WDA/DPM | 明确 NOT_SUPPORTED；X9 native backend 不创建内部 IPA 配置 client | 不打开假 IPA endpoint，不套 Qualcomm QMAP 映射 |

UIM 读请求仅接受 empty-AID primaryGW 的 current USIM（`3F007FFF`），或 card1 的 MF
`3F00` 下 EF_ICCID/EF_DIR。QMI 路径里的 uint16-LE 文件 ID 转为大写 hex，不能把任意输入插进 AT 字符串。
先读实际 FCP 验证 file ID、结构、大小/record 数，再验证范围与精确返回长度；无法解码明确失败。
CRSM 的扩展行为、FCP 返回格式和 DMS 扩展 AT 命令**尚未在 hn 实测**，不能用源码代替运行证据。

CardStatus 中 unknown PIN state 为 `NOT_INITIALIZED=0`，其固定 retry 字段不可用，**不代表真实剩余零次**；
没有声称 PIN disabled/verified，也没有推断真实 retry budget。完整 PIN/ACL 能力仍待原生 MD 真实接口。
显式 `+CME ERROR: 10` 可以报告卡不存在；其他错误/锁卡不猜成不存在。
文件属性 `0x24` 的 ACL 转译、非 provisioning AID 路由、SIM2 和其他控制操作仍返回未支持。

总 job deadline 180 s，单次 AT 10 s；队列/邮箱满返回资源错误，离线/超时明确失败，不盲目重试。
订阅以真实查询快照发送，不宣称立即 URC 驱动：同 client 周期至少 10 s，前台忙时延迟。

## 完整 Apple 固件：现在是 external + hardlink，不是旧挂载路线

输入为原始 iPhone X / D22AP、iOS **16.7.16 / 20H392** 的 Mav17 bbfw，Apple 数据不入 git。
`baseband_firmware.py --storage external` 生成独立压缩 pack/描述，不把 34 MiB pack 塞 boot。
实际固件 extent、inode、pinned/immutable、payload 大小/hash 来自私有 manifest，不能编造 DMA/磁盘地址。
首次 provisioning 的工具、失败恢复/旧 reservation 特例见 `x9/BUILD.md` 和 NOTES 历史交接；
**已经 stage 的设备不执行首次创建步骤。**

PID1、native writable X9 root、`x9_bb=1` 条件下：

1. 从独立受保护 extent 只读取包，验证整包和全部解压成员摘要。
2. 在两处真实固件路径优先保留现有匹配文件，缺失时只写一份。
3. 第二路径 hardlink 并核对，避免把约 57 MiB qdsp 再复制一份。
4. 不覆盖不同文件/符号链接，不覆盖 cryptex/dyld 目录，**不挂载固件目录**。

路径：

- `/usr/standalone/firmware/Baseband/Mav17`
- `/private/preboot/<BOOT_MANIFEST_HASH>/usr/local/standalone/firmware/Baseband/Mav17`

当前包是 **11 文件**，不是旧 README 的 12 文件/空 Options.plist/eq bindfs 方案。
157 已观察 `files 11 ... written 1 linked 1 same 20 external 1 complete 1 result errno=0`，qdsp stat/open 成功；
Sahara `finished`、QMUX1/2 打开、reset=0。Sahara 是 Apple 用户态的兼容交换，**不把 Apple 固件下载进物理 MTK MD**。
`x9_bb_firmware=0` 禁用安装；`x9_bb_logging=0` 禁用 create-if-missing 私有 logd options。
这两者不生成 Apple ticket，也不能证明真实蜂窝已激活。

## 后续完整流程与每一步的成功门

以下明确区分已有实现与待写的硬件驱动，不能跳过中间门直接“全成功”。

### 1. hn 的真实控制面闭环

先看新日志中的 hn marker、完整 11 文件、qdsp、Sahara/QMUX；再确认一个 UIM 请求：
同 txn/id 进入 X9 → MD 返回完整 final → `X9 QMI AT: response ... result 0 error 0 (real AT)` →
CommCenter 接受 card/slot/file 数据。随后验证 NAS 注册/信号和 DMS 查询。
只看到日志上的 CPIN READY，或只看到本地 activation 注册响应，都不算完成。

### 2. 补齐真实 SIM 会话/PIN/ACL 与变化通知

根据 hn 实际请求和 MD 响应确认 CRSM/FCP 形状，再接非 provisioning 的真实 AID 选择、
slot 映射、文件安全属性和 PIN 状态/重试预算。需要相应的真实 MD 能力，不能猜默认 AID、PIN2 或 retry。
锁卡/PIN/PUK 写入单独确认后再实现/验证；SIM2 不能转到 SIM1。
卡插拔/失效时清身份、应用/文件缓存及旧订阅状态，通知不带旧卡数据。

### 3. MT6993 DPMAIF v3 原生 packet backend（已编码/编译，未硬件验证）

`X9DPMAIF.inc` 使用实际 MT6993 DT/OPPO v3 契约，不使用 IPA/GSI/t7xx 的地址和端点。
`x9_dpmaif=0` 默认关，`=1` 只读探针/私有 DMA 分配，hs 显式 `=2` 主动初始化。
等真实 MD/AT owner online 后使能时钟、执行 bounded SRAM init，配置 TX DRB/BAT/fragment/双 PIT，
关闭 aggregation；每阶段读回通过才 ready。失败隔离并保留全部可能在飞 DMA，不 reset/assert/free。
私有物理连续 DMA 实际分配并验证范围/对齐，不能拿 DT 的动态 reservation 编造地址。

TX/RX 在单 bridge owner 内处理 MMIO、cache/barrier、描述符发布/回收；网络线程只复制到 mailbox。
TX 接 DRB 完成索引，RX 按 PIT sequence/h_bid/channel/header_offset 重组并归还 BAT credit；
完整 cache line 消费后毒化并返 PIT credit，避免 8-bit sequence 回绕读到上一圈。
仅支持单 normal+fragments 的非聚合 RX；不声称完整 LRO/GRO 或 RX1 已成功收发。

运行成功门依次为：阶段读回通过 → ready → WDS 真实 ccmni/MTU bind → 实际 TX 完成/RX 包。
逻辑 bind 或编译通过都不是 DMA 可达/收发证据。route epoch 和 MTU 双向检查；
**同 boot 每个 channel 只 bind 一次**，停拨后 MD 复用旧 channel 会被拒绝并清理，防止迟到旧 RX 跨 bearer。

### 4. MIPC IPv4 WDS 真实会话（已编码/编译，未拨号验证）

定点只读取证证明 Android 的 `mtkfusionrild` 实际加载 `libmtkmipc-ril.so`/`libmipc.so`，
并打开 **ttyCMIPC1**：queue5、CCCI RX216/TX217。不能用未加载的 AT RIL 后端或 MIPC0 默认名替代。
MIPC 使用 16 B header、8 B padded TLV；实际库的部分 tag 已不同于旧头文件。

1. `OPEN301`（SIM mask ff，version2/USIR）→实际 `302` version 回读。
2. `REGISTER_IND307` 分别订阅 DEACT4202 和 EIF4302，等待各自真实 `308` result0；之前不拨号。
3. `CreateProfile27` 接受原生空 profile（Type0/1、Persistence0），返回实际分配的 type/index；
   `ResetAndModifyProfileSettings c7` 原子替换 RAM 配置，`29/2b` 删除/读取同槽对象。
   配置对象不是 modem CID，不假成功持久写入；未知选项只存储，Start 不支持就拒绝。
   Start20（含原生 V2）校验显式/31引用的3GPP profile APN、IPv4、no-auth、DPMAIF ready；32仅验证
   配套3GPP2引用，不替代3GPP Internet。`GET_PDP_CID219/21a` 获取真实 CID 范围。
4. `ACT201/202` 明确 `NOT_REUSE=2`、fallback0（自有 fresh IPv4 策略，不是照抄 Android 默认值）；ID100 是 call ID，IF125 是独立 ccmni 索引，
   ACT12a 是 transIntfId，**不是 netmask，也不使用 CID-1 映射**。
5. `GET_CALL205/206` 再核对同 CID/interface/IPv4/MTU；同时等待本次 ACT 后真实 `EIF4302/IFUP` 的
   transIntfId/IPv4/MTU 一致。只有这些通过且 `x9d_bind` 成功才生成自有 handle/connected。
6. Settings 只返回实际 IPv4、DNS、MTU、family/APN；未知 gateway/netmask/IPv6prefix 不制造。
7. Stop 只接受该 client/generation 的自有 handle，先关 route，再 `DEACT203/204` 等真实确认；
   后台 GET 在飞时先排空再 deact。MD DEACT IND 和接口变化关闭 route；每 10 s 后台 GET 回读。

`NetAgentService::configureNetworkInterface` 的 IFUP 路径是本地 netdev/ioctl/address/回调，
EIPPORT 命令用于其他资源，未凭名字添加未知 MD 通知。当前仅接实际 EIF/IFUP 并建立本地 raw-IP route。
超时/DEL_CLIENT 不丢已发 token；迟到 ACT 成功只做 owned-call 清理，不迟到回复 host success。
partial-success/fallback cause 也清理，不冒充完整 IPv4；不明 framing/CNF 保留所有权隔离到下次 boot。
IPv6/SLAAC、持久/原 modem profile、auth、默认 APN 发现、autoconnect、语音/IMS 未实现；
非 Internet APN mask、未映射 profile/Start 扩展选项明确失败。RAM profile/Start20 新接线
仅完成源码与编译，真实 profile→ACT→IP 收发和默认数据卡切换仍须实机日志确认。

#### 20H392 QoS / PCO / Start V2 核对（静态证据，不是拨号成功）

原生 `StartNetworkIfaceV2::Request` 在 `1f1a8ab18` 构造的消息号仍为 **20**。
`1f1a8ba3c` 的 **d0 是 `getTLV<HandOverIPv6Prefix>` 标签**，不是第二种 Start 消息号；
适配层已删除旧 d0 消息别名。不同消息的同号 TLV 不能混用：profile36 是 emergency，Start36 是 handover。

| 消息 / TLV | 原生宽度、触发及含义 | 当前 MTK 映射 / 边界 |
|---|---|---|
| Start31 / 32 | 各 u8，3GPP / 3GPP2 profile index | 每 SIM QMI RAM 引用，非 MTK CID；只用3GPP APN拨号 |
| Start35 CallType | u8；扩展入口总是添加；`1f1a8beb0` 的 block 写1=Embedded NAI、0=Tethering NAI | 接受1，作为普通 embedded 呼叫选择元数据；不改已有 ACT201 Internet APN/no-auth。0不支持；不是 IMS/emergency 标记 |
| Start3e / profiledd | u64 APN type mask | 仅 Internet1；MTK ACT201 的102是 **u32**，经 `getMipcApnTypeId(0)` 也为1；其余 mask 不原样截断/照搬 |
| profile29 QoS/SigInd | **34 B**：traffic class u8、多个 LE u32/u8 QoS 字段、末尾 signaling u8；原生填 traffic3、priority1、signaling1，其余0 | 不是 bool，也不是 IMS APN 身份。须 APN observer 的 `isImsSupported` 谓词、set3GPPParameters 扩展 bool、carrier `SupportsIMSSignalingIndication=true` 都满足；carrier 缺键默认 false。非零仍拒绝，无可信请求映射 |
| profile1f / 36 | 各u8，PCSCF address via PCO / emergency support | 仅尺寸正确的0；非零不转普通 Internet |
| profile37 / 38 / 39 | OperatorReservedPcoId u16 / MCC u16 / MNC u16+u8；37须 context 提供非零ID；38/39来自 `PcoOptions` | 是协议配置，不是可忽略 RAM 注释；未证明 MTK 下发映射，仍拒绝 |
| profile47 PcoContainerIds | 固定 **20 B**，10个u16槽；容器列表非空且 PcoMcc/PcoMnc 均非零才追加38/39/47，超过10个截到10个 | 不把整个 QMI TLV 塞进 MTK profile-v2；未知映射仍拒绝 |
| Startc2 IDataCall | u8=1；handover 参数及 context 条件成立才添加，非普通 Start 无条件项 | 不等价 Internet type；尚无可信 MTK 对应，不放行 |
| Start36 / 3f / d0 | HandOverContext 固定21 B / ExtendedHandoffContext 变长（16 B头、长度前缀字符串列表及尾u8）/ IPv6Prefix 固定17 B（IPv6地址16 B+prefix u8）；只在相关 handover 条件成立时出现 | 需要旧会话地址、PDU session等真实状态；当前 fresh IPv4 dialler不支持，不能当默认0略过 |

MT6993 **已加载的** `libmtkmipc-ril.so` 中，`fillMipcApnItem@2c94c0` 填的是 **0x11c B**
`mipc_apn_profile_v2_struct4`（其中 APN/username/password 为定长数组、APN type为u32、protocol/auth为u8）。
其实际 callers 是 `onAttachApnChanged@2b5230` 和 `requestSyncApnTable@2c7f70`，后者发送 **APN113**
及 `8101` profile数组；**不是 ACT201 的 builder**。当前未在该结构的已写字段或
`requestSetupDataCall@2b6af0` 的 ACT builder 中确认上述 requested QoS / operator PCO 下发字段。
`updateEpsQos@2ccea0` 是发送 **221/GetQoS**、读回网络已分配 QoS；不能把它当作设置请求 QoS。
`onPcoInfoNotify@2a4910` 是读取 PCO count/内容的 indication 路径，也不证明 operator/container 请求已发出。

**默认值与查询路径不能混为一谈：**

- QoS 在 `1f1a7d440` 传入 **`kCFBooleanFalse`**，经 QMI driver → BBDataContextHelper 的
  `1002988c0` → CarrierSettings scoped key-with-default → `100534f80` 查询。
  对已存在的非字典值直接采用（显式 false 也不被默认 true 覆盖）；字典或缺键才进入
  country／Default fallback。`100b58cac` 以低优先级字典为基底、上层值覆盖同名项，
  仅两个字典递归合并，数组整体替换；上层 **`_Exclude`** 数组会删除结果中的指定键。
  该函数证明查询时的合并规则，不证明现场 carrier/override/preferences 已加载成什么值。
- PCO 的 `1f1a7db74` 调用 CarrierSettings **另一槽 `+58`**，经 `100536898` →
  `100535414/1000e1cac` 的 hierarchy 查询。它不经过上述 key-with-default 的 country／Default
  fallback；不能把 QoS 的合并路径直接套到 PCO。ContainerIds 缺键取空，PcoMcc/PcoMnc
  缺键或无法取数取0，且 `1f1a7de7c..de8c` 同时检查两者非零与列表非空。
- profile37 的 OperatorReservedPcoId 是另一来源：APN configuration 中 **`PcoContainerId`**
  经 `100a62060..620a8` 解析为 u16，缺键/非数值默认0；context getter `1003cb4d0` 返回此值。
  不等同 `PcoOptions/ContainerIds`，不能只检查后者便宣称所有 PCO 参数都不会出现。

本地官方 CBN54.1.0 carrier/D22 override，以及旧20H392 Default/Unknown/CMCC carrier 与 CMCC D22
override，定点递归读取均未见以上 capability、PcoOptions、PcoContainerId。旧根盘快照不是189现场
preferences；country domain、配置装载合并与实际 c7 参数仍须现场证据。若实机有非零29或37/38/39/47，
仍明确拒绝，不能绕过。拒绝日志只打印 tag/长度，不打印 APN、地址或认证内容。

**ACT201 两个差异参数（均为 u8，不是 QoS/PCO 字段）：**

| TLV | 已加载 RIL 普通分支 | 当前适配层 | 含义与修复边界 |
|---|---|---|---|
| 108 `IPV4V6_FALLBACK` | `2b8764..876c` 写 **0x7f** | **0=DISABLE** | 保存的 MT6993 P77 MD 中，`91326e76..91326e94` 将0/1/2映射为内部0/2/3，将7f保留为INT_MAX哨兵；`913d5cda..913d5cfc`再取每SIM策略。`l5updn_init`调用的custom函数返回1，初始化为IPv4-first；7f不是直接禁用。现场策略覆盖与最终family结果仍未证明 |
| 10a `REUSE_FLAG` | 普通分支 `2b8830..8840` 写 **0=DEFAULT**；APN type2/100 特殊分支写3 | **2=NOT_REUSE** | 0允许 MD 的默认复用政策，不等同强制新建；3=REUSE_ONLY，不是普通 Internet 分支。当前只管理自有 fresh call，不能直接改成0后仍沿用成功/超时即 DEACT 的清理逻辑 |

以上差分不是189失败原因的实跑证据：189没有 Start20/ACT201。当前保持 fallback0、NOT_REUSE2，
也不增加未知 QoS/PCO TLV。要改复用必须先有实际 existing-call 身份、所有权和 GET/EIF 资格，
避免清理非本次创建的 PDN；要改 fallback 必须确认 MD 的0x7f策略及对应 family/partial-success处理。

**MD消费端补充（静态，不替代实跑）**：原始`modem_a.img`内置`md1_dbginfo`解出146850条
符号，版本为`MOLY.NR17.R2.MP2.TC16.PR3.SP.V2.P77`。`mipc_data_act_call_req`→
`l5mipc_ilm_send`→L5UPDN消费链已定位；两处fallback映射表均为`[0,2,3]`。
10a的u8值0..3保存在请求`+a0`；L5UPDN对3与APN type2/100有明确existing-call复用分支，
另有按APN查旧call并调用`reuse_data_call_info`的路径。因此**2是否跨所有分支保证fresh PDN仍待确认**，
不以枚举名替代所有权验证。CarrierSettings装载已追到`100535418`按domain/filename缓存，
`1005388f0`→`100611f84`通过FileSystemInterface读取并转换字典；同文件名可跳过重载，
读取失败仅在RadioPersonality为1时尝试MobileHelper。D22 override组成及189有效配置仍未知。
证据与只读重放入口：`.local/x9/work/cell189-md-consumer-20261110/{findings.json,md-dis.py,replay.sh}`；
本轮未改下发字段、未构建镜像或采集新runtime。

### 5. iOS 网络最终验证

hs 已接 raw-IP send/receiver 和经 WDS 证据门控制的 route；仍需实机验证 Apple traffic gate 与 PDP unit/family。
确认 iOS 接口有实际地址、路由/DNS，TX/RX 不再为零；通过该蜂窝路径验证收发、断线重连、IPv4/IPv6。
四个 PDP helper 接口存在或 `data port step3` 只是初始化尝试结束，不是通网证据。

### 6. 语音、短信和 IMS/音频

需要额外真实 MD call/SMS/IMS 信令和音频路由实现；不属于已写的六个服务控制查询。
只能在相应真实请求/状态/音频闭环通过后报告可通话，不能用 LTE/5G 注册替代。

## 回读与维护

交接只写 `NOTES.md`；原始设备日志、设备标识/SIM 数据、Apple 固件、私有 manifest 保存在 `.local/`，不入 git。
最小日志检索（把 N 换成新运行）：

```sh
rg -n 'build=rev252hs|X9 BBFW|qdsp6|sahara|X9 QMI AT:|X9 DPMAIF:|X9 MIPC:|X9 WDS:|COACHZ pdp:' ~/N.txt
```

失败先回到最早不确定分支：AT offline/timeout、final、CRSM/FCP/session、QMI TLV、CommCenter 消费，
MIPC OPEN/订阅、ACT/GET/EIF 与 DPMAIF ready/bind 也要按顺序核对；不要反复重stage已成功的固件、绕过断言或将所有 unsupported 改为 success。
