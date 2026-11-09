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
| WDS `0xa2/4d/20/21/22/2d` | mux/family 意图；MIPC OPEN/订阅 → CID 范围 → ACT → GET + EIF/IFUP → DPMAIF bind | 只支持显式 APN/IPv4/no-auth；失败不生成 handle，不借用外部 context |
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
3. Start 校验显式 APN、IPv4、no-auth、DPMAIF ready；`GET_PDP_CID219/21a` 获取真实 CID 范围。
4. `ACT201/202` 明确 `NOT_REUSE=2`、fallback0；ID100 是 call ID，IF125 是独立 ccmni 索引，
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
IPv6/SLAAC、profile/auth、默认 APN 发现、autoconnect、语音/IMS 未实现；额外参数明确失败。

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
